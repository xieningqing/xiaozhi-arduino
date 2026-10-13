#include "Speaker.h"

Speaker::Speaker(uint8_t bclkPin,uint8_t lrcPin,uint8_t dinPin,uint32_t sampleRate)
: bclkPin_(bclkPin),lrcPin_(lrcPin),dinPin_(dinPin),sampleRate_(sampleRate){}

bool Speaker::begin(){
    // 麦克风用的是 I2S_NUM_0，这里必须用 I2S_NUM_1，否则两个通道会冲突。
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);

    chanCfg.auto_clear = true;

    if(i2s_new_channel(&chanCfg,&txHandle_,nullptr) != ESP_OK) {
        return false;
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate_),

        // MAX98357A 默认工作在标准 I2S（Philips）模式，
        // 不能使用 PCM 短帧配置，否则没有声音或只有杂音。
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_MONO),
        
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // 不需要主时钟
            .bclk = (gpio_num_t) bclkPin_, // 位时钟
            .ws = (gpio_num_t) lrcPin_, // 声道时钟
            .dout = (gpio_num_t) dinPin_, // 输出
            .din = I2S_GPIO_UNUSED, // 功放不接受数据


            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            },
        },
    };

    if(i2s_channel_init_std_mode(txHandle_,&stdCfg) != ESP_OK){
        return false;
    }

    if(i2s_channel_enable(txHandle_) != ESP_OK){
        return false;
    }
    return true;
}

size_t Speaker::writeSamples(const int16_t* buffer,size_t count){
    size_t bytesWritten = 0;
    if(i2s_channel_write(txHandle_,buffer,count*sizeof(int16_t),
                        &bytesWritten,portMAX_DELAY) != ESP_OK){
        return 0;
    }

    return bytesWritten / sizeof(int16_t);

}
