/*
  Garur AI Monitor ESP8266 v1.0
  Local Hub polling + OLED + sequential Firebase/Groq cycle.
  Cloud failure never blocks the Hub.
*/
#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>

#define FW_VERSION "GARUR-AI-MONITOR-1.0.0"
static const char* WIFI_SSID="CHANGE_ME";
static const char* WIFI_PASSWORD="CHANGE_ME";
static const IPAddress MONITOR_IP(192,168,0,40), GW(192,168,0,1), MASK(255,255,255,0), DNS(192,168,0,1);
static const char* HUB_BASE="http://192.168.0.10";
static const char* API_TOKEN="CHANGE_ME_LOCAL_TOKEN";
static const char* FIREBASE_BASE="https://YOUR_DB.firebasedatabase.app";
static const char* FIREBASE_AUTH="CHANGE_ME_FIREBASE_TOKEN";
// AI reasoning via Groq's OpenAI-compatible /chat/completions endpoint.
// Groq's own request/response shape is identical to OpenRouter's (both are
// OpenAI-compatible), so only the base URL, auth, model ID, and the
// response_format block below changed versus the OpenRouter version.
static const bool AI_ENABLED=true;
// openai/gpt-oss-20b, per Groq's own published rate-limit table
// (console.groq.com/docs/rate-limits, checked live): 30 requests/min,
// 1,000 requests/day, 8K tokens/min, 200K tokens/day, on the free tier with
// no credit card required. At CLOUD_MS below (~288 calls/day if run
// continuously) that's ~3.5x headroom on requests -- no purchase needed to
// stay clear of the daily *request* cap at this cadence, unlike OpenRouter's
// free tier. Real measured usage from Groq's own dashboard after this went
// live: ~740 input + ~200 output =~ 940 tokens/call, meaningfully more than
// first guessed -- at that rate the 200K TPD cap (not the 1000 RPD cap)
// becomes the binding limit, reached after ~211 calls, i.e. ~17.6 hours of
// *continuous* operation. Fine for a single evening event; a multi-day
// always-on deployment would need either a longer CLOUD_MS or a shorter
// prompt to avoid the last few hours of a long day going quiet. Also Groq's
// own official example model for Structured Outputs (response_format
// below), which is why it's picked over the same-limits
// openai/gpt-oss-120b/qwen/qwen3.6-27b alternatives: smaller and faster is
// a better fit for a small, well-specified JSON decision task, and
// gpt-oss-20b is the model Groq's own docs demonstrate strict-schema support
// with. Note: Groq's free model lineup has visibly changed over the past
// few months (the llama-3.x/mixtral/gemma models many older guides mention
// aren't on the current table at all) -- if gpt-oss-20b is ever retired,
// check console.groq.com/docs/rate-limits and swap this one line; Groq
// doesn't offer an OpenRouter-style auto-router fallback model.
static const char* GROQ_MODEL="openai/gpt-oss-20b";
static const char* GROQ_API_KEY="CHANGE_ME_GROQ_KEY"; // from console.groq.com/keys
static const uint32_t POLL_MS=2000UL, CLOUD_MS=300000UL;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0,U8X8_PIN_NONE);
struct Cache { bool valid; char state[24]; char light[24]; bool entry,exit,nanoOk; uint8_t es,xs; char fault[48]; char lastAction[40]; char lastReason[64]; } cache={};
static uint32_t lastPoll=0,lastCloud=0,cloudOk=0,cloudFail=0; static bool cloudBusy=false;
static uint32_t wifiRetryAt=0;
// Lightweight cross-cycle "memory": chat completion APIs are stateless per
// request, so instead of resending a growing conversation history (costly
// on an ESP8266's limited RAM/flash and on per-call tokens), we carry forward
// a short summary of the previous cycle and let the prompt reference it.
static char prevState[24]="";
static char prevAction[40]="NONE";
static uint16_t sameStateCycles=0;

// This used to parse /api/fullStatus with naive flat-key substring search
// (jsonString/jsonBool/jsonUInt, since removed). That silently failed for
// every field that actually lives inside a nested object in the real
// response -- entryActive/exitActive/nanoOk/entryActivityScore/
// exitActivityScore don't exist anywhere in the Hub's JSON as those exact
// substrings; the real paths are sensors.entry.active, nano.ok, etc. Only
// state/light/fault happened to work, because those particular keys
// occur as apparently-flat-looking substrings regardless of nesting. Net
// effect: cache.entry/exit/es/xs/nanoOk have been stuck at their zero-init
// defaults (false/false/0/0/false) since this file was first written --
// the AI has been deciding based on "nobody's ever at either sensor and
// the Nano's never OK", regardless of the Hub's actual state. This wasn't
// caught by the one live test earlier in this project specifically
// because that test had nothing physically connected, so entry/exit/nano
// genuinely were false/not-ok -- coincidentally matching the bug's
// failure-mode default and masking it completely. Switched to ArduinoJson
// (already a dependency here for the Groq response) so nesting is handled
// correctly instead of assumed away.
static void updateCache(const String& b) {
  JsonDocument doc;
  if (deserializeJson(doc, b)) { Serial.println(F("[AI] fullStatus parse failed")); return; }
  const char* state = doc["show"]["state"] | "";
  if (!state[0]) return; // no usable state in this response -- leave the previous cache intact
  cache.valid = true;
  strncpy(cache.state, state, sizeof(cache.state)-1); cache.state[sizeof(cache.state)-1]='\0';
  const char* light = doc["show"]["light"] | "";
  if (light[0]) { strncpy(cache.light, light, sizeof(cache.light)-1); cache.light[sizeof(cache.light)-1]='\0'; }
  cache.entry = doc["sensors"]["entry"]["active"] | false;
  cache.exit = doc["sensors"]["exit"]["active"] | false;
  cache.nanoOk = doc["nano"]["ok"] | false;
  const char* fault = doc["nano"]["fault"] | "";
  strncpy(cache.fault, fault, sizeof(cache.fault)-1); cache.fault[sizeof(cache.fault)-1]='\0';
  cache.es = (uint8_t)constrain((int)(doc["sensors"]["entry"]["activityScore"] | 0), 0, 100);
  cache.xs = (uint8_t)constrain((int)(doc["sensors"]["exit"]["activityScore"] | 0), 0, 100);
}

static String hubGet(){WiFiClient c;HTTPClient h;String u=String(HUB_BASE)+"/api/fullStatus";if(!h.begin(c,u))return String();h.setTimeout(700);if(strcmp(API_TOKEN,"CHANGE_ME_LOCAL_TOKEN")!=0)h.addHeader("X-Garur-Token",API_TOKEN);int code=h.GET();String r=code>=200&&code<300?h.getString():String();h.end();return r;}
static bool hubPost(const char* path,const String& body){WiFiClient c;HTTPClient h;String u=String(HUB_BASE)+path;if(!h.begin(c,u))return false;h.setTimeout(1200);h.addHeader("Content-Type","application/json");if(strcmp(API_TOKEN,"CHANGE_ME_LOCAL_TOKEN")!=0)h.addHeader("X-Garur-Token",API_TOKEN);int code=h.POST(body);h.end();return code>=200&&code<300;}
static String fbUrl(const char* p){String u=String(FIREBASE_BASE);if(u.endsWith("/"))u.remove(u.length()-1);u+="/";u+=p;u+=".json";if(strcmp(FIREBASE_AUTH,"CHANGE_ME_FIREBASE_TOKEN")!=0){u+="?auth=";u+=FIREBASE_AUTH;}return u;}
static bool fbPut(const char* p,const String& b){if(strstr(FIREBASE_BASE,"YOUR_DB"))return false;WiFiClientSecure c;c.setInsecure();HTTPClient h;if(!h.begin(c,fbUrl(p)))return false;h.setTimeout(10000);h.addHeader("Content-Type","application/json");int code=h.PUT(b);h.end();return code>=200&&code<300;}
static bool fbPost(const char* p,const String& b){if(strstr(FIREBASE_BASE,"YOUR_DB"))return false;WiFiClientSecure c;c.setInsecure();HTTPClient h;if(!h.begin(c,fbUrl(p)))return false;h.setTimeout(10000);h.addHeader("Content-Type","application/json");int code=h.POST(b);h.end();return code>=200&&code<300;}

// Single source of truth for what the model is allowed to touch and the
// valid range for each -- used both to build the prompt's allow-list (so
// the model is told the exact bounds up front) and to validate its response
// afterward (so an out-of-range or hallucinated path is rejected regardless
// of what the prompt said). Keep this in sync with the SET_U16/SET_U8 keys
// the Hub itself accepts on POST /api/config.
struct ConfigRange { const char* path; long lo; long hi; };
static const ConfigRange CONFIG_RANGES[] = {
  {"fightDurationSec",5,120},
  {"celebrationDurationSec",5,120},
  {"noPeopleIdleSec",30,1800},
  {"entryStandingNormalSec",30,1800},
  {"bothFightDebounceSec",1,30},
  {"bothActiveShowcaseSec",30,1800},
  {"fightCooldownSec",15,1800},
  {"normalRecoverySec",5,600},
};
static const uint8_t CONFIG_RANGES_N = sizeof(CONFIG_RANGES)/sizeof(CONFIG_RANGES[0]);
static bool findConfigRange(const char* path, long& lo, long& hi) {
  for (uint8_t i=0;i<CONFIG_RANGES_N;i++) if (!strcmp(CONFIG_RANGES[i].path,path)) { lo=CONFIG_RANGES[i].lo; hi=CONFIG_RANGES[i].hi; return true; }
  return false;
}
static String allowListText() {
  String s; s.reserve(220);
  for (uint8_t i=0;i<CONFIG_RANGES_N;i++) { if (i) s+=", "; s+=CONFIG_RANGES[i].path; s+=" ["; s+=String(CONFIG_RANGES[i].lo); s+="-"; s+=String(CONFIG_RANGES[i].hi); s+="]"; }
  return s;
}

// Static JSON Schema for Groq's Structured Outputs (response_format below).
// With strict:true Groq uses constrained decoding so the response is
// guaranteed to match this schema exactly -- no markdown fences, no stray
// text, no missing fields -- eliminating an entire class of parse failures
// the OpenRouter/Gemini versions had to defend against with prompt-only
// instructions. Verified as valid JSON (python json.loads) before embedding.
// Keep the field names/types in sync with what applyAI() reads below.
static const char* GROQ_RESPONSE_FORMAT =
  "{\"type\":\"json_schema\",\"json_schema\":{\"name\":\"ai_action\",\"strict\":true,"
  "\"schema\":{\"type\":\"object\",\"properties\":{"
  "\"action\":{\"type\":\"string\",\"enum\":[\"NONE\",\"SET_CONFIG\",\"CLEAR_FAULT\"]},"
  "\"path\":{\"type\":\"string\"},"
  "\"value\":{\"type\":\"integer\"},"
  "\"reason\":{\"type\":\"string\"}},"
  "\"required\":[\"action\",\"path\",\"value\",\"reason\"],"
  "\"additionalProperties\":false}}}";

static String callAI(const String& snap, const char* prevSt, const char* prevAct, uint16_t sameCycles) {
  if (!AI_ENABLED || strstr(GROQ_API_KEY,"CHANGE_ME")) return String();
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h;
  if (!h.begin(c,"https://api.groq.com/openai/v1/chat/completions")) return String();
  // Groq's LPU hardware is fast (typically sub-second even for this size of
  // model), but keep the same generous margin the OpenRouter version used
  // rather than assume best-case latency on a free/shared endpoint.
  h.setTimeout(20000);
  h.addHeader("Content-Type","application/json");
  h.addHeader("Authorization", String("Bearer ")+GROQ_API_KEY);

  String prompt; prompt.reserve(1700);
  prompt += "You are a safety-constrained monitoring assistant for a public Kali Puja Garur/Vishnu animatronic light-and-sound installation. ";
  prompt += "The Hub is the sole real-time authority; you only suggest advisory tuning, you never control motors, DMX, or audio directly. ";
  prompt += "Field meanings: state is the Hub's current scene (IDLE, NORMAL, FIGHT, SHOWCASE_*, CELEBRATION, FAULT_SAFE, RECOVERY). light is the current lighting mode. entry/exit report whether each sensor currently sees a visitor. entryScore/exitScore are 0-100 recent activity levels per sensor. nanoOk reports whether the Nano motor/light controller is responding. fault is the last fault reported, empty if none. ";
  prompt += "Context from your previous cycle, about 5 minutes ago: state was ";
  prompt += (prevSt[0] ? prevSt : "unknown, this is the first cycle");
  prompt += "; you took action: "; prompt += prevAct;
  prompt += "; consecutive cycles observed in the current state: "; prompt += String(sameCycles);
  prompt += ". Current snapshot: "; prompt += snap;
  prompt += ". Decide exactly ONE action. Use \"NONE\" -- nothing needs changing -- unless you have one clear, specific reason not to; this is the default and the safe choice. ";
  prompt += "Use \"SET_CONFIG\" to adjust exactly one setting, ONLY while state is IDLE or NORMAL. value MUST be a whole number inside that setting's inclusive range. Allowed settings and their valid ranges, no others are permitted: ";
  prompt += allowListText();
  prompt += ". Use \"CLEAR_FAULT\" if nanoOk is false AND fault is exactly NANO_LINK_TIMEOUT or NANO_ACK_TIMEOUT, since those are transient link hiccups worth nudging, not hardware faults -- the Hub itself will still refuse this if the Nano hasn't shown enough recent responses yet, so err toward proposing it rather than withholding it. ";
  prompt += "Rules: never propose a path that is not in the allowed list above; never propose a value outside that path's listed range, out-of-range values are discarded and wasted; do not repeat the exact SET_CONFIG you made last cycle unless the state has changed since then; prefer NONE unless recent cycles show a real, specific pattern worth adjusting, not routine variation. ";
  prompt += "path and value are only meaningful for SET_CONFIG; use \"\" and 0 otherwise. Keep reason under 15 words and specific to this decision.";

  String escapedPrompt; escapedPrompt.reserve(prompt.length()+40);
  for (char ch : prompt) { if (ch=='"') escapedPrompt+="\\\""; else if (ch=='\\') escapedPrompt+="\\\\"; else if (ch=='\n') escapedPrompt+="\\n"; else escapedPrompt+=ch; }
  String payload = String("{\"model\":\"")+GROQ_MODEL+"\",\"messages\":[{\"role\":\"user\",\"content\":\""+escapedPrompt+"\"}],\"temperature\":0.1,\"response_format\":"+GROQ_RESPONSE_FORMAT+"}";
  int code = h.POST(payload);
  String r;
  if (code==200) {
    r = h.getString();
  } else if (code==429) {
    // Don't retry within this call -- we're already only calling once per
    // CLOUD_MS, so a blind immediate retry would just spend more of a
    // scarce daily quota on the same rate limit. Log clearly so a 429 is
    // distinguishable from "no network"/"AI disabled" in Serial output,
    // and surface Retry-After if Groq sent one.
    String retryAfter = h.header("Retry-After");
    Serial.print(F("[AI] groq 429 rate limited"));
    if (retryAfter.length()) { Serial.print(F(", retry-after=")); Serial.print(retryAfter); Serial.print(F("s")); }
    Serial.println();
  } else if (code>0) {
    Serial.print(F("[AI] groq HTTP "));Serial.println(code);
  } else {
    Serial.print(F("[AI] groq connect/timeout, code="));Serial.println(code);
  }
  h.end();
  return r;
}

static void sanitizeForJson(char* buf) {
  // Model-generated free text is about to get hand-embedded into JSON we
  // build ourselves (Firebase log body) without a general escaper, so strip
  // anything that could break that JSON rather than escape it.
  for (char* p=buf; *p; ++p) if (*p=='"' || *p=='\\' || *p=='\n' || *p=='\r') *p=' ';
}

static void applyAI(const String& response) {
  // Groq is OpenAI-compatible, same envelope shape OpenRouter used:
  // {"choices":[{"message":{"content":"<our JSON, as a string>"}}]} --
  // the actual {"action":...} object is nested inside
  // choices[0].message.content as a string, not a top-level field. Extract
  // that first, then parse it as the action JSON. (With Structured Outputs
  // -- response_format above -- this content is schema-guaranteed valid
  // JSON already; this two-stage parse just gets it out of the envelope.)
  JsonDocument envelope;
  if (deserializeJson(envelope, response)) { Serial.println(F("[AI] groq response: outer JSON parse failed")); return; }
  const char* apiErr = envelope["error"]["message"] | "";
  if (apiErr[0]) { Serial.print(F("[AI] groq error: ")); Serial.println(apiErr); return; }
  const char* text = envelope["choices"][0]["message"]["content"] | "";
  if (!text[0]) { Serial.println(F("[AI] groq response: no content in choice (blocked/empty/rate-limited?)")); return; }
  const char* servedBy = envelope["model"] | "";
  if (servedBy[0]) { Serial.print(F("[AI] served by: ")); Serial.println(servedBy); }
  // Print exactly what the model sent back, unmodified -- this is the
  // direct answer to "is the model actually responding sensibly": you see
  // its raw JSON right here, not just our parsed summary of it. Also the
  // most useful line to have if the parse below ever fails, since it shows
  // exactly what didn't parse and why.
  Serial.print(F("[AI] raw response: ")); Serial.println(text);

  // Structured Outputs (strict:true) should make this unnecessary, but it's
  // a zero-cost safety net in case the model or provider ever changes to
  // one where that guarantee doesn't apply.
  String clean = text; clean.trim();
  if (clean.startsWith("```")) {
    int firstNl = clean.indexOf('\n');
    if (firstNl >= 0) clean = clean.substring(firstNl+1);
    int fence = clean.lastIndexOf("```");
    if (fence >= 0) clean = clean.substring(0, fence);
    clean.trim();
  }

  JsonDocument doc;
  if (deserializeJson(doc, clean)) { Serial.println(F("[AI] groq content was not valid JSON")); return; }
  const char* action = doc["action"] | "NONE";
  const char* path = doc["path"] | "";
  long value = doc["value"] | 0;
  const char* reason = doc["reason"] | "";
  bool safeState = !strcmp(cache.state,"IDLE") || !strcmp(cache.state,"NORMAL");
  // Printed on its own line too (not just embedded in the raw JSON above)
  // since this is the part worth reading every cycle -- why the model
  // decided what it decided, including a plain "NONE" cycle, which the raw
  // JSON alone would bury in punctuation.
  if (reason[0]) { Serial.print(F("[AI] model's reason: ")); Serial.println(reason); }

  strncpy(cache.lastAction, "NONE", sizeof(cache.lastAction)-1); cache.lastAction[sizeof(cache.lastAction)-1]='\0';
  strncpy(cache.lastReason, reason, sizeof(cache.lastReason)-1); cache.lastReason[sizeof(cache.lastReason)-1]='\0';
  sanitizeForJson(cache.lastReason);

  if (!strcmp(action,"SET_CONFIG")) {
    if (!safeState) { Serial.println(F("[AI] SET_CONFIG rejected: hub not IDLE/NORMAL")); return; }
    long lo,hi;
    if (!findConfigRange(path,lo,hi)) { Serial.print(F("[AI] SET_CONFIG rejected: path not on allow-list: ")); Serial.println(path); return; }
    if (value<lo || value>hi) { Serial.print(F("[AI] SET_CONFIG rejected: value out of range for ")); Serial.println(path); return; }
    String body = String("{\"") + path + "\":" + String(value) + "}";
    if (hubPost("/api/config", body)) {
      snprintf(cache.lastAction, sizeof(cache.lastAction), "SET %s=%ld", path, value);
    } else Serial.println(F("[AI] SET_CONFIG hubPost failed"));
  } else if (!strcmp(action,"CLEAR_FAULT")) {
    // The Hub reports nanoOk=true only when nanoFault=false, and clearFault()
    // always blanks the fault string in the same call that clears nanoFault
    // -- so "nanoOk true AND fault still set" (the old check here) can never
    // actually happen on the Hub, meaning this branch could never fire. The
    // Hub's own /api/command handler already has the real guard for this
    // (rejects with 409 unless nanoHealthyResponses >= its recovery
    // threshold, which hubPost() below correctly reads as failure) -- so
    // this only needs to rule out obviously-wrong requests before forwarding,
    // not duplicate the Hub's exact threshold logic.
    if (!(!strcmp(cache.fault,"NANO_LINK_TIMEOUT") || !strcmp(cache.fault,"NANO_ACK_TIMEOUT"))) {
      Serial.println(F("[AI] CLEAR_FAULT rejected: fault isn't a recognized transient type"));
      return;
    }
    if (hubPost("/api/command", "{\"command\":\"CLEAR_FAULT\"}")) {
      strncpy(cache.lastAction, "CLEAR_FAULT", sizeof(cache.lastAction)-1);
      cache.lastAction[sizeof(cache.lastAction)-1]='\0';
    } else Serial.println(F("[AI] CLEAR_FAULT hubPost failed (hub declined -- not yet recoverable, or link truly down)"));
  }
}

static void cloudCycle(){
  if(cloudBusy||!cache.valid||WiFi.status()!=WL_CONNECTED){Serial.println(F("[AI] cloud cycle skipped (busy/no data/no wifi)"));return;}
  cloudBusy=true;
  Serial.println(F("[AI] cloud cycle start"));
  sameStateCycles = !strcmp(cache.state,prevState) ? sameStateCycles+1 : 0;
  String snap=String("{\"state\":\"")+cache.state+"\",\"light\":\""+cache.light+"\",\"entry\":"+(cache.entry?"true":"false")+",\"exit\":"+(cache.exit?"true":"false")+",\"entryScore\":"+cache.es+",\"exitScore\":"+cache.xs+",\"nanoOk\":"+(cache.nanoOk?"true":"false")+",\"fault\":\""+cache.fault+"\"}";
  String r=callAI(snap,prevState,prevAction,sameStateCycles);
  if(r.length()){Serial.println(F("[AI] groq responded"));applyAI(r);Serial.print(F("[AI] action="));Serial.println(cache.lastAction);}
  else Serial.println(F("[AI] groq skipped/no response"));
  String current=snap.substring(0,snap.length()-1)+",\"aiAction\":\""+cache.lastAction+"\",\"aiReason\":\""+cache.lastReason+"\"}";
  bool ok=fbPut("garur/current",current);
  Serial.print(F("[AI] firebase current put "));Serial.println(ok?"OK":"FAILED");
  if(cache.lastAction[0]!='\0' && strcmp(cache.lastAction,"NONE")!=0)fbPost("garur/aiActions",String("{\"action\":\"")+cache.lastAction+"\",\"state\":\""+cache.state+"\",\"reason\":\""+cache.lastReason+"\",\"tsMs\":"+String(millis())+"}");
  fbPost("garur/events",String("{\"type\":\"AI_CYCLE\",\"state\":\"")+cache.state+"\"}");
  if(ok)cloudOk++;else cloudFail++;
  Serial.print(F("[AI] cloud cycle done ok="));Serial.print(cloudOk);Serial.print(F(" fail="));Serial.println(cloudFail);
  strncpy(prevState,cache.state,sizeof(prevState)-1); prevState[sizeof(prevState)-1]='\0';
  strncpy(prevAction,cache.lastAction,sizeof(prevAction)-1); prevAction[sizeof(prevAction)-1]='\0';
  cloudBusy=false;
}

static void draw(){oled.clearBuffer();oled.setFont(u8g2_font_6x10_tf);oled.drawStr(0,10,"GARUR AI MONITOR");oled.drawHLine(0,12,128);if(!cache.valid){oled.drawStr(0,30,"Hub: WAITING");oled.drawStr(0,44,WiFi.status()==WL_CONNECTED?"WiFi: OK":"WiFi: OFF");oled.sendBuffer();return;}char l[32];snprintf(l,sizeof(l),"State: %s",cache.state);oled.drawStr(0,25,l);snprintf(l,sizeof(l),"Entry:%s %u",cache.entry?"YES":"NO",cache.es);oled.drawStr(0,37,l);snprintf(l,sizeof(l),"Exit :%s %u",cache.exit?"YES":"NO",cache.xs);oled.drawStr(0,49,l);snprintf(l,sizeof(l),"Nano:%s AI:%s",cache.nanoOk?"OK":"ERR",cloudBusy?"SYNC":"IDLE");oled.drawStr(0,61,l);oled.sendBuffer();}

void setup(){
  Serial.begin(115200); delay(50);
  Serial.println(F("\n[AI] " FW_VERSION " booting"));
  Wire.begin(4,5); oled.begin();
  WiFi.persistent(false); WiFi.setSleepMode(WIFI_NONE_SLEEP); WiFi.mode(WIFI_STA);
  WiFi.config(MONITOR_IP,GW,MASK,DNS);
  Serial.print(F("[AI] connecting to ")); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  uint32_t s=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-s<12000UL){delay(250);yield();}
  if(WiFi.status()==WL_CONNECTED){Serial.print(F("[AI] WiFi OK ip="));Serial.println(WiFi.localIP());}
  else Serial.println(F("[AI] WiFi FAILED, will keep retrying in loop()"));
  Serial.print(F("[AI] hub="));Serial.println(HUB_BASE);
  Serial.print(F("[AI] groq="));Serial.println((AI_ENABLED&&!strstr(GROQ_API_KEY,"CHANGE_ME"))?"enabled":"disabled/unset");
  Serial.print(F("[AI] firebase="));Serial.println(strstr(FIREBASE_BASE,"YOUR_DB")?"unset":"configured");
  draw();
}
void loop(){
  uint32_t now=millis();
  if(WiFi.status()!=WL_CONNECTED){
    if(now-wifiRetryAt>15000){wifiRetryAt=now;Serial.println(F("[AI] WiFi down, retrying..."));WiFi.begin(WIFI_SSID,WIFI_PASSWORD);}
  }
  if(now-lastPoll>=POLL_MS){
    lastPoll=now;
    String b=hubGet();
    if(b.length()){updateCache(b);Serial.print(F("[AI] poll OK state="));Serial.println(cache.state);}
    else Serial.println(F("[AI] poll FAILED (hub unreachable?)"));
    draw();
  }
  if(now-lastCloud>=CLOUD_MS){lastCloud=now;cloudCycle();draw();}
  delay(5);
}
