/*
  WebUI Hotspot Example for Quectel EC200U

  This example turns the ESP32 into a standalone Wi-Fi Hotspot with a beautiful
  Web UI to control the Quectel EC200U modem. Features:
  - Dashboard with Signal, Operator, and Network status
  - SMS Send/Receive
  - GPS Location
  - Voice Call Dialer
  - AT Command Terminal
  - TCP Test Tool

  Hardware:
  - ESP32 Board
  - Quectel EC200U Module connected via UART

  Dependencies:
  - ArduinoJson
  - QuectelEC200U
*/
#include "index_html.h"
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <QuectelEC200U.h>
#include <time.h>
// --- Configuration ---
const char *ap_ssid = "Quectel_Manager";
const char *ap_password = "password";
// --- 认证配置 ---
const char* ADMIN_USER = "admin";
const char* ADMIN_PASS = "admin123";
const char* AUTH_COOKIE_NAME = "ESPSESSIONID";

const char* ntpServer = "ntp.aliyun.com"; // 阿里云 NTP 服务器
const long  gmtOffset_sec = 8 * 3600;     // 东八区偏移 (8小时 * 3600秒)
const int   daylightOffset_sec = 0;      // 夏令时偏移

// Modem Pins (Adjust for your board)
#define RX_PIN 04
#define TX_PIN 05
#define POWER_PIN 10 // Optional power pin
// ========== 新增：状态检测引脚 ==========
#define MODEM_STATUS_PIN 12 // 请根据实际硬件连接修改为正确的GPIO引脚

#if defined(ESP32)
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>


// Web Server on port 80
WebServer server(80);

// Initialize Modem
HardwareSerial modemSerial(1);
QuectelEC200U modem(modemSerial, 115200, RX_PIN, TX_PIN);

#elif defined(ESP8266)
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <Preferences.h> // <-- 新增这一行，引入真正的库

// ========== 温度传感器与运行时间 ==========
#if defined(ESP32)
#include <driver/temp_sensor.h>
#endif

// Web Server on port 80
ESP8266WebServer server(80);

// Initialize Modem
SoftwareSerial modemSerial(RX_PIN, TX_PIN);
QuectelEC200U modem(modemSerial);

#endif

Preferences preferences;
DNSServer dnsServer;

// Global variables
int currentSocketId = -1;

// 这些变量必须定义在所有函数之前（全局）
float current_temperature = -127.0f;
bool temp_sensor_initialized = false;
unsigned long last_temp_read = 0;
const unsigned long TEMP_READ_INTERVAL = 5000;
unsigned long boot_time_ms = 0;

struct ApnProfile {
  const char *keyword;
  const char *apn;
  const char *user;
  const char *pass;
  int auth;
};

static const ApnProfile APN_PROFILES[] = {
    {"jio", "jionet", "", "", 0},
    {"reliance", "jionet", "", "", 0},
    {"airtel", "airtelgprs.com", "", "", 0},
    {"vodafone", "www", "", "", 0},
    {"idea", "www", "", "", 0},
    {" vi", "www", "", "", 0},
    {"bsnl", "bsnlnet", "", "", 0},
    {"docomo", "tatadocomo.com", "", "", 0},
    {"mtnl", "mtnl.net", "", "", 0},
    {"telstra", "telstra.internet", "", "", 0},
    {"t-mobile", "fast.t-mobile.com", "", "", 0},
    {"att", "phone", "", "", 0},
    {"rogers", "internet.com", "", "", 0}};

struct ApnDetection {
  bool found;
  String apn;
  String user;
  String pass;
  int auth;
  String keyword;
};

struct ApnStored {
  String apn;
  String user;
  String pass;
  int auth;
  bool hasCustom;
};

struct ApnSelection {
  String apn;
  String user;
  String pass;
  int auth;
  String source;
  String operatorName;
  ApnDetection detected;
};

ApnStored storedApn;

bool mqttConnected = false;
String mqttServer = "";
int mqttPort = 1883;
int mqttCtxId = 1;
unsigned long mqttLastActivity = 0;
String mqttLastError = "";
String mqttLastTopic = "";

struct BatteryInfo {
  bool valid;
  int status;
  int percent;
  int millivolts;
  String raw;
};

// --- Helper Functions ---

void loadApnPreferences();
void saveApnPreferences(const String &apn, const String &user,
                        const String &pass, int auth);
void clearApnPreferences();
void sendCorsHeaders();
ApnDetection detectApnProfile(const String &operatorName);
ApnSelection getApnSelection(const String &operatorHint = String());
bool configureContextWithApn(int ctxId, const String &apn, const String &user,
                             const String &pass, int auth);
BatteryInfo parseBatteryInfo(const String &raw);
void appendCallEntries(const String &raw, JsonArray &entries);

int speakerVolumeLevel = 60;
int ringerVolumeLevel = 60;

bool isAuthorized() {
    if (server.hasHeader("Cookie")) {
        String cookie = server.header("Cookie");
        if (cookie.indexOf(String(AUTH_COOKIE_NAME) + "=valid_session_token") != -1) {
            return true;
        }
    }
    return false;
}

void bindProtected(const char* uri, HTTPMethod method, std::function<void()> handler) {
    server.on(uri, method, [handler]() {
        sendCorsHeaders();
        if (server.method() == HTTP_OPTIONS) {
            server.send(204);
            return;
        }
        if (!isAuthorized()) {
            server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        handler();
    });
}

void handleLogin() {
    sendCorsHeaders();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }

    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    
    if (doc["user"] == ADMIN_USER && doc["pass"] == ADMIN_PASS) {
        server.sendHeader("Set-Cookie", String(AUTH_COOKIE_NAME) + "=valid_session_token; Path=/; HttpOnly; Max-Age=300");
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(401, "application/json", "{\"success\":false}");
    }
}

void handleLogout() {
    sendCorsHeaders();
    server.sendHeader("Set-Cookie", String(AUTH_COOKIE_NAME) + "=; Path=/; Max-Age=0");
    server.send(200, "application/json", "{\"success\":true}");
}
// ========================================================

void handleGetTime() {
    sendCorsHeaders();
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        server.send(200, "application/json", "{\"success\":false, \"time\":\"Time Not Set\"}");
        return;
    }
    
    char timeString[20];
    // 格式化为: 2026-04-05 08:30:05
    strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    String response = "{\"success\":true, \"time\":\"" + String(timeString) + "\"}";
    server.send(200, "application/json", response);
}

void loadApnPreferences() {
  preferences.begin("cellular", true);
  storedApn.apn = preferences.getString("apn", "");
  storedApn.user = preferences.getString("user", "");
  storedApn.pass = preferences.getString("pass", "");
  storedApn.auth = preferences.getInt("auth", 0);
  preferences.end();
  storedApn.hasCustom = storedApn.apn.length() > 0;
}

void saveApnPreferences(const String &apn, const String &user,
                        const String &pass, int auth) {
  preferences.begin("cellular", false);
  preferences.putString("apn", apn);
  preferences.putString("user", user);
  preferences.putString("pass", pass);
  preferences.putInt("auth", auth);
  preferences.end();
  storedApn.apn = apn;
  storedApn.user = user;
  storedApn.pass = pass;
  storedApn.auth = auth;
  storedApn.hasCustom = apn.length() > 0;
}

void clearApnPreferences() {
  preferences.begin("cellular", false);
  preferences.clear();
  preferences.end();
  storedApn.apn = "";
  storedApn.user = "";
  storedApn.pass = "";
  storedApn.auth = 0;
  storedApn.hasCustom = false;
}

ApnDetection detectApnProfile(const String &operatorName) {
  ApnDetection detection;
  detection.found = false;
  detection.apn = "";
  detection.user = "";
  detection.pass = "";
  detection.auth = 0;
  detection.keyword = "";

  String lower = operatorName;
  lower.toLowerCase();
  for (const auto &profile : APN_PROFILES) {
    String keyword(profile.keyword);
    keyword.toLowerCase();
    if (keyword.length() == 0)
      continue;
    if (lower.indexOf(keyword) != -1) {
      detection.found = true;
      detection.apn = profile.apn;
      detection.user = profile.user;
      detection.pass = profile.pass;
      detection.auth = profile.auth;
      detection.keyword = keyword;
      break;
    }
  }

  return detection;
}

ApnSelection getApnSelection(const String &operatorHint) {
  ApnSelection selection;
  String operatorName = operatorHint;
  if (operatorName.length() == 0) {
    operatorName = modem.getOperator();
  }
  selection.operatorName = operatorName;
  selection.detected = detectApnProfile(operatorName);

  if (storedApn.hasCustom) {
    selection.apn = storedApn.apn;
    selection.user = storedApn.user;
    selection.pass = storedApn.pass;
    selection.auth = storedApn.auth;
    selection.source = "custom";
  } else if (selection.detected.found) {
    selection.apn = selection.detected.apn;
    selection.user = selection.detected.user;
    selection.pass = selection.detected.pass;
    selection.auth = selection.detected.auth;
    selection.source = "auto";
  } else {
    selection.apn = "internet";
    selection.user = "";
    selection.pass = "";
    selection.auth = 0;
    selection.source = "default";
  }

  return selection;
}

bool configureContextWithApn(int ctxId, const String &apn, const String &user,
                             const String &pass, int auth) {
  bool ctxReady = modem.configureContext(ctxId, 1, apn, user, pass, auth);
  bool pdpReady = ctxReady && modem.activatePDP(ctxId);
  return pdpReady;
}

BatteryInfo parseBatteryInfo(const String &raw) {
  BatteryInfo info;
  info.valid = false;
  info.status = -1;
  info.percent = -1;
  info.millivolts = -1;
  info.raw = raw;

  int tag = raw.indexOf(F("+CBC:"));
  if (tag == -1) {
    return info;
  }

  int colon = raw.indexOf(':', tag);
  int lineStart = (colon == -1) ? tag + 5 : colon + 1;
  int lineEnd = raw.indexOf('\n', lineStart);
  if (lineEnd == -1)
    lineEnd = raw.length();

  String line = raw.substring(lineStart, lineEnd);
  line.replace("\r", "");
  line.trim();

  int firstComma = line.indexOf(',');
  int secondComma = line.indexOf(',', firstComma + 1);
  if (firstComma == -1 || secondComma == -1) {
    return info;
  }

  info.status = line.substring(0, firstComma).toInt();
  info.percent = line.substring(firstComma + 1, secondComma).toInt();
  String voltageStr = line.substring(secondComma + 1);
  voltageStr.trim();
  info.millivolts = voltageStr.toInt();
  info.valid = true;
  return info;
}

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  sendCorsHeaders();
  server.send(204);
}

void handleRoot() {
  Serial.println(F("HTTP GET / (serving UI)"));
  // Serve large HTML directly from PROGMEM to avoid heap fragmentation
  server.send_P(200, "text/html", index_html);
}

void handleNotFound() {
  Serial.print(F("NotFound URI: "));
  Serial.println(server.uri());
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

static const uint8_t CSQ_PERCENT_LOOKUP[31] = {
    0,  3,  6,  9,  12, 15, 18, 21, 24, 28, 32, 36, 40, 44, 48, 52,
    56, 60, 64, 68, 72, 76, 80, 84, 88, 91, 94, 96, 98, 99, 100};

static int csqToPercent(int csq) {
  if (csq <= 0 || csq == 99)
    return 0;
  if (csq >= 31)
    return 100;
  return CSQ_PERCENT_LOOKUP[csq];
}

static String csqToText(int csq) {
  if (csq < 0 || csq == 99)
    return F("No signal");
  if (csq == 0)
    return F("< -113 dBm");
  if (csq == 1)
    return F("-111 dBm");
  if (csq == 31)
    return F("> -51 dBm");
  return String(-113 + (csq * 2)) + F(" dBm");
}

static String extractRatDetail(const String &qInfo) {
  int firstQuote = qInfo.indexOf('"');
  if (firstQuote == -1)
    return "";
  int secondQuote = qInfo.indexOf('"', firstQuote + 1);
  if (secondQuote == -1)
    return "";
  String detail = qInfo.substring(firstQuote + 1, secondQuote);
  detail.trim();
  return detail;
}

static String simplifyRatLabel(const String &detail) {
  if (detail.length() == 0)
    return "";
  String lower = detail;
  lower.toLowerCase();
  if (lower.indexOf("nr") != -1)
    return F("5G NR");
  if (lower.indexOf("lte") != -1)
    return F("4G LTE");
  if (lower.indexOf("wcdma") != -1 || lower.indexOf("umts") != -1 ||
      lower.indexOf("hspa") != -1)
    return F("3G");
  if (lower.indexOf("cdma") != -1 || lower.indexOf("td-scdma") != -1)
    return F("3G CDMA");
  if (lower.indexOf("edge") != -1 || lower.indexOf("gprs") != -1 ||
      lower.indexOf("gsm") != -1)
    return F("2G");
  return detail;
}

static void appendWifiEntries(const String &raw, JsonArray &entries) {
  const String tag = F("+QWIFISCAN:");
  int idx = 0;
  while ((idx = raw.indexOf(tag, idx)) != -1) {
    int end = raw.indexOf('\n', idx);
    if (end == -1)
      end = raw.length();
    String line = raw.substring(idx + tag.length(), end);
    line.replace("\r", "");
    line.trim();
    if (line.length() == 0) {
      idx = end;
      continue;
    }

    JsonObject entry = entries.createNestedObject();
    entry["raw"] = line;

    int firstQuote = line.indexOf('"');
    int secondQuote = line.indexOf('"', firstQuote + 1);
    if (firstQuote != -1 && secondQuote != -1 && secondQuote > firstQuote) {
      entry["ssid"] = line.substring(firstQuote + 1, secondQuote);
    }

    int thirdQuote = line.indexOf('"', secondQuote + 1);
    int fourthQuote = line.indexOf('"', thirdQuote + 1);
    if (thirdQuote != -1 && fourthQuote != -1 && fourthQuote > thirdQuote) {
      entry["bssid"] = line.substring(thirdQuote + 1, fourthQuote);
    }

    int cursor = fourthQuote != -1 ? fourthQuote + 1
                                   : (secondQuote != -1 ? secondQuote + 1 : 0);
    while (cursor < (int)line.length() &&
           (line[cursor] == ',' || line[cursor] == ' ')) {
      cursor++;
    }

    String remainder = line.substring(cursor);
    remainder.trim();
    int comma = remainder.indexOf(',');
    if (comma != -1) {
      entry["channel"] = remainder.substring(0, comma).toInt();
      remainder = remainder.substring(comma + 1);
      remainder.trim();
      comma = remainder.indexOf(',');
      if (comma != -1) {
        entry["rssi"] = remainder.substring(0, comma).toInt();
        remainder = remainder.substring(comma + 1);
        remainder.trim();
        if (remainder.length() > 0) {
          entry["auth"] = remainder;
        }
      } else if (remainder.length() > 0) {
        entry["rssi"] = remainder.toInt();
      }
    }

    idx = end;
  }
}

static void appendBluetoothEntries(const String &raw, JsonArray &entries) {
  const String tag = F("+QBTSCAN:");
  int idx = 0;
  while ((idx = raw.indexOf(tag, idx)) != -1) {
    int end = raw.indexOf('\n', idx);
    if (end == -1)
      end = raw.length();
    String line = raw.substring(idx + tag.length(), end);
    line.replace("\r", "");
    line.trim();
    if (line.length() == 0) {
      idx = end;
      continue;
    }

    JsonObject entry = entries.createNestedObject();
    entry["raw"] = line;

    int nameEnd = line.lastIndexOf('"');
    int nameStart = line.lastIndexOf('"', nameEnd - 1);
    if (nameStart != -1 && nameEnd != -1 && nameEnd > nameStart) {
      entry["name"] = line.substring(nameStart + 1, nameEnd);
    }

    int macStart = line.indexOf('"');
    int macEnd = line.indexOf('"', macStart + 1);
    if (macStart != -1 && macEnd != -1 && macEnd > macStart) {
      entry["mac"] = line.substring(macStart + 1, macEnd);
    } else {
      int colonIdx = line.indexOf(':');
      if (colonIdx > 1) {
        int start = colonIdx - 2;
        while (start > 0 && line[start - 1] != ',' && line[start - 1] != ' ')
          start--;
        int endIdx = colonIdx + 1;
        while (endIdx < (int)line.length() && line[endIdx] != ',' &&
               line[endIdx] != ' ')
          endIdx++;
        entry["mac"] = line.substring(start, endIdx);
      }
    }

    idx = end;
  }
}

static String callStateToText(int state) {
  switch (state) {
  case 0:
    return F("Active");
  case 1:
    return F("Held");
  case 2:
    return F("Dialing");
  case 3:
    return F("Alerting");
  case 4:
    return F("Incoming");
  case 5:
    return F("Waiting");
  default:
    return F("Unknown");
  }
}

static String callDirectionToText(int dir) {
  return (dir == 1) ? F("Incoming") : F("Outgoing");
}

static String callModeToText(int mode) {
  switch (mode) {
  case 0:
    return F("Voice");
  case 1:
    return F("Data");
  case 2:
    return F("Fax");
  case 3:
    return F("Voice (Alt)");
  case 4:
    return F("Video");
  default:
    return F("Unknown");
  }
}

void appendCallEntries(const String &raw, JsonArray &entries) {
  const String tag = F("+CLCC:");
  int idx = 0;
  while ((idx = raw.indexOf(tag, idx)) != -1) {
    int end = raw.indexOf('\n', idx);
    if (end == -1)
      end = raw.length();
    String line = raw.substring(idx + tag.length(), end);
    line.replace("\r", "");
    line.trim();
    if (line.length() == 0) {
      idx = end;
      continue;
    }

    int values[5] = {-1, -1, -1, -1, -1};
    int cursor = 0;
    for (int field = 0; field < 5; field++) {
      int comma = line.indexOf(',', cursor);
      String token;
      if (comma == -1) {
        token = line.substring(cursor);
        cursor = line.length();
      } else {
        token = line.substring(cursor, comma);
        cursor = comma + 1;
      }
      token.trim();
      values[field] = token.toInt();
    }

    String number = "";
    int type = -1;
    int quoteStart = line.indexOf('"', cursor);
    if (quoteStart != -1) {
      int quoteEnd = line.indexOf('"', quoteStart + 1);
      if (quoteEnd != -1) {
        number = line.substring(quoteStart + 1, quoteEnd);
        int typeComma = line.indexOf(',', quoteEnd + 1);
        if (typeComma != -1) {
          String typeToken = line.substring(typeComma + 1);
          typeToken.trim();
          type = typeToken.toInt();
        }
      }
    }

    JsonObject entry = entries.createNestedObject();
    entry["index"] = values[0];
    entry["direction_code"] = values[1];
    entry["state_code"] = values[2];
    entry["mode_code"] = values[3];
    entry["multiparty"] = values[4] == 1;
    entry["direction"] = callDirectionToText(values[1]);
    entry["state"] = callStateToText(values[2]);
    entry["mode"] = callModeToText(values[3]);
    entry["number"] = number;
    entry["type"] = type;
    entry["raw"] = line;

    idx = end;
  }
}

void handleStatus() {
  sendCorsHeaders();
  JsonDocument doc;

  int csq = modem.getSignalStrength();
  int reg = modem.getRegistrationStatus();
  String operatorName = modem.getOperator();
  ApnSelection apnSelection = getApnSelection(operatorName);
  String networkInfo = modem.getNetworkInfo();
  String ratDetail = extractRatDetail(networkInfo);
  String ratLabel = simplifyRatLabel(ratDetail);
  String registration = (reg == 1 || reg == 5) ? "Registered" : "Searching";

  doc["signal"] = csqToPercent(csq);
  doc["signal_csq"] = csq;
  doc["signal_text"] = csqToText(csq);
  doc["operator"] = operatorName;
  doc["net_type"] = ratLabel.length() ? ratLabel : registration;
  doc["registration"] = registration;
  doc["sim_status"] = modem.getSIMStatus();
  doc["imei"] = modem.getIMEI();
  doc["model"] = modem.getModelIdentification();
  doc["network_info"] = networkInfo;
  doc["rat_detail"] = ratDetail;

  JsonObject apnJson = doc["apn"].to<JsonObject>();
  apnJson["apn"] = apnSelection.apn;
  apnJson["user"] = apnSelection.user;
  apnJson["pass"] = apnSelection.pass;
  apnJson["auth"] = apnSelection.auth;
  apnJson["source"] = apnSelection.source;
  apnJson["operator"] = apnSelection.operatorName;
  apnJson["detected"] = apnSelection.detected.found;
  apnJson["keyword"] = apnSelection.detected.keyword;

  JsonObject mqttJson = doc["mqtt"].to<JsonObject>();
  mqttJson["connected"] = mqttConnected;
  mqttJson["server"] = mqttServer;
  mqttJson["port"] = mqttPort;
  mqttJson["last_topic"] = mqttLastTopic;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleModemInfo() {
  sendCorsHeaders();
  JsonDocument doc;
  doc["imei"] = modem.getIMEI();
  doc["manufacturer"] = modem.getManufacturerIdentification();
  doc["model"] = modem.getModelIdentification();
  doc["firmware"] = modem.getFirmwareRevision();
  doc["version"] = modem.getModuleVersion();

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleEspInfo() {
    sendCorsHeaders();
    JsonDocument doc;
    
#if defined(ESP32)
    doc["chip_model"] = "ESP32";
    doc["chip_revision"] = ESP.getChipRevision();
    doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
    doc["flash_size_mb"] = ESP.getFlashChipSize() / (1024 * 1024);
    doc["free_heap_kb"] = ESP.getFreeHeap() / 1024;
    doc["sketch_size_kb"] = ESP.getSketchSize() / 1024;
    doc["sketch_free_kb"] = ESP.getFreeSketchSpace() / 1024;
    doc["sdk_version"] = ESP.getSdkVersion();
#elif defined(ESP8266)
    doc["chip_model"] = "ESP8266";
    doc["chip_id"] = ESP.getChipId();
    doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
    doc["flash_size_mb"] = ESP.getFlashChipSize() / (1024 * 1024);
    doc["free_heap_kb"] = ESP.getFreeHeap() / 1024;
    doc["sketch_size_kb"] = ESP.getSketchSize() / 1024;
    doc["sketch_free_kb"] = ESP.getFreeSketchSpace() / 1024;
    doc["sdk_version"] = ESP.getSdkVersion();
#else
    doc["chip_model"] = "Unknown";
    doc["cpu_freq_mhz"] = 0;
    doc["flash_size_mb"] = 0;
    doc["free_heap_kb"] = 0;
    doc["sdk_version"] = "Unknown";
#endif

    // 网络信息
    doc["mac"] = WiFi.macAddress();
    IPAddress ip = WiFi.localIP();
    if (ip.toString() != "0.0.0.0") {
        doc["ip"] = ip.toString();
    } else {
        doc["ip"] = WiFi.softAPIP().toString();
    }
  // 当前 WiFi 连接信息（STA 模式或 AP 模式）
  if (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) {
    doc["current_wifi_ssid"] = WiFi.SSID();
    doc["current_wifi_rssi"] = WiFi.RSSI();
  } else if (WiFi.getMode() == WIFI_AP) {
    doc["current_wifi_ssid"] = String("AP Mode: ") + ap_ssid;
  } else {
    doc["current_wifi_ssid"] = "None";
  }
    // ========== 新增：温度传感器数据 ==========
    if (current_temperature > -100.0f) {
        doc["temperature_celsius"] = current_temperature;
        doc["temperature_fahrenheit"] = current_temperature * 9.0f / 5.0f + 32.0f;
    } else {
        doc["temperature_celsius"] = "N/A";
        doc["temperature_fahrenheit"] = "N/A";
    }

    // ========== 新增：运行时间（开机时间） ==========
    unsigned long uptime_seconds = (millis() - boot_time_ms) / 1000;
    doc["uptime_seconds"] = uptime_seconds;
    char uptime_str[20];
    unsigned long hours = uptime_seconds / 3600;
    unsigned long minutes = (uptime_seconds % 3600) / 60;
    unsigned long seconds = uptime_seconds % 60;
    snprintf(uptime_str, sizeof(uptime_str), "%02lu:%02lu:%02lu", hours, minutes, seconds);
    doc["uptime_formatted"] = uptime_str;

    // 可选：添加上次重启的绝对时间（需要NTP同步）
    // doc["boot_time_epoch"] = (uint32_t)(time(nullptr) - uptime_seconds);

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// ========== 新增：模块在线状态检测 API ==========
void handleModemStatus() {
  sendCorsHeaders();
  
  // 读取引脚电平：HIGH (1) = 在线, LOW (0) = 离线
  bool isOnline = (digitalRead(MODEM_STATUS_PIN) == HIGH);
  
  JsonDocument doc;
  doc["online"] = isOnline;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSmsSend() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));

  const char *number = doc["number"];
  const char *text = doc["text"];

  bool success = modem.sendSMS(number, text);

  JsonDocument res;
  res["success"] = success;
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleSmsRead() {
  sendCorsHeaders();
  int index = server.arg("index").toInt();

  String msg = modem.readSMS(index);

  JsonDocument res;
  res["success"] = msg.length() > 0;
  res["message"] = msg;

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleGpsLocation() {
  sendCorsHeaders();

  // Ensure GNSS is on
  if (!modem.isGNSSOn()) {
    modem.startGNSS();
  }

  QuectelEC200U::GNSSData data = modem.getGNSSData(5000);

  JsonDocument res;
  if (data.valid) {
    res["success"] = true;
    res["lat"] = data.lat;
    res["lon"] = data.lon;
    res["alt"] = data.altitude;
    res["time"] = data.utc_time;
    res["sats"] = data.nsat;
  } else {
    res["success"] = false;
  }

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleCallDial() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  const char *number = doc["number"];

  bool success = modem.dial(number);

  JsonDocument res;
  res["success"] = success;
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleCallHangup() {
  sendCorsHeaders();
  bool success = modem.hangup();

  JsonDocument res;
  res["success"] = success;
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleCallAnswer() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  bool success = modem.answer();
  JsonDocument res;
  res["success"] = success;
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleCallStatus() {
  sendCorsHeaders();
  String raw = modem.getCallList();

  JsonDocument res;
  res["success"] = true;
  res["raw"] = raw;
  JsonArray entries = res.createNestedArray("entries");
  appendCallEntries(raw, entries);
  res["active"] = entries.size() > 0;
  res["speaker_volume"] = speakerVolumeLevel;
  res["ringer_volume"] = ringerVolumeLevel;
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleCallVolume() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Invalid JSON\"}");
  }

  String target = doc["target"] | "speaker";
  target.toLowerCase();
  bool isRinger = target == "ringer";
  int delta = doc["delta"] | 0;
  bool hasLevel = doc.containsKey("level");
  int requestedLevel =
      doc["level"] | (isRinger ? ringerVolumeLevel : speakerVolumeLevel);

  int *store = isRinger ? &ringerVolumeLevel : &speakerVolumeLevel;
  if (hasLevel) {
    requestedLevel = constrain(requestedLevel, 0, 100);
    *store = requestedLevel;
  } else {
    *store = constrain(*store + delta, 0, 100);
  }

  bool success =
      isRinger ? modem.setRingerVolume(*store) : modem.setSpeakerVolume(*store);
  JsonDocument res;
  res["success"] = success;
  res["target"] = isRinger ? "ringer" : "speaker";
  res["level"] = *store;
  if (!success) {
    res["error"] = "Volume command failed";
  }

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleAT() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String cmd = doc["cmd"];

  modem.sendATRaw(cmd);
  String resp = modem.readResponse(5000); // Wait up to 5s

  JsonDocument res;
  res["response"] = resp;
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleTcpOpen() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Invalid JSON\"}");
  }

  String host = doc["host"] | "";
  int port = doc["port"] | 0;
  int ctxId = doc["ctxId"] | 1;
  int socketId = doc["socketId"] | 0;
  String operatorHint = doc["operator"] | "";
  ApnSelection selection = getApnSelection(operatorHint);

  String apn =
      doc.containsKey("apn") ? String((const char *)doc["apn"]) : selection.apn;
  String user = doc.containsKey("user") ? String((const char *)doc["user"])
                                        : selection.user;
  String pass = doc.containsKey("pass") ? String((const char *)doc["pass"])
                                        : selection.pass;
  int auth = doc.containsKey("auth") ? doc["auth"].as<int>() : selection.auth;

  JsonDocument res;

  if (host.length() == 0 || port <= 0) {
    res["success"] = false;
    res["error"] = "Host/port required";
  } else if (apn.length() == 0) {
    res["success"] = false;
    res["error"] = "APN missing";
  } else {
    bool ctxReady = modem.configureContext(ctxId, 1, apn, user, pass, auth);
    bool pdpReady = ctxReady && modem.activatePDP(ctxId);
    if (!ctxReady) {
      res["success"] = false;
      res["error"] = "Context config failed";
    } else if (!pdpReady) {
      res["success"] = false;
      res["error"] = "PDP activation failed";
    } else {
      int openedId = modem.tcpOpen(host, port, ctxId, socketId);
      currentSocketId = openedId;
      res["success"] = (openedId != -1);
      res["socketId"] = openedId;
      res["ctxId"] = ctxId;
      if (openedId == -1) {
        res["error"] = "Socket open failed";
      }
    }
  }
  JsonObject apnJson = res["apn"].to<JsonObject>();
  apnJson["apn"] = apn;
  apnJson["user"] = user;
  apnJson["pass"] = pass;
  apnJson["auth"] = auth;
  apnJson["source"] = doc.containsKey("apn") ? "request" : selection.source;
  apnJson["operator"] = selection.operatorName;

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleTcpClose() {
  sendCorsHeaders();
  bool success = false;
  bool hadSocket = currentSocketId != -1;
  if (hadSocket) {
    success = modem.tcpClose(currentSocketId);
    currentSocketId = -1;
  }

  JsonDocument res;
  res["success"] = success;
  if (!success) {
    res["error"] = hadSocket ? "Close failed" : "No open socket";
  }
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleTcpSend() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Invalid JSON\"}");
  }
  String data = doc["data"] | "";

  bool success = false;
  String tcpResponse = "";

  if (currentSocketId != -1) {
    success = modem.tcpSend(currentSocketId, data);
    if (success) {
      // Try to read response
      modem.tcpRecv(currentSocketId, tcpResponse, 1024, 3000);
    }
  }

  JsonDocument res;
  res["success"] = success;
  if (!success) {
    res["error"] = "No open socket or send failed";
  }
  String formatted = tcpResponse;
  formatted.replace("\r", "\n");
  res["response"] = formatted;
  res["raw"] = tcpResponse;

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleWifiSave() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String ssid = doc["ssid"];
  String pass = doc["pass"];

  if (ssid.length() > 0) {
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("pass", pass);
    preferences.end();

    server.send(200, "application/json",
                "{\"success\":true, \"message\":\"Saved. Rebooting...\"}");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "application/json",
                "{\"success\":false, \"message\":\"Invalid SSID\"}");
  }
}

void handleWifiForget() {
  sendCorsHeaders();
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  server.send(200, "application/json",
              "{\"success\":true, \"message\":\"Forgot. Rebooting...\"}");
  delay(1000);
  ESP.restart();
}

void handleModemPowerOn() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  modem.powerOn(POWER_PIN);
  bool success = modem.begin();

  JsonDocument res;
  res["success"] = success;
  res["message"] = success ? "Modem Powered On" : "Initialization Failed";
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleModemPowerOff() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  bool success = modem.powerOff();

  JsonDocument res;
  res["success"] = success;
  res["message"] = success ? "Modem Powered Off" : "Power Off Failed";
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleEspReboot() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  server.send(200, "application/json",
              "{\"success\":true,\"message\":\"Rebooting ESP32...\"}");
  delay(500);
  ESP.restart();
}

void handleQuectelWifiScan() {
  sendCorsHeaders();
  String scanResult = modem.getWifiScan();

  JsonDocument res;
  bool success =
      scanResult.length() > 0 && scanResult.indexOf(F("ERROR")) == -1;
  String formatted = scanResult;
  formatted.replace("\r", "\n");
  res["success"] = success;
  res["raw"] = formatted;
  if (success) {
    JsonArray entries = res.createNestedArray("entries");
    appendWifiEntries(formatted, entries);
  }
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleBluetoothScan() {
  sendCorsHeaders();
  String scanResult = modem.scanBluetooth();

  JsonDocument res;
  bool success =
      scanResult.length() > 0 && scanResult.indexOf(F("ERROR")) == -1;
  String formatted = scanResult;
  formatted.replace("\r", "\n");
  res["success"] = success;
  res["raw"] = formatted;
  if (success) {
    JsonArray entries = res.createNestedArray("entries");
    appendBluetoothEntries(formatted, entries);
  }
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleDeviceSensors() {
  sendCorsHeaders();
  BatteryInfo battery = parseBatteryInfo(modem.getBatteryCharge());
  int adcValue = modem.readADC();

  JsonDocument res;
  JsonObject batt = res["battery"].to<JsonObject>();
  batt["valid"] = battery.valid;
  batt["percent"] = battery.percent;
  batt["status"] = battery.status;
  batt["millivolts"] = battery.millivolts;
  batt["voltage"] =
      battery.millivolts > 0 ? battery.millivolts / 1000.0f : 0.0f;
  batt["raw"] = battery.raw;

  JsonObject sensors = res["sensors"].to<JsonObject>();
  sensors["adc_value"] = adcValue;

  res["timestamp"] = millis();

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handlePdpStatus() {
  sendCorsHeaders();
  int ctxId = server.hasArg("ctx") ? server.arg("ctx").toInt() : 1;
  if (ctxId <= 0)
    ctxId = 1;

  QuectelEC200U::PDPContext ctx = modem.getPDPContext(ctxId);
  ApnSelection selection = getApnSelection();

  JsonDocument res;
  res["ctxId"] = ctxId;
  bool active =
      ctx.cid == ctxId && ctx.p_addr.length() > 0 && ctx.p_addr != "0.0.0.0";
  res["active"] = active;
  res["ip"] = ctx.p_addr;
  res["apn"] = ctx.apn;
  res["type"] = ctx.pdp_type;
  res["dns_primary"] = ctx.dns_p;
  res["dns_secondary"] = ctx.dns_s;

  JsonObject sel = res["selection"].to<JsonObject>();
  sel["apn"] = selection.apn;
  sel["user"] = selection.user;
  sel["pass"] = selection.pass;
  sel["auth"] = selection.auth;
  sel["source"] = selection.source;
  sel["operator"] = selection.operatorName;
  sel["detected"] = selection.detected.found;
  sel["keyword"] = selection.detected.keyword;

  JsonObject stored = res["stored"].to<JsonObject>();
  stored["hasCustom"] = storedApn.hasCustom;
  stored["apn"] = storedApn.apn;
  stored["user"] = storedApn.user;
  stored["pass"] = storedApn.pass;
  stored["auth"] = storedApn.auth;

  res["registration"] = modem.getRegistrationStatus();

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handlePdpActivate() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Invalid JSON\"}");
  }

  int ctxId = doc["ctxId"] | 1;
  if (ctxId <= 0)
    ctxId = 1;
  String operatorHint = doc["operator"] | "";
  bool clearCustomFlag = doc["clearCustom"] | false;
  bool persist = doc["persist"] | false;

  if (clearCustomFlag) {
    clearApnPreferences();
  }

  ApnSelection selection = getApnSelection(operatorHint);
  String apn =
      doc.containsKey("apn") ? String((const char *)doc["apn"]) : selection.apn;
  String user = doc.containsKey("user") ? String((const char *)doc["user"])
                                        : selection.user;
  String pass = doc.containsKey("pass") ? String((const char *)doc["pass"])
                                        : selection.pass;
  int auth = doc.containsKey("auth") ? doc["auth"].as<int>() : selection.auth;

  apn.trim();
  if (apn.length() == 0) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"APN is required\"}");
  }

  if (persist) {
    saveApnPreferences(apn, user, pass, auth);
  }

  bool ctxReady = modem.configureContext(ctxId, 1, apn, user, pass, auth);
  bool pdpReady = ctxReady && modem.activatePDP(ctxId);

  JsonDocument res;
  res["success"] = ctxReady && pdpReady;
  res["ctx_ready"] = ctxReady;
  res["pdp_ready"] = pdpReady;
  res["ctxId"] = ctxId;
  JsonObject apnJson = res["apn"].to<JsonObject>();
  apnJson["apn"] = apn;
  apnJson["user"] = user;
  apnJson["pass"] = pass;
  apnJson["auth"] = auth;

  if (!ctxReady) {
    res["error"] = "Context configuration failed";
  } else if (!pdpReady) {
    res["error"] = "PDP activation failed";
  }

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handlePdpDeactivate() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Invalid JSON\"}");
  }

  int ctxId = doc["ctxId"] | 1;
  if (ctxId <= 0)
    ctxId = 1;
  bool success = modem.deactivatePDP(ctxId);

  JsonDocument res;
  res["success"] = success;
  res["ctxId"] = ctxId;
  if (!success) {
    res["error"] = "Deactivate failed";
  }

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handlePdpClear() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");
  clearApnPreferences();
  JsonDocument res;
  res["success"] = true;
  res["message"] = "Custom APN cleared";
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleMqttStatus() {
  sendCorsHeaders();
  JsonDocument res;
  res["connected"] = mqttConnected;
  res["server"] = mqttServer;
  res["port"] = mqttPort;
  res["ctxId"] = mqttCtxId;
  res["last_topic"] = mqttLastTopic;
  res["last_activity"] = mqttLastActivity;
  res["last_error"] = mqttLastError;
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

static void _updateMqttState(bool connected, const String &host, int port,
                             int ctxId, const String &error) {
  mqttConnected = connected;
  mqttServer = connected ? host : String();
  mqttPort = connected ? port : 0;
  mqttCtxId = ctxId;
  mqttLastError = error;
  if (connected) {
    mqttLastActivity = millis();
  }
}

void handleMqttConnect() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Invalid JSON\"}");
  }

  String host = doc["host"] | "";
  int port = doc["port"] | 1883;
  int ctxId = doc["ctxId"] | 1;
  if (ctxId <= 0)
    ctxId = 1;
  String operatorHint = doc["operator"] | "";

  JsonDocument res;

  if (host.length() == 0 || port <= 0) {
    res["success"] = false;
    res["error"] = "Host and port required";
  } else {
    ApnSelection selection = getApnSelection(operatorHint);

    String apn = doc.containsKey("apn") ? String((const char *)doc["apn"])
                                        : selection.apn;
    String user = doc.containsKey("user") ? String((const char *)doc["user"])
                                          : selection.user;
    String pass = doc.containsKey("pass") ? String((const char *)doc["pass"])
                                          : selection.pass;
    int auth = doc.containsKey("auth") ? doc["auth"].as<int>() : selection.auth;

    apn.trim();
    if (apn.length() == 0) {
      res["success"] = false;
      res["error"] = "APN required";
    } else {
      bool ctxReady = configureContextWithApn(ctxId, apn, user, pass, auth);
      bool connected = false;
      if (ctxReady) {
        if (mqttConnected) {
          modem.mqttDisconnect();
          mqttConnected = false;
        }
        connected = modem.mqttConnect(host, port);
        _updateMqttState(connected, host, port, ctxId,
                         connected ? "" : modem.getLastErrorString());
      }

      res["success"] = ctxReady && connected;
      res["ctx_ready"] = ctxReady;
      res["connected"] = connected;
      JsonObject apnJson = res["apn"].to<JsonObject>();
      apnJson["apn"] = apn;
      apnJson["user"] = user;
      apnJson["pass"] = pass;
      apnJson["auth"] = auth;
      apnJson["source"] = doc.containsKey("apn") ? "request" : selection.source;
      apnJson["operator"] = selection.operatorName;

      if (!ctxReady) {
        res["error"] = "Context failed";
      } else if (!connected) {
        res["error"] =
            mqttLastError.length() ? mqttLastError : "MQTT connect failed";
      }
    }
  }

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleMqttDisconnect() {
  sendCorsHeaders();
  bool success = true;
  if (mqttConnected) {
    success = modem.mqttDisconnect();
  }
  _updateMqttState(false, String(), 0, mqttCtxId,
                   success ? "" : "Disconnect failed");
  JsonDocument res;
  res["success"] = success;
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleMqttPublish() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Invalid JSON\"}");
  }

  String topic = doc["topic"] | "";
  String payload = doc["payload"] | "";

  JsonDocument res;
  if (!mqttConnected) {
    res["success"] = false;
    res["error"] = "MQTT not connected";
  } else if (topic.length() == 0) {
    res["success"] = false;
    res["error"] = "Topic required";
  } else {
    bool success = modem.mqttPublish(topic, payload);
    res["success"] = success;
    if (!success) {
      res["error"] = modem.getLastErrorString();
      mqttLastError = res["error"].as<String>();
    } else {
      mqttLastTopic = topic;
      mqttLastActivity = millis();
      mqttLastError = "";
    }
  }

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handleMqttSubscribe() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Invalid JSON\"}");
  }

  String topic = doc["topic"] | "";
  JsonDocument res;
  if (!mqttConnected) {
    res["success"] = false;
    res["error"] = "MQTT not connected";
  } else if (topic.length() == 0) {
    res["success"] = false;
    res["error"] = "Topic required";
  } else {
    bool success = modem.mqttSubscribe(topic);
    res["success"] = success;
    if (!success) {
      res["error"] = modem.getLastErrorString();
      mqttLastError = res["error"].as<String>();
    } else {
      mqttLastActivity = millis();
      mqttLastError = "";
    }
  }

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

void handlePing() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Invalid JSON\"}");
  }

  String host = doc["host"] | "";
  int contextId = doc["context"] | 1;
  int timeout = doc["timeout"] | 4;
  int count = doc["count"] | 4;

  if (host.length() == 0) {
    return server.send(400, "application/json",
                       "{\"success\":false,\"error\":\"Host required\"}");
  }

  String report;
  bool success = modem.ping(host, report, contextId, timeout, count);

  JsonDocument res;
  res["success"] = success;
  res["report"] = report;
  if (!success) {
    res["error"] = "Ping failed";
  }

  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
}

// Advanced Features Handlers
void handleSimSwitch() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");
  bool success = modem.switchSimCard();
  server.send(200, "application/json",
              success ? "{\"success\":true}" : "{\"success\":false}");
}

void handleToggleISIM() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");
  bool enable = server.arg("enable") == "1";
  bool success = modem.toggleISIM(enable);
  server.send(200, "application/json",
              success ? "{\"success\":true}" : "{\"success\":false}");
}

void handleSetDSDS() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");
  bool dsds = server.arg("dsds") == "1";
  bool success = modem.setDSDSMode(dsds);
  server.send(200, "application/json",
              success ? "{\"success\":true}" : "{\"success\":false}");
}

void handleBlockCalls() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");
  bool enable = server.arg("enable") == "1";
  bool success = modem.blockIncomingCalls(enable);
  server.send(200, "application/json",
              success ? "{\"success\":true}" : "{\"success\":false}");
}

void handleSetUSBMode() {
  sendCorsHeaders();
  if (server.method() != HTTP_POST)
    return server.send(405, "text/plain", "Method Not Allowed");
  bool success = modem.setUSBModeCDC();
  server.send(200, "application/json",
              success ? "{\"success\":true}" : "{\"success\":false}");
}

void setup() {
  Serial.begin(115200);
  // ========== 新增：初始化检测引脚 ==========
  pinMode(MODEM_STATUS_PIN, INPUT); // 设置为输入模式
    // 记录启动时间
    boot_time_ms = millis();

    // 初始化 ESP32 内部温度传感器（仅 ESP32）
#if defined(ESP32)
    esp_err_t ret = temp_sensor_set_config(TEMP_SENSOR_DEFAULT_CONFIG);
    if (ret == ESP_OK) {
        ret = temp_sensor_start();
        if (ret == ESP_OK) {
            temp_sensor_initialized = true;
            Serial.println("ESP32 internal temperature sensor initialized.");
        } else {
            Serial.println("Failed to start temp sensor.");
        }
    } else {
        Serial.println("Failed to set temp sensor config.");
    }
#else
    temp_sensor_initialized = false;
    Serial.println("No internal temperature sensor on ESP8266.");
#endif

#if defined(ESP32)
modemSerial.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
#elif defined(ESP8266)
modemSerial.begin(115200);
#endif
  loadApnPreferences();

  // Init Modem - Try to power on but don't block indefinitely
  Serial.println("Initializing Modem...");
  // modem.powerOn(POWER_PIN); // Moved to manual control or non-blocking check
  // Check if modem is responsive, if not, try power on
  /*if (!modem.begin()) {
    Serial.println("Modem not responding, attempting power on...");
    modem.powerOn(POWER_PIN);
    if (modem.begin()) {
      Serial.println("Modem initialized.");
    } else {
      Serial.println("Modem failed to initialize. Use Web UI to retry.");
    }
  } else {
    Serial.println("Modem already ready.");
  }
*/
  // Attempt Data Attach if APN is stored
  if (storedApn.hasCustom) {
    Serial.println("Found stored APN, attempting data attach...");
    if (modem.attachData(storedApn.apn, storedApn.user, storedApn.pass,
                         storedApn.auth)) {
      Serial.println("Data attached successfully!");
    } else {
      Serial.println("Data attachment failed.");
    }
  }

  // WiFi Setup
// ==========================================
  // WiFi Setup (增强版连接逻辑)
  // ==========================================
  preferences.begin("wifi", true);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  preferences.end();

  bool connected = false;
  if (ssid.length() > 0) {
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    
    // 【关键修复 1】彻底清理之前的 WiFi 状态
    WiFi.disconnect(true);
    delay(500); 
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    // 【关键修复 2】增加超时等待时间：30次 * 500ms = 15秒 (可根据需要改为 40次/20秒)
    for (int i = 0; i < 30; i++) {
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        break;
      }
      delay(500);
      Serial.print(".");
    }
    Serial.println();
  }

  if (connected) {
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    // 连不上才会 fallback 到 AP 模式
    if (ssid.length() > 0) {
      Serial.println("WiFi connection failed! Falling back to Hotspot...");
    } else {
      Serial.println("No WiFi credentials found. Starting Hotspot...");
    }
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_password);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);

    // Start DNS Server for Captive Portal
    dnsServer.start(53, "*", IP);
  }

configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
Serial.println("NTP 时间同步已启动...");

// 1. 允许服务器解析 Cookie (兼容 ESP32 和 ESP8266)
  server.collectHeaders("Cookie", "User-Agent");

  // 2. 注册公开路由 (不需要登录)
  server.on("/", handleRoot);
  server.on("/health", []() { server.send(200, "text/plain", "OK"); });
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/login", HTTP_OPTIONS, handleOptions);
  server.on("/api/logout", HTTP_POST, handleLogout);

  // 3. 使用 bindProtected 批量注册受保护路由
  bindProtected("/api/status", HTTP_GET, handleStatus);
  bindProtected("/api/modem/status", HTTP_GET, handleModemStatus); // <-- 新增这一行
  bindProtected("/api/modem/info", HTTP_GET, handleModemInfo);
  bindProtected("/api/sms/send", HTTP_POST, handleSmsSend);
  bindProtected("/api/sms/read", HTTP_GET, handleSmsRead);
  bindProtected("/api/gps/location", HTTP_GET, handleGpsLocation);
  bindProtected("/api/esp/info", HTTP_GET, handleEspInfo);
  bindProtected("/api/system/time", HTTP_GET, handleGetTime);
  
  bindProtected("/api/call/dial", HTTP_POST, handleCallDial);
  bindProtected("/api/call/hangup", HTTP_POST, handleCallHangup);
  bindProtected("/api/call/answer", HTTP_POST, handleCallAnswer);
  bindProtected("/api/call/status", HTTP_GET, handleCallStatus);
  bindProtected("/api/call/volume", HTTP_POST, handleCallVolume);
  
  bindProtected("/api/at", HTTP_POST, handleAT); // 修正：对应你代码里的 handleAT
  
  bindProtected("/api/tcp/open", HTTP_POST, handleTcpOpen);
  bindProtected("/api/tcp/close", HTTP_POST, handleTcpClose);
  bindProtected("/api/tcp/send", HTTP_POST, handleTcpSend);
  bindProtected("/api/ping", HTTP_POST, handlePing);
  
  bindProtected("/api/device/sensors", HTTP_GET, handleDeviceSensors);
  
  bindProtected("/api/pdp/status", HTTP_GET, handlePdpStatus);
  bindProtected("/api/pdp/activate", HTTP_POST, handlePdpActivate);
  bindProtected("/api/pdp/deactivate", HTTP_POST, handlePdpDeactivate);
  bindProtected("/api/pdp/clear", HTTP_POST, handlePdpClear);
  
  bindProtected("/api/mqtt/status", HTTP_GET, handleMqttStatus);
  bindProtected("/api/mqtt/connect", HTTP_POST, handleMqttConnect);
  bindProtected("/api/mqtt/disconnect", HTTP_POST, handleMqttDisconnect);
  bindProtected("/api/mqtt/publish", HTTP_POST, handleMqttPublish);
  bindProtected("/api/mqtt/subscribe", HTTP_POST, handleMqttSubscribe);
  
  bindProtected("/api/wifi/save", HTTP_POST, handleWifiSave);
  bindProtected("/api/wifi/forget", HTTP_POST, handleWifiForget);
  
  bindProtected("/api/modem/poweron", HTTP_POST, handleModemPowerOn);
  bindProtected("/api/modem/poweroff", HTTP_POST, handleModemPowerOff);
  bindProtected("/api/esp/reboot", HTTP_POST, handleEspReboot); // 修正：对应你代码里的 handleEspReboot
  
  bindProtected("/api/quectel/wifi/scan", HTTP_GET, handleQuectelWifiScan);
  bindProtected("/api/quectel/bt/scan", HTTP_GET, handleBluetoothScan);

  // Advanced Features
  bindProtected("/api/sim/switch", HTTP_POST, handleSimSwitch);
  bindProtected("/api/sim/isim", HTTP_POST, handleToggleISIM);
  bindProtected("/api/sim/dsds", HTTP_POST, handleSetDSDS);
  bindProtected("/api/call/block", HTTP_POST, handleBlockCalls);
  bindProtected("/api/system/usb", HTTP_POST, handleSetUSBMode);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Web Server with Auth started");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
    // 定期读取温度（非阻塞）
    if (millis() - last_temp_read >= TEMP_READ_INTERVAL) {
        last_temp_read = millis();
#if defined(ESP32)
        if (temp_sensor_initialized) {
            float tsens_out;
            esp_err_t ret = temp_sensor_read_celsius(&tsens_out);
            if (ret == ESP_OK) {
                current_temperature = tsens_out;
            } else {
                current_temperature = -127.0f;
            }
        } else {
            current_temperature = -127.0f;
        }
#else
        current_temperature = -127.0f;
#endif
    }
  // Add any non-blocking modem maintenance here if needed
}
