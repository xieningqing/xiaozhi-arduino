#pragma once

#include<Arduino.h>

namespace config{
  // USB CDC 模式下波特率由主机决定，不需要在这里配置
  constexpr uint32_t kSerialBaud = 115200;
  // boot 按键
  constexpr uint8_t kBootButtonPin = 0;

   // 软件消抖时间
   constexpr uint32_t kDebounceMs = 30;

   //麦克风
   constexpr uint8_t kMicBclkPin = 5;
   constexpr uint8_t kMicWsPin = 4;
   constexpr uint8_t kMicSdPin = 6;
   constexpr uint32_t kMicSampleRate = 16000;

   // 256 个样本 @16000Hz ≈ 16ms，够短，方便观察实时变化。
   constexpr size_t kMicReadSamples = 256;
   constexpr uint32_t kMicPrintIntervalMs = 100; // 间隔时间

    // 功放（MAX98357A，I2S）
    constexpr uint8_t kSpkBclkPin = 15;
    constexpr uint8_t kSpkLrcPin = 16;
    constexpr uint8_t kSpkDinPin = 7;
    constexpr uint32_t kSpkSampleRate = 24000; // 24kHz，MAX98357A 支持 8kHz~96kHz

    constexpr const char* kOtaUrl = "https://api.tenclass.net/xiaozhi/ota/";
   // WiFi 连接超时（毫秒）
   constexpr uint32_t kWifiTimeoutMs = 20000;
   // 配网热点前缀，完整名称会附加 MAC 后四位。
   constexpr const char* kWifiConfigApPrefix = "Xiaozhi-";
   // 运行中长按 BOOT 超过这个时间，重启进入配网模式。
   constexpr uint32_t kWifiConfigLongPressMs = 3000;
   // 两次短按落在这个窗口内视为双击，切换全双工实时模式。
   constexpr uint32_t kDoubleClickWindowMs = 450;

   // NTP 服务器，用于同步时间
   constexpr const char* kNtpServer = "ntp.aliyun.com";

   // 固件版本，会发给服务器
   constexpr const char* kFirmwareName = "xiaozhi-arduino/0.5.0";

  constexpr uint32_t kPlaybackSampleRate = 24000;
  constexpr size_t kPlaybackFrameSamples = 1440; //24000*0.06
  constexpr size_t kPlaybackQueueLength = 8;
  // 上行实时音频队列，队列满时丢弃最旧帧，避免阻塞麦克风任务。
  constexpr size_t kUploadQueueLength = 8;
  constexpr size_t kMaxOpusPacketBytes = 512;
  constexpr size_t kMaxEncryptedAudioPacketBytes = 1024;
  constexpr uint32_t kAudioChannelOpenTimeoutMs = 10000;
  // 官方协议要求音频通道超过 120 秒没有收到任何服务端数据就判为失效。
  constexpr uint32_t kAudioChannelTimeoutMs = 120000;
  // MQTT 断开后不要频繁重连，官方默认间隔为 60 秒。
  constexpr uint32_t kMqttReconnectIntervalMs = 60000;
  // 上行音频：16kHz、单声道、60ms 一帧。
  constexpr size_t  kMicFrameSamples = 960;
  constexpr uint32_t kAudioFrameDurationMs = 60;

  // opus最大字节数
  constexpr size_t kMaxEncodeOpusBytes = 512;
  
  //主循环等待麦克风超时时间
  constexpr uint32_t kMicReadTimeoutMs = 100;

  // 进入 Thinking 后等待服务器回复的最长时间。
  // 官方没有单独的 10 秒限制，这里与音频通道无数据超时保持一致。
  constexpr uint32_t kThinkingTimeoutMs = kAudioChannelTimeoutMs;
}
