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
static i2s_chan_handle_t i2s_tx_chan; //I2S发送通道句柄

void i2s_init_std_simplex(void)
{
    //I2S接收通道配置
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL,&i2s_rx_chan)); // 创建I2S接收通道
    //I2S发送通道配置
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &i2s_tx_chan,NULL));  // 创建I2S发送通道

    //I2S发送通道标准模式配置
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),//I2S发送通道标准模式配置，采样率为16kHz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),//I2S发送通道标准模式配置
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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan, &tx_std_cfg)); //初始化I2S发送通道的标准模式

    //I2S接收通道标准模式配置
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),//I2S接收通道标准模式配置，采样率为44.1kHz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),//I2S接收通道标准模式配置
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, //MCLK未使用
            .bclk = MIC_I2S_BCLK_STDIO2,  //BCLK引脚
            .ws   = MIC_I2S_WS_STDIO2,  //WS引脚
            .dout = MIC_I2S_DOUT_STDIO2,  //DOUT引脚
            .din  = MIC_I2S_DIN_STDIO2,  //DIN引脚
            .invert_flags = {
                .mclk_inv = false,//MCLK不反相
                .bclk_inv = false,//BCLK不反相
                .ws_inv   = false,//WS不反相
            },
        },
    };
    rx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH; //设置I2S接收通道的槽掩码为同时接收左右声道数据
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, &rx_std_cfg)); //初始化I2S接收通道的标准模式
}

//I2S读取任务函数
void i2s_read_task(void *arg)
{
    uint32_t *r_buf = (uint32_t *)calloc(1, BUFFER_SIZE); //分配缓冲区内存
    assert(r_buf); //断言缓冲区内存分配成功
    size_t r_bytes = 0; //实际读取的字节数

    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan)); //启用I2S接收通道

    while(1)
    {
        if(i2s_channel_read(i2s_rx_chan, r_buf, BUFFER_SIZE, &r_bytes, portMAX_DELAY) == ESP_OK) //从I2S接收通道读取数据
        {
            //处理接收到的数据
            printf("Received %d bytes of data\n", r_bytes); //打印接收到的数据字节数
            printf("[0] %x [1] %x [2] %x [3] %x [4] %x [5] %x [6] %x [7] %x\n\n",
                 r_buf[0], r_buf[1], r_buf[2], r_buf[3], r_buf[4], r_buf[5], r_buf[6], r_buf[7]); //打印接收到的数据的前8个字节
        }else
        {
            printf("Error reading data from I2S channel\n"); //打印读取数据错误信息
        }
        vTaskDelay(pdMS_TO_TICKS(200)); //延时200毫秒

    }
    free(r_buf); //释放缓冲区内存
    vTaskDelete(NULL); //删除当前任务
}

//I2S写入任务函数
void i2s_write_task(void *arg)
{
    uint32_t *w_buf = (uint32_t *)calloc(1, BUFFER_SIZE); //分配缓冲区内存
    assert(w_buf); //断言缓冲区内存分配成功

    for(int i = 0; i < BUFFER_SIZE; i+=8)
    {
        w_buf[i] = 0x12; //填充缓冲区数据
        w_buf[i+1] = 0x34;
        w_buf[i+2] = 0x56;
        w_buf[i+3] = 0x78;
        w_buf[i+4] = 0x9A;
        w_buf[i+5] = 0xBC;
        w_buf[i+6] = 0xDE;
        w_buf[i+7] = 0xF0;
    }

    size_t w_bytes = BUFFER_SIZE; //实际写入的字节数

    while(w_bytes == BUFFER_SIZE)
    {
        ESP_ERROR_CHECK(i2s_channel_preload_data(i2s_tx_chan, w_buf, BUFFER_SIZE, &w_bytes)); //预加载数据到I2S发送通道
    }

    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan)); //启用I2S发送通道
    while(1)
    {
        if(i2s_channel_write(i2s_tx_chan, w_buf, BUFFER_SIZE, &w_bytes, 1000) == ESP_OK) //向I2S发送通道写入数据
        {
            //处理发送的数据
            printf("Sent %d bytes of data\n", w_bytes); //打印发送的数据字节数
        }else
        {
            printf("Error writing data to I2S channel\n"); //打印写入数据错误信息
        }
        vTaskDelay(pdMS_TO_TICKS(200)); //延时200毫秒
    }
    free(w_buf); //释放缓冲区内存
    vTaskDelete(NULL); //删除当前任务
}