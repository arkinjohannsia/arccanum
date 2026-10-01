/*
 * ARCCANUM - Receiver node (ESP32-WROOM-32E)
 *
 * Listens for alerts from the transmitter node over ESP-NOW, the ESP32's
 * built-in direct WiFi link (no router, no internet, no password needed).
 *
 *   "ARC,CRIT,<pH>,<seq>"  -> the receiver reads the pH value in the message
 *                             and, if it is also >= 12.0, shows a flashing
 *                             CRITICAL alert with the pH on the OLED.
 *   "ARC,COLOR,<pH>,<seq>" -> colour-confirmed alert (picric acid strip turned
 *                             red-rose): switches the GREEN LED on.
 *
 * An alert stays active while alerts keep arriving and clears once no alert of
 * that kind has been received for ALERT_HOLD_MS.
 *
 * Library (Arduino Library Manager):
 *   - Adafruit SSD1306   (pulls in Adafruit GFX Library + Adafruit BusIO)
 * WiFi and ESP-NOW come with the ESP32 board package.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------
#define GREEN_LED_PIN  33

// SSD1306 OLED (critical alert display)
#define I2C_SDA    21
#define I2C_SCL    22
#define OLED_ADDRESS 0x3C

// ---------------------------------------------------------------------------
// Settings (WIFI_CHANNEL must match the transmitter)
// ---------------------------------------------------------------------------
const float PH_CRITICAL = 12.0;
const int   WIFI_CHANNEL = 1;

#define PACKET_HEADER "ARC"

// How long an alert stays active after the last matching message.
const unsigned long ALERT_HOLD_MS = 15000;

// The critical alert screen flashes (inverts) at this interval.
const unsigned long FLASH_MS = 500;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool displayOk = false;

unsigned long criticalSince = 0;  // time of the last critical alert
unsigned long greenOnSince = 0;   // time of the last colour-confirmed alert
bool criticalOn = false;
bool greenOn = false;
float criticalPh = 0;
float colorPh = 0;
bool lastFlashState = false;
bool screenDirty = true;          // redraw the OLED on the next loop

// The ESP-NOW callback runs in the WiFi task; it only copies the message here
// and loop() does the actual work.
char rxBuffer[64];
volatile bool rxPending = false;

// ---------------------------------------------------------------------------
// ESP-NOW receive callback (signature differs between ESP32 core 2.x and 3.x)
// ---------------------------------------------------------------------------
void storeMessage(const uint8_t *data, int len) {
  if (rxPending) return;  // previous message not handled yet; alerts repeat
  int n = min(len, (int)sizeof(rxBuffer) - 1);
  memcpy(rxBuffer, data, n);
  rxBuffer[n] = '\0';
  rxPending = true;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  storeMessage(data, len);
}
#else
void onReceive(const uint8_t *mac, const uint8_t *data, int len) {
  storeMessage(data, len);
}
#endif

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------
// Splits "ARC,<type>,<pH>,<seq>" into its fields. Returns false if malformed.
bool parsePacket(String packet, String &type, float &ph, long &seq) {
  int c1 = packet.indexOf(',');
  int c2 = packet.indexOf(',', c1 + 1);
  int c3 = packet.indexOf(',', c2 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0) return false;
  if (packet.substring(0, c1) != PACKET_HEADER) return false;

  type = packet.substring(c1 + 1, c2);
  String phText = packet.substring(c2 + 1, c3);
  if (phText.length() == 0) return false;
  ph = phText.toFloat();
  seq = packet.substring(c3 + 1).toInt();
  return ph >= 0.0 && ph <= 14.0;
}

void handlePacket(const String &packet) {
  String type;
  float ph;
  long seq;

  Serial.print("WiFi RX: ");
  Serial.println(packet);

  if (!parsePacket(packet, type, ph, seq)) {
    Serial.println("  ignored: not an ARCCANUM message");
    return;
  }

  if (type == "CRIT") {
    // Double-check the pH carried in the message before raising the alarm
    if (ph >= PH_CRITICAL) {
      criticalOn = true;
      criticalSince = millis();
      criticalPh = ph;
      screenDirty = true;
      Serial.printf("  CRITICAL alert: pH %.2f -> shown on OLED\n", ph);
    } else {
      Serial.printf("  CRIT message with pH %.2f < %.1f, ignored\n", ph, PH_CRITICAL);
    }
  } else if (type == "COLOR") {
    greenOn = true;
    greenOnSince = millis();
    colorPh = ph;
    screenDirty = true;
    Serial.printf("  Colour-confirmed NaCN alert: pH %.2f -> GREEN LED ON\n", ph);
  } else {
    Serial.println("  ignored: unknown alert type");
  }
}

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------
void drawScreen(bool flashOn) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (criticalOn) {
    display.setTextSize(2);
    display.setCursor(4, 0);
    display.print("!CRITICAL!");
    display.setTextSize(1);
    display.setCursor(22, 20);
    display.print("pH >= 12 DETECTED");
    display.setTextSize(2);
    display.setCursor(16, 34);
    display.print("pH ");
    display.print(criticalPh, 2);
    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print(greenOn ? "Strip alert also on" : "Check the water now");
    display.invertDisplay(flashOn);  // flashing alert
  } else {
    display.invertDisplay(false);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("ALERT RECEIVER");
    display.setTextSize(2);
    display.setCursor(0, 20);
    if (greenOn) {
      display.print("NaCN");
      display.setTextSize(1);
      display.setCursor(0, 38);
      display.print("Strip turned red-rose");
      display.setCursor(0, 47);
      display.print("pH ");
      display.print(colorPh, 2);
    } else {
      display.print("No alert");
      display.setTextSize(1);
      display.setCursor(0, 40);
      display.print("Monitoring...");
    }
    display.setCursor(0, 56);
    display.print("WiFi ch ");
    display.print(WIFI_CHANNEL);
  }
  display.display();
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nARCCANUM receiver starting");

  pinMode(GREEN_LED_PIN, OUTPUT);
  digitalWrite(GREEN_LED_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (!displayOk) Serial.println("SSD1306 OLED not found");
  drawScreen(false);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  while (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed, retrying...");
    if (displayOk) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("WiFi init failed");
      display.setCursor(0, 12);
      display.print("retrying...");
      display.display();
    }
    delay(1000);
  }
  esp_now_register_recv_cb(onReceive);

  Serial.print("Listening for alerts on WiFi channel ");
  Serial.println(WIFI_CHANNEL);
}

void loop() {
  if (rxPending) {
    String packet(rxBuffer);
    rxPending = false;
    handlePacket(packet);
  }

  unsigned long now = millis();
  if (criticalOn && now - criticalSince >= ALERT_HOLD_MS) {
    criticalOn = false;
    screenDirty = true;
    Serial.println("No critical alert for a while -> OLED alert cleared");
  }
  if (greenOn && now - greenOnSince >= ALERT_HOLD_MS) {
    greenOn = false;
    screenDirty = true;
    Serial.println("No colour alert for a while -> GREEN LED OFF");
  }

  digitalWrite(GREEN_LED_PIN, greenOn ? HIGH : LOW);

  // Redraw only when something changed, or to toggle the flashing alert
  bool flashOn = criticalOn && (now / FLASH_MS) % 2 == 0;
  if (screenDirty || flashOn != lastFlashState) {
    drawScreen(flashOn);
    lastFlashState = flashOn;
    screenDirty = false;
  }
}
