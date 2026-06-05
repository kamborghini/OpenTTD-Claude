/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file claude_advisor.cpp The "Claude Prompt Sandbox": a window where the player issues natural-language
 * prompts. Claude scores each prompt's engineering quality and turns it into real in-game actions executed
 * through OpenTTD's command system. Prompt quality scales the outcome. A learn-to-prompt game mode. */

#include "../stdafx.h"
#include "claude_advisor.h"

#include "../window_gui.h"
#include "../window_func.h"
#include "../widget_type.h"
#include "../querystring_gui.h"
#include "../strings_func.h"
#include "../gfx_func.h"
#include "../command_func.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../vehicle_base.h"
#include "../town.h"
#include "../town_cmd.h"
#include "../town_type.h"
#include "../misc_cmd.h"
#include "../tree_cmd.h"
#include "../tree_map.h"
#include "../company_cmd.h"
#include "../map_func.h"
#include "../tile_map.h"
#include "../economy_type.h"
#include "../timer/timer_game_calendar.h"
#include "../core/random_func.hpp"

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

/** Widgets of the Claude prompt sandbox window. */
enum ClaudeSandboxWidgets : WidgetID {
	WID_CS_OUTPUT,  ///< Conversation / results transcript.
	WID_CS_TEXTBOX, ///< Prompt input box.
	WID_CS_RUN,     ///< "Run" button (execute the prompt).
	WID_CS_IMPROVE, ///< "Improve my prompt" button (coach, no actions).
	WID_CS_SUGGEST, ///< "Suggest a prompt" button (coach, no actions).
};

/** What kind of request we send to Claude. */
enum class ClaudeMode : uint8_t {
	Act,     ///< Execute the prompt: returns JSON {reply, score, actions}.
	Improve, ///< Critique and rewrite the player's draft prompt (text only).
	Suggest, ///< Propose a strong example prompt for the current game (text only).
};

/**
 * State shared between the main (game) thread and the background HTTP worker thread.
 * Heap-allocated and never freed so a detached worker can never touch a destroyed object.
 */
struct ClaudeShared {
	std::mutex mutex;            ///< Guards every field below.
	std::string transcript;     ///< Full visible conversation / action log.
	int inflight = 0;           ///< Requests currently in flight.
	bool dirty = false;         ///< Window should redraw.
	bool has_pending_actions = false; ///< A parsed action plan is waiting for the MAIN thread to execute.
	std::string pending_actions_json; ///< The "actions" array (JSON) to execute.
	int pending_score = -1;     ///< Prompt quality score for the efficiency bonus.
};

static ClaudeShared &GS()
{
	static ClaudeShared *state = new ClaudeShared();
	return *state;
}

/** Lower-case an ASCII string (used for case-insensitive town matching). */
static std::string AsciiLower(std::string_view s)
{
	std::string out(s);
	for (char &c : out) {
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
	return out;
}

/** Find a town by (case-insensitive) exact name, then by substring. Main thread only. */
static const Town *FindTownByName(const std::string &name)
{
	if (name.empty()) return nullptr;
	std::string want = AsciiLower(name);
	for (const Town *t : Town::Iterate()) {
		if (AsciiLower(GetString(STR_TOWN_NAME, t->index)) == want) return t;
	}
	for (const Town *t : Town::Iterate()) {
		if (AsciiLower(GetString(STR_TOWN_NAME, t->index)).find(want) != std::string::npos) return t;
	}
	return nullptr;
}

/** Build a compact snapshot of the local company + nearby towns. Must run on the main thread. */
static std::string BuildGameStateSummary()
{
	CompanyID cid = _local_company;
	if (!Company::IsValidHumanID(cid)) {
		return "The player is currently a spectator and does not control a company yet, so most actions will fail until they start a company.";
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

	std::string towns_list;
	int town_count = 0;
	for (const Town *t : Town::Iterate()) {
		if (town_count < 14) {
			towns_list += fmt::format("{}{} (pop {})", town_count == 0 ? "" : ", ", GetString(STR_TOWN_NAME, t->index), t->cache.population);
		}
		town_count++;
	}
	if (town_count > 14) towns_list += ", ...";

	return fmt::format(
		"Year: {}\n"
		"Cash: {}\n"
		"Loan: {}\n"
		"Company value: {}\n"
		"Performance rating (0-1000): {}\n"
		"Fleet -> trains: {}, road vehicles: {}, ships: {}, aircraft: {}\n"
		"Towns on the map ({} total): {}\n"
		"(All money is in the game's internal currency units.)",
		TimerGameCalendar::year.base(),
		static_cast<int64_t>(c->money),
		static_cast<int64_t>(c->current_loan),
		static_cast<int64_t>(c->cur_economy.company_value),
		c->cur_economy.performance_history,
		trains, road_vehicles, ships, aircraft,
		town_count, towns_list);
}

/* ===== System prompts ===== */

static std::string BuildActSystemPrompt(const std::string &game_state)
{
	return
		"You are the engine of a PROMPT-ENGINEERING learning sandbox built inside OpenTTD, a transport "
		"simulation game. The player types a natural-language PROMPT to control their transport company. "
		"You do two jobs every turn:\n"
		"1) SCORE the prompt's engineering quality (0-100).\n"
		"2) Turn it into concrete in-game ACTIONS, chosen ONLY from the allowed list below.\n\n"
		"TEACHING PRINCIPLE — make prompt quality matter: a precise prompt (clear goal, specific named "
		"towns, explicit amounts, constraints) earns a HIGH score and more, better-targeted actions. A vague "
		"prompt ('make it better', 'help my towns') earns a LOW score, a short coaching tip, and only "
		"minimal or cautious actions. Score the PROMPT's specificity and clarity, not how polite it is.\n\n"
		"Allowed actions (objects in the \"actions\" array):\n"
		"- {\"type\":\"advertise_town\",\"town\":\"<name>\",\"size\":\"small|medium|large\"} - boost a town's station ratings / passengers.\n"
		"- {\"type\":\"fund_buildings\",\"town\":\"<name>\"} - pay to rapidly grow a town with new buildings.\n"
		"- {\"type\":\"build_statue\",\"town\":\"<name>\"} - build a statue (permanent local ratings boost).\n"
		"- {\"type\":\"plant_trees\",\"town\":\"<name>\"} - plant trees around a town.\n"
		"- {\"type\":\"take_loan\",\"amount\":<integer>} - borrow money.\n"
		"- {\"type\":\"repay_loan\",\"amount\":<integer>} - repay loan.\n"
		"- {\"type\":\"rename_company\",\"name\":\"<text>\"} - rename the company.\n"
		"- {\"type\":\"rename_president\",\"name\":\"<text>\"} - rename the president.\n"
		"- {\"type\":\"found_town\"} - attempt to found a new town (may be disabled in this game).\n\n"
		"Only reference towns that appear in the snapshot. Use the player's stated amounts; if they only "
		"vaguely imply an amount, pick a sensible one but lower the specificity score. Do NOT invent action "
		"types outside the list.\n\n"
		"Respond with STRICT JSON ONLY (no markdown fences, no text outside the JSON), exactly this shape:\n"
		"{\n"
		"  \"reply\": \"1-3 sentences to the player: what you're doing and why\",\n"
		"  \"score\": {\"overall\": <0-100>, \"clarity\": <0-100>, \"specificity\": <0-100>, \"constraints\": <0-100>, \"feedback\": \"1-2 sentence tip to make the prompt better\"},\n"
		"  \"actions\": [ ... zero or more action objects ... ]\n"
		"}\n\n"
		"Current game snapshot:\n" + game_state;
}

static std::string BuildImproveSystemPrompt(const std::string &game_state)
{
	return
		"You are a prompt-engineering coach inside an OpenTTD learning game. The player gives you a DRAFT "
		"prompt. Briefly critique it for prompt quality — does it have a clear goal, specific targets (named "
		"towns), explicit amounts/constraints, and useful context? Then give an improved rewrite. Be concise. "
		"Format: a few short bullet points of critique, then a final line starting with 'Improved prompt: ' "
		"and the rewrite. Do NOT take any actions.\n\n"
		"Game context to ground your rewrite:\n" + game_state;
}

static std::string BuildSuggestSystemPrompt(const std::string &game_state)
{
	return
		"You are a prompt-engineering coach inside an OpenTTD learning sandbox. Suggest ONE strong example "
		"prompt the player could try right now, tailored to their current game (use a REAL town name from the "
		"snapshot). Model good prompt engineering: a clear goal, a specific target, and a constraint or budget. "
		"Format: the suggested prompt on its own line in double quotes, then a single line explaining why it's "
		"a good prompt. Keep it short.\n\n"
		"Current game snapshot:\n" + game_state;
}

/* ===== HTTP (background thread) ===== */

#ifdef CLAUDE_HAVE_CURL
static size_t ClaudeWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
	return size * nmemb;
}

static void EnsureCurlInitialised()
{
	static std::once_flag flag;
	std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static bool ClaudeHttpRequest(const std::string &api_key, const std::string &model, const std::string &base_url,
		const std::string &system_prompt, const std::string &user_message, std::string &out)
{
	nlohmann::json body;
	body["model"] = model;
	body["max_tokens"] = 1024;
	body["system"] = system_prompt;
	body["messages"] = nlohmann::json::array();
	body["messages"].push_back({{"role", "user"}, {"content", user_message}});
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

/** Pull the outermost JSON object out of a possibly-fenced model reply. */
static std::string ExtractJsonObject(const std::string &raw)
{
	size_t start = raw.find('{');
	size_t end = raw.rfind('}');
	if (start == std::string::npos || end == std::string::npos || end < start) return raw;
	return raw.substr(start, end - start + 1);
}

/** Background worker: call Claude, then append the result to the shared transcript. */
static void ClaudeWorker(ClaudeMode mode, std::string api_key, std::string model, std::string base_url, std::string system_prompt, std::string user_message)
{
	std::string raw;
	bool ok = ClaudeHttpRequest(api_key, model, base_url, system_prompt, user_message, raw);

	ClaudeShared &s = GS();
	std::lock_guard<std::mutex> lock(s.mutex);

	if (!ok) {
		s.transcript += "\n\nClaude: [" + raw + "]";
	} else if (mode == ClaudeMode::Act) {
		try {
			nlohmann::json j = nlohmann::json::parse(ExtractJsonObject(raw));
			std::string reply = j.value("reply", std::string());
			int overall = -1, clarity = -1, specificity = -1, constraints = -1;
			std::string feedback;
			if (j.contains("score") && j["score"].is_object()) {
				const auto &sc = j["score"];
				overall = sc.value("overall", -1);
				clarity = sc.value("clarity", -1);
				specificity = sc.value("specificity", -1);
				constraints = sc.value("constraints", -1);
				feedback = sc.value("feedback", std::string());
			}
			s.transcript += "\n\nClaude: " + (reply.empty() ? "(no reply)" : reply);
			if (overall >= 0) {
				s.transcript += fmt::format("\nPrompt score: {}/100  (clarity {}, specificity {}, constraints {})", overall, clarity, specificity, constraints);
			}
			if (!feedback.empty()) s.transcript += "\nCoach: " + feedback;

			s.pending_actions_json = (j.contains("actions") && j["actions"].is_array()) ? j["actions"].dump() : std::string("[]");
			s.pending_score = overall;
			s.has_pending_actions = true;
		} catch (const std::exception &e) {
			s.transcript += fmt::format("\n\nClaude: [could not read the action plan: {}]", e.what());
		}
	} else {
		s.transcript += "\n\nCoach: " + raw;
	}

	if (s.inflight > 0) s.inflight--;
	s.dirty = true;
	if (s.transcript.size() > 9000) s.transcript.erase(0, s.transcript.size() - 9000);
}

/** Capture game state and dispatch a request to Claude on a background thread. Main thread only. */
static void ClaudeSubmit(ClaudeMode mode, const std::string &user_text)
{
	if (mode != ClaudeMode::Suggest && user_text.empty()) return;

	const char *api_key = std::getenv("ANTHROPIC_API_KEY");
	std::string game_state = BuildGameStateSummary();

	ClaudeShared &s = GS();
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		if (!s.transcript.empty()) s.transcript += "\n\n";
		switch (mode) {
			case ClaudeMode::Act:     s.transcript += "You: " + user_text; break;
			case ClaudeMode::Improve: s.transcript += "Improve this prompt: \"" + user_text + "\""; break;
			case ClaudeMode::Suggest: s.transcript += "(Asked Claude to suggest a prompt)"; break;
		}
		if (api_key == nullptr || api_key[0] == '\0') {
			s.transcript += "\n\nClaude: [Not configured: set the ANTHROPIC_API_KEY environment variable before launching OpenTTD, then try again.]";
			s.dirty = true;
			return;
		}
		s.inflight++;
		s.dirty = true;
	}

	EnsureCurlInitialised();

	std::string model = []() { const char *m = std::getenv("ANTHROPIC_MODEL"); return (m != nullptr && m[0] != '\0') ? std::string(m) : std::string("claude-sonnet-4-6"); }();
	std::string base_url = []() { const char *b = std::getenv("ANTHROPIC_BASE_URL"); return (b != nullptr && b[0] != '\0') ? std::string(b) : std::string("https://api.anthropic.com"); }();

	std::string system_prompt;
	std::string user_message;
	switch (mode) {
		case ClaudeMode::Act:     system_prompt = BuildActSystemPrompt(game_state);     user_message = user_text; break;
		case ClaudeMode::Improve: system_prompt = BuildImproveSystemPrompt(game_state); user_message = user_text; break;
		case ClaudeMode::Suggest: system_prompt = BuildSuggestSystemPrompt(game_state); user_message = "Suggest one strong prompt I could try in my current situation."; break;
	}

	std::thread(ClaudeWorker, mode, std::string(api_key), model, base_url, system_prompt, user_message).detach();
}

/* ===== Action executor (MAIN thread) ===== */

/** Execute the JSON action plan via OpenTTD commands and return a human-readable result log. */
static std::string ExecuteActionPlan(const std::string &actions_json, int score)
{
	if (!Company::IsValidHumanID(_local_company)) {
		return "  Cannot act: you don't control a company yet (start a new game first).\n";
	}

	/* Run the commands as the local player's company. */
	CompanyID backup = _current_company;
	_current_company = _local_company;

	std::string out;
	int total = 0;

	try {
		nlohmann::json arr = nlohmann::json::parse(actions_json);
		if (arr.is_array()) {
			for (const auto &a : arr) {
				if (!a.is_object() || !a.contains("type")) continue;
				std::string type = a.value("type", std::string());
				total++;
				bool ok = false;
				std::string label = type;

				if (type == "advertise_town" || type == "fund_buildings" || type == "build_statue") {
					std::string town_name = a.value("town", std::string());
					const Town *t = FindTownByName(town_name);
					if (t != nullptr) {
						TownAction act = TownAction::FundBuildings;
						if (type == "build_statue") {
							act = TownAction::BuildStatue;
							label = fmt::format("Build statue in {}", town_name);
						} else if (type == "fund_buildings") {
							act = TownAction::FundBuildings;
							label = fmt::format("Fund new buildings in {}", town_name);
						} else {
							std::string size = a.value("size", std::string("medium"));
							act = (size == "large") ? TownAction::AdvertiseLarge : (size == "small") ? TownAction::AdvertiseSmall : TownAction::AdvertiseMedium;
							label = fmt::format("{} advertising in {}", size, town_name);
						}
						ok = Command<Commands::TownAction>::Post(STR_ERROR_CAN_T_DO_THIS, t->xy, t->index, act);
					} else {
						label = fmt::format("{} (town \"{}\" not found)", type, town_name);
					}
				} else if (type == "plant_trees") {
					std::string town_name = a.value("town", std::string());
					const Town *t = FindTownByName(town_name);
					if (t != nullptr) {
						uint x = TileX(t->xy), y = TileY(t->xy);
						uint x2 = std::min<uint>(x + 4, Map::SizeX() - 2);
						uint y2 = std::min<uint>(y + 4, Map::SizeY() - 2);
						ok = Command<Commands::PlantTree>::Post(STR_ERROR_CAN_T_PLANT_TREE_HERE, TileXY(x2, y2), TileXY(x, y), TREE_INVALID, false);
						label = fmt::format("Plant trees around {}", town_name);
					} else {
						label = fmt::format("plant_trees (town \"{}\" not found)", town_name);
					}
				} else if (type == "take_loan") {
					int64_t amount = a.value("amount", static_cast<int64_t>(0));
					ok = Command<Commands::IncreaseLoan>::Post(STR_ERROR_CAN_T_BORROW_ANY_MORE_MONEY, LoanCommand::Amount, Money(amount));
					label = fmt::format("Take loan of {}", amount);
				} else if (type == "repay_loan") {
					int64_t amount = a.value("amount", static_cast<int64_t>(0));
					ok = Command<Commands::DecreaseLoan>::Post(STR_ERROR_CAN_T_REPAY_LOAN, LoanCommand::Amount, Money(amount));
					label = fmt::format("Repay loan of {}", amount);
				} else if (type == "rename_company") {
					std::string name = a.value("name", std::string());
					ok = Command<Commands::RenameCompany>::Post(STR_ERROR_CAN_T_CHANGE_COMPANY_NAME, name);
					label = fmt::format("Rename company to \"{}\"", name);
				} else if (type == "rename_president") {
					std::string name = a.value("name", std::string());
					ok = Command<Commands::RenamePresident>::Post(STR_ERROR_CAN_T_CHANGE_PRESIDENT, name);
					label = fmt::format("Rename president to \"{}\"", name);
				} else if (type == "found_town") {
					TileIndex tile = TileIndex(InteractiveRandomRange(Map::Size()));
					ok = Command<Commands::FoundTown>::Post(STR_ERROR_CAN_T_FOUND_TOWN_HERE, tile, TSZ_MEDIUM, false, TL_ORIGINAL, true, InteractiveRandom(), std::string());
					label = "Found a new town";
				} else {
					label = "Unknown action: " + type;
				}

				out += fmt::format("  {} {}\n", ok ? "[done]" : "[failed]", label);
			}
		}
	} catch (const std::exception &e) {
		out += fmt::format("  Could not read the action plan: {}\n", e.what());
	}

	/* Prompt quality -> efficiency bonus/penalty (single-player money adjustment). */
	if (score >= 0 && total > 0) {
		int64_t bonus = static_cast<int64_t>(score - 40) * 250; // ~ -10000 .. +15000
		if (bonus != 0) {
			Command<Commands::MoneyCheat>::Post(Money(bonus));
			out += fmt::format("  [bonus] Prompt quality {}/100 -> efficiency {}{}\n", score, bonus >= 0 ? "+" : "", bonus);
		}
	}
	if (total == 0) out += "  (no actions taken)\n";

	_current_company = backup;
	return out;
}

std::string ClaudeRunSelfTest()
{
	std::string town;
	for (const Town *t : Town::Iterate()) { town = GetString(STR_TOWN_NAME, t->index); break; }

	nlohmann::json plan = nlohmann::json::array();
	plan.push_back({{"type", "rename_company"}, {"name", "Prompt Rail Co"}});
	plan.push_back({{"type", "take_loan"}, {"amount", 50000}});
	plan.push_back({{"type", "repay_loan"}, {"amount", 20000}});
	if (!town.empty()) {
		plan.push_back({{"type", "advertise_town"}, {"town", town}, {"size", "large"}});
		plan.push_back({{"type", "fund_buildings"}, {"town", town}});
		plan.push_back({{"type", "plant_trees"}, {"town", town}});
	}
	return ExecuteActionPlan(plan.dump(), 85);
}

/* ===== Window ===== */

struct ClaudeSandboxWindow : public Window {
	QueryString prompt_editbox; ///< Prompt input box.

	explicit ClaudeSandboxWindow(WindowDesc &desc) : Window(desc), prompt_editbox(512)
	{
		this->querystrings[WID_CS_TEXTBOX] = &this->prompt_editbox;
		this->prompt_editbox.ok_button = WID_CS_RUN;

		this->CreateNestedTree();
		this->FinishInitNested(0);
		this->SetFocusedWidget(WID_CS_TEXTBOX);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_CS_OUTPUT) return;

		std::string text;
		int inflight;
		{
			ClaudeShared &s = GS();
			std::lock_guard<std::mutex> lock(s.mutex);
			text = s.transcript;
			inflight = s.inflight;
		}

		if (text.empty()) {
			text =
				"Claude Prompt Sandbox — you play by PROMPTING, not clicking.\n\n"
				"Type a prompt and press Run. Claude scores how well you prompted, then carries it out "
				"in the game. The clearer and more specific your prompt, the better the result.\n\n"
				"Try:\n"
				"  \"Run a large advertising campaign in <a town> and fund new buildings there.\"\n"
				"  \"We have spare cash — repay 50000 of our loan, then build a statue in <a town>.\"\n\n"
				"Stuck? Use \"Suggest a prompt\" or write a rough draft and hit \"Improve my prompt\".";
		}
		if (inflight > 0) text += "\n\nClaude is working…";

		DrawStringMultiLine(r.left + 4, r.right - 4, r.top + 3, r.bottom - 3, text, TextColour::Black, SA_LEFT | SA_BOTTOM);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		std::string draft(this->prompt_editbox.text.GetText());
		switch (widget) {
			case WID_CS_RUN:
				if (draft.empty()) return;
				ClaudeSubmit(ClaudeMode::Act, draft);
				this->prompt_editbox.text.DeleteAll();
				this->SetFocusedWidget(WID_CS_TEXTBOX);
				this->SetDirty();
				break;

			case WID_CS_IMPROVE:
				if (draft.empty()) return;
				ClaudeSubmit(ClaudeMode::Improve, draft);
				this->SetDirty();
				break;

			case WID_CS_SUGGEST:
				ClaudeSubmit(ClaudeMode::Suggest, "");
				this->SetDirty();
				break;
		}
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		bool redraw = false;
		bool execute = false;
		std::string actions_json;
		int score = -1;
		{
			ClaudeShared &s = GS();
			std::lock_guard<std::mutex> lock(s.mutex);
			if (s.dirty) { s.dirty = false; redraw = true; }
			if (s.has_pending_actions) {
				actions_json = s.pending_actions_json;
				score = s.pending_score;
				s.has_pending_actions = false;
				execute = true;
			}
		}

		if (execute) {
			/* Executing OpenTTD commands must happen on the main thread — which this is. */
			std::string result = ExecuteActionPlan(actions_json, score);
			ClaudeShared &s = GS();
			std::lock_guard<std::mutex> lock(s.mutex);
			s.transcript += "\nActions:\n" + result;
			redraw = true;
		}

		if (redraw) this->SetDirty();
	}
};

/** The widgets of the prompt sandbox window. */
static constexpr std::initializer_list<NWidgetPart> _nested_claude_sandbox_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey), SetStringTip(STR_CLAUDE_SANDBOX_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey, WID_CS_OUTPUT), SetMinimalSize(480, 260), SetResize(1, 1), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_EDITBOX, Colours::Grey, WID_CS_TEXTBOX), SetMinimalSize(380, 16), SetPadding(2, 2, 2, 2), SetResize(1, 0), SetStringTip(STR_NULL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CS_RUN), SetMinimalSize(70, 16), SetPadding(2, 2, 2, 0), SetStringTip(STR_CLAUDE_SANDBOX_RUN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CS_IMPROVE), SetMinimalSize(160, 14), SetPadding(0, 2, 2, 2), SetResize(1, 0), SetStringTip(STR_CLAUDE_SANDBOX_IMPROVE),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CS_SUGGEST), SetMinimalSize(160, 14), SetPadding(0, 2, 2, 0), SetResize(1, 0), SetStringTip(STR_CLAUDE_SANDBOX_SUGGEST),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

static WindowDesc _claude_sandbox_desc(
	WindowPosition::Center, "claude_sandbox", 520, 360,
	WindowClass::ClaudeAdvisor, WindowClass::None,
	{},
	_nested_claude_sandbox_widgets
);

void ShowClaudeAdvisorWindow(const std::string &question)
{
	Window *w = FindWindowById(WindowClass::ClaudeAdvisor, 0);
	if (w == nullptr) w = new ClaudeSandboxWindow(_claude_sandbox_desc);
	w->SetDirty();

	if (!question.empty()) ClaudeSubmit(ClaudeMode::Act, question);
}
