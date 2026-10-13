#include "Microphone.h"

Microphone::Microphone(uint8_t bclkPin,uint8_t wsPin,uint8_t sdPin,uint32_t sampleRate):
bclkPin_(bclkPin),wsPin_(wsPin),sdPin_(sdPin),sampleRate_(sampleRate){}

bool Microphone::begin(){
    // 配置 I2S 通道
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER); 

    chanCfg.auto_clear = true; // 自动清除 DMA 缓冲区

    // 配置 I2S RX 参数
    if(i2s_new_channel(&chanCfg,nullptr,&rxHandle_) != ESP_OK){
        return false;
    }

    // 配置 I2S RX 的标准参数
    i2s_std_config_t stdCfg = {
        // 配置时钟参数
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate_),
        // 配置数据槽参数
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, // 数据位宽
            I2S_SLOT_MODE_MONO // 单声道模式
        ),
        // 配置 GPIO 引脚
        .gpio_cfg = {
            // MCLK 主时钟：INMP441 不需要，所以填 UNUSED。
            .mclk = I2S_GPIO_UNUSED, // 不使用 MCLK

            // BCLK 位时钟
            .bclk = (gpio_num_t)bclkPin_, // BCLK 引脚

            // ws 帧同步时钟
            .ws = (gpio_num_t)wsPin_, // WS 引脚
            
            .dout = I2S_GPIO_UNUSED, // 不使用数据输出引脚
            // 数据输入引脚
            .din = (gpio_num_t)sdPin_, // SD 引脚

            .invert_flags = {
                .mclk_inv = 0, // 不反转 MCLK
                .bclk_inv = 0, // 不反转 BCLK
                .ws_inv = 0    // 不反转 WS
            },
        },
    };

    // 初始化 I2S RX 通道为标准模式
    if(i2s_channel_init_std_mode(rxHandle_, &stdCfg) != ESP_OK){
        return false;
    }

    // 启用通道，到这里开始真正的采集
    if(i2s_channel_enable(rxHandle_) != ESP_OK){
        return false;
    }

    return true;
}

size_t Microphone::readSamples(int32_t* buffer, size_t count,
                               uint32_t timeoutMs) {
    // 参数说明：
  //   rxHandle_  ：上面创建的接收通道
  //   buffer     ：把读到的数据放到哪里
  //   count * sizeof(int32_t)：想读多少字节
  //   &bytesRead ：实际读到了多少字节
  //   超时时间：避免 I2S 异常时永久阻塞整个主循环

  size_t bytesRead = 0;
  if (i2s_channel_read(rxHandle_, buffer, count * sizeof(int32_t), &bytesRead,
                       pdMS_TO_TICKS(timeoutMs)) != ESP_OK) {
    return 0;
  }

  return bytesRead / sizeof(int32_t);

}

