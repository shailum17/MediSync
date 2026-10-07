// ================================================================
//  MediSync v5.0 — Smart Medicine Reminder & Dispenser
//  Arduino UNO + DS1302 RTC + Servo + ESP8266 (WiFi) + Buzzer/LEDs
//
//  ----------------------------------------------------------------
//  WIRING
//  ----------------------------------------------------------------
//    DS1302 DAT   → Pin 4
//    DS1302 CLK   → Pin 5
//    DS1302 RST   → Pin 6
//    Green LED    → Pin 7   (220Ω resistor to GND)
//    Button       → Pin 8   (other leg to GND, uses INPUT_PULLUP)
//    Servo signal → Pin 9
//    ESP8266 RX   ← Pin 10  (Arduino's SoftwareSerial TX, via 1k/2k divider)
//    ESP8266 TX   → Pin 11  (Arduino's SoftwareSerial RX)
//    Red LED      → Pin 12  (220Ω resistor to GND)
//    Buzzer       → Pin 13
//    ESP8266 VCC  → 3.3V    (NOT 5V — will damage the module)
//    ESP8266 CH_PD→ 3.3V
//    ESP8266 GND  → Common GND with Arduino
//
//  ----------------------------------------------------------------
//  LED MEANING
//  ----------------------------------------------------------------
//    Red only          : Phase 1 — alarm just fired, waiting for button
//    Yellow (Red+Green): Phase 2 — 5-minute reminder buzzer running
//    Green only (3s)    : Dose confirmed taken (on-time or late)
//    Red, held steady   : Dose was NOT taken within the 5-min window
//                          (stays on until the next alarm fires)
//
//  ----------------------------------------------------------------
//  SERIAL MONITOR COMMANDS  (9600 baud, Newline line ending)
//  ----------------------------------------------------------------
//    A HH MM   — set next alarm,  e.g.  A 8 30   or   A 14 0
//    A?        — print current alarm time
//
//  ----------------------------------------------------------------
//  REQUIRED LIBRARIES (Library Manager)
//  ----------------------------------------------------------------
//    "Rtc by Makuna"   (provides RtcDS1302.h)
//    "Servo"           (built-in)
// ================================================================

#include <RtcDS1302.h>
#include <Servo.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>

// ================================================================
//  USER CONFIGURATION
// ================================================================

// Set true ONCE to write the time below into the RTC, upload,
// confirm success in Serial Monitor, then set back to false and
// re-upload. Leaving this true will reset the clock every boot.


#define SET_TIME    false
#define SET_YEAR    2026
#define SET_MONTH   6
#define SET_DAY     19
#define SET_HOUR    12
#define SET_MINUTE  0
#define SET_SECOND  0

// Initial alarm time (can be changed later via Serial command or
// the web dashboard while running)
int alarmHour   = 9;
int alarmMinute = 0;

// WiFi credentials
const char WIFI_SSID[] PROGMEM = "YOUR_WIFI_SSID";
const char WIFI_PASS[] PROGMEM = "YOUR_WIFI_PASSWORD";

// CallMeBot WhatsApp settings — sign up at callmebot.com to get
// your phone whitelisted and obtain an API key
const char WA_NUMBER[] PROGMEM = "91XXXXXXXXXX";   // include country code, no +
const char WA_APIKEY[] PROGMEM = "YOUR_CALLMEBOT_APIKEY";

// IFTTT Webhooks (optional — leave blank/default to skip)
const char IFTTT_KEY[]    PROGMEM = "YOUR_IFTTT_KEY";
const char IFTTT_TAKEN[]  PROGMEM = "dose_taken";
const char IFTTT_MISSED[] PROGMEM = "dose_missed";

// Timing constants
const unsigned long PHASE1_MS       = 10000UL;   // 10s initial window
const unsigned long PHASE2_MS       = 300000UL;  // 5 minutes reminder
const unsigned long BUZZ_ON_MS      = 300UL;
const unsigned long BUZZ_OFF_MS     = 200UL;

// ================================================================
//  PIN DEFINITIONS
// ================================================================
#define SERVO_PIN   9
#define BUZZER_PIN  13
#define GREEN_LED   7
#define RED_LED     12
#define BUTTON_PIN  8
#define ESP_RX_PIN  10   // Arduino reads ESP8266 TX here
#define ESP_TX_PIN  11   // Arduino writes to ESP8266 RX here

// ================================================================
//  OBJECTS
// ================================================================
ThreeWire   rtcWire(4, 5, 6);          // DAT, CLK, RST
RtcDS1302<ThreeWire> rtc(rtcWire);
Servo       servo;
SoftwareSerial esp8266(ESP_RX_PIN, ESP_TX_PIN);

// ================================================================
//  STATE
// ================================================================
bool alarmFired  = false;   // true while today's alarm cycle is active
bool doseMissed  = false;   // true → red LED held on until next alarm
bool wifiReady   = false;   // true only if WiFi actually verified connected
char webLog[80]  = "";      // rolling dose history shown on dashboard

// ================================================================
//  SMALL UTILITIES
// ================================================================

void setRGB(bool red, bool green) {
  digitalWrite(RED_LED,   red   ? HIGH : LOW);
  digitalWrite(GREEN_LED, green ? HIGH : LOW);
}

void beep(int times, int onMs = 100, int offMs = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

void printTwoDigit(int v) {
  if (v < 10) Serial.print('0');
  Serial.print(v);
}

void printClock(int h, int m, int s) {
  printTwoDigit(h); Serial.print(':');
  printTwoDigit(m); Serial.print(':');
  printTwoDigit(s);
}

void fmtTime(char* buf, int h, int m) {
  buf[0] = '0' + (h / 10); buf[1] = '0' + (h % 10);
  buf[2] = ':';
  buf[3] = '0' + (m / 10); buf[4] = '0' + (m % 10);
  buf[5] = '\0';
}

void appendLog(const char* entry) {
  if (strlen(webLog) + strlen(entry) + 4 >= sizeof(webLog)) {
    char* pipe = strchr(webLog, '|');
    if (pipe) memmove(webLog, pipe + 2, strlen(pipe + 2) + 1);
    else      webLog[0] = '\0';
  }
  if (webLog[0] != '\0') strncat(webLog, " | ", sizeof(webLog) - strlen(webLog) - 1);
  strncat(webLog, entry, sizeof(webLog) - strlen(webLog) - 1);
}

// Print a PROGMEM string char by char — avoids allocating a large
// local buffer (keeps UNO's 2KB RAM safe)
void printProgmemString(const char* progmemStr) {
  char c;
  const char* p = progmemStr;
  while ((c = pgm_read_byte(p++)) != '\0') Serial.print(c);
}

void sendProgmemToEsp(const char* progmemStr) {
  char c;
  const char* p = progmemStr;
  while ((c = pgm_read_byte(p++)) != '\0') esp8266.print(c);
}

// ================================================================
//  ESP8266 — REAL AT-COMMAND COMMUNICATION
//  (No simulated text. Every printed line is what the module
//   actually sent back, character for character.)
// ================================================================

// Sends a command, waits up to `timeoutMs` for a line containing
// `expect` (or `failTok`). Echoes every real byte received to the
// Serial Monitor as it arrives. Returns true only on a genuine match.
bool sendAndWait(const __FlashStringHelper* cmd,
                  const char* expect,
                  const char* failTok,
                  unsigned long timeoutMs) {
  // Clear any stale bytes first
  while (esp8266.available()) esp8266.read();

  esp8266.println(cmd);
  Serial.print(F(">> "));
  Serial.println(cmd);

  char window[64];
  memset(window, 0, sizeof(window));
  uint8_t wlen = 0;

  unsigned long start = millis();
  bool gotPrintable = false;

  while (millis() - start < timeoutMs) {
    while (esp8266.available()) {
      char c = esp8266.read();
      Serial.write(c);              // print the REAL byte from the module
      gotPrintable = true;

      // maintain a small rolling window to test for expect/fail tokens
      if (wlen < sizeof(window) - 1) {
        window[wlen++] = c;
      } else {
        memmove(window, window + 1, sizeof(window) - 2);
        window[sizeof(window) - 2] = c;
      }
      window[wlen < sizeof(window) - 1 ? wlen : sizeof(window) - 1] = '\0';

      if (expect  && strstr(window, expect))  { Serial.println(); return true;  }
      if (failTok && strstr(window, failTok)) { Serial.println(); return false; }
    }
  }

  if (!gotPrintable) {
    Serial.println(F("[no response from ESP8266 — check wiring/power]"));
  } else {
    Serial.println(F("\n[timeout waiting for expected reply]"));
  }
  return false;
}

// ================================================================
//  WIFI — REAL CONNECTION ATTEMPT, HONEST RESULT
// ================================================================

bool connectWiFi() {
  Serial.println(F("\n=== Connecting to WiFi ==="));

  bool atOk = sendAndWait(F("AT"), "OK", "ERROR", 1500);
  if (!atOk) {
    Serial.println(F("ESP8266 is not responding to basic AT command."));
    Serial.println(F("Check: 3.3V power, TX/RX wiring, CH_PD tied to 3.3V."));
    return false;
  }

  sendAndWait(F("AT+CWMODE=1"), "OK", "ERROR", 2000);

  sendAndWait(F("AT+CIPMUX=0"), "OK", "ERROR", 2000);

  // Build AT+CWJAP="ssid","pass" without large stack buffers
  Serial.print(F(">> AT+CWJAP=\""));
  printProgmemString(WIFI_SSID);
  Serial.println(F("\",\"********\""));   // password hidden in Serial log

  esp8266.print(F("AT+CWJAP=\""));
  sendProgmemToEsp(WIFI_SSID);
  esp8266.print(F("\",\""));
  sendProgmemToEsp(WIFI_PASS);
  esp8266.println(F("\""));

  // Real join can take up to ~15s
  char window[64];
  memset(window, 0, sizeof(window));
  uint8_t wlen = 0;
  unsigned long start = millis();
  bool joined = false;
  bool failed = false;

  while (millis() - start < 15000UL) {
    while (esp8266.available()) {
      char c = esp8266.read();
      Serial.write(c);
      if (wlen < sizeof(window) - 1) window[wlen++] = c;
      else { memmove(window, window + 1, sizeof(window) - 2); window[sizeof(window) - 2] = c; }
      window[wlen < sizeof(window) - 1 ? wlen : sizeof(window) - 1] = '\0';

      if (strstr(window, "OK") && strstr(window, "WIFI GOT IP")) { joined = true; }
      if (strstr(window, "FAIL"))  failed = true;
      if (strstr(window, "ERROR")) failed = true;
    }
    if (joined || failed) break;
  }
  Serial.println();

  if (!joined) {
    Serial.println(F(">>> WiFi join failed or timed out <<<"));
    return false;
  }

  // Confirm with CIFSR (prints real IP if connected)
  sendAndWait(F("AT+CIFSR"), "OK", "ERROR", 3000);

  sendAndWait(F("AT+CIPMUX=1"), "OK", "ERROR", 2000);

  Serial.println(F(">>> WiFi Connected <<<"));
  return true;
}

bool startWebServer() {
  bool ok = sendAndWait(F("AT+CIPSERVER=1,80"), "OK", "ERROR", 2000);
  if (ok) Serial.println(F("Web server listening on port 80."));
  else    Serial.println(F("Failed to start web server."));
  return ok;
}

// Generic HTTP GET over a TCP socket. Returns true if CIPSEND was
// accepted; does not guarantee the remote server responded 200.
bool httpGet(const char* host, const char* path) {
  if (!wifiReady) {
    Serial.println(F("Skipping HTTP request — WiFi not connected."));
    return false;
  }

  char cipCmd[64];
  snprintf(cipCmd, sizeof(cipCmd), "AT+CIPSTART=\"TCP\",\"%s\",80", host);
  while (esp8266.available()) esp8266.read();
  esp8266.println(cipCmd);
  Serial.print(F(">> ")); Serial.println(cipCmd);

  unsigned long t = millis();
  bool started = false;
  while (millis() - t < 6000UL) {
    while (esp8266.available()) {
      char c = esp8266.read();
      Serial.write(c);
      started = true;
    }
  }
  Serial.println();
  if (!started) {
    Serial.println(F("No response to CIPSTART — aborting request."));
    return false;
  }

  char req[200];
  int reqLen = snprintf(req, sizeof(req),
    "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
    path, host);
  if (reqLen <= 0 || reqLen >= (int)sizeof(req)) {
    Serial.println(F("Request too long — aborting."));
    sendAndWait(F("AT+CIPCLOSE"), "OK", "ERROR", 2000);
    return false;
  }

  char sendCmd[24];
  snprintf(sendCmd, sizeof(sendCmd), "AT+CIPSEND=%d", reqLen);
  while (esp8266.available()) esp8266.read();
  esp8266.println(sendCmd);
  Serial.print(F(">> ")); Serial.println(sendCmd);

  // Wait for the '>' prompt that means ESP8266 is ready for raw data
  unsigned long pt = millis();
  bool promptSeen = false;
  while (millis() - pt < 3000UL) {
    while (esp8266.available()) {
      char c = esp8266.read();
      Serial.write(c);
      if (c == '>') promptSeen = true;
    }
  }
  Serial.println();

  if (!promptSeen) {
    Serial.println(F("ESP8266 did not prompt for data — aborting send."));
    sendAndWait(F("AT+CIPCLOSE"), "OK", "ERROR", 2000);
    return false;
  }

  esp8266.print(req);
  memset(req, 0, sizeof(req));

  // Read the real HTTP response (or timeout) and print it as-is
  unsigned long rt = millis();
  while (millis() - rt < 5000UL) {
    while (esp8266.available()) Serial.write(esp8266.read());
  }
  Serial.println();

  sendAndWait(F("AT+CIPCLOSE"), "OK", "ERROR", 2000);
  return true;
}

void sendWhatsApp(const char* msg) {
  Serial.println(F("Sending WhatsApp via CallMeBot..."));
  char num[20], key[40];
  strncpy_P(num, WA_NUMBER, sizeof(num) - 1);  num[sizeof(num) - 1] = '\0';
  strncpy_P(key, WA_APIKEY, sizeof(key) - 1);  key[sizeof(key) - 1] = '\0';

  char path[160];
  snprintf(path, sizeof(path),
    "/whatsapp.php?phone=%s&text=%s&apikey=%s", num, msg, key);

  bool ok = httpGet("api.callmebot.com", path);
  memset(path, 0, sizeof(path));
  memset(num, 0, sizeof(num));
  memset(key, 0, sizeof(key));

  Serial.println(ok ? F("WhatsApp request sent.") : F("WhatsApp request failed."));
}

void sendIFTTT(const char* eventProgmem, const char* v1, const char* v2) {
  char evt[24], ikey[40];
  strncpy_P(evt,  eventProgmem, sizeof(evt)  - 1); evt[sizeof(evt) - 1] = '\0';
  strncpy_P(ikey, IFTTT_KEY,    sizeof(ikey) - 1); ikey[sizeof(ikey) - 1] = '\0';

  // Skip silently if the user hasn't configured IFTTT
  if (strcmp(ikey, "YOUR_IFTTT_KEY") == 0) return;

  char path[160];
  snprintf(path, sizeof(path),
    "/trigger/%s/with/key/%s?value1=%s&value2=%s", evt, ikey, v1, v2);

  bool ok = httpGet("maker.ifttt.com", path);
  memset(path, 0, sizeof(path));
  memset(ikey, 0, sizeof(ikey));

  Serial.println(ok ? F("IFTTT request sent.") : F("IFTTT request failed."));
}

// ================================================================
//  WEB DASHBOARD — serves real current time, alarm, and a form to
//  set the next alarm. Only responds when the ESP8266 actually
//  forwards an incoming client request (+IPD).
// ================================================================

int parseUrlParam(const char* req, const char* key) {
  const char* p = strstr(req, key);
  if (!p) return -1;
  p += strlen(key);
  int val = 0;
  bool found = false;
  while (*p >= '0' && *p <= '9') {
    val = val * 10 + (*p - '0');
    p++;
    found = true;
  }
  return found ? val : -1;
}

void handleWebRequest() {
  if (!esp8266.available()) return;   // non-blocking — nothing to do

  char req[160];
  memset(req, 0, sizeof(req));
  uint8_t idx = 0;
  unsigned long t = millis();
  while (millis() - t < 100UL && idx < sizeof(req) - 1) {
    if (esp8266.available()) {
      req[idx++] = esp8266.read();
      t = millis();
    }
  }
  while (esp8266.available()) esp8266.read();   // discard overflow

  if (!strstr(req, "GET")) return;
  char* ipd = strstr(req, "+IPD,");
  if (!ipd) return;
  char connId = *(ipd + 5);

  bool alarmUpdated = false;
  if (strstr(req, "GET /setalarm")) {
    int nh = parseUrlParam(req, "h=");
    int nm = parseUrlParam(req, "m=");
    if (nh >= 0 && nh <= 23 && nm >= 0 && nm <= 59) {
      alarmHour    = nh;
      alarmMinute  = nm;
      alarmFired   = false;
      doseMissed   = false;
      setRGB(false, false);
      alarmUpdated = true;
      Serial.print(F("Alarm updated via web dashboard: "));
      printTwoDigit(alarmHour); Serial.print(':'); printTwoDigit(alarmMinute);
      Serial.println();
    }
  }

  bool triggerNow = strstr(req, "GET /trigger") != NULL;

  RtcDateTime now = rtc.GetDateTime();
  char nowStr[6], alarmStr[6];
  fmtTime(nowStr,   now.Hour(),   now.Minute());
  fmtTime(alarmStr, alarmHour,    alarmMinute);

  // ---- Chunk 1: header, styles, time + alarm cards ----
  char cipCmd[24];
  snprintf(cipCmd, sizeof(cipCmd), "AT+CIPSEND=%c,%d", connId, 600);
  while (esp8266.available()) esp8266.read();
  esp8266.println(cipCmd);
  { unsigned long pt = millis();
    while (millis() - pt < 1500UL) { while (esp8266.available()) esp8266.read(); } }

  esp8266.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"));
  esp8266.print(F("<html><head><title>MediSync</title>"));
  esp8266.print(F("<meta http-equiv='refresh' content='15'>"));
  esp8266.print(F("<style>body{text-align:center;background:#f0f4ff;font-family:sans-serif;margin:0;padding:10px}"));
  esp8266.print(F("h1{color:#2c3e50}.card{background:#fff;border-radius:10px;padding:16px;margin:10px auto;max-width:360px;box-shadow:0 2px 6px rgba(0,0,0,.1)}"));
  esp8266.print(F(".time{font-size:34px;font-weight:bold;color:#2c3e50}.ok{color:#27ae60;font-weight:bold}"));
  esp8266.print(F("input[type=number]{width:54px;font-size:18px;text-align:center}"));
  esp8266.print(F("button{background:#2c3e50;color:#fff;border:none;padding:9px 18px;border-radius:6px;font-size:14px}"));
  esp8266.print(F("button.red{background:#e74c3c}</style></head><body><h1>MediSync</h1>"));
  esp8266.print(F("<div class='card'><h2>Current Time</h2><div class='time'>"));
  esp8266.print(nowStr);
  esp8266.print(F("</div></div><div class='card'><h2>Active Alarm</h2><div class='time'>"));
  esp8266.print(alarmStr);
  if (alarmUpdated) esp8266.print(F("<br><span class='ok'>Alarm saved</span>"));
  esp8266.print(F("</div><br><a href='/trigger'><button class='red'>Trigger Now</button></a></div>"));

  delay(150);

  // ---- Chunk 2: set-alarm form + log ----
  snprintf(cipCmd, sizeof(cipCmd), "AT+CIPSEND=%c,%d", connId, 380);
  while (esp8266.available()) esp8266.read();
  esp8266.println(cipCmd);
  { unsigned long pt = millis();
    while (millis() - pt < 1500UL) { while (esp8266.available()) esp8266.read(); } }

  esp8266.print(F("<div class='card'><h2>Set Next Alarm</h2>"));
  esp8266.print(F("<form method='GET' action='/setalarm'>"));
  esp8266.print(F("<input type='number' name='h' min='0' max='23' value='"));
  esp8266.print(alarmHour);
  esp8266.print(F("'> : <input type='number' name='m' min='0' max='59' value='"));
  esp8266.print(alarmMinute);
  esp8266.print(F("'><br><br><button type='submit'>Save Alarm</button></form></div>"));
  esp8266.print(F("<div class='card'><h2>Log</h2><p>"));
  esp8266.print(webLog[0] == '\0' ? "No doses yet." : webLog);
  esp8266.print(F("</p></div></body></html>"));

  delay(300);
  char closeCmd[20];
  snprintf(closeCmd, sizeof(closeCmd), "AT+CIPCLOSE=%c", connId);
  esp8266.println(closeCmd);
  { unsigned long pt = millis();
    while (millis() - pt < 1000UL) { while (esp8266.available()) esp8266.read(); } }

  if (triggerNow && !alarmFired) {
    Serial.println(F("Remote trigger received from web dashboard."));
    alarmFired = true;
    runAlarm();
  }
}

// ================================================================
//  ALARM SEQUENCE
// ================================================================

void runAlarm() {
  Serial.println(F("\n*** ALARM TRIGGERED ***"));
  beep(3, 150, 150);

  RtcDateTime now = rtc.GetDateTime();
  char timeStr[6];
  fmtTime(timeStr, now.Hour(), now.Minute());

  Serial.println(F("Opening compartment..."));
  servo.write(90);

  // ---- Phase 1: 10s window, RED LED ----
  Serial.println(F("Phase 1: 10s window for button press (Red LED)."));
  setRGB(true, false);

  unsigned long start = millis();
  bool taken = false;
  while (millis() - start < PHASE1_MS) {
    if ((millis() - start) % 3000UL < 150UL) {
      digitalWrite(BUZZER_PIN, HIGH);
      delay(100);
      digitalWrite(BUZZER_PIN, LOW);
    }
    if (digitalRead(BUTTON_PIN) == LOW) {
      delay(50);   // debounce
      taken = true;
      break;
    }
    delay(20);
  }

  servo.write(0);

  if (taken) {
    Serial.println(F("RESULT: Dose taken on time."));
    digitalWrite(BUZZER_PIN, LOW);
    setRGB(false, true);
    beep(2, 100, 100);
    delay(3000);
    setRGB(false, false);

    char entry[14] = "T ";
    strncat(entry, timeStr, sizeof(entry) - 3);
    appendLog(entry);

    char msg[56] = "MediSync:+Dose+TAKEN+at+";
    strncat(msg, timeStr, sizeof(msg) - strlen(msg) - 1);
    strncat(msg, ".+Good+job!", sizeof(msg) - strlen(msg) - 1);
    sendWhatsApp(msg);
    sendIFTTT(IFTTT_TAKEN, timeStr, "Taken");

    Serial.println(F("*** ALARM DONE ***\n"));
    return;
  }

  // ---- Phase 2: 5-minute reminder window, YELLOW LED ----
  Serial.println(F("Phase 2: 5-minute reminder buzzer (Yellow LED)."));
  setRGB(true, true);

  unsigned long missStart = millis();
  bool lateTaken = false;
  while (millis() - missStart < PHASE2_MS) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(BUZZ_ON_MS);
    digitalWrite(BUZZER_PIN, LOW);
    delay(BUZZ_OFF_MS);

    if (digitalRead(BUTTON_PIN) == LOW) {
      delay(50);
      lateTaken = true;
      break;
    }
  }
  digitalWrite(BUZZER_PIN, LOW);

  if (lateTaken) {
    Serial.println(F("RESULT: Late dose taken."));
    setRGB(false, true);
    beep(2, 100, 100);
    delay(3000);
    setRGB(false, false);

    char entry[14] = "L ";
    strncat(entry, timeStr, sizeof(entry) - 3);
    appendLog(entry);

    char msg[60] = "MediSync:+Late+dose+taken+at+";
    strncat(msg, timeStr, sizeof(msg) - strlen(msg) - 1);
    strncat(msg, ".+Better+late!", sizeof(msg) - strlen(msg) - 1);
    sendWhatsApp(msg);
    sendIFTTT(IFTTT_TAKEN, timeStr, "Late");

  } else {
    Serial.println(F("RESULT: Dose fully missed."));
    doseMissed = true;
    setRGB(true, false);   // Red — held until next alarm

    char entry[14] = "X ";
    strncat(entry, timeStr, sizeof(entry) - 3);
    appendLog(entry);

    char msg[56] = "MediSync:+DOSE+MISSED+at+";
    strncat(msg, timeStr, sizeof(msg) - strlen(msg) - 1);
    strncat(msg, ".+Please+take+medicine!", sizeof(msg) - strlen(msg) - 1);
    sendWhatsApp(msg);
    sendIFTTT(IFTTT_MISSED, timeStr, "Missed");

    setRGB(true, false);   // re-assert after network calls
  }

  Serial.println(F("*** ALARM DONE ***\n"));
}

// ================================================================
//  SERIAL COMMAND HANDLER — set alarm time manually
// ================================================================

void handleSerialCommand() {
  if (!Serial.available()) return;

  char buf[16];
  memset(buf, 0, sizeof(buf));
  uint8_t i = 0;
  while (Serial.available() && i < sizeof(buf) - 1) {
    buf[i++] = Serial.read();
  }

  char* p = buf;
  while (*p == ' ' || *p == '\r' || *p == '\n') p++;

  if (*p != 'A' && *p != 'a') return;
  p++;

  if (*p == '?') {
    Serial.print(F("Current alarm: "));
    printTwoDigit(alarmHour); Serial.print(':'); printTwoDigit(alarmMinute);
    Serial.println();
    return;
  }

  while (*p == ' ') p++;

  int nh = -1, nm = -1;
  if (*p >= '0' && *p <= '9') {
    nh = 0;
    while (*p >= '0' && *p <= '9') { nh = nh * 10 + (*p - '0'); p++; }
    while (*p == ' ' || *p == ':') p++;
    if (*p >= '0' && *p <= '9') {
      nm = 0;
      while (*p >= '0' && *p <= '9') { nm = nm * 10 + (*p - '0'); p++; }
    }
  }

  if (nh >= 0 && nh <= 23 && nm >= 0 && nm <= 59) {
    alarmHour   = nh;
    alarmMinute = nm;
    alarmFired  = false;
    doseMissed  = false;
    setRGB(false, false);
    Serial.print(F("Alarm set to "));
    printTwoDigit(alarmHour); Serial.print(':'); printTwoDigit(alarmMinute);
    Serial.println();
  } else {
    Serial.println(F("Usage: A HH MM   (e.g. A 8 30)   or   A?"));
  }
}

// ================================================================
//  SETUP
// ================================================================

void setup() {
  Serial.begin(9600);
  esp8266.begin(9600);
  delay(300);

  servo.attach(SERVO_PIN);
  servo.write(0);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED,  OUTPUT);
  pinMode(RED_LED,    OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);
  setRGB(false, false);

  rtc.Begin();

  if (SET_TIME) {
    Serial.println(F("\n=== TIME SET MODE ==="));
    RtcDateTime setDT(SET_YEAR, SET_MONTH, SET_DAY,
                       SET_HOUR, SET_MINUTE, SET_SECOND);
    rtc.SetDateTime(setDT);
    delay(200);

    RtcDateTime check = rtc.GetDateTime();
    Serial.print(F("RTC now reads: "));
    printClock(check.Hour(), check.Minute(), check.Second());
    Serial.println();

    if (check.Hour() == SET_HOUR && check.Minute() == SET_MINUTE) {
      Serial.println(F("SUCCESS. Set SET_TIME to false and re-upload."));
      for (int i = 0; i < 5; i++) { setRGB(false, true); delay(250); setRGB(false, false); delay(250); }
    } else {
      Serial.println(F("FAILED to set time — check DS1302 wiring."));
      for (int i = 0; i < 8; i++) { setRGB(true, false); delay(150); setRGB(false, false); delay(150); }
    }
    return;   // stay in this mode, do not proceed to normal operation
  }

  if (!rtc.IsDateTimeValid()) {
    Serial.println(F("WARNING: RTC reports invalid date/time."));
    Serial.println(F("Set SET_TIME to true, upload once, then set back to false."));
  }

  wifiReady = connectWiFi();
  if (wifiReady) {
    startWebServer();
  } else {
    Serial.println(F("Continuing without WiFi — alarms, servo, and"));
    Serial.println(F("local buzzer/LED logic still work normally."));
  }

  beep(1, 100, 0);

  Serial.println(F("\n====== MediSync Ready ======"));
  Serial.print(F("Alarm : "));
  printTwoDigit(alarmHour); Serial.print(':'); printTwoDigit(alarmMinute);
  Serial.println();
  Serial.print(F("WiFi  : "));
  Serial.println(wifiReady ? F("Connected") : F("Offline"));
  Serial.println(F("Type 'A HH MM' in Serial Monitor to set a new alarm."));
  Serial.println(F("=============================\n"));
}

// ================================================================
//  MAIN LOOP
// ================================================================

void loop() {
  if (SET_TIME) return;   // stay parked in time-set mode

  handleSerialCommand();
  if (wifiReady) handleWebRequest();

  RtcDateTime now = rtc.GetDateTime();
  int h = now.Hour();
  int m = now.Minute();
  int s = now.Second();

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 10000UL) {
    Serial.print(F("Time: "));
    printClock(h, m, s);
    Serial.print(F("   Alarm: "));
    printTwoDigit(alarmHour); Serial.print(':'); printTwoDigit(alarmMinute);
    Serial.print(F("   WiFi: "));
    Serial.println(wifiReady ? F("Connected") : F("Offline"));
    lastPrint = millis();
  }

  if (h == alarmHour && m == alarmMinute && s == 0 && !alarmFired) {
    alarmFired = true;
    doseMissed = false;
    setRGB(false, false);
    runAlarm();
  }

  // Keep red LED held on if the last dose was missed
  if (doseMissed) {
    setRGB(true, false);
  }

  // Clear the "fired today" flag once the alarm minute has passed,
  // without touching the LED if a dose is still marked missed
  if (m != alarmMinute && m != ((alarmMinute + 1) % 60)) {
    alarmFired = false;
    if (!doseMissed) setRGB(false, false);
  }

  delay(50);
}
