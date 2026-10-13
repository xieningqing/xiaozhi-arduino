#pragma once

#include<Arduino.h>
#include<driver/i2s_std.h>

class Microphone{
    public:
        Microphone(uint8_t blckPin, uint8_t wsPin, uint8_t sdPin, uint32_t sampleRate);

        bool begin();
        size_t readSamples(int32_t* buffer, size_t count,
                           uint32_t timeoutMs = 100);

    private:
        uint8_t bclkPin_;
        uint8_t wsPin_;
        uint8_t sdPin_;
        uint32_t sampleRate_;
        i2s_chan_handle_t rxHandle_ = nullptr;
};
