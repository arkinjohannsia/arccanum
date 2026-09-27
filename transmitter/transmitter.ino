/*
 * ARCCANUM - Transmitter node (ESP32-WROOM-32E)
 *
 * Continuously reads the water pH through the pH probe + amplifier board and
 * shows it on the SSD1306 OLED. The pH value is the primary decision gate:
 *
 *   pH >= 12.0          -> CRITICAL. Send a LoRa alert immediately, without
 *                          waiting for the TCS34725 colour sensor.
 *   7.9 < pH < 12.0     -> Intermediate range. Switch on the white LED, let the
 *                          TCS34725 read the picric acid strip and compare it
 *                          with the programmed colour criteria. If the strip
 *                          turned from yellowish to red-rose, NaCN is confirmed:
 *                          show it on the OLED and send a LoRa alert. Otherwise
 *                          no alert is sent.
 *   pH <= 7.9           -> Outside the detection range. No alert.
 *
 * After every pass the loop starts over and reads the pH again.
 *
 * Libraries (Arduino Library Manager):
 *   - LoRa               by Sandeep Mistry
 *   - Adafruit TCS34725
 *   - Adafruit SSD1306   (pulls in Adafruit GFX Library + Adafruit BusIO)
 */

#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <Adafruit_TCS34725.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------------------------------------------------------------------
// Pin map (see README.md for the full wiring table)
// ---------------------------------------------------------------------------
// RA-02 (SX1278) on the VSPI bus
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_NSS    5
#define LORA_RST   14
#define LORA_DIO0  26

// I2C bus shared by the TCS34725 and the SSD1306 OLED
#define I2C_SDA    21
#define I2C_SCL    22

// pH amplifier analog output (PO), wired directly to this pin
#define PH_PIN     34

// White illumination LED for the TCS34725 (or the breakout's LED pin)
#define WHITE_LED_PIN 25

// ---------------------------------------------------------------------------
// Detection thresholds
// ---------------------------------------------------------------------------
const float PH_CRITICAL     = 12.0;  // pH >= this -> immediate critical alert
const float PH_COLOR_CHECK  = 7.9;   // pH >  this (and < PH_CRITICAL) -> check strip

// ---------------------------------------------------------------------------
// pH conversion
//
// Standard response of the pH probe + amplifier board with its output wired
// straight to the ESP32: 2.60 V at pH 7 on this board, dropping about 0.18 V per pH
// unit as the water gets more alkaline (pH 10 -> about 2.06 V).
// ---------------------------------------------------------------------------
const float CAL_PH_1      = 7.00;
const float CAL_VOLTAGE_1 = 2.600;   // volts at the pin at pH 7.00 (measured, BNC shorted)
const float CAL_PH_2      = 10.00;
const float CAL_VOLTAGE_2 = 2.060;   // volts at the pin at pH 10.00

const int PH_SAMPLES = 20;           // ADC samples per pH reading

// ---------------------------------------------------------------------------
// Colour criteria for the picric acid strip (yellowish -> red-rose)
//
// The strip colour is judged by hue, saturation and brightness computed from
// the TCS34725 raw RGBC values. Yellow sits around 40-65 deg of hue; red-rose
// wraps around 0 deg (roughly 330-360 and 0-25). Tune these with the values
// printed on the Serial Monitor for your own strips and lighting.
// ---------------------------------------------------------------------------
const float RED_ROSE_HUE_LOW   = 330.0;  // hue >= this counts as red-rose ...
const float RED_ROSE_HUE_HIGH  = 25.0;   // ... or hue <= this
const float MIN_SATURATION     = 0.20;   // reject washed-out / grey readings
const float MIN_RED_TO_GREEN   = 1.40;   // R/G ratio; yellow is close to 1.0
const uint16_t MIN_CLEAR       = 100;    // reject readings that are too dark

const int COLOR_SAMPLES        = 3;      // TCS readings averaged per check
const unsigned long LED_SETTLE_MS = 150; // let the white LED stabilise

// ---------------------------------------------------------------------------
// LoRa settings (must match the receiver)
// ---------------------------------------------------------------------------
const long LORA_FREQUENCY   = 433E6;  // RA-02 is a 433 MHz module
const int  LORA_SF          = 9;
const long LORA_BANDWIDTH   = 125E3;
const int  LORA_SYNC_WORD   = 0x3A;   // private network id, keeps other nodes out
const int  LORA_TX_POWER    = 17;     // dBm

// Packet format: "ARC,<type>,<pH>,<seq>"   e.g. "ARC,CRIT,12.31,42"
//   type CRIT  -> pH >= 12.0, sent without colour confirmation
//   type COLOR -> intermediate pH, confirmed by the red-rose strip colour
#define PACKET_HEADER "ARC"

// Minimum time between two alerts while the condition persists, so the radio
// is not flooded. The receiver keeps its LED on while alerts keep arriving.
const unsigned long ALERT_RESEND_MS = 3000;

const unsigned long LOOP_INTERVAL_MS = 1000;

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_TCS34725 tcs(TCS34725_INTEGRATIONTIME_154MS, TCS34725_GAIN_4X);

bool displayOk = false;
bool tcsOk = false;
bool loraOk = false;

uint32_t packetSeq = 0;
unsigned long lastAlertMs = 0;
bool alertSentBefore = false;
const char *lastAlertType = "";

enum Status {
  STATUS_NORMAL,        // pH <= 7.9
  STATUS_CHECKING,      // 7.9 < pH < 12.0, strip not red-rose
  STATUS_NACN,          // 7.9 < pH < 12.0, strip red-rose -> NaCN confirmed
  STATUS_CRITICAL,      // pH >= 12.0
  STATUS_SENSOR_ERROR   // intermediate range but the TCS34725 is missing
};

struct ColorReading {
  uint16_t r, g, b, c;
  float hue;         // 0-360 deg
  float saturation;  // 0-1
  bool redRose;
};

// ---------------------------------------------------------------------------
// pH
// ---------------------------------------------------------------------------
float readPhVoltage() {
  // Take several samples, drop the lowest and highest, average the rest.
  uint32_t samples[PH_SAMPLES];
  for (int i = 0; i < PH_SAMPLES; i++) {
    samples[i] = analogReadMilliVolts(PH_PIN);
    delay(10);
  }
  uint32_t minV = samples[0], maxV = samples[0], sum = 0;
  for (int i = 0; i < PH_SAMPLES; i++) {
    sum += samples[i];
    if (samples[i] < minV) minV = samples[i];
    if (samples[i] > maxV) maxV = samples[i];
  }
  sum -= minV + maxV;
  return (sum / float(PH_SAMPLES - 2)) / 1000.0;
}

float voltageToPh(float voltage) {
  float slope = (CAL_PH_2 - CAL_PH_1) / (CAL_VOLTAGE_2 - CAL_VOLTAGE_1);
  float ph = CAL_PH_1 + (voltage - CAL_VOLTAGE_1) * slope;
  ph = constrain(ph, 0.0, 14.0);
  // Round to the 2 decimals shown on the OLED and sent over LoRa, so the
  // threshold decision always matches the displayed/transmitted value.
  return roundf(ph * 100.0) / 100.0;
}

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------
void computeHueSaturation(ColorReading &cr) {
  float r = cr.r, g = cr.g, b = cr.b;
  float maxC = max(r, max(g, b));
  float minC = min(r, min(g, b));
  float delta = maxC - minC;

  cr.saturation = (maxC > 0) ? delta / maxC : 0;

  if (delta == 0) {
    cr.hue = 0;
  } else if (maxC == r) {
    cr.hue = 60.0 * fmod((g - b) / delta, 6.0);
  } else if (maxC == g) {
    cr.hue = 60.0 * ((b - r) / delta + 2.0);
  } else {
    cr.hue = 60.0 * ((r - g) / delta + 4.0);
  }
  if (cr.hue < 0) cr.hue += 360.0;
}

bool matchesRedRose(const ColorReading &cr) {
  if (cr.c < MIN_CLEAR) return false;
  if (cr.saturation < MIN_SATURATION) return false;
  if (cr.g == 0 || float(cr.r) / cr.g < MIN_RED_TO_GREEN) return false;
  return cr.hue >= RED_ROSE_HUE_LOW || cr.hue <= RED_ROSE_HUE_HIGH;
}

ColorReading readStripColor() {
  ColorReading cr = {};

  digitalWrite(WHITE_LED_PIN, HIGH);
  delay(LED_SETTLE_MS);

  uint32_t sumR = 0, sumG = 0, sumB = 0, sumC = 0;
  for (int i = 0; i < COLOR_SAMPLES; i++) {
    uint16_t r, g, b, c;
    tcs.getRawData(&r, &g, &b, &c);  // blocks for one integration period
    sumR += r; sumG += g; sumB += b; sumC += c;
  }

  digitalWrite(WHITE_LED_PIN, LOW);

  cr.r = sumR / COLOR_SAMPLES;
  cr.g = sumG / COLOR_SAMPLES;
  cr.b = sumB / COLOR_SAMPLES;
  cr.c = sumC / COLOR_SAMPLES;
  computeHueSaturation(cr);
  cr.redRose = matchesRedRose(cr);
  return cr;
}

// ---------------------------------------------------------------------------
// LoRa
// ---------------------------------------------------------------------------
bool initLora() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY)) return false;
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.enableCrc();
  return true;
}

// Returns true when a packet actually went out.
bool sendAlert(const char *type, float ph) {
  if (!loraOk) {
    Serial.println("LoRa not available, alert NOT sent");
    return false;
  }
  unsigned long now = millis();
  if (alertSentBefore && strcmp(type, lastAlertType) == 0 &&
      now - lastAlertMs < ALERT_RESEND_MS) {
    return false;  // same condition still active, alert was sent recently
  }

  char packet[40];
  snprintf(packet, sizeof(packet), "%s,%s,%.2f,%lu",
           PACKET_HEADER, type, ph, (unsigned long)packetSeq++);

  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();

  lastAlertMs = now;
  lastAlertType = type;
  alertSentBefore = true;
  Serial.print("LoRa TX: ");
  Serial.println(packet);
  return true;
}

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------
void showScreen(float ph, Status status, const ColorReading *cr, bool alertSent) {
  if (!displayOk) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("pH MONITOR");
  display.setCursor(80, 0);
  display.print(loraOk ? "LoRa OK" : "LoRa ERR");

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print("pH ");
  display.print(ph, 2);

  display.setTextSize(1);
  display.setCursor(0, 36);
  switch (status) {
    case STATUS_NORMAL:
      display.print("Status: NORMAL");
      break;
    case STATUS_CHECKING:
      display.print("Strip: no change");
      break;
    case STATUS_NACN:
      display.print("NaCN DETECTED!");
      break;
    case STATUS_CRITICAL:
      display.print("CRITICAL pH >= 12!");
      break;
    case STATUS_SENSOR_ERROR:
      display.print("TCS34725 ERROR");
      break;
  }

  display.setCursor(0, 46);
  if (cr) {
    display.print("Hue ");
    display.print(cr->hue, 0);
    display.print(cr->redRose ? " RED-ROSE" : " yellow");
  }

  display.setCursor(0, 56);
  if (status == STATUS_CRITICAL || status == STATUS_NACN) {
    display.print(alertSent ? "Alert sent via LoRa" : "Alert active");
  }

  display.display();
}

void showBootError(const char *msg) {
  Serial.println(msg);
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(msg);
  display.display();
  delay(1500);
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nARCCANUM transmitter starting");

  pinMode(WHITE_LED_PIN, OUTPUT);
  digitalWrite(WHITE_LED_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(PH_PIN, ADC_11db);  // full 0-3.3 V range

  Wire.begin(I2C_SDA, I2C_SCL);

  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (!displayOk) Serial.println("SSD1306 not found");

  tcsOk = tcs.begin();
  if (!tcsOk) showBootError("TCS34725 not found");

  loraOk = initLora();
  if (!loraOk) showBootError("RA-02 LoRa init failed");

  Serial.println("Ready");
}

void loop() {
  unsigned long start = millis();

  // 1. Read the pH (primary decision gate)
  float voltage = readPhVoltage();
  float ph = voltageToPh(voltage);

  Serial.print("pH voltage: ");
  Serial.print(voltage, 3);
  Serial.print(" V  pH: ");
  Serial.println(ph, 2);

  Status status;
  ColorReading color;
  bool haveColor = false;
  bool alertSent = false;

  if (ph >= PH_CRITICAL) {
    // 2a. Critical: alert immediately, no colour confirmation
    status = STATUS_CRITICAL;
    alertSent = sendAlert("CRIT", ph);

  } else if (ph > PH_COLOR_CHECK) {
    // 2b. Intermediate range: confirm with the picric acid strip
    if (!tcsOk) {
      status = STATUS_SENSOR_ERROR;
    } else {
      color = readStripColor();
      haveColor = true;

      Serial.printf("TCS R:%u G:%u B:%u C:%u  hue:%.1f sat:%.2f -> %s\n",
                    color.r, color.g, color.b, color.c,
                    color.hue, color.saturation,
                    color.redRose ? "RED-ROSE" : "no change");

      if (color.redRose) {
        status = STATUS_NACN;
        alertSent = sendAlert("COLOR", ph);
      } else {
        status = STATUS_CHECKING;  // no colour change -> no alert
      }
    }

  } else {
    // 2c. pH <= 7.9: outside the detection range, keep monitoring
    status = STATUS_NORMAL;
  }

  // Once the alert condition clears, the next alert goes out without waiting.
  if (status != STATUS_CRITICAL && status != STATUS_NACN) {
    alertSentBefore = false;
  }

  showScreen(ph, status, haveColor ? &color : nullptr, alertSent);

  // 3. Back to the start of the monitoring cycle
  unsigned long elapsed = millis() - start;
  if (elapsed < LOOP_INTERVAL_MS) delay(LOOP_INTERVAL_MS - elapsed);
}
