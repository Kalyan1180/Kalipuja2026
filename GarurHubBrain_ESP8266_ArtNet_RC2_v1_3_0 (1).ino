/*
  Garur Show HUB Brain - ESP8266 - ArtNet Integrated

  READY TO UPLOAD SINGLE FILE

  Responsibilities:
  - Receives Entry + Exit tracker node packets over ESP-NOW
  - Decides Garur show state:
      IDLE
      NORMAL
      FIGHT
      CELEBRATION
      SHOWCASE: NORMAL -> FIGHT -> CELEBRATION -> NORMAL
      FAULT_SAFE
  - Sends scene commands to Arduino Nano over UART
  - Sends REAL ArtNet ArtDMX packets directly to ArtNet/DMX node
  - Provides web dashboard and configurable settings

  Board:
  - NodeMCU 1.0 ESP-12E Module

  ESP8266 <-> Nano UART:
  - ESP8266 D5 / GPIO14 TX  -> Nano RX / D0
  - Nano TX / D1             -> voltage divider -> ESP8266 D6 / GPIO12 RX
  - GND common

  ArtNet:
  - ArtNet node must be connected to the same Wi-Fi/channel as ESP-NOW.
  - Recommended: ArtNet node connects to this ESP8266 AP: GarurHub
  - Default ArtNet node IP: 192.168.4.50
  - ArtNet UDP port: 6454
  - Universe: configurable from web UI

  Fixture patch:
  - PAR 1: DMX 1-3    R,G,B
  - PAR 2: DMX 4-6    R,G,B
  - PAR 3: DMX 7-9    R,G,B
  - PAR 4: DMX 10-12  R,G,B
  - Relay 1: DMX 13   Suggested: Vishnu head light
  - Relay 2: DMX 14   Suggested: strobe/special effect

  Web:
  - Connect to Wi-Fi: GarurHub
  - Password: garur2026
  - Open: http://192.168.4.1
  - Settings: http://192.168.4.1/settings
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>

extern "C" {
  #include <espnow.h>
  #include <user_interface.h>
}

#define FW_VERSION "GARUR-HUB-ARTNET-1.3.0-RC2"

const uint8_t ESPNOW_CHANNEL = 6;
const char* HUB_AP_SSID     = "GarurHub";
const char* HUB_AP_PASSWORD = "garur2026";

const uint8_t NANO_RX_PIN = 12;  // D6 / GPIO12
const uint8_t NANO_TX_PIN = 14;  // D5 / GPIO14
SoftwareSerial nanoSerial(NANO_RX_PIN, NANO_TX_PIN);

ESP8266WebServer server(80);
WiFiUDP udp;

const uint32_t HUB_CONFIG_MAGIC = 0x20260704;
const int HUB_EEPROM_SIZE = 512;

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
  uint8_t artnetIp1;
  uint8_t artnetIp2;
  uint8_t artnetIp3;
  uint8_t artnetIp4;
  uint16_t artnetUniverse;
  uint16_t artnetSendIntervalMs;
  uint8_t idleBlue;
  uint8_t normalBlue;
  uint8_t normalAmber;
  uint8_t fightStrobeRelayEnabled;
};

HubConfig cfg;

unsigned long secToMs(uint16_t sec) { return (unsigned long)sec * 1000UL; }
IPAddress artnetNodeIp() { return IPAddress(cfg.artnetIp1, cfg.artnetIp2, cfg.artnetIp3, cfg.artnetIp4); }

void loadDefaultHubConfig() {
  cfg.magic = HUB_CONFIG_MAGIC;
  cfg.entryNodeId = 1;
  cfg.exitNodeId  = 2;
  cfg.sensorActiveHoldSec = 8;
  cfg.sensorStaleSec = 15;
  cfg.noPeopleIdleSec = 180;
  cfg.entryStandingNormalSec = 180;
  cfg.bothActiveShowcaseSec = 180;
  cfg.bothFightDebounceSec = 3;
  cfg.fightDurationSec = 40;
  cfg.celebrationDurationSec = 20;
  cfg.normalIntroSec = 20;
  cfg.normalRecoverySec = 60;
  cfg.fightCooldownSec = 90;
  cfg.showcaseRepeatCooldownSec = 90;
  cfg.nanoAckTimeoutMs = 1000;
  cfg.nanoLinkTimeoutSec = 15;
  cfg.nanoAutoRecover = 1;
  cfg.nanoRecoverGoodResponses = 3;
  cfg.startInMs = 300;
  cfg.artnetIp1 = 192;
  cfg.artnetIp2 = 168;
  cfg.artnetIp3 = 4;
  cfg.artnetIp4 = 50;
  cfg.artnetUniverse = 0;
  cfg.artnetSendIntervalMs = 25;
  cfg.idleBlue = 180;
  cfg.normalBlue = 160;
  cfg.normalAmber = 120;
  cfg.fightStrobeRelayEnabled = 1;
}

void validateHubConfig() {
  cfg.magic = HUB_CONFIG_MAGIC;
  if (cfg.entryNodeId < 1 || cfg.entryNodeId > 20) cfg.entryNodeId = 1;
  if (cfg.exitNodeId < 1 || cfg.exitNodeId > 20) cfg.exitNodeId = 2;
  if (cfg.entryNodeId == cfg.exitNodeId) cfg.exitNodeId = (cfg.entryNodeId == 1) ? 2 : 1;
  if (cfg.sensorActiveHoldSec < 2) cfg.sensorActiveHoldSec = 2;
  if (cfg.sensorActiveHoldSec > 60) cfg.sensorActiveHoldSec = 60;
  if (cfg.sensorStaleSec < 5) cfg.sensorStaleSec = 5;
  if (cfg.sensorStaleSec > 120) cfg.sensorStaleSec = 120;
  if (cfg.noPeopleIdleSec < 10) cfg.noPeopleIdleSec = 10;
  if (cfg.noPeopleIdleSec > 1800) cfg.noPeopleIdleSec = 1800;
  if (cfg.entryStandingNormalSec < 10) cfg.entryStandingNormalSec = 10;
  if (cfg.entryStandingNormalSec > 1800) cfg.entryStandingNormalSec = 1800;
  if (cfg.bothActiveShowcaseSec < 10) cfg.bothActiveShowcaseSec = 10;
  if (cfg.bothActiveShowcaseSec > 1800) cfg.bothActiveShowcaseSec = 1800;
  if (cfg.bothFightDebounceSec < 1) cfg.bothFightDebounceSec = 1;
  if (cfg.bothFightDebounceSec > 60) cfg.bothFightDebounceSec = 60;
  if (cfg.fightDurationSec < 5) cfg.fightDurationSec = 5;
  if (cfg.fightDurationSec > 300) cfg.fightDurationSec = 300;
  if (cfg.celebrationDurationSec < 5) cfg.celebrationDurationSec = 5;
  if (cfg.celebrationDurationSec > 300) cfg.celebrationDurationSec = 300;
  if (cfg.normalIntroSec < 5) cfg.normalIntroSec = 5;
  if (cfg.normalIntroSec > 300) cfg.normalIntroSec = 300;
  if (cfg.normalRecoverySec < 5) cfg.normalRecoverySec = 5;
  if (cfg.normalRecoverySec > 600) cfg.normalRecoverySec = 600;
  if (cfg.fightCooldownSec < 10) cfg.fightCooldownSec = 10;
  if (cfg.fightCooldownSec > 1800) cfg.fightCooldownSec = 1800;
  if (cfg.showcaseRepeatCooldownSec < 10) cfg.showcaseRepeatCooldownSec = 10;
  if (cfg.showcaseRepeatCooldownSec > 1800) cfg.showcaseRepeatCooldownSec = 1800;
  if (cfg.nanoAckTimeoutMs < 300) cfg.nanoAckTimeoutMs = 300;
  if (cfg.nanoAckTimeoutMs > 5000) cfg.nanoAckTimeoutMs = 5000;
  if (cfg.nanoLinkTimeoutSec < 5) cfg.nanoLinkTimeoutSec = 5;
  if (cfg.nanoLinkTimeoutSec > 120) cfg.nanoLinkTimeoutSec = 120;
  cfg.nanoAutoRecover = cfg.nanoAutoRecover ? 1 : 0;
  if (cfg.nanoRecoverGoodResponses < 1) cfg.nanoRecoverGoodResponses = 1;
  if (cfg.nanoRecoverGoodResponses > 10) cfg.nanoRecoverGoodResponses = 10;
  if (cfg.startInMs > 5000) cfg.startInMs = 5000;
  if (cfg.artnetIp1 < 1) cfg.artnetIp1 = 192;
  if (cfg.artnetIp4 < 1 || cfg.artnetIp4 > 254) cfg.artnetIp4 = 50;
  if (cfg.artnetUniverse > 32767) cfg.artnetUniverse = 0;
  if (cfg.artnetSendIntervalMs < 20) cfg.artnetSendIntervalMs = 20;
  if (cfg.artnetSendIntervalMs > 100) cfg.artnetSendIntervalMs = 100;
  cfg.fightStrobeRelayEnabled = cfg.fightStrobeRelayEnabled ? 1 : 0;
}

void saveHubConfig() {
  validateHubConfig();
  EEPROM.put(0, cfg);
  EEPROM.commit();
  Serial.println(F("[CFG] saved"));
}

void loadHubConfig() {
  EEPROM.begin(HUB_EEPROM_SIZE);
  EEPROM.get(0, cfg);
  if (cfg.magic != HUB_CONFIG_MAGIC) {
    Serial.println(F("[CFG] loading defaults"));
    loadDefaultHubConfig();
    saveHubConfig();
  } else validateHubConfig();
}

// This struct is intentionally EXACTLY the ESP-NOW SensorPacket used by
// Garur Servo Sensor Node v4.4.0-RC2. Do not add/remove/reorder fields
// unless the sensor node packet is changed too.
struct __attribute__((packed)) SensorPacketRC2 {
  uint8_t nodeId;
  uint8_t nodeRole;
  uint8_t eventType;
  uint8_t presence;
  uint8_t crowdLevel;
  uint8_t zoneMask;
  uint8_t targetAngle;
  uint16_t targetDistanceCm;
  uint8_t motionState;
  uint8_t confidence;
  uint8_t trackerState;
  uint8_t servoAttached;
  uint8_t activityScore;
  uint32_t eventId;
  uint32_t uptimeMs;
};

static_assert(sizeof(SensorPacketRC2) == 22, "RC2 SensorPacket must be 22 bytes");

enum SensorEventType : uint8_t {
  EVT_HEARTBEAT = 0,
  EVT_BOOT = 1,
  EVT_PRESENCE_DETECTED = 2,
  EVT_PRESENCE_CLEAR = 3,
  EVT_CROWD_UPDATE = 4,
  EVT_ZONE_UPDATE = 5,
  EVT_PATH_ACTIVITY = 6,
  EVT_PATH_BLOCKED = 7,
  EVT_NODE_ERROR = 8
};

struct TrackerRuntime {
  bool active = false;
  unsigned long activeSince = 0;
  unsigned long lastPacketAt = 0;
  unsigned long lastPresenceAt = 0;
  uint8_t lastPresence = 0;
  uint8_t lastConfidence = 0;
  uint8_t lastCrowd = 0;
  uint8_t lastZone = 0;
  uint8_t lastMotion = 0;
  uint8_t lastActivityScore = 0;
  uint8_t lastEventType = 0;
  bool nodeError = false;
  uint32_t nodeErrorCount = 0;
  uint32_t lastEventId = 0;
};

TrackerRuntime entryTracker;
TrackerRuntime exitTracker;

uint32_t rxPacketRC2Count = 0;
uint32_t rxPacketUnknownCount = 0;
uint8_t lastUnknownPacketLen = 0;

enum ShowState : uint8_t {
  SHOW_IDLE = 0,
  SHOW_NORMAL = 1,
  SHOW_FIGHT = 2,
  SHOW_CELEBRATION = 3,
  SHOW_SHOWCASE_NORMAL_INTRO = 10,
  SHOW_SHOWCASE_FIGHT = 11,
  SHOW_SHOWCASE_CELEBRATION = 12,
  SHOW_SHOWCASE_RECOVERY = 13,
  SHOW_FAULT_SAFE = 99
};

ShowState currentState = SHOW_IDLE;
unsigned long stateStartedAt = 0;
unsigned long stateRunStartedAt = 0;
unsigned long stateDurationMs = 0;
unsigned long noPeopleSince = 0;
unsigned long bothActiveSince = 0;
unsigned long entryOnlySince = 0;
unsigned long lastFightEndedAt = 0;
unsigned long lastShowcaseEndedAt = 0;
bool nanoFault = false;
char lastFault[40] = "";
uint8_t nanoHealthyResponses = 0;

uint32_t seqCounter = 1;
uint32_t currentSeq = 0;
bool waitingAck = false;
uint8_t cmdRetries = 0;
unsigned long lastCmdSentAt = 0;
char lastNanoScene[18] = "IDLE";
unsigned long lastNanoDuration = 0;
unsigned long lastNanoStartIn = 0;
unsigned long lastNanoRxAt = 0;
unsigned long lastNanoPingAt = 0;
uint32_t lastAckSeq = 0;
uint32_t lastDoneSeq = 0;

void setFaultReason(const char* reason) {
  nanoFault = true;
  nanoHealthyResponses = 0;
  strncpy(lastFault, reason ? reason : "FAULT", sizeof(lastFault) - 1);
  lastFault[sizeof(lastFault) - 1] = '\0';
}

void clearFaultReason() {
  nanoFault = false;
  nanoHealthyResponses = 0;
  lastFault[0] = '\0';
}

void markNanoHealthy() {
  lastNanoRxAt = millis();
  if (nanoFault && nanoHealthyResponses < 250) {
    nanoHealthyResponses++;
  }
}

void transitionTo(ShowState next, unsigned long durationMs);

#define DMX_CHANNELS 512
#define ARTNET_PORT 6454
uint8_t dmx[DMX_CHANNELS];
uint8_t artnetPacket[18 + DMX_CHANNELS];
uint8_t artSeq = 1;
unsigned long lastArtNetSendAt = 0;

enum LightScene : uint8_t {
  LIGHT_IDLE = 0,
  LIGHT_NORMAL = 1,
  LIGHT_FIGHT = 2,
  LIGHT_CELEBRATION = 3,
  LIGHT_BLACKOUT = 4
};

LightScene currentLightScene = LIGHT_IDLE;
LightScene pendingLightScene = LIGHT_IDLE;
bool lightPending = false;
unsigned long lightSceneStartAt = 0;
unsigned long lightSceneDurationMs = 0;
unsigned long lightPendingStartAt = 0;

void setPAR(uint8_t fixtureNum, uint8_t r, uint8_t g, uint8_t b) {
  if (fixtureNum < 1 || fixtureNum > 4) return;
  uint16_t base = (fixtureNum - 1) * 3;
  dmx[base + 0] = r;
  dmx[base + 1] = g;
  dmx[base + 2] = b;
}

void setAllPAR(uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t i = 1; i <= 4; i++) setPAR(i, r, g, b);
}

void setRelay(uint8_t relayNum, bool on) {
  if (relayNum < 1 || relayNum > 2) return;
  dmx[12 + (relayNum - 1)] = on ? 255 : 0;
}

void blackoutDMX() { memset(dmx, 0, DMX_CHANNELS); }

void buildArtDmxPacket() {
  memset(artnetPacket, 0, sizeof(artnetPacket));
  memcpy(artnetPacket, "Art-Net\0", 8);
  artnetPacket[8]  = 0x00;
  artnetPacket[9]  = 0x50;
  artnetPacket[10] = 0x00;
  artnetPacket[11] = 14;
  artnetPacket[12] = artSeq++;
  if (artSeq == 0) artSeq = 1;
  artnetPacket[13] = 0;
  artnetPacket[14] = cfg.artnetUniverse & 0xFF;
  artnetPacket[15] = (cfg.artnetUniverse >> 8) & 0x7F;
  artnetPacket[16] = (DMX_CHANNELS >> 8) & 0xFF;
  artnetPacket[17] = DMX_CHANNELS & 0xFF;
  memcpy(&artnetPacket[18], dmx, DMX_CHANNELS);
}

void sendArtNetFrame() {
  unsigned long now = millis();
  if (now - lastArtNetSendAt < cfg.artnetSendIntervalMs) return;
  lastArtNetSendAt = now;
  buildArtDmxPacket();
  udp.beginPacket(artnetNodeIp(), ARTNET_PORT);
  udp.write(artnetPacket, sizeof(artnetPacket));
  udp.endPacket();
}

void applyIdleLight() {
  blackoutDMX();
  setAllPAR(0, 0, cfg.idleBlue);
  setRelay(1, true);
  setRelay(2, false);
}

void applyNormalLight() {
  blackoutDMX();
  setPAR(1, 0, 0, cfg.normalBlue);
  setPAR(2, 255, cfg.normalAmber, 0);
  setPAR(3, 0, 0, cfg.normalBlue);
  setPAR(4, 255, cfg.normalAmber, 0);
  setRelay(1, true);
  setRelay(2, false);
}

void applyFightLight() {
  unsigned long t = millis() - lightSceneStartAt;
  uint8_t phase = (t / 120UL) % 4;
  blackoutDMX();
  setRelay(1, false);
  if (phase == 0) {
    setAllPAR(255, 255, 255);
    setRelay(2, cfg.fightStrobeRelayEnabled);
  } else if (phase == 1) {
    setAllPAR(255, 0, 0);
    setRelay(2, false);
  } else if (phase == 2) {
    setPAR(1, 255, 255, 255); setPAR(2, 255, 0, 0);
    setPAR(3, 255, 255, 255); setPAR(4, 255, 0, 0);
    setRelay(2, cfg.fightStrobeRelayEnabled);
  } else {
    setPAR(1, 255, 0, 0); setPAR(2, 255, 255, 255);
    setPAR(3, 255, 0, 0); setPAR(4, 255, 255, 255);
    setRelay(2, false);
  }
}

void applyCelebrationLight() {
  unsigned long t = millis() - lightSceneStartAt;
  blackoutDMX();
  setAllPAR(120, 70, 0);
  if (t < 6000UL) setRelay(1, ((t / 400UL) % 2 == 0));
  else setRelay(1, true);
  setRelay(2, false);
}

void activateLightSceneNow(LightScene scene, unsigned long durationMs) {
  currentLightScene = scene;
  lightSceneStartAt = millis();
  lightSceneDurationMs = durationMs;
  Serial.print(F("[LIGHT SCENE] "));
  Serial.print((int)scene);
  Serial.print(F(" duration="));
  Serial.println(durationMs);
}

void startLightScene(LightScene scene, unsigned long durationMs, unsigned long startInMs) {
  pendingLightScene = scene;
  lightSceneDurationMs = durationMs;
  lightPendingStartAt = millis() + startInMs;
  lightPending = true;
}

void processLightScene() {
  if (lightPending && millis() >= lightPendingStartAt) {
    lightPending = false;
    activateLightSceneNow(pendingLightScene, lightSceneDurationMs);
  }
  switch (currentLightScene) {
    case LIGHT_IDLE: applyIdleLight(); break;
    case LIGHT_NORMAL: applyNormalLight(); break;
    case LIGHT_FIGHT: applyFightLight(); break;
    case LIGHT_CELEBRATION: applyCelebrationLight(); break;
    case LIGHT_BLACKOUT: default: blackoutDMX(); break;
  }
}

const char* showStateText(ShowState s) {
  switch (s) {
    case SHOW_IDLE: return "IDLE";
    case SHOW_NORMAL: return "NORMAL";
    case SHOW_FIGHT: return "FIGHT";
    case SHOW_CELEBRATION: return "CELEBRATION";
    case SHOW_SHOWCASE_NORMAL_INTRO: return "SHOWCASE_NORMAL_INTRO";
    case SHOW_SHOWCASE_FIGHT: return "SHOWCASE_FIGHT";
    case SHOW_SHOWCASE_CELEBRATION: return "SHOWCASE_CELEBRATION";
    case SHOW_SHOWCASE_RECOVERY: return "SHOWCASE_RECOVERY";
    case SHOW_FAULT_SAFE: return "FAULT_SAFE";
    default: return "UNKNOWN";
  }
}

const char* lightSceneText(LightScene s) {
  switch (s) {
    case LIGHT_IDLE: return "IDLE";
    case LIGHT_NORMAL: return "NORMAL";
    case LIGHT_FIGHT: return "FIGHT";
    case LIGHT_CELEBRATION: return "CELEBRATION";
    case LIGHT_BLACKOUT: return "BLACKOUT";
    default: return "UNKNOWN";
  }
}

const char* actuatorSceneForState(ShowState s) {
  switch (s) {
    case SHOW_IDLE: return "IDLE";
    case SHOW_NORMAL: return "NORMAL";
    case SHOW_FIGHT: return "FIGHT";
    case SHOW_CELEBRATION: return "CELEBRATION";
    case SHOW_SHOWCASE_NORMAL_INTRO: return "NORMAL";
    case SHOW_SHOWCASE_FIGHT: return "FIGHT";
    case SHOW_SHOWCASE_CELEBRATION: return "CELEBRATION";
    case SHOW_SHOWCASE_RECOVERY: return "NORMAL";
    case SHOW_FAULT_SAFE: return "STOP";
    default: return "IDLE";
  }
}

LightScene lightSceneForState(ShowState s) {
  switch (s) {
    case SHOW_IDLE: return LIGHT_IDLE;
    case SHOW_NORMAL: return LIGHT_NORMAL;
    case SHOW_FIGHT: return LIGHT_FIGHT;
    case SHOW_CELEBRATION: return LIGHT_CELEBRATION;
    case SHOW_SHOWCASE_NORMAL_INTRO: return LIGHT_NORMAL;
    case SHOW_SHOWCASE_FIGHT: return LIGHT_FIGHT;
    case SHOW_SHOWCASE_CELEBRATION: return LIGHT_CELEBRATION;
    case SHOW_SHOWCASE_RECOVERY: return LIGHT_NORMAL;
    case SHOW_FAULT_SAFE: return LIGHT_IDLE;
    default: return LIGHT_IDLE;
  }
}

unsigned long stateElapsed() {
  if (stateDurationMs == 0) return 0;
  unsigned long now = millis();
  if (now < stateRunStartedAt) return 0;
  return now - stateRunStartedAt;
}

bool stateTimeFinished() {
  if (stateDurationMs == 0) return false;
  return stateElapsed() >= stateDurationMs;
}

void updateDerivedTrackerState(TrackerRuntime &t) {
  unsigned long now = millis();
  bool fresh = (t.lastPacketAt > 0 && now - t.lastPacketAt <= secToMs(cfg.sensorStaleSec));
  bool activeByHold = (t.lastPresenceAt > 0 && now - t.lastPresenceAt <= secToMs(cfg.sensorActiveHoldSec));
  bool newActive = fresh && activeByHold;
  if (newActive && !t.active) { t.active = true; t.activeSince = now; }
  else if (!newActive && t.active) { t.active = false; t.activeSince = 0; }
}

bool entryActive() { return entryTracker.active; }
bool exitActive() { return exitTracker.active; }

void sendNanoSceneCommand(uint32_t seq, const char* scene, unsigned long durationMs, unsigned long startInMs) {
  nanoSerial.print(F("CMD,")); nanoSerial.print(seq); nanoSerial.print(F(",SCENE,"));
  nanoSerial.print(scene); nanoSerial.print(F(",")); nanoSerial.print(durationMs); nanoSerial.print(F(","));
  nanoSerial.println(startInMs);
  Serial.print(F("[NANO TX] CMD,")); Serial.print(seq); Serial.print(F(",SCENE,"));
  Serial.print(scene); Serial.print(F(",")); Serial.print(durationMs); Serial.print(F(","));
  Serial.println(startInMs);
  strncpy(lastNanoScene, scene, sizeof(lastNanoScene) - 1);
  lastNanoScene[sizeof(lastNanoScene) - 1] = '\0';
  lastNanoDuration = durationMs;
  lastNanoStartIn = startInMs;
  waitingAck = true;
  cmdRetries = 0;
  lastCmdSentAt = millis();
}

void resendNanoCommand() {
  nanoSerial.print(F("CMD,")); nanoSerial.print(currentSeq); nanoSerial.print(F(",SCENE,"));
  nanoSerial.print(lastNanoScene); nanoSerial.print(F(",")); nanoSerial.print(lastNanoDuration);
  nanoSerial.print(F(",")); nanoSerial.println(lastNanoStartIn);
  lastCmdSentAt = millis();
  cmdRetries++;
}

void sendPingToNano() {
  unsigned long now = millis();
  if (now - lastNanoPingAt < 2000UL) return;
  lastNanoPingAt = now;
  nanoSerial.print(F("PING,")); nanoSerial.println(now);
}

void processNanoLine(char* line) {
  Serial.print(F("[NANO RX] ")); Serial.println(line);
  char* type = strtok(line, ",");
  if (!type) return;

  if (strcmp(type, "ACK") == 0) {
    char* s = strtok(NULL, ","); if (!s) return;
    uint32_t seq = strtoul(s, NULL, 10);
    lastAckSeq = seq;
    if (seq == currentSeq) waitingAck = false;
    markNanoHealthy();
  } else if (strcmp(type, "DONE") == 0) {
    char* s = strtok(NULL, ","); if (!s) return;
    lastDoneSeq = strtoul(s, NULL, 10);
    markNanoHealthy();
  } else if (strcmp(type, "FAULT") == 0) {
    strtok(NULL, ",");
    char* reason = strtok(NULL, ",");
    setFaultReason(reason ? reason : "NANO_FAULT");
    lastNanoRxAt = millis();
  } else if (strcmp(type, "PONG") == 0) {
    markNanoHealthy();
  }
}

void readNanoSerial() {
  static char line[100];
  static uint8_t pos = 0;
  while (nanoSerial.available()) {
    char c = nanoSerial.read();
    if (c == '\n') {
      line[pos] = '\0'; pos = 0;
      if (strlen(line) > 0) processNanoLine(line);
    } else if (c != '\r') {
      if (pos < sizeof(line) - 1) line[pos++] = c;
      else pos = 0;
    }
  }
}

void maintainNanoLink() {
  sendPingToNano();

  if (waitingAck && millis() - lastCmdSentAt > cfg.nanoAckTimeoutMs) {
    if (cmdRetries < 3) {
      resendNanoCommand();
    } else {
      waitingAck = false;
      setFaultReason("NANO_ACK_TIMEOUT");
    }
  }

  if (lastNanoRxAt > 0 && millis() - lastNanoRxAt > secToMs(cfg.nanoLinkTimeoutSec)) {
    if (!nanoFault) setFaultReason("NANO_LINK_TIMEOUT");
  }

  // Auto recover from a transient UART glitch if Nano is replying again.
  if (nanoFault && cfg.nanoAutoRecover && nanoHealthyResponses >= cfg.nanoRecoverGoodResponses) {
    clearFaultReason();
    waitingAck = false;
    if (currentState == SHOW_FAULT_SAFE) {
      transitionTo(SHOW_IDLE, 0);
    }
  }
}

void transitionTo(ShowState next, unsigned long durationMs = 0) {
  if (currentState == next && next != SHOW_FIGHT && next != SHOW_CELEBRATION &&
      next != SHOW_SHOWCASE_NORMAL_INTRO && next != SHOW_SHOWCASE_FIGHT &&
      next != SHOW_SHOWCASE_CELEBRATION && next != SHOW_SHOWCASE_RECOVERY) return;

  currentState = next;
  // stateStartedAt = command time. stateRunStartedAt = synchronized show start.
  // The timer measures only the actual scene duration, not startInMs.
  stateStartedAt = millis();
  stateRunStartedAt = stateStartedAt + (unsigned long)cfg.startInMs;
  stateDurationMs = durationMs;
  const char* actuatorScene = actuatorSceneForState(next);
  currentSeq = seqCounter++;
  Serial.print(F("[STATE] -> ")); Serial.print(showStateText(next));
  Serial.print(F(" | actuator=")); Serial.print(actuatorScene);
  Serial.print(F(" | light=")); Serial.print(lightSceneText(lightSceneForState(next)));
  Serial.print(F(" | duration=")); Serial.println(durationMs);
  sendNanoSceneCommand(currentSeq, actuatorScene, durationMs, cfg.startInMs);
  startLightScene(lightSceneForState(next), durationMs, cfg.startInMs);
}

void startShowcase() { transitionTo(SHOW_SHOWCASE_NORMAL_INTRO, secToMs(cfg.normalIntroSec)); }
void startFightSequence() { transitionTo(SHOW_FIGHT, secToMs(cfg.fightDurationSec)); }

void updatePresenceTimers() {
  bool e = entryActive(), x = exitActive();
  unsigned long now = millis();
  if (!e && !x) { if (noPeopleSince == 0) noPeopleSince = now; } else noPeopleSince = 0;
  if (e && x) { if (bothActiveSince == 0) bothActiveSince = now; } else bothActiveSince = 0;
  if (e && !x) { if (entryOnlySince == 0) entryOnlySince = now; } else entryOnlySince = 0;
}

void handleRunningTimedStates() {
  if (currentState == SHOW_FIGHT && stateTimeFinished()) {
    lastFightEndedAt = millis();
    transitionTo(SHOW_CELEBRATION, secToMs(cfg.celebrationDurationSec));
  } else if (currentState == SHOW_CELEBRATION && stateTimeFinished()) {
    transitionTo(SHOW_NORMAL, 0);
  } else if (currentState == SHOW_SHOWCASE_NORMAL_INTRO && stateTimeFinished()) {
    transitionTo(SHOW_SHOWCASE_FIGHT, secToMs(cfg.fightDurationSec));
  } else if (currentState == SHOW_SHOWCASE_FIGHT && stateTimeFinished()) {
    lastFightEndedAt = millis();
    transitionTo(SHOW_SHOWCASE_CELEBRATION, secToMs(cfg.celebrationDurationSec));
  } else if (currentState == SHOW_SHOWCASE_CELEBRATION && stateTimeFinished()) {
    transitionTo(SHOW_SHOWCASE_RECOVERY, secToMs(cfg.normalRecoverySec));
  } else if (currentState == SHOW_SHOWCASE_RECOVERY && stateTimeFinished()) {
    lastShowcaseEndedAt = millis();
    transitionTo(SHOW_NORMAL, 0);
  }
}

bool stateIsBusyShow() {
  return currentState == SHOW_FIGHT || currentState == SHOW_CELEBRATION ||
         currentState == SHOW_SHOWCASE_NORMAL_INTRO || currentState == SHOW_SHOWCASE_FIGHT ||
         currentState == SHOW_SHOWCASE_CELEBRATION || currentState == SHOW_SHOWCASE_RECOVERY;
}

void decideNextState() {
  updateDerivedTrackerState(entryTracker);
  updateDerivedTrackerState(exitTracker);
  updatePresenceTimers();
  handleRunningTimedStates();
  if (nanoFault) { if (currentState != SHOW_FAULT_SAFE) transitionTo(SHOW_FAULT_SAFE, 0); return; }
  if (stateIsBusyShow()) return;

  unsigned long now = millis();
  bool e = entryActive(), x = exitActive(), both = e && x;
  unsigned long bothDuration = bothActiveSince ? now - bothActiveSince : 0;
  unsigned long noPeopleDuration = noPeopleSince ? now - noPeopleSince : 0;
  unsigned long entryOnlyDuration = entryOnlySince ? now - entryOnlySince : 0;
  bool showcaseCooldownOk = (lastShowcaseEndedAt == 0 || now - lastShowcaseEndedAt >= secToMs(cfg.showcaseRepeatCooldownSec));
  bool fightCooldownOk = (lastFightEndedAt == 0 || now - lastFightEndedAt >= secToMs(cfg.fightCooldownSec));

  if (both && bothDuration >= secToMs(cfg.bothActiveShowcaseSec) && showcaseCooldownOk) { startShowcase(); return; }
  if (both && bothDuration >= secToMs(cfg.bothFightDebounceSec) && fightCooldownOk && showcaseCooldownOk) { startFightSequence(); return; }
  if (x) {
    if (currentState != SHOW_NORMAL) transitionTo(SHOW_NORMAL, 0);
    return;
  }

  if (e && !x && entryOnlyDuration >= secToMs(cfg.entryStandingNormalSec)) {
    if (currentState != SHOW_NORMAL) transitionTo(SHOW_NORMAL, 0);
    return;
  }

  if (!e && !x && noPeopleDuration >= secToMs(cfg.noPeopleIdleSec)) {
    if (currentState != SHOW_IDLE) transitionTo(SHOW_IDLE, 0);
    return;
  }
}

void updateTrackerFromPacket(const SensorPacketRC2 &p) {
  TrackerRuntime* t = nullptr;
  if (p.nodeId == cfg.entryNodeId) t = &entryTracker;
  else if (p.nodeId == cfg.exitNodeId) t = &exitTracker;
  else return;

  unsigned long now = millis();
  t->lastPacketAt = now;
  t->lastPresence = p.presence ? 1 : 0;
  t->lastConfidence = p.confidence;
  t->lastCrowd = p.crowdLevel;
  t->lastZone = p.zoneMask;
  t->lastMotion = p.motionState;
  t->lastActivityScore = p.activityScore;
  t->lastEventType = p.eventType;
  t->lastEventId = p.eventId;

  if (p.eventType == EVT_NODE_ERROR) {
    t->nodeError = true;
    t->nodeErrorCount++;
  } else if (p.eventType == EVT_BOOT) {
    // New boot from the sensor means previous sensor error is stale.
    t->nodeError = false;
  }

  if (p.presence) t->lastPresenceAt = now;
}

void onEspNowRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  // RC2 sensor node sends exactly SensorPacketRC2, 22 bytes, packed.
  if (len == sizeof(SensorPacketRC2)) {
    rxPacketRC2Count++;
    SensorPacketRC2 p;
    memcpy(&p, data, sizeof(p));
    updateTrackerFromPacket(p);
  } else {
    rxPacketUnknownCount++;
    lastUnknownPacketLen = len;
  }
}

void sendChunk(const __FlashStringHelper* s) {
  server.sendContent(String(s));
}

void sendChunk(const char* s) {
  server.sendContent(s);
}

void sendPageHeader(const char* title) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"));
  server.sendContent(F("<title>")); server.sendContent(title); server.sendContent(F("</title>"));
  server.sendContent(F("<style>body{font-family:Arial;background:#101010;color:#eee;padding:16px}"));
  server.sendContent(F(".card{background:#1f1f1f;padding:16px;border-radius:14px;margin-bottom:14px}"));
  server.sendContent(F("input{width:100%;padding:10px;margin:6px 0;border-radius:8px;border:0}"));
  server.sendContent(F("button,.btn{display:inline-block;background:#ffb300;color:#111;padding:10px 14px;border-radius:8px;text-decoration:none;font-weight:bold;margin:3px;border:0}"));
  server.sendContent(F(".ok{color:#00e676}.bad{color:#ff5252}.warn{color:#ffb300}small{color:#aaa}</style>"));
  server.sendContent(F("</head><body>"));
}

void sendPageEnd() {
  server.sendContent(F("</body></html>"));
  server.sendContent("");
}

void sendInput(const char* label, const char* name, unsigned long value,
               unsigned long minV, unsigned long maxV, const char* unit) {
  char buf[260];
  snprintf(buf, sizeof(buf),
           "<label>%s</label><input name='%s' type='number' min='%lu' max='%lu' value='%lu'><small>%s</small><br>",
           label, name, minV, maxV, value, unit);
  server.sendContent(buf);
}

void handleRoot() {
  sendPageHeader("Garur Hub");
  server.sendContent(F("<h2>Garur HUB Brain</h2>"));
  server.sendContent(F("<div class='card'><h3>Status</h3>"));
  server.sendContent(F("<p>Firmware: <b>")); server.sendContent(FW_VERSION); server.sendContent(F("</b></p>"));
  server.sendContent(F("<p>State: <b id='state'>-</b></p>"));
  server.sendContent(F("<p>Light: <b id='light'>-</b></p>"));
  server.sendContent(F("<p>Entry: <b id='entry'>-</b> | Conf: <b id='entryConf'>-</b> | Score: <b id='entryScore'>-</b></p>"));
  server.sendContent(F("<p>Exit: <b id='exit'>-</b> | Conf: <b id='exitConf'>-</b> | Score: <b id='exitScore'>-</b></p>"));
  server.sendContent(F("<p>Sensor Error: Entry <b id='entryErr'>-</b>, Exit <b id='exitErr'>-</b></p>"));
  server.sendContent(F("<p>ESP-NOW RX: RC2 <b id='rxRC2'>-</b>, Unknown <b id='rxUnknown'>-</b>, Last unknown len <b id='rxLen'>-</b></p>"));
  server.sendContent(F("<p>Nano: <b id='nano'>-</b> | Recovery count: <b id='nanoRec'>-</b></p>"));
  server.sendContent(F("<p>ArtNet Target: <b id='artnet'>-</b></p>"));
  server.sendContent(F("<p>Fault: <b id='fault'>-</b></p>"));
  server.sendContent(F("<p>Uptime: <b id='up'>-</b> sec</p></div>"));

  server.sendContent(F("<div class='card'><h3>Manual Test</h3>"));
  server.sendContent(F("<a class='btn' href='/force?scene=idle'>IDLE</a>"));
  server.sendContent(F("<a class='btn' href='/force?scene=normal'>NORMAL</a>"));
  server.sendContent(F("<a class='btn' href='/force?scene=fight'>FIGHT</a>"));
  server.sendContent(F("<a class='btn' href='/force?scene=celebration'>CELEBRATION</a>"));
  server.sendContent(F("<a class='btn' href='/force?scene=showcase'>SHOWCASE</a>"));
  server.sendContent(F("<a class='btn' href='/force?scene=blackout'>LIGHT BLACKOUT</a>"));
  server.sendContent(F("<a class='btn' href='/clearFault'>CLEAR FAULT</a>"));
  server.sendContent(F("<a class='btn' href='/settings'>SETTINGS</a></div>"));

  server.sendContent(F("<div class='card'><h3>Important</h3>"));
  server.sendContent(F("<p>This hub is matched to the v4.4.0-RC2 sensor packet: packed 22 bytes with activityScore.</p>"));
  server.sendContent(F("<p>The ArtNet node receives real DMX frames only. No text cue like LIGHT_FIGHT is sent.</p>"));
  server.sendContent(F("<p>Patch: PAR1 1-3, PAR2 4-6, PAR3 7-9, PAR4 10-12, Relay1 13, Relay2 14.</p></div>"));

  server.sendContent(F("<script>"));
  server.sendContent(F("function g(i){return document.getElementById(i)};"));
  server.sendContent(F("function u(){fetch('/api/status').then(r=>r.json()).then(j=>{"));
  server.sendContent(F("g('state').innerText=j.state;g('light').innerText=j.lightScene;"));
  server.sendContent(F("g('entry').innerText=j.entryActive?'ACTIVE':'clear';g('exit').innerText=j.exitActive?'ACTIVE':'clear';"));
  server.sendContent(F("g('entryConf').innerText=j.entryConfidence;g('exitConf').innerText=j.exitConfidence;"));
  server.sendContent(F("g('entryScore').innerText=j.entryActivityScore;g('exitScore').innerText=j.exitActivityScore;"));
  server.sendContent(F("g('entryErr').innerText=j.entryNodeError?'YES':'no';g('exitErr').innerText=j.exitNodeError?'YES':'no';"));
  server.sendContent(F("g('rxRC2').innerText=j.rxRC2;g('rxUnknown').innerText=j.rxUnknown;g('rxLen').innerText=j.lastUnknownLen;"));
  server.sendContent(F("g('nano').innerText=j.nanoOk?'OK':'CHECK';g('nanoRec').innerText=j.nanoHealthyResponses;"));
  server.sendContent(F("g('artnet').innerText=j.artnetIp+':6454 U'+j.artnetUniverse;g('fault').innerText=j.fault;"));
  server.sendContent(F("g('up').innerText=Math.floor(j.uptimeMs/1000);"));
  server.sendContent(F("}).finally(()=>setTimeout(u,1000));}u();"));
  server.sendContent(F("</script>"));
  sendPageEnd();
}

void handleStatus() {
  bool nanoOk = !nanoFault && (lastNanoRxAt == 0 || millis() - lastNanoRxAt < secToMs(cfg.nanoLinkTimeoutSec));

  char ip[16];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", cfg.artnetIp1, cfg.artnetIp2, cfg.artnetIp3, cfg.artnetIp4);

  static char json[1250];
  snprintf(json, sizeof(json),
    "{\"fw\":\"%s\"," 
    "\"state\":\"%s\"," 
    "\"lightScene\":\"%s\"," 
    "\"entryActive\":%s," 
    "\"exitActive\":%s," 
    "\"entryConfidence\":%u," 
    "\"exitConfidence\":%u," 
    "\"entryActivityScore\":%u," 
    "\"exitActivityScore\":%u," 
    "\"entryNodeError\":%s," 
    "\"exitNodeError\":%s," 
    "\"rxRC2\":%lu," 
    "\"rxUnknown\":%lu," 
    "\"lastUnknownLen\":%u," 
    "\"nanoOk\":%s," 
    "\"nanoHealthyResponses\":%u," 
    "\"fault\":\"%s\"," 
    "\"artnetIp\":\"%s\"," 
    "\"artnetUniverse\":%u," 
    "\"uptimeMs\":%lu}",
    FW_VERSION,
    showStateText(currentState),
    lightSceneText(currentLightScene),
    entryActive() ? "true" : "false",
    exitActive() ? "true" : "false",
    entryTracker.lastConfidence,
    exitTracker.lastConfidence,
    entryTracker.lastActivityScore,
    exitTracker.lastActivityScore,
    entryTracker.nodeError ? "true" : "false",
    exitTracker.nodeError ? "true" : "false",
    (unsigned long)rxPacketRC2Count,
    (unsigned long)rxPacketUnknownCount,
    lastUnknownPacketLen,
    nanoOk ? "true" : "false",
    nanoHealthyResponses,
    lastFault,
    ip,
    cfg.artnetUniverse,
    (unsigned long)millis()
  );

  server.send(200, "application/json", json);
}

void handleSettings() {
  sendPageHeader("Garur Hub Settings");
  server.sendContent(F("<h2>Garur Hub Settings</h2><form method='POST' action='/saveSettings'>"));

  server.sendContent(F("<div class='card'><h3>Tracker Nodes</h3>"));
  sendInput("Entry Node ID", "entryNodeId", cfg.entryNodeId, 1, 20, "ESP-NOW node id");
  sendInput("Exit Node ID", "exitNodeId", cfg.exitNodeId, 1, 20, "ESP-NOW node id");
  sendInput("Sensor Active Hold", "sensorActiveHoldSec", cfg.sensorActiveHoldSec, 2, 60, "seconds");
  sendInput("Sensor Stale Timeout", "sensorStaleSec", cfg.sensorStaleSec, 5, 120, "seconds");
  server.sendContent(F("</div>"));

  server.sendContent(F("<div class='card'><h3>Presence Rules</h3>"));
  sendInput("No People To Idle", "noPeopleIdleSec", cfg.noPeopleIdleSec, 10, 1800, "seconds");
  sendInput("Entry Standing Normal", "entryStandingNormalSec", cfg.entryStandingNormalSec, 10, 1800, "seconds");
  sendInput("Both Active Showcase Trigger", "bothActiveShowcaseSec", cfg.bothActiveShowcaseSec, 10, 1800, "seconds");
  sendInput("Both Active Fight Debounce", "bothFightDebounceSec", cfg.bothFightDebounceSec, 1, 60, "seconds");
  server.sendContent(F("</div>"));

  server.sendContent(F("<div class='card'><h3>Scene Durations</h3>"));
  sendInput("Normal Intro", "normalIntroSec", cfg.normalIntroSec, 5, 300, "seconds");
  sendInput("Fight Duration", "fightDurationSec", cfg.fightDurationSec, 5, 300, "seconds");
  sendInput("Celebration Duration", "celebrationDurationSec", cfg.celebrationDurationSec, 5, 300, "seconds");
  sendInput("Normal Recovery", "normalRecoverySec", cfg.normalRecoverySec, 5, 600, "seconds");
  server.sendContent(F("</div>"));

  server.sendContent(F("<div class='card'><h3>Cooldowns</h3>"));
  sendInput("Fight Cooldown", "fightCooldownSec", cfg.fightCooldownSec, 10, 1800, "seconds");
  sendInput("Showcase Repeat Cooldown", "showcaseRepeatCooldownSec", cfg.showcaseRepeatCooldownSec, 10, 1800, "seconds");
  server.sendContent(F("</div>"));

  server.sendContent(F("<div class='card'><h3>Nano Sync</h3>"));
  sendInput("Nano ACK Timeout", "nanoAckTimeoutMs", cfg.nanoAckTimeoutMs, 300, 5000, "milliseconds");
  sendInput("Nano Link Timeout", "nanoLinkTimeoutSec", cfg.nanoLinkTimeoutSec, 5, 120, "seconds");
  sendInput("Nano Auto Recover", "nanoAutoRecover", cfg.nanoAutoRecover, 0, 1, "0 off, 1 on");
  sendInput("Nano Recover Good Replies", "nanoRecoverGoodResponses", cfg.nanoRecoverGoodResponses, 1, 10, "PONG/ACK/DONE replies");
  sendInput("Scene Start Delay", "startInMs", cfg.startInMs, 0, 5000, "milliseconds");
  server.sendContent(F("</div>"));

  server.sendContent(F("<div class='card'><h3>ArtNet Node</h3>"));
  sendInput("ArtNet IP 1", "artnetIp1", cfg.artnetIp1, 1, 255, "example 192");
  sendInput("ArtNet IP 2", "artnetIp2", cfg.artnetIp2, 0, 255, "example 168");
  sendInput("ArtNet IP 3", "artnetIp3", cfg.artnetIp3, 0, 255, "example 4");
  sendInput("ArtNet IP 4", "artnetIp4", cfg.artnetIp4, 1, 254, "example 50");
  sendInput("ArtNet Universe", "artnetUniverse", cfg.artnetUniverse, 0, 32767, "usually 0");
  sendInput("ArtNet Send Interval", "artnetSendIntervalMs", cfg.artnetSendIntervalMs, 20, 100, "ms, 25 = 40fps");
  server.sendContent(F("</div>"));

  server.sendContent(F("<div class='card'><h3>Light Tuning</h3>"));
  sendInput("Idle Blue Brightness", "idleBlue", cfg.idleBlue, 0, 255, "0-255");
  sendInput("Normal Blue Brightness", "normalBlue", cfg.normalBlue, 0, 255, "0-255");
  sendInput("Normal Amber Green Component", "normalAmber", cfg.normalAmber, 0, 255, "0-255");
  sendInput("Fight Relay 2 Strobe Enable", "fightStrobeRelayEnabled", cfg.fightStrobeRelayEnabled, 0, 1, "0 off, 1 on");
  server.sendContent(F("</div>"));

  server.sendContent(F("<button type='submit'>Save Settings</button><a class='btn' href='/'>Back</a></form>"));
  sendPageEnd();
}

void handleSaveSettings() {
  if (server.hasArg("entryNodeId")) cfg.entryNodeId = server.arg("entryNodeId").toInt();
  if (server.hasArg("exitNodeId")) cfg.exitNodeId = server.arg("exitNodeId").toInt();
  if (server.hasArg("sensorActiveHoldSec")) cfg.sensorActiveHoldSec = server.arg("sensorActiveHoldSec").toInt();
  if (server.hasArg("sensorStaleSec")) cfg.sensorStaleSec = server.arg("sensorStaleSec").toInt();
  if (server.hasArg("noPeopleIdleSec")) cfg.noPeopleIdleSec = server.arg("noPeopleIdleSec").toInt();
  if (server.hasArg("entryStandingNormalSec")) cfg.entryStandingNormalSec = server.arg("entryStandingNormalSec").toInt();
  if (server.hasArg("bothActiveShowcaseSec")) cfg.bothActiveShowcaseSec = server.arg("bothActiveShowcaseSec").toInt();
  if (server.hasArg("bothFightDebounceSec")) cfg.bothFightDebounceSec = server.arg("bothFightDebounceSec").toInt();
  if (server.hasArg("fightDurationSec")) cfg.fightDurationSec = server.arg("fightDurationSec").toInt();
  if (server.hasArg("celebrationDurationSec")) cfg.celebrationDurationSec = server.arg("celebrationDurationSec").toInt();
  if (server.hasArg("normalIntroSec")) cfg.normalIntroSec = server.arg("normalIntroSec").toInt();
  if (server.hasArg("normalRecoverySec")) cfg.normalRecoverySec = server.arg("normalRecoverySec").toInt();
  if (server.hasArg("fightCooldownSec")) cfg.fightCooldownSec = server.arg("fightCooldownSec").toInt();
  if (server.hasArg("showcaseRepeatCooldownSec")) cfg.showcaseRepeatCooldownSec = server.arg("showcaseRepeatCooldownSec").toInt();
  if (server.hasArg("nanoAckTimeoutMs")) cfg.nanoAckTimeoutMs = server.arg("nanoAckTimeoutMs").toInt();
  if (server.hasArg("nanoLinkTimeoutSec")) cfg.nanoLinkTimeoutSec = server.arg("nanoLinkTimeoutSec").toInt();
  if (server.hasArg("nanoAutoRecover")) cfg.nanoAutoRecover = server.arg("nanoAutoRecover").toInt();
  if (server.hasArg("nanoRecoverGoodResponses")) cfg.nanoRecoverGoodResponses = server.arg("nanoRecoverGoodResponses").toInt();
  if (server.hasArg("startInMs")) cfg.startInMs = server.arg("startInMs").toInt();
  if (server.hasArg("artnetIp1")) cfg.artnetIp1 = server.arg("artnetIp1").toInt();
  if (server.hasArg("artnetIp2")) cfg.artnetIp2 = server.arg("artnetIp2").toInt();
  if (server.hasArg("artnetIp3")) cfg.artnetIp3 = server.arg("artnetIp3").toInt();
  if (server.hasArg("artnetIp4")) cfg.artnetIp4 = server.arg("artnetIp4").toInt();
  if (server.hasArg("artnetUniverse")) cfg.artnetUniverse = server.arg("artnetUniverse").toInt();
  if (server.hasArg("artnetSendIntervalMs")) cfg.artnetSendIntervalMs = server.arg("artnetSendIntervalMs").toInt();
  if (server.hasArg("idleBlue")) cfg.idleBlue = server.arg("idleBlue").toInt();
  if (server.hasArg("normalBlue")) cfg.normalBlue = server.arg("normalBlue").toInt();
  if (server.hasArg("normalAmber")) cfg.normalAmber = server.arg("normalAmber").toInt();
  if (server.hasArg("fightStrobeRelayEnabled")) cfg.fightStrobeRelayEnabled = server.arg("fightStrobeRelayEnabled").toInt();
  saveHubConfig();
  server.sendHeader("Location", "/settings", true); server.send(303, "text/plain", "");
}

void handleForce() {
  if (!server.hasArg("scene")) { server.send(400, "text/plain", "Missing scene"); return; }
  String s = server.arg("scene"); s.toLowerCase();
  if (s == "idle") transitionTo(SHOW_IDLE, 0);
  else if (s == "normal") transitionTo(SHOW_NORMAL, 0);
  else if (s == "fight") transitionTo(SHOW_FIGHT, secToMs(cfg.fightDurationSec));
  else if (s == "celebration") transitionTo(SHOW_CELEBRATION, secToMs(cfg.celebrationDurationSec));
  else if (s == "showcase") startShowcase();
  else if (s == "blackout") activateLightSceneNow(LIGHT_BLACKOUT, 0);
  else { server.send(400, "text/plain", "Unknown scene"); return; }
  server.sendHeader("Location", "/", true); server.send(303, "text/plain", "");
}

void handleClearFault() {
  clearFaultReason();
  transitionTo(SHOW_IDLE, 0);
  server.sendHeader("Location", "/", true); server.send(303, "text/plain", "");
}

void setupWiFiAndEspNow() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  bool apOk = WiFi.softAP(HUB_AP_SSID, HUB_AP_PASSWORD, ESPNOW_CHANNEL, false, 4);
  wifi_set_channel(ESPNOW_CHANNEL);
  Serial.print(F("[WIFI] AP ")); Serial.println(apOk ? F("started") : F("failed"));
  Serial.print(F("[WIFI] AP IP: ")); Serial.println(WiFi.softAPIP());
  Serial.print(F("[WIFI] Channel: ")); Serial.println(ESPNOW_CHANNEL);
  udp.begin(6453);
  if (esp_now_init() != 0) { Serial.println(F("[ESPNOW] init failed")); return; }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onEspNowRecv);
  Serial.println(F("[ESPNOW] receiver ready"));
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/force", HTTP_GET, handleForce);
  server.on("/clearFault", HTTP_GET, handleClearFault);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/saveSettings", HTTP_POST, handleSaveSettings);
  server.begin();
  Serial.println(F("[WEB] started"));
}

void setup() {
  Serial.begin(115200);
  delay(700);
  Serial.println();
  Serial.println(F("Garur HUB Brain ArtNet Integrated starting..."));
  loadHubConfig();
  nanoSerial.begin(9600);
  setupWiFiAndEspNow();
  setupWeb();
  lastNanoRxAt = millis();
  blackoutDMX();
  activateLightSceneNow(LIGHT_IDLE, 0);
  transitionTo(SHOW_IDLE, 0);
}

void loop() {
  server.handleClient();
  readNanoSerial();
  maintainNanoLink();
  decideNextState();
  processLightScene();
  sendArtNetFrame();
  delay(2);
}
