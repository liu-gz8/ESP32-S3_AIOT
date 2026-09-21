#pragma once
#include "audio_format.h"
#include "mic_inmp441.h"

#define RAW_I2S_FRAMES 1024 /*原始数据帧帧数(适配AFE框架)*/
#define RAW_I2S_BYTES_PER_FRAME (2 * sizeof(int32_t))   //原始帧大小
#define RAW_I2S_BUFFER_SIZES (RAW_I2S_FRAMES * RAW_I2S_BYTES_PER_FRAME)

#define PCM_16_FRAMES RAW_I2S_FRAMES  /*解耦帧数*/
#define PCM_16_BYTES_PER_FRAME (2 * sizeof(int16_t))   //解耦帧大小
#define PCM_16_BUFFER_BYTES (RAW_I2S_FRAMES * PCM_16_BYTES_PER_FRAME)  //解耦缓冲区

//打印解码后音频任务
void audio_app_task(void *arg);
void seg_consumer_task(void *arg);

