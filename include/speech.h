#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

/*
 * ESP-SR 语音前端对外接口(AFE + WakeNet)
 *
 * 数据流:
 *   mic_inmp441(32bit 槽, 立体声)
 *     -> audio_format 解码(int16 立体声)
 *     -> speech_feed()            [AFE 内部: AEC/SE/NS/VAD + WakeNet]
 *     -> speech_take_frame()      取出"新"的一帧干净单声道 16k int16
 *     -> 你的 VAD / audio_segment / MultiNet
 *
 * 职责边界:
 *   - 本模块只负责"脏音频 -> 干净音频 + 唤醒事件"
 *   - 采集(读 I2S)仍在 mic_inmp441;片段存储仍在 audio_segment;播报仍在 audio_out
 *   - 命令词识别(MultiNet)不在本模块,由上层用 speech_take_frame() 的输出喂
 */

typedef enum
{
    SPEECH_EVT_NONE = 0,   /* 本帧没有事件 */
    SPEECH_EVT_WAKE        /* 本帧命中唤醒词 */
} speech_event_t;

typedef enum
    {
        SPEECH_CMD_WAKENET_OFF,
        SPEECH_CMD_WAKENET_ON
    }speech_cmd_t;

/*
 * 初始化:挂载模型分区 -> 建 AFE -> 建 WakeNet。
 * 返回 ESP_ERR_NOT_FOUND 表示模型分区或唤醒词模型没找到(先查 partitions.csv 与 sdkconfig)
 * 返回 ESP_ERR_NO_MEM 表示 PSRAM 不够(AFE 约占 1~2MB)
 */
esp_err_t speech_init(void);

/*
 * 半双工开关:播放提示音前关唤醒,播完再开。
 * 必须成对调用;关闭期间可以继续 feed,也可以暂停。
 */
void speech_set_wakenet(bool on);

/*创建PCM队列和speech任务，在app_main里调用*/
esp_err_t speech_start(void);

/*采集任务将投递一块数据（256帧立体声），队列满返回ESP_ERR_NO_MEM,不阻塞*/
esp_err_t speech_push_pcm(const int16_t *pcm_stereo, size_t frames);

const int16_t *speech_last_frame(size_t *frames);