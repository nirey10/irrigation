#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include(<WebSocketsServer.h>)
#include <WebSocketsServer.h>
#define USE_WEBSOCKETS 1
#else
#define USE_WEBSOCKETS 0
#endif

#if USE_WEBSOCKETS
void webSocketEvent(uint8_t clientNumber, WStype_t type, uint8_t *payload, size_t length);
#endif

const uint8_t DEFAULT_PUMP_PIN = 16;
const bool DEFAULT_PUMP_ACTIVE_HIGH = false;  // Active-LOW relay: LOW turns pump on, HIGH turns pump off

const int WEB_PORT = 80;
const int WS_PORT = 81;

const char *DEFAULT_STATION_SSID = "Niris_2.4";
const char *DEFAULT_STATION_PASSWORD = "irchukit28";

const char *DEFAULT_AP_SSID = "ESP32-Irrigation";
const char *DEFAULT_AP_PASSWORD = "irrigation123";
const char *DEFAULT_AP_IP = "192.168.4.1";

// Israel timezone with daylight saving time. Change this if you are elsewhere.
const char *DEFAULT_TIMEZONE = "IST-2IDT,M3.4.4/26,M10.5.0";
const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "time.google.com";
const char *NTP_SERVER_3 = "time.nist.gov";

const unsigned long WIFI_RECONNECT_INTERVAL_MS = 30000UL;
const unsigned long TIME_SYNC_RETRY_INTERVAL_MS = 30000UL;
const unsigned long TIME_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
const unsigned long STATUS_BROADCAST_INTERVAL_MS = 1000UL;

struct IrrigationSettings {
  uint8_t pumpPin;
  bool pumpActiveHigh;

  bool scheduleEnabled;
  uint8_t scheduleHour;
  uint8_t scheduleMinute;
  uint32_t scheduleDurationSec;
  uint32_t manualDurationSec;
  String timezone;

  String stationSsid;
  String stationPassword;
  bool stationUseStaticIp;
  String stationIp;
  String stationGateway;
  String stationSubnet;
  String stationDns;

  String apSsid;
  String apPassword;
  String apIp;
};

WebServer server(WEB_PORT);
#if USE_WEBSOCKETS
WebSocketsServer webSocket(WS_PORT);
#endif
Preferences preferences;
IrrigationSettings settings;

bool pumpRunning = false;
unsigned long pumpStopAtMs = 0;
String pumpReason = "";

unsigned long lastWiFiAttemptMs = 0;
unsigned long lastTimeSyncAttemptMs = 0;
unsigned long lastStatusBroadcastMs = 0;
bool timeWasSynced = false;
bool statusDirty = true;
bool accessPointStarted = false;
bool networkRestartPending = false;
unsigned long networkRestartAtMs = 0;

int lastScheduledRunYear = -1;
int lastScheduledRunDay = -1;
int configuredWiFiChannel = 0;
int configuredWiFiRssi = -999;
uint8_t configuredWiFiBssid[6] = {0};
bool hasConfiguredWiFiBssid = false;

String htmlEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length());

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') {
      escaped += F("&amp;");
    } else if (c == '<') {
      escaped += F("&lt;");
    } else if (c == '>') {
      escaped += F("&gt;");
    } else if (c == '"') {
      escaped += F("&quot;");
    } else if (c == '\'') {
      escaped += F("&#39;");
    } else {
      escaped += c;
    }
  }

  return escaped;
}

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length());

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      escaped += '\\';
      escaped += c;
    } else if (c == '\n') {
      escaped += F("\\n");
    } else if (c == '\r') {
      escaped += F("\\r");
    } else if (c == '\t') {
      escaped += F("\\t");
    } else {
      escaped += c;
    }
  }

  return escaped;
}

bool parseIpAddress(const String &text, IPAddress &address) {
  String trimmed = text;
  trimmed.trim();
  return trimmed.length() > 0 && address.fromString(trimmed);
}

String checkedAttr(bool checked) {
  return checked ? " checked" : "";
}

void markStatusDirty() {
  statusDirty = true;
}

bool isOutputCapableGpio(uint8_t gpio) {
  return gpio <= 33;
}

void writePumpPin(uint8_t pin, bool activeHigh, bool on) {
  digitalWrite(pin, on == activeHigh ? HIGH : LOW);
}

void writePump(bool on) {
  writePumpPin(settings.pumpPin, settings.pumpActiveHigh, on);
}

void configurePumpOutput(uint8_t oldPin, bool oldActiveHigh) {
  if (oldPin != 255 && oldPin != settings.pumpPin) {
    pinMode(oldPin, OUTPUT);
    writePumpPin(oldPin, oldActiveHigh, false);
  }

  pinMode(settings.pumpPin, OUTPUT);
  writePump(false);
}

void configurePumpOutput() {
  configurePumpOutput(255, true);
}

void stopPump() {
  if (!pumpRunning) {
    writePump(false);
    return;
  }

  writePump(false);
  pumpRunning = false;
  pumpStopAtMs = 0;
  pumpReason = "";
  markStatusDirty();
  Serial.println("Pump is OFF");
}

void startPump(uint32_t durationSec, const String &reason) {
  if (durationSec == 0) {
    Serial.println("Pump start ignored because duration is 0 seconds");
    return;
  }

  if (durationSec > 86400UL) {
    durationSec = 86400UL;
  }

  pumpRunning = true;
  pumpReason = reason;
  pumpStopAtMs = millis() + (durationSec * 1000UL);
  writePump(true);
  markStatusDirty();

  Serial.printf("Pump is ON for %lu seconds (%s)\n",
                (unsigned long)durationSec,
                reason.c_str());
}

void updatePump() {
  if (pumpRunning && (long)(millis() - pumpStopAtMs) >= 0) {
    stopPump();
  }
}

uint32_t pumpRemainingSec() {
  if (!pumpRunning) {
    return 0;
  }

  long remainingMs = (long)(pumpStopAtMs - millis());
  if (remainingMs <= 0) {
    return 0;
  }

  return (remainingMs + 999L) / 1000UL;
}

void normalizeSettings() {
  if (!isOutputCapableGpio(settings.pumpPin)) {
    settings.pumpPin = DEFAULT_PUMP_PIN;
  }

  if (settings.scheduleHour > 23) {
    settings.scheduleHour = 5;
  }
  if (settings.scheduleMinute > 59) {
    settings.scheduleMinute = 0;
  }
  if (settings.scheduleDurationSec == 0 || settings.scheduleDurationSec > 86400UL) {
    settings.scheduleDurationSec = 60;
  }
  if (settings.manualDurationSec == 0 || settings.manualDurationSec > 86400UL) {
    settings.manualDurationSec = 10;
  }

  settings.stationSsid.trim();
  settings.apSsid.trim();
  settings.apIp.trim();
  settings.stationIp.trim();
  settings.stationGateway.trim();
  settings.stationSubnet.trim();
  settings.stationDns.trim();
  settings.timezone.trim();

  if (settings.timezone.length() == 0) {
    settings.timezone = DEFAULT_TIMEZONE;
  }

  if (settings.apSsid.length() == 0) {
    settings.apSsid = DEFAULT_AP_SSID;
  }
  if (settings.apPassword.length() < 8) {
    settings.apPassword = DEFAULT_AP_PASSWORD;
  }

  IPAddress testIp;
  if (!parseIpAddress(settings.apIp, testIp)) {
    settings.apIp = DEFAULT_AP_IP;
  }

  if (settings.stationSubnet.length() == 0) {
    settings.stationSubnet = "255.255.255.0";
  }
}

void loadSettings() {
  preferences.begin("irrigation", false);

  settings.pumpPin = preferences.getUChar("pumpPin", DEFAULT_PUMP_PIN);
  settings.pumpActiveHigh = preferences.getBool("pumpHigh", DEFAULT_PUMP_ACTIVE_HIGH);
  if (!preferences.getBool("pumpLowV1", false)) {
    settings.pumpActiveHigh = DEFAULT_PUMP_ACTIVE_HIGH;
    preferences.putBool("pumpHigh", settings.pumpActiveHigh);
    preferences.putBool("pumpLowV1", true);
    Serial.println("Pump logic migrated to active LOW default");
  }

  settings.scheduleEnabled = preferences.getBool("enabled", true);
  settings.scheduleHour = preferences.getUChar("hour", 5);
  settings.scheduleMinute = preferences.getUChar("minute", 0);
  settings.scheduleDurationSec = preferences.getUInt("runSecs", 60);
  settings.manualDurationSec = preferences.getUInt("manualSecs", 10);
  settings.timezone = preferences.getString("tz", DEFAULT_TIMEZONE);

  settings.stationSsid = preferences.getString("staSsid", DEFAULT_STATION_SSID);
  settings.stationPassword = preferences.getString("staPass", DEFAULT_STATION_PASSWORD);
  settings.stationUseStaticIp = preferences.getBool("useStatic", false);
  settings.stationIp = preferences.getString("staIp", "");
  settings.stationGateway = preferences.getString("staGw", "");
  settings.stationSubnet = preferences.getString("staMask", "255.255.255.0");
  settings.stationDns = preferences.getString("staDns", "");

  settings.apSsid = preferences.getString("apSsid", DEFAULT_AP_SSID);
  settings.apPassword = preferences.getString("apPass", DEFAULT_AP_PASSWORD);
  settings.apIp = preferences.getString("apIp", DEFAULT_AP_IP);

  normalizeSettings();
}

void saveSettings() {
  normalizeSettings();

  preferences.putUChar("pumpPin", settings.pumpPin);
  preferences.putBool("pumpHigh", settings.pumpActiveHigh);

  preferences.putBool("enabled", settings.scheduleEnabled);
  preferences.putUChar("hour", settings.scheduleHour);
  preferences.putUChar("minute", settings.scheduleMinute);
  preferences.putUInt("runSecs", settings.scheduleDurationSec);
  preferences.putUInt("manualSecs", settings.manualDurationSec);
  preferences.putString("tz", settings.timezone);

  preferences.putString("staSsid", settings.stationSsid);
  preferences.putString("staPass", settings.stationPassword);
  preferences.putBool("useStatic", settings.stationUseStaticIp);
  preferences.putString("staIp", settings.stationIp);
  preferences.putString("staGw", settings.stationGateway);
  preferences.putString("staMask", settings.stationSubnet);
  preferences.putString("staDns", settings.stationDns);

  preferences.putString("apSsid", settings.apSsid);
  preferences.putString("apPass", settings.apPassword);
  preferences.putString("apIp", settings.apIp);

  lastScheduledRunYear = -1;
  lastScheduledRunDay = -1;
  markStatusDirty();

  Serial.printf("Settings saved: pump GPIO%u active %s, timezone=%s, schedule=%s %02u:%02u for %lu sec, manual=%lu sec\n",
                (unsigned int)settings.pumpPin,
                settings.pumpActiveHigh ? "HIGH" : "LOW",
                settings.timezone.c_str(),
                settings.scheduleEnabled ? "enabled" : "disabled",
                (unsigned int)settings.scheduleHour,
                (unsigned int)settings.scheduleMinute,
                (unsigned long)settings.scheduleDurationSec,
                (unsigned long)settings.manualDurationSec);
}

const char *getWiFiStatusName(int status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:
      return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:
      return "WL_CONNECTED";
    case WL_CONNECT_FAILED:
      return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "WL_DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

const char *getWiFiEncryptionName(int encryptionType) {
  switch (encryptionType) {
    case WIFI_AUTH_OPEN:
      return "open";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2 Enterprise";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3";
    default:
      return "unknown";
  }
}

bool applyStationIpConfig() {
  if (!settings.stationUseStaticIp) {
    return true;
  }

  IPAddress stationIp;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns;

  if (!parseIpAddress(settings.stationIp, stationIp) ||
      !parseIpAddress(settings.stationGateway, gateway) ||
      !parseIpAddress(settings.stationSubnet, subnet)) {
    Serial.println("Static station IP settings are invalid. Falling back to DHCP.");
    return false;
  }

  if (!parseIpAddress(settings.stationDns, dns)) {
    dns = gateway;
  }

  if (!WiFi.config(stationIp, gateway, subnet, dns)) {
    Serial.println("WiFi.config failed. Falling back to DHCP.");
    return false;
  }

  Serial.print("Using static station IP: ");
  Serial.println(stationIp);
  return true;
}

void scanForConfiguredWiFi() {
  Serial.println("Scanning nearby 2.4GHz WiFi networks...");

  configuredWiFiChannel = 0;
  configuredWiFiRssi = -999;
  hasConfiguredWiFiBssid = false;

  int networkCount = WiFi.scanNetworks();
  if (networkCount <= 0) {
    Serial.println("No WiFi networks found by the ESP32.");
    WiFi.scanDelete();
    return;
  }

  bool foundConfiguredSsid = false;

  for (int i = 0; i < networkCount; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid == settings.stationSsid) {
      foundConfiguredSsid = true;
      Serial.printf("Found configured SSID '%s': RSSI %d dBm, channel %d, security %s\n",
                    settings.stationSsid.c_str(),
                    WiFi.RSSI(i),
                    WiFi.channel(i),
                    getWiFiEncryptionName((int)WiFi.encryptionType(i)));

      if (WiFi.RSSI(i) > configuredWiFiRssi) {
        configuredWiFiRssi = WiFi.RSSI(i);
        configuredWiFiChannel = WiFi.channel(i);
        memcpy(configuredWiFiBssid, WiFi.BSSID(i), sizeof(configuredWiFiBssid));
        hasConfiguredWiFiBssid = true;
      }
    }
  }

  if (!foundConfiguredSsid) {
    Serial.printf("Configured SSID '%s' was not found. Check spelling and use a 2.4GHz network.\n",
                  settings.stationSsid.c_str());
  } else {
    Serial.printf("Best match: RSSI %d dBm, channel %d, BSSID %02X:%02X:%02X:%02X:%02X:%02X\n",
                  configuredWiFiRssi,
                  configuredWiFiChannel,
                  configuredWiFiBssid[0],
                  configuredWiFiBssid[1],
                  configuredWiFiBssid[2],
                  configuredWiFiBssid[3],
                  configuredWiFiBssid[4],
                  configuredWiFiBssid[5]);

    if (configuredWiFiRssi < -80) {
      Serial.println("Warning: WiFi signal is very weak. Move the ESP32 closer to the router or use an extender.");
    }
  }

  WiFi.scanDelete();
}

bool startAccessPoint() {
  IPAddress apIp;
  if (!parseIpAddress(settings.apIp, apIp)) {
    apIp.fromString(DEFAULT_AP_IP);
  }

  IPAddress gateway = apIp;
  IPAddress subnet(255, 255, 255, 0);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(apIp, gateway, subnet);

  if (settings.apPassword.length() >= 8) {
    accessPointStarted = WiFi.softAP(settings.apSsid.c_str(), settings.apPassword.c_str());
  } else {
    accessPointStarted = WiFi.softAP(settings.apSsid.c_str());
  }

  if (accessPointStarted) {
    Serial.print("Configuration hotspot started. WiFi name: ");
    Serial.println(settings.apSsid);
    Serial.print("Settings page: http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Configuration hotspot failed to start.");
  }

  markStatusDirty();
  return accessPointStarted;
}

void printNetworkInfo() {
  Serial.println("Network details:");

  if (accessPointStarted) {
    Serial.print("  Hotspot SSID: ");
    Serial.println(settings.apSsid);
    Serial.print("  Hotspot page: http://");
    Serial.println(WiFi.softAPIP());
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("  Home WiFi IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("  Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("  Subnet: ");
    Serial.println(WiFi.subnetMask());
    Serial.print("  DNS: ");
    Serial.println(WiFi.dnsIP());
    Serial.print("  RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("  Home WiFi: disconnected");
  }
}

void connectToWiFi() {
  lastWiFiAttemptMs = millis();

  if (settings.stationSsid.length() == 0) {
    Serial.println("Home WiFi SSID is empty. Use the hotspot settings page to configure it.");
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(300);

  applyStationIpConfig();
  scanForConfiguredWiFi();

  if (hasConfiguredWiFiBssid) {
    WiFi.begin(settings.stationSsid.c_str(),
               settings.stationPassword.c_str(),
               configuredWiFiChannel,
               configuredWiFiBssid);
  } else {
    WiFi.begin(settings.stationSsid.c_str(), settings.stationPassword.c_str());
  }

  Serial.printf("Connecting to home WiFi: %s\n", settings.stationSsid.c_str());

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000UL) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Home WiFi connected.");
    printNetworkInfo();
  } else {
    int status = (int)WiFi.status();
    Serial.printf("Home WiFi connection failed. Status: %s (%d). Will retry in the background.\n",
                  getWiFiStatusName(status),
                  status);
  }

  markStatusDirty();
}

bool readLocalTime(struct tm *timeInfo, uint32_t waitMs) {
  return getLocalTime(timeInfo, waitMs);
}

String currentTimeText() {
  struct tm timeInfo;
  if (!readLocalTime(&timeInfo, 50)) {
    return "Time not synced";
  }

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
  return String(buffer);
}

void syncTimeIfNeeded(bool force) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!force) {
    unsigned long interval = timeWasSynced ? TIME_RESYNC_INTERVAL_MS : TIME_SYNC_RETRY_INTERVAL_MS;
    if (millis() - lastTimeSyncAttemptMs < interval) {
      return;
    }
  }

  lastTimeSyncAttemptMs = millis();
  Serial.println("Syncing time from NTP");

  configTzTime(settings.timezone.c_str(), NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  struct tm timeInfo;
  if (readLocalTime(&timeInfo, 5000)) {
    timeWasSynced = true;
    markStatusDirty();
    Serial.print("Time synced: ");
    Serial.println(currentTimeText());
  } else {
    Serial.println("Time sync failed. Will try again later.");
  }
}

void updateWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastWiFiAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      syncTimeIfNeeded(true);
    }
  }
}

void updateDailySchedule() {
  if (!settings.scheduleEnabled || !timeWasSynced) {
    return;
  }

  struct tm timeInfo;
  if (!readLocalTime(&timeInfo, 50)) {
    return;
  }

  bool isScheduledMinute = timeInfo.tm_hour == settings.scheduleHour &&
                           timeInfo.tm_min == settings.scheduleMinute;
  bool alreadyRanToday = timeInfo.tm_year == lastScheduledRunYear &&
                         timeInfo.tm_yday == lastScheduledRunDay;

  if (!isScheduledMinute || alreadyRanToday) {
    return;
  }

  lastScheduledRunYear = timeInfo.tm_year;
  lastScheduledRunDay = timeInfo.tm_yday;

  if (pumpRunning) {
    Serial.println("Scheduled watering skipped because pump is already running");
    return;
  }

  startPump(settings.scheduleDurationSec, "daily schedule");
}

uint32_t readUIntArg(const char *name, uint32_t minValue, uint32_t maxValue, uint32_t fallback) {
  if (!server.hasArg(name)) {
    return fallback;
  }

  long parsed = server.arg(name).toInt();
  if (parsed < (long)minValue) {
    return minValue;
  }

  uint32_t value = (uint32_t)parsed;
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

String statusJson() {
  String json;
  json.reserve(420);

  json += F("{\"pump\":\"");
  json += pumpRunning ? "ON" : "OFF";
  json += F("\",\"remaining\":");
  json += String(pumpRemainingSec());
  json += F(",\"reason\":\"");
  json += jsonEscape(pumpReason);
  json += F("\",\"time\":\"");
  json += jsonEscape(currentTimeText());
  json += F("\",\"station\":\"");
  json += WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
  json += F("\",\"stationIp\":\"");
  json += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  json += F("\",\"apIp\":\"");
  json += WiFi.softAPIP().toString();
  json += F("\",\"apSsid\":\"");
  json += jsonEscape(settings.apSsid);
  json += F("\",\"rssi\":");
  json += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : "0";
  json += F("}");

  return json;
}

#if USE_WEBSOCKETS
void sendStatusToClient(uint8_t clientNumber) {
  String payload = statusJson();
  webSocket.sendTXT(clientNumber, payload);
}

void broadcastStatus() {
  String payload = statusJson();
  webSocket.broadcastTXT(payload);
  statusDirty = false;
  lastStatusBroadcastMs = millis();
}

void webSocketEvent(uint8_t clientNumber, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    IPAddress remoteIp = webSocket.remoteIP(clientNumber);
    Serial.printf("WebSocket client %u connected from %s\n",
                  clientNumber,
                  remoteIp.toString().c_str());
    sendStatusToClient(clientNumber);
    return;
  }

  if (type == WStype_DISCONNECTED) {
    Serial.printf("WebSocket client %u disconnected\n", clientNumber);
    return;
  }

  if (type != WStype_TEXT) {
    return;
  }

  String message;
  for (size_t i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  if (message == "status") {
    sendStatusToClient(clientNumber);
  } else if (message == "stop") {
    stopPump();
    broadcastStatus();
  } else if (message.startsWith("manual:")) {
    uint32_t durationSec = (uint32_t)message.substring(7).toInt();
    startPump(durationSec, "manual websocket");
    broadcastStatus();
  }
}
#endif

void broadcastStatusIfNeeded() {
#if USE_WEBSOCKETS
  if (statusDirty || millis() - lastStatusBroadcastMs >= STATUS_BROADCAST_INTERVAL_MS) {
    broadcastStatus();
  }
#else
  if (statusDirty || millis() - lastStatusBroadcastMs >= STATUS_BROADCAST_INTERVAL_MS) {
    statusDirty = false;
    lastStatusBroadcastMs = millis();
  }
#endif
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void scheduleNetworkRestart() {
  networkRestartPending = true;
  networkRestartAtMs = millis() + 1200UL;
}

void restartNetworking() {
  Serial.println("Restarting networking with saved settings");

  WiFi.disconnect(false, false);
  WiFi.softAPdisconnect(true);
  delay(500);

  accessPointStarted = false;
  timeWasSynced = false;
  startAccessPoint();
  connectToWiFi();
  syncTimeIfNeeded(true);
  printNetworkInfo();
  markStatusDirty();
}

void updateNetworkRestart() {
  if (networkRestartPending && (long)(millis() - networkRestartAtMs) >= 0) {
    networkRestartPending = false;
    restartNetworking();
  }
}

void handleRoot() {
  String page;
  page.reserve(16000);

  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>ESP32 Irrigation</title>");
  page += F("<style>");
  page += F("body{font-family:Arial,sans-serif;margin:0;background:#f5f7f8;color:#1c2529}");
  page += F("main{max-width:880px;margin:0 auto;padding:24px}");
  page += F("section{background:#fff;border:1px solid #d9e0e3;border-radius:8px;padding:18px;margin:14px 0}");
  page += F("h1{font-size:28px;margin:0 0 14px}h2{font-size:20px;margin:0 0 12px}");
  page += F("label{display:block;font-weight:700;margin:12px 0 6px}");
  page += F("input{width:100%;box-sizing:border-box;padding:10px;border:1px solid #b9c3c8;border-radius:6px;font-size:16px}");
  page += F(".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}");
  page += F(".status{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}");
  page += F(".status div{background:#eef3f4;border-radius:6px;padding:10px;min-height:44px}");
  page += F("button{border:0;border-radius:6px;background:#116a5b;color:#fff;padding:11px 16px;font-size:16px;cursor:pointer}");
  page += F("button.secondary{background:#37474f}.muted{color:#607078;font-size:14px}");
  page += F("@media(max-width:720px){main{padding:16px}.row,.status{grid-template-columns:1fr}}");
  page += F("</style></head><body><main>");
  page += F("<h1>ESP32 Irrigation</h1>");

  page += F("<section><h2>Status</h2><div class='status'>");
  page += F("<div><strong>Pump</strong><br><span id='pumpState'>");
  page += pumpRunning ? "ON" : "OFF";
  page += F("</span></div>");
  page += F("<div><strong>Remaining</strong><br><span id='remaining'>");
  page += String(pumpRemainingSec());
  page += F("</span> sec</div>");
  page += F("<div><strong>Time</strong><br><span id='timeNow'>");
  page += htmlEscape(currentTimeText());
  page += F("</span></div>");
  page += F("<div><strong>Home WiFi</strong><br><span id='stationState'>");
  page += WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
  page += F("</span></div>");
  page += F("<div><strong>Home IP</strong><br><span id='stationIp'>");
  page += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  page += F("</span></div>");
  page += F("<div><strong>Hotspot IP</strong><br><span id='apIp'>");
  page += WiFi.softAPIP().toString();
  page += F("</span></div>");
  page += F("<div><strong>Pump GPIO</strong><br>GPIO ");
  page += String(settings.pumpPin);
  page += F("</div></div>");
  page += F("<p class='muted'>Connect to the ESP32 hotspot and open the hotspot IP shown here or in Serial Monitor.</p>");
  page += F("<p class='muted'>WebSocket: <span id='wsState'>connecting</span></p>");
  page += F("</section>");

  page += F("<section><h2>Settings</h2>");
  page += F("<form method='post' action='/settings'>");
  page += F("<h2>Hardware</h2>");
  page += F("<label for='pumpPin'>Pump GPIO pin</label><input id='pumpPin' name='pumpPin' type='number' min='0' max='33' value='");
  page += String(settings.pumpPin);
  page += F("'>");
  page += F("<label><input type='checkbox' name='pumpActiveHigh' value='1'");
  page += checkedAttr(settings.pumpActiveHigh);
  page += F(" style='width:auto'> Pump turns on when GPIO is HIGH</label>");
  page += F("<h2>Time</h2>");
  page += F("<label for='timezone'>Timezone POSIX string</label><input id='timezone' name='timezone' value='");
  page += htmlEscape(settings.timezone);
  page += F("'>");
  page += F("<h2>Daily Schedule</h2>");
  page += F("<label><input type='checkbox' name='enabled' value='1'");
  page += checkedAttr(settings.scheduleEnabled);
  page += F(" style='width:auto'> Enable daily watering</label>");
  page += F("<div class='row'><div><label for='hour'>Hour</label><input id='hour' name='hour' type='number' min='0' max='23' value='");
  page += String(settings.scheduleHour);
  page += F("'></div><div><label for='minute'>Minute</label><input id='minute' name='minute' type='number' min='0' max='59' value='");
  page += String(settings.scheduleMinute);
  page += F("'></div></div>");
  page += F("<label for='duration'>Scheduled pump duration, seconds</label><input id='duration' name='duration' type='number' min='1' max='86400' value='");
  page += String(settings.scheduleDurationSec);
  page += F("'>");
  page += F("<label for='manualDuration'>Default manual pump duration, seconds</label><input id='manualDuration' name='manualDuration' type='number' min='1' max='86400' value='");
  page += String(settings.manualDurationSec);
  page += F("'>");
  page += F("<p><button type='submit'>Save Schedule</button></p>");
  page += F("</form></section>");

  page += F("<section><h2>Manual Pump</h2>");
  page += F("<form method='post' action='/manual'>");
  page += F("<label for='manualRunDuration'>Run duration, seconds</label><input id='manualRunDuration' name='duration' type='number' min='1' max='86400' value='");
  page += String(settings.manualDurationSec);
  page += F("'>");
  page += F("<p><button type='submit'>Run Pump</button></p>");
  page += F("</form>");
  page += F("<form method='post' action='/stop'><button class='secondary' type='submit'>Stop Pump</button></form>");
  page += F("</section>");

  page += F("<section><h2>Network</h2>");
  page += F("<form method='post' action='/network'>");
  page += F("<h2>Home WiFi</h2>");
  page += F("<label for='stationSsid'>WiFi name</label><input id='stationSsid' name='stationSsid' value='");
  page += htmlEscape(settings.stationSsid);
  page += F("'>");
  page += F("<label for='stationPassword'>WiFi password</label><input id='stationPassword' name='stationPassword' type='password' placeholder='Leave blank to keep current password'>");
  page += F("<label><input type='checkbox' name='stationUseStaticIp' value='1'");
  page += checkedAttr(settings.stationUseStaticIp);
  page += F(" style='width:auto'> Use static home WiFi IP</label>");
  page += F("<div class='row'><div><label for='stationIp'>Home IP</label><input id='stationIp' name='stationIp' value='");
  page += htmlEscape(settings.stationIp);
  page += F("'></div><div><label for='stationGateway'>Gateway</label><input id='stationGateway' name='stationGateway' value='");
  page += htmlEscape(settings.stationGateway);
  page += F("'></div></div>");
  page += F("<div class='row'><div><label for='stationSubnet'>Subnet</label><input id='stationSubnet' name='stationSubnet' value='");
  page += htmlEscape(settings.stationSubnet);
  page += F("'></div><div><label for='stationDns'>DNS</label><input id='stationDns' name='stationDns' value='");
  page += htmlEscape(settings.stationDns);
  page += F("'></div></div>");
  page += F("<h2>ESP32 Hotspot</h2>");
  page += F("<label for='apSsid'>Hotspot name</label><input id='apSsid' name='apSsid' value='");
  page += htmlEscape(settings.apSsid);
  page += F("'>");
  page += F("<label for='apPassword'>Hotspot password</label><input id='apPassword' name='apPassword' type='password' minlength='8' placeholder='Leave blank to keep current password'>");
  page += F("<label for='apIpField'>Hotspot IP</label><input id='apIpField' name='apIp' value='");
  page += htmlEscape(settings.apIp);
  page += F("'>");
  page += F("<p><button type='submit'>Save Network</button></p>");
  page += F("</form></section>");

  page += F("<script>");
  page += F("let ws;function setText(id,v){const e=document.getElementById(id);if(e)e.textContent=v||'';}");
  page += F("function applyStatus(s){setText('pumpState',s.pump);setText('remaining',s.remaining);setText('timeNow',s.time);setText('stationState',s.station);setText('stationIp',s.stationIp);setText('apIp',s.apIp);}");
  page += F("async function pollStatus(){try{const r=await fetch('/status',{cache:'no-store'});applyStatus(await r.json());if(!ws||ws.readyState!==1)setText('wsState','polling');}catch(e){}}");
  page += F("function connectWs(){if(!('WebSocket'in window)){setText('wsState','polling');return;}setText('wsState','connecting');ws=new WebSocket('ws://'+location.hostname+':81/');");
  page += F("ws.onopen=()=>{setText('wsState','connected');ws.send('status');};");
  page += F("ws.onmessage=e=>{try{applyStatus(JSON.parse(e.data));}catch(err){}};");
  page += F("ws.onclose=()=>{setText('wsState','polling');setTimeout(connectWs,5000);};");
  page += F("ws.onerror=()=>{setText('wsState','error');};}");
  page += F("setInterval(()=>{if(!ws||ws.readyState!==1)pollStatus();},1000);connectWs();pollStatus();");
  page += F("</script>");

  page += F("</main></body></html>");

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
}

void handleSettings() {
  uint8_t oldPumpPin = settings.pumpPin;
  bool oldPumpActiveHigh = settings.pumpActiveHigh;
  String oldTimezone = settings.timezone;

  settings.pumpPin = (uint8_t)readUIntArg("pumpPin", 0, 33, settings.pumpPin);
  settings.pumpActiveHigh = server.hasArg("pumpActiveHigh");

  if (server.hasArg("timezone")) {
    settings.timezone = server.arg("timezone");
  }

  settings.scheduleEnabled = server.hasArg("enabled");
  settings.scheduleHour = (uint8_t)readUIntArg("hour", 0, 23, settings.scheduleHour);
  settings.scheduleMinute = (uint8_t)readUIntArg("minute", 0, 59, settings.scheduleMinute);
  settings.scheduleDurationSec = readUIntArg("duration", 1, 86400UL, settings.scheduleDurationSec);
  settings.manualDurationSec = readUIntArg("manualDuration", 1, 86400UL, settings.manualDurationSec);

  saveSettings();

  if (oldPumpPin != settings.pumpPin || oldPumpActiveHigh != settings.pumpActiveHigh) {
    pumpRunning = false;
    pumpStopAtMs = 0;
    pumpReason = "";
    configurePumpOutput(oldPumpPin, oldPumpActiveHigh);
    Serial.printf("Pump output configured: GPIO%u, active %s\n",
                  (unsigned int)settings.pumpPin,
                  settings.pumpActiveHigh ? "HIGH" : "LOW");
  }

  if (oldTimezone != settings.timezone) {
    timeWasSynced = false;
    syncTimeIfNeeded(true);
  }

  redirectHome();
}

void handleNetwork() {
  if (server.hasArg("stationSsid")) {
    settings.stationSsid = server.arg("stationSsid");
  }
  if (server.hasArg("stationPassword") && server.arg("stationPassword").length() > 0) {
    settings.stationPassword = server.arg("stationPassword");
  }

  settings.stationUseStaticIp = server.hasArg("stationUseStaticIp");
  if (server.hasArg("stationIp")) {
    settings.stationIp = server.arg("stationIp");
  }
  if (server.hasArg("stationGateway")) {
    settings.stationGateway = server.arg("stationGateway");
  }
  if (server.hasArg("stationSubnet")) {
    settings.stationSubnet = server.arg("stationSubnet");
  }
  if (server.hasArg("stationDns")) {
    settings.stationDns = server.arg("stationDns");
  }

  if (server.hasArg("apSsid")) {
    settings.apSsid = server.arg("apSsid");
  }
  if (server.hasArg("apPassword") && server.arg("apPassword").length() >= 8) {
    settings.apPassword = server.arg("apPassword");
  }
  if (server.hasArg("apIp")) {
    settings.apIp = server.arg("apIp");
  }

  saveSettings();
  scheduleNetworkRestart();
  redirectHome();
}

void handleManual() {
  uint32_t durationSec = readUIntArg("duration", 1, 86400UL, settings.manualDurationSec);
  startPump(durationSec, "manual");
  redirectHome();
}

void handleStop() {
  stopPump();
  redirectHome();
}

void handleStatus() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", statusJson());
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/network", HTTP_POST, handleNetwork);
  server.on("/manual", HTTP_POST, handleManual);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/status", HTTP_GET, handleStatus);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.printf("Web server started on port %d\n", WEB_PORT);
}

void setupWebSocket() {
#if USE_WEBSOCKETS
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.printf("WebSocket server started on port %d\n", WS_PORT);
#else
  Serial.println("WebSocket library not installed. Using HTTP status polling fallback.");
#endif
}

void setup() {
  Serial.begin(115200);
  delay(500);

  loadSettings();
  configurePumpOutput();

  Serial.println();
  Serial.println("Irrigation program started");
  Serial.printf("Pump pin: GPIO%u, active %s\n",
                (unsigned int)settings.pumpPin,
                settings.pumpActiveHigh ? "HIGH" : "LOW");
  Serial.printf("Daily schedule: %s at %02u:%02u for %lu seconds\n",
                settings.scheduleEnabled ? "enabled" : "disabled",
                (unsigned int)settings.scheduleHour,
                (unsigned int)settings.scheduleMinute,
                (unsigned long)settings.scheduleDurationSec);

  startAccessPoint();
  setupWebServer();
  setupWebSocket();
  connectToWiFi();
  syncTimeIfNeeded(true);
  printNetworkInfo();
}

void loop() {
  server.handleClient();
#if USE_WEBSOCKETS
  webSocket.loop();
#endif
  updateNetworkRestart();
  updateWiFi();
  syncTimeIfNeeded(false);
  updateDailySchedule();
  updatePump();
  broadcastStatusIfNeeded();
}
