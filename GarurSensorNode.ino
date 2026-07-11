/*
  Garur Servo Sensor Node - RELEASE CANDIDATE 2 (v4.4.0)
  ESP8266 + HC-SR04 + Custom Servo Pulse + ESP-NOW + Web UI + OTA
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>

extern "C" {
  #include <espnow.h>
  #include <user_interface.h>
}

#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include "WebOTA.h"

/* ---------------- VERSION ---------------- */

#define FW_VERSION "4.4.0-RC2"

/* ---------------- PIN CONFIG ---------------- */

const uint8_t TRIG_PIN = 14;        // D5 / GPIO14
const uint8_t ECHO_PIN = 12;        // D6 / GPIO12 through voltage divider
const uint8_t SERVO_PIN = 13;       // D7 / GPIO13, custom servo signal
const uint8_t HUB_LED_PIN = 5;      // D1 / GPIO5
const uint8_t DETECT_LED_PIN = 4;   // D2 / GPIO4

/* ---------------- ESP-NOW CONFIG ---------------- */

const uint8_t ESPNOW_CHANNEL = 6;
uint8_t HUB_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Replace with real HUB MAC
const char* AP_PASSWORD = "garur2026";
const char* OTA_USER     = "admin";
const char* OTA_PASSWORD = "garur2026ota";

/* ---------------- CONSTANTS ---------------- */

const uint8_t MAX_SCAN_POINTS = 19;
const unsigned long HUB_STABLE_WINDOW_MS = 7000UL;
const unsigned long HUB_DISCONNECT_MS = 15000UL;
const unsigned long SERVO_IDLE_DETACH_MS = 900UL;
const unsigned long SERVO_SETTLE_MS = 140UL;
const unsigned long SERVO_PULSE_PERIOD_US = 20000UL;
const uint8_t CONF_ACTIVE = 50;
const uint8_t CONF_STRONG = 70;
const uint16_t DEFAULT_BASELINE_CM = 400;
const unsigned long SENSOR_FAULT_MS = 60000UL; // 60 seconds

/* ---------------- ENUMS ---------------- */

enum NodeRole : uint8_t { ROLE_EXIT_PATH = 1, ROLE_THEME_FRONT = 2 };
enum EventType : uint8_t {
  EVT_HEARTBEAT = 0, EVT_BOOT = 1, EVT_PRESENCE_DETECTED = 2, EVT_PRESENCE_CLEAR = 3,
  EVT_CROWD_UPDATE = 4, EVT_ZONE_UPDATE = 5, EVT_PATH_ACTIVITY = 6,
  EVT_PATH_BLOCKED = 7, EVT_NODE_ERROR = 8
};
enum CrowdLevel : uint8_t { CROWD_NONE = 0, CROWD_LOW = 1, CROWD_MEDIUM = 2, CROWD_HIGH = 3 };
enum MotionState : uint8_t {
  MOTION_NONE = 0, MOTION_STANDING = 1, MOTION_MOVING_LEFT = 2, MOTION_MOVING_RIGHT = 3,
  MOTION_APPROACHING = 4, MOTION_LEAVING = 5, MOTION_UNKNOWN = 6
};
enum TrackerState : uint8_t {
  STATE_IDLE = 0, STATE_QUICK_SCAN = 1, STATE_SEARCH = 2, STATE_TRACK_ZONE = 3,
  STATE_CROWD_SCAN = 4, STATE_LOST_TARGET = 5
};

ESP8266WebServer server(80);

/* ---------------- SETTINGS ---------------- */

const uint32_t SETTINGS_MAGIC = 0x20260628; // Incremented for RC2 clean boot
const int EEPROM_SIZE = 300;

struct Settings {
  uint32_t magic; uint8_t nodeId; uint8_t nodeRole; uint8_t servoEnabled;
  uint16_t detectCm; uint16_t clearCm; uint16_t detectStableMs; uint16_t clearStableMs;
  uint16_t sampleMs; uint8_t servoStartAngle; uint8_t servoEndAngle; uint8_t servoStepDeg;
  uint16_t servoSpeedMs; uint16_t servoIdleSec; uint16_t miniScanIntervalSec;
  uint8_t trackingWindowDeg; uint16_t fullRescanIntervalSec; uint8_t mediumAngleCount;
  uint8_t highAngleCount; uint16_t standingTimeSec; uint16_t blockedTimeSec;
  uint16_t baselineDeltaCm; uint16_t heartbeatSec; uint16_t crowdUpdateSec;
};
Settings cfg;

/* ---------------- ESP-NOW PACKET ---------------- */

struct __attribute__((packed)) SensorPacket {
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

/* ---------------- FORWARD DECLARATIONS ---------------- */

void queueEvent(uint8_t eventType, uint8_t repeats = 3);
void sendPacketNow(uint8_t eventType, uint32_t eventId);
String htmlHeader(const String &title);
String roleText(); String crowdText(); String zoneText(); String motionText();
String stateText(); String getHubLedStateText(); String getDetectLedStateText();
void startSearch(); void enterIdleState(); void startQuickScan(); void startLostTarget();
void startCrowdScan(); void startTrackZone(); void setServoTarget(uint8_t angle);
void detachServoIfNeeded();

/* ---------------- RUNTIME MAP & STATE ---------------- */

uint8_t scanAngles[MAX_SCAN_POINTS];
uint8_t confidenceMap[MAX_SCAN_POINTS];
uint16_t distanceMap[MAX_SCAN_POINTS];
uint16_t baselineMap[MAX_SCAN_POINTS];
uint8_t baselineValid[MAX_SCAN_POINTS];
uint32_t lastSeenMap[MAX_SCAN_POINTS];

uint8_t scanCount = 0; uint8_t centerIndex = 0; uint8_t currentScanIndex = 0;
int8_t scanDirection = 1; uint8_t scanMinIndex = 0; uint8_t scanMaxIndex = 0;

TrackerState trackerState = STATE_IDLE;
bool servoSignalAttached = false; bool servoMoving = false; bool servoIdleMovePending = false;
int currentServoAngle = 90; int targetServoAngle = 90;
unsigned long lastServoStepAt = 0; unsigned long servoReachedAt = 0;
unsigned long servoIdleDetachAt = 0; unsigned long lastServoPulseAtUs = 0;

long lastDistanceCm = -1; bool detected = false;
bool sensorFault = false; unsigned long lastGoodReadingAt = 0;

long prevVelocityDistanceCm = -1; unsigned long prevVelocityAt = 0; int currentVelocityCmS = 0; 

float presenceScore = 0.0; uint8_t activityScore = 0;
uint32_t statTotalDetections = 0; uint32_t statZoneHits[3] = {0,0,0};
unsigned long statLastEventUptime = 0;

unsigned long lastPeopleSeenAt = 0; unsigned long presenceStartedAt = 0;
unsigned long lastMiniScanAt = 0; unsigned long lastFullRescanAt = 0;
unsigned long lastCrowdUpdateAt = 0;

uint8_t activeCount = 0; uint8_t zoneMask = 0; uint8_t strongestIndex = 0;
uint8_t strongestConfidence = 0; uint8_t crowdLevel = CROWD_NONE; uint8_t motionState = MOTION_NONE;

uint16_t targetDistanceCm = 0; uint8_t targetAngle = 90;
int prevMotionAngle = -1; unsigned long lastMotionSampleAt = 0;

uint32_t triggerCount = 0; uint32_t eventSeq = 1;
bool lastSendOk = false; uint8_t queuedEventType = EVT_HEARTBEAT;
uint8_t queuedRepeats = 0; uint32_t queuedEventId = 0;
unsigned long nextSendAt = 0; unsigned long lastHeartbeatAt = 0;
unsigned long lastSuccessfulSendAt = 0; unsigned long lastSendResultAt = 0; 
uint8_t consecutiveSendFail = 0; uint8_t consecutiveSendOk = 0;

/* ---------------- CORE SETTINGS ---------------- */

void loadDefaults() {
  cfg.magic = SETTINGS_MAGIC; cfg.nodeId = 1; cfg.nodeRole = ROLE_THEME_FRONT; cfg.servoEnabled = 1;
  cfg.detectCm = 120; cfg.clearCm = 170; cfg.detectStableMs = 350; cfg.clearStableMs = 1800; cfg.sampleMs = 140;
  cfg.servoStartAngle = 50; cfg.servoEndAngle = 130; cfg.servoStepDeg = 10; cfg.servoSpeedMs = 25; cfg.servoIdleSec = 20;
  cfg.miniScanIntervalSec = 5; cfg.trackingWindowDeg = 25; cfg.fullRescanIntervalSec = 5;
  cfg.mediumAngleCount = 3; cfg.highAngleCount = 6; cfg.standingTimeSec = 10; cfg.blockedTimeSec = 12;
  cfg.baselineDeltaCm = 35; cfg.heartbeatSec = 5; cfg.crowdUpdateSec = 2;
}

void validateSettings() {
  cfg.magic = SETTINGS_MAGIC;
  if (cfg.nodeId < 1 || cfg.nodeId > 20) cfg.nodeId = 1;
  if (cfg.nodeRole != ROLE_EXIT_PATH && cfg.nodeRole != ROLE_THEME_FRONT) cfg.nodeRole = ROLE_THEME_FRONT;
  cfg.servoEnabled = cfg.servoEnabled ? 1 : 0;
  if (cfg.detectCm < 20) cfg.detectCm = 20; if (cfg.detectCm > 350) cfg.detectCm = 350;
  if (cfg.clearCm <= cfg.detectCm) cfg.clearCm = cfg.detectCm + 30; if (cfg.clearCm > 400) cfg.clearCm = 400;
  if (cfg.detectStableMs < 100) cfg.detectStableMs = 100; if (cfg.detectStableMs > 5000) cfg.detectStableMs = 5000;
  if (cfg.clearStableMs < 500) cfg.clearStableMs = 500; if (cfg.clearStableMs > 10000) cfg.clearStableMs = 10000;
  if (cfg.sampleMs < 100) cfg.sampleMs = 100; if (cfg.sampleMs > 1000) cfg.sampleMs = 1000;
  if (cfg.servoStartAngle > 170) cfg.servoStartAngle = 170; if (cfg.servoEndAngle > 180) cfg.servoEndAngle = 180;
  if (cfg.servoEndAngle <= cfg.servoStartAngle) { cfg.servoEndAngle = cfg.servoStartAngle + 10; if (cfg.servoEndAngle > 180) { cfg.servoEndAngle = 180; cfg.servoStartAngle = 170; } }
  if (cfg.servoStepDeg < 5) cfg.servoStepDeg = 5; if (cfg.servoStepDeg > 30) cfg.servoStepDeg = 30;
  if (cfg.servoSpeedMs < 5) cfg.servoSpeedMs = 5; if (cfg.servoSpeedMs > 300) cfg.servoSpeedMs = 300;
  if (cfg.servoIdleSec < 5) cfg.servoIdleSec = 5; if (cfg.servoIdleSec > 600) cfg.servoIdleSec = 600;
  if (cfg.miniScanIntervalSec < 2) cfg.miniScanIntervalSec = 2; if (cfg.miniScanIntervalSec > 120) cfg.miniScanIntervalSec = 120;
  if (cfg.trackingWindowDeg < 10) cfg.trackingWindowDeg = 10; if (cfg.trackingWindowDeg > 80) cfg.trackingWindowDeg = 80;
  if (cfg.fullRescanIntervalSec < 3) cfg.fullRescanIntervalSec = 3; if (cfg.fullRescanIntervalSec > 120) cfg.fullRescanIntervalSec = 120;
  if (cfg.mediumAngleCount < 2) cfg.mediumAngleCount = 2; if (cfg.mediumAngleCount > MAX_SCAN_POINTS) cfg.mediumAngleCount = MAX_SCAN_POINTS;
  if (cfg.highAngleCount < cfg.mediumAngleCount) cfg.highAngleCount = cfg.mediumAngleCount + 1; if (cfg.highAngleCount > MAX_SCAN_POINTS) cfg.highAngleCount = MAX_SCAN_POINTS;
  if (cfg.standingTimeSec < 3) cfg.standingTimeSec = 3; if (cfg.standingTimeSec > 300) cfg.standingTimeSec = 300;
  if (cfg.blockedTimeSec < 5) cfg.blockedTimeSec = 5; if (cfg.blockedTimeSec > 300) cfg.blockedTimeSec = 300;
  if (cfg.baselineDeltaCm < 10) cfg.baselineDeltaCm = 10; if (cfg.baselineDeltaCm > 150) cfg.baselineDeltaCm = 150;
  if (cfg.heartbeatSec < 2) cfg.heartbeatSec = 2; if (cfg.heartbeatSec > 60) cfg.heartbeatSec = 60;
  if (cfg.crowdUpdateSec < 1) cfg.crowdUpdateSec = 1; if (cfg.crowdUpdateSec > 30) cfg.crowdUpdateSec = 30;
}

void saveSettings() { validateSettings(); EEPROM.put(0, cfg); EEPROM.commit(); }
void loadSettings() { EEPROM.begin(EEPROM_SIZE); EEPROM.get(0, cfg); if (cfg.magic != SETTINGS_MAGIC) { loadDefaults(); saveSettings(); } else validateSettings(); }

void rebuildScanAngles() {
  scanCount = 0;
  for (uint16_t a = cfg.servoStartAngle; a <= cfg.servoEndAngle && scanCount < MAX_SCAN_POINTS; a += cfg.servoStepDeg) scanAngles[scanCount++] = (uint8_t)a;
  if (scanCount == 0) { scanAngles[0] = 90; scanCount = 1; }
  if (scanAngles[scanCount - 1] != cfg.servoEndAngle && scanCount < MAX_SCAN_POINTS) scanAngles[scanCount++] = cfg.servoEndAngle;
  centerIndex = scanCount / 2; scanMinIndex = 0; scanMaxIndex = scanCount - 1;
  currentScanIndex = centerIndex; strongestIndex = centerIndex;
  for (uint8_t i = 0; i < MAX_SCAN_POINTS; i++) { confidenceMap[i] = 0; distanceMap[i] = 0; baselineMap[i] = 0; baselineValid[i] = 0; lastSeenMap[i] = 0; }
  currentServoAngle = scanAngles[centerIndex]; targetServoAngle = currentServoAngle; targetAngle = currentServoAngle;
}

/* ---------------- SERVO DRIVER ---------------- */

uint16_t angleToPulseUs(int angle) { if (angle < 0) angle = 0; if (angle > 180) angle = 180; return 500 + ((uint32_t)angle * 1900UL) / 180UL; }

void attachServoIfNeeded() {
  if (!cfg.servoEnabled) return;
  if (!servoSignalAttached) { pinMode(SERVO_PIN, OUTPUT); digitalWrite(SERVO_PIN, LOW); servoSignalAttached = true; lastServoPulseAtUs = micros(); }
}

void detachServoIfNeeded() { if (servoSignalAttached) { digitalWrite(SERVO_PIN, LOW); servoSignalAttached = false; } }

void processServoPulse() {
  if (!cfg.servoEnabled || !servoSignalAttached) return;
  unsigned long nowUs = micros();
  if ((unsigned long)(nowUs - lastServoPulseAtUs) >= SERVO_PULSE_PERIOD_US) {
    lastServoPulseAtUs = nowUs;
    uint16_t pulseUs = angleToPulseUs(currentServoAngle);
    digitalWrite(SERVO_PIN, HIGH); delayMicroseconds(pulseUs); digitalWrite(SERVO_PIN, LOW);
  }
}

uint8_t getCenterAngle() { return scanAngles[centerIndex]; }

void setServoTarget(uint8_t angle) {
  if (angle < cfg.servoStartAngle) angle = cfg.servoStartAngle;
  if (angle > cfg.servoEndAngle) angle = cfg.servoEndAngle;
  targetServoAngle = angle;
  if (!cfg.servoEnabled) { currentServoAngle = targetServoAngle; servoMoving = false; servoReachedAt = millis(); return; }
  attachServoIfNeeded();
  if (currentServoAngle != targetServoAngle) servoMoving = true; else { servoMoving = false; servoReachedAt = millis(); }
}

void processServoMove() {
  unsigned long now = millis();
  if (!cfg.servoEnabled || !servoMoving) return;
  if (now - lastServoStepAt < cfg.servoSpeedMs) return;
  lastServoStepAt = now;
  if (currentServoAngle < targetServoAngle) currentServoAngle++;
  else if (currentServoAngle > targetServoAngle) currentServoAngle--;
  if (currentServoAngle == targetServoAngle) { servoMoving = false; servoReachedAt = now; }
}

/* ---------------- TRACKER STATE MGMT ---------------- */

void enterIdleState() {
  trackerState = STATE_IDLE; scanMinIndex = centerIndex; scanMaxIndex = centerIndex; currentScanIndex = centerIndex;
  setServoTarget(getCenterAngle());
  if (cfg.servoEnabled) { servoIdleMovePending = true; servoIdleDetachAt = millis() + SERVO_IDLE_DETACH_MS; }
}

void startQuickScan() {
  trackerState = STATE_QUICK_SCAN; scanMinIndex = 0; scanMaxIndex = scanCount - 1;
  currentScanIndex = 0; scanDirection = 1; setServoTarget(scanAngles[currentScanIndex]);
}

void startSearch() {
  trackerState = STATE_SEARCH; scanMinIndex = 0; scanMaxIndex = scanCount - 1;
  currentScanIndex = 0; scanDirection = 1; lastFullRescanAt = millis(); setServoTarget(scanAngles[currentScanIndex]);
}

void setTrackWindowAround(uint8_t angle, bool widerForPathNode) {
  uint8_t windowDeg = cfg.trackingWindowDeg;
  if (widerForPathNode) { windowDeg += 20; if (windowDeg > 90) windowDeg = 90; }
  uint8_t minA = (angle > windowDeg) ? angle - windowDeg : cfg.servoStartAngle;
  uint8_t maxA = angle + windowDeg; if (maxA > cfg.servoEndAngle) maxA = cfg.servoEndAngle;
  scanMinIndex = 0; scanMaxIndex = scanCount - 1;
  for (uint8_t i = 0; i < scanCount; i++) if (scanAngles[i] >= minA) { scanMinIndex = i; break; }
  for (int i = scanCount - 1; i >= 0; i--) if (scanAngles[i] <= maxA) { scanMaxIndex = i; break; }
  if (scanMaxIndex < scanMinIndex) { scanMinIndex = 0; scanMaxIndex = scanCount - 1; }
  if (currentScanIndex < scanMinIndex || currentScanIndex > scanMaxIndex) { currentScanIndex = scanMinIndex; scanDirection = 1; }
}

void startTrackZone() { trackerState = STATE_TRACK_ZONE; setTrackWindowAround(scanAngles[strongestIndex], cfg.nodeRole == ROLE_EXIT_PATH); setServoTarget(scanAngles[currentScanIndex]); }

void startCrowdScan() {
  trackerState = STATE_CROWD_SCAN;
  uint8_t minActive = scanCount - 1; uint8_t maxActive = 0; bool any = false;
  for (uint8_t i = 0; i < scanCount; i++) { if (confidenceMap[i] >= CONF_ACTIVE) { if (i < minActive) minActive = i; if (i > maxActive) maxActive = i; any = true; } }
  if (!any) { startSearch(); return; }
  if (minActive > 0) minActive--; if (maxActive < scanCount - 1) maxActive++;
  scanMinIndex = minActive; scanMaxIndex = maxActive;
  if (currentScanIndex < scanMinIndex || currentScanIndex > scanMaxIndex) { currentScanIndex = scanMinIndex; scanDirection = 1; }
  setServoTarget(scanAngles[currentScanIndex]);
}

void startLostTarget() { trackerState = STATE_LOST_TARGET; setTrackWindowAround(scanAngles[strongestIndex], true); setServoTarget(scanAngles[currentScanIndex]); }

/* ---------------- ULTRASONIC ---------------- */

long readDistanceOnceCm() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10); digitalWrite(TRIG_PIN, LOW);
  unsigned long timeoutUs = ((unsigned long)cfg.clearCm + 30UL) * 58UL;
  if (timeoutUs > 18000UL) timeoutUs = 18000UL;
  if (timeoutUs < 3000UL) timeoutUs = 3000UL;
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, timeoutUs);
  if (duration == 0) return -1;
  long cm = duration / 58;
  if (cm < 2 || cm > 400) return -1;
  return cm;
}

long readDistanceFilteredCm() {
  const uint8_t N = 5; long values[N]; uint8_t count = 0;
  for (uint8_t i = 0; i < N; i++) {
    processServoPulse(); server.handleClient();
    long d = readDistanceOnceCm();
    if (d > 0) values[count++] = d;
    delay(5); yield();
  }
  if (count == 0) return -1;
  for (uint8_t i = 0; i < count; i++) {
    for (uint8_t j = i + 1; j < count; j++) { if (values[j] < values[i]) { long t = values[i]; values[i] = values[j]; values[j] = t; } }
  }
  return values[count / 2];
}

/* ---------------- ANALYSIS & STATE ---------------- */

uint8_t getZoneForIndex(uint8_t idx) {
  if (scanCount <= 1) return 2;
  uint16_t rel = (uint16_t)idx * 100 / (scanCount - 1);
  if (rel < 34) return 1; if (rel < 67) return 2; return 4;
}

void updateConfidenceAt(uint8_t idx, long d) {
  if (idx >= scanCount) return;
  unsigned long now = millis();

  if (d > 0) { distanceMap[idx] = d; lastDistanceCm = d; } 
  else distanceMap[idx] = 0;

  if (!baselineValid[idx]) {
    baselineMap[idx] = (d > 0) ? (uint16_t)d : DEFAULT_BASELINE_CM;
    baselineValid[idx] = 1;
    return;
  }

  bool simpleNear = (d > 0 && d <= cfg.detectCm);
  bool baselineCloser = false;
  if (d > 0 && baselineMap[idx] > d) {
    uint16_t delta = baselineMap[idx] - d;
    baselineCloser = (delta >= cfg.baselineDeltaCm && d <= cfg.clearCm);
  }

  if (simpleNear || baselineCloser) {
    if (confidenceMap[idx] <= 80) confidenceMap[idx] += 20; else confidenceMap[idx] = 100;
    lastSeenMap[idx] = now;
  } else {
    if (confidenceMap[idx] >= 10) confidenceMap[idx] -= 10; else confidenceMap[idx] = 0;
    if (d > 0 && confidenceMap[idx] == 0) {
      if (abs((long)baselineMap[idx] - d) < 150) baselineMap[idx] = (uint16_t)(((uint32_t)baselineMap[idx] * 19 + (uint32_t)d) / 20);
    }
  }
}

uint8_t countZones(uint8_t mask) { uint8_t c = 0; if (mask & 1) c++; if (mask & 2) c++; if (mask & 4) c++; return c; }

void decayUnseenAngles() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < scanCount; i++) {
    if (i == currentScanIndex || confidenceMap[i] == 0 || lastSeenMap[i] == 0) continue;
    if (now - lastSeenMap[i] > cfg.clearStableMs) {
      if (confidenceMap[i] >= 10) confidenceMap[i] -= 10; else confidenceMap[i] = 0;
      lastSeenMap[i] = now - cfg.clearStableMs + 150; 
    }
  }
}

void analyzeMap() {
  activeCount = 0; zoneMask = 0; strongestConfidence = 0; strongestIndex = centerIndex;
  uint32_t weightedSum = 0; uint32_t confSum = 0;

  for (uint8_t i = 0; i < scanCount; i++) {
    if (confidenceMap[i] >= CONF_ACTIVE) { activeCount++; zoneMask |= getZoneForIndex(i); }
    if (confidenceMap[i] > 0) { weightedSum += (uint32_t)scanAngles[i] * confidenceMap[i]; confSum += confidenceMap[i]; }
    if (confidenceMap[i] > strongestConfidence) { strongestConfidence = confidenceMap[i]; strongestIndex = i; }
  }

  if (confSum > 0) targetAngle = weightedSum / confSum; else targetAngle = scanAngles[centerIndex];
  targetDistanceCm = distanceMap[strongestIndex];

  uint32_t maxConf = scanCount * 100;
  activityScore = (maxConf > 0) ? (uint8_t)((confSum * 100UL) / maxConf) : 0;

  if (activityScore == 0) crowdLevel = CROWD_NONE;
  else if (activityScore > 40 || countZones(zoneMask) >= 3) crowdLevel = CROWD_HIGH;
  else if (activityScore > 15 || countZones(zoneMask) >= 2) crowdLevel = CROWD_MEDIUM;
  else crowdLevel = CROWD_LOW;
}

void updateMotionState() {
  unsigned long now = millis();
  if (crowdLevel == CROWD_NONE) { motionState = MOTION_NONE; prevMotionAngle = -1; lastMotionSampleAt = now; return; }

  if (lastMotionSampleAt == 0 || now - lastMotionSampleAt >= 1000UL) {
    if (prevMotionAngle >= 0) {
      int dAngle = (int)targetAngle - prevMotionAngle;
      if (abs(dAngle) >= 12) motionState = (dAngle < 0) ? MOTION_MOVING_LEFT : MOTION_MOVING_RIGHT;
    }
    prevMotionAngle = targetAngle; lastMotionSampleAt = now;
  }

  if (motionState != MOTION_MOVING_LEFT && motionState != MOTION_MOVING_RIGHT) {
    if (currentVelocityCmS <= -20) motionState = MOTION_APPROACHING;
    else if (currentVelocityCmS >= 20) motionState = MOTION_LEAVING;
    else if (detected && presenceStartedAt > 0 && ((now - presenceStartedAt) / 1000UL >= cfg.standingTimeSec)) motionState = MOTION_STANDING;
    else motionState = MOTION_UNKNOWN;
  }
}

void updatePresenceState() {
  unsigned long now = millis();
  
  static unsigned long lastPresenceUpdateAt = 0;
  if (lastPresenceUpdateAt == 0) lastPresenceUpdateAt = now;
  unsigned long elapsed = now - lastPresenceUpdateAt;
  lastPresenceUpdateAt = now;

  bool presenceNow = (activeCount > 0);
  float attackPoints = (100.0 / (float)cfg.detectStableMs) * elapsed;
  float decayPoints  = (100.0 / (float)cfg.clearStableMs) * elapsed;

  if (presenceNow) { presenceScore += attackPoints; lastPeopleSeenAt = now; } 
  else { presenceScore -= decayPoints; }

  if (presenceScore > 100.0) presenceScore = 100.0;
  if (presenceScore < 0.0) presenceScore = 0.0;

  if (!detected && presenceScore >= 100.0) {
    detected = true; triggerCount++; presenceStartedAt = now;
    statTotalDetections++; statLastEventUptime = now;
    if (zoneMask & 1) statZoneHits[0]++; if (zoneMask & 2) statZoneHits[1]++; if (zoneMask & 4) statZoneHits[2]++;
    queueEvent(EVT_PRESENCE_DETECTED, 3);
  } else if (detected && presenceScore <= 0.0) {
    detected = false; presenceStartedAt = 0; queueEvent(EVT_PRESENCE_CLEAR, 3);
  }
}

/* ---------------- TRACKER & DWELL ---------------- */

void chooseNextScanIndex() {
  if (scanCount <= 1) return;

  if (scanMinIndex >= scanMaxIndex) {
    currentScanIndex = scanMinIndex;
    setServoTarget(scanAngles[currentScanIndex]);
    return;
  }

  if (trackerState == STATE_QUICK_SCAN) {
    if (currentScanIndex == 0) currentScanIndex = centerIndex; else if (currentScanIndex == centerIndex) currentScanIndex = scanCount - 1;
    else { if (activeCount > 0) startSearch(); else enterIdleState(); return; }
    setServoTarget(scanAngles[currentScanIndex]); return;
  }

  uint8_t step = 1; 

  if (scanDirection > 0) {
    if (currentScanIndex >= scanMaxIndex) {
      scanDirection = -1;
      if (currentScanIndex > scanMinIndex) currentScanIndex--;
    } else {
      currentScanIndex++;
    }
  } else {
    if (currentScanIndex <= scanMinIndex) {
      scanDirection = 1;
      if (currentScanIndex < scanMaxIndex) currentScanIndex++;
    } else {
      currentScanIndex--;
    }
  }
  setServoTarget(scanAngles[currentScanIndex]);
}

void evaluateTrackerState() {
  unsigned long now = millis();
  if (trackerState == STATE_IDLE) {
    if (activeCount > 0) { startSearch(); return; }
    if (now - lastMiniScanAt >= (unsigned long)cfg.miniScanIntervalSec * 1000UL) { lastMiniScanAt = now; startQuickScan(); return; }
    if (cfg.servoEnabled && !servoMoving && servoIdleMovePending && now >= servoIdleDetachAt) { servoIdleMovePending = false; detachServoIfNeeded(); }
    return;
  }
  if (activeCount == 0) {
    if (trackerState != STATE_LOST_TARGET && trackerState != STATE_QUICK_SCAN) { startLostTarget(); return; }
    if (now - lastPeopleSeenAt >= (unsigned long)cfg.servoIdleSec * 1000UL) { enterIdleState(); return; }
  }
  if (activeCount > 0) {
    lastPeopleSeenAt = now;
    if (crowdLevel == CROWD_HIGH || countZones(zoneMask) >= 2) { if (trackerState != STATE_CROWD_SCAN) { startCrowdScan(); return; } } 
    else { if (trackerState != STATE_TRACK_ZONE) { startTrackZone(); return; } }
  }
  if (trackerState == STATE_TRACK_ZONE || trackerState == STATE_CROWD_SCAN) {
    if (now - lastFullRescanAt >= (unsigned long)cfg.fullRescanIntervalSec * 1000UL) {
      lastFullRescanAt = now; if (cfg.nodeRole == ROLE_EXIT_PATH) { startSearch(); return; }
    }
  }
}

void processTracker() {
  unsigned long now = millis();
  processServoPulse();
  processServoMove();

  if (servoMoving) return;
  if (now - servoReachedAt < SERVO_SETTLE_MS) return;

  static unsigned long lastReadAt = 0;
  if (now - lastReadAt < cfg.sampleMs) return;
  lastReadAt = now;

  long d = readDistanceFilteredCm();
  currentVelocityCmS = 0;

  if (d > 0) {
    unsigned long nowVel = millis();
    if (confidenceMap[currentScanIndex] >= CONF_ACTIVE && trackerState != STATE_SEARCH) {
      if (prevVelocityDistanceCm > 0 && prevVelocityAt > 0) {
        float dt = (nowVel - prevVelocityAt) / 1000.0;
        if (dt >= 0.3) { 
          currentVelocityCmS = (int)((d - prevVelocityDistanceCm) / dt);
          prevVelocityDistanceCm = d; prevVelocityAt = nowVel;
        }
      } else { prevVelocityDistanceCm = d; prevVelocityAt = nowVel; }
    } else { prevVelocityDistanceCm = -1; }
  } else { prevVelocityDistanceCm = -1; }

  if (d > 0) {
    lastGoodReadingAt = now;
    if (sensorFault) sensorFault = false;
  } else {
    if (lastGoodReadingAt > 0 && now - lastGoodReadingAt > SENSOR_FAULT_MS && !sensorFault) {
      sensorFault = true;
      queueEvent(EVT_NODE_ERROR, 3);
    }
  }

  updateConfidenceAt(currentScanIndex, d);
  decayUnseenAngles(); analyzeMap(); updateMotionState(); updatePresenceState(); evaluateTrackerState();
  if (trackerState != STATE_IDLE) chooseNextScanIndex();
}

/* ---------------- ESP-NOW ---------------- */

void onEspNowSent(uint8_t *mac, uint8_t status) {
  lastSendResultAt = millis();
  if (status == 0) {
    lastSendOk = true; lastSuccessfulSendAt = millis();
    if (consecutiveSendOk < 250) consecutiveSendOk++; consecutiveSendFail = 0;
  } else {
    lastSendOk = false;
    if (consecutiveSendFail < 250) consecutiveSendFail++; consecutiveSendOk = 0;
  }
}

void setupEspNow() {
  if (esp_now_init() != 0) return;
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(onEspNowSent);
  esp_now_add_peer(HUB_MAC, ESP_NOW_ROLE_SLAVE, ESPNOW_CHANNEL, NULL, 0);
}

void queueEvent(uint8_t eventType, uint8_t repeats) {
  if (queuedRepeats > 0) sendPacketNow(queuedEventType, queuedEventId);
  queuedEventType = eventType; queuedRepeats = repeats;
  queuedEventId = eventSeq++; nextSendAt = 0;
}

void sendPacketNow(uint8_t eventType, uint32_t eventId) {
  SensorPacket pkt;
  pkt.nodeId = cfg.nodeId; pkt.nodeRole = cfg.nodeRole; pkt.eventType = eventType;
  pkt.presence = detected ? 1 : 0; pkt.crowdLevel = crowdLevel; pkt.zoneMask = zoneMask;
  pkt.targetAngle = targetAngle; pkt.targetDistanceCm = targetDistanceCm;
  pkt.motionState = motionState; pkt.confidence = strongestConfidence;
  pkt.trackerState = trackerState; pkt.servoAttached = servoSignalAttached ? 1 : 0;
  pkt.activityScore = activityScore; 
  pkt.eventId = eventId; pkt.uptimeMs = millis();
  esp_now_send(HUB_MAC, (uint8_t*)&pkt, sizeof(pkt));
}

void processEspNowQueue() {
  unsigned long now = millis();
  if (queuedRepeats > 0 && now >= nextSendAt) {
    sendPacketNow(queuedEventType, queuedEventId);
    queuedRepeats--; nextSendAt = now + 70;
  }
  if (queuedRepeats == 0 && now - lastHeartbeatAt >= (unsigned long)cfg.heartbeatSec * 1000UL) {
    lastHeartbeatAt = now; sendPacketNow(EVT_HEARTBEAT, eventSeq++);
  }
  if (detected && now - lastCrowdUpdateAt >= (unsigned long)cfg.crowdUpdateSec * 1000UL) {
    lastCrowdUpdateAt = now;
    if (cfg.nodeRole == ROLE_EXIT_PATH && motionState == MOTION_STANDING && presenceStartedAt > 0 && ((now - presenceStartedAt) / 1000UL >= cfg.blockedTimeSec)) {
      sendPacketNow(EVT_PATH_BLOCKED, eventSeq++);
    } else if (cfg.nodeRole == ROLE_EXIT_PATH) sendPacketNow(EVT_PATH_ACTIVITY, eventSeq++);
    else sendPacketNow(EVT_CROWD_UPDATE, eventSeq++);
  }
}

/* ---------------- STATUS FORMATTERS ---------------- */

void setLed(uint8_t pin, bool on) { digitalWrite(pin, on ? HIGH : LOW); }
bool isBroadcastHubMac() { for (uint8_t i = 0; i < 6; i++) if (HUB_MAC[i] != 0xFF) return false; return true; }

String getHubLedStateText() {
  unsigned long now = millis();
  if (lastSuccessfulSendAt == 0 || (now - lastSuccessfulSendAt > HUB_DISCONNECT_MS)) return "OFF / DISCONNECTED";
  if (isBroadcastHubMac()) return "UNVERIFIED / BROADCAST MAC";
  if ((now - lastSuccessfulSendAt <= HUB_STABLE_WINDOW_MS) && consecutiveSendFail == 0) return "SOLID / STABLE";
  return "BLINK / UNSTABLE";
}

String stateText() {
  switch (trackerState) {
    case STATE_IDLE: return "IDLE"; case STATE_QUICK_SCAN: return "QUICK_SCAN";
    case STATE_SEARCH: return "SEARCH"; case STATE_TRACK_ZONE: return "TRACK_ZONE";
    case STATE_CROWD_SCAN: return "CROWD_SCAN"; case STATE_LOST_TARGET: return "LOST_TARGET";
  } return "UNKNOWN";
}
String crowdText() {
  switch (crowdLevel) {
    case CROWD_NONE: return "NONE"; case CROWD_LOW: return "LOW";
    case CROWD_MEDIUM: return "MEDIUM"; case CROWD_HIGH: return "HIGH";
  } return "UNKNOWN";
}
String motionText() {
  switch (motionState) {
    case MOTION_NONE: return "NONE"; case MOTION_STANDING: return "STANDING";
    case MOTION_MOVING_LEFT: return "MOVING_LEFT"; case MOTION_MOVING_RIGHT: return "MOVING_RIGHT";
    case MOTION_APPROACHING: return "APPROACHING"; case MOTION_LEAVING: return "LEAVING";
    case MOTION_UNKNOWN: return "UNKNOWN";
  } return "UNKNOWN";
}
String zoneText() {
  if (zoneMask == 0) return "NONE"; String s = "";
  if (zoneMask & 1) s += "LEFT "; if (zoneMask & 2) s += "CENTER "; if (zoneMask & 4) s += "RIGHT ";
  return s;
}
String roleText() { return (cfg.nodeRole == ROLE_EXIT_PATH) ? "EXIT/PATHWAY" : "THEME FRONT"; }

String getDetectLedStateText() {
  if (sensorFault) return "FAST BLINK / SENSOR FAULT";
  if (detected) return "SOLID / DETECTED";
  if (presenceScore > 0) return "BLINK / DETECTING";
  return "OFF / CLEAR";
}

void updateStatusLeds() {
  unsigned long now = millis();
  bool hubLedOn = false;
  if (lastSuccessfulSendAt == 0 || (now - lastSuccessfulSendAt > HUB_DISCONNECT_MS)) hubLedOn = false;
  else if (isBroadcastHubMac()) hubLedOn = ((now / 500) % 2 == 0);
  else if ((now - lastSuccessfulSendAt <= HUB_STABLE_WINDOW_MS) && consecutiveSendFail == 0) hubLedOn = true;
  else hubLedOn = ((now / 300) % 2 == 0);
  setLed(HUB_LED_PIN, hubLedOn);

  bool detectLedOn = false;
  if (sensorFault) detectLedOn = ((now / 120) % 2 == 0);
  else if (detected) detectLedOn = true;
  else if (presenceScore > 0) detectLedOn = ((now / 200) % 2 == 0);
  else detectLedOn = false;
  setLed(DETECT_LED_PIN, detectLedOn);
}

/* ---------------- WEB UI (Streamed & Buffer-Safe) ---------------- */

String webBuf;
void scFlush() { if (webBuf.length() > 0) { server.sendContent(webBuf); webBuf = ""; } }
inline void sc(const __FlashStringHelper* s) { webBuf += s; if(webBuf.length() > 1024) scFlush(); }
inline void sc(const String& s)              { webBuf += s; if(webBuf.length() > 1024) scFlush(); }
inline void sc(const char* s)                { webBuf += s; if(webBuf.length() > 1024) scFlush(); }

String getBusiestZoneText() {
  if (statZoneHits[0]==0 && statZoneHits[1]==0 && statZoneHits[2]==0) return "N/A";
  if (statZoneHits[0] >= statZoneHits[1] && statZoneHits[0] >= statZoneHits[2]) return "LEFT";
  if (statZoneHits[2] >= statZoneHits[0] && statZoneHits[2] >= statZoneHits[1]) return "RIGHT";
  return "CENTER";
}

String htmlHeader(const String &title) {
  String html; html.reserve(420);
  html += F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>"); html += title;
  html += F("</title><style>body{font-family:Arial;background:#101010;color:#eee;padding:16px}");
  html += F(".card{background:#1f1f1f;padding:16px;border-radius:14px;margin-bottom:14px}");
  html += F("input,select{width:100%;padding:10px;margin:6px 0;border-radius:8px;border:0}input[type=range]{accent-color:#ffb300}");
  html += F("button,.btn{display:inline-block;background:#ffb300;color:#111;padding:11px 14px;border-radius:8px;text-decoration:none;font-weight:bold;border:0;margin:3px}");
  html += F(".ok{color:#00e676;font-weight:bold}.bad{color:#ff5252;font-weight:bold}.warn{color:#ffb300;font-weight:bold}small{color:#aaa}hr{border-color:#333}");
  html += F("</style></head><body><h2>Garur Sensor Node</h2>");
  return html;
}

void scRange(const char* label, const char* id, const char* name, int minV, int maxV, int stepV, int value, const char* suffix) {
  sc(F("<label>")); sc(label); sc(F(": <b><span id='")); sc(id); sc(F("Val'>")); sc(String(value));
  sc(F("</span>")); sc(suffix); sc(F("</b></label><input id='")); sc(id); sc(F("' name='")); sc(name);
  sc(F("' type='range' min='")); sc(String(minV)); sc(F("' max='")); sc(String(maxV));
  sc(F("' step='")); sc(String(stepV)); sc(F("' value='")); sc(String(value)); sc(F("'>"));
}

void handleRoot() {
  webBuf.reserve(2048); webBuf = ""; 
  server.setContentLength(CONTENT_LENGTH_UNKNOWN); server.send(200, "text/html", "");

  sc(htmlHeader("Dashboard"));
  sc(F("<div class='card'><h3>Live Status</h3>"));
  sc(F("<p>Firmware: <b>")); sc(F(FW_VERSION)); sc(F("</b></p><p>Node ID: <b id='nodeId'>-</b></p><p>Role: <b id='role'>-</b></p><p>Tracker: <b id='trackerState'>-</b></p>"));
  sc(F("<p>Presence: <b id='presence'>-</b></p><p>Crowd (Score): <b id='crowd'>-</b></p><p>Zone: <b id='zone'>-</b></p><p>Motion: <b id='motion'>-</b></p>"));
  sc(F("<p>Distance: <b id='dist'>-</b> cm</p><p>Target Angle: <b id='angle'>-</b></p><p>Sensor: <b id='sensorOk'>-</b></p><p>Hub LED: <b id='hubLed'>-</b></p>"));
  sc(F("<p>Detection LED: <b id='detectLed'>-</b></p><p>Triggers: <b id='trig'>-</b></p><p>ESP-NOW OK/Fail: <b id='okfail'>-</b></p><p>Uptime: <b id='up'>-</b> s</p></div>"));

  sc(F("<div class='card'><h3>Analytics</h3><p>Total Detections: <b id='statTotal'>-</b></p><p>Busiest Zone: <b id='statZone'>-</b></p><p>Last Detect: <b id='statLast'>-</b> ago</p></div>"));

  sc(F("<div class='card'><h3>Actions</h3>"));
  sc(F("<p><a class='btn' href='/settings'>⚙️ Settings & Calibration</a></p>"));
  sc(F("<p><a class='btn' href='/baseline'>Relearn Baseline</a></p>"));
  sc(F("<p><a class='btn' href='/ota'>OTA Update</a></p><p><a class='btn' href='/reboot'>Reboot Node</a></p></div>"));

  sc(F("<script>"
       "function upd(){fetch('/api/status').then(r=>r.json()).then(j=>{"
       "document.getElementById('nodeId').innerText=j.nodeId;"
       "document.getElementById('role').innerText=j.roleText;"
       "document.getElementById('trackerState').innerText=j.trackerState;"
       "if(j.sensorFault){document.getElementById('presence').innerHTML='<span class=bad>FAULT / CHECK SENSOR</span>';}else{"
       "document.getElementById('presence').innerHTML=j.presence?'<span class=ok>YES ('+j.presenceScore+'%)</span>':'<span class=warn>NO ('+j.presenceScore+'%)</span>';}"
       "document.getElementById('crowd').innerText=j.crowdText+' ('+j.activityScore+')';"
       "document.getElementById('zone').innerText=j.zoneText;"
       "document.getElementById('motion').innerText=j.motionText+(j.velCmS!=0?' ('+j.velCmS+'cm/s)':'');"
       "document.getElementById('dist').innerText=j.distanceCm;"
       "document.getElementById('angle').innerText=j.targetAngle+' deg';"
       "document.getElementById('sensorOk').innerHTML=j.sensorFault?'<span class=warn>FAULT</span>':'<span class=ok>OK</span>';"
       "document.getElementById('hubLed').innerHTML=j.hubLedState.includes('STABLE')?'<span class=ok>'+j.hubLedState+'</span>':((j.hubLedState.includes('UNSTABLE')||j.hubLedState.includes('UNVERIFIED'))?'<span class=warn>'+j.hubLedState+'</span>':'<span class=bad>'+j.hubLedState+'</span>');"
       "document.getElementById('detectLed').innerHTML=j.detectLedState.includes('DETECTED')?'<span class=ok>'+j.detectLedState+'</span>':((j.detectLedState.includes('BLINK')||j.detectLedState.includes('FAULT'))?'<span class=warn>'+j.detectLedState+'</span>':'<span class=bad>'+j.detectLedState+'</span>');"
       "document.getElementById('trig').innerText=j.triggerCount;"
       "document.getElementById('okfail').innerText=j.consecutiveSendOk+'/'+j.consecutiveSendFail;"
       "let upS=Math.floor(j.uptimeMs/1000);"
       "document.getElementById('up').innerText=upS;"
       "document.getElementById('statTotal').innerText=j.statTotal;"
       "document.getElementById('statZone').innerText=j.statZone;"
       "document.getElementById('statLast').innerText=j.statLast==0?'Never':(upS-Math.floor(j.statLast/1000))+'s';"
       "}).catch(e=>{}).finally(()=>setTimeout(upd, 800));}"
       "setTimeout(upd, 800);</script></body></html>"));

  scFlush(); server.sendContent("");
}

void handleSettings() {
  webBuf.reserve(2048); webBuf = "";
  server.setContentLength(CONTENT_LENGTH_UNKNOWN); server.send(200, "text/html", "");

  sc(htmlHeader("Settings"));
  sc(F("<div class='card'><h3>System Configuration</h3><form method='POST' action='/save'>"));
  
  sc(F("<label>Node ID</label><input name='nodeId' type='number' min='1' max='20' value='"));
  sc(String(cfg.nodeId)); sc(F("'><label>Node Role</label><select name='nodeRole'><option value='1'"));
  if (cfg.nodeRole == ROLE_EXIT_PATH) sc(F(" selected")); sc(F(">Exit / Pathway Node</option><option value='2'"));
  if (cfg.nodeRole == ROLE_THEME_FRONT) sc(F(" selected")); sc(F(">Main Theme Front Node</option></select><label>Servo Signal</label><select name='servoEnabled'><option value='1'"));
  if (cfg.servoEnabled) sc(F(" selected")); sc(F(">Enabled</option><option value='0'"));
  if (!cfg.servoEnabled) sc(F(" selected")); sc(F(">Disabled / Debug Mode</option></select><hr><h3>Detection</h3>"));

  scRange("Detect distance", "detectCm", "detectCm", 20, 350, 5, cfg.detectCm, " cm");
  scRange("Clear distance", "clearCm", "clearCm", 30, 400, 5, cfg.clearCm, " cm");
  sc(F("<label>Detect stable ms</label><input name='detectStableMs' type='number' min='100' max='5000' value='")); sc(String(cfg.detectStableMs));
  sc(F("'><label>Clear stable ms</label><input name='clearStableMs' type='number' min='500' max='10000' value='")); sc(String(cfg.clearStableMs));
  sc(F("'><label>Sample interval ms</label><input name='sampleMs' type='number' min='100' max='1000' value='")); sc(String(cfg.sampleMs)); sc(F("'>"));
  scRange("Baseline delta", "baselineDeltaCm", "baselineDeltaCm", 10, 150, 5, cfg.baselineDeltaCm, " cm");

  sc(F("<hr><h3>Servo / Tracking</h3>"));
  scRange("Servo start angle", "servoStartAngle", "servoStartAngle", 0, 170, 1, cfg.servoStartAngle, "");
  scRange("Servo end angle", "servoEndAngle", "servoEndAngle", 10, 180, 1, cfg.servoEndAngle, "");
  scRange("Servo step degree", "servoStepDeg", "servoStepDeg", 5, 30, 5, cfg.servoStepDeg, "");
  scRange("Servo speed", "servoSpeedMs", "servoSpeedMs", 5, 200, 5, cfg.servoSpeedMs, " ms/deg");
  scRange("Servo idle time", "servoIdleSec", "servoIdleSec", 5, 300, 5, cfg.servoIdleSec, " sec");
  scRange("Mini scan interval", "miniScanIntervalSec", "miniScanIntervalSec", 2, 60, 1, cfg.miniScanIntervalSec, " sec");
  scRange("Tracking window", "trackingWindowDeg", "trackingWindowDeg", 10, 80, 5, cfg.trackingWindowDeg, " deg");
  scRange("Full rescan interval", "fullRescanIntervalSec", "fullRescanIntervalSec", 3, 60, 1, cfg.fullRescanIntervalSec, " sec");

  sc(F("<hr><h3>Crowd Rules</h3>"));
  scRange("Medium angle count", "mediumAngleCount", "mediumAngleCount", 2, MAX_SCAN_POINTS, 1, cfg.mediumAngleCount, "");
  scRange("High angle count", "highAngleCount", "highAngleCount", 3, MAX_SCAN_POINTS, 1, cfg.highAngleCount, "");
  scRange("Standing time", "standingTimeSec", "standingTimeSec", 3, 120, 1, cfg.standingTimeSec, " sec");
  scRange("Path blocked time", "blockedTimeSec", "blockedTimeSec", 5, 120, 1, cfg.blockedTimeSec, " sec");

  sc(F("<hr><h3>ESP-NOW</h3>"));
  scRange("Heartbeat interval", "heartbeatSec", "heartbeatSec", 2, 30, 1, cfg.heartbeatSec, " sec");
  scRange("Crowd update interval", "crowdUpdateSec", "crowdUpdateSec", 1, 10, 1, cfg.crowdUpdateSec, " sec");

  sc(F("<br><button type='submit'>Save Settings</button></form></div>"));
  sc(F("<div class='card'><a class='btn' href='/'>🔙 Back to Dashboard</a></div>"));
  
  sc(F("<script>document.querySelectorAll('input[type=range]').forEach(r=>{let s=document.getElementById(r.id+'Val');if(s){r.addEventListener('input',()=>s.innerText=r.value);}});</script></body></html>"));

  scFlush(); server.sendContent("");
}

void handleStatusJson() {
  String json; json.reserve(900);
  json += F("{");
  json += F("\"nodeId\":"); json += String(cfg.nodeId); json += F(",");
  json += F("\"roleText\":\""); json += roleText(); json += F("\",");
  json += F("\"presence\":"); json += detected ? F("true") : F("false"); json += F(",");
  json += F("\"presenceScore\":"); json += String((int)presenceScore); json += F(",");
  json += F("\"crowdLevel\":"); json += String(crowdLevel); json += F(",");
  json += F("\"crowdText\":\""); json += crowdText(); json += F("\",");
  json += F("\"activityScore\":"); json += String(activityScore); json += F(",");
  json += F("\"zoneText\":\""); json += zoneText(); json += F("\",");
  json += F("\"motionText\":\""); json += motionText(); json += F("\",");
  json += F("\"velCmS\":"); json += String(currentVelocityCmS); json += F(",");
  json += F("\"trackerState\":\""); json += stateText(); json += F("\",");
  json += F("\"distanceCm\":"); json += String(lastDistanceCm > 0 ? lastDistanceCm : 0); json += F(",");
  json += F("\"targetAngle\":"); json += String(targetAngle); json += F(",");
  json += F("\"sensorFault\":"); json += sensorFault ? F("true") : F("false"); json += F(",");
  json += F("\"hubLedState\":\""); json += getHubLedStateText(); json += F("\",");
  json += F("\"detectLedState\":\""); json += getDetectLedStateText(); json += F("\",");
  json += F("\"triggerCount\":"); json += String(triggerCount); json += F(",");
  json += F("\"consecutiveSendOk\":"); json += String(consecutiveSendOk); json += F(",");
  json += F("\"consecutiveSendFail\":"); json += String(consecutiveSendFail); json += F(",");
  json += F("\"uptimeMs\":"); json += String(millis()); json += F(",");
  json += F("\"statTotal\":"); json += String(statTotalDetections); json += F(",");
  json += F("\"statZone\":\""); json += getBusiestZoneText(); json += F("\",");
  json += F("\"statLast\":"); json += String(statLastEventUptime);
  json += F("}");
  server.send(200, "application/json", json);
}

void handleSave() {
  if (server.hasArg("nodeId")) cfg.nodeId = server.arg("nodeId").toInt();
  if (server.hasArg("nodeRole")) cfg.nodeRole = server.arg("nodeRole").toInt();
  if (server.hasArg("servoEnabled")) cfg.servoEnabled = server.arg("servoEnabled").toInt();
  if (server.hasArg("detectCm")) cfg.detectCm = server.arg("detectCm").toInt();
  if (server.hasArg("clearCm")) cfg.clearCm = server.arg("clearCm").toInt();
  if (server.hasArg("detectStableMs")) cfg.detectStableMs = server.arg("detectStableMs").toInt();
  if (server.hasArg("clearStableMs")) cfg.clearStableMs = server.arg("clearStableMs").toInt();
  if (server.hasArg("sampleMs")) cfg.sampleMs = server.arg("sampleMs").toInt();
  if (server.hasArg("baselineDeltaCm")) cfg.baselineDeltaCm = server.arg("baselineDeltaCm").toInt();
  if (server.hasArg("servoStartAngle")) cfg.servoStartAngle = server.arg("servoStartAngle").toInt();
  if (server.hasArg("servoEndAngle")) cfg.servoEndAngle = server.arg("servoEndAngle").toInt();
  if (server.hasArg("servoStepDeg")) cfg.servoStepDeg = server.arg("servoStepDeg").toInt();
  if (server.hasArg("servoSpeedMs")) cfg.servoSpeedMs = server.arg("servoSpeedMs").toInt();
  if (server.hasArg("servoIdleSec")) cfg.servoIdleSec = server.arg("servoIdleSec").toInt();
  if (server.hasArg("miniScanIntervalSec")) cfg.miniScanIntervalSec = server.arg("miniScanIntervalSec").toInt();
  if (server.hasArg("trackingWindowDeg")) cfg.trackingWindowDeg = server.arg("trackingWindowDeg").toInt();
  if (server.hasArg("fullRescanIntervalSec")) cfg.fullRescanIntervalSec = server.arg("fullRescanIntervalSec").toInt();
  if (server.hasArg("mediumAngleCount")) cfg.mediumAngleCount = server.arg("mediumAngleCount").toInt();
  if (server.hasArg("highAngleCount")) cfg.highAngleCount = server.arg("highAngleCount").toInt();
  if (server.hasArg("standingTimeSec")) cfg.standingTimeSec = server.arg("standingTimeSec").toInt();
  if (server.hasArg("blockedTimeSec")) cfg.blockedTimeSec = server.arg("blockedTimeSec").toInt();
  if (server.hasArg("heartbeatSec")) cfg.heartbeatSec = server.arg("heartbeatSec").toInt();
  if (server.hasArg("crowdUpdateSec")) cfg.crowdUpdateSec = server.arg("crowdUpdateSec").toInt();

  saveSettings(); rebuildScanAngles();
  detected = false; activeCount = 0;
  crowdLevel = CROWD_NONE; zoneMask = 0; motionState = MOTION_NONE;
  presenceScore = 0.0; activityScore = 0; prevVelocityDistanceCm = -1;

  if (!cfg.servoEnabled) detachServoIfNeeded();
  currentServoAngle = getCenterAngle(); targetServoAngle = currentServoAngle;
  servoReachedAt = millis(); enterIdleState();
  
  String html = htmlHeader("Saved");
  html += F("<div class='card'><h3>Settings Saved</h3><p>Saved Node ID: <b>"); html += String(cfg.nodeId); 
  html += F("</b></p><p><a class='btn' href='/settings'>Back to Settings</a></p><p><a class='btn' href='/'>Go to Dashboard</a></p></div></body></html>");
  server.send(200, "text/html", html);
}

void handleBaseline() {
  for (uint8_t i = 0; i < MAX_SCAN_POINTS; i++) {
    confidenceMap[i] = 0; distanceMap[i] = 0; baselineMap[i] = 0; baselineValid[i] = 0; lastSeenMap[i] = 0;
  }
  detected = false; activeCount = 0; crowdLevel = CROWD_NONE; zoneMask = 0; prevVelocityDistanceCm = -1;
  trackerState = STATE_SEARCH; currentScanIndex = 0; scanDirection = 1;
  setServoTarget(scanAngles[currentScanIndex]);
  server.sendHeader("Location", "/", true); server.send(303, "text/plain", "");
}

void handleReboot() { server.send(200, "text/plain", "Rebooting..."); delay(300); ESP.restart(); }

void setupWebRoutes() {
  server.on("/", HTTP_GET, handleRoot); 
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/api/status", HTTP_GET, handleStatusJson);
  server.on("/save", HTTP_POST, handleSave); 
  server.on("/baseline", HTTP_GET, handleBaseline);
  server.on("/reboot", HTTP_GET, handleReboot);
  setupWebOTA(server, OTA_USER, OTA_PASSWORD); 
  server.begin();
}

void setupWiFiAP() {
  WiFi.persistent(false); WiFi.mode(WIFI_AP_STA); WiFi.disconnect(); delay(100);
  String ssid = "GarurSensor-" + String(cfg.nodeId);
  WiFi.softAP(ssid.c_str(), AP_PASSWORD, ESPNOW_CHANNEL, false, 4); wifi_set_channel(ESPNOW_CHANNEL);
}

void setup() {
  Serial.begin(115200); delay(700);
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT); digitalWrite(TRIG_PIN, LOW);
  pinMode(HUB_LED_PIN, OUTPUT); pinMode(DETECT_LED_PIN, OUTPUT);
  digitalWrite(HUB_LED_PIN, LOW); digitalWrite(DETECT_LED_PIN, LOW);
  pinMode(SERVO_PIN, OUTPUT); digitalWrite(SERVO_PIN, LOW);

  loadSettings(); rebuildScanAngles(); setupWiFiAP(); setupEspNow(); setupWebRoutes();
  queueEvent(EVT_BOOT, 3);
  currentServoAngle = getCenterAngle(); targetServoAngle = currentServoAngle;
  servoReachedAt = millis(); lastPeopleSeenAt = millis(); lastMiniScanAt = millis();
  enterIdleState();
}

void loop() {
  server.handleClient(); processServoPulse(); processTracker();
  processEspNowQueue(); updateStatusLeds(); delay(2);
}