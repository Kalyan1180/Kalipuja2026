# Required Libraries

## ESP8266 sensor node

Install ESP8266 board package in Arduino IDE.

Built-in/core libraries used:

```text
ESP8266WiFi
ESP8266WebServer
ESP8266HTTPClient
ESP8266HTTPUpdateServer
EEPROM
```

## ESP8266 hub brain

Install ESP8266 board package.

Libraries used:

```text
ESP8266WiFi
ESP8266WebServer
ESP8266HTTPClient
WiFiUdp
SoftwareSerial
EEPROM
```

## Arduino Nano slave

Install:

```text
DFRobotDFPlayerMini
Servo
SoftwareSerial
```

DFPlayer SD card layout -- folder-per-scene, addressed via
`dfPlayer.playFolder(folder, file)`. Folder names must be exactly 2 digits,
file names exactly 3 digits + extension -- DFPlayer Mini's own hardware
addressing convention, not a library choice:

```text
/01/001.mp3, /01/002.mp3, ... = Idle ambience clips
/02/001.mp3, /02/002.mp3, ... = Normal clips
/03/001.mp3, /03/002.mp3, ... = Fight clips
/04/001.mp3, /04/002.mp3, ... = Celebration clips
```

How many files are in each folder is set per scene from the Hub's
/settings page ("Sound clips in folder"); the Nano picks one at random
each time that scene activates. Idle additionally re-picks a fresh random
clip each time the current one finishes, for continuous (though varied
rather than identically repeating) ambience.


## v2 AI monitor
- ESP8266WiFi
- ESP8266HTTPClient
- WiFiClientSecure (bundled with the esp8266 core, used for Firebase/Groq HTTPS)
- ArduinoJson
- U8g2

All HTTP/HTTPS calls in this project (Hub, sensor nodes, AI monitor) use the
blocking `HTTPClient` API, not an async library — none of the boards use
`ESPAsyncTCP`/`ESPAsyncWebServer`. See `docs/network-and-test-plan.md` for
which calls can block for how long and why that's safe for the show.
