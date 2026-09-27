/*
 * ARCCANUM - pH calibration tool (ESP32-WROOM-32E)
 *
 * Upload this to the TRANSMITTER board (same wiring: pH amplifier PO -> GPIO 34,
 * OLED on SDA 21 / SCL 22). It shows the live probe voltage and pH on the
 * Serial Monitor and the OLED, and walks you through a two-point calibration.
 * The result is saved in the ESP32's flash memory; the transmitter sketch loads
 * it automatically, so no code needs to be edited afterwards.
 *
 * Serial Monitor: 115200 baud, line ending "Newline" (or "Both NL & CR").
 *
 * Commands (type and press Enter):
 *   7         probe is in pH 7.00 buffer (or BNC shorted) -> save point 1
 *   4 / 10    probe is in pH 4.00 / 10.00 buffer        -> save point 2
 *             (any buffer value works, e.g. 6.86 or 9.18)
 *   s         show the saved calibration
 *   r         erase the saved calibration (back to defaults)
 *
 * Library: Adafruit SSD1306 (the OLED is optional; the tool also works without it)
 */

#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define PH_PIN     34
#define I2C_SDA    21
#define I2C_SCL    22
#define OLED_ADDRESS 0x3C

// Defaults, used until a calibration is saved (same as the transmitter)
const float DEFAULT_PH_1 = 7.00;
const float DEFAULT_V_1  = 2.600;
const float DEFAULT_PH_2 = 10.00;
const float DEFAULT_V_2  = 2.060;

// The ESP32 cannot measure outside roughly 0.14 V - 3.13 V
const float ADC_MIN_V = 0.16;
const float ADC_MAX_V = 3.10;

// Readings must stay within this range (volts) over the last few seconds
// before they count as stable
const float STABLE_RANGE_V = 0.010;
const int   HISTORY = 10;             // readings kept for the stability check

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Preferences prefs;
bool displayOk = false;

float calPh1, calV1, calPh2, calV2;
bool calSaved = false;

// Point 1 captured in this session, waiting for point 2
bool havePoint1 = false;
float newPh1, newV1;

float history[HISTORY];
int historyCount = 0, historyIndex = 0;

// ---------------------------------------------------------------------------
float readVoltage() {
  // 40 samples, drop the 5 lowest and 5 highest, average the rest
  const int N = 40, DROP = 5;
  uint32_t s[N];
  for (int i = 0; i < N; i++) {
    s[i] = analogReadMilliVolts(PH_PIN);
    delay(5);
  }
  for (int i = 1; i < N; i++) {             // insertion sort
    uint32_t v = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
    s[j + 1] = v;
  }
  uint32_t sum = 0;
  for (int i = DROP; i < N - DROP; i++) sum += s[i];
  return sum / float(N - 2 * DROP) / 1000.0;
}

float voltageToPh(float v) {
  float slope = (calPh2 - calPh1) / (calV2 - calV1);
  return calPh1 + (v - calV1) * slope;
}

void addHistory(float v) {
  history[historyIndex] = v;
  historyIndex = (historyIndex + 1) % HISTORY;
  if (historyCount < HISTORY) historyCount++;
}

// Spread (max - min) of the recent readings, or -1 if not enough yet
float recentSpread() {
  if (historyCount < HISTORY) return -1;
  float lo = history[0], hi = history[0];
  for (int i = 1; i < HISTORY; i++) {
    lo = min(lo, history[i]);
    hi = max(hi, history[i]);
  }
  return hi - lo;
}

const char *signalState(float v, float spread) {
  if (v >= ADC_MAX_V) return "AT MAX";
  if (v <= ADC_MIN_V) return "AT MIN";
  if (spread < 0) return "WAIT";
  if (spread <= STABLE_RANGE_V) return "STABLE";
  return "MOVING";
}

// ---------------------------------------------------------------------------
void loadCalibration() {
  prefs.begin("phcal", true);
  calSaved = prefs.getBool("valid", false);
  calPh1 = prefs.getFloat("ph1", DEFAULT_PH_1);
  calV1  = prefs.getFloat("v1",  DEFAULT_V_1);
  calPh2 = prefs.getFloat("ph2", DEFAULT_PH_2);
  calV2  = prefs.getFloat("v2",  DEFAULT_V_2);
  prefs.end();
  if (!calSaved) {
    calPh1 = DEFAULT_PH_1; calV1 = DEFAULT_V_1;
    calPh2 = DEFAULT_PH_2; calV2 = DEFAULT_V_2;
  }
}

void saveCalibration(float ph1, float v1, float ph2, float v2) {
  prefs.begin("phcal", false);
  prefs.putFloat("ph1", ph1);
  prefs.putFloat("v1", v1);
  prefs.putFloat("ph2", ph2);
  prefs.putFloat("v2", v2);
  prefs.putBool("valid", true);
  prefs.end();
  loadCalibration();
}

void printCalibration() {
  float slopeMv = (calV2 - calV1) / (calPh2 - calPh1) * 1000.0;
  Serial.println();
  Serial.println(calSaved ? "Saved calibration:" : "No calibration saved, using defaults:");
  Serial.printf("  pH %.2f = %.3f V\n", calPh1, calV1);
  Serial.printf("  pH %.2f = %.3f V\n", calPh2, calV2);
  Serial.printf("  slope: %.1f mV per pH unit\n", slopeMv);
  Serial.println("  Same values as code lines (only needed if you don't use saved memory):");
  Serial.printf("    const float CAL_PH_1      = %.2f;\n", calPh1);
  Serial.printf("    const float CAL_VOLTAGE_1 = %.3f;\n", calV1);
  Serial.printf("    const float CAL_PH_2      = %.2f;\n", calPh2);
  Serial.printf("    const float CAL_VOLTAGE_2 = %.3f;\n", calV2);
  Serial.println();
}

void printHelp() {
  Serial.println();
  Serial.println("=== pH CALIBRATION ===");
  Serial.println("1. Put the probe in pH 7.00 buffer (or short the BNC socket),");
  Serial.println("   wait until the line says STABLE, then type 7 and press Enter.");
  Serial.println("2. Rinse the probe, put it in the second buffer (pH 4.00 or 10.00),");
  Serial.println("   wait for STABLE, then type 4 or 10 and press Enter.");
  Serial.println("Other commands: s = show calibration, r = reset to defaults");
  Serial.println();
}

// Captures the current voltage for the given buffer pH
void capturePoint(float bufferPh) {
  // Fresh reading, independent of the display loop
  float v = readVoltage();
  float spread = recentSpread();

  if (v >= ADC_MAX_V || v <= ADC_MIN_V) {
    Serial.printf(">> NOT saved: voltage %.3f V is at the ESP32's limit.\n", v);
    Serial.println("   The probe signal is out of range. Check that the probe is in the");
    Serial.println("   liquid, the cap is off, and the plug is locked on the BNC socket.");
    return;
  }
  if (spread < 0 || spread > STABLE_RANGE_V) {
    Serial.println(">> Warning: reading is still moving. For best results wait for STABLE.");
  }

  if (!havePoint1) {
    newPh1 = bufferPh;
    newV1 = v;
    havePoint1 = true;
    Serial.printf(">> Point 1 saved: pH %.2f = %.3f V\n", bufferPh, v);
    Serial.println("   Rinse the probe, put it in the second buffer, wait for STABLE,");
    Serial.println("   then type that buffer's pH (e.g. 4 or 10) and press Enter.");
    return;
  }

  if (fabs(bufferPh - newPh1) < 1.0) {
    Serial.println(">> NOT saved: the two buffers must differ by at least 1 pH unit.");
    return;
  }

  float slopeMv = (v - newV1) / (bufferPh - newPh1) * 1000.0;
  Serial.printf(">> Point 2: pH %.2f = %.3f V  (slope %.1f mV/pH)\n", bufferPh, v, slopeMv);

  // A working probe + amplifier gives roughly -100 to -250 mV per pH unit
  // (voltage goes DOWN as pH goes UP). Anything else means a bad reading.
  if (slopeMv > -50 || slopeMv < -400) {
    Serial.println(">> NOT saved: that slope is not possible for a working probe.");
    Serial.println("   Check that each buffer's pH was typed correctly and the probe");
    Serial.println("   was rinsed between buffers. Point 1 is kept; try point 2 again,");
    Serial.println("   or type r to start over.");
    return;
  }

  saveCalibration(newPh1, newV1, bufferPh, v);
  havePoint1 = false;
  Serial.println(">> CALIBRATION SAVED to ESP32 memory.");
  Serial.println("   Now upload the transmitter sketch; it loads these values by itself.");
  printCalibration();
}

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  if (cmd == "s") {
    printCalibration();
  } else if (cmd == "r") {
    prefs.begin("phcal", false);
    prefs.clear();
    prefs.end();
    havePoint1 = false;
    loadCalibration();
    Serial.println(">> Calibration erased, back to defaults.");
    printCalibration();
  } else if (cmd == "h" || cmd == "?") {
    printHelp();
  } else {
    float ph = cmd.toFloat();
    if (ph >= 1.0 && ph <= 13.0) {
      capturePoint(ph);
    } else {
      Serial.println(">> Unknown command. Type 7, 4 or 10 (buffer pH), s, r or h.");
    }
  }
}

// ---------------------------------------------------------------------------
void showScreen(float v, float ph, const char *state) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("pH CALIBRATION");

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(v, 3);
  display.print(" V");
  display.setCursor(0, 34);
  display.print("pH ");
  display.print(ph, 2);

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(state);
  if (havePoint1) {
    display.setCursor(64, 56);
    display.print("P1 done");
  } else if (calSaved) {
    display.setCursor(64, 56);
    display.print("cal saved");
  }
  display.display();
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(300);

  analogReadResolution(12);
  analogSetPinAttenuation(PH_PIN, ADC_11db);

  Wire.begin(I2C_SDA, I2C_SCL);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);

  loadCalibration();
  printHelp();
  printCalibration();
}

void loop() {
  if (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }

  float v = readVoltage();
  addHistory(v);
  float spread = recentSpread();
  const char *state = signalState(v, spread);
  float ph = voltageToPh(v);

  Serial.printf("Voltage: %.3f V   pH: %5.2f   %s", v, ph, state);
  if (spread >= 0) Serial.printf("  (moved %.0f mV)", spread * 1000.0);
  if (v >= ADC_MAX_V) Serial.print("  <- probe signal too high, check probe");
  if (v <= ADC_MIN_V) Serial.print("  <- probe signal too low, check probe");
  Serial.println();

  showScreen(v, ph, state);
  delay(300);
}
