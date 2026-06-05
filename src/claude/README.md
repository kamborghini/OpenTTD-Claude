# Claude in-game advisor

Adds **Claude**, an AI strategy advisor, directly into OpenTTD. Ask it questions about
your transport company and it answers using a snapshot of your *current* game (year,
cash, loan, company value, performance rating, fleet counts, stations, towns).

## How to use

- Open the in-game console (press the <kbd>`</kbd> / <kbd>~</kbd> key) and type:
  - `claude` — opens the **Claude Advisor** window.
  - `claude why is my train losing money?` — opens the window and asks the question.
- Type follow-up questions in the window's input box and press <kbd>Enter</kbd> or click **Ask**.

## Configuration (environment variables)

The advisor talks to the Anthropic Messages API, so it needs an API key. Set these
before launching OpenTTD:

| Variable | Required | Default | Purpose |
|---|---|---|---|
| `ANTHROPIC_API_KEY` | **yes** | – | Your Anthropic API key (`sk-ant-...`). |
| `ANTHROPIC_MODEL` | no | `claude-sonnet-4-6` | Model id. Use `claude-haiku-4-5-20251001` for faster/cheaper replies. |
| `ANTHROPIC_BASE_URL` | no | `https://api.anthropic.com` | API base URL (for proxies/gateways). |

Example:

```sh
export ANTHROPIC_API_KEY="sk-ant-..."
./openttd
```

If no key is set, the window explains how to configure one — the game still runs normally.

## Implementation notes

- `claude_advisor.{h,cpp}` — everything lives here. No other engine behaviour is changed.
- The HTTP request runs on a **background thread** (libcurl, already a dependency) so the
  game never freezes while waiting for a reply. Results are handed back to the main thread
  and shown in the window via `OnRealtimeTick`.
- JSON is built/parsed with the bundled `nlohmann/json`.
- Touch points in the rest of the engine are intentionally minimal: one `WindowClass`
  enum value (`src/window_type.h`), one console command (`src/console_cmds.cpp`), two GUI
  strings (`src/lang/english.txt`), and the build wiring (`src/claude/CMakeLists.txt`,
  `src/CMakeLists.txt`).
