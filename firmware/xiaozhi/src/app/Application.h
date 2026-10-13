#pragma once

#include "../core/Config.h"
#include "../hal/Button.h"
#include "../ui/Display.h"

#include "../audio/Microphone.h"
#include "../audio/OpusAudioDecoder.h"
#include "../audio/Speaker.h"
#include "../net/Network.h"
#include "../net/MqttProtocol.h"
#include "../audio/OpusAudioEncoder.h"

class Application{
  public:
     void begin();
     void loop();

  private:
     // 全局对象创建时就传入引脚和消抖时间
     Button button_{config::kBootButtonPin,config::kDebounceMs};
     Display display_; // 屏幕
     XiaozhiNetwork network_; // 网络模块：WiFi、NTP、OTA
     MqttProtocol protocol_;  // 小智 MQTT 通道

     Microphone mic_{config::kMicBclkPin,config::kMicWsPin,
                      config::kMicSdPin,config::kMicSampleRate};

     OpusAudioEncoder opusEncoder_;

    //上行时间戳
    uint32_t uploadTimestampMs_ = 0;

    // 进入 Thinking 的时刻，用于超时判断。
    uint32_t thinkingStartMs_ = 0;

    Speaker spk_{config::kSpkBclkPin,config::kSpkLrcPin,config::kSpkDinPin,config::kSpkSampleRate};

    bool lastButtonPressed_ = false;

    // MQTT 重连节流，避免断开时疯狂重连。
    uint32_t lastReconnectAttemptMs_ = 0;
    bool mqttConfigured_ = false;

    // 长按 BOOT 会写入配网标记并重启。
    uint32_t bootButtonPressedAtMs_ = 0;
    bool configPortalRequested_ = false;
    bool suppressButtonRelease_ = false;

    // Idle/Realtime 下短暂延迟单击，用来区分双击。
    bool pendingSingleClick_ = false;
    uint32_t pendingSingleClickAtMs_ = 0;

    // 上传任务算出的麦克风电平百分比，主循环读取后刷新电平条。
    volatile uint8_t micLevelPercent_ = 0;

     // 服务器下发的加密音频包会在网络回调里进入这个队列。
     struct PlaybackPacket {
       uint32_t timestamp = 0;
       uint16_t length = 0;
       uint8_t data[config::kMaxOpusPacketBytes] = {};
     };

     OpusAudioDecoder opusDecoder_;
     QueueHandle_t playbackQueue_ = nullptr;
     TaskHandle_t playbackTaskHandle_ = nullptr;

     // 麦克风和 Opus 编码在独立任务中运行，实时模式下不会阻塞网络主循环。
     struct UploadPacket {
       uint16_t length = 0;
       uint32_t timestamp = 0;
       uint8_t data[config::kMaxOpusPacketBytes] = {};
     };

     QueueHandle_t uploadQueue_ = nullptr;
     // 服务端 AEC 需要上行时间戳与下行播放时间戳对齐。
     // 播放任务每播完一帧就压入该帧时间戳，上传任务编码时取出一个，
     // 与官方 AudioService 的 timestamp_queue_ 行为一致。
     QueueHandle_t uplinkTimestampQueue_ = nullptr;
     // 上传任务运行在另一个核心，用独立标志判断是否实时模式，
     // 避免跨核心直接读取状态机枚举。
     volatile bool realtimeModeActive_ = false;
     TaskHandle_t uploadTaskHandle_ = nullptr;
     volatile bool uploadEnabled_ = false;

     static void playbackTaskEntry(void* argument);
     void playbackTask();

     static void uploadTaskEntry(void* argument);
     void uploadTask();
     void drainUploadQueue();

     void enqueueIncomingAudioPacket(
         const uint8_t* decryptedOpusData,
         size_t decryptedOpusLength,
         uint32_t audioTimestamp);

    enum class ChatState{
      Idle, //待机
      Connecting, //正在申请udp音频通道
      Listening,  //录音上传
      Realtime,   // 全双工：持续上传并同时播放
      Thinking,   // 已停止录音，等待服务器识别和回复
      Speaking    // 正在播放服务器返回的语音
    };

    ChatState chatState_ = ChatState::Idle;

    // MQTT 事件在另一个任务里运行，不能直接刷屏或改状态。
    // 回调只记录这些标志，主循环再统一处理。
    volatile bool pendingTtsStart_ = false;
    volatile bool pendingTtsStop_ = false;

    // 服务器主动发来 goodbye，说明会话已失效。
    // 由主循环关闭音频通道并回到待机，否则会一直卡在 Thinking。
    volatile bool pendingServerGoodbye_ = false;

    // 识别文本使用定长缓冲，避免两个任务同时操作 String 造成堆竞争。
    char pendingSttText_[128] = {};
    volatile bool pendingSttReady_ = false;

    //按键松开调用，用来切换对话状态
    void handleButtonRelease();

    // 开始聆听：确保音频通道已打开，并发送 listen start。
    void startListening();

    // 停止聆听：
    void stopListening();

    // 进入/退出全双工实时模式。
    void enterRealtimeMode();
    void exitRealtimeMode(const char* reason);

    // 双击 BOOT 时切换实时模式。
    void handleDoubleClick();

    // 双击窗口结束后执行单击动作，避免重复进入双击判定。
    void handleSingleClick();

    // 把 INMP441 的 32 位原始数据转换成 16 位 PCM。
    void convertMicrophoneFrameToPcm(
        const int32_t* source,
        int16_t* destination,
        size_t sampleCount);
    
    // 处理服务端 tts、stt、llm、alert、goodbye 消息。
    void handleServerMessage(const String& payload);

    // 根据当前状态刷新屏幕的状态、模式和操作提示。
    void refreshUi();

    // 回到待机：清空残留音频、关闭音频通道、复位状态。
    // reason 只用于串口日志，方便定位是哪条路径触发的。
    void returnToIdle(const char* reason);
};
