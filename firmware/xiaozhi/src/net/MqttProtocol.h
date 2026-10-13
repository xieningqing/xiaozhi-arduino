#pragma once

#include <Arduino.h>

// esp_mqtt_client 是 ESP32 Arduino Core 自带的 MQTT 客户端组件。
// 它不需要额外安装库，直接 include 就能使用。
#include <mqtt_client.h>

#include <WiFiUdp.h>
#include <functional>

// UDP 解密后得到的是 Opus 压缩数据，不是 PCM。
// Application 收到后必须复制到队列，不能长期保存这个指针。
using IncomingOpusPacketCallback = std::function<void(
    const uint8_t* decryptedOpusData,
    size_t decryptedOpusLength,
    uint32_t audioTimestamp)>;

using IncomingJsonCallback = std::function<void(const String& payload)>;

// MQTT 回调运行在系统事件任务上，该任务栈只有 2048 字节。
// 因此绝不能在回调里创建这么大的栈对象，否则会把事件任务的栈写穿，
// 表现为随机时间、随机地址崩溃。
// 收发两端各用独立的成员缓冲，避免两个任务同时使用同一块内存。
struct PendingControlMessage {
  uint16_t length = 0;
  char payload[512] = {};
};



// 小智 MQTT 控制通道和 UDP 音频通道。
//
// 流程：
//   1. 用 OTA 下发的 client_id / username / password 连接 broker（TLS 8883）
//   2. 订阅 publish_topic，用于接收服务器回复
//   3. 向 publish_topic 发布 hello，申请 UDP 音频通道
//   4. 解析服务器 hello 中的 session_id、UDP 地址和 AES 密钥
//   5. 使用 WiFiUDP 接收音频包，并用 AES-CTR 解密
class MqttProtocol {
public:
  // 连接 MQTT Broker。
  // endpoint 可以写成 "host:8883"，也可以只写 "host"。
  bool connect(const String& endpoint,
               const String& clientId,
               const String& username,
               const String& password,
               const String& publishTopic);

  // MQTT Broker 是否已经连接。
  bool isConnected() const { return connected_; }

  // 是否已经收到并成功解析服务器 hello。
  bool serverHelloReceived() const { return serverHelloReceived_; }

  // 服务器 hello 里的信息。
  const String& sessionId() const { return sessionId_; }
  int serverSampleRate() const { return serverSampleRate_; }
  int serverFrameDuration() const { return serverFrameDuration_; }

  // 发布一条文本消息到 publish_topic。
  bool sendText(const String& text);

  // 发送 hello，等待服务器 hello，并打开本地 UDP 通道。
  bool openAudioChannel();

  // 持续读取 UDP 音频数据。
  // 必须在 Application::loop() 或独立任务中频繁调用。
  void pollAudioChannel();

  // 关闭 UDP 音频通道。
  // sendGoodbyeMessage 为 true 时，会先通过 MQTT 通知服务器。
  void closeAudioChannel(bool sendGoodbyeMessage = true);

  // UDP 音频通道是否已经打开。
  bool isAudioChannelOpen() const;

  // M4 测试入口：模拟检测到唤醒词。
  bool sendWakeWordDetected(const String& wakeWordText);

  // 发布开始聆听、停止聆听和打断播放的控制消息。
  // realtime 为 true 时使用服务器全双工模式，手上行发送由独立任务负责。
  bool sendStartListening(bool realtime = false);
  bool sendStopListening();
  bool sendAbortSpeaking();

  // 把编码后的 Opus 数据加密并通过 UDP 发送到服务器。
  bool sendAudioFrame(const uint8_t* opusData,
                      size_t opusDataLength,
                      uint32_t audioTimestamp);

  // 注册解密后的 Opus 数据回调。
  // 回调运行在调用 pollAudioChannel() 的任务中。
  void setIncomingOpusPacketCallback(
      IncomingOpusPacketCallback callback) {
    incomingOpusPacketCallback_ = callback;
  }

  void setIncomingJsonCallback(IncomingJsonCallback callback){
    incomingJsonCallback_ = callback;
  }
private:
  // esp_mqtt_client 的 C 风格事件回调。
  static void eventHandler(void* handlerArgs,
                           esp_event_base_t base,
                           int32_t eventId,
                           void* eventData);

  // 处理 MQTT 连接、断开、消息和错误事件。
  void onEvent(int32_t eventId, void* eventData);

  // 从队列取出 MQTT 原始报文并解析、分发。
  // 必须在主循环任务里调用。
  void drainIncomingMessages();

  // 发布 hello，申请 UDP 音频通道。
  void sendHello();

  // 解析服务器 hello。
  void parseServerHello(const String& payload);

  // 把十六进制字符串转换成字节数组。
  // udp.key 和 udp.nonce 都是 32 个字符，转换成 16 字节。
  static bool decodeHexString(const String& hexadecimalText,
                              uint8_t* outputBytes,
                              size_t expectedByteCount);

  // AES-CTR 加密和解密共用这个函数。
  bool cryptAudioPayloadWithAesCtr(const uint8_t* inputData,
                                   size_t inputLength,
                                   const uint8_t nonce[16],
                                   uint8_t* outputData);

  esp_mqtt_client_handle_t client_ = nullptr;

  // esp-mqtt 的配置字段都是 const char*，它不复制字符串，只保存指针。
  // 因此这些字符串必须活到 MQTT 客户端销毁为止，
  // 不能使用 connect() 里的局部变量，否则客户端后续读到的是已释放内存。
  String brokerHostStorage_;
  String brokerClientIdStorage_;
  String brokerUsernameStorage_;
  String brokerPasswordStorage_;

  // MQTT 事件回调和主循环运行在不同任务中，因此使用 volatile。
  volatile bool connected_ = false;
  volatile bool serverHelloReceived_ = false;

  // MQTT 断开事件运行在 MQTT 任务里，不能在那里直接关闭 UDP。
  // 因为主循环可能正在同一个 WiFiUDP 对象上收包，WiFiUDP 不是线程安全的。
  // 这里只置标志，真正的关闭动作由 pollAudioChannel() 在主循环执行。
  volatile bool pendingAudioChannelClose_ = false;

  String publishTopic_;
  String sessionId_;
  int serverSampleRate_ = 24000;
  int serverFrameDuration_ = 60;

  // UDP 音频通道。
  WiFiUDP udpAudioSocket_;
  String udpServerAddress_;
  uint16_t udpServerPort_ = 0;

  // 服务器下发的 AES-128 密钥和 nonce。
  uint8_t aesEncryptionKey_[16] = {};
  uint8_t aesEncryptionNonce_[16] = {};

  // 已经接收到的最大 UDP 序列号。
  uint32_t remoteAudioSequence_ = 0;

  // 发送端 UDP 音频包使用的递增序列号。
  uint32_t localAudioSequence_ = 0;

  bool audioChannelOpened_ = false;

  // 最近一次收到服务端 MQTT 报文或有效 UDP 音频包的时间。
  // 对齐官方 120 秒无数据超时，避免通道已经失效但状态机继续等待。
  uint32_t lastIncomingDataMs_ = 0;
  uint32_t channelOpenedAtMs_ = 0;

  bool isChannelTimedOut() const;

  // 解密完成后的 Opus 数据回调。
  IncomingOpusPacketCallback incomingOpusPacketCallback_;
  IncomingJsonCallback incomingJsonCallback_;

  // MQTT 原始报文队列，由 MQTT 任务写入，主循环任务读取。
  QueueHandle_t incomingMessageQueue_ = nullptr;

  // MQTT 事件任务的临时缓冲。
  // 必须放在对象里（BSS），不能放在事件回调的栈上：
  // 事件任务栈只有 2048 字节，栈上放 500 字节以上的对象会直接溢出。
  // 事件任务串行处理消息，所以这一块可以安全复用。
  PendingControlMessage mqttTaskStagingMessage_;
  PendingControlMessage mqttTaskDiscardMessage_;

  // 主循环任务侧的接收缓冲，同样避免放在栈上。
  PendingControlMessage mainTaskReceiveMessage_;
};
