# Bugfix pass

This pass fixed real compile- and logic-level bugs found by reading every
file in the project end to end. The architecture (router-based LAN, Hub as
real-time authority, AI/cloud strictly advisory and non-blocking) was sound
and is unchanged. No files were removed; nothing here changes pinouts,
network layout, or the wire protocols in `docs/`.

## Compile-breaking bugs (project would not have built)

- **`firmware/ai-monitor/.../GarurAIMonitor_v1.ino`** — `void loop(){...}`
  declared a local `static uint32_t t=0;` WiFi-reconnect timer on the exact
  same physical source line as `loop()`'s own signature and opening brace.
  The Arduino build system auto-generates forward prototypes by scanning the
  file with a line-based (ctags) heuristic, and it misattributed `static` to
  `loop()` itself, emitting a bogus `static void loop();` prototype that
  conflicts with the real (non-static) `void loop(void);` already declared
  by the esp8266 core — `'void loop()' was declared 'extern' and later
  'static'`. `loop()` was the only function in the whole project with a
  local `static` variable fused onto its own signature line (every other
  local `static` in the codebase is on its own line inside the function
  body, which doesn't trigger this), so it was the only one that broke.
  Fixed by hoisting the timer to a file-scope `static uint32_t wifiRetryAt`
  alongside the other tracking variables, removing the local `static`
  from `loop()` entirely.
- **`firmware/sensor-node/.../GarurSensorNode_v2.ino`** — a leftover,
  invalid function `bool false { for (...) if ([i] != 0xFF) ... }` (dead
  code from an older ESPNow-era build that checked for a broadcast MAC
  address). `false` is a reserved keyword and can't be a function name, and
  `[i]` had no array — this could not compile. Removed it and the two
  `if (false) ...` branches that referenced it; router/HTTP mode has no MAC
  to check, so those branches were unreachable anyway.
- **`firmware/hub-brain/.../GarurHubBrain_v2.ino`** — `handleSensorConfig()`
  uses `HTTPClient`/`WiFiClient` to proxy a config POST to a sensor node, but
  the file never included `<ESP8266HTTPClient.h>` / `<WiFiClient.h>`. Added
  both (the sensor-node and AI-monitor firmware already had them).
- **`firmware/hub-brain/.../GarurHubBrain_v2.ino`** — two call sites used
  `WiFi.client()` to get the requesting client's IP for logging. That method
  doesn't exist on `ESP8266WiFiClass`. Changed both to `server.client()`,
  which is the actual `ESP8266WebServer` API for the in-flight request's
  client.
- **`firmware/hub-brain/.../GarurHubBrain_v2.ino`, `handleRoot()`** — the
  dashboard HTML was declared as a *local* `const char page[] PROGMEM = ...`.
  Newer ESP8266 core / GCC versions reject a `PROGMEM` (section) attribute
  on a local (stack) variable ("section attribute cannot be specified for
  local variables"). Added `static` so the string has static storage
  duration, which is required for it to legally live in flash.
- **`firmware/hub-brain/.../GarurHubBrain_v2.ino` and
  `firmware/sensor-node/.../GarurSensorNode_v2.ino`, `setupRoutes()`** —
  both called the old `server.collectHeaders(hdrs, 1)` array+count form.
  That overload doesn't exist in the ESP8266WebServer version shipped with
  current esp8266 cores (3.x); `collectHeaders` is now a variadic template
  that takes each header name directly. Changed both call sites to
  `server.collectHeaders("X-Garur-Token")`.

## Logic bugs

- **Hub `handleSensorConfig()` could forward broken JSON.** Before proxying
  a settings POST to a sensor node, the code tried to strip the `"nodeId"`
  key out of the body with manual string surgery. When `nodeId` happened to
  be the last key in the JSON object, the code miscalculated the removal
  range and deleted the closing `}` along with it, sending the sensor node
  truncated JSON. The sensor node's own config handler already ignores
  unknown keys, so the strip was unnecessary — removed it and the body is
  now forwarded as-is.
- **Showcase could fire immediately back-to-back with a plain fight.** The
  Hub tracks how long both sensors have continuously read "active"
  (`bothActiveSince`) to decide when to trigger a full SHOWCASE sequence.
  That timer was never reset when a plain FIGHT started, so it kept
  accumulating through the entire FIGHT + CELEBRATION scene. If visitors
  stayed in place, `bothActiveSince` could already be past the showcase
  threshold by the time the ordinary fight finished, firing a full
  intro/fight/celebration/recovery SHOWCASE immediately afterward with no
  rest period. Fixed by rebasing `bothActiveSince` to "now" whenever a plain
  FIGHT is triggered, so a fresh `bothActiveShowcaseSec` of continued
  presence is required after any fight before a showcase can start.
- **Nano slave's watchdog was fed but never armed.** `loop()` called
  `wdt_reset()` every cycle (and the header comment describes this build as
  "SAFE"), but nothing ever called `wdt_enable()`, so a genuine hang in
  `loop()` would never trigger a safety reset. Added `wdt_enable(WDTO_2S)`
  at the end of `setup()` (after the DFPlayer handshake, which can block for
  a bit) and `MCUSR = 0; wdt_disable();` at the very start of `setup()` to
  avoid the classic AVR errata where a watchdog-triggered reset can leave
  the watchdog flags set and cause an immediate reset loop on boot.

## Dead code / cleanup

- Removed `requireTokenOr403()` in the Hub firmware — defined, never called
  anywhere, and even where it might have been used it only sent a response
  without signaling failure to the caller, so it couldn't have worked as a
  guard clause even if it had been wired in.

## Docs vs. implementation mismatch

- `docs/firebase-schema.md` documented a `garur/aiActions` log that the AI
  monitor never actually wrote to, and the dashboard's `AI` tile
  (`j.aiAction`) had nothing to read because `garur/current` never included
  it either. Fixed the AI monitor to track the last action it actually
  applied to the Hub (`SET <field>=<value>` or `CLEAR_FAULT`, else `NONE`),
  include it in `garur/current`, and append a record to `garur/aiActions`
  whenever it takes a real action. Updated `docs/ai-protocol.md` and
  `docs/firebase-schema.md` to match. The dashboard already reads
  `j.aiAction` from `garur/current`, so it now populates correctly with no
  dashboard changes needed.
- Added an explicit `#include <Wire.h>` in the AI monitor firmware, since it
  calls `Wire.begin(4,5)` directly; it happened to work because U8g2lib
  pulls Wire in transitively, but relying on a transitive include for a
  symbol you call directly is fragile.

## Network configuration fix (deployment-specific, not a bug in the original code)

The original firmware hardcoded every device's static IP, gateway, subnet,
and DNS to the `192.168.1.x` example range from `docs/network-and-test-plan.md`.
On this deployment the router's actual LAN is `192.168.0.x` (confirmed via
`ipconfig`: Default Gateway `192.168.0.1`), so every board was configuring
itself with an unreachable gateway and none of them — Hub, sensor nodes, or
AI monitor — could be reached at their documented dashboard addresses.
Updated every `IPAddress` constant (Hub, both sensor nodes, AI monitor, the
Hub's `sensorIpForNode()` lookup) plus `docs/network-and-test-plan.md` and
`firmware/artnet-node/README.md` to `192.168.0.x`, keeping the same
last-octet scheme: Hub `.10`, entry sensor `.20`, exit sensor `.21`, ArtNet
`.30`, AI monitor `.40`. If you re-flash on a different router later, check
`ipconfig`/`ip addr` for the real gateway first and update these constants
(and the router's DHCP reservations) to match — do not assume `192.168.1.x`.

## Feature: ArtNet IP/universe now editable from the dashboard

Previously `cfg.artnetIp1..4` (the IP the Hub sends ArtDMX to) could be read
via `GET /api/config` but there was no way to *set* it — `POST /api/config`
silently ignored those keys, and the `/settings` page had no fields for it.
Since ArtNet nodes are often third-party hardware without a static-IP option
of their own, this made it impossible to point the Hub at a DHCP-assigned
ArtNet node without reflashing. Added `artnetIp1..4` to `POST /api/config`'s
accepted keys, and added IP octet / universe / send-interval fields to the
`/settings` page with a short note pointing at the router-reservation
workflow. Also fixed `defaults()`, which still set the ArtNet IP's third
octet to `192.168.1.30` even after the rest of the project moved to the
`192.168.0.x` subnet — missed in the earlier pass because it's built from
four separate `uint8_t` assignments rather than an `IPAddress(...)` literal
or dotted string, so it didn't match the grep used to verify that pass.

## Serial diagnostics added to the AI monitor

The AI monitor previously called `Serial.begin(115200)` but never printed
anything — Serial Monitor stayed blank by design; all status went to the
OLED instead. Added `[AI] ...` diagnostic prints at boot (firmware version,
WiFi connect attempt/result, Hub/Gemini/Firebase config summary), on each
WiFi reconnect attempt, on each 2-second Hub poll (success + state, or
failure), and at each step of the 5-minute cloud cycle (skip reason, Gemini
response/action taken, Firebase write result, running ok/fail tally). These
only fire on actual events, not every `loop()` iteration — at most once per
poll (2s) or reconnect attempt (15s), with the 5-minute cloud cycle printing
a handful of lines — so there's no measurable impact on loop timing, HTTP
calls, or OLED refresh.

## AI monitor: Gemini response was never actually applied (real bug, not just prompt quality)

Found while polishing the prompt: `applyAI()` called `deserializeJson(doc,
response)` directly on Gemini's raw HTTP response and read `doc["action"]`
at the top level. But `generateContent`'s response is an envelope --
`{"candidates":[{"content":{"parts":[{"text":"<our JSON, as a string>"}]}}]}`
-- the actual `{"action":...}` object is nested as a *string* inside
`candidates[0].content.parts[0].text`, not a top-level field. `doc["action"]`
on the envelope was therefore always missing and silently defaulted to
`"NONE"` via the `| "NONE"` fallback. In other words, no matter what Gemini
said, the AI monitor could never have actually applied a SET_CONFIG or
CLEAR_FAULT -- every cycle was a no-op regardless of prompt quality. Fixed
`applyAI()` to parse the envelope first, extract the nested `text`, then
parse *that* as the action JSON.

## Gemini prompt rewritten: clearer context, range enforcement, cross-cycle memory

- **Polished prompt.** Replaced the one-line terse prompt with one that
  explains each snapshot field (state/light/entry/exit/scores/nanoOk/fault),
  states the exact allow-listed settings and their valid ranges inline, and
  gives explicit rules (don't propose a path off the list, don't repeat the
  same change back-to-back, prefer NONE by default). Also set
  `generationConfig: { temperature: 0.1, responseMimeType: "application/json" }`
  on the request so Gemini favors consistent, deterministic output and
  returns JSON directly rather than prose or markdown fences around it.
- **Range enforcement now has one source of truth.** The old validation was
  an `if/else` chain of hardcoded ranges duplicated separately from the
  prompt text (which didn't mention ranges at all). Replaced both with a
  single `CONFIG_RANGES` table that `allowListText()` renders into the
  prompt and `findConfigRange()` checks the response against, so the model
  is told the exact bounds and the firmware enforces the same bounds
  regardless of what it returns -- an out-of-range or invented path is
  rejected and logged (`Serial`), never silently ignored without a trace as
  before.
- **Lightweight cross-cycle memory.** Gemini's API is stateless per request,
  and resending a full conversation history isn't practical on an ESP8266's
  RAM/flash or worth the extra tokens every 5 minutes. Instead, added
  `prevState`/`prevAction`/`sameStateCycles` (file-scope, a few dozen bytes)
  that carry a one-line summary of the previous cycle into the next prompt
  ("state was X, you took action Y, N consecutive cycles in this state"),
  so the model has continuity without a growing context window.
- **Reason logging.** Gemini's stated `reason` is now captured
  (`cache.lastReason`, sanitized against quote/backslash/newline injection
  since it's untrusted model output being hand-embedded into JSON we build)
  and included in the `garur/aiActions` Firebase log, so you can see *why*
  the AI made a change after the fact, not just what it changed.

## `libraries.md` was stale

Still listed `espnow` and `user_interface` for the Hub and sensor node --
leftovers from before this "v2 router" HTTP-based rewrite (grepped both
firmwares to confirm neither actually includes or uses them). Also missing
`ESP8266HTTPClient` for both (needed since the router architecture, and
already fixed as a compile error earlier in this pass), and the AI monitor's
section only listed `ArduinoJson`/`U8g2`, missing `ESP8266WiFi`,
`ESP8266HTTPClient`, and `WiFiClientSecure`. Corrected all three sections to
match what's actually `#include`d, and added a one-line note that every HTTP
call in the project is blocking (`HTTPClient`), not async.

## Hub: node config proxy could stall the show in the worst case (real bug)

`server.handleClient()` runs before `decideShow()`/`sendArtNet()`/`readNano()`
in `loop()` every iteration, so *anything* that blocks inside a request
handler blocks the whole show for that duration. Every Hub handler the AI
monitor or dashboard normally calls (`/api/config`, `/api/command`) only
touches RAM/EEPROM and returns fast -- except `handleSensorConfig()`
(`POST /api/sensorConfig`, the node-configuration proxy), which makes its
own *outbound* blocking HTTP call to the target sensor node before it can
respond. If that were ever triggered while a scene was live and the target
sensor was slow or unreachable, ArtDMX output and Nano ACK handling would
stall for as long as that call took -- and per upstream esp8266 core issue
reports, `HTTPClient::setTimeout()` isn't reliably guaranteed to bound the
TCP-connect phase on every core version, so the real-world worst case can
exceed the configured `1200ms`. Added a `busyShow()` guard at the top of
`handleSensorConfig()` that refuses the request outright (`503`) while a
scene is active, so this can never happen mid-show rather than relying on
it just not being triggered at the wrong moment. Note this doesn't cover
the more general (lower-probability, harder-to-fix-without-a-different-web-
server-library) risk of a slow/misbehaving TCP client stalling
`handleClient()` itself on any endpoint -- the project's closed-LAN,
trusted-devices network design is the practical mitigation for that class
of risk, not application code.

## Not changed (flagged, but intentional/documented behavior)

- All three network-facing firmwares treat the literal string
  `CHANGE_ME_LOCAL_TOKEN` as "auth disabled" so the system is usable on the
  bench before you've set a real token. `docs/release-checklist.md` already
  tells you to change it before deployment — left as-is.

## AI monitor: switched from direct Gemini API to OpenRouter

Replaced the direct `generativelanguage.googleapis.com` call with
OpenRouter's OpenAI-compatible `/chat/completions` endpoint, currently
pointed at `google/gemma-4-31b-it:free` (verified as a real, current
OpenRouter model as of this change). The prompt text itself is unchanged;
only the transport, auth, and response envelope changed:

- `GEMINI_ENABLED`/`GEMINI_API_KEY`/`GEMINI_MODEL` -> `AI_ENABLED`/
  `OPENROUTER_API_KEY`/`OPENROUTER_MODEL`, and `gemini()` -> `callAI()`, so
  the code isn't misleadingly named after a provider it no longer calls.
- Auth moved from Gemini's `x-goog-api-key` header to OpenRouter's standard
  `Authorization: Bearer <key>`.
- Response parsing in `applyAI()` now extracts
  `choices[0].message.content` (OpenAI-compatible shape) instead of
  `candidates[0].content.parts[0].text` (Gemini's shape) before parsing the
  nested action JSON -- same two-stage-parse pattern as before, different
  envelope.
- Added a defensive strip of ` ```json ... ``` ` code fences before parsing,
  since smaller/free models are more prone to wrapping JSON in markdown than
  a first-party flash-tier model was, despite the prompt explicitly asking
  for raw JSON.
- Added a check for OpenRouter's `{"error":{"message":...}}` shape (some
  providers return this with HTTP 200 in edge cases like content filtering)
  so a failure gets a clear Serial log line instead of a generic parse
  failure.
- Bumped the request timeout from 12s to 20s -- this model supports an
  optional "thinking" mode that can add response latency versus a
  first-party flash-tier endpoint.
- Updated `docs/ai-protocol.md`, `docs/release-checklist.md`,
  `docs/network-and-test-plan.md`, `libraries.md`, and a stale header
  comment in the Hub firmware that still said "never waits for
  Gemini/cloud work" -- swept the whole project for leftover "Gemini"
  references after the change, not just the AI monitor file.

**Security note:** the OpenRouter key is embedded in firmware source as
plaintext, same as every other credential in this project (WiFi password,
Hub API token, Firebase auth). Anyone who gets the `.ino` or the compiled
binary can extract it. Treat a key that's been shared anywhere outside your
own machine (including pasted into a chat) as burned, and regenerate it in
your OpenRouter dashboard before relying on this for a live event.

## AI monitor: 429 on first request -- switched to openrouter/free, added rate-limit diagnostics

`google/gemma-4-31b-it:free` was returning HTTP 429 on the very first
request the device ever made. Root cause has two independent parts, only
one of which is about which model was chosen:

1. **OpenRouter's free tier has a hard account-level cap, not a per-model
   one:** 20 requests/minute and 50 requests/day until you've purchased
   $10+ in credits at any point (lifetime), after which the daily cap rises
   to 1000 permanently. At `CLOUD_MS` (5 min), this device makes ~288
   calls/day if left running continuously -- it will exhaust the 50/day
   free cap around hour 4 of any single continuous run, regardless of which
   free model is targeted. This is pure math, not fixable by switching
   models. Left `CLOUD_MS` unchanged (a behavioral cadence choice, not a
   bug) but documented the tradeoff in a code comment: either purchase the
   one-time $10 credit, or widen `CLOUD_MS` to ~30+ minutes to stay under
   50/day indefinitely without it.
2. **Free models can also be individually oversubscribed ("shared
   capacity"), independent of your own account's quota** -- a popular or
   newly-released free model can 429 you even when you're nowhere near your
   own limits, because OpenRouter's free capacity for that specific model
   is shared across everyone using it. `google/gemma-4-31b-it:free` was
   released in April 2026 and is a large (31B) free model, a plausible
   candidate for this. OpenRouter's free-model catalog is also documented
   to churn quickly -- 20 free models a few weeks before this change, down
   to 14 by early August 2026, with entire provider tiers delisted and
   later replaced -- so hardcoding one specific free model ID is fragile
   for a device that can't easily be re-flashed mid-event.

Switched `OPENROUTER_MODEL` to `"openrouter/free"`, OpenRouter's own
first-party router that automatically selects a currently-available free
model per request (filtered for the features the request needs) instead of
targeting one fixed model -- directly addresses point 2, since OpenRouter
itself routes around whichever free model is short on headroom right now,
and continues working automatically as the free catalog rotates. Left a
comment explaining the tradeoff (which underlying model answers can vary
call to call) and how to pin a specific model instead if repeatable
behavior matters more than resilience -- reasonable here either way, since
every response is re-validated against `CONFIG_RANGES` regardless of which
model produced it.

Also, independent of model choice:
- `callAI()` now distinguishes HTTP 429 from other failures and from "AI
  disabled", logging it explicitly (with `Retry-After` if OpenRouter sent
  one) instead of every failure mode looking identical as an opaque "no
  response". Deliberately does *not* retry within the same cycle -- we
  already call once per `CLOUD_MS`, and blind retries would only burn more
  of a scarce daily quota chasing the same rate limit.
- `applyAI()` now logs which underlying free model actually answered each
  cycle (`envelope["model"]`, present in every OpenRouter response) --
  useful for exactly this kind of debugging now that the served model can
  vary, and free since it's already in the response.

## AI monitor: switched from OpenRouter to Groq

Requested switch after the OpenRouter 429s, with the model choice left to
me. Picked Groq (`api.groq.com`) over staying on OpenRouter/another
aggregator: same OpenAI-compatible request/response shape (minimal
transport-layer diff), and its free tier's numbers -- verified live against
`console.groq.com/docs/rate-limits`, not blog summaries, several of which
disagreed with each other and turned out to be citing stale model lineups
-- are a meaningfully better fit for this project's usage pattern.

**Model: `openai/gpt-oss-20b`.** Free tier, no credit card: 30 requests/min,
1,000 requests/day, 8K tokens/min, 200K tokens/day. At `CLOUD_MS` (5 min,
~288 calls/day continuous) that's ~3.5x headroom on requests with no
purchase required at all -- a real improvement over OpenRouter's free tier,
where the same cadence exhausted the 50/day cap in about 4 hours. Token
budget is comfortable too: this prompt+response is roughly 450-500
tokens/call, well under the 8K/200K caps at this cadence. Picked over the
identically-rate-limited `openai/gpt-oss-120b` and `qwen/qwen3.6-27b`
because smaller/faster is a better fit for a small, well-specified JSON
decision task, and it's the model Groq's own Structured Outputs
documentation uses in its example.

**Adopted Groq's Structured Outputs** (`response_format: {"type":
"json_schema", "json_schema": {..., "strict": true}}`) instead of relying
on prompt instructions alone. With `strict: true`, Groq uses constrained
decoding so the response is guaranteed to match the schema exactly --
eliminates an entire class of failure the OpenRouter/Gemini versions had to
defend against with prompt wording and a markdown-fence-stripping
workaround (kept as a zero-cost fallback regardless, in case the model or
provider changes again to one without this guarantee). The exact schema
string was validated with `python3 -c "import json; json.loads(...)"`
before being embedded in firmware, rather than hand-counting nested braces.

**Other changes:** `OPENROUTER_API_KEY`/`OPENROUTER_MODEL` ->
`GROQ_API_KEY`/`GROQ_MODEL`; endpoint -> `api.groq.com/openai/v1/chat/
completions`; dropped the OpenRouter-only `X-Title` header (not applicable);
`Authorization: Bearer` auth is unchanged, both providers use the same
convention. The old OpenRouter key that had been embedded in firmware
source was removed rather than left in as dead code -- `GROQ_API_KEY` is a
fresh `CHANGE_ME_GROQ_KEY` placeholder, since no Groq key was provided.
Swept `docs/ai-protocol.md`, `docs/network-and-test-plan.md`,
`docs/release-checklist.md`, and `libraries.md` for OpenRouter references
and updated them to match.

**Known residual risk, stated plainly:** Groq's own current rate-limit
table (fetched live) lists a noticeably different model lineup than every
blog post written even a few months earlier -- several previously-free
Llama/Mixtral/Gemma models aren't on it at all anymore. Groq doesn't offer
an OpenRouter-style auto-router fallback, so if `openai/gpt-oss-20b` is
ever retired from the free tier, this needs a manual model swap (one line)
rather than resolving itself automatically the way the OpenRouter version
now does.

## First live end-to-end run: found two real bugs from the actual data

The user shared Serial output, a live `/api/fullStatus` response, and their
Groq usage dashboard from the first real cloud cycle. The pipeline worked
end to end (poll -> cloud cycle -> Groq call -> Firebase write -> next
poll, timed almost exactly 5 minutes apart as `CLOUD_MS` intends) -- but
the data surfaced two real, previously-undetected bugs:

**1. `CLEAR_FAULT`'s precondition could never be satisfied.** It required
`cache.nanoOk && fault is NANO_LINK_TIMEOUT/NANO_ACK_TIMEOUT`. But on the
Hub, `nanoOk` is computed as `!nanoFault`, and `clearFault()` blanks the
fault string in the exact same call that clears `nanoFault` -- so "nanoOk
true AND fault still populated" is a combination the Hub can never actually
report; confirmed by reading `nanoOk`'s formula at both `/api/status` and
`/api/fullStatus` call sites. The *prompt* had the identical bug baked in
independently, telling the model to watch for a field combination that
could never appear in the very snapshot JSON it was given (since `nanoOk`
and a non-empty `fault` are mutually exclusive in that JSON too) -- so
`CLEAR_FAULT` could never have fired regardless of what the model wanted,
for two independent reasons. Rewrote both: the prompt now says to propose
`CLEAR_FAULT` when `nanoOk is false AND fault` is one of the two transient
types (matching what the data can actually show), and `applyAI()`'s
precondition now only rules out an unrecognized fault type before
forwarding, rather than duplicating the Hub's own recovery-threshold logic
-- the Hub's `/api/command` handler already has the authoritative guard
(rejects with `409` unless `nanoHealthyResponses >= cfg.nanoRecoverGoodResponses`,
which `hubPost()` correctly reads as failure), so this follows the same
"Hub is the real-time authority, AI monitor does a cheap sanity check"
pattern used elsewhere in the project rather than re-implementing the
Hub's exact threshold on the AI-monitor side.

**2. The Hub's own EEPROM had a stale `artnetIp3` from before the
192.168.0.x subnet fix.** The live `/api/fullStatus` showed
`"artnet":{"ip":"192.168.1.30",...}` despite the source-level fix from
earlier in this pass. Every *other* live config value matched current
source defaults exactly (`fightDurationSec:40`, `celebrationDurationSec:20`,
etc.) -- only `artnetIp3` differed, which pinpoints the cause: `loadConfig()`
only calls `defaults()` when `cfg.magic != CONFIG_MAGIC`, so a board that
was ever booted before the subnet fix keeps its pre-fix EEPROM values
forever across reflashes, no matter how correct the source is, until
something forces a fresh load. Bumped `CONFIG_MAGIC` (0x47525532 ->
0x47525533) to force exactly that on next boot -- confirmed safe since every
other field already matched current defaults, so no real tuning is lost.
Added a note to `docs/release-checklist.md`: any future change to a
`defaults()` value needs either another `CONFIG_MAGIC` bump or a manual
`/settings` update on already-flashed boards, or it'll silently not take
effect the same way this one didn't.

**Also corrected a claim from the Groq switch changelog entry above,** now
that real usage data exists: measured ~740 input + ~200 output =~ 940
tokens/call (vs. the ~450-500 guessed there), meaning the 200K
tokens/day cap -- not the 1,000 requests/day cap -- is the actual binding
constraint at `CLOUD_MS`'s cadence, reached after ~211 calls (~17.6 hours of
*continuous* operation) rather than not being a practical concern at all.
Still comfortably fine for a single event of any normal length; only
matters for a multi-day always-on deployment. Updated the code comment to
match.

**Diagnostic note, not a firmware issue:** the shared `/api/fullStatus`
also shows `"nano":{"ok":false,"fault":"NANO_LINK_TIMEOUT","healthyResponses":0,...}`
-- zero healthy responses since the fault triggered means the Nano hasn't
been heard from at all, not a flag stuck after real recovery. That's a
physical link/power/wiring question on the actual hardware, not something
either the Hub or the AI monitor can resolve from software.

## Surfaced the model's actual response -- previously invisible

Until now, the only visibility into what Groq returned was the parsed
one-word outcome (`[AI] action=NONE`) -- the model's actual JSON, and
critically its stated `reason`, were extracted and then silently discarded
for the common "NONE, nothing to do" case (the `reason` was only ever sent
to Firebase when an action was taken, via `garur/aiActions`). No way to
tell a well-reasoned NONE from a confused one. Added visibility in three
places:

- **Serial:** `applyAI()` now prints the raw `choices[0].message.content`
  text verbatim right after extracting it (before any cleanup), and the
  parsed `reason` on its own line regardless of what action was decided.
  The raw-text line is also the most useful thing to have on hand if the
  JSON parse ever fails, since it shows exactly what didn't parse.
- **Firebase:** `garur/current`'s `aiReason` field is now always included
  (previously reasoning only reached Firebase via `garur/aiActions`, and
  only when `aiAction != NONE`), so the current reasoning is checkable
  remotely, not just over Serial.
- **Dashboard:** `dashboard/index.html`'s AI tile now shows the reason as
  small text under the action, reading the same `aiReason` field.

## Feature: per-scene colour/effect/intensity, configurable from the Hub UI

Requested, then rescoped mid-discussion: AI stays completely out of
lighting (dropped entirely, not just unused -- see below); instead the Hub
gets a real per-scene lighting config editable from `/settings`, stored in
EEPROM so it works without the UI (or network) present during an actual
show, matching the project's existing "Hub is the real-time authority"
pattern for everything else.

**Scope, exactly as specified:** colour, light effect, and intensity are
configurable per scene. Scene *timing* (durations, cooldowns, all the
existing `*Sec` fields) is unchanged and untouched by this feature. Relay
channels 13/14 stay exactly as they were hardcoded before -- not part of
this config -- since the ask was specifically colour/effect/intensity.

**Design, in `GarurHubTypes.h`:**
- `SceneLight { r,g,b, r2,g2,b2, intensity, effect }` (8 bytes) -- a
  primary colour, a secondary colour (only used by two-colour effects),
  an intensity that scales brightness independent of hue (there's no
  hardware dimmer channel on these fixtures, so this scales R/G/B in
  software before writing to DMX), and which effect to run.
- `LightEffect`: `SOLID | BLINK | STROBE | ALTERNATE | CHASE` -- generic,
  reusable patterns rather than bespoke code per scene. `HubConfig` gained
  `SceneLight sceneLight[4]`, indexed by `LightScene` (IDLE/NORMAL/FIGHT/
  CELEBRATION; no entry for BLACKOUT, which is always all-zeros by
  definition). Struct grew from ~48 to 80 bytes -- still trivial against
  the 512-byte `EEPROM_SIZE` budget (checked with a throwaway sizeof()
  program, not eyeballed).

**`applyLight()` rewritten** from one bespoke `switch(scene)` with
hardcoded colour logic per case into a generic effect dispatcher operating
on `cfg.sceneLight[s]`, plus a separate small switch that preserves the
old relay timing exactly (relays were deliberately kept out of the new
config, so their logic didn't need to change, just get reorganized).
Removed `idleBlue`/`normalBlue`/`normalAmber` (the only 3 lighting knobs
that existed before this) now that the richer per-scene struct supersedes
them -- also removed the matching 3 dead entries from the AI monitor's
`CONFIG_RANGES` allow-list, since those Hub fields no longer exist and
they'd have silently done nothing if left in.

**Defaults chosen to match the old hardcoded look as closely as the new
generic model allows** -- disclosed honestly rather than overclaiming
exact preservation: IDLE, NORMAL (a static two-fixture-group split, not
time-animated), and CELEBRATION (steady colour -- only its relay ever
blinked, and that's unchanged) reproduce the old look exactly. FIGHT's old
look was a bespoke 4-phase sequence unique to that one scene; the new
`EFFECT_STROBE` default is a genericized approximation (fast white/red
alternation across all fixtures) in the same spirit, not a pixel-identical
replay -- the tradeoff for one reusable effect system instead of bespoke
code per scene, and it's just as adjustable as everything else now.

**Storage/compat:** another `CONFIG_MAGIC` bump (0x47525533 -> 0x47525534)
-- required this time, not just precautionary, since `HubConfig`'s layout
actually changed (fields removed, a 32-byte array added), so old EEPROM
bytes can't be safely reinterpreted as the new struct.

**API:** `GET/POST /api/config` gained 32 new keys (8 fields x 4 scenes:
`idleR/G/B/R2/G2/B2/Intensity/Effect`, and the same for `normal`/`fight`/
`celeb`). Used small helper functions (`appendSceneJson`/`jsonSceneFields`/
`setSceneField`) that loop or get called once per scene rather than
writing out 32 near-identical lines by hand three times over (GET, JSON
POST, and form POST each needed the same 8 fields x 4 scenes).

**UI:** `/settings` gained a "Scene Lighting" section, one block per scene,
with R/G/B number inputs for both colours, an intensity number input, and
an effect dropdown -- same plain-HTML-input style the rest of the page
already uses, no new JS.

## Follow-up: fixture/relay assignment, colour picker, styling, and a real memory check

Extended the feature above per follow-up requests, and did the performance
check that was explicitly asked for with actual measurements, not guesses.

**Per-fixture and per-relay assignment.** `ALTERNATE` and `CHASE` used to
hardcode which of the 4 PAR fixtures (channels 1-12, 3 each) played which
role (fixtures 1&3 vs 2&4). Added `fixtureGroupMask` to `SceneLight` (bit
per fixture, group A/B) so any grouping is choosable per scene, via 4
dropdowns in the UI. `CHASE` cycles through whichever fixtures are in group
A one at a time; if none are (all assigned B), it falls back to all-secondary
rather than risking a divide-by-zero on an empty group.

Relays (channels 13/14) were explicitly out of scope in the previous round
("stays exactly as hardcoded") -- reversed on request. Replaced the old
fully-hardcoded per-scene relay switch (and the `fightStrobeRelayEnabled`
toggle it depended on) with `relay1Func`/`relay2Func` per scene, each one of
`Off | On | Blink | Strobe`, dispatched through one generic
`applyRelayFunction()` instead of bespoke per-scene code. `fightStrobeRelayEnabled`
removed from `HubConfig` entirely (superseded) -- confirmed no other file
still referenced it (Hub API, UI, AI monitor's allow-list, docs) before
removing it.

**Colour picker.** Each colour slot (primary and secondary, per scene) got
an `<input type=color>` next to its R/G/B number fields, synced via one
small vanilla-JS function (`syncColor()`, ~150 bytes) that splits the
picked hex value into the three number inputs on `oninput` -- no library,
consistent with the project's fully-offline requirement (the Hub's own UI
has to work with zero internet, so no CDN-hosted anything). The number
inputs stay visible and editable too, for fine-tuning past what a colour
wheel picks precisely.

**Styling.** Added an inline `<style>` block (~700 bytes) -- readable
font, card-style sections with subtle shadow/spacing, consistent input/
select styling, a proper button. No framework, no external stylesheet,
same offline-first reasoning as the colour picker.

**Storage:** `SceneLight` grew from 8 to 11 bytes (`fixtureGroupMask`,
`relay1Func`, `relay2Func`); `HubConfig` is now 92 bytes total against the
512-byte `EEPROM_SIZE` budget -- computed with a throwaway `sizeof()`
program, not estimated. Another `CONFIG_MAGIC` bump (0x47525534 ->
0x47525535), required since the struct layout changed again.

**The memory check, actually done:** built a native C++ test harness
(`std::string`, not Arduino `String`, but character-for-character identical
concatenation logic and content) reproducing `handleSettings()` exactly,
ran it with the real default values, and measured the output: **~12KB**
fully rendered (breakdown: ~1.4KB head/style/script, ~1.2KB timing card,
~0.7KB ArtNet card, ~2.15KB average per scene card x4). That's a real,
non-trivial fraction of a typical ESP8266's free heap (usually tens of KB
after the WiFi/TCP stack's own overhead), and a single ~12KB *contiguous*
allocation specifically is the kind of request that gets harder to satisfy
as heap fragments over a long uptime, even when aggregate free memory looks
fine on paper.

Rather than report that number and hope it's fine, changed
`handleSettings()` from building one large buffered `String` to streaming
the response in chunks (`server.setContentLength(CONTENT_LENGTH_UNKNOWN)` +
repeated `server.sendContent()`, sending and clearing each section before
building the next) -- a long-standing, well-documented ESP8266WebServer
pattern for exactly this problem. This drops peak memory from ~12KB to
whatever the single largest chunk is (~2.4KB, one scene card), roughly a 5x
reduction, at zero cost to correctness or user-visible behavior: this
endpoint is a manual page a human loads occasionally, not part of the
real-time show loop, so a few extra round trips to the TCP stack don't cost
anything that matters. Checked community bug reports on this exact
ESP8266WebServer pattern before relying on it (given how many core-API
surprises this project has already hit this pass) and found one genuine,
still-relevant gotcha: a chunked response needs an explicit empty
`sendContent("")` at the end or some clients can hang or render an
incomplete page waiting for more -- added that. `handleSaveSettings()` and
the JSON API (`GET/POST /api/config`) were unaffected by this change; only
`handleSettings()`'s internals changed, not its externally-visible
behavior or the data it renders.

## Feature: Nano actuator config (wing, lip, DFPlayer) now Hub-controlled

Previously the Nano's wing motor speeds, lip servo angles, and DFPlayer
track selection were all `const` values hardcoded in the Nano's own
source -- changing any of them meant reflashing the Nano. Extended the
Hub<->Nano protocol and the Hub's config/UI to make all of it
Hub-configurable and EEPROM-backed, with the Nano needing a firmware
update only if the *protocol* itself changes, not for routine retuning.

**Correcting an assumption from the request:** wing speed was believed to
already be Hub-controlled -- it wasn't. The Hub only ever sent which scene
and its duration/start-delay; the actual PWM values were Nano-local
constants the Hub had no way to influence.

**Confirmed and unchanged:** the magnetic home sensor stays wired directly
to the Nano (not the Hub) -- homing is a tight local feedback loop
(checked every ~5ms) that a Hub round-trip over UART can't keep up with
without overshoot. This was a design constraint, not a preference, so it
wasn't changed.

**Protocol:** added `CFG,<code>,<value>` as a new Hub->Nano message type,
alongside the existing `PING`/`CMD`. Deliberately simpler than `CMD`'s
ack/retry/dedup machinery: CFG values are idempotent (resending the same
field twice is harmless), so the Hub just walks every field in order on a
~150ms interval whenever a sync is triggered (Hub boot, every `/settings`
save, and Nano fault-recovery -- covers a Nano that rebooted mid-fault
without needing Nano-side EEPROM) rather than tracking per-field dirty
state or acks. The 150ms pacing between lines is deliberate: bursting ~20
lines at once risked overwhelming the Nano's soft-serial link while it's
also servicing DFPlayer/servo work. Nano replies `CFGACK,<code>,<value>`
per field for Serial-visible confirmation on the Hub's own USB debug
output (separate from the Nano link itself) -- diagnostic only, the Hub
doesn't act on it.

**Nano-side generalization:** replaced the per-scene hardcoded
`switch(scene)` block (bespoke wing/lip/audio logic written out once per
scene) with a config-driven model: `WingMode{Stop,Spin,Home}`,
`LipMode{Closed,Animate}`, and a DFPlayer file count, looked up from
`sceneCfg[scene]`. `SCENE_STOP` is the deliberate exception -- always a
hardcoded full safe-stop, never exposed to `sceneCfg[]`/remote config,
since it's the fallback used when something's already wrong and shouldn't
be something a bad config value could compromise.

**Disclosed, not hidden: two real behavior changes from generalizing.**
1. `processSceneDuration()`'s end-of-duration failsafe settle (stop
   spinning/animating if the Hub's next command hasn't arrived) used to
   only apply to `FIGHT`/`CELEBRATION` by hardcoded name. Now applies to
   any scene whose config says it's spinning/animating -- which means
   `NORMAL` (and its showcase sub-states, which do get real non-zero
   durations) now gets the same defensive settle-down if its next command
   is ever late. A deliberate robustness improvement, not just a refactor,
   but a real change in when it fires.
2. DFPlayer folder-addressing wasn't confidently verified to combine with
   the library's single-track-loop mode, so Idle's "continuous ambience"
   is achieved differently than before: rather than looping one track, it
   detects when the current clip finishes (`dfPlayer.available()` +
   `readType()==DFPlayerPlayFinished`) and picks a fresh random clip from
   its folder. Practical effect is similar (continuous ambience) but
   clips vary instead of repeating identically -- a deliberate choice to
   rely only on DFPlayer library behavior that's confidently correct here,
   not a library limitation being worked around apologetically.

**DFPlayer:** SD card layout changed from flat `/mp3/0001.mp3` numbering
to folder-per-scene (`/01/`../04/`, folder = scene index + 1), addressed
via `playFolder(folder, file)`. File count per folder is Hub-configurable
per scene ("Sound clips in folder" in `/settings`); the Nano picks one at
random each time that scene activates. Updated `libraries.md`'s SD card
layout section and added a release-checklist reminder that the file
naming convention (exactly 2-digit folders, 3-digit files) is DFPlayer
Mini's own hardware addressing scheme, not a library choice -- getting it
wrong won't error, it'll just silently not find the files.

**Storage:** `NanoSceneCfg{wingMode,wingSpeed,lipMode,dfFileCount}` (4
bytes) x4 scenes + 4 global bytes (wing home speed, lip closed angle, lip
open min/max) added to `HubConfig`, now 112 bytes against the 512-byte
budget -- computed with a throwaway `sizeof()` program, not estimated.
Another `CONFIG_MAGIC` bump (0x47525535 -> 0x47525536), required since the
layout changed again.

**Settings page re-measured after this addition** (same native-harness
approach as the styling changelog entry above, not re-estimated from
scratch): total page grew from ~12KB to ~15.5KB, but since it's streamed
in chunks rather than built as one buffer, what actually matters --
peak single-chunk size -- only grew from ~2.15KB to ~2.9KB (Normal's
card, now the largest with both lighting and actuator fields). This is
the streaming refactor doing exactly what it was for: adding substantially
more content to the page no longer means a proportional increase in peak
memory risk. Bumped the per-scene chunk's `reserve()` from 3100 to 3300 to
keep comfortable headroom over the newly-measured 2917-byte peak.

**API:** `GET/POST /api/config` gained 20 new keys (4 scenes x
`WingMode`/`WingSpeed`/`LipMode`/`DfFileCount`, plus 4 global
`wingHomePwm`/`lipClosedAngle`/`lipOpenMin`/`lipOpenMax`), via
`appendNanoSceneJson`/`jsonNanoSceneFields`/`setNanoSceneField` mirroring
the existing `SceneLight` helpers structurally, kept as separate functions
since the two structs are conceptually different (lighting vs. actuators)
despite sharing the same per-scene indexing.

**UI:** each scene's `/settings` card gained Wing mode/speed, Lip mode,
and DFPlayer file count fields; a new "Nano Actuators (global)" card holds
the 4 non-per-scene values.

## Full end-to-end review: sensor node -> Hub -> Nano -> AI monitor

Requested explicitly, done as a fresh read of every file rather than
relying on memory of earlier passes, specifically cross-checking each
interface boundary's two sides against each other rather than reviewing
each file in isolation.

### Major finding: the AI monitor has been reading the wrong data this whole time

`updateCache()` parsed `/api/fullStatus` with naive flat-key substring
search (`jsonString`/`jsonBool`/`jsonUInt`, all three now removed --
they're dead now that nothing calls them). That silently failed for every
field that actually lives inside a nested object in the real response:
`entryActive`/`exitActive`/`nanoOk`/`entryActivityScore`/
`exitActivityScore` don't exist as those exact substrings anywhere in the
Hub's JSON -- the real paths are `sensors.entry.active`, `nano.ok`,
`sensors.entry.activityScore`, etc. Only `state`/`light`/`fault` happened
to work, because those particular keys occur as apparently-flat-looking
substrings regardless of the surrounding nesting (each occurs exactly
once in the whole document, so a substring search finds the right thing
by luck, not because the parser understands structure).

**Net effect: `cache.entry`, `cache.exit`, `cache.es`, `cache.xs`, and
`cache.nanoOk` have been stuck at their zero-init defaults (`false`,
`false`, `0`, `0`, `false`) since this file was first written** -- every
Groq prompt, every OLED readout, and every Firebase `garur/current` write
has been reporting "nobody's ever at either sensor, 0% activity, Nano's
never OK" regardless of the Hub's actual live state. This wasn't caught
by the live test earlier in this project specifically because that test
had nothing physically connected -- entry/exit genuinely were inactive
and the Nano genuinely wasn't OK in that scenario, so the bug's
failure-mode defaults happened to exactly match ground truth and mask the
problem completely. It would have surfaced the moment a sensor actually
detected someone.

Fixed by switching `updateCache()` to ArduinoJson (already a dependency
in this file for the Groq response, so no new library cost) instead of
assuming a flat structure. Every downstream consumer of these cache
fields -- the Groq prompt snapshot, the OLED display, the Firebase write
-- needed no changes themselves, since they only read the struct fields;
fixing how those fields get populated fixed all of it at once.

### Other findings, smaller but real

- **Sensor node: `handleApiConfig()` (the remote/Hub-facing config
  endpoint) was missing `mediumAngleCount`/`highAngleCount`**, both of
  which the sensor's own local browser form (`handleSave()`) already
  accepted. Anyone extending the Hub to configure these two fields
  remotely would have found it silently do nothing. Added both to
  `handleApiConfig()`'s accepted keys.
- **Sensor node dashboard JS referenced a `hubLedState` value
  (`'UNVERIFIED'`) that can never actually be produced** -- a leftover
  from the ESPNow-era dead code removed earlier in this project's very
  first bugfix pass. `getHubLedStateText()` only ever returns one of three
  strings, none of which is "UNVERIFIED". Removed the dead check.
- **Hub: two stale doc comments** didn't reflect later changes made in
  the same overall session: the file's own header comment listing
  Nano UART message types hadn't been updated when `CFG` was added, and
  `handleSettings()`'s memory-sizing comment still cited the pre-Nano-
  actuator-feature measurement (~12KB total / ~2.2KB peak chunk) instead
  of the current one (~15.5KB / ~2.9KB). Both corrected.
- **Confirmed, not a bug:** `TrackerRuntime.lastZone`/`lastEventType`/
  `lastEventId` are written on every sensor POST but never read anywhere
  -- not in show logic, not in any API output. Harmless (a few bytes of
  unused state per tracker), but worth knowing about if you're looking
  for where zone/event data might already be available for a future
  feature -- it's captured, just not surfaced or acted on yet.
- **Confirmed, not a bug:** the Hub's `nanoScene()` (ShowState -> scene
  name string sent to the Nano) and the Nano's `parseSceneName()` (string
  -> its own `Scene` enum) were cross-checked directly against each
  other, field by field. Every `ShowState`, including every `SHOWCASE_*`
  substate, maps to exactly one of the 5 strings the Nano recognizes, and
  the Nano's `default: return SCENE_STOP` fallback only ever triggers on
  a genuinely unrecognized string. No gap.
- **Confirmed, not a bug:** the new `CFG` protocol's scene-letter prefix
  (`I`/`N`/`F`/`C`) is resolved to each side's *own* `Scene`/`LightScene`
  enum by name on both ends independently (`if (code[0]=='I') idx =
  SCENE_IDLE`), not by assuming the two sides' numeric enum values happen
  to match. The *value* fields (`WingMode`/`LipMode` numeric codes) do
  need to match numerically between the two separate sketches, and were
  verified to (both currently `STOP=0, SPIN=1, HOME=2` / `CLOSED=0,
  ANIMATE=1`) -- flagged as the one place in this protocol a future
  edit to either sketch alone, without updating the other, would silently
  break, since nothing catches a mismatch here at compile time or runtime.

### Also verified clean, no changes needed
- No ctags auto-prototype landmines (the `loop()`-local-`static` class of
  bug from earlier in this project) anywhere across all four `.ino`
  files, re-checked fresh.
- No stray non-`#include`/`#define` preprocessor directives.
- No leftover `TODO`/`FIXME`/`XXX` markers.
- `EEPROM_SIZE` headroom on both Hub (112/512 bytes) and sensor node
  (well under its 300-byte budget, struct unchanged this whole project).
- `dashboard/index.html` reads Firebase's flat `garur/current` document,
  which the AI monitor itself builds (correctly, once the fix above is in
  place) -- confirmed no separate mismatch there, since it was never
  reading the Hub's nested API directly.
- `docs/ai-protocol.md` still accurately describes current behavior, no
  changes needed.

## Validation audit: every config field, every write path, inline

Requested explicitly: confirm every AI-monitor-settable field (and, more
broadly, every config field anywhere in the project) goes through real
min/max validation, not just an assumption that it does.

**What was found, precisely -- including a self-correction made mid-review
rather than left standing.** Initially suspected the sensor node had zero
validation (an over-hasty grep for a `static`-qualified function missed
its actual, non-`static` `validateSettings()`). Re-checked properly:
`validateSettings()` is real, thorough, and every write path
(`handleSave()`, `handleApiConfig()`) already calls `saveSettings()`,
which calls it before every persist. Cross-checked every one of its ~24
default values against its own clamp range by hand -- all pass cleanly,
nothing gets silently rewritten on first boot. No sensor-node change was
needed; correcting the record here rather than letting the earlier
overstatement stand.

**The real, confirmed gap was on the Hub.** The 8 fields the AI monitor
can set (and every other original timing/threshold field alongside them
-- `sensorActiveHoldSec` through `artnetSendIntervalMs`, 18 fields total)
were raw-assigned via the `SET_U16`/`SET_U8` macros in `handleConfigPost()`
and via direct `cfg.field=n` in `handleSaveSettings()`, with **no bound
at the point of assignment** -- validation only happened later, when
`saveConfig()` happened to call `validateConfig()`. This never caused an
observable bug (ESP8266 is single-threaded and `saveConfig()` always runs
before either handler returns, so nothing else ever reads `cfg` in the
gap), but it was a real inconsistency: the scene-lighting and Nano-
actuator fields added later in this pass *do* validate inline, via
`jsonSceneFields()`/`jsonNanoSceneFields()`/`setSceneField()`/
`setNanoSceneField()`'s own `constrain()` calls. The original fields
never got the same treatment.

Fixed by defining every bound once, as named constants
(`NO_PEOPLE_IDLE_MIN`/`MAX`, `FIGHT_DURATION_MIN`/`MAX`, etc. -- 18 pairs,
36 constants), placed immediately before `validateConfig()` with a
comment explaining why they're there. Redefined `SET_U16`/`SET_U8` to
take explicit `lo,hi` and `constrain()` inline, updated every call site in
`handleConfigPost()` to pass its matching bound, and did the same in
`handleSaveSettings()`. `validateConfig()` itself now references the same
named constants instead of repeating the numbers -- so the bound for any
given field is written in exactly one place, read from three (both
inline-validating write paths, plus the final consistency pass), rather
than a number that could drift between two or more places that each typed
it separately. Verified programmatically (not by eye) that every declared
constant is actually referenced everywhere it should be, with no typos in
the names. All 18 bound *values* are unchanged from what `validateConfig()`
already enforced -- this is a pure hardening refactor (validate earlier
and in more places), not a behavior change to what values ultimately get
accepted.

**Confirmed, not changed:** the AI monitor's own `CONFIG_RANGES` table
was re-checked against the Hub's (unchanged) bounds field by field --
still a strict subset in every case (e.g. AI allows `fightDurationSec`
5-120, Hub allows 5-300), meaning the AI's client-side check was already
more conservative than the Hub's authoritative range everywhere, not less.
The Nano's own `applyNanoCfg()` was also re-checked: it already
`constrain()`s every field it receives over the `CFG` protocol, a third
independent validation layer on top of the Hub's two. Full chain, now
confirmed correct end to end: AI monitor (narrowest) -> Hub inline (at
parse time) -> Hub `validateConfig()` (final consistency pass, also
handles cross-field constraints like entry/exit node IDs needing to
differ, which no single-field bound can express) -> Nano `applyNanoCfg()`
(defensive re-validation on receipt). No single point of failure if any
one layer were ever bypassed or a future edit forgot to add a bound
somewhere.

## Two real bugs, both found by reading the code directly, not by me

Both confirmed exactly as described before fixing -- neither was a
misunderstanding, and both were genuine gaps this project's own earlier
passes (including mine) had missed.

**1. WiFi fallback had no way home.** `startFallbackAP()` put the Hub into
pure `WIFI_AP` mode when the router wasn't reachable at boot, and
`maintainWiFi()`'s very first line was `if (WiFi.getMode() == WIFI_AP)
return false;` -- meaning once a Hub ever fell into the fallback AP (e.g.
the router took longer to boot than the Hub after a shared power outage,
an entirely normal real-world sequence), it would *never* attempt the
router again, for any reason, short of a manual power-cycle. Fixed by
switching the fallback to `WIFI_AP_STA` (serves the setup AP *and* keeps
attempting the router in the background) and rewriting `maintainWiFi()` to
retry `WiFi.begin()` on the same 15s cadence regardless of current mode,
recognizing a successful reconnect and cleanly dropping back to STA-only
once the router is reachable again.

**2. The release checklist told you to break your own Settings page.**
`/settings`'s Save button is a plain HTML `<form method='POST'
action='/saveSettings'>`, and `handleSaveSettings()` required the
`X-Garur-Token` header -- which a native HTML form submission cannot send,
with no fallback. Harmless while `API_TOKEN` is still the `CHANGE_ME`
placeholder (`tokenOK()` bypasses the check entirely then), but the moment
you follow `docs/release-checklist.md`'s own first item ("Change Hub API
token") before an event, the Save button starts returning 403 with no
way to fix it from the page itself. Fixed by adding
`settingsFormTokenOK()` -- accepts the existing header check OR a `token`
form field as a fallback -- and rendering that token into a hidden input
on the page. Scoped to this one endpoint deliberately, not folded into
`tokenOK()` itself: every other write endpoint (the JSON API the AI
monitor and any other automation actually uses) keeps the stricter,
header-only requirement completely unchanged. The one honest tradeoff:
since `/settings` (GET, viewing current config) already has zero auth and
never has, embedding the token in that same unauthenticated page means
anyone who can already view it can also read the token out of the page
source -- but that's not a new capability being granted, since anyone who
can load the page already has full read+write access via the legitimate
form regardless.

## Feature: wing motor soft-start/soft-stop ramping

Requested to address real physical inertia: an instant jump to full speed
or an instant cut to zero puts mechanical stress on the wing's gearbox/
linkage that a gradual ramp avoids.

**Design:** a ramp state machine on the Nano (`wingRampCurrentPwm` tracks
what's actually being commanded, `wingRampTargetPwm` what it's ramping
toward, advanced by `processWingRamp()` every ~20ms) replaces every direct
`wingForward()`/`wingStop()` call from scene logic. `activateScene()` and
the duration-expiry failsafe now call `wingRampSetTarget()` instead of
snapping directly -- both starting a spin and stopping one ease through
the configured `wingRampMs` (time to cross the full 0-255 range; a partial
ramp, e.g. between two spin speeds, takes proportionally less time, not
the full duration).

**Two deliberate exceptions, matching what was actually asked rather than
ramping everything uniformly:**
- **Homing** ramps *up* to `wingHomePwm` (gentler start, same as any other
  spin), but stops *instantly* the moment the sensor triggers, via a new
  `wingRampSnapTo()` that bypasses the ramp entirely. This matches the
  request directly: homing's approach speed is already independently
  configurable and typically low, and reacting fast to the sensor matters
  more than gentleness once you're already at the stop point.
- **`SCENE_STOP`** (the hardcoded safety fallback) also snaps instantly,
  never ramped -- consistent with every other place in this project
  SCENE_STOP has been kept deliberately exempt from configuration: it's
  the path used when something's already wrong, and must not be softened
  or delayed by anything.

**New config, one field:** `wingRampMs` (default 600, range 0-5000,
0=disables ramping), added end to end the same way every other Nano
actuator field was: `HubConfig` + named bound constants (`WING_RAMP_MIN`/
`MAX`, matching the Nano's own independent clamp on the same field) +
`validateConfig()` + the `WRM` code in the Hub<->Nano CFG sync protocol +
`GET`/`POST /api/config` + a new row in the existing "Nano Actuators
(global)" `/settings` card, with a hint explaining the homing exception
so it's not a surprise when tuning. Verified the settings page's reserve()
size against actual content length (984 bytes measured vs. 900 originally
reserved -- bumped to 1100) rather than assume the new row fit; small
enough not to warrant the full native-harness measurement used for the
bigger per-scene cards earlier, but not skipped either. Bumped
`CONFIG_MAGIC` again (new struct field).

**Small drive-by fix, found while already editing these exact lines:**
`processHoming()`'s timeout/fault branch never set `doneSent=true` (the
success branch just above it did) -- meaning a homing timeout could leave
`doneSent` false, letting a still-pending duration timer fire a confusing
duplicate `DONE` after the `FAULT` had already been sent. Fixed alongside
the ramp changes to those same lines, called out separately here so it
doesn't get lost as if it were part of what was actually requested.
