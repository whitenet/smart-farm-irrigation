// ============================================================
// CENTRAL.ino  (FINAL PRO - CENTRAL CONTROLLER)
// ESP32 CENTRAL + WiFi Dashboard + ESP-NOW + Smart Watering
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <Update.h>

// ---------------- WIFI ----------------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASS";

// ---------------- SYSTEM ----------------
#define MAX_NODES 10
#define HISTORY_SIZE 24

WebServer server(80);
Preferences prefs;

// ---------------- SMART SETTINGS ----------------
bool autoMode = true;
bool holidayMode = false;

unsigned long lastWaterCycle = 0;
const unsigned long WATER_INTERVAL = 6UL * 60UL * 60UL * 1000UL;

float DRY_LEVEL = 35.0;
float OK_LEVEL  = 55.0;
float LOW_BATT  = 11.5;

// ---------------- NODE STRUCT ----------------
struct NodeData {
  uint8_t mac[6];
  uint8_t id;
  char name[16];

  float moistureA;
  float moistureB;
  float voltage;

  bool online;

  unsigned long lastSeen;

  float histA[HISTORY_SIZE];
  float histB[HISTORY_SIZE];
  int histIndex;
};

NodeData nodes[MAX_NODES];
int nodeCount = 0;

// ---------------- PACKETS ----------------
typedef struct {
  uint8_t nodeID;
  float moistureA;
  float moistureB;
  float voltage;
} SensorPacket;

typedef struct {
  uint8_t cmd;       // 1=pumpA 2=pumpB 3=alloff
  uint16_t seconds;
} CmdPacket;

// ============================================================
// UTIL
// ============================================================

String macToString(const uint8_t *mac){
  char s[18];
  sprintf(s,"%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(s);
}

int findNode(const uint8_t *mac){
  for(int i=0;i<nodeCount;i++){
    if(memcmp(nodes[i].mac,mac,6)==0) return i;
  }
  return -1;
}

void addPeer(const uint8_t *mac){
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 1;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void savePrefs(){
  prefs.begin("farm", false);
  prefs.putInt("count", nodeCount);
  prefs.putBytes("nodes", nodes, sizeof(nodes));
  prefs.end();
}

void loadPrefs(){
  prefs.begin("farm", true);
  nodeCount = prefs.getInt("count", 0);
  prefs.getBytes("nodes", nodes, sizeof(nodes));
  prefs.end();
}

// ============================================================
// WATER LOGIC
// ============================================================

int calcWaterTime(float moisture, float batt){

  if(batt < LOW_BATT) return 0;

  if(holidayMode) return 60;

  if(moisture < DRY_LEVEL) return 300;
  if(moisture < OK_LEVEL)  return 120;

  return 0;
}

void sendPump(int idx, int pump, int sec){

  CmdPacket c;

  if(pump == 1) c.cmd = 1;
  if(pump == 2) c.cmd = 2;

  c.seconds = sec;

  esp_now_send(nodes[idx].mac, (uint8_t*)&c, sizeof(c));

  Serial.printf("SEND -> Node %d Pump %d Time %d sec\n",
      nodes[idx].id, pump, sec);
}

void scheduler(){

  if(!autoMode) return;

  if(millis() - lastWaterCycle < WATER_INTERVAL) return;

  Serial.println("SMART WATER CYCLE");

  for(int i=0;i<nodeCount;i++){

    int tA = calcWaterTime(nodes[i].moistureA, nodes[i].voltage);
    int tB = calcWaterTime(nodes[i].moistureB, nodes[i].voltage);

    if(tA > 0) sendPump(i,1,tA);
    if(tB > 0) sendPump(i,2,tB);
  }

  lastWaterCycle = millis();
}

// ============================================================
// ESPNOW RECEIVE
// ============================================================

void onRecv(const esp_now_recv_info_t *info,
            const uint8_t *data,
            int len){

  SensorPacket p;
  memcpy(&p, data, sizeof(p));

  int idx = findNode(info->src_addr);

  if(idx < 0 && nodeCount < MAX_NODES){

    idx = nodeCount++;

    memcpy(nodes[idx].mac, info->src_addr, 6);
    nodes[idx].id = p.nodeID;

    sprintf(nodes[idx].name, "NODE_%d", p.nodeID);

    addPeer(info->src_addr);
  }

  if(idx >= 0){

    nodes[idx].moistureA = p.moistureA;
    nodes[idx].moistureB = p.moistureB;
    nodes[idx].voltage   = p.voltage;

    nodes[idx].online = true;
    nodes[idx].lastSeen = millis();

    nodes[idx].histA[nodes[idx].histIndex] = p.moistureA;
    nodes[idx].histB[nodes[idx].histIndex] = p.moistureB;

    nodes[idx].histIndex++;
    if(nodes[idx].histIndex >= HISTORY_SIZE)
      nodes[idx].histIndex = 0;
  }
}

// ============================================================
// WEB UI
// ============================================================

String page(){

  String h = "<html><head>";
  h += "<meta name='viewport' content='width=device-width'>";
  h += "<style>";
  h += "body{font-family:Arial;background:#111;color:#fff}";
  h += ".box{border:1px solid #444;padding:10px;margin:10px;border-radius:10px}";
  h += "button{padding:10px;margin:4px;font-size:16px}";
  h += "</style></head><body>";

  h += "<h2>SMART FARM CENTRAL</h2>";

  h += "<button onclick=\"fetch('/auto')\">AUTO/MANUAL</button>";
  h += "<button onclick=\"fetch('/holiday')\">HOLIDAY</button>";

  h += "<br>Mode: ";
  h += autoMode ? "AUTO " : "MANUAL ";
  h += holidayMode ? "(HOLIDAY)" : "";

  for(int i=0;i<nodeCount;i++){

    bool alive = millis()-nodes[i].lastSeen < 7200000;

    h += "<div class='box'>";
    h += "<b>"+String(nodes[i].name)+"</b><br>";
    h += "ID: "+String(nodes[i].id)+"<br>";
    h += "A: "+String(nodes[i].moistureA,1)+"%<br>";
    h += "B: "+String(nodes[i].moistureB,1)+"%<br>";
    h += "Batt: "+String(nodes[i].voltage,2)+"V<br>";
    h += "Status: ";
    h += alive ? "ONLINE":"OFFLINE";
    h += "<br>";

    h += "<button onclick=\"fetch('/p1?id="+String(i)+"')\">Pump A</button>";
    h += "<button onclick=\"fetch('/p2?id="+String(i)+"')\">Pump B</button>";

    h += "</div>";
  }

  h += "</body></html>";
  return h;
}

// ============================================================
// WEB ROUTES
// ============================================================

void setupWeb(){

  server.on("/", [](){
    server.send(200,"text/html",page());
  });

  server.on("/auto", [](){
    autoMode = !autoMode;
    server.send(200,"text/plain","OK");
  });

  server.on("/holiday", [](){
    holidayMode = !holidayMode;
    server.send(200,"text/plain","OK");
  });

  server.on("/p1", [](){
    int id = server.arg("id").toInt();
    if(id < nodeCount) sendPump(id,1,120);
    server.send(200,"text/plain","OK");
  });

  server.on("/p2", [](){
    int id = server.arg("id").toInt();
    if(id < nodeCount) sendPump(id,2,120);
    server.send(200,"text/plain","OK");
  });

  server.begin();
}

// ============================================================
// SETUP
// ============================================================

void setup(){

  Serial.begin(115200);
  delay(1000);

  loadPrefs();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi");

  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println(WiFi.localIP());

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if(esp_now_init() != ESP_OK){
    Serial.println("ESP NOW FAIL");
    return;
  }

  esp_now_register_recv_cb(onRecv);

  setupWeb();

  Serial.println("CENTRAL READY");
}

// ============================================================
// LOOP
// ============================================================

void loop(){

  server.handleClient();

  scheduler();

  delay(10);
}
