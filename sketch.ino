/**
 * Freestyle Libre glucose tracker for
 * ESP32-C3 OLED development board with 0.42 inch OLED
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <Hash.h>
#include <SHA256.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <time.h>
#include <ctype.h>

#define SHA256_SIZE 32

// OLED
#define OLED_RESET U8X8_PIN_NONE  // Reset pin
#define OLED_SDA 5
#define OLED_SCL 6

// WiFi credentials
const char *ssid = "YOUR-WIFI-NAME";         // Change this to your WiFi SSID
const char *password = "YOUR-WIFI-PASS";  // Change this to your WiFi password


// API credentials
const char *libre_email = "example@email.com";
const char *libre_password = "XXXXXXXXXXXX";
const char *libre_login_url = "https://api-eu.libreview.io/llu/auth/login";
const char *libre_graph_url = "https://api-eu.libreview.io/llu/connections/%s/graph";
const char *libre_product = "llu.android";
const char *libre_version = "4.15.0";

// OLED configuration
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, OLED_RESET, OLED_SCL, OLED_SDA);
int xOffset = 30;
int yOffset = 0;
int width = 72;
int height = 40;

const char *arrows[] = {
  "⇓⇓", "⇓", "⇘", "→", "⇗", "⇑", "⇑⇑"
};

// Networking
WiFiClientSecure client;
HTTPClient https;
WiFiUDP ntpUDP;

// Create an NTPClient object
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7200, 60000);  // UTC+2

// Authenticated user data
String userId;
String userIdHash;
String authToken;

// Glucose measurement data
float value = 0.0;
int trendArrow = 3;  // default: →
int secondsAgo = 0;
String measurementTimestamp;
long measurementEpochLocal = 0;  // parsed local-epoch from measurementTimestamp

// Failure state
String failed = "";

// Timers
long lastApiCallMillis = -60000;  // for Libre GET
long lastOledUpdateMillis = 0;    // for OLED refresh


// --------------------------------------------------
// Utility Functions
// --------------------------------------------------

String sha256Hash(const String &input) {
  SHA256 sha256;
  uint8_t hash[SHA256_SIZE];

  sha256.reset();
  sha256.update(input.c_str(), input.length());
  sha256.finalize(hash, sizeof(hash));

  String result;
  for (int i = 0; i < SHA256_SIZE; i++) {
    if (hash[i] < 16) result += "0";
    result += String(hash[i], HEX);
  }
  return result;
}

const char *wl_status_to_string(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    default: return "UNKNOWN";
  }
}

void printJsonError(const DeserializationError &error, const String &context) {
  failed = "JSON parse error: " + context;
  Serial.println(failed);
  Serial.println(error.c_str());
}
/**
 * Parse Libre timestamp "M/D/YYYY hh:mm:ss AM/PM" and return
 * a *local-epoch* seconds value compatible with timeClient.getEpochTime()
 * (which already includes the UTC offset you configured).
 *
 */
long parseLibreTimestampLocalEpoch(const String &ts) {
  int month = 0, day = 0, year = 0, hour = 0, minute = 0, second = 0;
  char ampm[3] = { 0 };

  // Example: "9/6/2025 10:45:21 PM"
  int matched = sscanf(ts.c_str(), "%d/%d/%d %d:%d:%d %2s",
                       &month, &day, &year, &hour, &minute, &second, ampm);
  if (matched != 7) {
    Serial.println("Failed to parse timestamp: " + ts);
    return 0;
  }

  Serial.println("Matched all!");

  // Normalize AM/PM
  char A0 = toupper((unsigned char)ampm[0]);
  if (A0 == 'P' && hour < 12) hour += 12;
  if (A0 == 'A' && hour == 12) hour = 0;


  // Calculate the number of days in each month
  int daysInMonth[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

  // Check for leap year
  if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
    daysInMonth[2] = 29;
  }

  // Calculate total days from year 1970 to the given year
  long totalDays = 0;
  for (int y = 1970; y < year; y++) {
    totalDays += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
  }

  // Add days for the months in the current year
  for (int m = 1; m < month; m++) {
    totalDays += daysInMonth[m];
  }

  // Add the days in the current month
  totalDays += (day - 1);

  // Calculate total seconds
  long totalSeconds = totalDays * 86400 + hour * 3600 + minute * 60 + second;

  return totalSeconds;
}

// --------------------------------------------------
// OLED Rendering
// --------------------------------------------------

void handle_oled() {
  u8g2.clearBuffer();

  if (failed.length()) {
    u8g2.setFont(u8g2_font_6x12_t_symbols);
    u8g2.setFontPosTop();
    u8g2.println(failed);
    u8g2.sendBuffer();
    return;
  }

  // Main glucose reading
  u8g2.setFontPosTop();
  u8g2.setFont(u8g2_font_fub25_tr);
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%2.1f", value);
  u8g2.drawStr(xOffset, yOffset, buffer);

  // Trend arrow (bottom-left)
  u8g2.setFont(u8g2_font_9x15_t_symbols);
  u8g2.setFontPosBottom();
  u8g2.drawUTF8(xOffset, height, arrows[trendArrow]);

  // Minutes ago (bottom-right)
  snprintf(buffer, sizeof(buffer), "%2d", secondsAgo);
  u8g2.setFont(u8g2_font_6x12_t_symbols);
  u8g2.setFontPosBottom();
  u8g2.drawStr(xOffset + width - u8g2.getStrWidth(buffer) - 5, height, buffer);

  u8g2.sendBuffer();
}

// --------------------------------------------------
// WiFi / API Communication
// --------------------------------------------------

void setup_wifi() {
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    failed = "Failed to connect WiFi: " + String(wl_status_to_string(WiFi.status()));
    return;
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void libre_authenticate() {
  if (WiFi.status() != WL_CONNECTED) {
    failed = "No WiFi: " + String(wl_status_to_string(WiFi.status()));
    return;
  }

  Serial.println("Sending POST to " + String(libre_login_url));
  if (!https.begin(client, libre_login_url)) {
    failed = "Unable to connect: " + String(libre_login_url);
    return;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("product", libre_product);
  https.addHeader("version", libre_version);

  String body = String("{\"email\":\"") + libre_email + "\",\"password\":\"" + libre_password + "\"}";
  int httpCode = https.POST(body);

  if (httpCode != 200) {
    failed = "Login failed: " + httpCode + https.errorToString(httpCode);
    https.end();
    return;
  }

  String payload = https.getString();
  https.end();

  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) return printJsonError(error, "login");

  userId = doc["data"]["user"]["id"].as<String>();
  authToken = doc["data"]["authTicket"]["token"].as<String>();
  userIdHash = sha256Hash(userId);

  Serial.println("Authenticated:");
  Serial.println("  User ID: " + userId);
  Serial.println("  Auth Token: " + authToken);
  Serial.println("  userIdHash: " + userIdHash);
}

void libre_get_measurement() {
  if (WiFi.status() != WL_CONNECTED) {
    failed = "No WiFi: " + String(wl_status_to_string(WiFi.status()));
    return;
  }

  String graph_url = "https://api-eu.libreview.io/llu/connections/" + userId + "/graph";
  Serial.println("Sending GET to " + graph_url);

  if (!https.begin(client, graph_url)) {
    failed = "Unable to connect: " + graph_url;
    return;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", "Bearer " + authToken);
  https.addHeader("product", libre_product);
  https.addHeader("version", libre_version);
  https.addHeader("account-id", userIdHash);

  int httpCode = https.GET();
  if (httpCode != 200) {
    failed = "GET failed: " + https.errorToString(httpCode);
    https.end();
    return;
  }

  String payload = https.getString();
  https.end();

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) return printJsonError(error, "graph");

  value = doc["data"]["connection"]["glucoseMeasurement"]["Value"].as<float>();
  trendArrow = doc["data"]["connection"]["glucoseMeasurement"]["TrendArrow"].as<int>();
  measurementTimestamp = doc["data"]["connection"]["glucoseMeasurement"]["Timestamp"].as<String>();
  measurementEpochLocal = parseLibreTimestampLocalEpoch(measurementTimestamp);

  Serial.println("Measurement:");
  Serial.println("  Value: " + String(value));
  Serial.println("  TrendArrow: " + String(trendArrow));
  Serial.println("  Timestamp: " + String(measurementTimestamp));
  Serial.println("  EpochLocal: " + String(measurementEpochLocal));
}

// --------------------------------------------------
// Arduino Setup / Loop
// --------------------------------------------------

void setup() {
  Serial.begin(115200);
  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setBusClock(400000);

  client.setInsecure();  // ⚠️ Not secure, skip cert validation

  failed = "Loading";
  handle_oled();
  failed = "";

  setup_wifi();
  timeClient.begin();
  libre_authenticate();

  if (failed.length()) Serial.println("Failed: " + failed);
}

void loop() {
  timeClient.update();

  unsigned long now = millis();

  // Update OLED every 1 second
  if (now - lastOledUpdateMillis >= 1000) {
    Serial.println(timeClient.getFormattedTime());

    // Recompute secondsAgo each second from NTP local-epoch
    time_t nowLocalEpoch = timeClient.getEpochTime();
    secondsAgo = (nowLocalEpoch - measurementEpochLocal);
    Serial.println("secondsAgo: " + String(secondsAgo));

    lastOledUpdateMillis = now;
    handle_oled();
  }

  // Call Libre API every 60 seconds
  if (now - lastApiCallMillis >= 60000) {
    libre_get_measurement();
    if (failed.length()) {
      Serial.println("Failed: " + failed);
      sleep(1000);
    }
    lastApiCallMillis = now;
    failed = "";
  }
}
