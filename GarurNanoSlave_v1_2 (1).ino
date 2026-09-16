/*
  Garur Actuator Slave - Arduino Nano v1.2.0 SAFE

  Works with ESP8266 Garur Hub Brain v2.

  Receives commands from ESP8266 HUB over hardware Serial:
    CMD,<seq>,SCENE,<scene>,<durationMs>,<startInMs>
    CFG,<code>,<value>   -- see CFG CODE MAP below; idempotent, no seq/ack needed

  Sends:
    ACK,<seq>
    DONE,<seq>
    FAULT,<seq>,<reason>
    PONG,<ms>
    CFGACK,<code>,<value>  -- diagnostic only, Hub does not act on it

  Scene behavior is config-driven per scene (wing mode, wing speed, lip
  mode, DFPlayer folder file count), pushed from the Hub over CFG lines and
  editable from the Hub's /settings page -- no Nano reflash needed to
  retune any of it. Defaults below match the fixed behavior this sketch
  had before that existed, so a Nano that hasn't heard from the Hub yet (or
  is talking to an older Hub) behaves exactly as before.

  SCENE_STOP is the one exception: always hardcoded to an immediate, full
  safe-stop, deliberately NOT exposed to remote config, since it's the
  fallback used when something's already wrong.

  Upload note:
  - Disconnect Nano D0/D1 serial wires while uploading.
  - Reconnect after upload.
*/

#include <Arduino.h>
#include <Servo.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <avr/wdt.h>

/* ---------------- PIN CONFIG ---------------- */

const uint8_t WING_PWM_PIN = 5;   // PWM
const uint8_t WING_IN1_PIN = 7;
const uint8_t WING_IN2_PIN = 8;

// Reed switch / Hall sensor output to GND when active. INPUT_PULLUP = active LOW.
// Wired directly to the Nano, not the Hub: homing is a tight local feedback
// loop (checked every ~5ms below) that a Hub round-trip over UART couldn't
// keep up with without overshoot.
const uint8_t HOME_SENSOR_PIN = 2;

const uint8_t LIP_SERVO_PIN = 9;

// DFPlayer Mini on SoftwareSerial.
// Nano D10 receives DFPlayer TX.
// Nano D11 sends to DFPlayer RX through 1k resistor.
const uint8_t DF_RX_PIN = 10;
const uint8_t DF_TX_PIN = 11;

/* ---------------- HUB-CONFIGURABLE ACTUATOR VALUES ---------------- */

// These used to be `const` -- now mutable, overridden by CFG lines from the
// Hub, but initialized to the exact same values so behavior is identical
// until/unless the Hub actually changes something.
uint8_t wingHomePwm = 80;   // speed used only while actively homing
// Time (ms) to ramp across the full 0-255 PWM range; a partial ramp (e.g.
// between two spin speeds) takes proportionally less time, not this full
// duration. 0 effectively disables ramping (snaps in one ~20ms step).
// 600ms is a starting point, not a derived value -- the right number
// depends on this wing's actual mass/gearing and needs tuning on the real
// hardware, same as wingHomePwm/wingSpeed always have.
uint16_t wingRampMs = 600;
uint8_t lipClosedAngle = 35;
uint8_t lipOpenMin = 55;
uint8_t lipOpenMax = 105;

enum WingMode : uint8_t { WING_MODE_STOP = 0, WING_MODE_SPIN = 1, WING_MODE_HOME = 2 };
enum LipMode  : uint8_t { LIP_MODE_CLOSED = 0, LIP_MODE_ANIMATE = 1 };

// Must match GarurHubTypes.h's NanoSceneCfg field order and WingMode/LipMode
// numeric values exactly -- Hub and Nano are separate sketches with no
// shared header, and these values cross the wire as plain integers.
struct NanoSceneCfg {
  uint8_t wingMode;
  uint8_t wingSpeed;    // 0-255 PWM, used only when wingMode==WING_MODE_SPIN
  uint8_t lipMode;
  uint8_t dfFileCount;  // files in this scene's DFPlayer folder (folder number = scene index + 1), for random pick
};

// Indexed by Scene (0=IDLE,1=NORMAL,2=FIGHT,3=CELEBRATION); SCENE_STOP (4)
// intentionally has no entry -- it always hardcodes a full safe stop.
// Defaults reproduce this sketch's previous fixed per-scene behavior.
NanoSceneCfg sceneCfg[4] = {
  { WING_MODE_HOME, 0,   LIP_MODE_CLOSED,  1 }, // IDLE
  { WING_MODE_SPIN, 90,  LIP_MODE_CLOSED,  1 }, // NORMAL
  { WING_MODE_SPIN, 210, LIP_MODE_ANIMATE, 1 }, // FIGHT
  { WING_MODE_STOP, 0,   LIP_MODE_CLOSED,  1 }, // CELEBRATION
};

// Reduced from 12s to 6s to reduce stall risk if wing is jammed.
const unsigned long HOME_TIMEOUT_MS = 6000UL;
const unsigned long ESP_LINK_TIMEOUT_MS = 15000UL;

/* ---------------- DFPLAYER SD CARD LAYOUT ---------------- */

// Folder-per-scene, addressed via dfPlayer.playFolder(folder, file):
//   /01/001.mp3, /01/002.mp3, ... = Idle ambience clips
//   /02/001.mp3, /02/002.mp3, ... = Normal clips
//   /03/001.mp3, /03/002.mp3, ... = Fight clips
//   /04/001.mp3, /04/002.mp3, ... = Celebration clips
// Folder names must be exactly 2 digits ("01".."04"), file names exactly 3
// digits + extension ("001.mp3".."0NN.mp3") -- DFPlayer Mini's own
// hardware addressing convention, not a library choice. How many files are
// in each folder is set via sceneCfg[n].dfFileCount from the Hub; one is
// picked at random each time that scene activates. Idle additionally
// re-picks a fresh random clip from its folder each time the current one
// finishes, for continuous (if varied, rather than identically repeating)
// ambience -- simpler and more clearly correct than relying on combining
// folder-addressing with the DFPlayer library's single-track-loop mode,
// which isn't confidently verified here.

SoftwareSerial dfSerial(DF_RX_PIN, DF_TX_PIN);
DFRobotDFPlayerMini dfPlayer;
bool dfReady = false;

Servo lipServo;
int lastLipAngle = -1;

/* ---------------- SCENE STATE ---------------- */

enum Scene : uint8_t {
  SCENE_IDLE = 0,
  SCENE_NORMAL = 1,
  SCENE_FIGHT = 2,
  SCENE_CELEBRATION = 3,
  SCENE_STOP = 4
};

Scene currentScene = SCENE_STOP;

bool pendingActive = false;
Scene pendingScene = SCENE_STOP;
uint32_t pendingSeq = 0;
unsigned long pendingDuration = 0;
unsigned long pendingStartAt = 0;

uint32_t activeSeq = 0;
unsigned long sceneStartedAt = 0;
unsigned long sceneDurationMs = 0;
bool doneSent = false;

uint32_t lastCommandSeq = 0;

bool homingActive = false;
unsigned long homingStartedAt = 0;
uint32_t homingSeq = 0;

unsigned long lastEspRxAt = 0;
bool espContactSeen = false;
bool espTimeoutFaultSent = false;

unsigned long nextLipMoveAt = 0;
unsigned long lastDfActionAt = 0;

/* ---------------- MOTOR HELPERS ---------------- */

void wingStop() {
  analogWrite(WING_PWM_PIN, 0);
  digitalWrite(WING_IN1_PIN, LOW);
  digitalWrite(WING_IN2_PIN, LOW);
}

void wingForward(uint8_t pwm) {
  digitalWrite(WING_IN1_PIN, HIGH);
  digitalWrite(WING_IN2_PIN, LOW);
  analogWrite(WING_PWM_PIN, pwm);
}

void wingReverse(uint8_t pwm) {
  digitalWrite(WING_IN1_PIN, LOW);
  digitalWrite(WING_IN2_PIN, HIGH);
  analogWrite(WING_PWM_PIN, pwm);
}

/* ---------------- WING RAMP ---------------- */
// Gently accelerates to, and decelerates from, a target PWM rather than
// snapping directly to/from it -- the wing's own inertia means an instant
// full-speed start or stop puts real mechanical stress on the gearbox/
// linkage. Two deliberate exceptions, not oversights:
//  - Homing's approach speed (wingHomePwm) is independently configurable
//    and typically already low, and the moment it reaches the sensor it
//    stops immediately (wingRampSnapTo) -- reacting fast matters more
//    than gentleness once you're already home.
//  - SCENE_STOP (the hardcoded safety fallback) also stops immediately,
//    never ramped -- it exists for when something's already wrong, and
//    must not be softened or delayed by anything, including this.
uint8_t wingRampCurrentPwm = 0;
uint8_t wingRampTargetPwm = 0;
uint32_t lastWingRampStepAt = 0;
const uint16_t WING_RAMP_STEP_MS = 20;

void wingRampSetTarget(uint8_t target) {
  wingRampTargetPwm = target;
}

// Bypasses the ramp entirely and applies immediately -- only for the two
// exceptions above.
void wingRampSnapTo(uint8_t pwm) {
  wingRampCurrentPwm = pwm;
  wingRampTargetPwm = pwm;
  if (pwm == 0) wingStop(); else wingForward(pwm);
}

void processWingRamp() {
  if (wingRampCurrentPwm == wingRampTargetPwm) return;
  uint32_t now = millis();
  if (now - lastWingRampStepAt < WING_RAMP_STEP_MS) return;
  lastWingRampStepAt = now;

  // Step size derived from wingRampMs (time to cross the full 0-255
  // range), so a shorter partial ramp -- e.g. between two spin speeds --
  // takes proportionally less time rather than this full duration
  // regardless of distance.
  uint16_t steps = wingRampMs / WING_RAMP_STEP_MS;
  if (steps == 0) steps = 1;
  uint8_t stepAmount = (uint8_t)max((uint16_t)1, (uint16_t)(255 / steps));

  if (wingRampCurrentPwm < wingRampTargetPwm) {
    uint16_t next = (uint16_t)wingRampCurrentPwm + stepAmount;
    wingRampCurrentPwm = (next >= wingRampTargetPwm) ? wingRampTargetPwm : (uint8_t)next;
  } else {
    int16_t next = (int16_t)wingRampCurrentPwm - stepAmount;
    wingRampCurrentPwm = (next <= wingRampTargetPwm) ? wingRampTargetPwm : (uint8_t)next;
  }

  if (wingRampCurrentPwm == 0) wingStop(); else wingForward(wingRampCurrentPwm);
}

bool homeSensorActive() {
  return digitalRead(HOME_SENSOR_PIN) == LOW;
}

/* ---------------- SERIAL RESPONSES ---------------- */

void sendAck(uint32_t seq) {
  Serial.print(F("ACK,"));
  Serial.println(seq);
}

void sendDone(uint32_t seq) {
  Serial.print(F("DONE,"));
  Serial.println(seq);
}

void sendFault(uint32_t seq, const char* reason) {
  Serial.print(F("FAULT,"));
  Serial.print(seq);
  Serial.print(F(","));
  Serial.println(reason);
}

/* ---------------- AUDIO ---------------- */

void dfStopAndSettle() {
  if (!dfReady) return;
  dfPlayer.stop();
  delay(60);
  lastDfActionAt = millis();
}

// Picks one file at random from a scene's DFPlayer folder and plays it
// once. fileCount==0 is treated as "nothing configured for this scene yet"
// and safely does nothing rather than calling random(1,1) (which Arduino's
// random() would treat as an always-zero range -- not a crash, but not a
// valid file number either).
void playRandomFromFolder(uint8_t folderNum, uint8_t fileCount) {
  if (!dfReady || fileCount == 0) return;
  uint8_t fileNum = random(1, (uint16_t)fileCount + 1);
  dfStopAndSettle();
  delay(40);
  dfPlayer.playFolder(folderNum, fileNum);
  lastDfActionAt = millis();
}

// DFPlayer folder addressing doesn't confidently combine with the
// library's single-track-loop mode (see the SD card layout note above), so
// Idle's continuous ambience is simulated here: whenever the currently
// playing clip finishes AND the scene is still Idle, pick and play a fresh
// random one from Idle's folder. Anything else finishing (Normal/Fight/
// Celebration's one-shot effect) is left alone -- those should play once
// and stop, not auto-repeat.
void processDfPlayerEvents() {
  if (!dfReady) return;
  if (!dfPlayer.available()) return;
  uint8_t type = dfPlayer.readType();
  if (type == DFPlayerPlayFinished && currentScene == SCENE_IDLE) {
    playRandomFromFolder(1, sceneCfg[SCENE_IDLE].dfFileCount);
  }
}

void stopAudio() {
  if (!dfReady) return;
  dfPlayer.stop();
}

/* ---------------- LIP SERVO ---------------- */

void writeLipAngle(int angle) {
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;
  if (angle == lastLipAngle) return;
  lipServo.write(angle);
  lastLipAngle = angle;
}

void lipClosed() {
  writeLipAngle(lipClosedAngle);
}

void processLipServo() {
  // currentScene can be SCENE_STOP (4), which has no sceneCfg[] entry --
  // treat that (and any other out-of-range value) as "not animating".
  bool animate = (currentScene < 4) && (sceneCfg[currentScene].lipMode == LIP_MODE_ANIMATE);
  if (!animate) {
    lipClosed();
    return;
  }

  unsigned long now = millis();
  if (now < nextLipMoveAt) return;

  // Defensive: random(min,max) needs max>min. Hub-side validation keeps
  // lipOpenMin/Max sane, but a corrupted CFG line over UART shouldn't be
  // able to wedge this into a bad range.
  uint8_t lo = lipOpenMin, hi = lipOpenMax;
  if (hi <= lo) hi = lo + 1;
  int angle = random(lo, (int)hi + 1);
  writeLipAngle(angle);
  nextLipMoveAt = now + random(80, 180);
}

/* ---------------- HOMING ---------------- */

void startHoming(uint32_t seq) {
  homingActive = true;
  homingStartedAt = millis();
  homingSeq = seq;

  if (homeSensorActive()) {
    wingRampSnapTo(0); // already home -- instant, no ramp needed
    homingActive = false;
    sendDone(seq);
    doneSent = true;
    return;
  }

  wingRampSetTarget(wingHomePwm); // ramps up gently to homing speed; sensor-triggered stop below stays instant
}

void processHoming() {
  if (!homingActive) return;

  if (homeSensorActive()) {
    wingRampSnapTo(0); // reached home -- instant stop, reacting fast matters more here than gentleness
    homingActive = false;
    sendDone(homingSeq);
    doneSent = true;
    return;
  }

  if (millis() - homingStartedAt > HOME_TIMEOUT_MS) {
    wingRampSnapTo(0);
    homingActive = false;
    sendFault(homingSeq, "HOME_TIMEOUT");
    doneSent = true; // was missing -- left doneSent false, so a stale duration timer could still fire a confusing DONE after this FAULT
  }
}

/* ---------------- SCENE CONTROL ---------------- */

Scene parseSceneName(const char* s) {
  if (strcmp(s, "IDLE") == 0) return SCENE_IDLE;
  if (strcmp(s, "NORMAL") == 0) return SCENE_NORMAL;
  if (strcmp(s, "FIGHT") == 0) return SCENE_FIGHT;
  if (strcmp(s, "CELEBRATION") == 0) return SCENE_CELEBRATION;
  if (strcmp(s, "STOP") == 0) return SCENE_STOP;
  return SCENE_STOP;
}

void activateScene(Scene scene, uint32_t seq, unsigned long durationMs) {
  currentScene = scene;
  activeSeq = seq;
  sceneStartedAt = millis();
  sceneDurationMs = durationMs;
  doneSent = false;
  homingActive = false;

  if (scene == SCENE_STOP || scene > SCENE_CELEBRATION) {
    // Hardcoded, deliberately not exposed to sceneCfg[] -- see file header:
    // this is the fallback used when something's already wrong, so it must
    // not be something a bad config value could compromise. Instant stop,
    // never ramped -- see the WING RAMP section for why.
    wingRampSnapTo(0);
    lipClosed();
    stopAudio();
    sendDone(seq);
    doneSent = true;
    return;
  }

  NanoSceneCfg& sc = sceneCfg[scene];

  switch (sc.wingMode) {
    case WING_MODE_HOME: startHoming(seq); break; // handles its own DONE/ack timing once the sensor triggers
    case WING_MODE_SPIN: wingRampSetTarget(sc.wingSpeed); break;
    case WING_MODE_STOP: default: wingRampSetTarget(0); break;
  }

  if (sc.lipMode == LIP_MODE_ANIMATE) {
    nextLipMoveAt = millis(); // animate starting right away, not waiting for the next natural tick
  } else {
    lipClosed();
  }

  playRandomFromFolder(scene + 1, sc.dfFileCount);
}

void scheduleScene(Scene scene, uint32_t seq, unsigned long durationMs, unsigned long startInMs) {
  pendingActive = true;
  pendingScene = scene;
  pendingSeq = seq;
  pendingDuration = durationMs;
  pendingStartAt = millis() + startInMs;
}

void processPendingScene() {
  if (!pendingActive) return;
  if (millis() >= pendingStartAt) {
    pendingActive = false;
    activateScene(pendingScene, pendingSeq, pendingDuration);
  }
}

void processSceneDuration() {
  if (doneSent) return;
  if (sceneDurationMs == 0) return;

  if (millis() - sceneStartedAt >= sceneDurationMs) {
    // Local failsafe settle: if the Hub's next scene command hasn't arrived
    // by the time this scene's own duration elapses, stop spinning/animating
    // rather than keep going indefinitely waiting for a command that might
    // be delayed or lost. Config-driven (any scene whose wingMode/lipMode
    // says it's active) rather than hardcoded to FIGHT/CELEBRATION like
    // before -- a deliberate generalization, not just a refactor: Normal
    // (and its showcase sub-states, which do get real durations) now gets
    // the same defensive settle-down if its next command is ever late,
    // which it didn't before. Ramped, not instant -- this is a graceful
    // settle, not the emergency SCENE_STOP path, so the same inertia
    // consideration applies here too.
    if (currentScene < 4) {
      if (sceneCfg[currentScene].wingMode == WING_MODE_SPIN) wingRampSetTarget(0);
      if (sceneCfg[currentScene].lipMode == LIP_MODE_ANIMATE) lipClosed();
    }
    sendDone(activeSeq);
    doneSent = true;
  }
}

/* ---------------- NANO-SIDE CONFIG APPLY ---------------- */

// code is <S><F> for a per-scene field (S=I/N/F/C selects sceneCfg[0..3],
// F=WM/WS/LM/DF selects the field) or one of the 4 global codes
// (WHP/LCA/LMN/LMX). Must match GarurHubBrain_v2.ino's getNanoCfgField()
// code table exactly -- see that function's comment for the full map.
void applyNanoCfg(const char* code, long value) {
  int8_t idx = -1;
  if (code[0] == 'I') idx = SCENE_IDLE;
  else if (code[0] == 'N') idx = SCENE_NORMAL;
  else if (code[0] == 'F') idx = SCENE_FIGHT;
  else if (code[0] == 'C') idx = SCENE_CELEBRATION;

  if (idx >= 0) {
    NanoSceneCfg& sc = sceneCfg[idx];
    const char* field = code + 1;
    if (!strcmp(field, "WM")) sc.wingMode = (uint8_t)constrain(value, 0, 2);
    else if (!strcmp(field, "WS")) sc.wingSpeed = (uint8_t)constrain(value, 0, 255);
    else if (!strcmp(field, "LM")) sc.lipMode = (uint8_t)constrain(value, 0, 1);
    else if (!strcmp(field, "DF")) sc.dfFileCount = (uint8_t)constrain(value, 0, 255);
    return;
  }

  if (!strcmp(code, "WHP")) wingHomePwm = (uint8_t)constrain(value, 0, 255);
  else if (!strcmp(code, "LCA")) lipClosedAngle = (uint8_t)constrain(value, 0, 180);
  else if (!strcmp(code, "LMN")) lipOpenMin = (uint8_t)constrain(value, 0, 180);
  else if (!strcmp(code, "LMX")) lipOpenMax = (uint8_t)constrain(value, 0, 180);
  else if (!strcmp(code, "WRM")) wingRampMs = (uint16_t)constrain(value, 0, 5000);
  // Unrecognized codes are silently ignored -- forward-compatible with a
  // Hub sending a field this sketch version doesn't know about yet.
}

/* ---------------- COMMAND PARSER ---------------- */

void markEspContact() {
  lastEspRxAt = millis();
  espContactSeen = true;
  espTimeoutFaultSent = false;
}

void processCommandLine(char* line) {
  markEspContact();

  char* type = strtok(line, ",");
  if (!type) return;

  if (strcmp(type, "PING") == 0) {
    char* value = strtok(NULL, ",");
    Serial.print(F("PONG,"));
    Serial.println(value ? value : "0");
    return;
  }

  if (strcmp(type, "CFG") == 0) {
    char* code = strtok(NULL, ",");
    char* valueStr = strtok(NULL, ",");
    if (!code || !valueStr) return;
    long value = strtol(valueStr, NULL, 10);
    applyNanoCfg(code, value);
    // Diagnostic only -- the Hub doesn't act on this reply (CFG values are
    // idempotent, so it just re-sends the whole config set on the next
    // sync rather than tracking per-field acks), but it's visible in the
    // Hub's own Serial output since this rides the same link CMD/PING do.
    Serial.print(F("CFGACK,"));
    Serial.print(code);
    Serial.print(F(","));
    Serial.println(value);
    return;
  }

  if (strcmp(type, "CMD") != 0) return;

  char* seqStr = strtok(NULL, ",");
  char* category = strtok(NULL, ",");
  char* sceneStr = strtok(NULL, ",");
  char* durationStr = strtok(NULL, ",");
  char* startInStr = strtok(NULL, ",");

  if (!seqStr || !category || !sceneStr || !durationStr) {
    sendFault(0, "BAD_CMD");
    return;
  }

  uint32_t seq = strtoul(seqStr, NULL, 10);
  unsigned long durationMs = strtoul(durationStr, NULL, 10);
  unsigned long startInMs = startInStr ? strtoul(startInStr, NULL, 10) : 0;

  if (strcmp(category, "SCENE") != 0) {
    sendFault(seq, "BAD_CATEGORY");
    return;
  }

  // Duplicate command protection. If ESP retries because ACK was lost, ACK again
  // but do not restart the scene.
  if (seq == lastCommandSeq) {
    sendAck(seq);
    return;
  }

  lastCommandSeq = seq;
  Scene scene = parseSceneName(sceneStr);
  sendAck(seq);
  scheduleScene(scene, seq, durationMs, startInMs);
}

void readEspSerial() {
  static char line[100];
  static uint8_t pos = 0;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[pos] = '\0';
      pos = 0;
      if (strlen(line) > 0) processCommandLine(line);
    } else if (c != '\r') {
      if (pos < sizeof(line) - 1) line[pos++] = c;
      else pos = 0;
    }
  }
}

/* ---------------- FAILSAFE ---------------- */

void processFailsafe() {
  if (!espContactSeen) return;

  if (espContactSeen && millis() - lastEspRxAt > ESP_LINK_TIMEOUT_MS) {
    if (!espTimeoutFaultSent) {
      sendFault(activeSeq, "ESP_TIMEOUT");
      espTimeoutFaultSent = true;
    }

    if (currentScene != SCENE_IDLE && currentScene != SCENE_STOP) {
      pendingActive = false;
      activateScene(SCENE_IDLE, activeSeq, 0);
    }
  }
}

/* ---------------- SETUP ---------------- */

void setup() {
  // AVR erratum: after a watchdog-triggered reset, WDIE/WDE can remain set
  // and cause an immediate re-reset loop unless cleared first thing.
  MCUSR = 0;
  wdt_disable();

  pinMode(WING_PWM_PIN, OUTPUT);
  pinMode(WING_IN1_PIN, OUTPUT);
  pinMode(WING_IN2_PIN, OUTPUT);
  pinMode(HOME_SENSOR_PIN, INPUT_PULLUP);

  wingStop();

  lipServo.attach(LIP_SERVO_PIN);
  lipClosed();

  Serial.begin(9600);
  dfSerial.begin(9600);
  delay(500);

  dfReady = dfPlayer.begin(dfSerial);
  if (dfReady) {
    dfPlayer.volume(25); // 0 to 30
  }

  randomSeed(analogRead(A0));

  // Do not start ESP timeout until first PING/CMD arrives.
  lastEspRxAt = 0;
  espContactSeen = false;

  activateScene(SCENE_IDLE, 0, 0);

  // Enable the watchdog last, once setup (including the DFPlayer handshake,
  // which can block for a while) is done, so a hang anywhere in loop() forces
  // a real reset instead of the "SAFE" claim in the header being aspirational.
  wdt_enable(WDTO_2S);
}

void loop() {
  wdt_reset();
  readEspSerial();
  processPendingScene();
  processHoming();
  processWingRamp();
  processLipServo();
  processDfPlayerEvents();
  processSceneDuration();
  processFailsafe();
  delay(5);
}
