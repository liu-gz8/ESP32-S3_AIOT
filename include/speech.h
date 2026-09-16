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

/*
 * 初始化:挂载模型分区 -> 建 AFE -> 建 WakeNet。
 * 返回 ESP_ERR_NOT_FOUND 表示模型分区或唤醒词模型没找到(先查 partitions.csv 与 sdkconfig)
 * 返回 ESP_ERR_NO_MEM 表示 PSRAM 不够(AFE 约占 1~2MB)
 */
esp_err_t speech_init(void);

/*
 * 喂一帧立体声 16bit PCM
 *   pcm_stereo: [L0 R0 L1 R1 ...],与 mic_read_frame 读到的帧数一致(当前 256)
 *   frames    : 每声道的点数(不是字节数)
 * 内部攒够 AFE 要求的帧长(预计 512)才真正 feed/fetch,返回本帧事件。
 * 节拍必须跟着 I2S 走,不要用 vTaskDelay 凑时间。
 */
speech_event_t speech_feed(const int16_t *pcm_stereo, size_t frames);

/*
 * 取一帧"新"的 AFE 输出(干净单声道 16k int16)
 *   out   : 回填这一帧的首地址
 *   frames: 回填这一帧的点数(AFE 一帧约 512 点 = 32ms)
 *
 * 返回 true : *out / *frames 有效,且这一帧只会被取出一次(取走后标记清除)
 * 返回 false: 还没有新帧,*frames 置 0,不要复用上一次拿到的指针
 *
 * 为什么必须有"新帧"语义:
 *   AFE 要攒够 chunk(约 512 点)才产出一帧,而采集侧每次只喂 256 点,
 *   所以每两次 speech_feed() 才有一帧新数据。如果用"取最新一帧"的接口,
 *   没新帧的那次调用会拿到上一帧的旧数据,同一段音频会被重复处理一遍
 *   (片段重复、时长统计翻倍)。
 *
 * 注意:指针指向模块内部缓冲,下一次取到新帧时会被覆盖,不要长期持有。
 */
bool speech_take_frame(const int16_t **out, size_t *frames);

/*
 * 半双工开关:播放提示音前关唤醒,播完再开。
 * 必须成对调用;关闭期间可以继续 feed,也可以暂停。
 */
void speech_set_wakenet(bool on);

/* 调试:打印实际生效的处理流水线和模型信息 */
//void speech_dump(void);
