# AI protocol

Monitor polls `/api/fullStatus` every 2 seconds.
Every 5 minutes it writes Firebase `garur/current`, calls an LLM via Groq's OpenAI-compatible `/chat/completions` endpoint (currently `openai/gpt-oss-20b`, using Groq's Structured Outputs so the response is schema-guaranteed valid JSON) with a prompt that includes the current snapshot plus a short summary of the previous cycle (state, last action, how many consecutive cycles in that state), applies only allow-listed timing/brightness changes within their documented ranges or a guarded transient fault clear, then logs the cycle to `garur/events` and, if an action was applied, to `garur/aiActions` (including the model's stated reason).
The Hub never waits for cloud work. The model's response is validated against the same allow-list/range table the prompt was built from, regardless of what it actually returned, so an out-of-range or hallucinated setting is discarded rather than applied.
