/*
  Garur Show HUB Brain v2
  Router/LAN architecture.

  Local network:
    Router -> Hub / Sensors / ArtNet node / AI monitor
  Cloud:
    AI monitor only.

  Hub is authoritative for the show. It never waits for AI/cloud work.

  HTTP API:
    GET  /api/status
    GET  /api/fullStatus
    GET  /api/config
    POST /api/sensor
    POST /api/config
    POST /api/command
    POST /api/scene

  Nano UART:
    CMD,<seq>,SCENE,<scene>,<durationMs>,<startInMs>
    CFG,<code>,<value>  -- actuator config (wing/lip/DFPlayer), idempotent,
                           no ack/retry needed; see getNanoCfgField()
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include "GarurHubTypes.h"

#define FW_VERSION "GARUR-HUB-2.0.0"

static const char* WIFI_SSID = "CHANGE_ME";
static const char* WIFI_PASSWORD = "CHANGE_ME";
static const bool WIFI_USE_STATIC_IP = true;

// Reserve this address in your router DHCP table or keep it outside the pool.
static const IPAddress HUB_IP(192,168,0,10);
static const IPAddress HUB_GATEWAY(192,168,0,1);
static const IPAddress HUB_SUBNET(255,255,255,0);
static const IPAddress HUB_DNS(192,168,0,1);

// Local LAN API token. Change before deployment.
static const char* API_TOKEN = "CHANGE_ME_LOCAL_TOKEN";

// Maintenance AP if router is unavailable.
static const char* FALLBACK_AP_SSID = "GarurHub-Setup";
static const char* FALLBACK_AP_PASSWORD = "garur2026";

static const uint8_t NANO_RX_PIN = 12; // D6: Nano TX -> divider -> ESP RX
static const uint8_t NANO_TX_PIN = 14; // D5: ESP TX -> Nano RX
SoftwareSerial nanoSerial(NANO_RX_PIN, NANO_TX_PIN);

ESP8266WebServer server(80);
WiFiUDP udp;

// Bumped again: HubConfig grew a wingRampMs field for wing motor soft-
// start/soft-stop ramping. Same reasoning as the previous bumps.
static const uint32_t CONFIG_MAGIC = 0x47525537UL;
static const int EEPROM_SIZE = 512;

struct HubConfig {
  uint32_t magic;
  uint8_t entryNodeId;
  uint8_t exitNodeId;
  uint16_t sensorActiveHoldSec;
  uint16_t sensorStaleSec;
  uint16_t noPeopleIdleSec;
  uint16_t entryStandingNormalSec;
  uint16_t bothActiveShowcaseSec;
  uint16_t bothFightDebounceSec;
  uint16_t fightDurationSec;
  uint16_t celebrationDurationSec;
  uint16_t normalIntroSec;
  uint16_t normalRecoverySec;
  uint16_t fightCooldownSec;
  uint16_t showcaseRepeatCooldownSec;
  uint16_t nanoAckTimeoutMs;
  uint16_t nanoLinkTimeoutSec;
  uint8_t nanoAutoRecover;
  uint8_t nanoRecoverGoodResponses;
  uint16_t startInMs;
  uint8_t artnetIp1, artnetIp2, artnetIp3, artnetIp4;
  uint16_t artnetUniverse;
  uint16_t artnetSendIntervalMs;
  // Indexed by LightScene (0=IDLE,1=NORMAL,2=FIGHT,3=CELEBRATION); no entry
  // for LIGHT_BLACKOUT since it's always all-zeros by definition.
  SceneLight sceneLight[4];
  // Same indexing (0=IDLE,1=NORMAL,2=FIGHT,3=CELEBRATION); Nano's
  // SCENE_STOP has no entry either -- always hardcoded on that side too.
  NanoSceneCfg nanoScene[4];
  uint8_t wingHomePwm;
  uint8_t lipClosedAngle, lipOpenMin, lipOpenMax;
  uint16_t wingRampMs;
};
HubConfig cfg;

TrackerRuntime entryTracker = {};
TrackerRuntime exitTracker = {};

static uint32_t rxHttpCount = 0;
static uint32_t rxHttpBadCount = 0;
static uint32_t stateTransitions = 0;

static ShowState currentState = SHOW_IDLE;
static LightScene currentLightScene = LIGHT_IDLE;

static uint32_t stateStartedAt = 0;
static uint32_t stateRunStartedAt = 0;
static uint32_t stateDurationMs = 0;
static uint32_t noPeopleSince = 0;
static uint32_t bothActiveSince = 0;
static uint32_t entryOnlySince = 0;
static uint32_t lastFightEndedAt = 0;
static uint32_t lastShowcaseEndedAt = 0;

static bool nanoFault = false;
static char lastFault[48] = "";
static uint8_t nanoHealthyResponses = 0;

static uint32_t seqCounter = 1;
static uint32_t currentSeq = 0;
static bool waitingAck = false;
static uint8_t cmdRetries = 0;
static uint32_t lastCmdSentAt = 0;
static uint32_t lastNanoRxAt = 0;
static uint32_t lastNanoPingAt = 0;
static uint32_t lastAckSeq = 0;
static uint32_t lastDoneSeq = 0;
static char lastNanoScene[18] = "IDLE";
static uint32_t lastNanoDuration = 0;
static uint32_t lastNanoStartIn = 0;

// Nano config sync: unlike scene commands, a CFG line is idempotent (
// re-sending the same field twice is harmless), so rather than track which
// specific fields changed, a full resync just walks every field in order
// on a modest interval. Simpler than per-field dirty-tracking, and the
// full walk only takes ~20 fields x 150ms =~ 3s, worth it for not
// overwhelming the Nano's soft-serial link with a burst of lines it also
// has to service DFPlayer/servo work around.
static bool nanoCfgSyncActive = false;
static uint8_t nanoCfgSyncIndex = 0;
static uint32_t lastNanoCfgSentAt = 0;
static const uint8_t NANO_CFG_FIELD_COUNT = 21; // 4 scenes x 4 fields + 5 globals

#define DMX_CHANNELS 512
#define ARTNET_PORT 6454
static uint8_t dmx[DMX_CHANNELS];
static uint8_t artnetPacket[18 + DMX_CHANNELS];
static uint8_t artSeq = 1;
static uint32_t lastArtNetSendAt = 0;
static uint32_t lightSceneStartAt = 0;
static uint32_t pendingLightStartAt = 0;
static uint32_t pendingLightDuration = 0;
static LightScene pendingLightScene = LIGHT_IDLE;
static bool lightPending = false;

// ---------- utility ----------
static uint32_t secMs(uint16_t s) { return (uint32_t)s * 1000UL; }

static void ipText(const IPAddress& ip, char* out, size_t n) {
  if (!out || n == 0) return;
  snprintf(out, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static bool tokenOK() {
  if (API_TOKEN[0] == '\0' || strcmp(API_TOKEN, "CHANGE_ME_LOCAL_TOKEN") == 0) return true;
  return server.hasHeader("X-Garur-Token") && server.header("X-Garur-Token") == API_TOKEN;
}
// A native HTML <form> submission (like /settings's Save button) can't
// send a custom header, so tokenOK() alone always fails there once a real
// token is configured -- exactly what the release checklist's own
// "change the Hub API token" step would silently break. Scoped to this one
// endpoint rather than added to tokenOK() itself, so every other endpoint
// (the JSON API used by the AI monitor and any other automation) keeps its
// stricter header-only requirement unchanged.
static bool settingsFormTokenOK() {
  if (tokenOK()) return true;
  return server.hasArg("token") && server.arg("token") == API_TOKEN;
}

static bool requestJSONContains(const String& body, const char* key) {
  String needle = String("\"") + key + "\"";
  return body.indexOf(needle) >= 0;
}

static bool jsonInt(const String& body, const char* key, long& out) {
  String needle = String("\"") + key + "\"";
  int p = body.indexOf(needle);
  if (p < 0) return false;
  p = body.indexOf(':', p + needle.length());
  if (p < 0) return false;
  p++;
  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
  int e = p;
  if (e < (int)body.length() && (body[e] == '-')) e++;
  while (e < (int)body.length() && isDigit(body[e])) e++;
  if (e == p) return false;
  out = body.substring(p, e).toInt();
  return true;
}

static bool jsonBool(const String& body, const char* key, bool& out) {
  String needle = String("\"") + key + "\"";
  int p = body.indexOf(needle);
  if (p < 0) return false;
  p = body.indexOf(':', p + needle.length());
  if (p < 0) return false;
  String v = body.substring(p + 1);
  v.trim();
  if (v.startsWith("true")) { out = true; return true; }
  if (v.startsWith("false")) { out = false; return true; }
  long n;
  if (jsonInt(body, key, n)) { out = (n != 0); return true; }
  return false;
}

static bool jsonString(const String& body, const char* key, String& out) {
  String needle = String("\"") + key + "\"";
  int p = body.indexOf(needle);
  if (p < 0) return false;
  p = body.indexOf(':', p + needle.length());
  if (p < 0) return false;
  p = body.indexOf('"', p + 1);
  if (p < 0) return false;
  int e = body.indexOf('"', p + 1);
  if (e < 0) return false;
  out = body.substring(p + 1, e);
  return true;
}

static void sendJson(const String& json, int status = 200) {
  server.send(status, "application/json", json);
}

// Single source of truth for every numeric config field's valid range.
// Referenced from three places: inline in handleConfigPost() (the JSON
// API) and handleSaveSettings() (the browser form) at the exact moment
// each value is parsed, AND in validateConfig() below as a full-struct
// safety net run before every persist. One set of numbers, three call
// sites, rather than bounds duplicated (and free to silently drift apart)
// across them -- which is exactly what had happened before this pass: the
// newer scene-lighting/Nano-actuator fields already validated inline when
// they were added, but the original timing/threshold fields never did,
// relying solely on validateConfig() running later. Both now validate the
// same way, from the same numbers.
static const uint16_t SENSOR_ACTIVE_HOLD_MIN=2, SENSOR_ACTIVE_HOLD_MAX=60;
static const uint16_t SENSOR_STALE_MIN=5, SENSOR_STALE_MAX=120;
static const uint16_t NO_PEOPLE_IDLE_MIN=10, NO_PEOPLE_IDLE_MAX=1800;
static const uint16_t ENTRY_STANDING_NORMAL_MIN=10, ENTRY_STANDING_NORMAL_MAX=1800;
static const uint16_t BOTH_ACTIVE_SHOWCASE_MIN=10, BOTH_ACTIVE_SHOWCASE_MAX=1800;
static const uint16_t BOTH_FIGHT_DEBOUNCE_MIN=1, BOTH_FIGHT_DEBOUNCE_MAX=60;
static const uint16_t FIGHT_DURATION_MIN=5, FIGHT_DURATION_MAX=300;
static const uint16_t CELEBRATION_DURATION_MIN=5, CELEBRATION_DURATION_MAX=300;
static const uint16_t NORMAL_INTRO_MIN=5, NORMAL_INTRO_MAX=300;
static const uint16_t NORMAL_RECOVERY_MIN=5, NORMAL_RECOVERY_MAX=600;
static const uint16_t FIGHT_COOLDOWN_MIN=10, FIGHT_COOLDOWN_MAX=1800;
static const uint16_t SHOWCASE_REPEAT_COOLDOWN_MIN=10, SHOWCASE_REPEAT_COOLDOWN_MAX=1800;
static const uint16_t NANO_ACK_TIMEOUT_MIN=300, NANO_ACK_TIMEOUT_MAX=5000;
static const uint16_t NANO_LINK_TIMEOUT_MIN=5, NANO_LINK_TIMEOUT_MAX=120;
static const uint8_t NANO_RECOVER_GOOD_RESPONSES_MIN=1, NANO_RECOVER_GOOD_RESPONSES_MAX=10;
static const uint16_t START_IN_MS_MIN=0, START_IN_MS_MAX=5000;
static const uint16_t ARTNET_UNIVERSE_MIN=0, ARTNET_UNIVERSE_MAX=32767;
static const uint16_t ARTNET_SEND_INTERVAL_MIN=20, ARTNET_SEND_INTERVAL_MAX=100;
// Matches the Nano's own clamp on WRM (wingRampMs) exactly -- both sides
// independently bound this the same way, same reasoning as every other
// field in this validation chain.
static const uint16_t WING_RAMP_MIN=0, WING_RAMP_MAX=5000;

static void validateConfig() {
  cfg.magic = CONFIG_MAGIC;
  if (cfg.entryNodeId < 1 || cfg.entryNodeId > 20) cfg.entryNodeId = 1;
  if (cfg.exitNodeId < 1 || cfg.exitNodeId > 20 || cfg.exitNodeId == cfg.entryNodeId) cfg.exitNodeId = (cfg.entryNodeId == 1) ? 2 : 1;

  cfg.sensorActiveHoldSec = constrain(cfg.sensorActiveHoldSec, SENSOR_ACTIVE_HOLD_MIN, SENSOR_ACTIVE_HOLD_MAX);
  cfg.sensorStaleSec = constrain(cfg.sensorStaleSec, SENSOR_STALE_MIN, SENSOR_STALE_MAX);
  cfg.noPeopleIdleSec = constrain(cfg.noPeopleIdleSec, NO_PEOPLE_IDLE_MIN, NO_PEOPLE_IDLE_MAX);
  cfg.entryStandingNormalSec = constrain(cfg.entryStandingNormalSec, ENTRY_STANDING_NORMAL_MIN, ENTRY_STANDING_NORMAL_MAX);
  cfg.bothActiveShowcaseSec = constrain(cfg.bothActiveShowcaseSec, BOTH_ACTIVE_SHOWCASE_MIN, BOTH_ACTIVE_SHOWCASE_MAX);
  cfg.bothFightDebounceSec = constrain(cfg.bothFightDebounceSec, BOTH_FIGHT_DEBOUNCE_MIN, BOTH_FIGHT_DEBOUNCE_MAX);
  cfg.fightDurationSec = constrain(cfg.fightDurationSec, FIGHT_DURATION_MIN, FIGHT_DURATION_MAX);
  cfg.celebrationDurationSec = constrain(cfg.celebrationDurationSec, CELEBRATION_DURATION_MIN, CELEBRATION_DURATION_MAX);
  cfg.normalIntroSec = constrain(cfg.normalIntroSec, NORMAL_INTRO_MIN, NORMAL_INTRO_MAX);
  cfg.normalRecoverySec = constrain(cfg.normalRecoverySec, NORMAL_RECOVERY_MIN, NORMAL_RECOVERY_MAX);
  cfg.fightCooldownSec = constrain(cfg.fightCooldownSec, FIGHT_COOLDOWN_MIN, FIGHT_COOLDOWN_MAX);
  cfg.showcaseRepeatCooldownSec = constrain(cfg.showcaseRepeatCooldownSec, SHOWCASE_REPEAT_COOLDOWN_MIN, SHOWCASE_REPEAT_COOLDOWN_MAX);
  cfg.nanoAckTimeoutMs = constrain(cfg.nanoAckTimeoutMs, NANO_ACK_TIMEOUT_MIN, NANO_ACK_TIMEOUT_MAX);
  cfg.nanoLinkTimeoutSec = constrain(cfg.nanoLinkTimeoutSec, NANO_LINK_TIMEOUT_MIN, NANO_LINK_TIMEOUT_MAX);
  cfg.nanoRecoverGoodResponses = constrain(cfg.nanoRecoverGoodResponses, NANO_RECOVER_GOOD_RESPONSES_MIN, NANO_RECOVER_GOOD_RESPONSES_MAX);
  cfg.startInMs = constrain(cfg.startInMs, START_IN_MS_MIN, START_IN_MS_MAX);
  cfg.artnetUniverse = constrain(cfg.artnetUniverse, ARTNET_UNIVERSE_MIN, ARTNET_UNIVERSE_MAX);
  cfg.artnetSendIntervalMs = constrain(cfg.artnetSendIntervalMs, ARTNET_SEND_INTERVAL_MIN, ARTNET_SEND_INTERVAL_MAX);
  cfg.wingRampMs = constrain(cfg.wingRampMs, WING_RAMP_MIN, WING_RAMP_MAX);
  cfg.nanoAutoRecover = cfg.nanoAutoRecover ? 1 : 0;
  // Clamp effect/relay-function to valid enum values and mask the fixture
  // group bitmask down to its 4 meaningful bits -- an out-of-range value
  // wouldn't crash applyLight() (its switches all have safe defaults), but
  // this keeps GET /api/config honest about what's actually stored.
  for (uint8_t i=0;i<4;i++) {
    cfg.sceneLight[i].effect = constrain(cfg.sceneLight[i].effect, (uint8_t)EFFECT_SOLID, (uint8_t)EFFECT_CHASE);
    cfg.sceneLight[i].fixtureGroupMask &= 0x0F;
    cfg.sceneLight[i].relay1Func = constrain(cfg.sceneLight[i].relay1Func, (uint8_t)RELAY_OFF, (uint8_t)RELAY_STROBE);
    cfg.sceneLight[i].relay2Func = constrain(cfg.sceneLight[i].relay2Func, (uint8_t)RELAY_OFF, (uint8_t)RELAY_STROBE);
    cfg.nanoScene[i].wingMode = constrain(cfg.nanoScene[i].wingMode, (uint8_t)WING_MODE_STOP, (uint8_t)WING_MODE_HOME);
    cfg.nanoScene[i].lipMode = constrain(cfg.nanoScene[i].lipMode, (uint8_t)LIP_MODE_CLOSED, (uint8_t)LIP_MODE_ANIMATE);
    // dfFileCount and wingSpeed are already uint8_t (0-255), no clamp needed.
  }
  // Servo-realistic angle range -- also keeps the Nano's own random(lo,hi)
  // call safe from a lo>hi wraparound on an already-defensive fallback.
  cfg.lipClosedAngle = constrain(cfg.lipClosedAngle, (uint8_t)0, (uint8_t)180);
  cfg.lipOpenMin = constrain(cfg.lipOpenMin, (uint8_t)0, (uint8_t)180);
  cfg.lipOpenMax = constrain(cfg.lipOpenMax, (uint8_t)0, (uint8_t)180);
}

static void defaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CONFIG_MAGIC;
  cfg.entryNodeId = 1; cfg.exitNodeId = 2;
  cfg.sensorActiveHoldSec = 8; cfg.sensorStaleSec = 15;
  cfg.noPeopleIdleSec = 180; cfg.entryStandingNormalSec = 180;
  cfg.bothActiveShowcaseSec = 180; cfg.bothFightDebounceSec = 3;
  cfg.fightDurationSec = 40; cfg.celebrationDurationSec = 20;
  cfg.normalIntroSec = 20; cfg.normalRecoverySec = 60;
  cfg.fightCooldownSec = 90; cfg.showcaseRepeatCooldownSec = 90;
  cfg.nanoAckTimeoutMs = 1000; cfg.nanoLinkTimeoutSec = 15;
  cfg.nanoAutoRecover = 1; cfg.nanoRecoverGoodResponses = 3;
  cfg.startInMs = 300;
  cfg.artnetIp1 = 192; cfg.artnetIp2 = 168; cfg.artnetIp3 = 0; cfg.artnetIp4 = 30;
  cfg.artnetUniverse = 0; cfg.artnetSendIntervalMs = 25;
  // Defaults chosen to match the previous hardcoded look as closely as the
  // new generic effect/fixture-group/relay-function model allows. IDLE and
  // NORMAL (a static two-group split -- fixtures 1&3 vs 2&4, matching the
  // old hardcoded assignment exactly) reproduce the old look exactly.
  // CELEBRATION's color is unchanged (steady); its relay used to blink for
  // the first 6s then hold on -- that specific timed handoff isn't
  // expressible in the new OFF/ON/BLINK/STROBE-for-the-whole-scene model,
  // so it defaults to steady ON (the majority of the old behavior); pick
  // BLINK instead via /settings if you'd rather it blink the whole scene.
  // FIGHT's old look was a bespoke 4-phase sequence unique to that scene;
  // EFFECT_STROBE + RELAY_STROBE below is a genericized approximation
  // (fast white/red alternation, relay pulsing along with it) in the same
  // spirit, not a pixel-identical replay -- the tradeoff for one reusable
  // effect/relay system instead of bespoke code per scene. All of this is
  // fully editable via /settings afterward, including these choices.
  cfg.sceneLight[LIGHT_IDLE]        = {0,0,180,     0,0,0,     255, EFFECT_SOLID,     0b0000, RELAY_ON,  RELAY_OFF};
  cfg.sceneLight[LIGHT_NORMAL]      = {0,0,160,     255,120,0, 255, EFFECT_ALTERNATE, 0b1010, RELAY_ON,  RELAY_OFF};
  cfg.sceneLight[LIGHT_FIGHT]       = {255,255,255, 255,0,0,   255, EFFECT_STROBE,    0b0000, RELAY_OFF, RELAY_STROBE};
  cfg.sceneLight[LIGHT_CELEBRATION] = {120,70,0,    0,0,0,     255, EFFECT_SOLID,     0b0000, RELAY_ON,  RELAY_OFF};
  // Nano actuator defaults -- exactly reproduce what was hardcoded on the
  // Nano before this feature existed (wing speeds, lip angles, one fixed
  // DFPlayer track per scene), same backward-compatibility approach as the
  // lighting defaults above. Folder number = scene index + 1 on the Nano
  // side; dfFileCount=1 matches today's single-fixed-track-per-scene setup
  // until you add more clips per folder and raise the count.
  cfg.nanoScene[LIGHT_IDLE]        = {WING_MODE_HOME, 0,   LIP_MODE_CLOSED,  1};
  cfg.nanoScene[LIGHT_NORMAL]      = {WING_MODE_SPIN, 90,  LIP_MODE_CLOSED,  1};
  cfg.nanoScene[LIGHT_FIGHT]       = {WING_MODE_SPIN, 210, LIP_MODE_ANIMATE, 1};
  cfg.nanoScene[LIGHT_CELEBRATION] = {WING_MODE_STOP, 0,   LIP_MODE_CLOSED,  1};
  cfg.wingHomePwm = 80;
  cfg.lipClosedAngle = 35; cfg.lipOpenMin = 55; cfg.lipOpenMax = 105;
  // Matches the Nano's own default exactly, for the same reason
  // wingHomePwm/wingSpeed's defaults do -- a starting point to tune on
  // the real hardware, not a derived value.
  cfg.wingRampMs = 600;
  validateConfig();
}

static void saveConfig() {
  validateConfig();
  EEPROM.put(0, cfg);
  EEPROM.commit();
  // Simplest correct option: resync all Nano fields on every settings save,
  // not just when a Nano-specific one changed. Wasteful of a few idle
  // seconds' worth of low-rate serial traffic on saves that didn't touch
  // Nano fields at all, but avoids needing per-field change detection for
  // something that's cheap and harmless to just always do.
  queueNanoCfgSync();
}

static void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);
  if (cfg.magic != CONFIG_MAGIC) {
    defaults();
    saveConfig();
  } else {
    validateConfig();
  }
}

// ---------- network ----------
static void startFallbackAP() {
  // WIFI_AP_STA (not pure WIFI_AP): the ESP8266 keeps attempting the
  // router in the background while still serving the setup AP, instead of
  // committing to AP-only. Previously this was pure WIFI_AP, and
  // maintainWiFi() bailed out immediately whenever mode==WIFI_AP -- so a
  // Hub that ever fell back here (e.g. the router took longer to boot
  // than the Hub after a shared power outage) had no way back to the real
  // network short of a manual power-cycle.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASSWORD, 6, false, 4);
  if (WIFI_USE_STATIC_IP) WiFi.config(HUB_IP, HUB_GATEWAY, HUB_SUBNET, HUB_DNS);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void setupWiFi() {
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA);
  if (WIFI_USE_STATIC_IP) WiFi.config(HUB_IP, HUB_GATEWAY, HUB_SUBNET, HUB_DNS);
  WiFi.hostname("GarurHub");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(250);
    yield();
  }
  if (WiFi.status() != WL_CONNECTED) startFallbackAP();
  Serial.print(F("[WIFI] mode="));
  Serial.println(WiFi.status() == WL_CONNECTED ? F("STA") : F("FALLBACK_AP"));
  Serial.print(F("[WIFI] IP="));
  Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP());
}

static bool maintainWiFi() {
  static uint32_t lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) {
    // Reachable, but still in the AP_STA fallback mode from earlier --
    // the router just came back. Drop the setup AP and return to normal
    // STA-only operation rather than running both indefinitely.
    if (WiFi.getMode() != WIFI_STA) {
      Serial.println(F("[WIFI] router reachable again, leaving fallback AP"));
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
    }
    return true;
  }
  if (millis() - lastTry < 15000UL) return false;
  lastTry = millis();
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  return false;
}

// ---------- ArtNet ----------
static IPAddress artIp() { return IPAddress(cfg.artnetIp1,cfg.artnetIp2,cfg.artnetIp3,cfg.artnetIp4); }

static void blackoutDMX() { memset(dmx, 0, sizeof(dmx)); }
static void setPAR(uint8_t n, uint8_t r, uint8_t g, uint8_t b) {
  if (n < 1 || n > 4) return;
  uint16_t b0 = (n - 1) * 3;
  dmx[b0] = r; dmx[b0+1] = g; dmx[b0+2] = b;
}
static void setAllPAR(uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t n=1;n<=4;n++) setPAR(n,r,g,b);
}
static void setRelay(uint8_t n, bool on) {
  if (n < 1 || n > 2) return;
  dmx[12 + n - 1] = on ? 255 : 0;
}
static void buildArtDmx() {
  memcpy(artnetPacket, "Art-Net\0", 8);
  artnetPacket[8]=0x00; artnetPacket[9]=0x50;
  artnetPacket[10]=0x00; artnetPacket[11]=0x0E;
  artnetPacket[12]=artSeq++; if (artSeq == 0) artSeq = 1;
  artnetPacket[13]=0x00;
  artnetPacket[14]=cfg.artnetUniverse & 0xFF;
  artnetPacket[15]=(cfg.artnetUniverse >> 8) & 0x7F;
  artnetPacket[16]=0x02; artnetPacket[17]=0x00;
  memcpy(artnetPacket+18, dmx, DMX_CHANNELS);
}
static void sendArtNet() {
  uint32_t now = millis();
  if (now - lastArtNetSendAt < cfg.artnetSendIntervalMs) return;
  lastArtNetSendAt = now;
  buildArtDmx();
  udp.beginPacket(artIp(), ARTNET_PORT);
  udp.write(artnetPacket, sizeof(artnetPacket));
  udp.endPacket();
}

static const char* lightName(LightScene s) {
  switch(s) {
    case LIGHT_IDLE:return "IDLE"; case LIGHT_NORMAL:return "NORMAL";
    case LIGHT_FIGHT:return "FIGHT"; case LIGHT_CELEBRATION:return "CELEBRATION";
    case LIGHT_BLACKOUT:return "BLACKOUT"; default:return "UNKNOWN";
  }
}
static void applyRelayFunction(uint8_t relayNum, uint8_t func, uint32_t t) {
  bool on;
  switch (func) {
    case RELAY_ON: on = true; break;
    case RELAY_BLINK: on = (t/400UL)%2==0; break;
    case RELAY_STROBE: on = (t/120UL)%2==0; break;
    case RELAY_OFF: default: on = false; break;
  }
  setRelay(relayNum, on);
}
static void applyLight(LightScene s) {
  uint32_t t = millis() - lightSceneStartAt;
  if (s == LIGHT_BLACKOUT) { blackoutDMX(); return; }

  const SceneLight& sl = cfg.sceneLight[s]; // s is 0..3 for IDLE/NORMAL/FIGHT/CELEBRATION
  blackoutDMX();

  // Scale brightness by the configured intensity without a hardware dimmer
  // channel -- these are plain 3-channel RGB fixtures.
  uint8_t pr=(uint16_t)sl.r *sl.intensity/255, pg=(uint16_t)sl.g *sl.intensity/255, pb=(uint16_t)sl.b *sl.intensity/255;
  uint8_t sr=(uint16_t)sl.r2*sl.intensity/255, sg=(uint16_t)sl.g2*sl.intensity/255, sb=(uint16_t)sl.b2*sl.intensity/255;

  switch (sl.effect) {
    case EFFECT_BLINK:
      if ((t/400UL)%2==0) setAllPAR(pr,pg,pb); // else left black by blackoutDMX() above
      break;
    case EFFECT_STROBE:
      if ((t/120UL)%2==0) setAllPAR(pr,pg,pb); else setAllPAR(sr,sg,sb);
      break;
    case EFFECT_ALTERNATE:
      // Static split -- whichever fixtures are assigned group A get primary,
      // group B gets secondary, no time variation (matches the old hardcoded
      // 1&3-vs-2&4 NORMAL look when the mask is set that way, but any
      // grouping is now valid).
      for (uint8_t n=1;n<=4;n++) {
        bool isA = ((sl.fixtureGroupMask>>(n-1))&1)==FIXTURE_GROUP_A;
        setPAR(n, isA?pr:sr, isA?pg:sg, isA?pb:sb);
      }
      break;
    case EFFECT_CHASE: {
      // Cycle through whichever fixtures are in group A, one lit at a time;
      // everything else (group B, and group-A fixtures not currently lit)
      // shows secondary. If no fixture is assigned to group A, this safely
      // falls back to all-secondary rather than risking a divide-by-zero.
      uint8_t groupAFixtures[4], groupACount=0;
      for (uint8_t n=1;n<=4;n++) if (((sl.fixtureGroupMask>>(n-1))&1)==FIXTURE_GROUP_A) groupAFixtures[groupACount++]=n;
      uint8_t litFixture = groupACount ? groupAFixtures[(t/200UL)%groupACount] : 0;
      for (uint8_t n=1;n<=4;n++) { bool isLit=(n==litFixture); setPAR(n, isLit?pr:sr, isLit?pg:sg, isLit?pb:sb); }
      break;
    }
    case EFFECT_SOLID:
    default:
      setAllPAR(pr,pg,pb);
      break;
  }

  applyRelayFunction(1, sl.relay1Func, t);
  applyRelayFunction(2, sl.relay2Func, t);
}
static void scheduleLight(LightScene s, uint32_t duration, uint32_t startIn) {
  pendingLightScene=s; pendingLightDuration=duration; pendingLightStartAt=millis()+startIn; lightPending=true;
}
static void processLight() {
  if (lightPending && millis() >= pendingLightStartAt) {
    lightPending=false; currentLightScene=pendingLightScene; lightSceneStartAt=millis();
  }
  applyLight(currentLightScene);
}

// ---------- Nano ----------
static void setFault(const char* reason) {
  nanoFault=true; nanoHealthyResponses=0;
  strncpy(lastFault, reason?reason:"FAULT", sizeof(lastFault)-1);
  lastFault[sizeof(lastFault)-1]='\0';
}
static void clearFault() { nanoFault=false; nanoHealthyResponses=0; lastFault[0]='\0'; }

static void markNanoHealthy() {
  lastNanoRxAt = millis();
  if (nanoFault && nanoHealthyResponses < 250) nanoHealthyResponses++;
}

static const char* nanoScene(ShowState s) {
  switch(s) {
    case SHOW_IDLE:return "IDLE"; case SHOW_NORMAL:return "NORMAL"; case SHOW_FIGHT:return "FIGHT";
    case SHOW_CELEBRATION:return "CELEBRATION"; case SHOW_SHOWCASE_NORMAL_INTRO:return "NORMAL";
    case SHOW_SHOWCASE_FIGHT:return "FIGHT"; case SHOW_SHOWCASE_CELEBRATION:return "CELEBRATION";
    case SHOW_SHOWCASE_RECOVERY:return "NORMAL"; case SHOW_FAULT_SAFE:return "STOP"; default:return "IDLE";
  }
}
static void sendNano(uint32_t seq, const char* scene, uint32_t duration, uint32_t startIn) {
  nanoSerial.print(F("CMD,")); nanoSerial.print(seq); nanoSerial.print(F(",SCENE,"));
  nanoSerial.print(scene); nanoSerial.print(','); nanoSerial.print(duration); nanoSerial.print(',');
  nanoSerial.println(startIn);
  strncpy(lastNanoScene, scene, sizeof(lastNanoScene)-1); lastNanoScene[sizeof(lastNanoScene)-1]='\0';
  lastNanoDuration=duration; lastNanoStartIn=startIn;
  waitingAck=true; cmdRetries=0; lastCmdSentAt=millis();
}
static void resendNano() {
  sendNano(currentSeq,lastNanoScene,lastNanoDuration,lastNanoStartIn);
  cmdRetries++;
}
static void pingNano() {
  uint32_t now=millis(); if(now-lastNanoPingAt<2000) return;
  lastNanoPingAt=now; nanoSerial.print(F("PING,")); nanoSerial.println(now);
}
// Indexed 0..19: scene fields first (4 scenes x 4 fields, code = scene
// letter + field letters), then the 4 global fields. Must match the Nano's
// applyNanoCfg() code table exactly -- see that function's comment there.
static bool getNanoCfgField(uint8_t idx, char* codeOut, uint16_t& valueOut) {
  static const char sceneLetter[4] = {'I','N','F','C'};
  if (idx < 16) {
    uint8_t scene = idx / 4, field = idx % 4;
    NanoSceneCfg& sc = cfg.nanoScene[scene];
    switch (field) {
      case 0: snprintf(codeOut,5,"%cWM",sceneLetter[scene]); valueOut=sc.wingMode; break;
      case 1: snprintf(codeOut,5,"%cWS",sceneLetter[scene]); valueOut=sc.wingSpeed; break;
      case 2: snprintf(codeOut,5,"%cLM",sceneLetter[scene]); valueOut=sc.lipMode; break;
      case 3: snprintf(codeOut,5,"%cDF",sceneLetter[scene]); valueOut=sc.dfFileCount; break;
    }
    return true;
  } else if (idx < NANO_CFG_FIELD_COUNT) {
    switch (idx-16) {
      case 0: strcpy(codeOut,"WHP"); valueOut=cfg.wingHomePwm; break;
      case 1: strcpy(codeOut,"LCA"); valueOut=cfg.lipClosedAngle; break;
      case 2: strcpy(codeOut,"LMN"); valueOut=cfg.lipOpenMin; break;
      case 3: strcpy(codeOut,"LMX"); valueOut=cfg.lipOpenMax; break;
      case 4: strcpy(codeOut,"WRM"); valueOut=cfg.wingRampMs; break;
    }
    return true;
  }
  return false;
}
static void queueNanoCfgSync() { nanoCfgSyncIndex=0; nanoCfgSyncActive=true; }
static void processNanoLine(char* line) {
  char* type=strtok(line,","); if(!type)return;
  if(!strcmp(type,"ACK")) {
    char* s=strtok(NULL,","); if(!s)return; uint32_t seq=strtoul(s,NULL,10);
    lastAckSeq=seq; if(seq==currentSeq) waitingAck=false; markNanoHealthy();
  } else if(!strcmp(type,"DONE")) {
    char* s=strtok(NULL,","); if(!s)return; lastDoneSeq=strtoul(s,NULL,10); markNanoHealthy();
  } else if(!strcmp(type,"FAULT")) {
    strtok(NULL,","); char* r=strtok(NULL,","); setFault(r?r:"NANO_FAULT"); lastNanoRxAt=millis();
  } else if(!strcmp(type,"PONG")) markNanoHealthy();
  else if(!strcmp(type,"CFGACK")) {
    // Diagnostic only, Serial.print goes to the Hub's own USB debug output
    // (separate from nanoSerial, the link this line itself arrived on) --
    // nothing here affects Hub state, since CFG sync doesn't need per-field
    // ack/retry the way scene commands do.
    char* code=strtok(NULL,","); char* val=strtok(NULL,",");
    Serial.print(F("[NANO] cfg acked: ")); Serial.print(code?code:"?"); Serial.print(F("=")); Serial.println(val?val:"?");
  }
}
static void readNano() {
  static char line[96]; static uint8_t pos=0;
  while(nanoSerial.available()) {
    char c=nanoSerial.read();
    if(c=='\n') { line[pos]='\0'; if(pos) processNanoLine(line); pos=0; }
    else if(c!='\r') { if(pos<sizeof(line)-1) line[pos++]=c; else pos=0; }
  }
}
static void maintainNano() {
  pingNano();
  uint32_t now=millis();
  if(waitingAck && now-lastCmdSentAt>cfg.nanoAckTimeoutMs) {
    if(cmdRetries<3) resendNano(); else {waitingAck=false; setFault("NANO_ACK_TIMEOUT");}
  }
  if(lastNanoRxAt>0 && now-lastNanoRxAt>secMs(cfg.nanoLinkTimeoutSec) && !nanoFault) setFault("NANO_LINK_TIMEOUT");
  if(nanoFault && cfg.nanoAutoRecover && nanoHealthyResponses>=cfg.nanoRecoverGoodResponses) {
    clearFault();
    if(currentState==SHOW_FAULT_SAFE) {
      // Safe recovery: stop then let normal presence logic restart the show.
      currentState=SHOW_IDLE; stateDurationMs=0; lightPending=false;
      sendNano(seqCounter++,"IDLE",0,0); currentSeq=seqCounter-1;
      currentLightScene=LIGHT_IDLE; lightSceneStartAt=millis();
    }
    // The Nano that just recovered may have rebooted (fault could be a
    // brownout/reset, not just a slow response) -- resync its actuator
    // config rather than assume it still remembers what we last told it.
    queueNanoCfgSync();
  }
  // Drain the Nano config sync queue, one field per ~150ms so a burst of
  // updates doesn't overwhelm the soft-serial link the Nano is also trying
  // to service DFPlayer/servo work around.
  if (nanoCfgSyncActive && now-lastNanoCfgSentAt>=150) {
    char code[6]; uint16_t val;
    if (getNanoCfgField(nanoCfgSyncIndex, code, val)) {
      nanoSerial.print(F("CFG,")); nanoSerial.print(code); nanoSerial.print(','); nanoSerial.println(val);
      lastNanoCfgSentAt=now; nanoCfgSyncIndex++;
    } else {
      nanoCfgSyncActive=false;
    }
  }
}

// ---------- show state ----------
static const char* showName(ShowState s) {
  switch(s) {
    case SHOW_IDLE:return "IDLE"; case SHOW_NORMAL:return "NORMAL"; case SHOW_FIGHT:return "FIGHT";
    case SHOW_CELEBRATION:return "CELEBRATION"; case SHOW_SHOWCASE_NORMAL_INTRO:return "SHOWCASE_NORMAL_INTRO";
    case SHOW_SHOWCASE_FIGHT:return "SHOWCASE_FIGHT"; case SHOW_SHOWCASE_CELEBRATION:return "SHOWCASE_CELEBRATION";
    case SHOW_SHOWCASE_RECOVERY:return "SHOWCASE_RECOVERY"; case SHOW_FAULT_SAFE:return "FAULT_SAFE";
    default:return "UNKNOWN";
  }
}
static bool busyShow() {
  return currentState==SHOW_FIGHT || currentState==SHOW_CELEBRATION ||
         currentState==SHOW_SHOWCASE_NORMAL_INTRO || currentState==SHOW_SHOWCASE_FIGHT ||
         currentState==SHOW_SHOWCASE_CELEBRATION || currentState==SHOW_SHOWCASE_RECOVERY;
}
static void transitionTo(ShowState next, uint32_t durationMs) {
  if(currentState==next && !(next==SHOW_FIGHT||next==SHOW_CELEBRATION||
      next==SHOW_SHOWCASE_NORMAL_INTRO||next==SHOW_SHOWCASE_FIGHT||
      next==SHOW_SHOWCASE_CELEBRATION||next==SHOW_SHOWCASE_RECOVERY)) return;
  currentState=next; stateStartedAt=millis(); stateRunStartedAt=stateStartedAt+cfg.startInMs; stateDurationMs=durationMs;
  currentSeq=seqCounter++; stateTransitions++;
  const char* ns=nanoScene(next); sendNano(currentSeq,ns,durationMs,cfg.startInMs);
  scheduleLight(next==SHOW_IDLE?LIGHT_IDLE:next==SHOW_NORMAL||next==SHOW_SHOWCASE_NORMAL_INTRO||next==SHOW_SHOWCASE_RECOVERY?LIGHT_NORMAL:
              next==SHOW_FIGHT||next==SHOW_SHOWCASE_FIGHT?LIGHT_FIGHT:
              next==SHOW_CELEBRATION||next==SHOW_SHOWCASE_CELEBRATION?LIGHT_CELEBRATION:LIGHT_IDLE,
              durationMs,cfg.startInMs);
}
static bool stateFinished() { return stateDurationMs>0 && (int32_t)(millis()-stateRunStartedAt) >= (int32_t)stateDurationMs; }
static void timedStates() {
  if(currentState==SHOW_FIGHT && stateFinished()){lastFightEndedAt=millis();transitionTo(SHOW_CELEBRATION,secMs(cfg.celebrationDurationSec));}
  else if(currentState==SHOW_CELEBRATION && stateFinished()) transitionTo(SHOW_NORMAL,0);
  else if(currentState==SHOW_SHOWCASE_NORMAL_INTRO && stateFinished()) transitionTo(SHOW_SHOWCASE_FIGHT,secMs(cfg.fightDurationSec));
  else if(currentState==SHOW_SHOWCASE_FIGHT && stateFinished()){lastFightEndedAt=millis();transitionTo(SHOW_SHOWCASE_CELEBRATION,secMs(cfg.celebrationDurationSec));}
  else if(currentState==SHOW_SHOWCASE_CELEBRATION && stateFinished()) transitionTo(SHOW_SHOWCASE_RECOVERY,secMs(cfg.normalRecoverySec));
  else if(currentState==SHOW_SHOWCASE_RECOVERY && stateFinished()){lastShowcaseEndedAt=millis();transitionTo(SHOW_NORMAL,0);}
}
static void deriveTracker(TrackerRuntime& t) {
  uint32_t now=millis();
  bool fresh=t.lastPacketAt>0 && now-t.lastPacketAt<=secMs(cfg.sensorStaleSec);
  bool active=fresh && t.lastPresenceAt>0 && now-t.lastPresenceAt<=secMs(cfg.sensorActiveHoldSec);
  if(active && !t.active){t.active=true;t.activeSince=now;}
  if(!active && t.active){t.active=false;t.activeSince=0;}
}
static bool entryActive(){return entryTracker.active;}
static bool exitActive(){return exitTracker.active;}

static void presenceTimers() {
  bool e=entryActive(), x=exitActive(); uint32_t now=millis();
  noPeopleSince=(!e && !x)?(noPeopleSince?noPeopleSince:now):0;
  bothActiveSince=(e&&x)?(bothActiveSince?bothActiveSince:now):0;
  entryOnlySince=(e&&!x)?(entryOnlySince?entryOnlySince:now):0;
}

static void decideShow() {
  deriveTracker(entryTracker); deriveTracker(exitTracker); presenceTimers(); timedStates();
  if(nanoFault){ if(currentState!=SHOW_FAULT_SAFE) transitionTo(SHOW_FAULT_SAFE,0); return; }
  if(busyShow()) return;
  uint32_t now=millis(); bool e=entryActive(),x=exitActive(),both=e&&x;
  uint32_t bothD=bothActiveSince?now-bothActiveSince:0;
  uint32_t entryD=entryOnlySince?now-entryOnlySince:0;
  uint32_t idleD=noPeopleSince?now-noPeopleSince:0;
  bool showOk=lastShowcaseEndedAt==0 || now-lastShowcaseEndedAt>=secMs(cfg.showcaseRepeatCooldownSec);
  bool fightOk=lastFightEndedAt==0 || now-lastFightEndedAt>=secMs(cfg.fightCooldownSec);

  if(both && bothD>=secMs(cfg.bothActiveShowcaseSec) && showOk){transitionTo(SHOW_SHOWCASE_NORMAL_INTRO,secMs(cfg.normalIntroSec));return;}
  if(both && bothD>=secMs(cfg.bothFightDebounceSec) && fightOk && showOk){
    // Rebase the "both active" timer here. Without this, bothActiveSince keeps
    // accumulating through the FIGHT/CELEBRATION scene itself, so if visitors
    // stayed put, bothD could already be past bothActiveShowcaseSec by the time
    // the plain fight ends -- firing a full SHOWCASE immediately back-to-back.
    // Resetting means a fresh bothActiveShowcaseSec of continued presence is
    // required after this fight before a showcase can trigger.
    bothActiveSince=now;
    transitionTo(SHOW_FIGHT,secMs(cfg.fightDurationSec));return;
  }
  if(x){ if(currentState!=SHOW_NORMAL) transitionTo(SHOW_NORMAL,0); return; }
  if(e&&!x&&entryD>=secMs(cfg.entryStandingNormalSec)){if(currentState!=SHOW_NORMAL)transitionTo(SHOW_NORMAL,0);return;}
  if(!e&&!x&&idleD>=secMs(cfg.noPeopleIdleSec)){if(currentState!=SHOW_IDLE)transitionTo(SHOW_IDLE,0);return;}
}

// ---------- sensor HTTP receiver ----------
static void updateTrackerFromJson(const String& body, TrackerRuntime& t, const String& sourceIp) {
  long v=0; bool b=false; String s;
  if(jsonInt(body,"nodeId",v)) {
    if(v<1 || v>20) return;
    t.lastPacketAt=millis();
  } else return;
  if(jsonBool(body,"presence",b)){t.lastPresence=b?1:0;if(b)t.lastPresenceAt=millis();}
  if(jsonInt(body,"confidence",v))t.lastConfidence=constrain(v,0,100);
  if(jsonInt(body,"crowdLevel",v))t.lastCrowd=constrain(v,0,3);
  if(jsonInt(body,"zoneMask",v))t.lastZone=constrain(v,0,7);
  if(jsonInt(body,"motionState",v))t.lastMotion=constrain(v,0,6);
  if(jsonInt(body,"activityScore",v))t.lastActivityScore=constrain(v,0,100);
  if(jsonInt(body,"eventType",v))t.lastEventType=constrain(v,0,8);
  if(jsonInt(body,"eventId",v))t.lastEventId=(uint32_t)v;
  if(jsonBool(body,"sensorFault",b))t.nodeError=b;
  ipText(server.client().remoteIP(), t.lastIp, sizeof(t.lastIp));
}
static void handleSensor() {
  if(!tokenOK()){sendJson("{\"ok\":false,\"error\":\"invalid_token\"}",403);return;}
  if(server.method()!=HTTP_POST){sendJson("{\"ok\":false}",405);return;}
  String body=server.arg("plain");
  if(body.length()==0 || body.length()>1800){rxHttpBadCount++;sendJson("{\"ok\":false,\"error\":\"bad_body\"}",400);return;}
  long nodeId=0;
  if(!jsonInt(body,"nodeId",nodeId)){rxHttpBadCount++;sendJson("{\"ok\":false,\"error\":\"nodeId_required\"}",400);return;}
  TrackerRuntime* t=nullptr;
  if(nodeId==cfg.entryNodeId)t=&entryTracker;
  else if(nodeId==cfg.exitNodeId)t=&exitTracker;
  else {rxHttpBadCount++;sendJson("{\"ok\":false,\"error\":\"unknown_node\"}",404);return;}
  updateTrackerFromJson(body,*t,server.client().remoteIP().toString());
  rxHttpCount++;
  sendJson("{\"ok\":true}");
}

// ---------- API ----------
static void handleStatus() {
  char ip[16]; char hubIp[16];
  ipText(WiFi.localIP(),hubIp,sizeof(hubIp)); ipText(artIp(),ip,sizeof(ip));
  char json[1400];
  snprintf(json,sizeof(json),
    "{\"apiVersion\":2,\"fw\":\"%s\",\"uptimeMs\":%lu,\"wifi\":\"%s\",\"wifiIp\":\"%s\",\"wifiRssi\":%d,"
    "\"state\":\"%s\",\"lightScene\":\"%s\",\"entryActive\":%s,\"exitActive\":%s,"
    "\"entryConfidence\":%u,\"exitConfidence\":%u,\"entryActivityScore\":%u,\"exitActivityScore\":%u,"
    "\"entryNodeError\":%s,\"exitNodeError\":%s,\"rxHttp\":%lu,\"rxBad\":%lu,"
    "\"nanoOk\":%s,\"nanoHealthyResponses\":%u,\"nanoFault\":\"%s\",\"lastAckSeq\":%lu,\"lastDoneSeq\":%lu,"
    "\"artnetIp\":\"%s\",\"artnetUniverse\":%u,\"artnetSendIntervalMs\":%u}",
    FW_VERSION,(unsigned long)millis(),WiFi.status()==WL_CONNECTED?"STA":"AP",hubIp,(int)WiFi.RSSI(),
    showName(currentState),lightName(currentLightScene),entryActive()?"true":"false",exitActive()?"true":"false",
    entryTracker.lastConfidence,exitTracker.lastConfidence,entryTracker.lastActivityScore,exitTracker.lastActivityScore,
    entryTracker.nodeError?"true":"false",exitTracker.nodeError?"true":"false",(unsigned long)rxHttpCount,(unsigned long)rxHttpBadCount,
    (!nanoFault && (lastNanoRxAt==0 || millis()-lastNanoRxAt<secMs(cfg.nanoLinkTimeoutSec)))?"true":"false",
    nanoHealthyResponses,lastFault,(unsigned long)lastAckSeq,(unsigned long)lastDoneSeq,ip,cfg.artnetUniverse,cfg.artnetSendIntervalMs);
  sendJson(json);
}

static void handleFullStatus() {
  String ip=WiFi.localIP().toString(), art=artIp().toString();
  char json[1900];
  snprintf(json,sizeof(json),
    "{\"apiVersion\":2,\"fw\":\"%s\",\"uptimeMs\":%lu,\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d},"
    "\"show\":{\"state\":\"%s\",\"light\":\"%s\",\"busy\":%s},"
    "\"sensors\":{\"entry\":{\"active\":%s,\"confidence\":%u,\"activityScore\":%u,\"crowd\":%u,\"motion\":%u,\"fault\":%s,\"lastSeenMs\":%lu},"
    "\"exit\":{\"active\":%s,\"confidence\":%u,\"activityScore\":%u,\"crowd\":%u,\"motion\":%u,\"fault\":%s,\"lastSeenMs\":%lu}},"
    "\"nano\":{\"ok\":%s,\"fault\":\"%s\",\"healthyResponses\":%u,\"lastAck\":%lu,\"lastDone\":%lu},"
    "\"artnet\":{\"ip\":\"%s\",\"universe\":%u,\"sendIntervalMs\":%u},"
    "\"config\":{\"noPeopleIdleSec\":%u,\"entryStandingNormalSec\":%u,\"bothActiveShowcaseSec\":%u,\"bothFightDebounceSec\":%u,\"fightDurationSec\":%u,\"celebrationDurationSec\":%u,\"normalRecoverySec\":%u,\"fightCooldownSec\":%u}}",
    FW_VERSION,(unsigned long)millis(),WiFi.status()==WL_CONNECTED?"true":"false",ip.c_str(),(int)WiFi.RSSI(),
    showName(currentState),lightName(currentLightScene),busyShow()?"true":"false",
    entryActive()?"true":"false",entryTracker.lastConfidence,entryTracker.lastActivityScore,entryTracker.lastCrowd,entryTracker.lastMotion,entryTracker.nodeError?"true":"false",(unsigned long)entryTracker.lastPacketAt,
    exitActive()?"true":"false",exitTracker.lastConfidence,exitTracker.lastActivityScore,exitTracker.lastCrowd,exitTracker.lastMotion,exitTracker.nodeError?"true":"false",(unsigned long)exitTracker.lastPacketAt,
    (!nanoFault && (lastNanoRxAt==0 || millis()-lastNanoRxAt<secMs(cfg.nanoLinkTimeoutSec)))?"true":"false",lastFault,nanoHealthyResponses,(unsigned long)lastAckSeq,(unsigned long)lastDoneSeq,
    art.c_str(),cfg.artnetUniverse,cfg.artnetSendIntervalMs,cfg.noPeopleIdleSec,cfg.entryStandingNormalSec,cfg.bothActiveShowcaseSec,cfg.bothFightDebounceSec,cfg.fightDurationSec,cfg.celebrationDurationSec,cfg.normalRecoverySec,cfg.fightCooldownSec);
  sendJson(json);
}

static void appendSceneJson(String& out, const char* prefix, const SceneLight& sl) {
  out += ",\""; out+=prefix; out+="R\":"; out+=sl.r;
  out += ",\""; out+=prefix; out+="G\":"; out+=sl.g;
  out += ",\""; out+=prefix; out+="B\":"; out+=sl.b;
  out += ",\""; out+=prefix; out+="R2\":"; out+=sl.r2;
  out += ",\""; out+=prefix; out+="G2\":"; out+=sl.g2;
  out += ",\""; out+=prefix; out+="B2\":"; out+=sl.b2;
  out += ",\""; out+=prefix; out+="Intensity\":"; out+=sl.intensity;
  out += ",\""; out+=prefix; out+="Effect\":"; out+=sl.effect;
  out += ",\""; out+=prefix; out+="FixtureGroupMask\":"; out+=sl.fixtureGroupMask;
  out += ",\""; out+=prefix; out+="Relay1Func\":"; out+=sl.relay1Func;
  out += ",\""; out+=prefix; out+="Relay2Func\":"; out+=sl.relay2Func;
}
static void appendNanoSceneJson(String& out, const char* prefix, const NanoSceneCfg& nc) {
  out += ",\""; out+=prefix; out+="WingMode\":"; out+=nc.wingMode;
  out += ",\""; out+=prefix; out+="WingSpeed\":"; out+=nc.wingSpeed;
  out += ",\""; out+=prefix; out+="LipMode\":"; out+=nc.lipMode;
  out += ",\""; out+=prefix; out+="DfFileCount\":"; out+=nc.dfFileCount;
}
static void handleConfigGet() {
  char json[1300];
  snprintf(json,sizeof(json),
    "{\"apiVersion\":2,\"entryNodeId\":%u,\"exitNodeId\":%u,\"sensorActiveHoldSec\":%u,\"sensorStaleSec\":%u,"
    "\"noPeopleIdleSec\":%u,\"entryStandingNormalSec\":%u,\"bothActiveShowcaseSec\":%u,\"bothFightDebounceSec\":%u,"
    "\"fightDurationSec\":%u,\"celebrationDurationSec\":%u,\"normalIntroSec\":%u,\"normalRecoverySec\":%u,"
    "\"fightCooldownSec\":%u,\"showcaseRepeatCooldownSec\":%u,\"nanoAckTimeoutMs\":%u,\"nanoLinkTimeoutSec\":%u,"
    "\"nanoAutoRecover\":%u,\"nanoRecoverGoodResponses\":%u,\"startInMs\":%u,\"artnetUniverse\":%u,\"artnetSendIntervalMs\":%u,"
    "\"wingHomePwm\":%u,\"lipClosedAngle\":%u,\"lipOpenMin\":%u,\"lipOpenMax\":%u,\"wingRampMs\":%u",
    cfg.entryNodeId,cfg.exitNodeId,cfg.sensorActiveHoldSec,cfg.sensorStaleSec,cfg.noPeopleIdleSec,cfg.entryStandingNormalSec,
    cfg.bothActiveShowcaseSec,cfg.bothFightDebounceSec,cfg.fightDurationSec,cfg.celebrationDurationSec,cfg.normalIntroSec,cfg.normalRecoverySec,
    cfg.fightCooldownSec,cfg.showcaseRepeatCooldownSec,cfg.nanoAckTimeoutMs,cfg.nanoLinkTimeoutSec,cfg.nanoAutoRecover,cfg.nanoRecoverGoodResponses,
    cfg.startInMs,cfg.artnetUniverse,cfg.artnetSendIntervalMs,
    cfg.wingHomePwm,cfg.lipClosedAngle,cfg.lipOpenMin,cfg.lipOpenMax,cfg.wingRampMs);
  // Appended (rather than folded into the snprintf above) so the 60
  // per-scene fields don't need their own place in that format string --
  // this endpoint's payload is built once every settings-page load, not on
  // a hot path, so the extra String concatenation costs nothing that matters.
  String out = json;
  appendSceneJson(out,"idle",cfg.sceneLight[LIGHT_IDLE]);
  appendSceneJson(out,"normal",cfg.sceneLight[LIGHT_NORMAL]);
  appendSceneJson(out,"fight",cfg.sceneLight[LIGHT_FIGHT]);
  appendSceneJson(out,"celeb",cfg.sceneLight[LIGHT_CELEBRATION]);
  appendNanoSceneJson(out,"idle",cfg.nanoScene[LIGHT_IDLE]);
  appendNanoSceneJson(out,"normal",cfg.nanoScene[LIGHT_NORMAL]);
  appendNanoSceneJson(out,"fight",cfg.nanoScene[LIGHT_FIGHT]);
  appendNanoSceneJson(out,"celeb",cfg.nanoScene[LIGHT_CELEBRATION]);
  out += "}";
  sendJson(out);
}

static void jsonSceneFields(const String& body, const char* prefix, SceneLight& sl) {
  long n; char key[24];
  snprintf(key,sizeof(key),"%sR",prefix);               if(jsonInt(body,key,n)) sl.r=(uint8_t)constrain(n,0,255);
  snprintf(key,sizeof(key),"%sG",prefix);               if(jsonInt(body,key,n)) sl.g=(uint8_t)constrain(n,0,255);
  snprintf(key,sizeof(key),"%sB",prefix);               if(jsonInt(body,key,n)) sl.b=(uint8_t)constrain(n,0,255);
  snprintf(key,sizeof(key),"%sR2",prefix);              if(jsonInt(body,key,n)) sl.r2=(uint8_t)constrain(n,0,255);
  snprintf(key,sizeof(key),"%sG2",prefix);              if(jsonInt(body,key,n)) sl.g2=(uint8_t)constrain(n,0,255);
  snprintf(key,sizeof(key),"%sB2",prefix);              if(jsonInt(body,key,n)) sl.b2=(uint8_t)constrain(n,0,255);
  snprintf(key,sizeof(key),"%sIntensity",prefix);       if(jsonInt(body,key,n)) sl.intensity=(uint8_t)constrain(n,0,255);
  snprintf(key,sizeof(key),"%sEffect",prefix);          if(jsonInt(body,key,n)) sl.effect=(uint8_t)constrain(n,0,(long)EFFECT_CHASE);
  snprintf(key,sizeof(key),"%sFixtureGroupMask",prefix);if(jsonInt(body,key,n)) sl.fixtureGroupMask=(uint8_t)constrain(n,0,15);
  snprintf(key,sizeof(key),"%sRelay1Func",prefix);      if(jsonInt(body,key,n)) sl.relay1Func=(uint8_t)constrain(n,0,(long)RELAY_STROBE);
  snprintf(key,sizeof(key),"%sRelay2Func",prefix);      if(jsonInt(body,key,n)) sl.relay2Func=(uint8_t)constrain(n,0,(long)RELAY_STROBE);
}
static void jsonNanoSceneFields(const String& body, const char* prefix, NanoSceneCfg& nc) {
  long n; char key[24];
  snprintf(key,sizeof(key),"%sWingMode",prefix);   if(jsonInt(body,key,n)) nc.wingMode=(uint8_t)constrain(n,0,(long)WING_MODE_HOME);
  snprintf(key,sizeof(key),"%sWingSpeed",prefix);  if(jsonInt(body,key,n)) nc.wingSpeed=(uint8_t)constrain(n,0,255);
  snprintf(key,sizeof(key),"%sLipMode",prefix);    if(jsonInt(body,key,n)) nc.lipMode=(uint8_t)constrain(n,0,(long)LIP_MODE_ANIMATE);
  snprintf(key,sizeof(key),"%sDfFileCount",prefix);if(jsonInt(body,key,n)) nc.dfFileCount=(uint8_t)constrain(n,0,255);
}
static void handleConfigPost() {
  if(!tokenOK()){sendJson("{\"ok\":false,\"error\":\"invalid_token\"}",403);return;}
  String body=server.arg("plain"); long n;
  // Bounds applied inline, at the moment each value is parsed -- not
  // deferred to validateConfig() alone. Both still run (validateConfig()
  // via saveConfig() below), but a value can no longer sit in cfg
  // unvalidated even momentarily, and the bound lives visibly next to the
  // field it protects rather than only in one function far from here.
  #define SET_U16(key,field,lo,hi) if(jsonInt(body,key,n)){cfg.field=(uint16_t)constrain(n,(long)(lo),(long)(hi));}
  #define SET_U8(key,field,lo,hi) if(jsonInt(body,key,n)){cfg.field=(uint8_t)constrain(n,(long)(lo),(long)(hi));}
  SET_U8("entryNodeId",entryNodeId,1,20); SET_U8("exitNodeId",exitNodeId,1,20);
  SET_U16("sensorActiveHoldSec",sensorActiveHoldSec,SENSOR_ACTIVE_HOLD_MIN,SENSOR_ACTIVE_HOLD_MAX);
  SET_U16("sensorStaleSec",sensorStaleSec,SENSOR_STALE_MIN,SENSOR_STALE_MAX);
  SET_U16("noPeopleIdleSec",noPeopleIdleSec,NO_PEOPLE_IDLE_MIN,NO_PEOPLE_IDLE_MAX);
  SET_U16("entryStandingNormalSec",entryStandingNormalSec,ENTRY_STANDING_NORMAL_MIN,ENTRY_STANDING_NORMAL_MAX);
  SET_U16("bothActiveShowcaseSec",bothActiveShowcaseSec,BOTH_ACTIVE_SHOWCASE_MIN,BOTH_ACTIVE_SHOWCASE_MAX);
  SET_U16("bothFightDebounceSec",bothFightDebounceSec,BOTH_FIGHT_DEBOUNCE_MIN,BOTH_FIGHT_DEBOUNCE_MAX);
  SET_U16("fightDurationSec",fightDurationSec,FIGHT_DURATION_MIN,FIGHT_DURATION_MAX);
  SET_U16("celebrationDurationSec",celebrationDurationSec,CELEBRATION_DURATION_MIN,CELEBRATION_DURATION_MAX);
  SET_U16("normalIntroSec",normalIntroSec,NORMAL_INTRO_MIN,NORMAL_INTRO_MAX);
  SET_U16("normalRecoverySec",normalRecoverySec,NORMAL_RECOVERY_MIN,NORMAL_RECOVERY_MAX);
  SET_U16("fightCooldownSec",fightCooldownSec,FIGHT_COOLDOWN_MIN,FIGHT_COOLDOWN_MAX);
  SET_U16("showcaseRepeatCooldownSec",showcaseRepeatCooldownSec,SHOWCASE_REPEAT_COOLDOWN_MIN,SHOWCASE_REPEAT_COOLDOWN_MAX);
  SET_U16("nanoAckTimeoutMs",nanoAckTimeoutMs,NANO_ACK_TIMEOUT_MIN,NANO_ACK_TIMEOUT_MAX);
  SET_U16("nanoLinkTimeoutSec",nanoLinkTimeoutSec,NANO_LINK_TIMEOUT_MIN,NANO_LINK_TIMEOUT_MAX);
  SET_U8("nanoAutoRecover",nanoAutoRecover,0,1);
  SET_U8("nanoRecoverGoodResponses",nanoRecoverGoodResponses,NANO_RECOVER_GOOD_RESPONSES_MIN,NANO_RECOVER_GOOD_RESPONSES_MAX);
  SET_U16("startInMs",startInMs,START_IN_MS_MIN,START_IN_MS_MAX);
  SET_U16("artnetUniverse",artnetUniverse,ARTNET_UNIVERSE_MIN,ARTNET_UNIVERSE_MAX);
  SET_U16("artnetSendIntervalMs",artnetSendIntervalMs,ARTNET_SEND_INTERVAL_MIN,ARTNET_SEND_INTERVAL_MAX);
  SET_U8("artnetIp1",artnetIp1,0,255); SET_U8("artnetIp2",artnetIp2,0,255);
  SET_U8("artnetIp3",artnetIp3,0,255); SET_U8("artnetIp4",artnetIp4,0,255);
  SET_U8("wingHomePwm",wingHomePwm,0,255); SET_U8("lipClosedAngle",lipClosedAngle,0,180);
  SET_U16("wingRampMs",wingRampMs,WING_RAMP_MIN,WING_RAMP_MAX);
  SET_U8("lipOpenMin",lipOpenMin,0,180); SET_U8("lipOpenMax",lipOpenMax,0,180);
  #undef SET_U16
  #undef SET_U8
  jsonSceneFields(body,"idle",cfg.sceneLight[LIGHT_IDLE]);
  jsonSceneFields(body,"normal",cfg.sceneLight[LIGHT_NORMAL]);
  jsonSceneFields(body,"fight",cfg.sceneLight[LIGHT_FIGHT]);
  jsonSceneFields(body,"celeb",cfg.sceneLight[LIGHT_CELEBRATION]);
  jsonNanoSceneFields(body,"idle",cfg.nanoScene[LIGHT_IDLE]);
  jsonNanoSceneFields(body,"normal",cfg.nanoScene[LIGHT_NORMAL]);
  jsonNanoSceneFields(body,"fight",cfg.nanoScene[LIGHT_FIGHT]);
  jsonNanoSceneFields(body,"celeb",cfg.nanoScene[LIGHT_CELEBRATION]);
  saveConfig();
  sendJson("{\"ok\":true}");
}

static void applyManualScene(const String& cmd) {
  if(cmd=="IDLE") transitionTo(SHOW_IDLE,0);
  else if(cmd=="NORMAL") transitionTo(SHOW_NORMAL,0);
  else if(cmd=="FIGHT") transitionTo(SHOW_FIGHT,secMs(cfg.fightDurationSec));
  else if(cmd=="CELEBRATION") transitionTo(SHOW_CELEBRATION,secMs(cfg.celebrationDurationSec));
  else if(cmd=="SHOWCASE") transitionTo(SHOW_SHOWCASE_NORMAL_INTRO,secMs(cfg.normalIntroSec));
  else if(cmd=="CLEAR_FAULT") {
    if(nanoFault && nanoHealthyResponses < cfg.nanoRecoverGoodResponses) return;
    clearFault(); transitionTo(SHOW_IDLE,0);
  }
}
static void handleCommand() {
  if(!tokenOK()){sendJson("{\"ok\":false,\"error\":\"invalid_token\"}",403);return;}
  String body=server.arg("plain"),cmd;
  if(!jsonString(body,"command",cmd)){sendJson("{\"ok\":false,\"error\":\"command_required\"}",400);return;}
  cmd.toUpperCase();
  if(cmd=="CLEAR_FAULT" && nanoFault && nanoHealthyResponses < cfg.nanoRecoverGoodResponses) {
    sendJson("{\"ok\":false,\"error\":\"fault_not_recoverable_yet\"}",409); return;
  }
  if(cmd=="REBOOT") { sendJson("{\"ok\":true}"); delay(200); ESP.restart(); return; }
  if(cmd!="IDLE"&&cmd!="NORMAL"&&cmd!="FIGHT"&&cmd!="CELEBRATION"&&cmd!="SHOWCASE"&&cmd!="CLEAR_FAULT") {
    sendJson("{\"ok\":false,\"error\":\"unknown_command\"}",400); return;
  }
  applyManualScene(cmd); sendJson("{\"ok\":true}");
}
static void handleScene() {
  if(!tokenOK()){sendJson("{\"ok\":false,\"error\":\"invalid_token\"}",403);return;}
  String body=server.arg("plain"),scene; long duration=0,startIn=cfg.startInMs;
  if(!jsonString(body,"scene",scene)){sendJson("{\"ok\":false,\"error\":\"scene_required\"}",400);return;}
  scene.toUpperCase();
  jsonInt(body,"durationMs",duration); jsonInt(body,"startInMs",startIn);
  if(scene=="FIGHT")transitionTo(SHOW_FIGHT,(uint32_t)constrain(duration>0?duration:(long)secMs(cfg.fightDurationSec),5000L,300000L));
  else if(scene=="NORMAL")transitionTo(SHOW_NORMAL,0);
  else if(scene=="CELEBRATION")transitionTo(SHOW_CELEBRATION,(uint32_t)constrain(duration>0?duration:(long)secMs(cfg.celebrationDurationSec),5000L,300000L));
  else if(scene=="IDLE")transitionTo(SHOW_IDLE,0);
  else {sendJson("{\"ok\":false,\"error\":\"unknown_scene\"}",400);return;}
  sendJson("{\"ok\":true}");
}


static const IPAddress sensorIpForNode(uint8_t nodeId) {
  return nodeId == cfg.entryNodeId ? IPAddress(192,168,0,20) : IPAddress(192,168,0,21);
}
static void handleSensorConfig() {
  if(!tokenOK()){sendJson("{\"ok\":false,\"error\":\"invalid_token\"}",403);return;}
  // This handler makes its own outbound HTTP call to the sensor node before
  // it can respond, and that call runs inside server.handleClient(), which
  // loop() calls before decideShow()/sendArtNet()/readNano() every
  // iteration. If it were allowed to fire during a live scene, a slow or
  // unreachable sensor node would stall ArtDMX output and Nano ACK handling
  // for as long as the call takes -- worse than its 1200ms setTimeout()
  // suggests, since that timeout isn't guaranteed to bound the TCP connect
  // phase on every esp8266 core version. Refuse outright while busy so this
  // can never happen mid-show, rather than relying on nobody triggering it
  // at the wrong moment.
  if(busyShow()){sendJson("{\"ok\":false,\"error\":\"busy_show\"}",503);return;}
  String body=server.arg("plain"); long node=0;
  if(!jsonInt(body,"nodeId",node) || node<1 || node>20){sendJson("{\"ok\":false,\"error\":\"nodeId_required\"}",400);return;}
  if(node!=cfg.entryNodeId && node!=cfg.exitNodeId){sendJson("{\"ok\":false,\"error\":\"unknown_node\"}",404);return;}
  // The sensor node's own /api/config handler only reads the keys it
  // recognizes and ignores everything else, so the body (including the
  // "nodeId" routing key) can be forwarded as-is. Editing it in place here
  // used to risk dropping the closing '}' whenever nodeId was the last key.
  WiFiClient client; HTTPClient http;
  String url=String("http://")+sensorIpForNode((uint8_t)node).toString()+"/api/config";
  if(!http.begin(client,url)){sendJson("{\"ok\":false,\"error\":\"sensor_connect\"}",502);return;}
  http.setTimeout(1200); http.addHeader("Content-Type","application/json");
  if(API_TOKEN[0] != '\0' && strcmp(API_TOKEN,"CHANGE_ME_LOCAL_TOKEN")!=0) http.addHeader("X-Garur-Token",API_TOKEN);
  int code=http.POST(body); String r=code>=200&&code<300?http.getString():String(); http.end();
  sendJson(r.length()?r:"{\"ok\":false,\"error\":\"sensor_http\"}",code>=200&&code<300?200:502);
}

// ---------- dashboard ----------
static void handleRoot() {
  static const char page[] PROGMEM =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Garur Hub v2</title><style>body{font-family:Arial;background:#101010;color:#eee;padding:16px}.card{background:#1e1e1e;padding:14px;border-radius:12px;margin:10px 0}.ok{color:#0f0}.bad{color:#f55}.btn{display:inline-block;padding:10px 13px;margin:4px;border-radius:8px;background:#f5a400;color:#111;text-decoration:none;font-weight:700}</style></head>"
    "<body><h2>Garur Hub v2</h2><div class='card'>"
    "<div>State: <b id=s>...</b></div><div>Entry: <b id=e>...</b> | Exit: <b id=x>...</b></div>"
    "<div>Entry score: <b id=es>...</b> | Exit score: <b id=xs>...</b></div>"
    "<div>Nano: <b id=n>...</b> | ArtNet: <b id=a>...</b></div><div>Fault: <b id=f>...</b></div>"
    "</div><div class='card'><a class=btn href='/api/fullStatus'>Full Status JSON</a><a class=btn href='/settings'>Settings</a></div>"
    "<div class='card'><b>Manual</b><br>"
    "<a class=btn href='/api/command?scene=IDLE'>IDLE</a><a class=btn href='/api/command?scene=NORMAL'>NORMAL</a>"
    "<a class=btn href='/api/command?scene=FIGHT'>FIGHT</a><a class=btn href='/api/command?scene=CELEBRATION'>CELEBRATION</a></div>"
    "<script>async function u(){try{let j=await fetch('/api/status').then(r=>r.json());s.textContent=j.state;e.textContent=j.entryActive?'ACTIVE':'clear';x.textContent=j.exitActive?'ACTIVE':'clear';es.textContent=j.entryActivityScore;xs.textContent=j.exitActivityScore;n.textContent=j.nanoOk?'OK':'CHECK';a.textContent=j.artnetIp;f.textContent=j.nanoFault||'none';}catch(e){}}setInterval(u,1000);u();</script>"
    "</body></html>";
  server.send_P(200,"text/html",page);
}
static String hexColor(uint8_t r, uint8_t g, uint8_t b) {
  char buf[8]; snprintf(buf,sizeof(buf),"#%02X%02X%02X",r,g,b); return String(buf);
}
static void handleSettings() {
  // ~15.5KB fully rendered (measured with a native test harness, not
  // guessed): a single String that size is a meaningful fraction of a
  // typical ESP8266's free heap (usually tens of KB after the WiFi/TCP
  // stack's own overhead) and, more importantly, a single ~15.5KB
  // *contiguous* allocation is the kind of request that gets harder to
  // satisfy as heap fragments over a long uptime, even when aggregate free
  // memory looks fine. Streamed in chunks instead of built as one buffer --
  // peak memory is whatever the single largest chunk is (~2.9KB, one scene
  // card, now covering both lighting and Nano actuator fields) rather than
  // the full page, since each chunk is sent and cleared before the next is
  // built. This is a manual settings page a human loads occasionally, not
  // part of the real-time show loop, so the extra round trips to the TCP
  // stack cost nothing that matters.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  String h; h.reserve(1700);
  h += F(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Garur Hub Settings</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Arial,sans-serif;max-width:640px;margin:0 auto;padding:16px;background:#f4f5f7;color:#222}"
    "h2{margin-top:0}"
    ".card{background:#fff;border-radius:8px;padding:14px 16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.12)}"
    ".card>p{margin-top:0;font-size:0.85rem;color:#666}"
    ".card h3{margin:0 0 10px;font-size:1.05rem;border-bottom:2px solid #eee;padding-bottom:6px}"
    ".row{margin-bottom:8px;display:flex;align-items:center;flex-wrap:wrap;gap:6px}"
    ".row label{min-width:150px;font-size:0.9rem;flex-shrink:0}"
    "input[type=number]{width:60px;padding:5px;border:1px solid #ccc;border-radius:4px}"
    "select{padding:5px;border:1px solid #ccc;border-radius:4px}"
    "input[type=color]{width:36px;height:30px;border:1px solid #ccc;border-radius:4px;padding:0;vertical-align:middle}"
    ".hint{font-size:0.78rem;color:#888;flex-basis:100%;margin:-4px 0 2px}"
    "button{background:#2b6cb0;color:#fff;border:0;padding:11px 24px;border-radius:6px;font-size:1rem}"
    "a{color:#2b6cb0}"
    "</style>"
    "<script>function syncColor(r,g,b,hex){document.getElementById(r).value=parseInt(hex.substr(1,2),16);document.getElementById(g).value=parseInt(hex.substr(3,2),16);document.getElementById(b).value=parseInt(hex.substr(5,2),16);}</script>"
    "</head><body><h2>Hub Settings</h2><form method='POST' action='/saveSettings'>");
  // A native HTML form can't send the X-Garur-Token header, so the token
  // (once a real one is set) rides along as a hidden field instead --
  // settingsFormTokenOK() accepts either. Harmless when API_TOKEN is still
  // the CHANGE_ME placeholder, since tokenOK() already bypasses the check
  // entirely in that case.
  h += "<input type=hidden name='token' value='"; h += API_TOKEN; h += "'>";
  server.sendContent(h); h = "";

  h.reserve(1300);
  h += F("<div class=card><h3>Scene Timing</h3>");
  const char* fields[]={"noPeopleIdleSec","entryStandingNormalSec","bothActiveShowcaseSec","bothFightDebounceSec","fightDurationSec","celebrationDurationSec","normalIntroSec","normalRecoverySec","fightCooldownSec","showcaseRepeatCooldownSec"};
  const uint16_t vals[]={cfg.noPeopleIdleSec,cfg.entryStandingNormalSec,cfg.bothActiveShowcaseSec,cfg.bothFightDebounceSec,cfg.fightDurationSec,cfg.celebrationDurationSec,cfg.normalIntroSec,cfg.normalRecoverySec,cfg.fightCooldownSec,cfg.showcaseRepeatCooldownSec};
  for(uint8_t i=0;i<10;i++){h += "<div class=row><label>"+String(fields[i])+"</label><input type=number name='"+String(fields[i])+"' value='"+String(vals[i])+"'></div>";}
  h += F("</div>");
  server.sendContent(h); h = "";

  h.reserve(800);
  h += F("<div class=card><h3>ArtNet</h3><p>If your ArtNet node doesn't have a static IP, reserve its address in your router's DHCP settings, then set the same address here.</p>");
  h += "<div class=row><label>IP address</label><input type=number min=0 max=255 name='artnetIp1' value='"+String(cfg.artnetIp1)+"'>.";
  h += "<input type=number min=0 max=255 name='artnetIp2' value='"+String(cfg.artnetIp2)+"'>.";
  h += "<input type=number min=0 max=255 name='artnetIp3' value='"+String(cfg.artnetIp3)+"'>.";
  h += "<input type=number min=0 max=255 name='artnetIp4' value='"+String(cfg.artnetIp4)+"'></div>";
  h += "<div class=row><label>artnetUniverse</label><input type=number min=0 max=32767 name='artnetUniverse' value='"+String(cfg.artnetUniverse)+"'></div>";
  h += "<div class=row><label>artnetSendIntervalMs</label><input type=number min=20 max=100 name='artnetSendIntervalMs' value='"+String(cfg.artnetSendIntervalMs)+"'></div></div>";
  server.sendContent(h); h = "";

  h.reserve(1100);
  h += F("<div class=card><h3>Nano Actuators (global)</h3><p>Applies across all scenes -- per-scene wing/lip/sound behavior is set in each scene's card below.</p>");
  h += "<div class=row><label>Wing home speed</label><input type=number min=0 max=255 name='wingHomePwm' value='"+String(cfg.wingHomePwm)+"'></div>";
  h += "<div class=row><label>Lip closed angle</label><input type=number min=0 max=180 name='lipClosedAngle' value='"+String(cfg.lipClosedAngle)+"'></div>";
  h += "<div class=row><label>Lip open min</label><input type=number min=0 max=180 name='lipOpenMin' value='"+String(cfg.lipOpenMin)+"'></div>";
  h += "<div class=row><label>Lip open max</label><input type=number min=0 max=180 name='lipOpenMax' value='"+String(cfg.lipOpenMax)+"'></div>";
  h += "<div class=row><label>Wing ramp time (ms)</label><input type=number min=0 max=5000 name='wingRampMs' value='"+String(cfg.wingRampMs)+"'>";
  h += "<span class=hint>Time to ramp the wing motor across its full speed range, easing inertia at start/stop. 0 = instant (no ramp). Homing always uses its own low speed above and stops instantly the moment it reaches the sensor, unaffected by this.</span></div></div>";
  server.sendContent(h); h = "";

  // Fixtures: 4 PAR cans on channels 1-12 (3 each, consecutive), relays on
  // channels 13/14. Fixture group (A/B) only matters for Alternate/Chase --
  // other effects treat all 4 fixtures identically and ignore it.
  struct SceneUi { const char* prefix; const char* label; SceneLight* sl; NanoSceneCfg* nc; };
  SceneUi scenes[] = {
    {"idle","Idle",&cfg.sceneLight[LIGHT_IDLE],&cfg.nanoScene[LIGHT_IDLE]},
    {"normal","Normal",&cfg.sceneLight[LIGHT_NORMAL],&cfg.nanoScene[LIGHT_NORMAL]},
    {"fight","Fight",&cfg.sceneLight[LIGHT_FIGHT],&cfg.nanoScene[LIGHT_FIGHT]},
    {"celeb","Celebration",&cfg.sceneLight[LIGHT_CELEBRATION],&cfg.nanoScene[LIGHT_CELEBRATION]},
  };
  const char* effectNames[] = {"Solid","Blink","Strobe","Alternate","Chase"};
  const char* relayFuncNames[] = {"Off","On","Blink","Strobe"};
  const char* wingModeNames[] = {"Stop","Spin","Home"};
  const char* lipModeNames[] = {"Closed","Animate"};
  for (uint8_t i=0;i<4;i++) {
    SceneUi& su = scenes[i];
    String p(su.prefix);
    h.reserve(3300);
    h += "<div class=card><h3>"+String(su.label)+"</h3>";

    h += "<div class=row><label>Colour</label><input type=color id='"+p+"Pick' value='"+hexColor(su.sl->r,su.sl->g,su.sl->b)+"' oninput=\"syncColor('"+p+"R','"+p+"G','"+p+"B',this.value)\"> ";
    h += "R <input type=number id='"+p+"R' min=0 max=255 name='"+p+"R' value='"+String(su.sl->r)+"'> ";
    h += "G <input type=number id='"+p+"G' min=0 max=255 name='"+p+"G' value='"+String(su.sl->g)+"'> ";
    h += "B <input type=number id='"+p+"B' min=0 max=255 name='"+p+"B' value='"+String(su.sl->b)+"'></div>";

    h += "<div class=row><label>2nd colour</label><input type=color id='"+p+"Pick2' value='"+hexColor(su.sl->r2,su.sl->g2,su.sl->b2)+"' oninput=\"syncColor('"+p+"R2','"+p+"G2','"+p+"B2',this.value)\"> ";
    h += "R <input type=number id='"+p+"R2' min=0 max=255 name='"+p+"R2' value='"+String(su.sl->r2)+"'> ";
    h += "G <input type=number id='"+p+"G2' min=0 max=255 name='"+p+"G2' value='"+String(su.sl->g2)+"'> ";
    h += "B <input type=number id='"+p+"B2' min=0 max=255 name='"+p+"B2' value='"+String(su.sl->b2)+"'>";
    h += "<span class=hint>2nd colour and fixture groups below are only used by Alternate/Chase</span></div>";

    h += "<div class=row><label>Intensity</label><input type=number min=0 max=255 name='"+p+"Intensity' value='"+String(su.sl->intensity)+"'></div>";

    h += "<div class=row><label>Effect</label><select name='"+p+"Effect'>";
    for (uint8_t e=0;e<5;e++) h += "<option value='"+String(e)+"'"+(su.sl->effect==e?" selected":"")+">"+effectNames[e]+"</option>";
    h += "</select></div>";

    h += "<div class=row><label>Fixture groups</label>";
    for (uint8_t fx=1;fx<=4;fx++) {
      bool isB = (su.sl->fixtureGroupMask>>(fx-1))&1;
      h += "#"+String(fx)+" <select name='"+p+"Fixture"+String(fx)+"'>";
      h += String("<option value=0")+(!isB?" selected":"")+">A</option>";
      h += String("<option value=1")+(isB?" selected":"")+">B</option>";
      h += "</select> ";
    }
    h += "</div>";

    h += "<div class=row><label>Relay 1 (ch 13)</label><select name='"+p+"Relay1Func'>";
    for (uint8_t rf=0;rf<4;rf++) h += "<option value='"+String(rf)+"'"+(su.sl->relay1Func==rf?" selected":"")+">"+relayFuncNames[rf]+"</option>";
    h += "</select></div>";
    h += "<div class=row><label>Relay 2 (ch 14)</label><select name='"+p+"Relay2Func'>";
    for (uint8_t rf=0;rf<4;rf++) h += "<option value='"+String(rf)+"'"+(su.sl->relay2Func==rf?" selected":"")+">"+relayFuncNames[rf]+"</option>";
    h += "</select></div>";

    h += "<div class=row><label>Wing mode</label><select name='"+p+"WingMode'>";
    for (uint8_t wm=0;wm<3;wm++) h += "<option value='"+String(wm)+"'"+(su.nc->wingMode==wm?" selected":"")+">"+wingModeNames[wm]+"</option>";
    h += "</select></div>";
    h += "<div class=row><label>Wing speed</label><input type=number min=0 max=255 name='"+p+"WingSpeed' value='"+String(su.nc->wingSpeed)+"'>";
    h += "<span class=hint>Only used when wing mode is Spin</span></div>";
    h += "<div class=row><label>Lip mode</label><select name='"+p+"LipMode'>";
    for (uint8_t lm=0;lm<2;lm++) h += "<option value='"+String(lm)+"'"+(su.nc->lipMode==lm?" selected":"")+">"+lipModeNames[lm]+"</option>";
    h += "</select></div>";
    h += "<div class=row><label>Sound clips in folder</label><input type=number min=0 max=255 name='"+p+"DfFileCount' value='"+String(su.nc->dfFileCount)+"'>";
    h += "<span class=hint>DFPlayer folder /0"+String(i+1)+"/ -- one picked at random each time this scene starts</span></div>";

    h += "</div>";
    server.sendContent(h); h = "";
  }
  h += F("<button type=submit>Save</button></form><p><a href='/'>Back</a></p></body></html>");
  server.sendContent(h);
  // A chunked response (setContentLength(CONTENT_LENGTH_UNKNOWN) above) needs
  // an explicit empty chunk to tell the client the transfer is finished --
  // documented ESP8266WebServer gotcha, not optional cleanup: without it
  // some clients can hang or render an incomplete page waiting for more.
  server.sendContent(F(""));
}
static bool setSceneField(SceneLight& sl, const String& key, const char* prefix, long n) {
  String p(prefix);
  if (key==p+"R") { sl.r=(uint8_t)constrain(n,0,255); return true; }
  if (key==p+"G") { sl.g=(uint8_t)constrain(n,0,255); return true; }
  if (key==p+"B") { sl.b=(uint8_t)constrain(n,0,255); return true; }
  if (key==p+"R2") { sl.r2=(uint8_t)constrain(n,0,255); return true; }
  if (key==p+"G2") { sl.g2=(uint8_t)constrain(n,0,255); return true; }
  if (key==p+"B2") { sl.b2=(uint8_t)constrain(n,0,255); return true; }
  if (key==p+"Intensity") { sl.intensity=(uint8_t)constrain(n,0,255); return true; }
  if (key==p+"Effect") { sl.effect=(uint8_t)constrain(n,0,(long)EFFECT_CHASE); return true; }
  if (key==p+"Relay1Func") { sl.relay1Func=(uint8_t)constrain(n,0,(long)RELAY_STROBE); return true; }
  if (key==p+"Relay2Func") { sl.relay2Func=(uint8_t)constrain(n,0,(long)RELAY_STROBE); return true; }
  for (uint8_t fx=1;fx<=4;fx++) {
    if (key==p+"Fixture"+String(fx)) {
      uint8_t bit=(uint8_t)constrain(n,0,1);
      if (bit) sl.fixtureGroupMask |= (1<<(fx-1)); else sl.fixtureGroupMask &= ~(1<<(fx-1));
      return true;
    }
  }
  return false;
}
static bool setNanoSceneField(NanoSceneCfg& nc, const String& key, const char* prefix, long n) {
  String p(prefix);
  if (key==p+"WingMode") { nc.wingMode=(uint8_t)constrain(n,0,(long)WING_MODE_HOME); return true; }
  if (key==p+"WingSpeed") { nc.wingSpeed=(uint8_t)constrain(n,0,255); return true; }
  if (key==p+"LipMode") { nc.lipMode=(uint8_t)constrain(n,0,(long)LIP_MODE_ANIMATE); return true; }
  if (key==p+"DfFileCount") { nc.dfFileCount=(uint8_t)constrain(n,0,255); return true; }
  return false;
}
static void handleSaveSettings() {
  if(!settingsFormTokenOK()){sendJson("{\"ok\":false,\"error\":\"invalid_token\"}",403);return;}
  long n;
  for(uint8_t i=0;i<server.args();i++) {
    String k=server.argName(i); n=server.arg(i).toInt();
    if(k=="noPeopleIdleSec") cfg.noPeopleIdleSec=(uint16_t)constrain(n,(long)NO_PEOPLE_IDLE_MIN,(long)NO_PEOPLE_IDLE_MAX);
    else if(k=="entryStandingNormalSec") cfg.entryStandingNormalSec=(uint16_t)constrain(n,(long)ENTRY_STANDING_NORMAL_MIN,(long)ENTRY_STANDING_NORMAL_MAX);
    else if(k=="bothActiveShowcaseSec") cfg.bothActiveShowcaseSec=(uint16_t)constrain(n,(long)BOTH_ACTIVE_SHOWCASE_MIN,(long)BOTH_ACTIVE_SHOWCASE_MAX);
    else if(k=="bothFightDebounceSec") cfg.bothFightDebounceSec=(uint16_t)constrain(n,(long)BOTH_FIGHT_DEBOUNCE_MIN,(long)BOTH_FIGHT_DEBOUNCE_MAX);
    else if(k=="fightDurationSec") cfg.fightDurationSec=(uint16_t)constrain(n,(long)FIGHT_DURATION_MIN,(long)FIGHT_DURATION_MAX);
    else if(k=="celebrationDurationSec") cfg.celebrationDurationSec=(uint16_t)constrain(n,(long)CELEBRATION_DURATION_MIN,(long)CELEBRATION_DURATION_MAX);
    else if(k=="normalIntroSec") cfg.normalIntroSec=(uint16_t)constrain(n,(long)NORMAL_INTRO_MIN,(long)NORMAL_INTRO_MAX);
    else if(k=="normalRecoverySec") cfg.normalRecoverySec=(uint16_t)constrain(n,(long)NORMAL_RECOVERY_MIN,(long)NORMAL_RECOVERY_MAX);
    else if(k=="fightCooldownSec") cfg.fightCooldownSec=(uint16_t)constrain(n,(long)FIGHT_COOLDOWN_MIN,(long)FIGHT_COOLDOWN_MAX);
    else if(k=="showcaseRepeatCooldownSec") cfg.showcaseRepeatCooldownSec=(uint16_t)constrain(n,(long)SHOWCASE_REPEAT_COOLDOWN_MIN,(long)SHOWCASE_REPEAT_COOLDOWN_MAX);
    else if(k=="artnetIp1") cfg.artnetIp1=(uint8_t)constrain(n,0,255);
    else if(k=="artnetIp2") cfg.artnetIp2=(uint8_t)constrain(n,0,255);
    else if(k=="artnetIp3") cfg.artnetIp3=(uint8_t)constrain(n,0,255);
    else if(k=="artnetIp4") cfg.artnetIp4=(uint8_t)constrain(n,0,255);
    else if(k=="artnetUniverse") cfg.artnetUniverse=(uint16_t)constrain(n,(long)ARTNET_UNIVERSE_MIN,(long)ARTNET_UNIVERSE_MAX);
    else if(k=="artnetSendIntervalMs") cfg.artnetSendIntervalMs=(uint16_t)constrain(n,(long)ARTNET_SEND_INTERVAL_MIN,(long)ARTNET_SEND_INTERVAL_MAX);
    else if(k=="wingHomePwm") cfg.wingHomePwm=(uint8_t)constrain(n,0,255);
    else if(k=="lipClosedAngle") cfg.lipClosedAngle=(uint8_t)constrain(n,0,180);
    else if(k=="lipOpenMin") cfg.lipOpenMin=(uint8_t)constrain(n,0,180);
    else if(k=="lipOpenMax") cfg.lipOpenMax=(uint8_t)constrain(n,0,180);
    else if(k=="wingRampMs") cfg.wingRampMs=(uint16_t)constrain(n,(long)WING_RAMP_MIN,(long)WING_RAMP_MAX);
    else if(setSceneField(cfg.sceneLight[LIGHT_IDLE],k,"idle",n)) {}
    else if(setSceneField(cfg.sceneLight[LIGHT_NORMAL],k,"normal",n)) {}
    else if(setSceneField(cfg.sceneLight[LIGHT_FIGHT],k,"fight",n)) {}
    else if(setSceneField(cfg.sceneLight[LIGHT_CELEBRATION],k,"celeb",n)) {}
    else if(setNanoSceneField(cfg.nanoScene[LIGHT_IDLE],k,"idle",n)) {}
    else if(setNanoSceneField(cfg.nanoScene[LIGHT_NORMAL],k,"normal",n)) {}
    else if(setNanoSceneField(cfg.nanoScene[LIGHT_FIGHT],k,"fight",n)) {}
    else if(setNanoSceneField(cfg.nanoScene[LIGHT_CELEBRATION],k,"celeb",n)) {}
  }
  saveConfig();
  server.sendHeader("Location","/settings",true);
  server.send(303,"text/plain","");
}

// Support browser GET for manual testing only. AI uses POST.
static void handleCommandQuery() {
  if(server.hasArg("scene")){
    String s=server.arg("scene"); s.toUpperCase();
    if(s=="IDLE")transitionTo(SHOW_IDLE,0);
    else if(s=="NORMAL")transitionTo(SHOW_NORMAL,0);
    else if(s=="FIGHT")transitionTo(SHOW_FIGHT,secMs(cfg.fightDurationSec));
    else if(s=="CELEBRATION")transitionTo(SHOW_CELEBRATION,secMs(cfg.celebrationDurationSec));
    else {server.send(400,"text/plain","bad scene");return;}
    server.sendHeader("Location","/");server.send(303,"text/plain","");
  } else server.send(400,"text/plain","missing scene");
}

static void setupRoutes() {
  server.collectHeaders("X-Garur-Token");
  server.on("/",HTTP_GET,handleRoot);
  server.on("/settings",HTTP_GET,handleSettings);
  server.on("/saveSettings",HTTP_POST,handleSaveSettings);
  server.on("/api/status",HTTP_GET,handleStatus);
  server.on("/api/fullStatus",HTTP_GET,handleFullStatus);
  server.on("/api/config",HTTP_GET,handleConfigGet);
  server.on("/api/config",HTTP_POST,handleConfigPost);
  server.on("/api/sensor",HTTP_POST,handleSensor);
  server.on("/api/sensorConfig",HTTP_POST,handleSensorConfig);
  server.on("/api/command",HTTP_POST,handleCommand);
  server.on("/api/scene",HTTP_POST,handleScene);
  server.on("/api/command",HTTP_GET,handleCommandQuery);
  server.begin();
}

static void setupArtNet() { udp.begin(6454); blackoutDMX(); currentLightScene=LIGHT_IDLE; lightSceneStartAt=millis(); }

static void setupNano() {
  nanoSerial.begin(9600);
  lastNanoRxAt=millis();
  sendNano(seqCounter++,"IDLE",0,0); currentSeq=seqCounter-1;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  loadConfig();
  // Always resync on boot, not just the first-ever-boot path inside
  // loadConfig()'s defaults()+saveConfig() branch -- a normal boot with
  // already-valid EEPROM config skips that branch entirely, and the Nano
  // (which just powered up fresh, or is a different physical board than
  // last time) needs to learn the Hub's stored values regardless of which
  // boot path this was.
  queueNanoCfgSync();
  setupWiFi();
  setupArtNet();
  setupNano();
  setupRoutes();
  currentState=SHOW_IDLE; stateStartedAt=millis(); stateRunStartedAt=stateStartedAt; stateDurationMs=0;
}

void loop() {
  maintainWiFi();
  server.handleClient();
  readNano();
  maintainNano();
  decideShow();
  processLight();
  sendArtNet();
  delay(2);
}
