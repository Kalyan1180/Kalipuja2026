# DMX / ArtNet Patch

The ESP8266 hub sends real ArtNet ArtDMX packets to the ArtNet node.

Default ArtNet target:

```text
IP: 192.168.4.50
Port: 6454
Universe: 0
```

## Fixture patch

```text
PAR 1   DMX 1-3    R=1  G=2  B=3
PAR 2   DMX 4-6    R=4  G=5  B=6
PAR 3   DMX 7-9    R=7  G=8  B=9
PAR 4   DMX 10-12  R=10 G=11 B=12
Relay 1 DMX 13     >127 = ON
Relay 2 DMX 14     >127 = ON
```

Suggested relay usage:

```text
Relay 1 -> Vishnu head light
Relay 2 -> optional strobe / special effect
```

## Scene lighting

IDLE: blue PARs, Vishnu head light ON, relay 2 OFF.

NORMAL: blue + amber/gold calm lighting, Vishnu head light ON.

FIGHT: Vishnu head light OFF, fast red/white PAR effect, optional relay 2 pulse.

CELEBRATION: soft gold PAR focus, Vishnu head blinks first, then stable ON.
