#pragma once

#include <Arduino.h>
#include <opus.h>

// 只负责把服务器下发的 Opus 音频解码成 PCM。
// 网络接收、播放队列和 I2S 输出都由 Application 负责。
class OpusAudioDecoder {
public:
  // 初始化指定采样率的单声道解码器。
  bool begin(uint32_t sampleRate);

  // 解码一帧 Opus 数据，返回实际写入的 PCM 样本数。
  size_t decodeFrame(const uint8_t* opusPacket,
                     size_t opusPacketLength,
                     int16_t* pcmFrame,
                     size_t pcmFrameCapacity);

private:
  OpusDecoder* decoder_ = nullptr;
};
