#include "Application.h"
#include "../core/Version.h"
#include<ArduinoJson.h>

void Application::begin()
{
  // USB CDC On Boot = Disabled：Serial 走 UART0，必须传波特率。
  Serial.begin(config::kSerialBaud);
  // 给足上电时间
  delay(300);

  button_.begin();
  display_.begin();
  display_.showBoot();

  Serial.println("boot ok");

  // 初始化麦克风
  if(mic_.begin()){
    Serial.println("mic ok");
  }else{
    Serial.println("mic init failed");
  }

  // 功放只初始化一次
  if (spk_.begin()) {
    Serial.println("speaker ok");
  } else {
    Serial.println("speaker init failed");
  }

  // Opus 解码器负责把服务端返回的压缩音频解码成 PCM。
  if (opusDecoder_.begin(config::kPlaybackSampleRate)) {
    Serial.println("opus decoder ok");
  } else {
    Serial.println("opus decoder init failed");
  }

  // Opus 编码器负责把麦克风 PCM 压缩后上传。
  if (opusEncoder_.begin(config::kMicSampleRate,
    config::kAudioFrameDurationMs)) {
    Serial.println("opus encoder ok");
  } else {
    Serial.println("opus encoder init failed");
  }



  // 网络回调只负责入队，播放任务负责解码和写 I2S。
  playbackQueue_ = xQueueCreate(config::kPlaybackQueueLength,
                                sizeof(PlaybackPacket));
  if (playbackQueue_ == nullptr) {
    Serial.println("playback queue create failed");
  } else {
    // Opus 解码内部使用 alloca() 在任务栈上分配临时缓冲，
    // 24kHz 一帧的临时数据量不小，因此栈必须给足。
    // 这里从 16KB 提高到 32KB，并保存句柄用于观测栈水位。
    xTaskCreatePinnedToCore(playbackTaskEntry, "opus_playback", 32768, this,
                            5, &playbackTaskHandle_, 1);
  }

  // 麦克风采集和编码独立于主循环运行。
  uplinkTimestampQueue_ = xQueueCreate(config::kUploadQueueLength,
                                       sizeof(uint32_t));
  uploadQueue_ = xQueueCreate(config::kUploadQueueLength,
                              sizeof(UploadPacket));
  if (uploadQueue_ == nullptr) {
    Serial.println("upload queue create failed");
  } else {
    // Opus 编码在任务栈上使用 alloca()，栈必须留足。
    xTaskCreatePinnedToCore(uploadTaskEntry, "opus_upload", 32768, this, 5,
                            &uploadTaskHandle_, 1);
  }


  // wifi
  const bool wifiConnected = network_.connectWifi(
      [this](const String& statusText) {
        display_.setStatus(statusText.c_str(), TFT_YELLOW);
        display_.setHint("configure WiFi in browser");
      });
  if(!wifiConnected){
    Serial.println("wifi failed");
    return;
  }
  // 生成/读取设备身份（必须在 WiFi 连上之后，MAC 才可读）
  // loadIdentity 由 fetchOtaConfig 内部第一次调用即可，
  // 这里先同步时间。
  network_.syncTime();

  if(network_.fetchOtaConfig()){
    Serial.println("fetch ota config ok");

    // 这台设备在平台上被登记为"开源版"接入，服务器启用的是 MQTT + UDP。
    // WebSocket 的 token 是占位值，走不通，所以这里用 MQTT。
    if (network_.hasMqttConfig()) {
      mqttConfigured_ = true;
      if (!protocol_.connect(network_.mqttEndpoint(),
                             network_.mqttClientId(),
                             network_.mqttUsername(),
                             network_.mqttPassword(),
                             network_.mqttPublishTopic())) {
        Serial.println("mqtt connect failed");
      } else {
        // 注册回调后，再把解密完成的 Opus 数据复制到播放队列。
        protocol_.setIncomingOpusPacketCallback(
            [this](const uint8_t* decryptedOpusData,
                   size_t decryptedOpusLength,
                   uint32_t audioTimestamp) {
              enqueueIncomingAudioPacket(decryptedOpusData,
                                         decryptedOpusLength,
                                         audioTimestamp);
            });
        protocol_.setIncomingJsonCallback(
          [this](const String& payload){
            handleServerMessage(payload);
          });
        if (protocol_.openAudioChannel()) {
          chatState_ = ChatState::Idle;
          display_.setNetwork(true, true);
          display_.showMainScreen();
          refreshUi();
          Serial.println("mqtt audio channel open ok");
        } else {
          chatState_ = ChatState::Idle;
          display_.setNetwork(true, false);
          display_.setStatus("CONNECT FAIL", TFT_RED);
          display_.setHint("check WiFi or service");
          Serial.println("mqtt audio channel open failed");
        }
      }
    } else {
      Serial.println("no mqtt config in ota response");
    }
  }else{
    Serial.println("fetch ota config failed");
  }
}


void Application::loop() {
  // MQTT 断线重连。
  // 断线后 connected_ 会变 false，只在 Idle 状态按官方节奏重连。
  // 重连成功后需要重新申请音频通道，所以状态一并复位。
  if (mqttConfigured_ && !protocol_.isConnected() &&
      chatState_ == ChatState::Idle &&
      millis() - lastReconnectAttemptMs_ >= config::kMqttReconnectIntervalMs) {
    lastReconnectAttemptMs_ = millis();

    Serial.println("mqtt reconnecting...");
    if (protocol_.connect(network_.mqttEndpoint(),
                          network_.mqttClientId(),
                          network_.mqttUsername(),
                          network_.mqttPassword(),
                          network_.mqttPublishTopic())) {
      Serial.println("mqtt reconnected");

      if (protocol_.openAudioChannel()) {
        Serial.println("audio channel reopened");
        display_.setNetwork(true, true);
      } else {
        Serial.println("audio channel reopen failed");
        display_.setNetwork(true, false);
      }
      chatState_ = ChatState::Idle;
      display_.showMainScreen();
      refreshUi();
    }
  }

  // UDP 音频包必须持续轮询，否则收不到服务端语音。
  protocol_.pollAudioChannel();

  // 上行音频由独立任务采集和编码，这里只负责按网络节奏发送。
  drainUploadQueue();

  // 屏幕只重绘变化区域，电平条按百分比增量刷新。
  display_.setMicLevel(micLevelPercent_);

  // 长消息超过两行时按节奏向上滚动。
  display_.tick();

  // 只有等待服务端回复的 Thinking 才用 120 秒无下行超时兜底。
  // Listening/Realtime 正在持续上行，不能因为服务端暂时没回音频就断开。
  if (mqttConfigured_ && protocol_.isConnected() &&
      !protocol_.isAudioChannelOpen() &&
      chatState_ == ChatState::Thinking) {
    returnToIdle("audio channel timeout");
  }

  // MQTT 在对话过程中断开时，也必须先回到 Idle。
  // 否则重连逻辑会因为没有回到 Idle 而永远不会执行。
  if (mqttConfigured_ && !protocol_.isConnected() &&
      chatState_ != ChatState::Idle &&
      chatState_ != ChatState::Connecting) {
    returnToIdle("mqtt disconnected");
  }

  if (chatState_ == ChatState::Realtime &&
      !protocol_.isAudioChannelOpen()) {
    exitRealtimeMode("realtime channel closed");
  }

  // 按键边沿检测：只在“松开”时触发一次，避免长按连发。
  // BOOT 长按用于重新配网，普通短按才切换对话状态。
  const bool bootButtonRaw = digitalRead(config::kBootButtonPin) == LOW;
  if (bootButtonRaw) {
    if (bootButtonPressedAtMs_ == 0) {
      bootButtonPressedAtMs_ = millis();
    } else if (!configPortalRequested_ &&
               millis() - bootButtonPressedAtMs_ >=
                   config::kWifiConfigLongPressMs) {
      configPortalRequested_ = true;
      suppressButtonRelease_ = true;
      Serial.println("entering WiFi configuration mode...");
      display_.setMode("SETUP");
      display_.setStatus("WIFI SETUP", TFT_YELLOW);
      display_.setHint("connect Xiaozhi-xxxx AP");

      if (!network_.requestConfigPortalOnNextBoot()) {
        Serial.println("failed to save WiFi configuration flag");
        display_.setStatus("SAVE FAIL", TFT_RED);
      } else {
        delay(300);
        ESP.restart();
      }
    }
  } else {
    bootButtonPressedAtMs_ = 0;
  }

  if (button_.update()) {
    const bool pressed = button_.isPressed();

    if (lastButtonPressed_ && !pressed) {
      if (suppressButtonRelease_) {
        suppressButtonRelease_ = false;
      } else {
        handleButtonRelease();
      }
    }

    lastButtonPressed_ = pressed;
  }

  // 延迟单击判定：双击用来切换全双工，避免双击的第一次松开就进入手动模式。
  if (pendingSingleClick_ &&
      millis() - pendingSingleClickAtMs_ >= config::kDoubleClickWindowMs) {
    pendingSingleClick_ = false;
    handleSingleClick();
  }

  // MQTT 回调只置了标志，这里在主任务里统一处理，
  // 避免在 MQTT 任务里直接操作屏幕。
  if (pendingTtsStart_) {
    pendingTtsStart_ = false;
    // 新录音已经开始时，忽略上一轮迟到的 tts start。
    // 实时模式没有 Speaking 状态，TTS 只作为播放状态提示。
    if (chatState_ == ChatState::Thinking ||
        chatState_ == ChatState::Idle) {
      chatState_ = ChatState::Speaking;
      refreshUi();
    } else if (chatState_ == ChatState::Realtime) {
      refreshUi();
    }
  }

  if (pendingTtsStop_) {
    pendingTtsStop_ = false;
    // 旧会话的 tts stop 不能在新一轮 Listening 中把状态打回 Idle。
    if (chatState_ == ChatState::Speaking) {
      chatState_ = ChatState::Idle;
      refreshUi();
    } else if (chatState_ == ChatState::Realtime) {
      refreshUi();
    }
  }

  if (pendingSttReady_) {
    pendingSttReady_ = false;
    display_.setMessage(pendingSttText_);
  }

  if (pendingServerGoodbye_) {
    pendingServerGoodbye_ = false;
    returnToIdle("server goodbye");
  }

  // Thinking 超时兜底：
  // 服务端可能在网络抖动时收不到本轮音频，也就不回 tts start。
  // 这里超时后主动回到待机，避免状态机永久卡住。
  if (chatState_ == ChatState::Thinking &&
      millis() - thinkingStartMs_ >= config::kThinkingTimeoutMs) {
    returnToIdle("thinking timeout");
  }

  // 让出 CPU，避免主循环空转。
  delay(1);
}

void Application::playbackTaskEntry(void* argument) {
  static_cast<Application*>(argument)->playbackTask();
}

void Application::playbackTask() {
  PlaybackPacket packet;
  int16_t pcmFrame[config::kPlaybackFrameSamples];

  while (true) {
    if (xQueueReceive(playbackQueue_, &packet, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    const size_t decodedSamples = opusDecoder_.decodeFrame(
        packet.data, packet.length, pcmFrame,
        config::kPlaybackFrameSamples);

    if (decodedSamples > 0) {
      // 每播放一帧就登记它的时间戳，供上行编码按顺序取用。
      // 队列满时丢弃最旧时间戳，保持实时性。
      if (uplinkTimestampQueue_ != nullptr && packet.timestamp != 0) {
        if (xQueueSend(uplinkTimestampQueue_, &packet.timestamp, 0) != pdTRUE) {
          uint32_t droppedTimestamp = 0;
          xQueueReceive(uplinkTimestampQueue_, &droppedTimestamp, 0);
          xQueueSend(uplinkTimestampQueue_, &packet.timestamp, 0);
        }
      }
      spk_.writeSamples(pcmFrame, decodedSamples);
    }
  }
}

void Application::uploadTaskEntry(void* argument) {
  static_cast<Application*>(argument)->uploadTask();
}

void Application::uploadTask() {
  int32_t micSamples[config::kMicFrameSamples];
  int16_t pcmSamples[config::kMicFrameSamples];
  uint8_t opusData[config::kMaxOpusPacketBytes];

  while (true) {
    if (!uploadEnabled_) {
      delay(5);
      continue;
    }

    const size_t readSamples = mic_.readSamples(
        micSamples, config::kMicFrameSamples, config::kMicReadTimeoutMs);

    if (readSamples != config::kMicFrameSamples) {
      delay(1);
      continue;
    }

    // 读取期间会话可能已经被停止，丢弃这一帧，避免无效上传。
    if (!uploadEnabled_) {
      continue;
    }

    convertMicrophoneFrameToPcm(micSamples, pcmSamples, readSamples);

    // 计算本帧平均幅度，供屏幕电平条显示。
    uint32_t amplitudeSum = 0;
    for (size_t index = 0; index < readSamples; ++index) {
      const int32_t sample = pcmSamples[index];
      amplitudeSum += static_cast<uint32_t>(sample < 0 ? -sample : sample);
    }
    const uint32_t averageAmplitude =
        readSamples > 0 ? amplitudeSum / readSamples : 0;
    uint32_t levelPercent = averageAmplitude * 100 / 6000;
    if (levelPercent > 100) {
      levelPercent = 100;
    }
    micLevelPercent_ = static_cast<uint8_t>(levelPercent);

    const size_t encodedBytes = opusEncoder_.encodeFrame(
        pcmSamples, readSamples, opusData, config::kMaxEncodeOpusBytes);

    if (encodedBytes == 0) {
      continue;
    }

    UploadPacket packet;
    packet.length = static_cast<uint16_t>(encodedBytes);
    memcpy(packet.data, opusData, encodedBytes);

    // 只有全双工实时模式才使用下行播放时间戳做服务端 AEC 对齐；
    // 手动模式仍使用本地递增时间戳（0 由发送端补齐）。
    packet.timestamp = 0;
    if (realtimeModeActive_ && uplinkTimestampQueue_ != nullptr) {
      xQueueReceive(uplinkTimestampQueue_, &packet.timestamp, 0);
    }

    if (xQueueSend(uploadQueue_, &packet, 0) != pdTRUE) {
      UploadPacket droppedPacket;
      xQueueReceive(uploadQueue_, &droppedPacket, 0);
      xQueueSend(uploadQueue_, &packet, 0);
    }
  }
}

void Application::drainUploadQueue() {
  if (uploadQueue_ == nullptr || !uploadEnabled_ ||
      !protocol_.isAudioChannelOpen()) {
    return;
  }

  UploadPacket packet;
  while (xQueueReceive(uploadQueue_, &packet, 0) == pdTRUE) {
    // 实时且拿到了下行时间戳时直接使用；否则按 60ms 递增。
    if (packet.timestamp != 0) {
      uploadTimestampMs_ = packet.timestamp;
    } else {
      uploadTimestampMs_ += config::kAudioFrameDurationMs;
      packet.timestamp = uploadTimestampMs_;
    }
    if (!protocol_.sendAudioFrame(packet.data, packet.length,
                                  packet.timestamp)) {
      Serial.println("audio upload failed");
      break;
    }
  }
}

void Application::enqueueIncomingAudioPacket(
    const uint8_t* decryptedOpusData,
    size_t decryptedOpusLength,
    uint32_t audioTimestamp) {
  if (playbackQueue_ == nullptr || decryptedOpusData == nullptr ||
      decryptedOpusLength == 0 ||
      decryptedOpusLength > config::kMaxOpusPacketBytes) {
    return;
  }

  // 实时模式必须持续接收下行音频；手动模式在等待回复和播放时接收。
  // tts start 走 MQTT、音频走 UDP，两者可能乱序到达，
  // 所以 Thinking 阶段也要接收，否则会丢掉最开始几帧。
  if (chatState_ != ChatState::Realtime &&
      chatState_ != ChatState::Thinking &&
      chatState_ != ChatState::Speaking) {
    return;
  }

  PlaybackPacket packet;
  packet.timestamp = audioTimestamp;
  packet.length = static_cast<uint16_t>(decryptedOpusLength);
  memcpy(packet.data, decryptedOpusData, decryptedOpusLength);

  // 网络音频优先保证实时性，队列满时丢弃最旧的包。
  if (xQueueSend(playbackQueue_, &packet, 0) != pdTRUE) {
    PlaybackPacket droppedPacket;
    xQueueReceive(playbackQueue_, &droppedPacket, 0);
    xQueueSend(playbackQueue_, &packet, 0);
  }
}

void Application::handleButtonRelease(){
  // Idle 和 Realtime 中，单击可能只是双击的第一下。
  // 因此这里不立即执行，等双击窗口过去后再走手动模式。
  if (chatState_ == ChatState::Idle ||
      chatState_ == ChatState::Realtime) {
    if (pendingSingleClick_) {
      pendingSingleClick_ = false;
      handleDoubleClick();
      return;
    }

    pendingSingleClick_ = true;
    pendingSingleClickAtMs_ = millis();
    return;
  }

  if (pendingSingleClick_) {
    pendingSingleClick_ = false;
  }

  switch(chatState_){
    case ChatState::Listening:
      stopListening();
      break;

    case ChatState::Thinking:
      // 已停止录音，正在等待服务器识别，不响应按键。
      break;
    
    case ChatState::Speaking:
     // 播放中按一下打断，并直接开始下一轮录音。
     protocol_.sendAbortSpeaking();

     // 清空还没播放的旧音频，避免打断后继续出声。
     if (playbackQueue_ != nullptr) {
       xQueueReset(playbackQueue_);
     }

     chatState_ = ChatState::Idle;
     startListening();
     break;

    case ChatState::Connecting:
      break;

    case ChatState::Idle:
    case ChatState::Realtime:
      // 已在函数开头处理。
      break;
  }
}

void Application::handleDoubleClick() {
  pendingSingleClick_ = false;

  if (chatState_ == ChatState::Realtime) {
    exitRealtimeMode("double click");
  } else if (chatState_ == ChatState::Idle ||
             chatState_ == ChatState::Speaking) {
    enterRealtimeMode();
  }
}

void Application::handleSingleClick() {
  if (chatState_ == ChatState::Idle) {
    startListening();
  } else if (chatState_ == ChatState::Realtime) {
    // 实时模式下双击才退出，单击保持会话，避免误触中断。
    refreshUi();
  }
}

void Application::enterRealtimeMode() {
  if (chatState_ == ChatState::Realtime) {
    return;
  }

  if (chatState_ == ChatState::Speaking) {
    protocol_.sendAbortSpeaking();
    if (playbackQueue_ != nullptr) {
      xQueueReset(playbackQueue_);
    }
  }

  if (!protocol_.isAudioChannelOpen()) {
    chatState_ = ChatState::Connecting;
    display_.setStatus("CONNECTING", TFT_YELLOW);
    if (!protocol_.openAudioChannel()) {
      chatState_ = ChatState::Idle;
      display_.setStatus("CONNECT FAIL", TFT_RED);
      return;
    }
  }

  if (!protocol_.sendStartListening(true)) {
    display_.setStatus("REALTIME FAIL", TFT_RED);
    chatState_ = ChatState::Idle;
    return;
  }

  uploadTimestampMs_ = 0;
  if (uploadQueue_ != nullptr) {
    xQueueReset(uploadQueue_);
  }
  if (uplinkTimestampQueue_ != nullptr) {
    xQueueReset(uplinkTimestampQueue_);
  }
  if (playbackQueue_ != nullptr) {
    xQueueReset(playbackQueue_);
  }

  realtimeModeActive_ = true;
  uploadEnabled_ = true;
  chatState_ = ChatState::Realtime;
  refreshUi();
  Serial.println("realtime mode entered");
}

void Application::exitRealtimeMode(const char* reason) {
  uploadEnabled_ = false;
  realtimeModeActive_ = false;
  pendingSingleClick_ = false;
  if (uploadQueue_ != nullptr) {
    xQueueReset(uploadQueue_);
  }
  if (uplinkTimestampQueue_ != nullptr) {
    xQueueReset(uplinkTimestampQueue_);
  }
  if (playbackQueue_ != nullptr) {
    xQueueReset(playbackQueue_);
  }

  protocol_.sendStopListening();
  chatState_ = ChatState::Idle;
  refreshUi();

  Serial.print("realtime mode exited: ");
  Serial.println(reason);
}

void Application::startListening(){
  if(chatState_ != ChatState::Idle){
    return;
  }

  if(!protocol_.isAudioChannelOpen()){
    chatState_ = ChatState::Connecting;
    display_.setStatus("CONNECTING", TFT_YELLOW);

    if(!protocol_.openAudioChannel()){
      chatState_ = ChatState::Idle;
      display_.setStatus("CONNECT FAIL", TFT_RED);
      return;
    }
  }
  if(!protocol_.sendStartListening()){
    display_.setStatus("LISTEN FAIL", TFT_RED);
    protocol_.closeAudioChannel(false);
    chatState_ = ChatState::Idle;
    return;
  }

  // 时间戳从 0 开始按帧递增，避免依赖开机毫秒数。
  uploadTimestampMs_ = 0;
  realtimeModeActive_ = false;
  uploadEnabled_ = true;
  if (uploadQueue_ != nullptr) {
    xQueueReset(uploadQueue_);
  }
  if (uplinkTimestampQueue_ != nullptr) {
    xQueueReset(uplinkTimestampQueue_);
  }

  // 开始新一轮对话前清空上一轮残留的音频包。
  if (playbackQueue_ != nullptr) {
    xQueueReset(playbackQueue_);
  }

  chatState_ = ChatState::Listening;
  refreshUi();
}

void Application::stopListening() {
  protocol_.sendStopListening();

  uploadEnabled_ = false;
  realtimeModeActive_ = false;
  if (uploadQueue_ != nullptr) {
    xQueueReset(uploadQueue_);
  }
  if (uplinkTimestampQueue_ != nullptr) {
    xQueueReset(uplinkTimestampQueue_);
  }

  // 进入 Thinking，直到服务器返回 tts start 才切到 Speaking。
  // 如果这里直接回到 Idle，用户再次按键会重复发送 listen start。
  chatState_ = ChatState::Thinking;
  thinkingStartMs_ = millis();
  refreshUi();
}

void Application::convertMicrophoneFrameToPcm(
  const int32_t* source, int16_t* destination, size_t sampleCount) {
    for (size_t index = 0; index < sampleCount; ++index) {
      // INMP441 的 24 位有效数据在高位，先右移 8 位还原 24 位有符号值。
      int32_t sample = source[index] >> 8;

      // 再缩小到 16 位量级。
      sample = sample >> 8;

      if (sample > 32767) {
        sample = 32767;
      } else if (sample < -32768) {
        sample = -32768;
      }

      destination[index] = static_cast<int16_t>(sample);
  }
}
void Application::handleServerMessage(const String& payload){
  JsonDocument doc;

  if (deserializeJson(doc, payload)) {
    return;
  }

  const char* type = doc["type"] | "";

  if (strcmp(type, "tts") == 0) {
    const char* state = doc["state"] | "";

    if (strcmp(state, "start") == 0) {
      // 只置标志，屏幕和状态由主循环处理。
      pendingTtsStart_ = true;
    } else if (strcmp(state, "stop") == 0) {
      pendingTtsStop_ = true;
    }
  } else if (strcmp(type, "stt") == 0) {
    // 复制到定长缓冲，主循环再显示。
    const char* text = doc["text"] | "";
    snprintf(pendingSttText_, sizeof(pendingSttText_), "%s", text);
    pendingSttReady_ = true;
  } else if (strcmp(type, "goodbye") == 0) {
    const char* sessionId = doc["session_id"] | "";
    if (*sessionId != '\0' && protocol_.sessionId() == sessionId) {
      // 服务器因会话超时等原因主动结束当前会话。
      // 只置标志，真正的通道关闭和状态复位在主循环里做。
      pendingServerGoodbye_ = true;
    }
  } else if (strcmp(type, "llm") == 0) {
    Serial.print("llm emotion: ");
    Serial.println(doc["emotion"] | "neutral");
  } else if (strcmp(type, "alert") == 0) {
    Serial.println("server alert received");
  }
}

// 回到待机：清空残留音频、关闭音频通道、复位状态。
// reason 只用于串口日志，方便定位是哪条路径触发的。
void Application::refreshUi() {
  switch (chatState_) {
    case ChatState::Idle:
      display_.setMode("IDLE");
      display_.setStatus("IDLE", TFT_WHITE);
      display_.setHint("click: talk  dbl: realtime");
      display_.setMicLevel(0);
      break;

    case ChatState::Connecting:
      display_.setMode("LINK");
      display_.setStatus("CONNECTING", TFT_YELLOW);
      display_.setHint("opening audio channel");
      break;

    case ChatState::Listening:
      display_.setMode("MANUAL");
      display_.setStatus("LISTENING", TFT_GREEN);
      display_.setHint("click again to send");
      break;

    case ChatState::Realtime:
      display_.setMode("REALTIME");
      display_.setStatus("LIVE", TFT_GREEN);
      display_.setHint("dbl: exit  hold: wifi");
      display_.setMessage("");
      break;

    case ChatState::Thinking:
      display_.setMode("MANUAL");
      display_.setStatus("THINKING", TFT_ORANGE);
      display_.setHint("waiting for reply");
      display_.setMicLevel(0);
      break;

    case ChatState::Speaking:
      display_.setMode("MANUAL");
      display_.setStatus("SPEAKING", TFT_CYAN);
      display_.setHint("click: interrupt");
      display_.setMicLevel(0);
      break;
  }
}

void Application::returnToIdle(const char* reason) {
  uploadEnabled_ = false;
  realtimeModeActive_ = false;
  pendingSingleClick_ = false;
  if (uploadQueue_ != nullptr) {
    xQueueReset(uploadQueue_);
  }
  if (uplinkTimestampQueue_ != nullptr) {
    xQueueReset(uplinkTimestampQueue_);
  }
  if (playbackQueue_ != nullptr) {
    xQueueReset(playbackQueue_);
  }

  // 会话已经不可用，关闭通道，下一轮按键会重新申请。
  protocol_.closeAudioChannel(false);

  pendingTtsStart_ = false;
  pendingTtsStop_ = false;
  pendingSttReady_ = false;
  pendingServerGoodbye_ = false;
  chatState_ = ChatState::Idle;
  refreshUi();

  Serial.print("back to idle: ");
  Serial.println(reason);
}
