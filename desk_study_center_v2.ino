#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

const char* ssid = "wifi-ssid";
const char* password = "wifi-password";

#define TRIG_PIN D5
#define ECHO_PIN D6
#define LED_PIN D4

const float PRESENCE_THRESHOLD = 80.0;
const int FILTER_SAMPLES = 5;
const unsigned long PRESENCE_DEBOUNCE_MS = 2000;
const unsigned long AWAY_DEBOUNCE_MS = 3000;

const unsigned long DEFAULT_STUDY_MS = 10UL * 60UL * 1000UL;
const unsigned long DEFAULT_BREAK_MS = 2UL * 60UL * 1000UL;

unsigned long studyDurationSetting = DEFAULT_STUDY_MS;
unsigned long breakDurationSetting = DEFAULT_BREAK_MS;

enum State { IDLE, STUDYING, BREAK_TIME, AWAY };
State currentState = IDLE;
State stateBeforeAway = IDLE; 

unsigned long sessionStartTime = 0;
unsigned long totalStudyTime = 0;
unsigned long currentSessionElapsed = 0;
int sessionCount = 0;
int breakCount = 0;

unsigned long lastPresenceChangeTime = 0;
bool wasPresent = false;
bool isPresent = false;

float distanceReadings[FILTER_SAMPLES];
int readIndex = 0;

bool oledReady = false;

ESP8266WebServer server(80);

//  To-do list
struct TodoItem {
  String text;
  bool completed;
};
const int MAX_TODOS = 10;
TodoItem todos[MAX_TODOS];
int todoCount = 0;

//  Session history
struct SessionRecord {
  unsigned long duration;
  unsigned long timestamp;
  bool isCompleted;
};
const int MAX_SESSIONS = 20;
SessionRecord sessions[MAX_SESSIONS];
int sessionRecordCount = 0;


unsigned long pausedSessionTime = 0; 


float readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;

  float distance = duration * 0.034 / 2.0;
  if (distance <= 0 || distance > 400) return -1;

  return distance;
}

float getFilteredDistance() {
  float d = readDistance();
  if (d > 0) {
    distanceReadings[readIndex] = d;
    readIndex = (readIndex + 1) % FILTER_SAMPLES;
  }
  float sum = 0;
  int count = 0;
  for (int i = 0; i < FILTER_SAMPLES; i++) {
    if (distanceReadings[i] > 0) { sum += distanceReadings[i]; count++; }
  }
  if (count == 0) return -1;
  return sum / count;
}

bool checkPresence() {
  float distance = getFilteredDistance();
  return (distance > 0 && distance < PRESENCE_THRESHOLD);
}

void updatePresenceState() {
  bool currentReading = checkPresence();
  unsigned long now = millis();

  if (currentReading != wasPresent) {
    unsigned long debounce = currentReading ? PRESENCE_DEBOUNCE_MS : AWAY_DEBOUNCE_MS;
    if (now - lastPresenceChangeTime > debounce) {
      wasPresent = currentReading;
      isPresent = currentReading;
      lastPresenceChangeTime = now;
    }
  } else {
    lastPresenceChangeTime = now;
  }
}

void saveSession(unsigned long duration, bool completed) {
  if (sessionRecordCount < MAX_SESSIONS) {
    sessions[sessionRecordCount++] = { duration, millis(), completed };
  } else {
    for (int i = 1; i < MAX_SESSIONS; i++) sessions[i - 1] = sessions[i];
    sessions[MAX_SESSIONS - 1] = { duration, millis(), completed };
  }
}

// ─── State machine ────────────────────────────────────────────────────────────
void updateStateMachine() {
  unsigned long now = millis();

  switch (currentState) {

    case IDLE:
      if (isPresent) {
        currentState = STUDYING;
        stateBeforeAway = STUDYING;
        sessionStartTime = now;
        pausedSessionTime = 0;
        sessionCount++;
        digitalWrite(LED_PIN, LOW);
      }
      break;

    case STUDYING: {
      // Total elapsed = time since last resume + anything accumulated before
      unsigned long elapsed = (now - sessionStartTime) + pausedSessionTime;
      currentSessionElapsed = elapsed;

      if (!isPresent) {
        // Save how far we got and go to AWAY
        pausedSessionTime = elapsed;
        stateBeforeAway = STUDYING;
        currentState = AWAY;
        sessionStartTime = 0;
        digitalWrite(LED_PIN, HIGH);
      } else if (elapsed >= studyDurationSetting) {
        // Study phase complete
        totalStudyTime += studyDurationSetting;
        saveSession(studyDurationSetting, true);
        currentState = BREAK_TIME;
        stateBeforeAway = BREAK_TIME;
        sessionStartTime = now;
        pausedSessionTime = 0;
        breakCount++;
        digitalWrite(LED_PIN, HIGH);
      } else {
        digitalWrite(LED_PIN, LOW);
      }
      break;
    }

    case BREAK_TIME: {
      unsigned long elapsed = (now - sessionStartTime) + pausedSessionTime;

      if (!isPresent) {
        // Pause the break too
        pausedSessionTime = elapsed;
        stateBeforeAway = BREAK_TIME;
        currentState = AWAY;
        sessionStartTime = 0;
        digitalWrite(LED_PIN, HIGH);
      } else if (elapsed >= breakDurationSetting) {
        // Break complete — start new study session
        currentState = STUDYING;
        stateBeforeAway = STUDYING;
        sessionStartTime = now;
        pausedSessionTime = 0;
        sessionCount++;
        digitalWrite(LED_PIN, LOW);
      } else {
        // Blink LED during break
        digitalWrite(LED_PIN, (now % 1000 < 500) ? LOW : HIGH);
      }
      break;
    }

    case AWAY:
      if (isPresent) {
        // Resume whichever phase was paused — pausedSessionTime already holds
        // the accumulated elapsed, so just restart the wall-clock reference.
        currentState = stateBeforeAway;
        sessionStartTime = now;
        // pausedSessionTime is intentionally NOT reset here — it carries over
        if (currentState == STUDYING) {
          digitalWrite(LED_PIN, LOW);
        } else {
          digitalWrite(LED_PIN, HIGH);
        }
      } else {
        digitalWrite(LED_PIN, HIGH);
      }
      break;
  }
}

// ─── OLED ─────────────────────────────────────────────────────────────────────
void updateDisplay() {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  unsigned long now = millis();
  unsigned long elapsed = 0;
  unsigned long remaining = 0;

  // Compute elapsed the same way the state machine does
  if (sessionStartTime > 0)
    elapsed = (now - sessionStartTime) + pausedSessionTime;
  else
    elapsed = pausedSessionTime;

  const char* face = "(=^.^=)";
  const char* stateText = "Idle";

  switch (currentState) {
    case IDLE:
      face = "(=^.^=)";
      stateText = "Idle";
      break;
    case STUDYING:
      face = "(=^.^=)";
      stateText = "Studying";
      remaining = (studyDurationSetting > elapsed) ? studyDurationSetting - elapsed : 0;
      break;
    case BREAK_TIME:
      face = "(=^o^=)";
      stateText = "Break";
      remaining = (breakDurationSetting > elapsed) ? breakDurationSetting - elapsed : 0;
      break;
    case AWAY:
      face = "(=._.=)";
      stateText = "Away";
      // Show remaining time for the paused phase
      if (stateBeforeAway == STUDYING)
        remaining = (studyDurationSetting > pausedSessionTime) ? studyDurationSetting - pausedSessionTime : 0;
      else if (stateBeforeAway == BREAK_TIME)
        remaining = (breakDurationSetting > pausedSessionTime) ? breakDurationSetting - pausedSessionTime : 0;
      break;
  }

  display.setTextSize(2);
  display.setCursor(10, 0);
  display.print(face);

  display.setTextSize(1);
  display.setCursor(0, 25);
  display.print(stateText);

  if (remaining > 0 || currentState == AWAY) {
    unsigned long secs = remaining / 1000;
    unsigned long mins = secs / 60;
    secs %= 60;
    display.setTextSize(2);
    display.setCursor(20, 45);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", mins, secs);
    display.print(buf);
  }

  display.display();
}

// ─── Web pages ────────────────────────────────────────────────────────────────
// Shared CSS + nav used by both pages
String sharedHead(const char* activeTab) {
  String s = R"(<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>
  <title>Study Command Center</title>
  <style>
    * { margin:0; padding:0; box-sizing:border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
      background: linear-gradient(135deg,#667eea 0%,#764ba2 100%);
      min-height: 100vh; padding: 20px; color: #333;
    }
    .container { max-width: 800px; margin: 0 auto; }
    h1 { color:white; text-align:center; margin-bottom:20px; font-size:2.2em;
         text-shadow:2px 2px 4px rgba(0,0,0,.2); }
    /* ── Tabs ── */
    .tabs { display:flex; gap:8px; margin-bottom:20px; }
    .tab {
      padding:10px 28px; border-radius:30px; cursor:pointer; font-weight:600;
      font-size:0.95em; border:2px solid rgba(255,255,255,.5);
      color:white; background:rgba(255,255,255,.15); text-decoration:none;
      transition: background .2s;
    }
    .tab.active, .tab:hover { background:white; color:#667eea; border-color:white; }
    /* ── Cards ── */
    .card { background:white; border-radius:15px; padding:25px;
            margin-bottom:20px; box-shadow:0 10px 30px rgba(0,0,0,.2); }
    .status-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(140px,1fr));
                   gap:15px; margin-bottom:20px; }
    .stat { text-align:center; padding:15px; background:#f8f9fa; border-radius:10px; }
    .stat-label { font-size:.85em; color:#666; margin-bottom:5px; }
    .stat-value { font-size:1.8em; font-weight:bold; color:#667eea; }
    .state-display { text-align:center; padding:30px;
      background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);
      color:white; border-radius:10px; font-size:2em; margin-bottom:20px; }
    .controls { display:grid; grid-template-columns:1fr 1fr; gap:15px; }
    label { display:block; margin-bottom:5px; font-weight:500; color:#555; }
    input[type=number], input[type=text] {
      width:100%; padding:12px; border:2px solid #e0e0e0;
      border-radius:8px; font-size:1em; }
    input:focus { outline:none; border-color:#667eea; }
    button {
      padding:12px 20px; border:none; border-radius:8px; font-size:1em;
      font-weight:bold; cursor:pointer; transition:all .2s; }
    .btn-primary { background:#667eea; color:white; }
    .btn-primary:hover { background:#764ba2; transform:translateY(-2px);
      box-shadow:0 5px 15px rgba(0,0,0,.2); }
    .btn-danger { background:#e74c3c; color:white; margin-top:10px; width:100%; }
    .btn-danger:hover { background:#c0392b; }
    .btn-full { width:100%; margin-top:10px; }
    .session-list { max-height:200px; overflow-y:auto; }
    .session-item { padding:10px; background:#f8f9fa; margin-bottom:8px;
      border-radius:5px; display:flex; justify-content:space-between; }
    /* ── To-do ── */
    .todo-input-row { display:flex; gap:10px; margin-bottom:18px; }
    .todo-input-row input { flex:1; }
    .todo-input-row button { flex-shrink:0; }
    .todo-item {
      display:flex; align-items:center; gap:12px;
      padding:12px 14px; background:#f8f9fa; border-radius:10px;
      margin-bottom:10px; transition: opacity .2s;
    }
    .todo-item.done { opacity:.5; }
    .todo-checkbox {
      width:20px; height:20px; cursor:pointer; accent-color:#667eea;
      flex-shrink:0;
    }
    .todo-text { flex:1; font-size:1em; }
    .todo-item.done .todo-text { text-decoration:line-through; color:#aaa; }
    .todo-delete {
      background:none; border:none; color:#ccc; font-size:1.2em;
      cursor:pointer; padding:0 4px; line-height:1;
    }
    .todo-delete:hover { color:#e74c3c; }
    .todo-empty { text-align:center; color:#bbb; padding:20px 0; font-size:.95em; }
  </style>
</head>
<body>
<div class='container'>
  <h1>📚 Study Dashboard </h1>
  <div class='tabs'>
    <a class='tab )";
  s += (strcmp(activeTab, "dashboard") == 0) ? "active" : "";
  s += "' href='/'>Dashboard</a>";
  s += "<a class='tab ";
  s += (strcmp(activeTab, "tasks") == 0) ? "active" : "";
  s += "' href='/tasks'>Tasks</a>";
  s += "</div>";
  return s;
}

// ── Dashboard ─────────────────────────────────────────────────────────────────
void handleRoot() {
  String html = sharedHead("dashboard");
  html += R"(
    <div class='card'>
      <div class='state-display' id='state'>Loading...</div>
      <div class='status-grid'>
        <div class='stat'><div class='stat-label'>Sessions</div>
          <div class='stat-value' id='sessions'>0</div></div>
        <div class='stat'><div class='stat-label'>Total Study</div>
          <div class='stat-value' id='totalTime'>0m</div></div>
        <div class='stat'><div class='stat-label'>Breaks</div>
          <div class='stat-value' id='breaks'>0</div></div>
        <div class='stat'><div class='stat-label'>Present</div>
          <div class='stat-value' id='presence'>No</div></div>
      </div>
    </div>

    <div class='card'>
      <h2 style='margin-bottom:15px'>⚙️ Settings</h2>
      <div class='controls'>
        <div><label>Study Duration (min)</label>
          <input type='number' id='studyMin' value='10' min='1'></div>
        <div><label>Break Duration (min)</label>
          <input type='number' id='breakMin' value='2' min='1'></div>
      </div>
      <button class='btn-primary btn-full' onclick='updateSettings()'>Update Settings</button>
      <button class='btn-danger' onclick='resetSession()'>Reset Session</button>
    </div>

    <div class='card'>
      <h2 style='margin-bottom:15px'>📊 Recent Sessions</h2>
      <div class='session-list' id='sessionList'>
        <p style='text-align:center;color:#999'>No sessions yet</p>
      </div>
    </div>
  </div>

  <script>
    function updateStatus() {
      fetch('/api/status').then(r=>r.json()).then(data=>{
        document.getElementById('state').textContent = data.state;
        document.getElementById('sessions').textContent = data.sessionCount;
        document.getElementById('totalTime').textContent = Math.floor(data.totalStudyTime/60000)+'m';
        document.getElementById('breaks').textContent = data.breakCount;
        document.getElementById('presence').textContent = data.present ? 'Yes' : 'No';
        const list = document.getElementById('sessionList');
        if (data.sessions && data.sessions.length > 0) {
          list.innerHTML = data.sessions.map(s =>
            `<div class='session-item'>
              <span>Session ${s.id}</span>
              <span>${Math.floor(s.duration/60000)}m ${Math.floor((s.duration%60000)/1000)}s</span>
            </div>`).join('');
        }
      });
    }
    function updateSettings() {
      fetch(`/api/settings?study=${document.getElementById('studyMin').value}&break=${document.getElementById('breakMin').value}`)
        .then(()=>alert('Settings updated!'));
    }
    function resetSession() {
      if (confirm('Reset current session?')) fetch('/api/reset').then(()=>updateStatus());
    }
    setInterval(updateStatus, 1000);
    updateStatus();
  </script>
</body></html>)";
  server.send(200, "text/html", html);
}

// ── Tasks page ────────────────────────────────────────────────────────────────
void handleTasks() {
  // Handle POST actions first, then redirect
  if (server.method() == HTTP_POST) {
    if (server.hasArg("add") && todoCount < MAX_TODOS) {
      String t = server.arg("add"); t.trim();
      if (t.length() > 0) {
        todos[todoCount].text = t;
        todos[todoCount].completed = false;
        todoCount++;
      }
    }
    if (server.hasArg("toggle")) {
      int idx = server.arg("toggle").toInt();
      if (idx >= 0 && idx < todoCount) todos[idx].completed = !todos[idx].completed;
    }
    if (server.hasArg("delete")) {
      int idx = server.arg("delete").toInt();
      if (idx >= 0 && idx < todoCount) {
        for (int i = idx; i < todoCount - 1; i++) todos[i] = todos[i + 1];
        todoCount--;
      }
    }
    server.sendHeader("Location", "/tasks");
    server.send(303);
    return;
  }

  String html = sharedHead("tasks");
  html += "<div class='card'>";
  html += "<h2 style='margin-bottom:18px'>✅ Tasks</h2>";

  // Add-task form
  html += "<form method='POST' action='/tasks'>";
  html += "<div class='todo-input-row'>";
  html += "<input type='text' name='add' placeholder='Add a new task...' required>";
  html += "<button type='submit' class='btn-primary'>Add</button>";
  html += "</div></form>";

  // Task list
  if (todoCount == 0) {
    html += "<p class='todo-empty'>No tasks yet — add one above!</p>";
  } else {
    for (int i = 0; i < todoCount; i++) {
      html += "<div class='todo-item";
      if (todos[i].completed) html += " done";
      html += "'>";

      // Toggle checkbox
      html += "<form method='POST' action='/tasks' style='display:contents'>";
      html += "<input type='hidden' name='toggle' value='" + String(i) + "'>";
      html += "<input type='checkbox' class='todo-checkbox' onclick='this.form.submit()'";
      if (todos[i].completed) html += " checked";
      html += ">";
      html += "</form>";

      // Text
      html += "<span class='todo-text'>" + todos[i].text + "</span>";

      // Delete
      html += "<form method='POST' action='/tasks' style='display:contents'>";
      html += "<input type='hidden' name='delete' value='" + String(i) + "'>";
      html += "<button type='submit' class='todo-delete' title='Delete'>✕</button>";
      html += "</form>";

      html += "</div>";
    }
  }

  // Progress hint
  if (todoCount > 0) {
    int done = 0;
    for (int i = 0; i < todoCount; i++) if (todos[i].completed) done++;
    html += "<p style='text-align:right;color:#aaa;font-size:.85em;margin-top:10px'>";
    html += String(done) + " / " + String(todoCount) + " done</p>";
  }

  html += "</div></div></body></html>";
  server.send(200, "text/html", html);
}

// ── API endpoints ─────────────────────────────────────────────────────────────
void handleStatus() {
  String stateStr = "Idle";
  switch (currentState) {
    case IDLE:       stateStr = "Idle"; break;
    case STUDYING:   stateStr = "Studying"; break;
    case BREAK_TIME: stateStr = "Break Time"; break;
    case AWAY:       stateStr = "Away (paused)"; break;
  }

  String json = "{";
  json += "\"state\":\"" + stateStr + "\",";
  json += "\"present\":" + String(isPresent ? "true" : "false") + ",";
  json += "\"sessionCount\":" + String(sessionCount) + ",";
  json += "\"breakCount\":" + String(breakCount) + ",";
  json += "\"totalStudyTime\":" + String(totalStudyTime) + ",";
  json += "\"sessions\":[";

  int idx = 1;
  for (int i = 0; i < sessionRecordCount; i++) {
    if (sessions[i].isCompleted) {
      if (idx > 1) json += ",";
      json += "{\"id\":" + String(idx++) + ",\"duration\":" + String(sessions[i].duration) + "}";
    }
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleSettings() {
  if (server.hasArg("study")) studyDurationSetting = server.arg("study").toInt() * 60UL * 1000UL;
  if (server.hasArg("break")) breakDurationSetting = server.arg("break").toInt() * 60UL * 1000UL;
  server.send(200, "text/plain", "OK");
}

void handleReset() {
  currentState = IDLE;
  stateBeforeAway = IDLE;
  sessionStartTime = 0;
  pausedSessionTime = 0;
  currentSessionElapsed = 0;
  server.send(200, "text/plain", "OK");
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\nStudy Dashboard Starting...");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  for (int i = 0; i < FILTER_SAMPLES; i++) distanceReadings[i] = -1;

  delay(1000);
  Wire.begin(D1, D2);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledReady = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Study Dashboard");
    display.println("\nConnecting WiFi...");
    display.display();
    Serial.println("OLED initialized");
  } else {
    Serial.println("OLED init failed");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nIP: "); Serial.println(WiFi.localIP());
    if (oledReady) {
      display.clearDisplay(); display.setCursor(0, 0);
      display.println("WiFi Connected!");
      display.println(""); display.print("IP: ");
      display.println(WiFi.localIP()); display.display();
      delay(3000);
    }
  } else {
    Serial.println("\nWiFi failed!");
    if (oledReady) {
      display.clearDisplay(); display.setCursor(0, 0);
      display.println("WiFi Failed!"); display.println("Check credentials");
      display.display();
    }
  }

  server.on("/",            handleRoot);
  server.on("/tasks",       handleTasks);
  server.on("/api/status",  handleStatus);
  server.on("/api/settings",handleSettings);
  server.on("/api/reset",   handleReset);
  server.begin();
  Serial.println("Web server started");

  lastPresenceChangeTime = millis();
}

void loop() {
  server.handleClient();
  updatePresenceState();
  updateStateMachine();
  updateDisplay();
  delay(100);
}
