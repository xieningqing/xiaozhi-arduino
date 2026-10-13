#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// 240x240 状态面板。
//
// 布局：
//   y   0.. 28  标题栏 + WiFi / 服务连接指示
//   y  32.. 50  模式行（IDLE / MANUAL / REALTIME）
//   y  56..120  大号状态文字
//   y 130..152  麦克风电平条
//   y 162..212  消息区（识别文本等，最多两行）
//   y 216..238  操作提示
//
// 每个区域只在内容变化时重绘，避免播放音频期间整屏刷新造成闪烁。
class Display {
 public:
  Display();

  void begin();

  // 兼容旧调用：等价于设置大号状态文字。
  void showText(const char* text);
  void clear();

  void showBoot();

  // 首次进入主界面时全量绘制所有区域。
  void showMainScreen();

  void setStatus(const char* text, uint16_t color);
  void setMode(const char* mode);
  void setNetwork(bool wifiUp, bool serverUp);
  void setMessage(const char* text);
  void setHint(const char* hint);
  void setMicLevel(uint8_t percent);

  // 消息超过两行时向上滚动一行，由主循环持续调用。
  void tick();

 private:
  static constexpr int16_t kWidth = 240;
  static constexpr int16_t kHeight = 240;
  static constexpr uint8_t kVisibleMessageLines = 2;
  static constexpr uint8_t kMaxMessageLines = 12;
  static constexpr size_t kMaxMessageLineChars = 32;

  TFT_eSPI tft_;

  char status_[24] = "BOOT";
  char mode_[14] = "";
  char message_[128] = "";
  char hint_[48] = "";
  uint16_t statusColor_ = 0xFFFF;
  bool wifiUp_ = false;
  bool serverUp_ = false;
  uint8_t micLevel_ = 0;
  uint8_t drawnMicLevel_ = 255;

  // 折行后的消息，以及当前两行窗口的起始行。
  char messageLines_[kMaxMessageLines][kMaxMessageLineChars] = {};
  uint8_t messageLineCount_ = 0;
  uint8_t messageLineOffset_ = 0;
  uint32_t lastMessageScrollMs_ = 0;

  void drawHeader();
  void drawMode();
  void drawStatus();
  void drawLevel();
  void drawMessage();
  void drawHint();
  uint8_t wrapMessage();
  void drawCentered(const char* text, int16_t centerY, uint8_t size,
                    uint16_t color);

  static void sanitizeAscii(char* destination, size_t destinationSize,
                            const char* source);
};
