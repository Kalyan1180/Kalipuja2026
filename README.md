# Kalipuja2026 — Garur/Vishnu Automation

Firmware and documentation for the 2026 Kali Puja Garur/Vishnu automation project.

```text
ESP8266 Sensor Nodes  ->  ESP-NOW  ->  ESP8266 Hub Brain  ->  UART  ->  Arduino Nano Slave
                                            |
                                            +-> ArtNet DMX -> ArtNet/DMX Node -> PAR lights / relays
```

## Planned repository structure

```text
firmware/
  sensor-node/GarurSensorNode/
  hub-brain/GarurHubBrain_ESP8266_ArtNet_RC2/
  nano-slave/GarurNanoSlave_ArduinoNano/
  reference/

docs/
  architecture.md
  wiring.md
  dmx-patch.md
  testing-checklist.md
```

## Default settings

```text
ESP-NOW channel: 6
Hub AP SSID: GarurHub
Hub AP password: garur2026
Hub dashboard: http://192.168.4.1
Entry tracker node ID: 1
Exit tracker node ID: 2
Default ArtNet node IP: 192.168.4.50
ArtNet port: 6454
ArtNet universe: 0
```

## Controllers

- Sensor node: ESP8266 + HC-SR04 + servo scanning tracker.
- Hub brain: ESP8266 ESP-NOW receiver + Nano UART command + ArtNet DMX sender.
- Nano slave: wing motor, Garur lip servo, DFPlayer Mini, magnetic home sensor.
