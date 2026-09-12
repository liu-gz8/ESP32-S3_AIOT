#pragma once
#include <stdbool.h>
#include <esp_err.h>

esp_err_t storage_init(void);            /* 挂载 SPIFFS(分区标签 storage) */
bool      storage_is_mounted(void);
int       storage_list_files(const char *path);   /* 调试:列出文件 */