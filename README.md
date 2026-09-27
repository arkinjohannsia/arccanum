# ARCCANUM – pH + colorimetric NaCN alert system

Two ESP32-WROOM-32E nodes linked by RA-02 (SX1278, 433 MHz) LoRa modules.

- **Transmitter** (`transmitter/transmitter.ino`): pH probe + amplifier, TCS34725 colour sensor, white LED, SSD1306 OLED, RA-02.
- **Receiver** (`receiver/receiver.ino`): RA-02, red LED, green LED.

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
 LoRa "CRIT"   LoRa "COLOR"
   │            │
   ▼            ▼
 Receiver: pH in packet ≥ 12.0 → RED LED     Receiver: GREEN LED
```

- The pH sensor is the primary gate. At pH ≥ 12.0 the alert goes out immediately, with no colour confirmation.
- The TCS34725 is only used in the intermediate range (the white LED is switched on just for that reading).
- The spec gives the intermediate range as "greater than 7.9 but less than 11.9". The code uses **7.9 < pH < 12.0**, so no gap is left between 11.9 and 12.0. Both limits are constants (`PH_COLOR_CHECK`, `PH_CRITICAL`) at the top of the transmitter sketch.
- While a condition persists, the alert is re-sent at most every 3 s (`ALERT_RESEND_MS`). The receiver keeps its LED lit while alerts keep arriving and turns it off 15 s after the last one (`ALERT_HOLD_MS`).

LoRa packet: `ARC,<CRIT|COLOR>,<pH>,<sequence>`, for example `ARC,CRIT,12.31,42`.

## Wiring

### Transmitter

| Part | Pin | ESP32 GPIO |
|---|---|---|
| RA-02 | SCK / MISO / MOSI / NSS | 18 / 19 / 23 / 5 |
| RA-02 | RST / DIO0 | 14 / 26 |
| RA-02 | VCC | **3.3 V only** |
| TCS34725 + SSD1306 (shared I2C) | SDA / SCL | 21 / 22 |
| pH amplifier (PO / analog out) | through a voltage divider | 34 |
| White LED (via resistor or transistor) | + | 25 |

The pH amplifier boards (e.g. PH-4502C) are powered from 5 V and can output more than 3.3 V. Put a voltage divider (for example 10 kΩ / 20 kΩ) between `PO` and GPIO 34 so the pin never goes above 3.3 V. The calibration below is done on the divided voltage, so the divider ratio does not need to be entered anywhere.

If your TCS34725 breakout has its own white LED, you can drive its `LED` pin from GPIO 25 instead of using a separate LED.

### Receiver

| Part | Pin | ESP32 GPIO |
|---|---|---|
| RA-02 | SCK / MISO / MOSI / NSS | 18 / 19 / 23 / 5 |
| RA-02 | RST / DIO0 | 14 / 26 |
| Red LED (via 220 Ω) | + | 27 |
| Green LED (via 220 Ω) | + | 33 |

Always attach the antenna before powering the RA-02 modules.

## Libraries

Install with the Arduino Library Manager:

- **LoRa** by Sandeep Mistry
- **Adafruit TCS34725**
- **Adafruit SSD1306** (installs Adafruit GFX and Adafruit BusIO)

Board: *ESP32 Dev Module* (esp32 core by Espressif).

## Calibration

### pH probe

1. Upload the transmitter sketch and open the Serial Monitor at 115200 baud.
2. Put the probe in a **pH 7.00** buffer and wait for the reading to settle. Note the `pH voltage` value.
3. Rinse, put it in a **pH 10.00** buffer and note the voltage. (You can use a different second buffer; change `CAL_PH_2` to match.)
4. Enter the values in `CAL_VOLTAGE_1` / `CAL_VOLTAGE_2` and upload again.

### Picric acid strip colour

Each intermediate-range reading prints `R G B C hue sat` to the Serial Monitor. Read an unreacted (yellowish) strip and a reacted (red-rose) strip under the white LED, then adjust these values if needed:

| Constant | Default | Meaning |
|---|---|---|
| `RED_ROSE_HUE_LOW` / `RED_ROSE_HUE_HIGH` | 330° / 25° | hue window for red-rose (it wraps around 0°); yellow is about 40–65° |
| `MIN_RED_TO_GREEN` | 1.40 | R/G ratio; yellow is close to 1.0, red-rose is clearly higher |
| `MIN_SATURATION` | 0.20 | rejects grey or washed-out readings |
| `MIN_CLEAR` | 100 | rejects readings that are too dark (LED off or no strip) |

Keep the sensor, LED and strip in a fixed, shaded holder so ambient light does not change the reading.
