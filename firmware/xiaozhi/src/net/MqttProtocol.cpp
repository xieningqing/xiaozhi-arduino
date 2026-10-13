#include "MqttProtocol.h"

#include <ArduinoJson.h>
#include <aes/esp_aes.h>
#include <esp_crt_bundle.h>
#include <lwip/inet.h>

#include <cstring>

#include "../core/Config.h"

namespace {

// 把一个十六进制字符转换成 0 到 15 的数值。
// 返回 -1 表示该字符不是合法的十六进制字符。
int hexCharacterToValue(char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }

  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }

  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }

  return -1;
}

}  // namespace

// MQTT 事件回调。
// esp_mqtt_client 是异步的：连接、收到消息、断开都通过事件通知，
// 所以要把回调转成成员函数调用。
void MqttProtocol::eventHandler(void* handlerArgs, esp_event_base_t base,
                                int32_t eventId, void* eventData) {
  (void)base;

  auto* self = static_cast<MqttProtocol*>(handlerArgs);
  self->onEvent(eventId, eventData);
}

void MqttProtocol::onEvent(int32_t eventId, void* eventData) {
  auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);

  switch (eventId) {
    case MQTT_EVENT_CONNECTED: {
      Serial.println("mqtt connected");

      connected_ = true;
      serverHelloReceived_ = false;

      // 创建一次消息队列。MQTT 任务只负责入队，主循环再解析。
      if (incomingMessageQueue_ == nullptr) {
        incomingMessageQueue_ =
            xQueueCreate(8, sizeof(PendingControlMessage));
      }

      if (publishTopic_.isEmpty()) {
        Serial.println("mqtt: publish topic is empty");
        break;
      }

      // 必须先订阅，服务器回复的消息才会进入 MQTT_EVENT_DATA。
      const int subscribeMessageId = esp_mqtt_client_subscribe(
          client_, publishTopic_.c_str(), 0);

      if (subscribeMessageId < 0) {
        Serial.println("mqtt: subscribe failed");
        break;
      }

      Serial.print("mqtt: subscribed, message id = ");
      Serial.println(subscribeMessageId);

      // 这里不发送 hello。
      // 由 openAudioChannel() 决定什么时候申请音频通道。
      break;
    }

    case MQTT_EVENT_DISCONNECTED:
      Serial.println("mqtt disconnected");

      connected_ = false;
      serverHelloReceived_ = false;

      // 这里运行在 MQTT 任务中，不能直接停止 UDP。
      // 主循环可能正在 pollAudioChannel() 里使用同一个 WiFiUDP 对象，
      // 跨任务操作会写坏接收缓冲区。只置标志，交由主循环处理。
      pendingAudioChannelClose_ = true;
      break;

    case MQTT_EVENT_DATA: {
      // 收到的消息可能是分片的，只有收齐了才处理。
      if (event->data_len != event->total_data_len) {
        break;
      }

      // 关键：MQTT 任务运行在核心 0，栈空间有限。
      // 这里不构造 String、不解析 JSON、也不打印串口，
      // 只把原始字节复制进队列，由主循环任务取出后再处理。
      // 在核心 0 做堆分配或串口输出都可能破坏共享内存。
      if (incomingMessageQueue_ != nullptr) {
        size_t copyLength = static_cast<size_t>(event->data_len);
        if (copyLength >= sizeof(mqttTaskStagingMessage_.payload)) {
          copyLength = sizeof(mqttTaskStagingMessage_.payload) - 1;
        }

        memcpy(mqttTaskStagingMessage_.payload, event->data, copyLength);
        mqttTaskStagingMessage_.payload[copyLength] = '\0';
        mqttTaskStagingMessage_.length =
            static_cast<uint16_t>(copyLength);

        // 队列满时丢弃最旧的一条，保证实时性。
        if (xQueueSend(incomingMessageQueue_,
                       &mqttTaskStagingMessage_, 0) != pdTRUE) {
          xQueueReceive(incomingMessageQueue_,
                        &mqttTaskDiscardMessage_, 0);
          xQueueSend(incomingMessageQueue_,
                     &mqttTaskStagingMessage_, 0);
        }
      }
      break;
    }

    case MQTT_EVENT_ERROR: {
      // 出错时打印 TLS 层和 socket 层的错误码，方便定位。
      Serial.print("mqtt error, type: ");
      Serial.println(static_cast<int>(event->error_handle->error_type));

      if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        Serial.print("  tls err: ");
        Serial.println(
            static_cast<int>(event->error_handle->esp_tls_last_esp_err));

        Serial.print("  socket errno: ");
        Serial.println(event->error_handle->esp_transport_sock_errno);
      }
      break;
    }

    default:
      break;
  }
}

bool MqttProtocol::connect(const String& endpoint,
                           const String& clientId,
                           const String& username,
                           const String& password,
                           const String& publishTopic) {
  // 重连时先销毁旧客户端，否则 esp-mqtt 会残留任务、socket 和事件注册。
  if (client_ != nullptr) {
    esp_mqtt_client_stop(client_);
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
  }

  connected_ = false;
  serverHelloReceived_ = false;
  pendingAudioChannelClose_ = false;
  closeAudioChannel(false);
  if (incomingMessageQueue_ != nullptr) {
    xQueueReset(incomingMessageQueue_);
  }

  publishTopic_ = publishTopic;

  // endpoint 可能带端口，形如 "host:8883"。
  String hostText = endpoint;
  int port = 8883;

  const int colon = endpoint.indexOf(':');
  if (colon >= 0) {
    hostText = endpoint.substring(0, colon);
    port = endpoint.substring(colon + 1).toInt();
  }

  // esp-mqtt 不复制这些字符串，只保存指针。
  // 必须存进成员变量，保证它们活到 MQTT 客户端销毁，
  // 否则客户端在连接后读到的是已释放的堆内存，会破坏堆。
  brokerHostStorage_ = hostText;
  brokerClientIdStorage_ = clientId;
  brokerUsernameStorage_ = username;
  brokerPasswordStorage_ = password;

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.hostname = brokerHostStorage_.c_str();
  cfg.broker.address.port = static_cast<uint32_t>(port);

  // 8883 走 TLS，用核心自带的证书包做校验，不用自己准备证书文件。
  if (port == 8883) {
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  } else {
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  }

  cfg.credentials.client_id = brokerClientIdStorage_.c_str();
  cfg.credentials.username = brokerUsernameStorage_.c_str();
  cfg.credentials.authentication.password =
      brokerPasswordStorage_.c_str();
  cfg.session.keepalive = 240;

  client_ = esp_mqtt_client_init(&cfg);
  if (client_ == nullptr) {
    Serial.println("mqtt init failed");
    return false;
  }

  const esp_err_t registerResult = esp_mqtt_client_register_event(
      client_, MQTT_EVENT_ANY, &MqttProtocol::eventHandler, this);

  if (registerResult != ESP_OK) {
    Serial.println("mqtt event register failed");

    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
    return false;
  }

  Serial.println("mqtt starting...");

  if (esp_mqtt_client_start(client_) != ESP_OK) {
    Serial.println("mqtt start failed");

    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
    return false;
  }

  // 等待连接结果。
  // 这里阻塞等待是可以接受的：连上之前后面的流程都做不了。
  const uint32_t startMs = millis();

  while (!connected_ && millis() - startMs < 15000) {
    delay(100);
  }

  if (!connected_) {
    Serial.println("mqtt connect timeout");
    return false;
  }

  return true;
}

void MqttProtocol::sendHello() {
  if (!connected_) {
    Serial.println("mqtt: cannot send hello, not connected");
    return;
  }

  if (publishTopic_.isEmpty()) {
    Serial.println("mqtt: publish topic is empty");
    return;
  }

  // MQTT 通道的 hello 和 WebSocket 不同：
  // transport 是 "udp"，version 是 3。
  JsonDocument doc;
  doc["type"] = "hello";
  doc["version"] = 3;
  doc["transport"] = "udp";
  // 当前固件没有实现 MCP 工具调用，不能声明 mcp=true，
  // 否则服务端会下发无法处理的工具消息。
  doc["features"]["mcp"] = false;
  // 全双工模式依赖服务端 AEC。设备端没有本地 AEC，
  // 因此声明 aec=true，让服务端启用回声消除，避免喇叭声被重新识别。
  doc["features"]["aec"] = true;
  doc["audio_params"]["format"] = "opus";
  doc["audio_params"]["sample_rate"] = 16000;
  doc["audio_params"]["channels"] = 1;
  doc["audio_params"]["frame_duration"] = 60;

  String out;
  serializeJson(doc, out);

  Serial.print("send hello: ");
  Serial.println(out);

  const int msgId = esp_mqtt_client_publish(client_, publishTopic_.c_str(),
                                            out.c_str(), out.length(), 0, 0);
  if (msgId < 0) {
    Serial.println("mqtt publish failed");
  }
}

bool MqttProtocol::sendText(const String& text) {
  if (!connected_ || publishTopic_.isEmpty()) {
    return false;
  }

  return esp_mqtt_client_publish(client_, publishTopic_.c_str(),
                                 text.c_str(), text.length(), 0, 0) >= 0;
}

void MqttProtocol::parseServerHello(const String& payload) {
  JsonDocument doc;

  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("mqtt json parse failed: ");
    Serial.println(err.c_str());
    return;
  }

  const char* type = doc["type"] | "";
  if (strcmp(type, "hello") != 0) {
    return;
  }

  // 服务器用 transport 字段说明音频通道类型。
  const char* transport = doc["transport"] | "";
  if (strcmp(transport, "udp") != 0) {
    Serial.print("mqtt: unexpected transport: ");
    Serial.println(transport);
    return;
  }

  const char* receivedSessionId = doc["session_id"] | "";
  if (receivedSessionId == nullptr || receivedSessionId[0] == '\0') {
    Serial.println("mqtt: session id is missing");
    return;
  }

  sessionId_ = receivedSessionId;
  lastIncomingDataMs_ = millis();

  JsonObject audio = doc["audio_params"].as<JsonObject>();
  if (!audio.isNull()) {
    serverSampleRate_ = audio["sample_rate"] | 24000;
    serverFrameDuration_ = audio["frame_duration"] | 60;
  }

  // UDP 通道参数。
  JsonObject udp = doc["udp"].as<JsonObject>();
  if (udp.isNull()) {
    Serial.println("mqtt: udp configuration is missing");
    return;
  }

  const char* receivedUdpServer = udp["server"] | "";
  const int receivedUdpPort = udp["port"] | 0;
  const char* receivedKey = udp["key"] | "";
  const char* receivedNonce = udp["nonce"] | "";

  if (receivedUdpServer == nullptr || receivedUdpServer[0] == '\0') {
    Serial.println("mqtt: udp server is missing");
    return;
  }

  if (receivedUdpPort <= 0 || receivedUdpPort > 65535) {
    Serial.println("mqtt: udp port is invalid");
    return;
  }

  if (!decodeHexString(receivedKey, aesEncryptionKey_,
                       sizeof(aesEncryptionKey_))) {
    Serial.println("mqtt: aes key is invalid");
    return;
  }

  if (!decodeHexString(receivedNonce, aesEncryptionNonce_,
                       sizeof(aesEncryptionNonce_))) {
    Serial.println("mqtt: aes nonce is invalid");
    return;
  }

  udpServerAddress_ = receivedUdpServer;
  udpServerPort_ = static_cast<uint16_t>(receivedUdpPort);
  remoteAudioSequence_ = 0;

  Serial.print("session id: ");
  Serial.println(sessionId_);

  Serial.print("server audio: ");
  Serial.print(serverSampleRate_);
  Serial.print(" Hz, ");
  Serial.print(serverFrameDuration_);
  Serial.println(" ms");

  Serial.print("udp server: ");
  Serial.print(udpServerAddress_);
  Serial.print(":");
  Serial.println(udpServerPort_);

  // 所有关键字段都解析成功以后，才标记 hello 已经收到。
  serverHelloReceived_ = true;
}

bool MqttProtocol::openAudioChannel() {
  if (audioChannelOpened_) {
    if (!isChannelTimedOut()) {
      return true;
    }

    Serial.println("mqtt: audio channel timed out, reopening");
    closeAudioChannel(false);
  }

  if (!connected_) {
    Serial.println("mqtt: cannot open audio, not connected");
    return false;
  }

  serverHelloReceived_ = false;
  sendHello();

  // 等待服务器 hello。
  // 消息现在由队列传递，所以这里必须主动消费队列，
  // 否则 hello 永远不会被解析，等待必然超时。
  const uint32_t waitStartMs = millis();

  while (!serverHelloReceived_ &&
         millis() - waitStartMs < config::kAudioChannelOpenTimeoutMs) {

    // 取出并解析队列里的 MQTT 报文。
    // openAudioChannel() 与 pollAudioChannel() 都在主循环任务中调用，
    // 因此不会并发访问同一个队列。
    drainIncomingMessages();

    delay(10);
  }

  if (!serverHelloReceived_) {
    Serial.println("mqtt: server hello timeout");
    return false;
  }

  if (udpServerAddress_.isEmpty() || udpServerPort_ == 0) {
    Serial.println("mqtt: udp configuration is unavailable");
    return false;
  }

  // 绑定本地随机 UDP 端口。
  if (!udpAudioSocket_.begin(0)) {
    Serial.println("mqtt: udp socket bind failed");
    return false;
  }

  audioChannelOpened_ = true;
  channelOpenedAtMs_ = millis();
  lastIncomingDataMs_ = 0;

  Serial.println("mqtt: udp channel opened");

  return true;
}

void MqttProtocol::pollAudioChannel() {
  // 先处理 MQTT 原始报文队列。
  // 这一步必须在主循环任务里做，因为 JSON 解析比较耗栈，
  // 不能放在核心 0 的 MQTT 事件任务里执行。
  drainIncomingMessages();

  // MQTT 已断开时，在这里安全地关闭 UDP 通道。
  // 这个函数只在主循环任务里调用，和 WiFiUDP 的收包逻辑同任务，
  // 不会与 MQTT 事件任务并发访问同一个 socket。
  if (pendingAudioChannelClose_) {
    pendingAudioChannelClose_ = false;
    if (audioChannelOpened_) {
      closeAudioChannel(false);
    }
    return;
  }

  if (!audioChannelOpened_) {
    return;
  }

  // 限制单次轮询处理量，避免音频洪峰让主循环长时间不能处理按键和 MQTT 消息。
  for (int processedPackets = 0; processedPackets < 8; ++processedPackets) {
    const int pendingPacketLength = udpAudioSocket_.parsePacket();

    if (pendingPacketLength <= 0) {
      return;
    }

    if (pendingPacketLength >
        static_cast<int>(config::kMaxEncryptedAudioPacketBytes)) {
      Serial.println("mqtt: udp packet is too large");
      continue;
    }

    uint8_t encryptedAudioPacket[
        config::kMaxEncryptedAudioPacketBytes];

    const int receivedLength = udpAudioSocket_.read(
        encryptedAudioPacket, sizeof(encryptedAudioPacket));

    if (receivedLength < 16) {
      continue;
    }

    // 小智 UDP 音频包固定以 0x01 开头。
    if (encryptedAudioPacket[0] != 0x01) {
      Serial.print("mqtt: unexpected udp packet type = ");
      Serial.println(encryptedAudioPacket[0]);
      continue;
    }

    uint16_t encryptedPayloadLength = 0;
    uint32_t audioTimestamp = 0;
    uint32_t receivedRemoteSequence = 0;

    // UDP 音频包格式：
    // type 1B | flags 1B | payload_len 2B | ssrc 4B
    // | timestamp 4B | sequence 4B | payload
    memcpy(&encryptedPayloadLength, encryptedAudioPacket + 2,
           sizeof(encryptedPayloadLength));

    memcpy(&audioTimestamp, encryptedAudioPacket + 8,
           sizeof(audioTimestamp));

    memcpy(&receivedRemoteSequence, encryptedAudioPacket + 12,
           sizeof(receivedRemoteSequence));

    // 网络字节序转成主机字节序。
    encryptedPayloadLength = ntohs(encryptedPayloadLength);
    audioTimestamp = ntohl(audioTimestamp);
    receivedRemoteSequence = ntohl(receivedRemoteSequence);

    // 丢弃重复或过期的数据包。
    if (receivedRemoteSequence <= remoteAudioSequence_) {
      continue;
    }

    if (encryptedPayloadLength > config::kMaxOpusPacketBytes) {
      Serial.println("mqtt: opus payload is too large");
      continue;
    }

    if (receivedLength != static_cast<int>(16 + encryptedPayloadLength)) {
      Serial.println("mqtt: udp payload length mismatch");
      continue;
    }

    remoteAudioSequence_ = receivedRemoteSequence;
    lastIncomingDataMs_ = millis();

    uint8_t decryptedOpusData[config::kMaxOpusPacketBytes];

    // UDP 头本身就是 AES-CTR 使用的 nonce。
    const uint8_t* aesNonce = encryptedAudioPacket;

    if (!cryptAudioPayloadWithAesCtr(
            encryptedAudioPacket + 16, encryptedPayloadLength,
            aesNonce, decryptedOpusData)) {
      Serial.println("mqtt: udp decrypt failed");
      continue;
    }

    // 回调中的数据只在本次调用期间有效。
    // Application 必须复制到播放队列。
    if (incomingOpusPacketCallback_) {
      incomingOpusPacketCallback_(decryptedOpusData,
                                  encryptedPayloadLength,
                                  audioTimestamp);
    }
  }
}

// 从队列取出 MQTT 原始报文，在主循环任务里解析并分发。
void MqttProtocol::drainIncomingMessages() {
  if (incomingMessageQueue_ == nullptr) {
    return;
  }

  // 每轮最多处理 4 条，避免长时间占用主循环。
  for (int processed = 0; processed < 4; ++processed) {
    if (xQueueReceive(incomingMessageQueue_,
                      &mainTaskReceiveMessage_, 0) != pdTRUE) {
      return;
    }

    const String payload(mainTaskReceiveMessage_.payload,
                         mainTaskReceiveMessage_.length);
    lastIncomingDataMs_ = millis();

    // 串口输出放在主循环任务里执行，不要放在 MQTT 任务里。
    Serial.print("mqtt message: ");
    Serial.println(payload);

    JsonDocument messageDocument;
    const DeserializationError parseError =
        deserializeJson(messageDocument, payload);

    const char* messageType =
        parseError ? "" : (messageDocument["type"] | "");

    if (strcmp(messageType, "hello") == 0) {
      parseServerHello(payload);
    } else if (incomingJsonCallback_) {
      incomingJsonCallback_(payload);
    }
  }
}

bool MqttProtocol::decodeHexString(const String& hexadecimalText,
                                   uint8_t* outputBytes,
                                   size_t expectedByteCount) {
  if (outputBytes == nullptr) {
    return false;
  }

  const size_t expectedCharacterCount = expectedByteCount * 2;

  if (hexadecimalText.length() != expectedCharacterCount) {
    return false;
  }

  for (size_t byteIndex = 0; byteIndex < expectedByteCount; ++byteIndex) {
    const char highCharacter = hexadecimalText[byteIndex * 2];
    const char lowCharacter = hexadecimalText[byteIndex * 2 + 1];

    const int highValue = hexCharacterToValue(highCharacter);
    const int lowValue = hexCharacterToValue(lowCharacter);

    if (highValue < 0 || lowValue < 0) {
      memset(outputBytes, 0, expectedByteCount);
      return false;
    }

    outputBytes[byteIndex] =
        static_cast<uint8_t>((highValue << 4) | lowValue);
  }

  return true;
}

bool MqttProtocol::cryptAudioPayloadWithAesCtr(
    const uint8_t* inputData,
    size_t inputLength,
    const uint8_t nonce[16],
    uint8_t* outputData) {
  if (inputData == nullptr || inputLength == 0 || nonce == nullptr ||
      outputData == nullptr) {
    return false;
  }

  esp_aes_context aesContext;
  esp_aes_init(&aesContext);

  int operationResult =
      esp_aes_setkey(&aesContext, aesEncryptionKey_, 128);

  size_t streamOffset = 0;
  uint8_t counter[16] = {};
  uint8_t streamBlock[16] = {};

  memcpy(counter, nonce, sizeof(counter));

  if (operationResult == 0) {
    operationResult = esp_aes_crypt_ctr(
        &aesContext, inputLength, &streamOffset, counter, streamBlock,
        inputData, outputData);
  }

  esp_aes_free(&aesContext);

  return operationResult == 0;
}

void MqttProtocol::closeAudioChannel(bool sendGoodbyeMessage) {
  if (sendGoodbyeMessage && connected_ && !sessionId_.isEmpty()) {
    JsonDocument doc;
    doc["session_id"] = sessionId_;
    doc["type"] = "goodbye";

    String out;
    serializeJson(doc, out);
    sendText(out);
  }

  udpAudioSocket_.stop();

  audioChannelOpened_ = false;
  serverHelloReceived_ = false;

  udpServerAddress_ = "";
  udpServerPort_ = 0;
  sessionId_ = "";
  remoteAudioSequence_ = 0;
  localAudioSequence_ = 0;
  lastIncomingDataMs_ = 0;
  channelOpenedAtMs_ = 0;

  memset(aesEncryptionKey_, 0, sizeof(aesEncryptionKey_));
  memset(aesEncryptionNonce_, 0, sizeof(aesEncryptionNonce_));

  Serial.println("mqtt: udp audio channel closed");
}

bool MqttProtocol::isChannelTimedOut() const {
  if (!audioChannelOpened_) {
    return false;
  }

  // 还没有收到任何服务端数据时，以通道打开时间作为超时起点。
  // 避免客户端发了 hello 后服务端一直不应答，状态机却无限等待。
  if (lastIncomingDataMs_ == 0) {
    return millis() - channelOpenedAtMs_ >= config::kAudioChannelTimeoutMs;
  }

  return millis() - lastIncomingDataMs_ >= config::kAudioChannelTimeoutMs;
}

bool MqttProtocol::isAudioChannelOpen() const {
  return audioChannelOpened_ && connected_ && !isChannelTimedOut();
}

bool MqttProtocol::sendWakeWordDetected(const String& wakeWordText) {
  if (!connected_) {
    Serial.println("mqtt: cannot send wake word, not connected");
    return false;
  }

  if (sessionId_.isEmpty()) {
    Serial.println("mqtt: cannot send wake word, session is empty");
    return false;
  }

  JsonDocument doc;
  doc["session_id"] = sessionId_;
  doc["type"] = "listen";
  doc["state"] = "detect";
  doc["text"] = wakeWordText;

  String out;
  serializeJson(doc, out);
  return sendText(out);
}

// 发送“开始聆听”控制消息。
// manual 模式表示由按键决定何时停止聆听，不由服务器自动判断。
bool MqttProtocol::sendStartListening(bool realtime) {
  if (sessionId_.isEmpty()) {
    Serial.println("mqtt: cannot start listening, session is empty");
    return false;
  }

  JsonDocument doc;
  doc["session_id"] = sessionId_;
  doc["type"] = "listen";
  doc["state"] = "start";
  doc["mode"] = realtime ? "realtime" : "manual";

  String out;
  serializeJson(doc, out);
  return sendText(out);
}

// 发送“停止聆听”控制消息，通知服务器上行录音结束。
bool MqttProtocol::sendStopListening() {
  if (sessionId_.isEmpty()) {
    Serial.println("mqtt: cannot stop listening, session is empty");
    return false;
  }

  JsonDocument doc;
  doc["session_id"] = sessionId_;
  doc["type"] = "listen";
  doc["state"] = "stop";

  String out;
  serializeJson(doc, out);
  return sendText(out);
}

// 发送“打断”控制消息，用于打断服务器正在播放的语音。
bool MqttProtocol::sendAbortSpeaking() {
  if (sessionId_.isEmpty()) {
    Serial.println("mqtt: cannot abort, session is empty");
    return false;
  }

  JsonDocument doc;
  doc["session_id"] = sessionId_;
  doc["type"] = "abort";

  String out;
  serializeJson(doc, out);
  return sendText(out);
}

// 把一帧 Opus 数据加密后通过 UDP 发送给服务器。
//
// UDP 加密音频包格式：
//   type 1B | flags 1B | payload_len 2B | ssrc 4B
//   | timestamp 4B | sequence 4B | encrypted payload
//
// payload_len、timestamp、sequence 使用网络字节序。
// AES-CTR 的 nonce 就是这 16 字节头，其中后三个字段每包都会更新。
bool MqttProtocol::sendAudioFrame(const uint8_t* opusData,
                                  size_t opusDataLength,
                                  uint32_t audioTimestamp) {
  if (!audioChannelOpened_ || opusData == nullptr ||
      opusDataLength == 0 ||
      opusDataLength > config::kMaxOpusPacketBytes) {
    return false;
  }

  const uint16_t payloadLength =
      htons(static_cast<uint16_t>(opusDataLength));
  const uint32_t timestamp = htonl(audioTimestamp);
  const uint32_t sequence = htonl(++localAudioSequence_);

  // 服务器下发的 nonce 本身就是 16 字节 UDP 头初值，
  // 其中已经包含 type、flags 和 ssrc 字段。
  // 客户端只需要覆盖每包都会变化的三个字段。
  uint8_t packetHeader[16] = {};
  memcpy(packetHeader, aesEncryptionNonce_, sizeof(packetHeader));

  memcpy(packetHeader + 2, &payloadLength, sizeof(payloadLength));
  memcpy(packetHeader + 8, &timestamp, sizeof(timestamp));
  memcpy(packetHeader + 12, &sequence, sizeof(sequence));

  // 加密时必须直接用 packetHeader 作为 nonce。
  // 如果再单独构造一个 nonce，两边只要有任何一个字节不同，
  // 服务器解密出来就是噪声。
  uint8_t encryptedPayload[config::kMaxOpusPacketBytes] = {};
  if (!cryptAudioPayloadWithAesCtr(opusData, opusDataLength, packetHeader,
                                   encryptedPayload)) {
    Serial.println("mqtt: udp encrypt failed");
    return false;
  }

  if (!udpAudioSocket_.beginPacket(udpServerAddress_.c_str(),
                                   udpServerPort_)) {
    Serial.println("mqtt: udp beginPacket failed");
    return false;
  }

  udpAudioSocket_.write(packetHeader, sizeof(packetHeader));
  udpAudioSocket_.write(encryptedPayload, opusDataLength);

  return udpAudioSocket_.endPacket() == 1;
}
