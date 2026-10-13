#include "OpusAudioDecoder.h"

bool OpusAudioDecoder::begin(uint32_t sampleRate) {
  int errorCode = OPUS_OK;

  decoder_ = opus_decoder_create(sampleRate, 1, &errorCode);
  if (errorCode != OPUS_OK || decoder_ == nullptr) {
    decoder_ = nullptr;
    return false;
  }

  return true;
}

size_t OpusAudioDecoder::decodeFrame(const uint8_t* opusPacket,
                                     size_t opusPacketLength,
                                     int16_t* pcmFrame,
                                     size_t pcmFrameCapacity) {
  if (decoder_ == nullptr || opusPacket == nullptr ||
      opusPacketLength == 0 || pcmFrame == nullptr ||
      pcmFrameCapacity == 0) {
    return 0;
  }

  const int decodedSamples = opus_decode(
      decoder_, opusPacket, static_cast<opus_int32>(opusPacketLength),
      pcmFrame, static_cast<int>(pcmFrameCapacity), 0);

  if (decodedSamples <= 0) {
    return 0;
  }

  return static_cast<size_t>(decodedSamples);
}
