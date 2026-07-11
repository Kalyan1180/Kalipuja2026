# Wiring

## ESP8266 Hub to Arduino Nano

```text
ESP8266 D5 / GPIO14 TX  -> Nano RX / D0
Nano TX / D1            -> voltage divider -> ESP8266 D6 / GPIO12 RX
ESP8266 GND             -> Nano GND
```

Nano TX is 5V. ESP8266 RX is 3.3V. Use voltage divider:

```text
Nano TX ---- 1kΩ ---- ESP8266 RX
                  |
                 2kΩ
                  |
                 GND
```

For long wire or noisy pandal wiring, replace direct UART with RS485 modules.

## Sensor node wiring

```text
HC-SR04 TRIG  -> D5 / GPIO14
HC-SR04 ECHO  -> D6 / GPIO12 through voltage divider
Servo signal  -> D7 / GPIO13
Hub LED       -> D1 / GPIO5
Detect LED    -> D2 / GPIO4
```

HC-SR04 ECHO voltage divider:

```text
ECHO ---- 1kΩ ---- ESP8266 D6/GPIO12
              |
             2kΩ
              |
             GND
```

Servo must use external 5V supply. Do not power servo from ESP8266 3.3V.

## Nano actuator wiring

```text
Wing PWM      -> D5
Wing IN1      -> D7
Wing IN2      -> D8
Home sensor   -> D2, active LOW, INPUT_PULLUP
Lip servo     -> D9
DFPlayer RX   -> Nano D11 through 1k resistor
DFPlayer TX   -> Nano D10
```

Use common GND across all modules.
