#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <Preferences.h>

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASS";

#define MAX_NODES 10
#define HISTORY_SIZE 24

WebServer server(80);
Preferences prefs;

bool autoMode = true;
bool holidayMode = false;

unsigned long lastWaterCycle = 0;
const unsigned long WATER_INTERVAL = 6UL * 60UL * 60UL * 1000UL;

float DRY_LEVEL = 35.0;
float OK_LEVEL  = 55.0;

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
  uint8_t cmd;       // 1=pumpA, 2=pumpB, 3=all off
  uint16_t seconds;
} CmdPacket;

struct NodeData {
  uint8_t mac[6];
  uint8_t id;
  char name[16];

  float moistureA;
  float moistureB;

  float batteryVoltage;
  float batteryCurrent_mA;
  float batteryPower_mW;

  float solarVoltage;
  float solarCurrent_mA;
  float solarPower_mW;

  uint8_t flags;

  float voltageFiltered = 0;
  float batteryPercent = 0;
  float daysLeft = -1;
  float dropPerDay = 0;
  unsigned long batteryStartTime = 0;
  float batteryStartVoltage = 0;

  bool batteryLowAlert = false;
  bool batteryCriticalAlert = false;

  bool online = false;
  unsigned long lastSeen = 0;

  float histA[HISTORY_SIZE];
  float histB[HISTORY_SIZE];
  int histIndex = 0;
};

NodeData nodes[MAX_NODES];
int nodeCount = 0;

String macToString(const uint8_t *mac) {
  char s[18];
  sprintf(s, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(s);
}

int findNode(const uint8_t *mac) {
  for (int i = 0; i < nodeCount; i++) {
    if (memcmp(nodes[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

void addPeer(const uint8_t *mac) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(mac)) {
    esp_now_add_peer(&peer);
  }
}

void savePrefs() {
  prefs.begin("farm", false);
  prefs.putInt("count", nodeCount);
  prefs.putBytes("nodes", nodes, sizeof(nodes));
  prefs.putBool("autoMode", autoMode);
  prefs.putBool("holiday", holidayMode);
  prefs.end();
}

void loadPrefs() {
  prefs.begin("farm", true);
  nodeCount = prefs.getInt("count", 0);
  prefs.getBytes("nodes", nodes, sizeof(nodes));
  autoMode = prefs.getBool("autoMode", true);
  holidayMode = prefs.getBool("holiday", false);
  prefs.end();

  for (int i = 0; i < nodeCount; i++) {
    addPeer(nodes[i].mac);
  }
}

float calcBatteryPercent(float v) {
  if (v >= BAT_FULL) return 100;
  if (v <= BAT_EMPTY) return 0;
  return ((v - BAT_EMPTY) / (BAT_FULL - BAT_EMPTY)) * 100.0;
}

void updateSmartBattery(int idx, float newVoltage) {
  if (newVoltage <= 0.1) return;

  if (nodes[idx].voltageFiltered == 0) {
    nodes[idx].voltageFiltered = newVoltage;
    nodes[idx].batteryStartVoltage = newVoltage;
    nodes[idx].batteryStartTime = millis();
  }

  nodes[idx].voltageFiltered =
    nodes[idx].voltageFiltered * 0.7 + newVoltage * 0.3;

  nodes[idx].batteryPercent =
    calcBatteryPercent(nodes[idx].voltageFiltered);

  nodes[idx].batteryLowAlert =
    nodes[idx].voltageFiltered <= BAT_LOW;

  nodes[idx].batteryCriticalAlert =
    nodes[idx].voltageFiltered <= BAT_CRITICAL;

  if (nodes[idx].batteryCriticalAlert) {
    nodes[idx].daysLeft = 0;
    return;
  }

  float hours =
    (millis() - nodes[idx].batteryStartTime) / 3600000.0;

  if (hours < 6) {
    nodes[idx].daysLeft = -1;
    return;
  }

  float drop =
    nodes[idx].batteryStartVoltage - nodes[idx].voltageFiltered;

  if (drop <= 0.03) {
    nodes[idx].daysLeft = 99;
    nodes[idx].dropPerDay = 0;
    return;
  }

  nodes[idx].dropPerDay = drop / (hours / 24.0);
  float remaining = nodes[idx].voltageFiltered - BAT_EMPTY;
  nodes[idx].daysLeft = remaining / nodes[idx].dropPerDay;

  if (nodes[idx].daysLeft < 0) nodes[idx].daysLeft = 0;
}

bool batteryAllowsWatering(int idx) {
  return nodes[idx].voltageFiltered > BAT_LOW;
}

int calcWaterTime(float moisture, float batt) {
  if (batt < BAT_LOW) return 0;
  if (batt < BAT_OK) return 60;

  if (holidayMode) return 60;

  if (moisture < DRY_LEVEL) return 300;
  if (moisture < OK_LEVEL) return 120;

  return 0;
}

void sendPump(int idx, int pump, int sec) {
  if (idx < 0 || idx >= nodeCount) return;

  if (!batteryAllowsWatering(idx)) {
    Serial.printf("BLOCKED: Node %d battery too low %.2fV\n",
                  nodes[idx].id,
                  nodes[idx].voltageFiltered);
    return;
  }

  CmdPacket c;
  c.cmd = pump;
  c.seconds = sec;

  esp_err_t result = esp_now_send(
    nodes[idx].mac,
    (uint8_t*)&c,
    sizeof(c)
  );

  Serial.printf("SEND -> Node %d Pump %d Time %d sec Result %d\n",
                nodes[idx].id, pump, sec, result);
}

void scheduler() {
  if (!autoMode) return;
  if (millis() - lastWaterCycle < WATER_INTERVAL) return;

  Serial.println("SMART WATER CYCLE");

  for (int i = 0; i < nodeCount; i++) {
    int tA = calcWaterTime(nodes[i].moistureA, nodes[i].voltageFiltered);
    int tB = calcWaterTime(nodes[i].moistureB, nodes[i].voltageFiltered);

    if (tA > 0) sendPump(i, 1, tA);
    if (tB > 0) sendPump(i, 2, tB);
  }

  lastWaterCycle = millis();
}

void onRecv(const esp_now_recv_info_t *info,
            const uint8_t *data,
            int len) {
  if (len != sizeof(SensorPacket)) {
    Serial.printf("Invalid packet size: %d expected %d\n", len, sizeof(SensorPacket));
    return;
  }

  SensorPacket p;
  memcpy(&p, data, sizeof(p));

  int idx = findNode(info->src_addr);

  if (idx < 0 && nodeCount < MAX_NODES) {
    idx = nodeCount++;

    memcpy(nodes[idx].mac, info->src_addr, 6);
    nodes[idx].id = p.nodeID;
    sprintf(nodes[idx].name, "NODE_%d", p.nodeID);

    addPeer(info->src_addr);
    savePrefs();

    Serial.print("New node detected: ");
    Serial.println(macToString(info->src_addr));
  }

  if (idx >= 0) {
    nodes[idx].moistureA = p.moistureA;
    nodes[idx].moistureB = p.moistureB;

    nodes[idx].batteryVoltage = p.batteryVoltage;
    nodes[idx].batteryCurrent_mA = p.batteryCurrent_mA;
    nodes[idx].batteryPower_mW = p.batteryPower_mW;

    nodes[idx].solarVoltage = p.solarVoltage;
    nodes[idx].solarCurrent_mA = p.solarCurrent_mA;
    nodes[idx].solarPower_mW = p.solarPower_mW;

    nodes[idx].flags = p.flags;

    updateSmartBattery(idx, p.batteryVoltage);

    nodes[idx].online = true;
    nodes[idx].lastSeen = millis();

    nodes[idx].histA[nodes[idx].histIndex] = p.moistureA;
    nodes[idx].histB[nodes[idx].histIndex] = p.moistureB;
    nodes[idx].histIndex++;
    if (nodes[idx].histIndex >= HISTORY_SIZE) nodes[idx].histIndex = 0;

    Serial.printf("RX Node %d | A %.1f%% | B %.1f%% | BAT %.2fV %.1fmA %.1fmW | SOL %.2fV %.1fmA %.1fmW | Flags %u\n",
                  nodes[idx].id,
                  nodes[idx].moistureA,
                  nodes[idx].moistureB,
                  nodes[idx].batteryVoltage,
                  nodes[idx].batteryCurrent_mA,
                  nodes[idx].batteryPower_mW,
                  nodes[idx].solarVoltage,
                  nodes[idx].solarCurrent_mA,
                  nodes[idx].solarPower_mW,
                  nodes[idx].flags);
  }
}

String flagText(uint8_t f) {
  String s = "";
  if (f & FLAG_BAT_LOW) s += "BAT_LOW ";
  if (f & FLAG_BAT_CRITICAL) s += "BAT_CRITICAL ";
  if (f & FLAG_PUMP_BLOCKED) s += "PUMP_BLOCKED ";
  if (f & FLAG_SOLAR_CHARGE) s += "SOLAR_CHARGE ";
  if (f & FLAG_INA_BAT_OK) s += "INA_BAT_OK ";
  if (f & FLAG_INA_SOLAR_OK) s += "INA_SOLAR_OK ";
  if (s == "") s = "OK";
  return s;
}

String page() {
  String h = "<html><head>";
  h += "<meta name='viewport' content='width=device-width'>";
  h += "<style>";
  h += "body{font-family:Arial;background:#111;color:#fff}";
  h += ".box{border:1px solid #444;padding:10px;margin:10px;border-radius:10px}";
  h += "button{padding:10px;margin:4px;font-size:16px}";
  h += "input{font-size:16px;width:120px}";
  h += ".bad{color:red;font-weight:bold}.warn{color:orange;font-weight:bold}.ok{color:#4cff4c}";
  h += "</style></head><body>";

  h += "<h2>SMART FARM CENTRAL</h2>";

  h += "<button onclick=\"fetch('/auto').then(()=>location.reload())\">AUTO/MANUAL</button>";
  h += "<button onclick=\"fetch('/holiday').then(()=>location.reload())\">HOLIDAY</button>";

  h += "<p>Mode: ";
  h += autoMode ? "AUTO " : "MANUAL ";
  h += holidayMode ? "(HOLIDAY)" : "";
  h += "</p>";

  for (int i = 0; i < nodeCount; i++) {
    bool alive = millis() - nodes[i].lastSeen < 7200000;

    h += "<div class='box'>";
    h += "<b>" + String(nodes[i].name) + "</b><br>";
    h += "ID: " + String(nodes[i].id) + "<br>";
    h += "MAC: " + macToString(nodes[i].mac) + "<br>";
    h += "Status: ";
    h += alive ? "<span class='ok'>ONLINE</span><br>" : "<span class='bad'>OFFLINE</span><br>";

    h += "<hr>";
    h += "Moisture A: " + String(nodes[i].moistureA, 1) + "%<br>";
    h += "Moisture B: " + String(nodes[i].moistureB, 1) + "%<br>";

    h += "<hr><b>Battery INA219</b><br>";
    h += "Battery voltage: " + String(nodes[i].batteryVoltage, 2) + "V<br>";
    h += "Battery current: " + String(nodes[i].batteryCurrent_mA, 1) + "mA<br>";
    h += "Battery power: " + String(nodes[i].batteryPower_mW, 1) + "mW<br>";
    h += "Battery filtered: " + String(nodes[i].voltageFiltered, 2) + "V<br>";
    h += "Battery: " + String(nodes[i].batteryPercent, 0) + "%<br>";

    h += "Battery forecast: ";
    if (nodes[i].daysLeft < 0) {
      h += "learning...<br>";
    } else if (nodes[i].daysLeft >= 90) {
      h += "charging / stable<br>";
    } else {
      h += String(nodes[i].daysLeft, 1) + " days<br>";
    }

    h += "<hr><b>Solar INA219</b><br>";
    h += "Solar voltage: " + String(nodes[i].solarVoltage, 2) + "V<br>";
    h += "Solar current: " + String(nodes[i].solarCurrent_mA, 1) + "mA<br>";
    h += "Solar power: " + String(nodes[i].solarPower_mW, 1) + "mW<br>";

    if (nodes[i].flags & FLAG_SOLAR_CHARGE) {
      h += "<span class='ok'>Solar charging detected</span><br>";
    }

    h += "<hr>";
    h += "Flags: " + flagText(nodes[i].flags) + "<br>";

    if (nodes[i].voltageFiltered <= BAT_CRITICAL || (nodes[i].flags & FLAG_BAT_CRITICAL)) {
      h += "<span class='bad'>CRITICAL BATTERY - WATERING BLOCKED</span><br>";
    } else if (nodes[i].voltageFiltered <= BAT_LOW || (nodes[i].flags & FLAG_BAT_LOW)) {
      h += "<span class='warn'>LOW BATTERY - WATERING BLOCKED</span><br>";
    }

    h += "<hr>";
    h += "<input id='name" + String(i) + "' value='" + String(nodes[i].name) + "'>";
    h += "<button onclick=\"setName(" + String(i) + ")\">SET NAME</button><br>";

    h += "<button onclick=\"fetch('/p1?id=" + String(i) + "')\">Pump A 120s</button>";
    h += "<button onclick=\"fetch('/p2?id=" + String(i) + "')\">Pump B 120s</button>";
    h += "<button onclick=\"fetch('/off?id=" + String(i) + "')\">ALL OFF</button>";

    h += "</div>";
  }

  h += R"rawliteral(
<script>
function setName(i){
  let v=document.getElementById('name'+i).value;
  fetch('/name?id='+i+'&v='+encodeURIComponent(v)).then(()=>location.reload());
}
</script>
</body></html>)rawliteral";

  return h;
}

void setupWeb() {
  server.on("/", []() {
    server.send(200, "text/html", page());
  });

  server.on("/auto", []() {
    autoMode = !autoMode;
    savePrefs();
    server.send(200, "text/plain", "OK");
  });

  server.on("/holiday", []() {
    holidayMode = !holidayMode;
    savePrefs();
    server.send(200, "text/plain", "OK");
  });

  server.on("/name", []() {
    int id = server.arg("id").toInt();
    String v = server.arg("v");

    if (id >= 0 && id < nodeCount) {
      v.toCharArray(nodes[id].name, sizeof(nodes[id].name));
      savePrefs();
    }

    server.send(200, "text/plain", "OK");
  });

  server.on("/p1", []() {
    int id = server.arg("id").toInt();
    if (id < nodeCount) sendPump(id, 1, 120);
    server.send(200, "text/plain", "OK");
  });

  server.on("/p2", []() {
    int id = server.arg("id").toInt();
    if (id < nodeCount) sendPump(id, 2, 120);
    server.send(200, "text/plain", "OK");
  });

  server.on("/off", []() {
    int id = server.arg("id").toInt();
    if (id < nodeCount) {
      CmdPacket c;
      c.cmd = 3;
      c.seconds = 0;
      esp_now_send(nodes[id].mac, (uint8_t*)&c, sizeof(c));
    }
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  loadPrefs();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("WiFi channel: ");
  Serial.println(WiFi.channel());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onRecv);

  setupWeb();

  Serial.println("CENTRAL READY");
}

void loop() {
  server.handleClient();
  scheduler();
  delay(10);
}
