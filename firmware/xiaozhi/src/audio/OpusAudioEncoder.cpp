#include "OpusAudioEncoder.h"

bool OpusAudioEncoder::begin(uint32_t sampleRate, uint32_t frameDurationMs) {
  int errorCode = OPUS_OK;

  // 小智上行音频使用 16kHz、单声道、语音模式。
  encoder_ = opus_encoder_create(sampleRate, 1, OPUS_APPLICATION_VOIP,
                                 &errorCode);
  if (errorCode != OPUS_OK || encoder_ == nullptr) {
    encoder_ = nullptr;
    return false;
  }

  // 24000 bit/s 适合 16kHz 语音，complexity 0 降低 CPU 占用。
  opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(24000));
  opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(0));
  opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

  expectedSampleCount_ = sampleRate * frameDurationMs / 1000;
  return expectedSampleCount_ > 0;
}

size_t OpusAudioEncoder::encodeFrame(const int16_t* pcmFrame,
                                     size_t pcmSampleCount,
                                     uint8_t* opusPacket,
                                     size_t opusPacketCapacity) {
  if (encoder_ == nullptr || pcmFrame == nullptr ||
      opusPacket == nullptr || opusPacketCapacity == 0) {
    return 0;
  }

  // Opus 要求每次编码的样本数正好等于一个完整帧。
  if (pcmSampleCount != expectedSampleCount_) {
    return 0;
  }

  const opus_int32 encodedBytes = opus_encode(
      encoder_, pcmFrame, static_cast<int>(pcmSampleCount), opusPacket,
      static_cast<opus_int32>(opusPacketCapacity));

  // 返回 2 字节或更少表示 DTX，当前不需要发送这种空帧。
  if (encodedBytes <= 2) {
    return 0;
  }

  return static_cast<size_t>(encodedBytes);
}
