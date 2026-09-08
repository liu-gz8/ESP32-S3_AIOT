#pragma once

#include "sdkconfig.h"
#include "soc/gpio_num.h"

/* 
I2S引脚定义（ESP32S3）
双总线：01发送 02接收 
*/
#define MIC_I2S_BCLK_STDIO1 GPIO_NUM_6
#define MIC_I2S_WS_STDIO1 GPIO_NUM_7
#define MIC_I2S_DOUT_STDIO1 GPIO_NUM_8
#define MIC_I2S_DIN_STDIO1 GPIO_NUM_9

#define MIC_I2S_BCLK_STDIO2 GPIO_NUM_10
#define MIC_I2S_WS_STDIO2 GPIO_NUM_11
#define MIC_I2S_DOUT_STDIO2 GPIO_NUM_12
#define MIC_I2S_DIN_STDIO2 GPIO_NUM_13

