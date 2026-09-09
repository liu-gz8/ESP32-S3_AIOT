/*解码 + RMS*/

#include "audio_format.h"

#include <math.h>
#include <string.h>

//原始信号I2S解耦函数
/*====================================================================
 *raw：传入原始数据32槽（24有效数据）
 *frames ：传入帧数
 *out ：输出16bit数据
  ====================================================================*/
void audio_convert_to_s16(const int32_t *raw, size_t frames, int16_t *out)
{
    for (size_t i = 0; i < frames; i++) // 每帧包含两个样本（左声道和右声道）
    {
        out[2 * i] = (int16_t)(raw[2 * i] >> 16); // 将32位样本右移16位并转换为16位
        out[2 * i + 1] = (int16_t)(raw[2 * i + 1] >> 16); // 将32位样本右移16位并转换为16位
    }
}

//RMS 统计函数
/*==============================================================
 *pcm : 16bit样本数据串
 *frames : 统计帧数
  ==============================================================*/
block_rms_t audio_rms(const int16_t *pcm, size_t frames)
{
    block_rms_t st = {};
    st.frames = frames;
    
    for(size_t i = 0; i < frames; i++)
    {
        int32_t l = pcm[2 * i];
        int32_t r = pcm[2 * i + 1];
        st.sum_sq_l += (int64_t)l * l;
        st.sum_sq_r += (int64_t)r * r;
    }
    st.rms_l = sqrtf((float)st.sum_sq_l / st.frames);
    st.rms_r = sqrtf((float)st.sum_sq_r / st.frames);

    return st;
}
