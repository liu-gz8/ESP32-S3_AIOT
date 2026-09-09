#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 *输入：int32立体声帧【L0 R0 L1 R1 ...】
 *输出：int16立体声帧【L0 R0 L1 R1 ...】
 *frame：帧数（不是字节数）
 */

typedef struct{
    int64_t sum_sq_l;
    int64_t sum_sq_r;
    float rms_l;
    float rms_r;
    size_t frames;
}block_rms_t;

//RMS 统计
block_rms_t audio_rms(const int16_t *pcm, size_t frames);

//原始数据解耦
void audio_convert_to_s16(const int32_t *raw, size_t frames, int16_t *out);