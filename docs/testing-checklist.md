# Testing Checklist

## Bench test without actuators

1. Flash ESP8266 hub.
2. Connect to Wi-Fi `GarurHub`.
3. Open `http://192.168.4.1`.
4. Check dashboard loads.
5. Open settings page.
6. Press manual test buttons: IDLE, NORMAL, FIGHT, CELEBRATION, SHOWCASE.

## Nano communication test

1. Flash Nano slave.
2. Disconnect Nano D0/D1 wires while uploading.
3. Reconnect UART wiring.
4. Open hub dashboard.
5. Confirm Nano status is OK.
6. Press NORMAL and check motor/audio response.
7. Press FIGHT and check fast wing + lip servo + fight audio.
8. Press IDLE and confirm magnetic home sensor stops wing.

## Sensor test

1. Flash Entry sensor with node ID 1.
2. Flash Exit sensor with node ID 2.
3. Confirm both use ESP-NOW channel 6.
4. On hub dashboard, Entry/Exit confidence should update and unknown packet count should stay 0.

## ArtNet test

1. Configure ArtNet node static IP: `192.168.4.50`.
2. Connect ArtNet node to `GarurHub` Wi-Fi.
3. Press hub manual buttons and confirm DMX output.
4. Check PAR channels 1-12 and relay channels 13-14.

## Field testing values

For fast testing:

```text
No People To Idle = 30 sec
Both Active Showcase Trigger = 20 sec
Fight Duration = 15 sec
Celebration Duration = 8 sec
Normal Intro = 10 sec
Normal Recovery = 15 sec
```

For final show:

```text
No People To Idle = 180 sec
Both Active Showcase Trigger = 180 sec
Fight Duration = 40 sec
Celebration Duration = 20 sec
Normal Intro = 20 sec
Normal Recovery = 60 sec
```
