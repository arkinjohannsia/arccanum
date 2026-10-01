# ARCCANUM – pH + colorimetric NaCN alert system

Two ESP32-WROOM-32E boards linked directly over WiFi using ESP-NOW, the ESP32's built-in board-to-board radio link. It needs no router, internet, password or extra modules.

- **Transmitter** (`transmitter/transmitter.ino`): pH probe + amplifier, TCS34725 colour sensor, white LED, SSD1306 OLED.
- **Receiver** (`receiver/receiver.ino`): SSD1306 OLED (critical alert), green LED.

## Detection logic

```
             read pH  ──►  show on OLED
                │
   ┌────────────┼─────────────────────────┐
 pH ≥ 12.0   7.9 < pH < 12.0          pH ≤ 7.9
   │            │                         │
   │     white LED ON, TCS34725 reads     no action,
   │     the picric acid strip            back to start
   │            │
   │     red-rose? ──no──► no alert, back to start
   │            │yes
   │     OLED: "NaCN DETECTED!"
   ▼            ▼
 WiFi "CRIT"   WiFi "COLOR"
   │            │
   ▼            ▼
 Receiver: pH in message ≥ 12.0 → flashing OLED alert     Receiver: GREEN LED
```

- The pH sensor is the primary gate. At pH ≥ 12.0 the alert goes out immediately, with no colour confirmation.
- The TCS34725 is only used in the intermediate range (the white LED is switched on just for that reading).
- The spec gives the intermediate range as "greater than 7.9 but less than 11.9". The code uses **7.9 < pH < 12.0**, so no gap is left between 11.9 and 12.0. Both limits are constants (`PH_COLOR_CHECK`, `PH_CRITICAL`) at the top of the transmitter sketch.
- While a condition persists, the alert is re-sent at most every 3 s (`ALERT_RESEND_MS`). The receiver keeps its alert (OLED or LED) on while alerts keep arriving and clears it 15 s after the last one (`ALERT_HOLD_MS`).

WiFi message: `ARC,<CRIT|COLOR>,<pH>,<sequence>`, for example `ARC,CRIT,12.31,42`. It is broadcast on WiFi channel 1 (`WIFI_CHANNEL`, which must be the same in both sketches), so neither board needs to know the other's address. Broadcasts aren't acknowledged, but the alert repeats every 3 s while the condition lasts.

## Wiring

### Transmitter

| Part | Pin | ESP32 GPIO |
|---|---|---|
| TCS34725 + SSD1306 (shared I2C) | SDA / SCL | 21 / 22 |
| pH amplifier (PO / analog out) | direct | 34 |
| White LED (via resistor or transistor) | + | 25 |

If your TCS34725 breakout has its own white LED, you can drive its `LED` pin from GPIO 25 instead of using a separate LED.

### Receiver

| Part | Pin | ESP32 GPIO |
|---|---|---|
| SSD1306 OLED | SDA / SCL | 21 / 22 |
| Green LED (via 220 Ω) | + | 33 |

## Libraries

WiFi and ESP-NOW come with the ESP32 board package. Also install these from the Arduino Library Manager (the receiver only needs Adafruit SSD1306):

- **Adafruit TCS34725**
- **Adafruit SSD1306** (installs Adafruit GFX and Adafruit BusIO)

Board: *ESP32 Dev Module* (esp32 core by Espressif).

## Default values

The code works as-is with the wiring above; nothing needs to be added or measured.

- **pH:** uses the standard pH amplifier response (2.60 V at pH 7 on this board, falling about 0.18 V per pH unit).
- **Strip colour:** counts the strip as red-rose when its hue is 330–360° or 0–25° and red is at least 1.4 times green. Yellow is about 40–65°.

Every reading (pH voltage, pH, and the TCS34725 R/G/B/C, hue and saturation values) is printed on the Serial Monitor at 115200 baud. All of these limits are constants at the top of `transmitter/transmitter.ino`.

## pH calibration tool (optional)

`calibration/calibration.ino` runs on the transmitter board with the same wiring. It shows the live probe voltage and pH on the Serial Monitor (115200 baud, line ending "Newline") and on the OLED, and saves a two-point calibration to the ESP32's flash memory:

1. Probe in pH 7.00 buffer (or the BNC socket shorted), wait for `STABLE`, type `7` + Enter.
2. Rinse, probe in pH 4.00 or 10.00 buffer, wait for `STABLE`, type `4` or `10` + Enter.
3. Upload the transmitter sketch again. It loads the saved calibration at startup.

`s` shows the saved calibration and `r` erases it. Readings stuck at the ESP32's limits (about 0.14 V or 3.13 V) are refused, because they mean the probe signal is out of range.
