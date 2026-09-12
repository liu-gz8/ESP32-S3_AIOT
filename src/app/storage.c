#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"

#include "storage.h"

#define TAG                 "storage"
#define STORAGE_PARTITION   "storage"
#define STORAGE_MOUNT_POINT "/spiffs"

esp_err_t storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = STORAGE_MOUNT_POINT,
        .partition_label        = STORAGE_PARTITION,
        .max_files              = 5,
        .format_if_mount_failed = true,      /* 首次为空时会自动格式化 */
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "未找到分区 '%s':检查 partitions.csv 是否已烧录(需 erase + upload)",
                     STORAGE_PARTITION);
        } else if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "挂载失败且格式化失败(分区可能损坏)");
        } else {
            ESP_LOGE(TAG, "挂载失败: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ESP_ERROR_CHECK(esp_spiffs_info(STORAGE_PARTITION, &total, &used));
    ESP_LOGI(TAG, "挂载成功: %u / %u KB 已用", (unsigned)(used / 1024), (unsigned)(total / 1024));

    storage_list_files(STORAGE_MOUNT_POINT);   /* 启动时把文件列出来,方便确认 */
    return ESP_OK;
}

bool storage_is_mounted(void)
{
    return esp_spiffs_mounted(STORAGE_PARTITION);
}

int storage_list_files(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "无法打开目录 %s", path);
        return -1;
    }

    struct dirent *entry;
    struct stat st;
    char full[300];
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (stat(full, &st) == 0) {
            ESP_LOGI(TAG, "  %-20s %8ld B", entry->d_name, (long)st.st_size);
            count++;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "共 %d 个文件", count);
    return count;
}