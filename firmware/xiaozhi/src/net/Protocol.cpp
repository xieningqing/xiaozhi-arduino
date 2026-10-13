#include "Protocol.h"
#include "../core/Config.h"

#include <ArduinoJson.h>

Protocol::Protocol(){}

// 解析 wss://host/path，拆出主机名和路径。
// 这个库的 connectSecure 需要分开传 host 和 path。
static bool splitUrl(const String& url,String& host,String& path){
    String rest = url;
    
    // 去掉协议头
    if(rest.startsWith("wss://")){
        rest = rest.substring(6);
    }else if(rest.startsWith("ws://")){
        rest = rest.substring(5);
    }else{
        return false;
    }

    // 找到第一个 /，前面是 host，后面是 path
    const int slash = rest.indexOf('/');
    if(slash < 0){
        host = rest;
        path = "/";
    }else{
        host = rest.substring(0,slash);
        path = rest.substring(slash);
    }

    // 去掉端口号
    const int colon = host.indexOf(':');
    if(colon >= 0){
        host = host.substring(0,colon);
    }

    return !host.isEmpty();
}


bool Protocol::connect(const String& url,const String& token,
                     const String& deviceId,const String& clientId,int version){
    
    deviceId_ = deviceId;
    clientId_ = clientId;
    version_ = version;

    String host,path;
    if(!splitUrl(url,host,path)){
        Serial.println("invalid url");
        return false;
    }

    // 跳过 TLS 证书校验，先用它把流程跑通。
    // 测通后可以换成 setCACert()。
    client_.setInsecure();

    // 设置 token 作为自定义 header
    // Bearer 和 token 之间必须有一个空格，否则服务器认为认证格式非法
    client_.addHeader("Authorization", "Bearer " + token);
    client_.addHeader("Protocol-Version", String(version_));
    client_.addHeader("Device-Id", deviceId_);
    client_.addHeader("Client-Id", clientId_);

    client_.onMessage([this](websockets::WebsocketsMessage message){
        this->onMessage(message);
    });
    client_.onEvent([this](websockets::WebsocketsEvent event, String data){
        this->onEvent(event,data);
    });

    Serial.print("connecting to ");
    Serial.print(host);
    Serial.println();
    Serial.println("ws: tls connect starting");

    // 443是默认端口wss
    const bool ok = client_.connectSecure(host.c_str(),443,path.c_str());
    Serial.println("ws: tls connect returned");
    if(!ok){
        Serial.println("connect failed");
        return false;
    }

    connected_ = true;
    Serial.println("connect ok");
    return true;
}

// 必须在 loop 中持续调用，处理网络事件
// 轮询 + 回调
void Protocol::poll(){
    client_.poll();
}

void Protocol::onEvent(websockets::WebsocketsEvent event,String data){
    if(event == websockets::WebsocketsEvent::ConnectionOpened){
        Serial.println("ws opened");

        sendHello();
    }else if(event == websockets::WebsocketsEvent::ConnectionClosed){
        Serial.println("ws closed");
        connected_ = false;
    }else if(event == websockets::WebsocketsEvent::GotPing){
        Serial.println("ws ping");
    }
}

void Protocol::onMessage(websockets::WebsocketsMessage msg){
    if(msg.isBinary()){
        Serial.print("ws binary frame: ");
        Serial.println(msg.length());
        return ;
    }

    const String text = msg.data();
    Serial.print("ws text: ");
    Serial.println(text);

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc,text);
    if(err){
        Serial.print("json parse failed: ");
        Serial.println(err.c_str());
        return;
    }

    const char* type = doc["type"] | "";
    if(strcmp(type,"hello") == 0){
        sessionId_ = doc["session_id"] | "";

        JsonObject audio = doc["audio_params"].as<JsonObject>();
        if(!audio.isNull()){
            serverSampleRate_ = audio["sample_rate"] | 24000;
            serverFrameDuration_ = audio["frame_duration"] | 60;
        }

        serverHelloReceived_ = true;

        Serial.print("session id: ");
        Serial.println(sessionId_);
        Serial.print("server audio: ");
        Serial.print(serverSampleRate_);
        Serial.print(" Hz, ");
        Serial.print(serverFrameDuration_);
        Serial.println(" ms");
    }
}

void Protocol::sendHello() {
    // hello 描述客户端能力，服务器据此决定音频参数
    JsonDocument doc;
    doc["type"] = "hello";
    doc["version"] = version_;
    doc["transport"] = "websocket";
    doc["features"]["mcp"] = true;
    doc["audio_params"]["format"] = "opus";
    doc["audio_params"]["sample_rate"] = 16000;
    doc["audio_params"]["channels"] = 1;
    doc["audio_params"]["frame_duration"] = 60;
  
    String out;
    serializeJson(doc, out);
  
    Serial.print("send hello: ");
    Serial.println(out);
    client_.send(out);
  }
  
  bool Protocol::sendText(const String& text) {
    return client_.send(text);
  }
