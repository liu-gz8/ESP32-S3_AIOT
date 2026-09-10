#pragma once

#include <stdint.h>

#define VAD_THRESHOLD 8 //高于噪声底多少作为有效音频
#define  VAD_ONSET_FRAME 3  //大于多少帧判定起始帧  3*16ms
#define  VAD_OFFSET_FRAME 18  //大于多少帧判定结束帧 18*16ms
#define VAD_NOISE_ALPHA 0.96 //噪声底 EMA 系数(越大越稳、越迟钝)
#define VAD_MIN_SPEECH_FRAMES 6 //最短语音判断条件
#define VAD_MAX_SPEECH_FRAMES 1875 //最长语音判断条件  30s
#define VAD_NOISE_FLOOR_INIT_DB 30 //上电初始底噪


//语音状态
typedef enum
{
    VAD_STATE_SILENCE = 0,
    VAD_STATE_SPEEK
}vad_state_t;

//vad事件状态
typedef enum
{
    VAD_EVENT_NONE = 0,
    VAD_EVENT_START,
    VAD_EVENT_END
}vad_enent_t;

typedef struct
{
    float voice_db;  //声音分贝
    float noise_db;  //底噪分贝
    vad_state_t state;  //帧数段状态
    int32_t hit_cnt; //连续高于阈值帧数
    int32_t low_cnt; //连续低于阈值帧数
    int32_t speech_frames;  //语音段长度帧数
}vad_t;

//初始化vad
/*
 *vad ：初始化所用参数块
 */
void vad_init(vad_t* vad);

//事件判断
/*
 *vad：事件判断参考参数块
 *rms_l：左声道能量（db）
 *rms_r: 右声道能量（db）
 */
vad_enent_t vad_process(vad_t* vad, float rms_l, float rms_r);

