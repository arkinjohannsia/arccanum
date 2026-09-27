/*
 * ARCCANUM - Receiver node (ESP32-WROOM-32E)
 *
 * Listens on the RA-02 LoRa module for alerts from the transmitter node.
 *
 *   "ARC,CRIT,<pH>,<seq>"  -> the receiver reads the pH value in the packet and,
 *                             if it is also >= 12.0, switches the RED LED on
 *                             (critical alert).
 *   "ARC,COLOR,<pH>,<seq>" -> colour-confirmed alert (picric acid strip turned
 *                             red-rose): switches the GREEN LED on.
 *
 * An LED stays on while alerts keep arriving and switches off once no alert of
 * that kind has been received for ALERT_HOLD_MS.
 *
 * Libraries (Arduino Library Manager):
 *   - LoRa  by Sandeep Mistry
 */

#include <SPI.h>
#include <LoRa.h>

// ---------------------------------------------------------------------------
// Pin map (see README.md for the full wiring table)
// ---------------------------------------------------------------------------
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_NSS    5
#define LORA_RST   14
#define LORA_DIO0  26

#define RED_LED_PIN    27
#define GREEN_LED_PIN  33

// ---------------------------------------------------------------------------
// Settings (LoRa values must match the transmitter)
// ---------------------------------------------------------------------------
const float PH_CRITICAL = 12.0;

const long LORA_FREQUENCY   = 433E6;
const int  LORA_SF          = 9;
const long LORA_BANDWIDTH   = 125E3;
const int  LORA_SYNC_WORD   = 0x3A;

#define PACKET_HEADER "ARC"

// How long an LED stays on after the last matching alert.
const unsigned long ALERT_HOLD_MS = 15000;

unsigned long redOnSince = 0;    // time of the last critical alert
unsigned long greenOnSince = 0;  // time of the last colour-confirmed alert
bool redOn = false;
bool greenOn = false;

// ---------------------------------------------------------------------------
// Packet handling
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

  Serial.print("LoRa RX: ");
  Serial.print(packet);
  Serial.print("  RSSI ");
  Serial.println(LoRa.packetRssi());

  if (!parsePacket(packet, type, ph, seq)) {
    Serial.println("  ignored: not an ARCCANUM packet");
    return;
  }

  if (type == "CRIT") {
    // Double-check the pH carried in the packet before raising the alarm
    if (ph >= PH_CRITICAL) {
      redOn = true;
      redOnSince = millis();
      Serial.printf("  CRITICAL alert: pH %.2f -> RED LED ON\n", ph);
    } else {
      Serial.printf("  CRIT packet with pH %.2f < %.1f, ignored\n", ph, PH_CRITICAL);
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

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  while (!LoRa.begin(LORA_FREQUENCY)) {
    // Blink both LEDs so a wiring fault is visible without a serial monitor
    Serial.println("RA-02 LoRa init failed, retrying...");
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, HIGH);
    delay(250);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    delay(750);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();

  Serial.println("Listening for alerts");
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    String packet;
    while (LoRa.available()) packet += (char)LoRa.read();
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
