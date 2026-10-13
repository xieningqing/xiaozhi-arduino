#pragma once

#include<Arduino.h>
#include <ArduinoWebsockets.h>
class Protocol {
public:
   Protocol();
   // 用 OTA 返回的 url 和 token 建立连接
   bool connect(const String& url,const String& token,
                const String& deviceId,const String& clientId,int version);

    // 必须在loop中持续调用
    void poll();

    bool isConnected() const { return connected_; }

    // 服务端信息
    const String& sessionId() const { return sessionId_; }
    int serverSampleRate() const { return serverSampleRate_; }
    int serverFrameDuration() const { return serverFrameDuration_; }

    // 发送文本消息，返回是否成功
    bool sendText(const String& text);

private:
   void onMessage(websockets::WebsocketsMessage message);
   void onEvent(websockets::WebsocketsEvent event, String data);
   void sendHello();

   websockets::WebsocketsClient client_;

   bool connected_ = false;
   bool serverHelloReceived_ = false;

   String sessionId_;
   int serverSampleRate_ = 24000;
   int serverFrameDuration_ = 60; 

   String deviceId_;
   String clientId_;
   int version_ = 1;
};
