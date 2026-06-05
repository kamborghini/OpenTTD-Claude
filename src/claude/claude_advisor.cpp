/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file claude_advisor.cpp In-game Claude AI strategy advisor: a chat window that reads the
 * current game state, asks the Anthropic API on a background thread and shows the reply. */

#include "../stdafx.h"
#include "claude_advisor.h"

#include "../window_gui.h"
#include "../window_func.h"
#include "../widget_type.h"
#include "../querystring_gui.h"
#include "../strings_func.h"
#include "../gfx_func.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../vehicle_base.h"
#include "../station_base.h"
#include "../town.h"
#include "../timer/timer_game_calendar.h"

#include "../3rdparty/nlohmann/json.hpp"

#include "table/strings.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#if __has_include(<curl/curl.h>)
#	include <curl/curl.h>
#	define CLAUDE_HAVE_CURL 1
#endif

#include "../safeguards.h"

/** Widgets of the Claude advisor window. */
enum ClaudeAdvisorWidgets : WidgetID {
	WID_CA_OUTPUT,  ///< Conversation display panel.
	WID_CA_TEXTBOX, ///< Question input box.
	WID_CA_SEND,    ///< "Ask" button.
};

/**
 * State shared between the main (game) thread and the background HTTP worker thread.
 * Intentionally heap-allocated and never freed (see GetClaudeState) so that a detached
 * worker that is still running at program exit can never touch a destroyed object.
 */
struct ClaudeState {
	std::mutex mutex;       ///< Guards every field below.
	std::string transcript; ///< Full visible conversation.
	int inflight = 0;       ///< Number of requests currently in flight.
	bool dirty = false;     ///< Set when the window should redraw.
};

static ClaudeState &GetClaudeState()
{
	static ClaudeState *state = new ClaudeState();
	return *state;
}

/**
 * Build a compact, human-readable snapshot of the local company. Must run on the main thread
 * because it reads live game state.
 */
static std::string BuildGameStateSummary()
{
	CompanyID cid = _local_company;
	if (!Company::IsValidHumanID(cid)) {
		return "The player is currently a spectator and does not control a company yet.";
	}

	const Company *c = Company::Get(cid);

	int trains = 0, road_vehicles = 0, ships = 0, aircraft = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->owner != cid || !v->IsPrimaryVehicle()) continue;
		switch (v->type) {
			case VehicleType::Train:    trains++; break;
			case VehicleType::Road:     road_vehicles++; break;
			case VehicleType::Ship:     ships++; break;
			case VehicleType::Aircraft: aircraft++; break;
			default: break;
		}
	}

	int stations = 0;
	for (const Station *st : Station::Iterate()) {
		if (st->owner == cid) stations++;
	}

	int towns = 0;
	uint64_t population = 0;
	for (const Town *t : Town::Iterate()) {
		towns++;
		population += t->cache.population;
	}

	const CompanyEconomyEntry &eco = (c->num_valid_stat_ent > 0) ? c->old_economy[0] : c->cur_economy;

	return fmt::format(
		"Current in-game year: {}\n"
		"Cash available: {}\n"
		"Outstanding loan: {}\n"
		"Company value: {}\n"
		"Performance rating (0-1000): {}\n"
		"Most recent quarter income: {}\n"
		"Most recent quarter expenses: {}\n"
		"Fleet -> trains: {}, road vehicles: {}, ships: {}, aircraft: {}\n"
		"Stations / stops owned: {}\n"
		"World map: {} towns, total population around {}.\n"
		"(All monetary values are in the game's internal currency units.)",
		TimerGameCalendar::year.base(),
		static_cast<int64_t>(c->money),
		static_cast<int64_t>(c->current_loan),
		static_cast<int64_t>(eco.company_value),
		eco.performance_history,
		static_cast<int64_t>(eco.income),
		static_cast<int64_t>(eco.expenses),
		trains, road_vehicles, ships, aircraft,
		stations,
		towns, population);
}

/** Compose the system prompt that grounds Claude in OpenTTD and the player's situation. */
static std::string BuildSystemPrompt(const std::string &game_state)
{
	return
		"You are Claude, a friendly and concise strategy advisor built into OpenTTD, an "
		"open-source transport simulation game in the style of Transport Tycoon Deluxe. The "
		"player runs a transport company using trains, road vehicles, ships and aircraft, "
		"connecting industries and towns to move cargo and passengers for profit, while "
		"managing loans, running costs, station catchment areas and signals.\n\n"
		"Answer the player's question with practical, specific, actionable advice. Keep it "
		"short: a few sentences or a tight bulleted list. Avoid generic filler and do not "
		"repeat the question back.\n\n"
		"Snapshot of the player's current game:\n" + game_state;
}

#ifdef CLAUDE_HAVE_CURL
/** libcurl write callback: append received bytes to a std::string. */
static size_t ClaudeWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
	return size * nmemb;
}

/** Initialise libcurl exactly once, on the thread that first submits a question (the main thread). */
static void EnsureCurlInitialised()
{
	static std::once_flag flag;
	std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

/**
 * Perform the blocking HTTP round-trip to the Anthropic Messages API. Runs on a worker thread.
 * @return true on success, with @p out set to the answer text; false with @p out set to an error.
 */
static bool ClaudeHttpRequest(const std::string &api_key, const std::string &model, const std::string &base_url,
		const std::string &system_prompt, const std::string &question, std::string &out)
{
	nlohmann::json body;
	body["model"] = model;
	body["max_tokens"] = 800;
	body["system"] = system_prompt;
	body["messages"] = nlohmann::json::array();
	body["messages"].push_back({{"role", "user"}, {"content", question}});
	std::string payload = body.dump();

	CURL *curl = curl_easy_init();
	if (curl == nullptr) {
		out = "could not initialise libcurl";
		return false;
	}

	std::string url = base_url + "/v1/messages";
	std::string response;

	curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "content-type: application/json");
	headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
	std::string key_header = "x-api-key: " + api_key;
	headers = curl_slist_append(headers, key_header.c_str());

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ClaudeWriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "OpenTTD-Claude/1.0");

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		out = fmt::format("network error: {}", curl_easy_strerror(res));
		return false;
	}

	try {
		nlohmann::json j = nlohmann::json::parse(response);
		if (j.contains("error") && j["error"].is_object()) {
			out = fmt::format("Anthropic API error (HTTP {}): {}", http_code, j["error"].value("message", std::string("unknown error")));
			return false;
		}
		if (j.contains("content") && j["content"].is_array()) {
			std::string text;
			for (const auto &block : j["content"]) {
				if (block.value("type", std::string()) == "text") text += block.value("text", std::string());
			}
			if (!text.empty()) {
				out = text;
				return true;
			}
		}
		out = fmt::format("unexpected API response (HTTP {})", http_code);
		return false;
	} catch (const nlohmann::json::exception &e) {
		out = fmt::format("could not parse API response (HTTP {}): {}", http_code, e.what());
		return false;
	}
}
#else
static void EnsureCurlInitialised() {}
static bool ClaudeHttpRequest(const std::string &, const std::string &, const std::string &,
		const std::string &, const std::string &, std::string &out)
{
	out = "this OpenTTD build was compiled without libcurl, so Claude cannot be reached";
	return false;
}
#endif /* CLAUDE_HAVE_CURL */

/** Background worker: make the request and append the result to the shared transcript. */
static void ClaudeWorker(std::string api_key, std::string model, std::string base_url, std::string system_prompt, std::string question)
{
	std::string answer;
	bool ok = ClaudeHttpRequest(api_key, model, base_url, system_prompt, question, answer);

	ClaudeState &state = GetClaudeState();
	std::lock_guard<std::mutex> lock(state.mutex);
	state.transcript += "\n\nClaude: ";
	state.transcript += ok ? answer : ("[" + answer + "]");
	if (state.inflight > 0) state.inflight--;
	state.dirty = true;
	/* Keep the transcript from growing without bound. */
	if (state.transcript.size() > 8000) state.transcript.erase(0, state.transcript.size() - 8000);
}

/** Read an environment variable, falling back to a default when unset or empty. */
static std::string EnvOr(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	return (value != nullptr && value[0] != '\0') ? std::string(value) : std::string(fallback);
}

/**
 * Capture the game state and dispatch a question to Claude on a background thread.
 * Must be called on the main thread.
 */
static void ClaudeAdvisorSubmit(const std::string &question)
{
	if (question.empty()) return;

	const char *api_key = std::getenv("ANTHROPIC_API_KEY");
	std::string game_state = BuildGameStateSummary();

	ClaudeState &state = GetClaudeState();
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		if (!state.transcript.empty()) state.transcript += "\n\n";
		state.transcript += "You: " + question;
		if (api_key == nullptr || api_key[0] == '\0') {
			state.transcript += "\n\nClaude: [Not configured: set the ANTHROPIC_API_KEY environment variable before launching OpenTTD, then ask again.]";
			state.dirty = true;
			return;
		}
		state.inflight++;
		state.dirty = true;
	}

	EnsureCurlInitialised();

	std::string model = EnvOr("ANTHROPIC_MODEL", "claude-sonnet-4-6");
	std::string base_url = EnvOr("ANTHROPIC_BASE_URL", "https://api.anthropic.com");
	std::string system_prompt = BuildSystemPrompt(game_state);

	std::thread(ClaudeWorker, std::string(api_key), model, base_url, system_prompt, question).detach();
}

/** Window to chat with the Claude advisor. */
struct ClaudeAdvisorWindow : public Window {
	QueryString message_editbox; ///< Question input box.

	explicit ClaudeAdvisorWindow(WindowDesc &desc) : Window(desc), message_editbox(512)
	{
		this->querystrings[WID_CA_TEXTBOX] = &this->message_editbox;
		this->message_editbox.ok_button = WID_CA_SEND;

		this->CreateNestedTree();
		this->FinishInitNested(0);
		this->SetFocusedWidget(WID_CA_TEXTBOX);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_CA_OUTPUT) return;

		std::string text;
		int inflight;
		{
			ClaudeState &state = GetClaudeState();
			std::lock_guard<std::mutex> lock(state.mutex);
			text = state.transcript;
			inflight = state.inflight;
		}

		if (text.empty()) {
			text = "Ask Claude anything about your transport empire. For example:\n"
				"  • What should I build next?\n"
				"  • Why is my train losing money?\n"
				"  • How do I grow this town faster?";
		}
		if (inflight > 0) {
			text += "\n\nClaude is thinking…";
		}

		DrawStringMultiLine(r.left + 4, r.right - 4, r.top + 3, r.bottom - 3, text, TextColour::Black, SA_LEFT | SA_BOTTOM);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_CA_SEND) return;

		std::string question(this->message_editbox.text.GetText());
		if (question.empty()) return;

		ClaudeAdvisorSubmit(question);
		this->message_editbox.text.DeleteAll();
		this->SetFocusedWidget(WID_CA_TEXTBOX);
		this->SetDirty();
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		bool needs_redraw = false;
		{
			ClaudeState &state = GetClaudeState();
			std::lock_guard<std::mutex> lock(state.mutex);
			if (state.dirty) {
				state.dirty = false;
				needs_redraw = true;
			}
		}
		if (needs_redraw) this->SetDirty();
	}
};

/** The widgets of the Claude advisor window. */
static constexpr std::initializer_list<NWidgetPart> _nested_claude_advisor_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey), SetStringTip(STR_CLAUDE_ADVISOR_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey, WID_CA_OUTPUT), SetMinimalSize(460, 240), SetResize(1, 1), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_EDITBOX, Colours::Grey, WID_CA_TEXTBOX), SetMinimalSize(380, 16), SetPadding(2, 0, 2, 2), SetResize(1, 0), SetStringTip(STR_NULL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CA_SEND), SetMinimalSize(70, 16), SetPadding(2, 2, 2, 2), SetStringTip(STR_CLAUDE_ADVISOR_SEND),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

/** Window description for the Claude advisor. */
static WindowDesc _claude_advisor_desc(
	WindowPosition::Center, "claude_advisor", 480, 320,
	WindowClass::ClaudeAdvisor, WindowClass::None,
	{},
	_nested_claude_advisor_widgets
);

void ShowClaudeAdvisorWindow(const std::string &question)
{
	Window *w = FindWindowById(WindowClass::ClaudeAdvisor, 0);
	if (w == nullptr) w = new ClaudeAdvisorWindow(_claude_advisor_desc);
	w->SetDirty();

	if (!question.empty()) ClaudeAdvisorSubmit(question);
}
