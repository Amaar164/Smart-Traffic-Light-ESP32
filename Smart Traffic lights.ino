#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// =====================================================
//                 SMART TRAFFIC AI
//             ESP32 + ST7735 + 2 ULTRASONIC
// =====================================================

// ---------------- TFT: ST7735 160x128 ----------------
#define TFT_CS    5
#define TFT_DC    16
#define TFT_RST   17
#define TFT_MOSI  23
#define TFT_SCK   18
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

// ---------------- ULTRASONIC SENSORS -----------------
#define TRIG_A 32
#define ECHO_A 34
#define TRIG_B 33
#define ECHO_B 35

// ---------------- BUZZER -----------------------------
#define BUZZER 22

// ---------------- WIFI -------------------------------

const char* WIFI_SSID = "WE_Amaar";
const char* WIFI_PASSWORD = "Amaar164";

const char* AP_SSID = "SMART-TRAFFIC-AI";
const char* AP_PASSWORD = "12345678";

// ---------------- TELEGRAM ---------------------------

const char* BOT_TOKEN = "9186512693:AAHyD8iteBmDpi87BPq1V7Lu0R52OEabvpI";
const char* CHAT_ID = "8092342515";
// =====================================================
//                    OBJECTS
// =====================================================
WebServer server(80);
Preferences preferences;

bool apMode = false;
bool serverStarted = false;
volatile bool telegramBusy = false;

// =====================================================
//                    COLORS
// =====================================================
#define BG       0x0841
#define PANEL    0x18E3
#define WHITE    0xFFFF
#define GREEN    0x07E0
#define RED      0xF800
#define YELLOW   0xFFE0
#define CYAN     0x07FF
#define GREY     0x8410
#define ORANGE   0xFD20
#define BLACK    0x0000

// =====================================================
//                 TRAFFIC PHASES
// =====================================================
#define PHASE_A_GREEN       0
#define PHASE_A_YELLOW      1
#define PHASE_ALL_RED_B     2
#define PHASE_B_GREEN       3
#define PHASE_B_YELLOW      4
#define PHASE_ALL_RED_A     5

uint8_t currentPhase = PHASE_A_GREEN;
const unsigned long GREEN_TIME = 12000;
const unsigned long YELLOW_TIME = 3000;
const unsigned long ALL_RED_TIME = 1000;
unsigned long phaseStart = 0;
unsigned long phaseDuration = GREEN_TIME;

// =====================================================
//                 SENSOR SETTINGS
// =====================================================
// Both sensors detect a close object at the stop-line demo zone.
// Sensor A is active only while Road A is RED (Road B GREEN).
// Sensor B is active only while Road B is RED (Road A GREEN).
const float DETECT_MIN_CM = 3.0;
const float DETECT_MAX_CM = 7.0;
const int DETECT_SAMPLES = 1;
const int CLEAR_SAMPLES = 3;
const unsigned long SENSOR_INTERVAL = 100;
const unsigned long PULSE_TIMEOUT = 2500;

// =====================================================
//                 BUZZER SETTINGS
// =====================================================
const unsigned long VIOLATION_ALARM_DURATION = 2000;

// =====================================================
//                    VARIABLES
// =====================================================
float distanceA = -1;
float distanceB = -1;

bool sensorAHit = false;
bool sensorBHit = false;
int sensorAClearSamples = 0;
int sensorBClearSamples = 0;

unsigned long violationAlarmUntil = 0;
unsigned long violationCountA = 0;
unsigned long violationCountB = 0;

bool buzzerOutput = false;
unsigned long lastSensorRead = 0;
unsigned long lastScreenUpdate = 0;
unsigned long lastSerialUpdate = 0;
bool screenInitialized = false;

// ---------------- PHONE SETTINGS --------------------
int speedLimitKmh = 50;
String warningA = "NONE";
String warningB = "NONE";

// =====================================================
//                TRAFFIC HELPERS
// =====================================================
bool isRoadAGreen() {
  return currentPhase == PHASE_A_GREEN;
}

bool isRoadAYellow() {
  return currentPhase == PHASE_A_YELLOW;
}

bool isRoadARed() {
  return !isRoadAGreen() && !isRoadAYellow();
}

bool isRoadBGreen() {
  return currentPhase == PHASE_B_GREEN;
}

// Monitor only during the opposing road's full green phase.
bool sensorAEnabled() { return currentPhase == PHASE_B_GREEN; }
bool sensorBEnabled() { return currentPhase == PHASE_A_GREEN; }

const char* phaseName() {
  switch (currentPhase) {
    case PHASE_A_GREEN:   return "A GREEN";
    case PHASE_A_YELLOW:  return "A YELLOW";
    case PHASE_ALL_RED_B: return "ALL RED";
    case PHASE_B_GREEN:   return "B GREEN";
    case PHASE_B_YELLOW:  return "B YELLOW";
    case PHASE_ALL_RED_A: return "ALL RED";
  }
  return "UNKNOWN";
}

const char* roadSignalName(bool roadA) {
  if (roadA) {
    if (currentPhase == PHASE_A_GREEN) return "GREEN";
    if (currentPhase == PHASE_A_YELLOW) return "YELLOW";
    return "RED";
  }
  if (currentPhase == PHASE_B_GREEN) return "GREEN";
  if (currentPhase == PHASE_B_YELLOW) return "YELLOW";
  return "RED";
}

uint16_t roadSignalColor(bool roadA) {
  if (roadA) {
    if (currentPhase == PHASE_A_GREEN) return GREEN;
    if (currentPhase == PHASE_A_YELLOW) return YELLOW;
    return RED;
  }
  if (currentPhase == PHASE_B_GREEN) return GREEN;
  if (currentPhase == PHASE_B_YELLOW) return YELLOW;
  return RED;
}

bool dangerActive() {
  // DANGER depends only on the two selected road warnings.
  return warningA != "NONE" || warningB != "NONE";
}

// =====================================================
//             ULTRASONIC READING
// =====================================================
float readDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, PULSE_TIMEOUT);
  if (duration == 0) return -1;
  float cm = duration * 0.0343f / 2.0f;
  if (cm < 2.0f || cm > 50.0f) return -1;
  return cm;
}

// =====================================================
//                 RESET FUNCTIONS
// =====================================================
void resetSensorMonitors() {
  sensorAHit = false;
  sensorBHit = false;
  sensorAClearSamples = 0;
  sensorBClearSamples = 0;
  distanceA = -1;
  distanceB = -1;
}

// =====================================================
//                TELEGRAM URL ENCODING
// =====================================================
String urlEncode(const String &value) {
  String encoded = "";
  const char hex[] = "0123456789ABCDEF";

  for (size_t i = 0; i < value.length(); i++) {
    uint8_t c = (uint8_t)value[i];
    bool safe =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~';

    if (safe) encoded += (char)c;
    else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

// =====================================================
//             TELEGRAM BACKGROUND TASK
// =====================================================
void telegramTask(void* parameter) {
  String* message = (String*)parameter;

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    String url =
      "https://api.telegram.org/bot" + String(BOT_TOKEN) +
      "/sendMessage?chat_id=" + urlEncode(String(CHAT_ID)) +
      "&text=" + urlEncode(*message);

    https.setConnectTimeout(4000);
    https.setTimeout(4000);

    if (https.begin(client, url)) {
      int response = https.GET();
      Serial.print("Telegram HTTP response: ");
      Serial.println(response);
      https.end();
    } else {
      Serial.println("Telegram initialization failed.");
    }
    client.stop();
  } else {
    Serial.println("Telegram: WiFi disconnected.");
  }

  delete message;
  telegramBusy = false;
  vTaskDelete(NULL);
}

void sendTelegramAlert(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Telegram unavailable: WiFi disconnected.");
    return;
  }
  if (telegramBusy) {
    Serial.println("Telegram message already being sent.");
    return;
  }

  telegramBusy = true;
  String* payload = new String(message);
  BaseType_t result = xTaskCreatePinnedToCore(
    telegramTask, "TelegramTask", 8192, payload, 1, NULL, 0
  );

  if (result != pdPASS) {
    delete payload;
    telegramBusy = false;
    Serial.println("Could not create Telegram task.");
  }
}

// =====================================================
//           RED-LIGHT VIOLATION SENSOR MONITOR
// =====================================================
void reportRedLightEvent(bool roadA) {
  unsigned long now = millis();
  violationAlarmUntil = now + VIOLATION_ALARM_DURATION;
  if (roadA) violationCountA++;
  else violationCountB++;

  Serial.println("RED LIGHT OBJECT DETECTED");
  Serial.println(roadA ? "ROAD A" : "ROAD B");

  String message = "🚨 تنبيه مروري رسمي\n\n";
  message += "تم رصد تجاوز إحدى المركبات للإشارة الحمراء.\n\n";
  message += roadA ? "الموقع: الطريق A\n\n" : "الموقع: الطريق B\n\n";
  message += "يرجى اتخاذ الإجراءات اللازمة.";
  sendTelegramAlert(message);
}

void updateOneSensor(int trigPin, int echoPin, float &distance,
                     bool &latched, int &clearSamples, bool roadA) {
  float d = readDistance(trigPin, echoPin);
  distance = d;
  bool insideZone = (d >= DETECT_MIN_CM && d <= DETECT_MAX_CM);

  // Send immediately on the first valid reading in the detection zone.
  if (!latched && insideZone) {
    latched = true;
    clearSamples = 0;
    reportRedLightEvent(roadA);
    return;
  }

  // Re-arm only after three consecutive readings outside the zone.
  if (latched) {
    if (insideZone) {
      clearSamples = 0;
    } else {
      clearSamples++;
      if (clearSamples >= CLEAR_SAMPLES) {
        latched = false;
        clearSamples = 0;
      }
    }
  }
}

void updateSensorA() {
  if (!sensorAEnabled()) {
    sensorAHit = false;
    sensorAClearSamples = 0;
    distanceA = -1;
    return;
  }
  updateOneSensor(TRIG_A, ECHO_A, distanceA,
                  sensorAHit, sensorAClearSamples, true);
}

void updateSensorB() {
  if (!sensorBEnabled()) {
    sensorBHit = false;
    sensorBClearSamples = 0;
    distanceB = -1;
    return;
  }
  updateOneSensor(TRIG_B, ECHO_B, distanceB,
                  sensorBHit, sensorBClearSamples, false);
}

// =====================================================
//                    BUZZER
// =====================================================
void setBuzzer(bool state) {
  if (state != buzzerOutput) {
    buzzerOutput = state;
    digitalWrite(BUZZER, state ? HIGH : LOW);
  }
}

void updateBuzzer() {
  // Short alarm only when a red-light detection event occurs.
  setBuzzer(millis() < violationAlarmUntil);
}

// =====================================================
//                  TRAFFIC CONTROL
// =====================================================
void enterPhase(uint8_t newPhase) {
  currentPhase = newPhase;
  phaseStart = millis();

  switch (currentPhase) {
    case PHASE_A_GREEN:   phaseDuration = GREEN_TIME; break;
    case PHASE_A_YELLOW:  phaseDuration = YELLOW_TIME; break;
    case PHASE_ALL_RED_B: phaseDuration = ALL_RED_TIME; break;
    case PHASE_B_GREEN:   phaseDuration = GREEN_TIME; break;
    case PHASE_B_YELLOW:  phaseDuration = YELLOW_TIME; break;
    case PHASE_ALL_RED_A: phaseDuration = ALL_RED_TIME; break;
  }

  resetSensorMonitors();
  setBuzzer(false);

  Serial.print("NEW PHASE: ");
  Serial.println(phaseName());
}

void updateTraffic() {
  if (millis() - phaseStart < phaseDuration) return;

  switch (currentPhase) {
    case PHASE_A_GREEN:   enterPhase(PHASE_A_YELLOW); break;
    case PHASE_A_YELLOW:  enterPhase(PHASE_ALL_RED_B); break;
    case PHASE_ALL_RED_B: enterPhase(PHASE_B_GREEN); break;
    case PHASE_B_GREEN:   enterPhase(PHASE_B_YELLOW); break;
    case PHASE_B_YELLOW:  enterPhase(PHASE_ALL_RED_A); break;
    case PHASE_ALL_RED_A: enterPhase(PHASE_A_GREEN); break;
  }
}

// =====================================================
//                    COUNTDOWN
// =====================================================
int getRemainingSeconds() {
  unsigned long elapsed = millis() - phaseStart;
  if (elapsed >= phaseDuration) return 0;
  return (phaseDuration - elapsed + 999) / 1000;
}

// =====================================================
//                 WARNING SETTINGS
// =====================================================
bool validWarning(const String &code) {
  return code == "NONE" ||
         code == "ROAD_DAMAGED" ||
         code == "ACCIDENT" ||
         code == "ROAD_CLOSED" ||
         code == "OBSTACLE" ||
         code == "ROAD_WORK" ||
         code == "FLOODING";
}

const char* warningLabel(const String &code) {
  if (code == "ROAD_DAMAGED") return "ROAD DAMAGE";
  if (code == "ACCIDENT") return "ACCIDENT";
  if (code == "ROAD_CLOSED") return "ROAD CLOSED";
  if (code == "OBSTACLE") return "OBSTACLE";
  if (code == "ROAD_WORK") return "ROAD WORK";
  if (code == "FLOODING") return "FLOODING";
  return "NONE";
}

const char* warningShortLabel(const String &code) {
  if (code == "ROAD_DAMAGED") return "DAMAGE";
  if (code == "ACCIDENT") return "ACCIDENT";
  if (code == "ROAD_CLOSED") return "CLOSED";
  if (code == "OBSTACLE") return "OBSTACLE";
  if (code == "ROAD_WORK") return "WORK";
  if (code == "FLOODING") return "FLOOD";
  return "NONE";
}

// =====================================================
//                 LOAD SETTINGS
// =====================================================
void loadSettings() {
  preferences.begin("traffic", false);

  speedLimitKmh = preferences.getUInt("speed", 50);
  warningA = preferences.getString("warnA", "NONE");
  warningB = preferences.getString("warnB", "NONE");

  if (speedLimitKmh < 5 || speedLimitKmh > 120) speedLimitKmh = 50;
  if (!validWarning(warningA)) warningA = "NONE";
  if (!validWarning(warningB)) warningB = "NONE";

  // Old manualWarn preference is intentionally ignored.
}

// =====================================================
//                  WEB DASHBOARD
// =====================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta charset="UTF-8">
<title>Smart Traffic AI</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:16px;background:#101827;color:#eef4fc;font-family:Arial,sans-serif}
.container{max-width:560px;margin:auto}
header{padding:18px 5px;border-bottom:1px solid #35445a;margin-bottom:18px}
h1{font-size:23px;margin:0;color:#36d9e9}
.subtitle{color:#94a7bd;font-size:12px;margin-top:7px}
.card{background:#1b293c;border:1px solid #34445a;border-radius:12px;padding:17px;margin-bottom:14px}
h2{font-size:15px;color:#36d9e9;margin:0 0 15px}
.row{display:flex;justify-content:space-between;align-items:center;padding:11px 0;border-bottom:1px solid #34445a;font-size:13px;gap:10px}
.row:last-child{border:0}
.value{font-weight:bold;text-align:right;color:#36d9e9}
.safe{color:#45e7a3}.danger{color:#ff6464}
input,select{width:100%;padding:12px;margin-top:8px;border:1px solid #445873;border-radius:7px;background:#101827;color:white;font-size:14px}
label{display:block;font-size:12px;color:#aebed0;margin-top:12px}
button{width:100%;border:0;padding:13px;margin-top:12px;border-radius:7px;background:#087f9c;color:white;font-weight:bold;font-size:14px;cursor:pointer}
button:active{opacity:.75}
.orange{background:#a76a17}.purple{background:#6449ad}
.warning-status{font-size:12px;font-weight:bold;color:#ff6464;margin-top:9px;min-height:15px}
#message{min-height:22px;color:#45e7a3;font-size:13px;margin:12px 0}
.footer{color:#72869f;text-align:center;font-size:11px;padding:12px}
</style>
</head>
<body>
<div class="container">
<header>
<h1>SMART TRAFFIC AI</h1>
<div class="subtitle">TRAFFIC CONTROL &amp; SAFETY MONITORING</div>
</header>

<div class="card">
<h2>LIVE TRAFFIC STATUS</h2>
<div class="row"><span>Current Phase</span><span class="value" id="phase">---</span></div>
<div class="row"><span>Road A Signal</span><span class="value" id="roadA">---</span></div>
<div class="row"><span>Road B Signal</span><span class="value" id="roadB">---</span></div>
<div class="row"><span>Time Remaining</span><span class="value" id="timer">---</span></div>
<div class="row"><span>Road Safety</span><span class="value safe" id="safety">SAFE</span></div>
<div class="row"><span>Speed Limit Setting</span><span class="value" id="speedDisplay">50 km/h</span></div>
</div>

<div class="card">
<h2>SENSOR MONITORING</h2>
<div class="row"><span>Sensor A - Cars</span><span class="value" id="sensorA">---</span></div>
<div class="row"><span>Distance A</span><span class="value" id="distanceA">---</span></div>
<div class="row"><span>Sensor B - Cars</span><span class="value" id="sensorB">---</span></div>
<div class="row"><span>Distance B</span><span class="value" id="distanceB">---</span></div>
<div class="row"><span>Buzzer</span><span class="value" id="buzzer">---</span></div>
</div>

<div class="card">
<h2>ROAD A WARNING</h2>
<label for="warnA">Select warning type</label>
<select id="warnA">
<option value="NONE">No warning</option>
<option value="ROAD_DAMAGED">Road damaged</option>
<option value="ACCIDENT">Accident</option>
<option value="ROAD_CLOSED">Road closed</option>
<option value="OBSTACLE">Obstacle</option>
<option value="ROAD_WORK">Road work</option>
<option value="FLOODING">Flooding</option>
</select>
<div class="warning-status" id="warnAStatus"></div>
<button class="orange" onclick="saveWarning('A')">SAVE ROAD A WARNING</button>
</div>

<div class="card">
<h2>ROAD B WARNING</h2>
<label for="warnB">Select warning type</label>
<select id="warnB">
<option value="NONE">No warning</option>
<option value="ROAD_DAMAGED">Road damaged</option>
<option value="ACCIDENT">Accident</option>
<option value="ROAD_CLOSED">Road closed</option>
<option value="OBSTACLE">Obstacle</option>
<option value="ROAD_WORK">Road work</option>
<option value="FLOODING">Flooding</option>
</select>
<div class="warning-status" id="warnBStatus"></div>
<button class="orange" onclick="saveWarning('B')">SAVE ROAD B WARNING</button>
</div>

<div class="card">
<h2>SPEED LIMIT SETTING</h2>
<label for="speed">Speed limit setting (5-120 km/h)</label>
<input type="number" id="speed" min="5" max="120" value="50">
<button onclick="saveSpeed()">UPDATE SPEED SETTING</button>
</div>

<div class="card">
<h2>TELEGRAM</h2>
<button class="purple" onclick="testTelegram()">SEND TEST MESSAGE</button>
</div>

<div id="message"></div>
<div class="footer">SMART TRAFFIC AI SYSTEM</div>
</div>

<script>
let dirtyA = false;
let dirtyB = false;
const warningNames = {
  NONE: 'No warning',
  ROAD_DAMAGED: 'Road damaged',
  ACCIDENT: 'Accident',
  ROAD_CLOSED: 'Road closed',
  OBSTACLE: 'Obstacle',
  ROAD_WORK: 'Road work',
  FLOODING: 'Flooding'
};

document.getElementById('warnA').addEventListener('change', () => { dirtyA = true; });
document.getElementById('warnB').addEventListener('change', () => { dirtyB = true; });

async function request(url){
  const r = await fetch(url, {cache:'no-store'});
  const t = await r.text();
  if(!r.ok) throw new Error(t || 'Request failed');
  return t;
}

function updateWarningStatus(road, code){
  const el = document.getElementById('warn' + road + 'Status');
  if(code && code !== 'NONE'){
    el.textContent = '! ' + (warningNames[code] || code);
  }else{
    el.textContent = '';
  }
}

async function saveWarning(road){
  try{
    const type = document.getElementById('warn' + road).value;
    const result = await request('/setWarning?road=' + road + '&type=' + encodeURIComponent(type));
    if(road === 'A') dirtyA = false;
    if(road === 'B') dirtyB = false;
    updateWarningStatus(road, type);
    document.getElementById('message').textContent = result;
    await refreshStatus();
  }catch(e){
    document.getElementById('message').textContent = e.message;
  }
}

async function saveSpeed(){
  try{
    const speed = document.getElementById('speed').value;
    const result = await request('/setSpeed?value=' + encodeURIComponent(speed));
    document.getElementById('message').textContent = result;
    await refreshStatus();
  }catch(e){
    document.getElementById('message').textContent = e.message;
  }
}

async function testTelegram(){
  try{
    const result = await request('/testTelegram');
    document.getElementById('message').textContent = result;
  }catch(e){
    document.getElementById('message').textContent = e.message;
  }
}

async function refreshStatus(){
  try{
    const r = await fetch('/api/status', {cache:'no-store'});
    if(!r.ok) throw new Error('Offline');
    const d = await r.json();

    for(const id of ['phase','roadA','roadB','timer','sensorA','sensorB','distanceA','distanceB','buzzer']){
      document.getElementById(id).textContent = d[id];
    }

    document.getElementById('speedDisplay').textContent = d.speed + ' km/h';
    const safety = document.getElementById('safety');
    safety.textContent = d.danger ? 'DANGER' : 'SAFE';
    safety.className = d.danger ? 'value danger' : 'value safe';

    if(!dirtyA) document.getElementById('warnA').value = d.warningA;
    if(!dirtyB) document.getElementById('warnB').value = d.warningB;
    updateWarningStatus('A', d.warningA);
    updateWarningStatus('B', d.warningB);

    if(document.activeElement.id !== 'speed'){
      document.getElementById('speed').value = d.speed;
    }
  }catch(e){
    document.getElementById('phase').textContent = 'OFFLINE';
  }
}

setInterval(refreshStatus, 1000);
refreshStatus();
</script>
</body>
</html>
)rawliteral";

// =====================================================
//                 WEB SERVER HANDLERS
// =====================================================
void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  String json = "{";
  json += "\"phase\":\"" + String(phaseName()) + "\",";
  json += "\"roadA\":\"" + String(roadSignalName(true)) + "\",";
  json += "\"roadB\":\"" + String(roadSignalName(false)) + "\",";
  json += "\"timer\":\"" + String(getRemainingSeconds()) + " sec\",";
  json += "\"speed\":" + String(speedLimitKmh) + ",";

  json += "\"sensorA\":\"";
  if (!sensorAEnabled()) json += "INACTIVE";
  else if (sensorAHit) json += "CAR DETECTED";
  else json += "ACTIVE / CLEAR";
  json += "\",";

  json += "\"distanceA\":\"";
  if (distanceA < 0) json += "NO TARGET";
  else json += String(distanceA, 1) + " cm";
  json += "\",";

  json += "\"sensorB\":\"";
  if (!sensorBEnabled()) json += "INACTIVE";
  else if (sensorBHit) json += "CAR DETECTED";
  else json += "ACTIVE / CLEAR";
  json += "\",";

  json += "\"distanceB\":\"";
  if (distanceB < 0) json += "NO TARGET";
  else json += String(distanceB, 1) + " cm";
  json += "\",";

  json += "\"buzzer\":\"" + String(buzzerOutput ? "ACTIVE" : "OFF") + "\",";
  json += "\"warningA\":\"" + warningA + "\",";
  json += "\"warningB\":\"" + warningB + "\",";
  json += "\"danger\":" + String(dangerActive() ? "true" : "false");
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// =====================================================
//                 SET ROAD WARNING
// =====================================================
void handleSetWarning() {
  if (!server.hasArg("road") || !server.hasArg("type")) {
    server.send(400, "text/plain", "Missing parameters");
    return;
  }

  String road = server.arg("road");
  String type = server.arg("type");
  type.trim();
  road.trim();

  if (!validWarning(type)) {
    server.send(400, "text/plain", "Invalid warning");
    return;
  }

  if (road == "A") {
    warningA = type;
    preferences.putString("warnA", warningA);
  } else if (road == "B") {
    warningB = type;
    preferences.putString("warnB", warningB);
  } else {
    server.send(400, "text/plain", "Invalid road");
    return;
  }

  Serial.print("ROAD ");
  Serial.print(road);
  Serial.print(" WARNING: ");
  Serial.println(warningLabel(type));

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "Warning updated successfully");
}

// =====================================================
//                    SET SPEED
// =====================================================
void handleSetSpeed() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "Missing speed");
    return;
  }

  String raw = server.arg("value");
  raw.trim();
  if (raw.length() == 0) {
    server.send(400, "text/plain", "Invalid speed");
    return;
  }
  for (size_t i = 0; i < raw.length(); i++) {
    if (!isDigit(raw[i])) {
      server.send(400, "text/plain", "Speed must be a number");
      return;
    }
  }

  int value = raw.toInt();
  if (value < 5 || value > 120) {
    server.send(400, "text/plain", "Speed must be 5-120");
    return;
  }

  speedLimitKmh = value;
  preferences.putUInt("speed", speedLimitKmh);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "Speed setting updated successfully");
}

// =====================================================
//                  TEST TELEGRAM
// =====================================================
void handleTestTelegram() {
  sendTelegramAlert(
    "SMART TRAFFIC AI TEST\n"
    "Telegram connection test from ESP32."
  );
  server.send(200, "text/plain", "Telegram test requested. Check Serial Monitor.");
}

// =====================================================
//                 WEB SERVER SETUP
// =====================================================
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/setWarning", HTTP_GET, handleSetWarning);
  server.on("/setSpeed", HTTP_GET, handleSetSpeed);
  server.on("/testTelegram", HTTP_GET, handleTestTelegram);
  // Manual danger toggle has been removed intentionally.
  server.begin();
  serverStarted = true;
  Serial.println("Web Server Started");
}

// =====================================================
//                    WIFI SETUP
// =====================================================
void setupWiFi() {
  Serial.println();
  Serial.println("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.println("WiFi Connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Starting Access Point...");
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    bool result = WiFi.softAP(AP_SSID, AP_PASSWORD);
    apMode = true;

    if (result) {
      Serial.println("Access Point Started");
      Serial.print("Network: ");
      Serial.println(AP_SSID);
      Serial.print("Open: ");
      Serial.println(WiFi.softAPIP());
    } else {
      Serial.println("Access Point failed to start.");
    }
  }
  setupWebServer();
}

// =====================================================
//                 TFT DRAWING HELPERS
// =====================================================
void drawCarIcon(int x, int y, uint16_t color) {
  tft.fillRoundRect(x, y + 7, 25, 10, 3, color);
  tft.fillTriangle(x + 5, y + 7, x + 9, y, x + 18, y, color);
  tft.fillTriangle(x + 18, y, x + 23, y + 7, x + 5, y + 7, color);
  tft.fillRect(x + 9, y + 2, 7, 4, BG);
  tft.fillCircle(x + 6, y + 17, 3, BLACK);
  tft.fillCircle(x + 20, y + 17, 3, BLACK);
  tft.drawCircle(x + 6, y + 17, 2, GREY);
  tft.drawCircle(x + 20, y + 17, 2, GREY);
}

void drawPersonIcon(int x, int y, uint16_t color) {
  tft.fillCircle(x, y, 3, color);
  tft.drawLine(x, y + 3, x, y + 13, color);
  tft.drawLine(x, y + 6, x - 6, y + 10, color);
  tft.drawLine(x, y + 6, x + 6, y + 10, color);
  tft.drawLine(x, y + 13, x - 5, y + 20, color);
  tft.drawLine(x, y + 13, x + 5, y + 20, color);
}

void drawTrafficLight(int x, int y, uint16_t activeColor) {
  // Larger, clearer traffic-light housing and three distinct lamps.
  const uint16_t LAMP_OFF = 0x4208;
  tft.fillRoundRect(x, y, 14, 34, 3, BLACK);
  tft.drawRoundRect(x, y, 14, 34, 3, WHITE);

  uint16_t redLamp    = (activeColor == RED)    ? RED    : LAMP_OFF;
  uint16_t yellowLamp = (activeColor == YELLOW) ? YELLOW : LAMP_OFF;
  uint16_t greenLamp  = (activeColor == GREEN)  ? GREEN  : LAMP_OFF;

  tft.fillCircle(x + 7, y + 7, 4, redLamp);
  tft.fillCircle(x + 7, y + 17, 4, yellowLamp);
  tft.fillCircle(x + 7, y + 27, 4, greenLamp);

  // Small white highlights make the active lamps easier to distinguish.
  if (activeColor == RED)    tft.fillCircle(x + 6, y + 6, 1, WHITE);
  if (activeColor == YELLOW) tft.fillCircle(x + 6, y + 16, 1, WHITE);
  if (activeColor == GREEN)  tft.fillCircle(x + 6, y + 26, 1, WHITE);
}

// =====================================================
//                 TFT STATIC SCREEN
// =====================================================
void drawStaticScreen() {
  tft.fillScreen(BG);

  // Minimal cyan accent stripe; no SMART TRAFFIC AI title.
  tft.fillRect(0, 0, 160, 3, CYAN);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  // ROAD A panel
  tft.drawRoundRect(3, 7, 75, 49, 4, GREY);
  tft.setTextColor(WHITE);
  tft.setCursor(8, 11);
  tft.print("ROAD A");

  // ROAD B panel
  tft.drawRoundRect(82, 7, 75, 49, 4, GREY);
  tft.setCursor(87, 11);
  tft.print("ROAD B");

  // Sensor panels
  tft.drawRoundRect(3, 60, 75, 27, 4, GREY);
  tft.setTextColor(CYAN);
  tft.setCursor(8, 64);
  tft.print("CAR SENSOR");

  tft.drawRoundRect(82, 60, 75, 27, 4, GREY);
  tft.setCursor(87, 64);
  tft.print("CAR SENSOR");

  // Bottom information panel
  tft.drawRoundRect(3, 91, 154, 34, 4, GREY);
  tft.setTextColor(WHITE);
  tft.setCursor(8, 97);
  tft.print("SPD:");
  tft.setCursor(68, 97);
  tft.print("TIME:");

  screenInitialized = true;
}

// =====================================================
//                 TFT UPDATE DASHBOARD
// =====================================================
void updateDashboard() {
  if (!screenInitialized) return;

  // ---------------- ROAD A ----------------
  uint16_t colorA = roadSignalColor(true);
  tft.fillRect(8, 23, 43, 10, BG);
  tft.setTextSize(1);
  tft.setTextColor(colorA);
  tft.setCursor(8, 24);
  tft.print(roadSignalName(true));
  drawTrafficLight(61, 19, colorA);

  // Car color follows the complete Road A signal: green, yellow, or red.
  tft.fillRect(9, 35, 40, 19, BG);
  drawCarIcon(15, 35, colorA);

  // ---------------- ROAD B ----------------
  uint16_t colorB = roadSignalColor(false);
  tft.fillRect(87, 23, 43, 10, BG);
  tft.setTextColor(colorB);
  tft.setCursor(87, 24);
  tft.print(roadSignalName(false));
  drawTrafficLight(139, 19, colorB);

  // Keep the Road B car clear of its traffic light; use the same signal colors.
  tft.fillRect(90, 35, 36, 19, BG);
  drawCarIcon(96, 35, colorB);

  // ---------------- SENSOR A ----------------
  tft.fillRect(7, 77, 67, 8, BG);
  if (!sensorAEnabled()) {
    tft.setTextColor(GREY);
    tft.setCursor(8, 77);
    tft.print("INACTIVE");
  } else if (sensorAHit) {
    tft.setTextColor(YELLOW);
    tft.setCursor(8, 77);
    tft.print("CAR WAITING");
  } else {
    tft.setTextColor(GREEN);
    tft.setCursor(8, 77);
    tft.print("CLEAR");
  }

  // ---------------- SENSOR B ----------------
  tft.fillRect(86, 77, 67, 8, BG);
  if (!sensorBEnabled()) {
    tft.setTextColor(GREY);
    tft.setCursor(87, 77);
    tft.print("INACTIVE");
  } else if (sensorBHit) {
    tft.setTextColor(RED);
    tft.setCursor(87, 77);
    tft.print("DETECTED");
  } else {
    tft.setTextColor(GREEN);
    tft.setCursor(87, 77);
    tft.print("CLEAR");
  }

  // ---------------- SPEED ----------------
  tft.fillRect(32, 95, 31, 11, BG);
  tft.setTextColor(WHITE);
  tft.setCursor(32, 97);
  tft.print(speedLimitKmh);

  // ---------------- COUNTDOWN ----------------
  tft.fillRect(100, 95, 26, 11, BG);
  tft.setTextColor(CYAN);
  tft.setCursor(100, 97);
  tft.print(getRemainingSeconds());

  // ---------------- ROAD WARNING STATUS ----------------
  tft.fillRect(8, 110, 144, 11, BG);
  tft.setTextSize(1);
  if (dangerActive()) {
    tft.setTextColor(RED);
    tft.setCursor(8, 111);
    tft.print("!");
    tft.print(" A:");
    tft.print(warningShortLabel(warningA));
    tft.print(" B:");
    tft.print(warningShortLabel(warningB));
  } else {
    tft.setTextColor(GREEN);
    tft.setCursor(8, 111);
    tft.print("SAFE - NO WARNINGS");
  }
}

// =====================================================
//                 STARTUP SCREEN
// =====================================================
void startupScreen() {
  tft.fillScreen(BG);
  tft.fillRect(0, 0, 160, 3, CYAN);
  tft.setTextColor(CYAN);
  tft.setTextSize(1);
  tft.setCursor(42, 43);
  tft.print("SYSTEM STARTING");
  tft.setTextColor(WHITE);
  tft.setCursor(48, 60);
  tft.print("PLEASE WAIT");
  tft.drawRoundRect(15, 85, 130, 12, 4, CYAN);
  for (int i = 0; i <= 120; i += 4) {
    tft.fillRect(20, 89, i, 4, GREEN);
    delay(15);
  }
  delay(250);
}

// =====================================================
//                    SETUP
// =====================================================
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_A, OUTPUT);
  pinMode(ECHO_A, INPUT);
  pinMode(TRIG_B, OUTPUT);
  pinMode(ECHO_B, INPUT);
  digitalWrite(TRIG_A, LOW);
  digitalWrite(TRIG_B, LOW);

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  loadSettings();

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // Landscape: 160 x 128
  tft.setTextWrap(false);

  startupScreen();
  setupWiFi();

  enterPhase(PHASE_A_GREEN);
  drawStaticScreen();
  updateDashboard();

  Serial.println();
  Serial.println("================================");
  Serial.println("SMART TRAFFIC AI READY");
  Serial.println("================================");
  Serial.println("Sensor A: Cars / ROAD A RED only");
  Serial.println("Sensor B: Cars / ROAD B RED only");
  Serial.println("Manual danger toggle: REMOVED");
  Serial.println("TFT: ST7735 160x128");
  Serial.println("Dashboard: warning selectors + speed setting");
}

// =====================================================
//                    MAIN LOOP
// =====================================================
void loop() {
  unsigned long now = millis();

  if (serverStarted) server.handleClient();
  updateTraffic();

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    updateSensorA();
    updateSensorB();
  }

  updateBuzzer();

  if (now - lastScreenUpdate >= 250) {
    lastScreenUpdate = now;
    updateDashboard();
  }

  if (now - lastSerialUpdate >= 1000) {
    lastSerialUpdate = now;
    Serial.print("PHASE: "); Serial.print(phaseName());
    Serial.print(" | A distance: "); Serial.print(distanceA);
    Serial.print(" | B distance: "); Serial.print(distanceB);
    Serial.print(" | Road A events: "); Serial.print(violationCountA);
    Serial.print(" | Road B events: "); Serial.print(violationCountB);
    Serial.print(" | BUZZER: "); Serial.println(buzzerOutput ? "ON" : "OFF");
  }
}
