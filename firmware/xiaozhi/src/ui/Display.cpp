#include "Display.h"

#include <cstring>

namespace {

constexpr uint16_t kBackground = TFT_BLACK;
constexpr uint16_t kHeaderBackground = 0x1082;
constexpr uint16_t kAccent = 0x07FF;
constexpr uint16_t kTextDim = 0x7BEF;
constexpr uint16_t kTextMain = TFT_WHITE;
constexpr uint16_t kBarEmpty = 0x2104;
constexpr uint16_t kLevelLow = 0x07E0;
constexpr uint16_t kLevelMid = 0xFFE0;
constexpr uint16_t kLevelHigh = 0xF800;

constexpr int16_t kHeaderHeight = 28;
constexpr int16_t kModeTop = 32;
constexpr int16_t kModeHeight = 20;
constexpr int16_t kStatusTop = 56;
constexpr int16_t kStatusHeight = 64;
constexpr int16_t kLevelTop = 128;
constexpr int16_t kLevelHeight = 24;
constexpr int16_t kMessageTop = 158;
constexpr int16_t kMessageHeight = 56;
constexpr int16_t kHintTop = 216;
constexpr int16_t kHintHeight = 24;

// 消息滚动间隔：每 1.8 秒上移一行。
constexpr uint32_t kMessageScrollIntervalMs = 1800;

}  // namespace

Display::Display() : tft_() {}

void Display::begin() {
  tft_.init();
  tft_.setRotation(0);
  tft_.fillScreen(kBackground);
  tft_.setTextWrap(false);
}

void Display::showBoot() {
  tft_.fillScreen(kBackground);

  drawCentered("XIAOZHI", 92, 4, kAccent);
  drawCentered("ARDUINO VOICE", 132, 2, kTextDim);

  // 底部渐入的三段装饰条，区别于纯文字界面。
  tft_.fillRect(60, 168, 36, 4, 0x07E0);
  tft_.fillRect(102, 168, 36, 4, 0xFFE0);
  tft_.fillRect(144, 168, 36, 4, 0xF800);

  status_[0] = '\0';
  mode_[0] = '\0';
  message_[0] = '\0';
  hint_[0] = '\0';
  wifiUp_ = false;
  serverUp_ = false;
  micLevel_ = 0;
  drawnMicLevel_ = 255;
}

void Display::showMainScreen() {
  tft_.fillScreen(kBackground);
  drawHeader();
  drawMode();
  drawStatus();
  drawLevel();
  drawMessage();
  drawHint();
  drawnMicLevel_ = micLevel_;
}

void Display::drawHeader() {
  tft_.fillRect(0, 0, kWidth, kHeaderHeight, kHeaderBackground);
  tft_.setTextSize(2);
  tft_.setTextColor(kTextMain, kHeaderBackground);
  tft_.setCursor(10, 6);
  tft_.print("XIAOZHI");

  // 连接指示：两个圆点 + 短标签，横向排在同一行。
  constexpr int16_t kDotRadius = 5;
  constexpr int16_t kDotY = 13;

  tft_.fillCircle(152, kDotY, kDotRadius,
                  wifiUp_ ? kLevelLow : 0x39E7);
  tft_.setTextSize(1);
  tft_.setTextColor(kTextDim, kHeaderBackground);
  tft_.setCursor(161, 9);
  tft_.print("WIFI");

  tft_.fillCircle(198, kDotY, kDotRadius,
                  serverUp_ ? kLevelLow : 0x39E7);
  tft_.setTextColor(kTextDim, kHeaderBackground);
  tft_.setCursor(207, 9);
  tft_.print("SRV");
}

void Display::drawMode() {
  tft_.fillRect(0, kModeTop, kWidth, kModeHeight, kBackground);
  tft_.setTextSize(2);
  tft_.setTextColor(kTextDim, kBackground);
  tft_.setCursor(10, kModeTop + 1);
  tft_.print("MODE ");

  const uint16_t modeColor =
      (strcmp(mode_, "REALTIME") == 0) ? kLevelLow
      : (strcmp(mode_, "MANUAL") == 0) ? kAccent
                                       : kTextDim;
  tft_.setTextColor(modeColor, kBackground);
  tft_.print(mode_);
}

void Display::drawStatus() {
  tft_.fillRect(0, kStatusTop, kWidth, kStatusHeight, kBackground);
  drawCentered(status_, kStatusTop + kStatusHeight / 2, 4, statusColor_);
}

void Display::drawLevel() {
  constexpr int16_t kBarLeft = 16;
  constexpr int16_t kBarWidth = 208;
  constexpr int16_t kBarHeight = 14;

  tft_.fillRect(0, kLevelTop, kWidth, kLevelHeight, kBackground);

  tft_.setTextSize(1);
  tft_.setTextColor(kTextDim, kBackground);
  tft_.setCursor(kBarLeft, kLevelTop);
  tft_.print("MIC");

  const int16_t barTop = kLevelTop + 10;
  tft_.drawRect(kBarLeft, barTop, kBarWidth, kBarHeight, 0x39E7);
  tft_.fillRect(kBarLeft + 2, barTop + 2, kBarWidth - 4, kBarHeight - 4,
                kBarEmpty);

  const int16_t innerWidth = kBarWidth - 4;
  int16_t filled = static_cast<int16_t>(innerWidth) * micLevel_ / 100;
  if (micLevel_ > 0 && filled < 2) {
    filled = 2;
  }

  const uint16_t levelColor =
      micLevel_ >= 80 ? kLevelHigh : (micLevel_ >= 45 ? kLevelMid : kLevelLow);
  if (filled > 0) {
    tft_.fillRect(kBarLeft + 2, barTop + 2, filled, kBarHeight - 4, levelColor);
  }
}

void Display::drawMessage() {
  tft_.fillRect(0, kMessageTop, kWidth, kMessageHeight, kBackground);
  tft_.drawFastHLine(10, kMessageTop, kWidth - 20, 0x2104);

  if (messageLineCount_ == 0) {
    return;
  }

  tft_.setTextSize(2);
  tft_.setTextColor(kTextMain, kBackground);

  // 固定两行窗口，从 messageLineOffset_ 开始显示。
  for (uint8_t row = 0; row < kVisibleMessageLines; ++row) {
    const uint8_t lineIndex = messageLineOffset_ + row;
    if (lineIndex >= messageLineCount_) {
      break;
    }
    tft_.setCursor(10, kMessageTop + 8 + row * 24);
    tft_.print(messageLines_[lineIndex]);
  }

  // 内容超过两行时在右下角显示当前窗口位置。
  if (messageLineCount_ > kVisibleMessageLines) {
    char page[12];
    snprintf(page, sizeof(page), "%u/%u",
             static_cast<unsigned>(messageLineOffset_ + 1),
             static_cast<unsigned>(messageLineCount_));
    tft_.setTextSize(1);
    tft_.setTextColor(kTextDim, kBackground);
    tft_.setCursor(kWidth - 34, kMessageTop + kMessageHeight - 12);
    tft_.print(page);
  }
}

void Display::drawHint() {
  tft_.fillRect(0, kHintTop, kWidth, kHintHeight, kBackground);
  tft_.setTextSize(1);
  tft_.setTextColor(kTextDim, kBackground);
  tft_.setCursor(10, kHintTop + 8);
  tft_.print(hint_);
}

void Display::drawCentered(const char* text, int16_t centerY, uint8_t size,
                           uint16_t color) {
  if (text == nullptr || text[0] == '\0') {
    return;
  }

  tft_.setTextSize(size);
  tft_.setTextColor(color, kBackground);

  const int16_t textWidth = tft_.textWidth(text);
  const int16_t textHeight = tft_.fontHeight();
  int16_t x = (kWidth - textWidth) / 2;
  if (x < 0) {
    x = 0;
  }

  tft_.setCursor(x, centerY - textHeight / 2);
  tft_.print(text);
}

void Display::sanitizeAscii(char* destination, size_t destinationSize,
                            const char* source) {
  if (destination == nullptr || destinationSize == 0) {
    return;
  }

  destination[0] = '\0';
  if (source == nullptr) {
    return;
  }

  size_t out = 0;
  bool hasNonAscii = false;

  for (size_t index = 0; source[index] != '\0' &&
                         out + 1 < destinationSize;
       ++index) {
    const uint8_t character = static_cast<uint8_t>(source[index]);
    if (character >= 0x20 && character <= 0x7E) {
      destination[out++] = static_cast<char>(character);
    } else if (character >= 0x80) {
      hasNonAscii = true;
    }
  }
  destination[out] = '\0';

  // 当前没有加载中文字库，用标记提示文本里含中文，避免显示成乱码方块。
  if (hasNonAscii) {
    constexpr size_t kPrefixLength = 5;  // "[zh] "
    size_t bodyLength = out;
    if (bodyLength + kPrefixLength + 1 > destinationSize) {
      bodyLength = destinationSize > kPrefixLength + 1
                       ? destinationSize - kPrefixLength - 1
                       : 0;
    }
    memmove(destination + kPrefixLength, destination, bodyLength);
    memcpy(destination, "[zh] ", kPrefixLength);
    destination[kPrefixLength + bodyLength] = '\0';
  }
}

void Display::showText(const char* text) {
  setStatus(text, kTextMain);
}

void Display::setStatus(const char* text, uint16_t color) {
  char sanitized[sizeof(status_)];
  sanitizeAscii(sanitized, sizeof(sanitized), text);

  if (strcmp(sanitized, status_) == 0 && color == statusColor_) {
    return;
  }

  snprintf(status_, sizeof(status_), "%s", sanitized);
  statusColor_ = color;

  // 兼容开机阶段：主界面还没搭起来时，只更新状态区域。
  drawStatus();
}

void Display::setMode(const char* mode) {
  char sanitized[sizeof(mode_)];
  sanitizeAscii(sanitized, sizeof(sanitized), mode);
  if (strcmp(sanitized, mode_) == 0) {
    return;
  }
  snprintf(mode_, sizeof(mode_), "%s", sanitized);
  drawMode();
}

void Display::setNetwork(bool wifiUp, bool serverUp) {
  if (wifiUp == wifiUp_ && serverUp == serverUp_) {
    return;
  }
  wifiUp_ = wifiUp;
  serverUp_ = serverUp;
  drawHeader();
}

void Display::setMessage(const char* text) {
  char sanitized[sizeof(message_)];
  sanitizeAscii(sanitized, sizeof(sanitized), text);
  if (strcmp(sanitized, message_) == 0) {
    return;
  }
  snprintf(message_, sizeof(message_), "%s", sanitized);
  messageLineCount_ = wrapMessage();
  messageLineOffset_ = 0;
  lastMessageScrollMs_ = millis();
  drawMessage();
}

// 按像素宽度折行，返回实际行数。最多保留 kMaxMessageLines 行。
uint8_t Display::wrapMessage() {
  for (uint8_t line = 0; line < kMaxMessageLines; ++line) {
    messageLines_[line][0] = '\0';
  }

  if (message_[0] == '\0') {
    return 0;
  }

  tft_.setTextSize(2);

  uint8_t lineCount = 0;
  size_t sourceIndex = 0;
  const size_t sourceLength = strlen(message_);
  const int16_t maxWidth = kWidth - 20;

  while (sourceIndex < sourceLength && lineCount < kMaxMessageLines) {
    char* line = messageLines_[lineCount];
    size_t out = 0;
    size_t lastSpace = 0;
    bool hasSpace = false;

    while (sourceIndex < sourceLength &&
           out + 1 < kMaxMessageLineChars) {
      line[out] = message_[sourceIndex];
      line[out + 1] = '\0';

      if (tft_.textWidth(line) > maxWidth) {
        break;
      }

      if (message_[sourceIndex] == ' ') {
        lastSpace = out;
        hasSpace = true;
      }

      ++out;
      ++sourceIndex;
    }

    // 优先在空格处断行，避免把单词从中间切开。
    if (sourceIndex < sourceLength && hasSpace && lastSpace > 0) {
      sourceIndex -= (out - lastSpace);
      out = lastSpace;
    }

    // 至少要放下一个字符，避免卡死。
    if (out == 0) {
      line[0] = message_[sourceIndex++];
      out = 1;
    }

    line[out] = '\0';
    ++lineCount;

    // 跳过断行处的空格。
    while (sourceIndex < sourceLength && message_[sourceIndex] == ' ') {
      ++sourceIndex;
    }
  }

  return lineCount;
}

void Display::tick() {
  if (messageLineCount_ <= kVisibleMessageLines) {
    return;
  }

  const uint8_t maxOffset = messageLineCount_ - kVisibleMessageLines;
  if (messageLineOffset_ >= maxOffset) {
    return;
  }

  if (millis() - lastMessageScrollMs_ < kMessageScrollIntervalMs) {
    return;
  }

  lastMessageScrollMs_ = millis();
  ++messageLineOffset_;
  drawMessage();
}

void Display::setHint(const char* hint) {
  char sanitized[sizeof(hint_)];
  sanitizeAscii(sanitized, sizeof(sanitized), hint);
  if (strcmp(sanitized, hint_) == 0) {
    return;
  }
  snprintf(hint_, sizeof(hint_), "%s", sanitized);
  drawHint();
}

void Display::setMicLevel(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  if (percent == micLevel_) {
    return;
  }
  micLevel_ = percent;

  // 限制刷新粒度，避免每一帧都刷电平条拖慢音频。
  // 归零必须立即刷新，否则停止录音后电平条会残留。
  if (percent != 0 && drawnMicLevel_ != 255 &&
      (percent > drawnMicLevel_ ? percent - drawnMicLevel_
                                : drawnMicLevel_ - percent) < 4) {
    return;
  }
  drawnMicLevel_ = percent;
  drawLevel();
}

void Display::clear() {
  tft_.fillScreen(kBackground);
}
