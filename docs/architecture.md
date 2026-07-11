# Architecture

## High-level flow

```text
Entry Sensor Node  ----ESP-NOW----\
                                   > ESP8266 Hub Brain ----UART---- Arduino Nano Slave
Exit Sensor Node   ----ESP-NOW----/          |
                                             +----ArtNet UDP---- ArtNet/DMX Node
```

## State machine

Main states:

```text
IDLE
NORMAL
FIGHT
CELEBRATION
SHOWCASE_NORMAL_INTRO
SHOWCASE_FIGHT
SHOWCASE_CELEBRATION
SHOWCASE_RECOVERY
FAULT_SAFE
```

## Behavior rules

| Condition | Action |
|---|---|
| No entry and no exit for configured delay | IDLE |
| Exit active | NORMAL |
| Entry active for configured standing delay and exit clear | NORMAL |
| Entry + exit active for fight debounce | FIGHT then CELEBRATION |
| Entry + exit active for showcase delay | NORMAL -> FIGHT -> CELEBRATION -> NORMAL |
| Nano fault/link loss | FAULT_SAFE |

## Synchronization

The hub is the only state decision maker.

The Nano receives scene commands:

```text
CMD,<seq>,SCENE,<scene>,<durationMs>,<startInMs>
```

The Nano replies:

```text
ACK,<seq>
DONE,<seq>
FAULT,<seq>,<reason>
PONG,<timestamp>
```

The ArtNet node receives only DMX frames, not text cues.
