/*
 * ARCCANUM - Receiver node (ESP32-WROOM-32E)
 *
 * Listens for alerts from the transmitter node over ESP-NOW, the ESP32's
 * built-in direct WiFi link (no router, no internet, no password needed).
 *
 *   "ARC,CRIT,<pH>,<seq>"  -> the receiver reads the pH value in the message
 *                             and, if it is also >= 12.0, switches the RED LED
 *                             on (critical alert).
 *   "ARC,COLOR,<pH>,<seq>" -> colour-confirmed alert (picric acid strip turned
 *                             red-rose): switches the GREEN LED on.
 *
 * An LED stays on while alerts keep arriving and switches off once no alert of
 * that kind has been received for ALERT_HOLD_MS.
 *
 * No extra libraries needed (WiFi and ESP-NOW come with the ESP32 board package).
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------
#define RED_LED_PIN    27
#define GREEN_LED_PIN  33

// ---------------------------------------------------------------------------
// Settings (WIFI_CHANNEL must match the transmitter)
// ---------------------------------------------------------------------------
const float PH_CRITICAL = 12.0;
const int   WIFI_CHANNEL = 1;

#define PACKET_HEADER "ARC"

// How long an LED stays on after the last matching alert.
const unsigned long ALERT_HOLD_MS = 15000;

unsigned long redOnSince = 0;    // time of the last critical alert
unsigned long greenOnSince = 0;  // time of the last colour-confirmed alert
bool redOn = false;
bool greenOn = false;

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
      redOn = true;
      redOnSince = millis();
      Serial.printf("  CRITICAL alert: pH %.2f -> RED LED ON\n", ph);
    } else {
      Serial.printf("  CRIT message with pH %.2f < %.1f, ignored\n", ph, PH_CRITICAL);
    }
  } else if (type == "COLOR") {
    greenOn = true;
    greenOnSince = millis();
    Serial.printf("  Colour-confirmed NaCN alert: pH %.2f -> GREEN LED ON\n", ph);
  } else {
    Serial.println("  ignored: unknown alert type");
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nARCCANUM receiver starting");

  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  while (esp_now_init() != ESP_OK) {
    // Blink both LEDs so a failure is visible without a serial monitor
    Serial.println("ESP-NOW init failed, retrying...");
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, HIGH);
    delay(250);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    delay(750);
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
  if (redOn && now - redOnSince >= ALERT_HOLD_MS) {
    redOn = false;
    Serial.println("No critical alert for a while -> RED LED OFF");
  }
  if (greenOn && now - greenOnSince >= ALERT_HOLD_MS) {
    greenOn = false;
    Serial.println("No colour alert for a while -> GREEN LED OFF");
  }

  digitalWrite(RED_LED_PIN, redOn ? HIGH : LOW);
  digitalWrite(GREEN_LED_PIN, greenOn ? HIGH : LOW);
}
