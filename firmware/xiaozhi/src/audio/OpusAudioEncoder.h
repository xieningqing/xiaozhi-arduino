#pragma once

#include <Arduino.h>
#include <opus.h>

// 只负责把麦克风 PCM 编码成 Opus 音频帧。
// 网络发送、采样格式转换和状态判断都由 Application 负责。
class OpusAudioEncoder {
public:
  // 初始化指定采样率的单声道编码器。
  bool begin(uint32_t sampleRate, uint32_t frameDurationMs);

  // 编码一帧 PCM，返回 Opus 数据长度。
  size_t encodeFrame(const int16_t* pcmFrame,
                     size_t pcmSampleCount,
                     uint8_t* opusPacket,
                     size_t opusPacketCapacity);

private:
  OpusEncoder* encoder_ = nullptr;
  size_t expectedSampleCount_ = 0;
};
