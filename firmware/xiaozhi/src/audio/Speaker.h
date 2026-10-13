#pragma once

#include<Arduino.h>
#include<driver/i2s_std.h>

class Speaker {
    public:
        Speaker(uint8_t bclkPin,uint8_t lrcPin,uint8_t dinPin,uint32_t sampleRate);

        bool begin();
        size_t writeSamples(const int16_t* buffer,size_t count);
    private:
        uint8_t bclkPin_;
        uint8_t lrcPin_;
        uint8_t dinPin_;
        uint32_t sampleRate_;

        // i2s 发送信道
        i2s_chan_handle_t txHandle_ = nullptr;
};