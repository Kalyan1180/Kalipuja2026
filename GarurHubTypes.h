#pragma once
#include <Arduino.h>

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

enum LightScene : uint8_t {
  LIGHT_IDLE = 0,
  LIGHT_NORMAL = 1,
  LIGHT_FIGHT = 2,
  LIGHT_CELEBRATION = 3,
  LIGHT_BLACKOUT = 4
};

// Effects are generic, reusable patterns -- not hardcoded per scene -- so
// any of the 4 configurable scenes (IDLE/NORMAL/FIGHT/CELEBRATION) can use
// any effect. "Secondary" color is only used by ALTERNATE/CHASE; effects
// that only need one color ignore it.
enum LightEffect : uint8_t {
  EFFECT_SOLID = 0,     // steady primary color, no animation
  EFFECT_BLINK = 1,     // primary color on/off, ~400ms period
  EFFECT_STROBE = 2,    // primary color on/off, fast (~120ms period)
  EFFECT_ALTERNATE = 3, // fixtures split into two groups (fixtureGroupMask): primary vs secondary, static
  EFFECT_CHASE = 4      // group-A fixtures cycle being "lit" with primary one at a time; group-B always secondary
};

// A scene's 4 PAR fixtures (channels 1-12, 3 each) can each be assigned to
// group A or B -- which fixtures land in which group is what ALTERNATE/
// CHASE actually animate between, rather than a fixed assumption like
// "1&3 vs 2&4". Bit (fixtureNumber-1) of fixtureGroupMask: 0=GROUP_A,
// 1=GROUP_B. Ignored by SOLID/BLINK/STROBE, which treat all 4 the same.
static const uint8_t FIXTURE_GROUP_A = 0;
static const uint8_t FIXTURE_GROUP_B = 1;

// What each of the 2 relays (channels 13/14) does during a scene --
// replaces the old fully-hardcoded per-scene relay logic and the
// fightStrobeRelayEnabled single-purpose toggle.
enum RelayFunction : uint8_t {
  RELAY_OFF = 0,
  RELAY_ON = 1,
  RELAY_BLINK = 2,  // ~400ms period, same timing as EFFECT_BLINK
  RELAY_STROBE = 3  // ~120ms period, same timing as EFFECT_STROBE
};

// Per-scene lighting config: colors, intensity, effect, which fixtures are
// in which group (for ALTERNATE/CHASE), and what each relay does.
struct SceneLight {
  uint8_t r, g, b;
  uint8_t r2, g2, b2;
  uint8_t intensity;
  uint8_t effect;
  uint8_t fixtureGroupMask; // bits 0-3 = fixtures 1-4; other bits unused
  uint8_t relay1Func, relay2Func;
};

// Mirrors GarurNanoSlave_v1_2.ino's WingMode/LipMode/NanoSceneCfg exactly --
// the two are separate sketches with no shared header, and these values
// cross the Hub<->Nano UART link as plain integers, so field order and
// numeric enum values must stay in sync by convention, not by the compiler.
enum WingMode : uint8_t { WING_MODE_STOP = 0, WING_MODE_SPIN = 1, WING_MODE_HOME = 2 };
enum LipMode  : uint8_t { LIP_MODE_CLOSED = 0, LIP_MODE_ANIMATE = 1 };
struct NanoSceneCfg {
  uint8_t wingMode;
  uint8_t wingSpeed;    // 0-255 PWM, used only when wingMode==WING_MODE_SPIN
  uint8_t lipMode;
  uint8_t dfFileCount;  // files in this scene's DFPlayer folder (folder number = scene index + 1), for random pick
};

struct TrackerRuntime {
  bool active;
  uint32_t activeSince;
  uint32_t lastPacketAt;
  uint32_t lastPresenceAt;
  uint8_t lastPresence;
  uint8_t lastConfidence;
  uint8_t lastCrowd;
  uint8_t lastZone;
  uint8_t lastMotion;
  uint8_t lastActivityScore;
  uint8_t lastEventType;
  bool nodeError;
  uint32_t nodeErrorCount;
  uint32_t lastEventId;
  char lastIp[16];
};

static_assert(sizeof(TrackerRuntime) <= 64, "TrackerRuntime unexpectedly large");
