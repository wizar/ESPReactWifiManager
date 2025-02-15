#include <ESPReactWifiManager.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <FS.h>

extern "C" {
#include <user_interface.h>
#include <wpa2_enterprise.h>
}

typedef int wifi_ssid_count_t;
typedef uint8 wifi_cred_t;
typedef struct station_config sta_config_t;
#define esp_wifi_sta_wpa2_ent_set_identity wifi_station_set_enterprise_identity
#define esp_wifi_sta_wpa2_ent_set_username wifi_station_set_enterprise_username
#define esp_wifi_sta_wpa2_ent_set_password wifi_station_set_enterprise_password
#define ENCRYPTION_NONE ENC_TYPE_NONE
#define ENCRYPTION_ENT 255

#else
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_wifi.h>
#include <esp_wpa2.h>

typedef int16_t wifi_ssid_count_t;
typedef unsigned char wifi_cred_t;
typedef wifi_sta_config_t sta_config_t;
#define ENCRYPTION_NONE WIFI_AUTH_OPEN
#define ENCRYPTION_ENT WIFI_AUTH_WPA2_ENTERPRISE
#endif

#include <Ticker.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <algorithm>
#include <memory>

#define ARDUINOJSON_ENABLE_PROGMEM 1
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <vector>

namespace {

ESPReactWifiManager *instance = nullptr;

bool isConnecting = false;
bool fallbackToAp = true;

String connectSsid;
String connectPassword;
String connectLogin;
String connectBssid;

String connectApName;
String connectApPassword;

uint32_t shouldScan = 0;
uint32_t shouldConnect = 0;

uint32_t reconnectInterval = 60 * 1000;

DNSServer* dnsServer = nullptr;
void (*finishedCallback)(bool) = nullptr;
void (*notFoundCallback)(AsyncWebServerRequest*) = nullptr;
bool (*captiveCallback)(AsyncWebServerRequest*) = nullptr;

String wifiHostname;

std::vector<ESPReactWifiManager::WifiResult> wifiResults;
int wifiIndex = 0;

Ticker wifiReconnectTimer;

uint8_t retryCount = 0;
uint8_t retryLimit = 5;

#if defined(ESP8266)
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
#endif

const float wifiReconnectDelay = 5;

bool signalLess(const ESPReactWifiManager::WifiResult& a,
                const ESPReactWifiManager::WifiResult& b)
{
    return a.rssi > b.rssi;
}

bool ssidEqual(const ESPReactWifiManager::WifiResult& a,
               const ESPReactWifiManager::WifiResult& b)
{
    return a.ssid == b.ssid;
}

bool ssidLess(const ESPReactWifiManager::WifiResult& a,
              const ESPReactWifiManager::WifiResult& b)
{
    return a.ssid == b.ssid ? signalLess(a, b) : a.ssid < b.ssid;
}

int str2mac(const char* mac, uint8_t* values){
   if (6 == sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])) {
       return 1;
   } else {
       return 0;
   }
}

void notFoundHandler(AsyncWebServerRequest* request)
{
    if (request->url().endsWith(F(".map"))) {
        request->send(404);
        return;
    }

    Serial.print(F("Not found: "));
    Serial.println(request->url());

    Serial.print(F("Request: "));
    Serial.println(request->client()->localIP());

    Serial.print(F("Local: "));
    Serial.println(WiFi.localIP());

    bool isLocal = WiFi.localIP() == request->client()->localIP();

    if (!isLocal && captiveCallback && captiveCallback(request)) {
        return;
    }

    if (!isLocal) {
        Serial.print(F("Request redirected to captive portal: "));
        Serial.println(request->url());
        String redirect = String(F("http://"))
                + request->client()->localIP().toString()
                + String(F("/wifi.html"));
        Serial.print(F("To: "));
        Serial.println(redirect);

        request->redirect(redirect);
        return;
    }

    if (notFoundCallback) {
        notFoundCallback(request);
    }
}

void connectToWifi()
{
    if (!instance) {
        return;
    }

    instance->connect();
}

void setupAP() {
    bool success = WiFi.softAPConfig(
        IPAddress(8, 8, 8, 8),
        IPAddress(8, 8, 8, 8),
        IPAddress(255, 255, 255, 0));
    if (!success) {
        Serial.println(F("Error setting static IP for AP mode"));
        ESP.restart();
        return;
    }
}

void checkRetryCount() {
    if (isConnecting || WiFi.softAPgetStationNum() > 0) {
        return;
    }

    if (++retryCount <= retryLimit || !fallbackToAp) {
        wifiReconnectTimer.once(wifiReconnectDelay, connectToWifi);
    } else {
        shouldConnect = millis() + reconnectInterval;
        instance->startAP();
    }
}

#if defined(ESP32)
void WiFiEvent(WiFiEvent_t event) {
    Serial.print("[WiFi-event] event: ");
    switch(event) {

    case SYSTEM_EVENT_WIFI_READY:
        Serial.println("SYSTEM_EVENT_WIFI_READY");
        break;
    case SYSTEM_EVENT_SCAN_DONE:
        Serial.println("SYSTEM_EVENT_SCAN_DONE");
        break;
    case SYSTEM_EVENT_STA_START:
        Serial.println("SYSTEM_EVENT_STA_START");
        break;
    case SYSTEM_EVENT_STA_STOP:
        Serial.println("SYSTEM_EVENT_STA_STOP");
        break;
    case SYSTEM_EVENT_STA_CONNECTED:
        Serial.println("SYSTEM_EVENT_STA_CONNECTED");
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        Serial.println("SYSTEM_EVENT_STA_DISCONNECTED");
        checkRetryCount();
        break;
    case SYSTEM_EVENT_STA_AUTHMODE_CHANGE:
        Serial.println("SYSTEM_EVENT_STA_AUTHMODE_CHANGE");
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        Serial.println("SYSTEM_EVENT_STA_GOT_IP");
        instance->finishConnection(false);
        break;
    case SYSTEM_EVENT_STA_LOST_IP:
        Serial.println("SYSTEM_EVENT_STA_LOST_IP");
        break;
    case SYSTEM_EVENT_STA_WPS_ER_SUCCESS:
        Serial.println("SYSTEM_EVENT_STA_WPS_ER_SUCCESS");
        break;
    case SYSTEM_EVENT_STA_WPS_ER_FAILED:
        Serial.println("SYSTEM_EVENT_STA_WPS_ER_FAILED");
        break;
    case SYSTEM_EVENT_STA_WPS_ER_TIMEOUT:
        Serial.println("SYSTEM_EVENT_STA_WPS_ER_TIMEOUT");
        break;
    case SYSTEM_EVENT_STA_WPS_ER_PIN:
        Serial.println("SYSTEM_EVENT_STA_WPS_ER_PIN");
        break;
    case SYSTEM_EVENT_STA_WPS_ER_PBC_OVERLAP:
        Serial.println("SYSTEM_EVENT_STA_WPS_ER_PBC_OVERLAP");
        break;
    case SYSTEM_EVENT_AP_START:
        Serial.println("SYSTEM_EVENT_AP_START");
        break;
    case SYSTEM_EVENT_AP_STOP:
        Serial.println("SYSTEM_EVENT_AP_STOP");
        break;
    case SYSTEM_EVENT_AP_STACONNECTED:
        Serial.println("SYSTEM_EVENT_AP_STACONNECTED");
        instance->scheduleScan(200);
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        Serial.println("SYSTEM_EVENT_AP_STADISCONNECTED");
        break;
    case SYSTEM_EVENT_AP_STAIPASSIGNED:
        Serial.println("SYSTEM_EVENT_AP_STAIPASSIGNED");
        break;
    case SYSTEM_EVENT_AP_PROBEREQRECVED:
        Serial.println("SYSTEM_EVENT_AP_PROBEREQRECVED");
        break;
    case SYSTEM_EVENT_GOT_IP6:
        Serial.println("SYSTEM_EVENT_GOT_IP6");
        break;
    case SYSTEM_EVENT_ETH_START:
        Serial.println("SYSTEM_EVENT_ETH_START");
        break;
    case SYSTEM_EVENT_ETH_STOP:
        Serial.println("SYSTEM_EVENT_ETH_STOP");
        break;
    case SYSTEM_EVENT_ETH_CONNECTED:
        Serial.println("SYSTEM_EVENT_ETH_CONNECTED");
        break;
    case SYSTEM_EVENT_ETH_DISCONNECTED:
        Serial.println("SYSTEM_EVENT_ETH_DISCONNECTED");
        break;
    case SYSTEM_EVENT_ETH_GOT_IP:
        Serial.println("SYSTEM_EVENT_ETH_GOT_IP");
        break;
    case SYSTEM_EVENT_MAX:
        Serial.println("SYSTEM_EVENT_MAX");
        break;
    }
}
#else
void onWifiConnect(const WiFiEventStationModeGotIP& event) {
    Serial.println("Connected to Wi-Fi.");
    instance->finishConnection(false);
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
    Serial.println("Disconnected from Wi-Fi.");
    checkRetryCount();
}
#endif
}

ESPReactWifiManager::ESPReactWifiManager()
{
    instance = this;

#if defined(ESP8266)
    wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
    wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);
#else
    WiFi.onEvent(WiFiEvent);
#endif
}

void ESPReactWifiManager::loop()
{
    if (dnsServer) {
        dnsServer->processNextRequest();
    }

    uint32_t now = millis();

    if (shouldScan > 0 && now > shouldScan) {
        shouldScan = 0;

        scan();
    }

    if (WiFi.status() != WL_CONNECTED && now > shouldConnect && shouldConnect > 0) {
        shouldConnect = 0;
        connect();
    }
}

void ESPReactWifiManager::disconnect()
{
    WiFi.softAPdisconnect(true);
#if defined(ESP8266)
    //trying to fix connection in progress hanging
    ETS_UART_INTR_DISABLE();
    wifi_station_disconnect();
    ETS_UART_INTR_ENABLE();
#else
    WiFi.disconnect(false);
#endif
}

void ESPReactWifiManager::setApOptions(String apName, String apPassword)
{
    connectApName = apName;
    connectApPassword = apPassword;
}

void ESPReactWifiManager::setStaOptions(String ssid, String password, String login, String bssid)
{
    connectSsid = ssid;
    connectPassword = password;
    connectLogin = login;
    connectBssid = bssid;
}

bool ESPReactWifiManager::connect()
{
    Serial.println();

    isConnecting = true;
    disconnect();
    delay(1000);
    WiFi.mode(WIFI_STA);
    delay(1000);
    if (!wifiHostname.isEmpty()) {
#if defined(ESP8266)
        WiFi.hostname(wifiHostname.c_str());
#else
        WiFi.setHostname(wifiHostname.c_str());
#endif
    }

    if (connectSsid.length() == 0) {
        sta_config_t sta_conf;
#if defined(ESP32)
        wifi_config_t current_conf;
        esp_wifi_get_config(WIFI_IF_STA, &current_conf);
        sta_conf = current_conf.sta;
#else
        wifi_station_get_config_default(&sta_conf);
#endif
        connectSsid = String(reinterpret_cast<const char*>(sta_conf.ssid));

        if (connectSsid.length() == 0) {
            isConnecting = false;
            return false;
        }

        String savedPassword = String(reinterpret_cast<const char*>(sta_conf.password));

        if (savedPassword.startsWith(F("x:"))) {
            int passwordIndex = savedPassword.indexOf(F(":"), 2);
            connectPassword = savedPassword.substring(passwordIndex + 1);
            connectLogin = savedPassword.substring(2, passwordIndex);
        } else {
            connectPassword = savedPassword;
        }

        if (connectSsid.length() > 0) {
            Serial.println(F("Connecting to last saved network"));
        } else {
            Serial.println(F("No last saved network"));
            isConnecting = false;
            return false;
        }
    }

    String tempPassword = connectPassword;
    if (connectLogin.length() == 0) {
        Serial.print(F("Connecting to network: "));
        Serial.println(connectSsid);
        Serial.flush();
    } else {
        Serial.print(F("Connecting to secure network: "));
        Serial.println(connectSsid);
        Serial.flush();
        tempPassword = F("x:");
        tempPassword += connectLogin;
        tempPassword += F(":");
        tempPassword += connectPassword;
#if defined(ESP32)
        esp_wifi_sta_wpa2_ent_enable();
#else
        wifi_station_set_wpa2_enterprise_auth(1);
#endif
        esp_wifi_sta_wpa2_ent_set_identity((wifi_cred_t*)connectLogin.c_str(), connectLogin.length());
        esp_wifi_sta_wpa2_ent_set_username((wifi_cred_t*)connectLogin.c_str(), connectLogin.length());
        esp_wifi_sta_wpa2_ent_set_password((wifi_cred_t*)connectPassword.c_str(), connectPassword.length());
    }
    uint8_t mac[6] = { 0 };
    if (connectBssid.length() > 0 && str2mac(connectBssid.c_str(), mac)) {
        Serial.print(F("Pin to BSSID: "));
        Serial.println(connectBssid);
        WiFi.begin(connectSsid.c_str(), tempPassword.c_str(), 0, mac);
    } else {
        WiFi.begin(connectSsid.c_str(), tempPassword.c_str());
    }

    Serial.println(F("Finished connecting"));
    isConnecting = false;
    return true;
}

bool ESPReactWifiManager::autoConnect()
{
    return connect() || startAP();
}

void ESPReactWifiManager::setFallbackToAp(bool enable)
{
    fallbackToAp = enable;
}

bool ESPReactWifiManager::startAP()
{
    Serial.println();
    disconnect();

    bool success = WiFi.mode(WIFI_AP);
    if (!success) {
        Serial.println(F("Error changing mode to AP"));
        ESP.restart();
        return false;
    }
#if defined(ESP8266)
    setupAP();
#endif
    Serial.print(F("Starting AP: "));
    Serial.println(connectApName);
    success = WiFi.softAP(connectApName.c_str(), connectApPassword.c_str());
    if (success) {
#if defined(ESP32)
        delay(500);
        setupAP();
#endif
        instance->finishConnection(true);
    } else {
        WiFi.printDiag(Serial);
        Serial.print(F("Error starting AP: "));
        Serial.println(WiFi.status());
        ESP.restart();
        return false;
    }

    return true;
}


void ESPReactWifiManager::setupHandlers(AsyncWebServer *server)
{
    if (!server) {
        Serial.println(F("WebServer is null!"));
        return;
    }

    server->on(PSTR("/wifiSave"), HTTP_POST, [this](AsyncWebServerRequest* request) {
        Serial.println("wifiSave request");

        String login;
        String password;
        String ssid;

        String message;

        for (uint8_t i = 0; i < request->args(); i++) {
            if (request->argName(i) == F("login")) {
                login = request->arg(i);
            }
            if (request->argName(i) == F("password")) {
                password = request->arg(i);
            }
            if (request->argName(i) == F("ssid")) {
                ssid = request->arg(i);
            }
        }
        if (ssid.length() > 0) {
            message = F("Connect to: ");
            message += ssid;
            message += F(" after module reboot");

            setStaOptions(ssid, password, login);
            connect();
        } else {
            message = F("Wrong request. No ssid");
        }
        request->send(200, F("text/html"), message);
    });

    server->on(PSTR("/wifiList"), HTTP_GET, [](AsyncWebServerRequest* request) {
        wifiIndex = 0;
        Serial.printf_P(PSTR("wifiList count: %zu\n"), wifiResults.size());
        AsyncWebServerResponse* response = request->beginChunkedResponse(
            F("application/json"),
            [](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
                if (index == 0) {
                    buffer[0] = '[';
                    return 1;
                } else if (wifiIndex >= wifiResults.size()) {
                    return 0;
                } else {
                    String security;
                    if (wifiResults[wifiIndex].encryptionType == ENCRYPTION_NONE) {
                        security = F("none");
                    } else if (wifiResults[wifiIndex].encryptionType == ENCRYPTION_ENT) {
                        security = F("WPA2");
                    } else {
                        security = F("WEP");
                    }
                    const size_t capacity = JSON_OBJECT_SIZE(3) + 31 // fields length
                                            + security.length()
                                            + wifiResults[wifiIndex].ssid.length();
                    DynamicJsonDocument doc(capacity);
                    JsonObject obj = doc.to<JsonObject>();
                    obj[F("ssid")] = wifiResults[wifiIndex].ssid;
                    obj[F("signalStrength")] = wifiResults[wifiIndex].quality;
                    obj[F("security")] = security;
                    size_t len = serializeJson(doc, (char*)buffer, maxLen);
                    if ((wifiIndex + 1) == wifiResults.size()) {
                        buffer[len] = ']';
                    } else {
                        buffer[len] = ',';
                    }
                    ++len;
                    ++wifiIndex;
                    return len;
                }
            });
        request->send(response);
    });

    server->onNotFound(notFoundHandler);
}

void ESPReactWifiManager::onFinished(void (*func)(bool))
{
    finishedCallback = func;
}

void ESPReactWifiManager::onNotFound(void (*func)(AsyncWebServerRequest*))
{
    notFoundCallback = func;
}

void ESPReactWifiManager::onCaptiveRedirect(bool (*func)(AsyncWebServerRequest*))
{
    captiveCallback = func;
}

void ESPReactWifiManager::finishConnection(bool apMode)
{
    if (apMode) {
        Serial.println("AP started");
        Serial.print(F("AP IP address: "));
        Serial.println(WiFi.softAPIP());
    } else {
        Serial.println("Connected to Wi-Fi.");
        Serial.print(F("AP ssid: "));
        Serial.println(WiFi.SSID());
        Serial.print(F("AP bssid: "));
        Serial.println(WiFi.BSSIDstr());
        Serial.print(F("STA IP address: "));
        Serial.println(WiFi.localIP());
    }

    if (!dnsServer && apMode) {
        dnsServer = new DNSServer();
        dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
        bool dnsOk = dnsServer->start(53, F("*"), WiFi.softAPIP());
        Serial.printf_P(PSTR("Starting DNS server: %s\n"), dnsOk ? PSTR("success") : PSTR("fail"));
    } else if (dnsServer && !apMode) {
        Serial.println(F("Stopping DNS server"));
        dnsServer->stop();
        delete dnsServer;
        dnsServer = nullptr;
    }

#if defined(ESP8266)
    scheduleScan();
#endif

    if (finishedCallback) {
        finishedCallback(apMode);
    }
}

void ESPReactWifiManager::scheduleScan(int timeout)
{
    Serial.println(F("scheduleScan"));
    shouldScan = millis() + timeout;
}

bool ESPReactWifiManager::scan()
{
    wifi_ssid_count_t n = WiFi.scanNetworks();
    Serial.println(F("Scan done"));
    if (n == WIFI_SCAN_FAILED) {
        Serial.println(F("scanNetworks returned: WIFI_SCAN_FAILED!"));
        return false;
    } else if (n == WIFI_SCAN_RUNNING) {
        Serial.println(F("scanNetworks returned: WIFI_SCAN_RUNNING!"));
        return false;
    } else if (n < 0) {
        Serial.print(F("scanNetworks failed with unknown error code: "));
        Serial.println(n);
        return false;
    } else if (n == 0) {
        Serial.println(F("No networks found"));
        return false;
    } else {
        Serial.print(F("Found networks: "));
        Serial.println(n);
        wifiResults.clear();
        for (wifi_ssid_count_t i = 0; i < n; i++) {
            WifiResult result;
            bool res = WiFi.getNetworkInfo(i, result.ssid, result.encryptionType,
                result.rssi, result.bssid, result.channel
#if defined(ESP8266)
                ,
                result.isHidden
#endif
            );

            if (!res) {
                Serial.printf_P(PSTR("Error getNetworkInfo for %d\n"), i);
            } else {
                if (result.ssid.length() == 0) {
                    continue;
                }

                result.quality = 0;

                if (result.rssi <= -100) {
                    result.quality = 0;
                } else if (result.rssi >= -50) {
                    result.quality = 100;
                } else {
                    result.quality = 2 * (result.rssi + 100);
                }

                Serial.printf("index: %d\n", i);
                Serial.printf("ssid: %s\n", result.ssid.c_str());
                Serial.printf("bssid: %02X:%02X:%02X:%02X:%02X:%02X\n", result.bssid[0]
                                                          , result.bssid[1]
                                                          , result.bssid[2]
                                                          , result.bssid[3]
                                                          , result.bssid[4]
                                                          , result.bssid[5]);

                wifiResults.push_back(result);
            }
        }

        sort(wifiResults.begin(), wifiResults.end(), ssidLess);
        wifiResults.erase(unique(wifiResults.begin(), wifiResults.end(), ssidEqual), wifiResults.end());
        sort(wifiResults.begin(), wifiResults.end(), signalLess);

        return true;
    }
}

int ESPReactWifiManager::size()
{
    return wifiResults.size();
}

void ESPReactWifiManager::setHostname(String hostname)
{
    wifiHostname = hostname;
}

std::vector<ESPReactWifiManager::WifiResult> ESPReactWifiManager::results()
{
    return wifiResults;
}
