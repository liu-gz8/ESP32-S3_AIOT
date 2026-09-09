/*INMP441驱动*/
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

#include "mic_inmp441.h"
#include "board_config.h"

static i2s_chan_handle_t i2s_rx_chan; //I2S接收通道句柄


//麦克风初始化函数
esp_err_t mic_init(void)
{
    //I2S接收通道配置
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL,&i2s_rx_chan)); // 创建I2S接收通道

    //I2S接收通道标准模式配置
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE_HZ),//I2S接收通道标准模式配置，采样率为16kHz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),//I2S接收通道标准模式配置
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, //MCLK未使用
            .bclk = MIC_I2S_BCLK_STDIO1,  //BCLK引脚
            .ws   = MIC_I2S_WS_STDIO1,  //WS引脚
            .dout = MIC_I2S_DOUT_STDIO1,  //DOUT引脚
            .din  = MIC_I2S_DIN_STDIO1,  //DIN引脚
            .invert_flags = {
                .mclk_inv = false,//MCLK不反相
                .bclk_inv = false,//BCLK不反相
                .ws_inv   = false,//WS不反相
            },
        },
    };
    rx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH; //设置I2S接收通道的槽掩码为同时接收左右声道数据
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, &rx_std_cfg)); //初始化I2S接收通道的标准模式

    return i2s_channel_enable(i2s_rx_chan);
}

//I2S读取任务函数
/*=================================================
 *r_buf :数据接收缓冲区
 *r_bytes :读取的字节数
 ====================================================*/
esp_err_t mic_read_frame(int32_t *r_buf, const size_t r_bytes)
{
    return i2s_channel_read(i2s_rx_chan, r_buf, r_bytes, NULL, portMAX_DELAY);
}
