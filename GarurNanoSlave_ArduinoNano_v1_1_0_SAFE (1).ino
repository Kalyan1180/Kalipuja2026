/*
  Garur Actuator Slave - Arduino Nano v1.1.0 SAFE

  Works with ESP8266 Garur Hub Brain v1.3.0-RC2.

  Receives commands from ESP8266 HUB over hardware Serial:
    CMD,<seq>,SCENE,<scene>,<durationMs>,<startInMs>

  Sends:
    ACK,<seq>
    DONE,<seq>
    FAULT,<seq>,<reason>
    PONG,<ms>

  Scene behavior:
    IDLE        -> home wings using magnetic sensor, Vishnu/Garur rest position
    NORMAL      -> slow wings + calm sound loop
    FIGHT       -> fast wings + lip servo + fight sound
    CELEBRATION -> wings stop + victory sound
    STOP        -> all actuators safe stop

  Upload note:
  - Disconnect Nano D0/D1 serial wires while uploading.
  - Reconnect after upload.
*/

#include <Arduino.h>
#include <Servo.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

/* ---------------- PIN CONFIG ---------------- */

const uint8_t WING_PWM_PIN = 5;   // PWM
const uint8_t WING_IN1_PIN = 7;
const uint8_t WING_IN2_PIN = 8;

// Reed switch / Hall sensor output to GND when active. INPUT_PULLUP = active LOW.
const uint8_t HOME_SENSOR_PIN = 2;

const uint8_t LIP_SERVO_PIN = 9;

// DFPlayer Mini on SoftwareSerial.
// Nano D10 receives DFPlayer TX.
// Nano D11 sends to DFPlayer RX through 1k resistor.
const uint8_t DF_RX_PIN = 10;
const uint8_t DF_TX_PIN = 11;

/* ---------------- SPEEDS / POSITIONS ---------------- */

const uint8_t WING_NORMAL_PWM = 90;
const uint8_t WING_FIGHT_PWM  = 210;
const uint8_t WING_HOME_PWM   = 80;

const int LIP_CLOSED_ANGLE = 35;
const int LIP_OPEN_MIN     = 55;
const int LIP_OPEN_MAX     = 105;

// Reduced from 12s to 6s to reduce stall risk if wing is jammed.
const unsigned long HOME_TIMEOUT_MS = 6000UL;
const unsigned long ESP_LINK_TIMEOUT_MS = 15000UL;

/* ---------------- DFPLAYER TRACK MAP ---------------- */

// SD card format:
// /mp3/0001.mp3 = calm Garur bird / ambience
// /mp3/0002.mp3 = fight sound
// /mp3/0003.mp3 = victory / celebration sound
// /mp3/0004.mp3 = idle low ambience
const uint8_t TRACK_NORMAL      = 1;
const uint8_t TRACK_FIGHT       = 2;
const uint8_t TRACK_CELEBRATION = 3;
const uint8_t TRACK_IDLE        = 4;

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
  delay(60); // DFPlayer clone modules often need a short settle before new play/loop.
}

void playLoopSafe(uint8_t track) {
  if (!dfReady) return;
  dfStopAndSettle();
  dfPlayer.loop(track);
}

void playOnceSafe(uint8_t track) {
  if (!dfReady) return;
  dfStopAndSettle();
  dfPlayer.play(track);
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
  writeLipAngle(LIP_CLOSED_ANGLE);
}

void processLipServo() {
  if (currentScene != SCENE_FIGHT) {
    lipClosed();
    return;
  }

  unsigned long now = millis();
  if (now < nextLipMoveAt) return;

  int angle = random(LIP_OPEN_MIN, LIP_OPEN_MAX + 1);
  writeLipAngle(angle);
  nextLipMoveAt = now + random(80, 180);
}

/* ---------------- HOMING ---------------- */

void startHoming(uint32_t seq) {
  homingActive = true;
  homingStartedAt = millis();
  homingSeq = seq;

  if (homeSensorActive()) {
    wingStop();
    homingActive = false;
    sendDone(seq);
    doneSent = true;
    return;
  }

  wingForward(WING_HOME_PWM);
}

void processHoming() {
  if (!homingActive) return;

  if (homeSensorActive()) {
    wingStop();
    homingActive = false;
    sendDone(homingSeq);
    doneSent = true;
    return;
  }

  if (millis() - homingStartedAt > HOME_TIMEOUT_MS) {
    wingStop();
    homingActive = false;
    sendFault(homingSeq, "HOME_TIMEOUT");
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

  switch (scene) {
    case SCENE_IDLE:
      lipClosed();
      playLoopSafe(TRACK_IDLE);
      startHoming(seq);
      break;

    case SCENE_NORMAL:
      lipClosed();
      wingForward(WING_NORMAL_PWM);
      playLoopSafe(TRACK_NORMAL);
      break;

    case SCENE_FIGHT:
      wingForward(WING_FIGHT_PWM);
      playOnceSafe(TRACK_FIGHT);
      nextLipMoveAt = millis();
      break;

    case SCENE_CELEBRATION:
      wingStop();
      lipClosed();
      playOnceSafe(TRACK_CELEBRATION);
      break;

    case SCENE_STOP:
    default:
      wingStop();
      lipClosed();
      stopAudio();
      sendDone(seq);
      doneSent = true;
      break;
  }
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
    if (currentScene == SCENE_FIGHT || currentScene == SCENE_CELEBRATION) {
      wingStop();
      lipClosed();
    }
    sendDone(activeSeq);
    doneSent = true;
  }
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

  if (millis() - lastEspRxAt > ESP_LINK_TIMEOUT_MS) {
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
}

void loop() {
  readEspSerial();
  processPendingScene();
  processHoming();
  processLipServo();
  processSceneDuration();
  processFailsafe();
  delay(5);
}
