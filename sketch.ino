/**
 * Freestyle libre glucose tracker for
 * ESP32-C3 OLED development board with 0.42 inch OLED
 * 
*/


#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <Hash.h>
#include <SHA256.h>

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

/* 
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RESET, OLED_SCL, OLED_SDA);
int xOffset = 30;
int yOffset = 24;
*/

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, OLED_RESET, OLED_SCL, OLED_SDA);
int xOffset = 30;
int yOffset = 0;
int width = 72;
int height = 40;

const char *arrows[] = {
  "⇓⇓",
  "⇓",
  "⇘",
  "→",
  "⇗",
  "⇑",
  "⇑⇑"
};


int c = 0;


WiFiClientSecure client;
HTTPClient https;

// Authenticated user data
String userId;
String userIdHash;
String authToken;

// Glucose measurement data
float value;
int trendArrow;
int minutesAgo;
String measurementTimestamp;

//
String failed = "";

String sha256Hash(const String &inputString) {
  SHA256 sha256;
  uint8_t hash[SHA256_SIZE];

  sha256.reset();
  sha256.update(inputString.c_str(), inputString.length());
  sha256.finalize(hash, sizeof(hash));

  String hashString = "";
  for (int i = 0; i < SHA256_SIZE; i++) {
    // Convert each byte to a two-digit hexadecimal string
    if (hash[i] < 16) {
      hashString += "0";  // Add leading zero for single-digit hex
    }
    hashString += String(hash[i], HEX);
  }
  return hashString;
}

void handle_oled() {

  if (failed.length()) {
    u8g2.clearBuffer();
    u8g2.setFontPosTop();
    u8g2.setFont(u8g2_font_6x12_t_symbols);
    u8g2.println(failed);
    return;
  }

  u8g2.clearBuffer();
  u8g2.setFontPosTop();

  // write main reading
  u8g2.setFont(u8g2_font_fub25_tr);
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%2.1f", value);
  u8g2.drawStr(xOffset + 0, yOffset, buffer);

  // draw direction arrow aligned to bottom left
  u8g2.setFontPosBottom();
  // u8g2.setFont(u8g2_font_6x12_t_symbols);
  u8g2.setFont(u8g2_font_9x15_t_symbols);
  u8g2.drawUTF8(xOffset + 0, height, arrows[trendArrow]);

  // write timestamp aligned to bottom right
  u8g2.setFontPosBottom();
  u8g2.setFont(u8g2_font_6x12_t_symbols);
  snprintf(buffer, sizeof(buffer), "%2d", minutesAgo);
  u8g2.drawStr(xOffset + width - u8g2.getStrWidth(buffer) - 5, height, buffer);

  // write to display
  u8g2.sendBuffer();
}


const char *wl_status_to_string(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_STOPPED: return "WL_STOPPED";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
  }
  return "UNKNOWN";
}


void setup_wifi() {
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();

  // Keep trying for 10 seconds
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    failed = "Failed to connecect to WiFi " + String(wl_status_to_string(WiFi.status()));
    return;
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}


void libre_authenticate() {
  if (WiFi.status() != WL_CONNECTED) {
    failed = "Failed to connecect to WiFi " + String(wl_status_to_string(WiFi.status()));
    return;
  }

  Serial.println("Sending POST request..." + String(libre_login_url));

  if (!https.begin(client, libre_login_url)) {
    Serial.println("Unable to connect to libre server: " + String(libre_login_url));
    failed = "Unable to connect to libre server";
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("product", libre_product);
  https.addHeader("version", libre_version);

  // Build JSON body
  String json = String("{\"email\":\"") + libre_email + "\",\"password\":\"" + libre_password + "\"}";
  int httpCode = https.POST(json);
  if (httpCode != 200) {
    failed = "POST failed, error: " + https.errorToString(httpCode);
    return;
  }

  Serial.printf("HTTP code: %d\n", httpCode);
  String payload = https.getString();
  Serial.println("Response:");
  Serial.println(payload);

  // Parse JSON
  JsonDocument doc;  // adjust if response is larger
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    failed = "JSON parse error on " + String(libre_login_url);
    return;
  }

  // Extract required fields
  userId = doc["data"]["user"]["id"].as<String>();
  authToken = doc["data"]["authTicket"]["token"].as<String>();
  userIdHash = sha256Hash(userId);

  Serial.println("Extracted values:");
  Serial.print("User ID: ");
  Serial.println(userId);
  Serial.print("Auth Token: ");
  Serial.println(authToken);
  Serial.print("userIdHash: ");
  Serial.println(userIdHash);

  https.end();
}


void libre_get_measurement() {
  if (WiFi.status() != WL_CONNECTED) {
    failed = "Failed to connecect to WiFi " + String(wl_status_to_string(WiFi.status()));
    return;
  }

  String graph_url = "https://api-eu.libreview.io/llu/connections/" + userId + "/graph";

  Serial.println("Sending POST request to" + graph_url);
  if (!https.begin(client, graph_url)) {
    Serial.println("Unable to connect to libre server: " + graph_url);
    failed = "Unable to connect to libre server: " + graph_url;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", "Bearer " + authToken);
  https.addHeader("product", libre_product);
  https.addHeader("version", libre_version);
  https.addHeader("account-id", userIdHash);

  // Build JSON body
  int httpCode = https.GET();
  Serial.printf("HTTP code: %d\n", httpCode);

  if (httpCode != 200) {
    Serial.println("GET failed, error: " + https.errorToString(httpCode)) + " " + https.getString();
    ;
    failed = "GET failed, error: " + https.errorToString(httpCode) + " " + https.getString();
    ;
    return;
  }

  String payload = https.getString();
  Serial.println("Response:");
  Serial.println(payload);

  // Parse JSON
  JsonDocument doc;  // adjust if response is larger
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.println("JSON parse error on " + String(libre_login_url));
    failed = "JSON parse error on " + String(libre_login_url);
    return;
  }

  // Extract required fields
  value = doc["data"]["connection"]["glucoseMeasurement"]["Value"].as<float>();
  trendArrow = doc["data"]["connection"]["glucoseMeasurement"]["TrendArrow"].as<int>();
  measurementTimestamp = doc["data"]["connection"]["glucoseMeasurement"]["Timestamp"].as<String>();

  Serial.println("Extracted values:");
  Serial.print("value: ");
  Serial.println(value);
  Serial.print("trendArrow: ");
  Serial.println(trendArrow);
  Serial.print("measurementTimestamp: ");
  Serial.println(measurementTimestamp);

  https.end();
}


void setup(void) {
  Serial.begin(115200);
  u8g2.begin();
  u8g2.setContrast(255);     // set contrast to maximum
  u8g2.setBusClock(400000);  //400kHz I2C

  client.setInsecure();  // skip certificate check (quick test; not secure for prod)

  failed = "Loading";
  handle_oled();
  failed = "";

  setup_wifi();
  libre_authenticate();
  if (failed.length()){
    Serial.println("Failed: " + failed);
  }
}

void loop(void) {

  // if (check_wifi_connection()){
  //   handle_wifi_disconnected();
  //   delay(10000);
  //   return;
  // }


  libre_get_measurement();
  if (failed.length()){
    Serial.println("Failed: " + failed);
  }

  handle_oled();
  for (int j = 0; j < 20; j++) {
    Serial.print(".");
    delay(3010);
  }
  Serial.println("");
}
