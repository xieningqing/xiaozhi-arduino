#include "Network.h"

#include "../core/Config.h"
#include "../core/Secrets.h"
#include "../core/Version.h"

#include<WiFi.h>
#include<WiFiClientSecure.h>
#include<HTTPClient.h>
#include<ArduinoJson.h>
#include<Preferences.h>
#include<DNSServer.h>
#include<WiFiServer.h>
#include<time.h>
#include<esp_system.h>

namespace {

constexpr const char* kConfigPortalFlagKey = "wifi_config";

}  // namespace

// 生成一个 v4 格式的 UUID 字符串。
// 用 esp_random() 取真随机数，避免不同设备撞号。
static String makeUuid() {
  uint8_t b[16];
  for (int i = 0; i < 16; i++) {
    b[i] = (uint8_t)esp_random();
  }

  // 按 UUID v4 规范设置版本位和变体位
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;

  char s[40];
  snprintf(s, sizeof(s),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return String(s);
}

// 读取设备身份。
// MAC 每次都从 WiFi 取；UUID 只在第一次生成，存进 NVS 后一直复用。
// 如果每次开机都换 UUID，服务器会把它当成新设备。
void XiaozhiNetwork::loadIdentity(){
    deviceId_ = WiFi.macAddress();
    Preferences prefs;
    // 打开 NVS 分区，命名空间为 "xiaozhi"，读写模式
    prefs.begin("xiaozhi", false);
    
    clientId_ = prefs.getString("uuid", "");
    if(clientId_.isEmpty()){
        clientId_ = makeUuid();
        prefs.putString("uuid", clientId_);
        Serial.println("generated new uuid");
    }

    prefs.end(); 

    Serial.print("device id: ");
    Serial.println(deviceId_);
    Serial.print("client id: ");
    Serial.println(clientId_);
}

bool XiaozhiNetwork::loadWifiCredentials(String& ssid, String& password) {
    // 优先读 NVS。没有保存过时，继续使用 Secrets.h 作为开发期默认值。
    Preferences prefs;
    if (!prefs.begin("wifi", true)) {
        return false;
    }

    ssid = prefs.getString("ssid", "");
    password = prefs.getString("password", "");
    prefs.end();

    if (!ssid.isEmpty()) {
        return true;
    }

    if (strlen(WIFI_SSID) > 0) {
        ssid = WIFI_SSID;
        password = WIFI_PASSWORD;
        Serial.println("using WiFi credentials from Secrets.h");
        return true;
    }

    return false;
}

bool XiaozhiNetwork::saveWifiCredentials(const String& ssid,
                                         const String& password) {
    if (ssid.isEmpty()) {
        return false;
    }

    Preferences prefs;
    if (!prefs.begin("wifi", false)) {
        return false;
    }

    // putString() 返回写入长度，空密码时可能返回 0；
    // 密码为空本身是合法的开放网络配置，不能据此判断保存失败。
    prefs.putString("ssid", ssid);
    prefs.putString("password", password);
    prefs.end();

    return true;
}

bool XiaozhiNetwork::consumeConfigPortalFlag() {
    Preferences prefs;
    if (!prefs.begin("wifi", false)) {
        return false;
    }

    const bool requested = prefs.getBool(kConfigPortalFlagKey, false);
    if (requested) {
        prefs.remove(kConfigPortalFlagKey);
    }
    prefs.end();
    return requested;
}

bool XiaozhiNetwork::requestConfigPortalOnNextBoot() {
    Preferences prefs;
    if (!prefs.begin("wifi", false)) {
        return false;
    }

    const bool saved = prefs.putBool(kConfigPortalFlagKey, true) > 0;
    prefs.end();
    return saved;
}

bool XiaozhiNetwork::tryConnectWifi(const String& ssid,
                                    const String& password) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    Serial.print("connecting wifi");
    const uint32_t startMs = millis();

    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startMs > config::kWifiTimeoutMs) {
            Serial.println();
            Serial.println("wifi timeout");
            WiFi.disconnect(true);
            return false;
        }
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.print("ip: ");
    Serial.println(WiFi.localIP());

    // 刚连上时射频还在稳定过程中，立即改省电模式容易让
    // 紧接着的 TLS 握手失败（表现为 http code: -11）。
    delay(200);

    // 关闭 WiFi 省电（modem sleep）。
    // 小智官方固件在音频通道工作时会把 WiFi 功耗等级设为 PERFORMANCE，
    // Arduino 的 STA 默认开启 modem sleep，会让音频和网络时序更不稳定。
    WiFi.setSleep(false);
    Serial.println("wifi power save: off");
    return true;
}

String XiaozhiNetwork::makeConfigApSsid() const {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    const String suffix = mac.length() >= 4 ? mac.substring(mac.length() - 4) : "0000";
    return String(config::kWifiConfigApPrefix) + suffix;
}

String XiaozhiNetwork::escapeHtml(const String& text) const {
    String escaped = text;
    escaped.replace("&", "&amp;");
    escaped.replace("<", "&lt;");
    escaped.replace(">", "&gt;");
    escaped.replace("\"", "&quot;");
    escaped.replace("'", "&#39;");
    return escaped;
}

String XiaozhiNetwork::urlDecode(const String& text) const {
    String decoded;
    decoded.reserve(text.length());

    for (size_t index = 0; index < text.length(); ++index) {
        const char character = text[index];
        if (character == '+') {
            decoded += ' ';
        } else if (character == '%' && index + 2 < text.length()) {
            char hex[3] = {text[index + 1], text[index + 2], '\0'};
            char* end = nullptr;
            const long value = strtol(hex, &end, 16);
            if (end == hex + 2) {
                decoded += static_cast<char>(value);
                index += 2;
            } else {
                decoded += character;
            }
        } else {
            decoded += character;
        }
    }

    return decoded;
}

void XiaozhiNetwork::sendConfigRoot() {
    String savedSsid;
    String ignoredPassword;
    loadWifiCredentials(savedSsid, ignoredPassword);

    const String page =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<title>小智配网</title>"
          "<style>body{font-family:system-ui,sans-serif;margin:0;background:#f4f5f7;color:#202124}"
          "main{max-width:420px;margin:12vh auto;background:#fff;padding:28px;border-radius:10px;"
          "box-shadow:0 6px 24px #0002}h1{font-size:24px;margin:0 0 8px}p{color:#5f6368;line-height:1.5}"
          "label{display:block;margin:18px 0 6px;font-weight:600}input{width:100%;box-sizing:border-box;"
          "font-size:16px;padding:12px;border:1px solid #c7c9cc;border-radius:6px}"
          "button{width:100%;margin-top:24px;padding:13px;border:0;border-radius:6px;background:#1769aa;"
          "color:#fff;font-size:16px;font-weight:600}small{display:block;margin-top:14px;color:#777}</style>"
          "</head><body><main><h1>小智配网</h1>"
          "<p>连接家庭 WiFi 后，设备会自动重启并连接小智服务。</p>"
          "<form method=\"post\" action=\"/save\">"
          "<label>WiFi 名称</label><input name=\"ssid\" maxlength=\"32\" required value=\"" +
        escapeHtml(savedSsid) +
        "\"><label>WiFi 密码</label><input name=\"password\" type=\"password\" maxlength=\"64\">"
          "<button type=\"submit\">保存并重启</button></form>"
          "<small>配置热点仅用于设置，保存成功后会自动关闭。</small>"
          "</main></body></html>";

    configClient_.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"));
    configClient_.print(F("Connection: close\r\nCache-Control: no-store\r\n\r\n"));
    configClient_.print(page);
}

void XiaozhiNetwork::saveAndRestart(const String& requestBody) {
    String ssid;
    String password;

    const int ssidStart = requestBody.indexOf("ssid=");
    const int passwordStart = requestBody.indexOf("password=");

    if (ssidStart >= 0) {
        const int valueStart = ssidStart + 5;
        const int valueEnd = requestBody.indexOf('&', valueStart);
        ssid = urlDecode(requestBody.substring(
            valueStart, valueEnd < 0 ? requestBody.length() : valueEnd));
    }

    if (passwordStart >= 0) {
        const int valueStart = passwordStart + 9;
        const int valueEnd = requestBody.indexOf('&', valueStart);
        password = urlDecode(requestBody.substring(
            valueStart, valueEnd < 0 ? requestBody.length() : valueEnd));
    }

    if (ssid.isEmpty()) {
        configClient_.print(
            F("HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\n"
              "Connection: close\r\n\r\nmissing ssid"));
        return;
    }

    if (!saveWifiCredentials(ssid, password)) {
        configClient_.print(
            F("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain; charset=utf-8\r\n"
              "Connection: close\r\n\r\nsave failed"));
        return;
    }

    configClient_.print(
        F("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
          "Connection: close\r\n\r\n"
          "<!doctype html><html><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<title>保存成功</title></head><body style=\"font-family:system-ui,sans-serif;"
          "text-align:center;padding-top:20vh\"><h2>保存成功</h2>"
          "<p>设备正在重启...</p></body></html>"));

    delay(800);
    ESP.restart();
}

void XiaozhiNetwork::sendCaptivePortalRedirect() {
    configClient_.print(
        F("HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/\r\n"
          "Connection: close\r\nContent-Length: 0\r\n\r\n"));
}

void XiaozhiNetwork::handleConfigClient() {
    if (!configClient_.connected()) {
        return;
    }

    const String requestLine = configClient_.readStringUntil('\n');
    if (requestLine.isEmpty()) {
        return;
    }

    int contentLength = 0;
    for (int headerCount = 0; headerCount < 32; ++headerCount) {
        String headerLine = configClient_.readStringUntil('\n');
        headerLine.trim();
        if (headerLine.isEmpty()) {
            break;
        }
        if (headerLine.startsWith("Content-Length:")) {
            contentLength = headerLine.substring(15).toInt();
        }
    }

    String requestBody;
    if (contentLength > 0) {
        requestBody.reserve(contentLength);
        const uint32_t startMs = millis();
        while (configClient_.available() < contentLength &&
               millis() - startMs < 2000) {
            delay(2);
        }
        for (int index = 0; index < contentLength && configClient_.available();
             ++index) {
            requestBody += static_cast<char>(configClient_.read());
        }
    }

    const bool isCaptivePortalCheck =
        requestLine.indexOf("/generate_204") >= 0 ||
        requestLine.indexOf("/gen_204") >= 0 ||
        requestLine.indexOf("/hotspot-detect.html") >= 0 ||
        requestLine.indexOf("/connecttest.txt") >= 0 ||
        requestLine.indexOf("/redirect") >= 0;

    if (isCaptivePortalCheck) {
        sendCaptivePortalRedirect();
        configClient_.stop();
        return;
    }

    if (requestLine.startsWith("POST /save")) {
        saveAndRestart(requestBody);
        configClient_.stop();
        return;
    }

    sendConfigRoot();
    configClient_.stop();
}

bool XiaozhiNetwork::runConfigPortal(const WifiStatusCallback& statusCallback) {
    WiFi.mode(WIFI_AP);
    String apSsid = makeConfigApSsid();
    if (!WiFi.softAP(apSsid.c_str())) {
        Serial.println("wifi config AP start failed");
        return false;
    }

    const IPAddress apIp = WiFi.softAPIP();
    configWebServer_.begin(80);
    configDnsServer_.start(53, "*", apIp);
    configPortalRunning_ = true;

    Serial.println();
    Serial.print("wifi config AP: ");
    Serial.println(apSsid);
    Serial.print("open http://");
    Serial.println(apIp);

    if (statusCallback) {
        statusCallback("WIFI SETUP");
    }

    while (configPortalRunning_) {
        configDnsServer_.processNextRequest();
        WiFiClient incomingClient = configWebServer_.available();
        if (incomingClient) {
            configClient_ = incomingClient;
            handleConfigClient();
        }
        delay(2);
    }
    return true;
}

bool XiaozhiNetwork::connectWifi(const WifiStatusCallback& statusCallback) {
    const bool forceConfigPortal = consumeConfigPortalFlag();
    String ssid;
    String password;
    const bool hasCredentials = loadWifiCredentials(ssid, password);

    if (forceConfigPortal || !hasCredentials) {
        Serial.println(forceConfigPortal ? "forced wifi configuration"
                                         : "no saved wifi credentials");
        return runConfigPortal(statusCallback);
    }

    if (tryConnectWifi(ssid, password)) {
        return true;
    }

    Serial.println("wifi connect failed, starting config portal");
    return runConfigPortal(statusCallback);
}

bool XiaozhiNetwork::syncTime(){
    // TLS 证书校验需要正确的系统时间。
   // 现在虽然跳过校验，但后面连 wss 时必须要对，所以先同步好。
    configTime(8*3600,0,config::kNtpServer);

    Serial.print("syncing time");
    const uint32_t startMs = millis();
    time_t now;
    while(millis() - startMs < 10000){
        time(&now);
        if (now > 1700000000) {   // 2023-11-15 之后都算同步成功
          Serial.println();
          Serial.print("time: ");
          Serial.println(ctime(&now));
          return true;
        }
        delay(500);
        Serial.print(".");
    }


  Serial.println();
  Serial.println("ntp timeout");
  return false;
}

bool XiaozhiNetwork::fetchOtaConfig(){
    if(deviceId_.isEmpty()){
        loadIdentity();
    }

    // 用 WiFiClientSecure 显式创建 TLS 连接。
    // setInsecure() 跳过证书校验，先用它把流程跑通，
    // 后面稳定了再换成带 CA 证书的写法。
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if(!http.begin(client, config::kOtaUrl)){
        Serial.println("http begin failed");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Activation-Version", "1");
    http.addHeader("Client-Id", clientId_);
    http.addHeader("Device-Id", deviceId_);
    http.addHeader("User-Agent", config::kFirmwareName);

    // 服务器通过这些信息识别设备类型
    JsonDocument doc;

    // 字段名和结构必须和官方固件的 GetSystemInfoJson() 对齐，
    // 否则服务器会直接返回 400。
    doc["version"] = 2;
    doc["language"] = "zh-CN";
    doc["flash_size"] = 16777216;
    doc["mac_address"] = deviceId_;
    doc["uuid"] = clientId_;
    doc["chip_model_name"] = "esp32s3";

    // ESP32-S3 的芯片信息
    doc["chip_info"]["model"] = 9;
    doc["chip_info"]["cores"] = 2;
    doc["chip_info"]["revision"] = 0;
    doc["chip_info"]["features"] = 0;

    doc["application"]["name"] = XIAOZHI_PROJECT_NAME;
    doc["application"]["version"] = XIAOZHI_PROJECT_VERSION;
    doc["application"]["compile_time"] = "";
    doc["application"]["idf_version"] = esp_get_idf_version();
    doc["application"]["elf_sha256"] = "";

    // 服务器会校验 board.type，缺失时会返回
    // "Invalid board type or app version"。
    // bread-compact-wifi-lcd 是这块板对应的官方板型字符串。
    doc["board"]["type"] = "bread-compact-wifi-lcd";
    doc["board"]["name"] = "bread-compact-wifi-lcd";
    doc["board"]["ssid"] = WiFi.SSID();
    doc["board"]["rssi"] = WiFi.RSSI();
    doc["board"]["channel"] = WiFi.channel();
    doc["board"]["ip"] = WiFi.localIP().toString();
    doc["board"]["mac"] = deviceId_;

    // 屏幕信息，帮助服务器判断设备的显示能力
    doc["display"]["monochrome"] = false;
    doc["display"]["width"] = 240;
    doc["display"]["height"] = 240;

    // 当前运行的分区标签
    doc["ota"]["label"] = "ota_0";

    String body;
    serializeJson(doc, body);
    Serial.print("ota request: ");
    Serial.println(body);

    // 偶发的 TLS 握手失败（http code -11）很常见，
    // 这里最多重试 3 次，每次间隔 2 秒，避免一次失败就直接放弃。
    int code = 0;
    String responseBody;

    for (int attempt = 1; attempt <= 3; ++attempt) {
        code = http.POST(body);

        Serial.print("http code: ");
        Serial.println(code);

        if (code == 200) {
            responseBody = http.getString();
            break;
        }

        // 失败时把服务器响应体打印出来，通常会说明哪些字段不合法。
        const String errorBody = http.getString();
        if (!errorBody.isEmpty()) {
            Serial.print("ota response: ");
            Serial.println(errorBody);
        }

        if (attempt < 3) {
            Serial.print("ota attempt ");
            Serial.print(attempt);
            Serial.println(" failed, retrying...");
            delay(2000);
        }
    }

    http.end();

    if (code != 200) {
        Serial.println("ota failed after retries");
        return false;
    }

    const String resp = responseBody;

    JsonDocument respDoc;
    const DeserializationError err = deserializeJson(respDoc, resp);
    if(err){
        Serial.print("json parse failed: ");
        Serial.println(err.c_str());
        return false;
    }

    // ---------------- 解析 MQTT 配置 ----------------
    // 官方固件在 application.cc 里优先选 MQTT：
    //   if (ota.HasMqttConfig()) { 用 MqttProtocol }
    //   else if (ota.HasWebsocketConfig()) { 用 WebsocketProtocol }
    // 这台设备在平台上被登记为"开源版"接入，所以要按 MQTT 走。
    JsonObject mqttObj = respDoc["mqtt"].as<JsonObject>();
    if(!mqttObj.isNull()){
        mqttEndpoint_     = mqttObj["endpoint"]        | "";
        mqttClientId_     = mqttObj["client_id"]       | "";
        mqttUsername_     = mqttObj["username"]        | "";
        mqttPassword_     = mqttObj["password"]        | "";
        mqttPublishTopic_ = mqttObj["publish_topic"]   | "";

        Serial.print("mqtt endpoint: ");
        Serial.println(mqttEndpoint_);
        Serial.print("mqtt publish topic: ");
        Serial.println(mqttPublishTopic_);
    }else{
        Serial.println("no mqtt config");
    }

    // ---------------- 解析 WebSocket 配置 ----------------
    // 保留解析，但目前服务器只给占位 token，实际不会走这条路。
    JsonObject wsObj = respDoc["websocket"].as<JsonObject>();
    if(!wsObj.isNull()){
        wsUrl_     = wsObj["url"]     | "";
        wsToken_   = wsObj["token"]   | "";
        wsVersion_ = wsObj["version"] | 1;
    }

    // 只要拿到可用的 MQTT 配置就算成功
    return hasMqttConfig();
}
