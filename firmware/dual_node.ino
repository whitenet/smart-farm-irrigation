#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <esp_sleep.h>

// ================= USER CONFIG =================
#define NODE_ID 1

uint8_t centralMac[] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};

#define SLEEP_SECONDS 3600

// ================= PINOUT =================
#define SENSOR_A_PIN 34
#define SENSOR_B_PIN 35

#define PUMP_A_PIN   25
#define PUMP_B_PIN   26

#define LED_PIN      2

// ================= INA219 ADDRESSES =================
// Battery / load INA219 = 0x40
// Solar INA219          = 0x41
Adafruit_INA219 inaBattery(0x40);
Adafruit_INA219 inaSolar(0x41);

bool inaBatteryReady = false;
bool inaSolarReady = false;

// ================= BATTERY 18650 3S =================
const float BAT_FULL     = 12.6;
const float BAT_OK       = 11.5;
const float BAT_LOW      = 10.8;
const float BAT_CRITICAL = 10.5;
const float BAT_EMPTY    = 10.2;

#define FLAG_BAT_LOW       0x01
#define FLAG_BAT_CRITICAL  0x02
#define FLAG_PUMP_BLOCKED  0x04
#define FLAG_SOLAR_CHARGE  0x08
#define FLAG_INA_BAT_OK    0x10
#define FLAG_INA_SOLAR_OK  0x20

// ================= PACKETS =================
typedef struct {
  uint8_t nodeID;

  float moistureA;
  float moistureB;

  float batteryVoltage;
  float batteryCurrent_mA;
  float batteryPower_mW;

  float solarVoltage;
  float solarCurrent_mA;
  float solarPower_mW;

  uint8_t flags;
} SensorPacket;

typedef struct {
  uint8_t cmd;       // 1 PumpA / 2 PumpB / 3 OFF
  uint16_t seconds;
} CmdPacket;

// ================= GLOBALS =================
bool commandReceived = false;
uint8_t lastFlags = 0;

// ================= SENSOR UTILS =================
float readMoisturePercent(int pin) {
  int raw = analogRead(pin);

  // Kalibracja:
  // dry ~3200, wet ~1500
  float pct = map(raw, 3200, 1500, 0, 100);

  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  return pct;
}

float safeBusVoltage(Adafruit_INA219 &ina, bool ready) {
  if (!ready) return 0.0;
  return ina.getBusVoltage_V();
}

float safeCurrent_mA(Adafruit_INA219 &ina, bool ready) {
  if (!ready) return 0.0;
  return ina.getCurrent_mA();
}

float safePower_mW(Adafruit_INA219 &ina, bool ready) {
  if (!ready) return 0.0;
  return ina.getPower_mW();
}

float readBatteryVoltage() {
  if (inaBatteryReady) {
    return inaBattery.getBusVoltage_V();
  }

  return 0.0;
}

bool batteryLow(float v) {
  return v > 0.1 && v <= BAT_LOW;
}

bool batteryCritical(float v) {
  return v > 0.1 && v <= BAT_CRITICAL;
}

float batteryPercent(float v) {
  if (v >= BAT_FULL) return 100;
  if (v <= BAT_EMPTY) return 0;
  return ((v - BAT_EMPTY) / (BAT_FULL - BAT_EMPTY)) * 100.0;
}

uint8_t buildFlags(float batteryVoltage,
                   float solarVoltage,
                   float solarCurrent_mA) {
  uint8_t f = 0;

  if (inaBatteryReady) f |= FLAG_INA_BAT_OK;
  if (inaSolarReady) f |= FLAG_INA_SOLAR_OK;

  if (batteryLow(batteryVoltage)) f |= FLAG_BAT_LOW;
  if (batteryCritical(batteryVoltage)) f |= FLAG_BAT_CRITICAL;

  if (solarVoltage > batteryVoltage + 0.2 && solarCurrent_mA > 5.0) {
    f |= FLAG_SOLAR_CHARGE;
  }

  return f;
}

// ================= PUMP =================
void pumpOffAll() {
  digitalWrite(PUMP_A_PIN, LOW);
  digitalWrite(PUMP_B_PIN, LOW);
}

void runPump(uint8_t pin, int sec) {
  float batt = readBatteryVoltage();

  if (batteryCritical(batt)) {
    Serial.println("CRITICAL BATTERY - pump blocked");
    lastFlags |= FLAG_BAT_CRITICAL | FLAG_PUMP_BLOCKED;
    pumpOffAll();
    return;
  }

  if (batteryLow(batt)) {
    Serial.println("LOW BATTERY - pump blocked");
    lastFlags |= FLAG_BAT_LOW | FLAG_PUMP_BLOCKED;
    pumpOffAll();
    return;
  }

  digitalWrite(pin, HIGH);
  digitalWrite(LED_PIN, HIGH);

  Serial.printf("Pump ON pin %d for %d sec\n", pin, sec);

  unsigned long start = millis();

  while (millis() - start < (unsigned long)sec * 1000UL) {
    delay(100);
  }

  digitalWrite(pin, LOW);
  digitalWrite(LED_PIN, LOW);

  Serial.println("Pump OFF");
}

// ================= ESP-NOW CALLBACK =================
void onDataRecv(const esp_now_recv_info_t *info,
                const uint8_t *incomingData,
                int len) {
  if (len != sizeof(CmdPacket)) {
    Serial.println("Invalid command size");
    return;
  }

  CmdPacket cmd;
  memcpy(&cmd, incomingData, sizeof(cmd));

  commandReceived = true;

  Serial.printf("Command received: cmd=%d sec=%d\n",
                cmd.cmd,
                cmd.seconds);

  if (cmd.cmd == 1) {
    runPump(PUMP_A_PIN, cmd.seconds);
  }
  else if (cmd.cmd == 2) {
    runPump(PUMP_B_PIN, cmd.seconds);
  }
  else if (cmd.cmd == 3) {
    pumpOffAll();
    Serial.println("All pumps OFF");
  }
}

// ================= SEND STATUS =================
void sendStatus() {
  SensorPacket data;

  data.nodeID = NODE_ID;
  data.moistureA = readMoisturePercent(SENSOR_A_PIN);
  data.moistureB = readMoisturePercent(SENSOR_B_PIN);

  data.batteryVoltage    = safeBusVoltage(inaBattery, inaBatteryReady);
  data.batteryCurrent_mA = safeCurrent_mA(inaBattery, inaBatteryReady);
  data.batteryPower_mW   = safePower_mW(inaBattery, inaBatteryReady);

  data.solarVoltage    = safeBusVoltage(inaSolar, inaSolarReady);
  data.solarCurrent_mA = safeCurrent_mA(inaSolar, inaSolarReady);
  data.solarPower_mW   = safePower_mW(inaSolar, inaSolarReady);

  data.flags = buildFlags(
                 data.batteryVoltage,
                 data.solarVoltage,
                 data.solarCurrent_mA
               ) | lastFlags;

  esp_err_t result = esp_now_send(
    centralMac,
    (uint8_t *)&data,
    sizeof(data)
  );

  if (result == ESP_OK) {
    Serial.println("Telemetry sent");
  } else {
    Serial.printf("Send failed: %d\n", result);
  }

  Serial.printf("A: %.1f %%\n", data.moistureA);
  Serial.printf("B: %.1f %%\n", data.moistureB);

  Serial.println("BATTERY INA219:");
  Serial.printf("Voltage: %.2f V\n", data.batteryVoltage);
  Serial.printf("Current: %.1f mA\n", data.batteryCurrent_mA);
  Serial.printf("Power: %.1f mW\n", data.batteryPower_mW);
  Serial.printf("Percent: %.0f %%\n", batteryPercent(data.batteryVoltage));

  Serial.println("SOLAR INA219:");
  Serial.printf("Voltage: %.2f V\n", data.solarVoltage);
  Serial.printf("Current: %.1f mA\n", data.solarCurrent_mA);
  Serial.printf("Power: %.1f mW\n", data.solarPower_mW);

  Serial.printf("Flags: %u\n", data.flags);
}

// ================= SLEEP =================
void goSleep() {
  Serial.println("Going to deep sleep");

  esp_sleep_enable_timer_wakeup(
    (uint64_t)SLEEP_SECONDS * 1000000ULL
  );

  delay(200);
  esp_deep_sleep_start();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PUMP_A_PIN, OUTPUT);
  pinMode(PUMP_B_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  pumpOffAll();

  analogReadResolution(12);

  Wire.begin(21, 22);

  if (inaBattery.begin()) {
    inaBatteryReady = true;
    Serial.println("INA219 BATTERY OK @0x40");
  } else {
    inaBatteryReady = false;
    Serial.println("INA219 BATTERY not found @0x40");
  }

  if (inaSolar.begin()) {
    inaSolarReady = true;
    Serial.println("INA219 SOLAR OK @0x41");
  } else {
    inaSolarReady = false;
    Serial.println("INA219 SOLAR not found @0x41");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init fail");
    goSleep();
  }

  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, centralMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(centralMac)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Peer add failed");
      goSleep();
    }
  }

  Serial.println("NODE READY");

  sendStatus();

  unsigned long waitStart = millis();

  while (millis() - waitStart < 20000) {
    delay(50);
  }

  sendStatus();

  goSleep();
}

void loop() {
}
