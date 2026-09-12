/*音频输出(功放/播报):I2S0 全双工 TX 通道 + MAX98357A*/

#include <stdio.h>
#include <math.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "board_config.h"
#include "audio_out.h"
#include "mic_inmp441.h"

#define TAG                        "audio_out"
#define TX_WRITE_TIMEOUT_MS        1000
#define PLAY_LOCK_TIMEOUT_MS       1000
#define PLAY_TAIL_SILENCE_MS       50      /* 等最后一段数据真正播完 */

static volatile bool      s_playing;                  /* 半双工协调标志 */
static i2s_chan_handle_t  s_tx_chan;                  /* 缓存 TX 句柄 */
static int32_t            s_tx_buf[TX_FRAME_COUNT * 2]; /* 32bit 立体声槽位缓冲(2KB, BSS) */
static SemaphoreHandle_t  s_lock;                     /* 播放互斥 */

/*------------------------------------------------------------------
 * 写一块 PCM:16bit 单声道 → 32bit 立体声槽位(左右写同一样本)
 * 16bit 样本左移 16 位填进 32bit 槽位,与麦克风链路(64fs)保持一致
 * samples 可大于 TX_FRAME_COUNT,内部自动分块
 *-----------------------------------------------------------------*/
static esp_err_t audio_out_write_chunk(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) return ESP_ERR_INVALID_ARG;
    if (!s_tx_chan)           return ESP_ERR_INVALID_STATE;

    size_t done = 0;

    while (done < samples)
    {
        size_t n = (samples - done) > TX_FRAME_COUNT ? TX_FRAME_COUNT : (samples - done);

        for (size_t i = 0; i < n; i++)
        {
            int32_t v = ((int32_t)pcm[done + i]) << 16;   /* 16bit → 32bit 左对齐 */
            s_tx_buf[2 * i]     = v;                      /* 左声道 */
            s_tx_buf[2 * i + 1] = v;                      /* 右声道 */
        }

        size_t bytes = n * 2 * sizeof(int32_t);
        size_t sent  = 0;

        /* i2s_channel_write 可能只写一部分,循环补齐 */
        while (sent < bytes)
        {
            size_t written = 0;
            esp_err_t ret = i2s_channel_write(s_tx_chan,
                                              (uint8_t *)s_tx_buf + sent,
                                              bytes - sent,
                                              &written,
                                              pdMS_TO_TICKS(TX_WRITE_TIMEOUT_MS));
            if (ret != ESP_OK) return ret;
            if (written == 0)  return ESP_ERR_TIMEOUT;
            sent += written;
        }

        done += n;
    }

    return ESP_OK;
}

esp_err_t audio_out_init(void)
{
    gpio_config_t cfg =
    {
        .pin_bit_mask = 1ULL << AUDIO_AMP_EN_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) return ret;

    gpio_set_level(AUDIO_AMP_EN_GPIO, 0);   /* 默认关断功放 */

    s_tx_chan = mic_get_tx_chan();
    if (!s_tx_chan)
    {
        ESP_LOGE(TAG, "获取 I2S TX 句柄失败");
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
    {
        ESP_LOGE(TAG, "创建播放互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    s_playing = false;
    ESP_LOGI(TAG, "init ok (AMP_EN=GPIO%d)", AUDIO_AMP_EN_GPIO);
    return ESP_OK;
}

esp_err_t audio_out_play(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(PLAY_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "有播放在进行,本次丢弃");
        return ESP_ERR_INVALID_STATE;
    }

    s_playing = true;
    audio_out_set_enable(true);

    esp_err_t ret = audio_out_write_chunk(pcm, samples);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "写 I2S 失败: %s", esp_err_to_name(ret));
    }

    vTaskDelay(pdMS_TO_TICKS(PLAY_TAIL_SILENCE_MS));
    audio_out_set_enable(false);
    s_playing = false;

    xSemaphoreGive(s_lock);
    return ret;
}

void audio_out_set_enable(bool on)
{
    gpio_set_level(AUDIO_AMP_EN_GPIO, on ? 1 : 0);
}

bool audio_out_is_playing(void)
{
    return s_playing;
}

/*------------------------------------------------------------------
 * 播放 storage 分区中的 16kHz/16bit/单声道 原始 PCM 文件
 * 例:audio_out_play_file("/spiffs/wake.pcm")
 *-----------------------------------------------------------------*/
esp_err_t audio_out_play_file(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        ESP_LOGE(TAG, "打开文件失败: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(PLAY_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "有播放在进行,本次丢弃");
        fclose(fp);
        return ESP_ERR_INVALID_STATE;
    }

    s_playing = true;
    audio_out_set_enable(true);
    vTaskDelay(pdMS_TO_TICKS(30));

    int16_t chunk[TX_FRAME_COUNT];      /* 512 字节,栈上安全 */
    size_t  n = 0;
    esp_err_t ret = ESP_OK;

    while ((n = fread(chunk, sizeof(int16_t), TX_FRAME_COUNT, fp)) > 0)
    {
        ret = audio_out_write_chunk(chunk, n);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "播放中断: %s", esp_err_to_name(ret));
            break;
        }
    }
    if (ferror(fp)) ret = ESP_FAIL;

    fclose(fp);

    vTaskDelay(pdMS_TO_TICKS(PLAY_TAIL_SILENCE_MS));
    audio_out_set_enable(false);
    s_playing = false;

    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t audio_out_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (freq_hz == 0 || duration_ms == 0) return ESP_ERR_INVALID_ARG;

    size_t total = (size_t)TONE_SAMPLE_RATE * duration_ms / 1000;
    size_t fade  = (size_t)TONE_SAMPLE_RATE * TONE_FADE_MS / 1000;
    if (fade == 0) fade = 1;
    if (fade * 2 >= total) fade = total / 8 ? total / 8 : 1;   /* 极短音保护 */

    int16_t *buf = heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;

    float w = TWO_PI * (float)freq_hz / (float)TONE_SAMPLE_RATE;

    for (size_t n = 0; n < total; n++)
    {
        float v = sinf(w * (float)n) * (float)TONE_AMPLITUDE;

        if (n < fade)                     v *= (float)n / (float)fade;
        else if (n >= total - fade)       v *= (float)(total - n) / (float)fade;

        buf[n] = (int16_t)v;
    }

    esp_err_t ret = audio_out_play(buf, total);
    heap_caps_free(buf);
    return ret;
}
