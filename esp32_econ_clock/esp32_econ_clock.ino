// Economic Calendar Clock - ESP32 firmware
//
// OLED (SSD1306, I2C): SDA=21, SCL=22 -> event name + countdown
// Active buzzer: GPIO 25               -> beeps on a new event, and at 5 min
//
// Libraries (Arduino IDE -> Library Manager):
//   "Adafruit SSD1306", "Adafruit GFX Library", "ArduinoJson"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <time.h>

// Credentials live in arduino_secrets.h, which is gitignored.
// Copy arduino_secrets.example.h to arduino_secrets.h and fill it in.
#include "arduino_secrets.h"

const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;
const char* API_URL   = SECRET_API_URL;

#define OLED_W 128
#define OLED_H 64
#define BUZZER   25   // buzzer +
#define BUZZER_N 26   // buzzer - (held low in code as the ground return)

const unsigned long POLL_MS  = 10UL * 60UL * 1000UL;  // 10 min
const unsigned long RETRY_MS = 45UL * 1000UL;         // after a failed poll / 503

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

String eventName = "";
time_t eventTime = 0;         // UTC epoch of the next event, 0 = none
unsigned long lastPoll = 0;
bool haveData = false;
bool buzzed = false;
bool newEvent = false;
bool oledReady = false;

void showMessage(const char* msg) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println(msg);
  oled.display();
}

void drawScreen(long secs) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Event name: small text at the top, broken on spaces (21 chars per line).
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  String line = "";
  String rest = eventName;
  while (rest.length() > 0) {
    int sp = rest.indexOf(' ');
    String word = (sp < 0) ? rest : rest.substring(0, sp);
    rest = (sp < 0) ? "" : rest.substring(sp + 1);
    if (line.length() == 0) {
      line = word;
    } else if (line.length() + 1 + word.length() <= 21) {
      line += " " + word;
    } else {
      oled.println(line);
      line = word;
    }
  }
  if (line.length() > 0) oled.println(line);

  // Countdown: large text along the bottom.
  char buf[16];
  if (secs >= 86400) {
    snprintf(buf, sizeof(buf), "%ldd %02ld:%02ld",
             secs / 86400, (secs % 86400) / 3600, (secs % 3600) / 60);
  } else {
    snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld",
             secs / 3600, (secs % 3600) / 60, secs % 60);
  }
  oled.setTextSize(2);
  oled.setCursor(0, 46);
  oled.print(buf);

  oled.display();
}

// Parse "2026-09-10T12:30:00" (naive UTC) into an epoch.
// configTime(0, 0, ...) puts the RTC in UTC, so mktime treats it as UTC too.
time_t parseIsoUtc(const char* s) {
  struct tm tmv = {0};
  if (sscanf(s, "%d-%d-%dT%d:%d:%d",
             &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday,
             &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) != 6) {
    return 0;
  }
  tmv.tm_year -= 1900;
  tmv.tm_mon  -= 1;
  return mktime(&tmv);
}

bool fetchNextEvent() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // Render serves HTTPS, a local Flask box serves HTTP - support both.
  // setInsecure() skips certificate checking; fine for public calendar data,
  // and it avoids shipping a CA bundle that expires.
  WiFiClientSecure secure;
  WiFiClient plain;
  HTTPClient http;

  if (String(API_URL).startsWith("https")) {
    secure.setInsecure();
    http.begin(secure, API_URL);
  } else {
    http.begin(plain, API_URL);
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(12000);        // TLS handshake needs more headroom
  int code = http.GET();

  if (code != 200) {                 // 503 = server still warming up
    Serial.printf("HTTP %d\n", code);
    http.end();
    return false;
  }

  // Keep only the fields we need so the document stays small.
  JsonDocument filter;
  JsonObject f = filter.add<JsonObject>();
  f["name"] = true;
  f["datetime"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return false;
  }

  time_t now = time(nullptr);
  time_t best = 0;
  String bestName = "";

  for (JsonObject ev : doc.as<JsonArray>()) {
    const char* dt = ev["datetime"];
    const char* nm = ev["name"];
    if (!dt || !nm) continue;
    time_t t = parseIsoUtc(dt);
    if (t > now && (best == 0 || t < best)) {
      best = t;
      bestName = nm;
    }
  }

  if (best == 0) return false;

  if (best != eventTime) {                 // a different event than before
    buzzed = false;                        // re-arm the 5-minute alert
    newEvent = true;                       // and announce it with the buzzer
  }
  eventTime = best;
  eventName = bestName;
  haveData = true;
  Serial.printf("Next: %s in %ld s\n", eventName.c_str(), (long)(best - now));
  return true;
}

// Active buzzer: fixed pitch, so alerts are told apart by rhythm.
// A pattern is {on, off, on, off, ...} in milliseconds.
void playPattern(const int* pat, int len) {
  digitalWrite(BUZZER_N, LOW);              // acts as the ground return
  for (int i = 0; i < len; i += 2) {
    digitalWrite(BUZZER, HIGH);
    delay(pat[i]);
    digitalWrite(BUZZER, LOW);
    delay(pat[i + 1]);
  }
}

// Two quick taps - "there's a new event on the board".
const int PATTERN_NEW[] = {110, 90, 110, 0};

// Three fast, one long, three times over - the "get up now" alarm.
const int PATTERN_ALERT[] = {
  90, 80,  90, 80,  90, 250,  500, 350,
  90, 80,  90, 80,  90, 250,  500, 350,
  90, 80,  90, 80,  90, 250,  700, 0
};

void alertIfDue(long secs) {
  if (secs <= 300 && !buzzed) {             // 5 minutes out
    playPattern(PATTERN_ALERT, sizeof(PATTERN_ALERT) / sizeof(int));
    buzzed = true;
  }
}

// Returns the address of the first I2C device found, or 0. Tries both
// pin orientations so a swapped SDA/SCL shows up here, not as a blank screen.
uint8_t scanI2C() {
  Serial.println("--- I2C scan ---");
  for (int attempt = 0; attempt < 2; attempt++) {
    int sda = attempt ? 22 : 21;
    int scl = attempt ? 21 : 22;
    Wire.begin(sda, scl);
    delay(50);
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.printf("  FOUND 0x%02X on SDA=%d SCL=%d\n", addr, sda, scl);
        return addr;
      }
    }
  }
  Serial.println("  nothing found - reseat wires, I will keep scanning");
  Wire.begin(21, 22);
  return 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  pinMode(BUZZER_N, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(BUZZER_N, LOW);
  playPattern(PATTERN_NEW, sizeof(PATTERN_NEW) / sizeof(int));  // boot check

  uint8_t found = scanI2C();

  if (found) {
    oledReady = oled.begin(SSD1306_SWITCHCAPVCC, found);
    Serial.printf("OLED init %s\n", oledReady ? "OK" : "FAILED");
  }
  showMessage("Connecting WiFi...");

  Serial.println("\n--- WiFi scan ---");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    Serial.printf("  [%s]  rssi=%d  ch=%d  enc=%d\n",
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.channel(i), WiFi.encryptionType(i));
  }
  Serial.printf("--- connecting to [%s] ---\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    // 1 = SSID not found, 4 = wrong password, 6 = disconnected/retrying
    Serial.printf("status=%d\n", WiFi.status());
    if (++tries > 20) {
      Serial.println("giving up - restarting");
      ESP.restart();
    }
  }
  Serial.printf("\nWiFi OK, IP %s\n", WiFi.localIP().toString().c_str());

  showMessage("Syncing time...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // 0,0 = UTC
  while (time(nullptr) < 100000) delay(500);

  showMessage("Fetching events...");
  if (!fetchNextEvent()) showMessage("No data yet...");
  lastPoll = millis();
}

void loop() {
  unsigned long wait = haveData ? POLL_MS : RETRY_MS;
  if (millis() - lastPoll > wait) {
    lastPoll = millis();
    fetchNextEvent();
  }

  if (!oledReady) {                // keep looking so you can reseat wires live
    uint8_t a = scanI2C();
    if (a) {
      oledReady = oled.begin(SSD1306_SWITCHCAPVCC, a);
      Serial.printf("OLED init %s\n", oledReady ? "OK" : "FAILED");
    }
  }

  if (newEvent) {                  // a new event took the top slot
    newEvent = false;
    playPattern(PATTERN_NEW, sizeof(PATTERN_NEW) / sizeof(int));
  }

  if (haveData) {
    long secs = (long)(eventTime - time(nullptr));
    if (secs <= 0) {               // event passed - refresh on the next loop
      haveData = false;
      lastPoll = millis() - RETRY_MS;
    } else {
      if (oledReady) drawScreen(secs);
      alertIfDue(secs);
    }
  }
  delay(1000);
}
