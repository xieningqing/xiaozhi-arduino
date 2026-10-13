#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <functional>

using WifiStatusCallback = std::function<void(const String&)>;

// 不能直接叫 Network：ESP32 Core 3.3.12 自带 libraries/Network，
// 会和核心的类型撞名，编译时提示"Network 不是类型名"。
// 加上项目前缀避免冲突。
class XiaozhiNetwork {
public:
    // Connect to the saved WiFi network.
    // If no valid credentials exist or provisioning is forced, start the AP portal.
    bool connectWifi(const WifiStatusCallback& statusCallback = nullptr);

    // Mark the next boot as a provisioning boot and restart from the caller.
    bool requestConfigPortalOnNextBoot();
   
    // 同步时间
    bool syncTime();

    //请求小智ota
    bool fetchOtaConfig();

    // ota返回websocket配置
    const String& wsUrl() const { return wsUrl_; }
    const String& wsToken() const { return wsToken_; }
    int wsVersion() const { return wsVersion_; }

    // ota返回的mqtt配置
    // 这台设备在平台上被登记为"开源版"接入，
    // 服务器下发的是可用的 MQTT 凭据，WebSocket 只是占位。
    const String& mqttEndpoint() const { return mqttEndpoint_; }
    const String& mqttClientId() const { return mqttClientId_; }
    const String& mqttUsername() const { return mqttUsername_; }
    const String& mqttPassword() const { return mqttPassword_; }
    const String& mqttPublishTopic() const { return mqttPublishTopic_; }
    bool hasMqttConfig() const { return !mqttEndpoint_.isEmpty(); }

    const String& clientId() const { return clientId_; }
    const String& deviceId() const { return deviceId_; }

private:
    String wsUrl_; // websocket url
    String deviceId_; // 设备id
    String clientId_; // 客户端id
    String wsToken_; // websocket token
    int wsVersion_ = 1; // websocket版本

    String mqttEndpoint_;     // MQTT broker 地址
    String mqttClientId_;     // MQTT 客户端 ID
    String mqttUsername_;     // MQTT 用户名
    String mqttPassword_;     // MQTT 密码
    String mqttPublishTopic_; // 发布主题

    DNSServer configDnsServer_;
    WiFiServer configWebServer_;
    WiFiClient configClient_;
    bool configPortalRunning_ = false;

    void loadIdentity(); // 生成uuid并保存到NVS
    bool loadWifiCredentials(String& ssid, String& password);
    bool saveWifiCredentials(const String& ssid, const String& password);
    bool consumeConfigPortalFlag();
    bool tryConnectWifi(const String& ssid, const String& password);
    bool runConfigPortal(const WifiStatusCallback& statusCallback);
    void handleConfigClient();
    void sendConfigRoot();
    void saveAndRestart(const String& requestBody);
    void sendCaptivePortalRedirect();
    String urlDecode(const String& text) const;
    String makeConfigApSsid() const;
    String escapeHtml(const String& text) const;
};
