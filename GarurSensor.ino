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
  if (lastPresenceU
