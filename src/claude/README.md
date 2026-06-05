# Claude Prompt Sandbox

A learn-to-prompt game mode inside OpenTTD. Instead of clicking to build, you **write prompts**.
Claude scores how well you prompted and then carries your prompt out in the game — the clearer
and more specific the prompt, the better the result.

## How to play

1. Start a new game (so you control a company).
2. Open the in-game console with the <kbd>`</kbd> / <kbd>~</kbd> key and type `claude` to open the
   **Claude Prompt Sandbox** window (or `claude <your prompt>` to open it and run a prompt at once).
3. In the window:
   - Type a prompt and click **Run**. Claude returns a **prompt-quality score** (clarity, specificity,
     constraints) with a coaching tip, then performs **real in-game actions**. A vague prompt scores low
     and does little; a specific one with a clear goal, named towns and amounts scores high and does more.
   - **Improve my prompt** — Claude critiques your draft and rewrites it better (no actions taken).
   - **Suggest a prompt** — Claude proposes a strong example prompt for your current game.

Prompt quality also applies a visible cash **efficiency bonus/penalty**, so better prompting literally pays off.

## Actions Claude can take (current vocabulary)

Town development & economy (executed via OpenTTD's command system, on the main thread):

- Advertise a town (small / medium / large)
- Fund new buildings in a town (rapid growth)
- Build a statue in a town
- Plant trees around a town
- Take / repay a loan (by amount)
- Rename the company / president
- Found a new town (if enabled in this game)

Transport construction (bus/rail lines, vehicle routing) is the planned next step.

## Configuration (environment variables)

Claude is reached through the Anthropic Messages API, so set your key before launching OpenTTD:

| Variable | Required | Default | Purpose |
|---|---|---|---|
| `ANTHROPIC_API_KEY` | **yes** | – | Your Anthropic API key (`sk-ant-...`). |
| `ANTHROPIC_MODEL` | no | `claude-sonnet-4-6` | Model id (`claude-haiku-4-5-20251001` is faster/cheaper). |
| `ANTHROPIC_BASE_URL` | no | `https://api.anthropic.com` | API base URL. |

```sh
export ANTHROPIC_API_KEY="sk-ant-..."
./openttd
```

Without a key the sandbox window still opens and explains how to configure one.

## How it works / implementation notes

- **`claude_advisor.{h,cpp}`** holds the whole feature. Claude is asked for **strict JSON**:
  `{ reply, score{overall,clarity,specificity,constraints,feedback}, actions[] }`.
- The HTTP call runs on a **background thread** (libcurl) so the game never freezes. The reply is
  handed to the **main thread**, which parses the action plan and executes each action through
  `Command<Commands::…>::Post(...)`, then applies the prompt-quality cash bonus. Game state is only
  ever touched on the main thread (`OnRealtimeTick`).
- JSON is built/parsed with the bundled `nlohmann/json`.
- `claudeexec` is a console command that runs a small canned plan to verify action execution
  without calling the API (development aid).
- Engine touch points stay minimal: one `WindowClass` value, two console commands, four GUI strings,
  and the CMake wiring.
