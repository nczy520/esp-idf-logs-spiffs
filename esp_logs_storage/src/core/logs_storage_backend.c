/**
 * @file logs_storage_backend.c
 * @brief 存储后端核心：SPIFFS 挂载、日志轮转、写入入口
 *
 * 纯队列驱动，worker 任务独占所有文件操作，无需任何互斥锁。
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */

#include "logs_storage_internal.h"
#include "logs_storage_rotation.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_partition.h"

static const char *TAG = "LOG_MGR";

static esp_vfs_spiffs_conf_t s_spiffs_conf = {
    .base_path = STORAGE_BASE_PATH,
    .partition_label = "storage",
    .max_files = 5,
    .format_if_mount_failed = true,
};

/* 日志文件轮转：关闭旧文件，清理空间，创建新文件 */
bool logs_storage_rotate_file(void) {
    logs_storage_rotation_config_t cfg;
    logs_storage_rotation_config_get(&cfg);

    if (g_logs_storage.current_log_file) {
        fclose(g_logs_storage.current_log_file);
        g_logs_storage.current_log_file = NULL;
    }

    if (!ensure_free_space(cfg.rotate_threshold_bytes)) {
        ESP_LOGE(TAG, "Cannot create new log file, insufficient space");
        return false;
    }

    enforce_max_file_count();

    int next_num = get_next_log_number();
    generate_log_path(g_logs_storage.current_log_path, next_num);
    g_logs_storage.current_log_file = fopen(g_logs_storage.current_log_path, "a");
    if (!g_logs_storage.current_log_file) {
        ESP_LOGE(TAG, "Failed to create log file: %s", g_logs_storage.current_log_path);
        return false;
    }

    setvbuf(g_logs_storage.current_log_file, NULL, _IOLBF, 256);
    g_logs_storage.current_log_size = 0;  /* 新文件初始大小设为 0 */
    ESP_LOGI(TAG, "New log file: %s (number %d, size: 0 bytes)", g_logs_storage.current_log_path, next_num);
    return true;
}

/* 初始化存储后端：挂载 SPIFFS，创建首个日志文件 */
bool logs_storage_backend_init(void) {
    esp_err_t err = esp_vfs_spiffs_register(&s_spiffs_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    err = esp_spiffs_info(s_spiffs_conf.partition_label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted, total: %d KB, used: %d KB, free: %d KB",
                 (int)(total / 1024), (int)(used / 1024), (int)((total - used) / 1024));
    }

    if (!logs_storage_rotate_file()) {
        ESP_LOGE(TAG, "Failed to create initial log file");
        return false;
    }

    return true;
}

/* 反初始化：关闭文件，卸载 SPIFFS */
void logs_storage_backend_deinit(void) {
    if (g_logs_storage.current_log_file) {
        fflush(g_logs_storage.current_log_file);
        fclose(g_logs_storage.current_log_file);
        g_logs_storage.current_log_file = NULL;
    }

    esp_vfs_spiffs_unregister(s_spiffs_conf.partition_label);
    ESP_LOGI(TAG, "SPIFFS unmounted");
}

/* 启动时枚举并打印所有现存日志文件 */
void logs_storage_list_existing(void) {
    char **files = NULL;
    int file_count = 0;
    if (get_sorted_log_files(&files, &file_count) != ESP_OK || file_count == 0) {
        ESP_LOGI(TAG, "No log files found");
        if (files) free(files);
        return;
    }

    size_t total_size = 0;

    ESP_LOGI(TAG, "========== Existing log files (sorted) ==========");
    for (int i = 0; i < file_count; i++) {
        const char *path = files[i];
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        struct stat st;
        if (stat(path, &st) == 0) {
            total_size += (size_t)st.st_size;
            ESP_LOGI(TAG, "  %s  %ld B", name, (long)st.st_size);
        } else {
            ESP_LOGI(TAG, "  %s  (size unknown)", name);
        }
        free(files[i]);
    }
    free(files);

    size_t spiffs_total = 0, spiffs_used = 0;
    if (esp_spiffs_info(s_spiffs_conf.partition_label, &spiffs_total, &spiffs_used) == ESP_OK) {
        ESP_LOGI(TAG, "---- summary ----");
        ESP_LOGI(TAG, "  files: %d", file_count);
        ESP_LOGI(TAG, "  log files total:  %6zu B (%4zu KB)", total_size, total_size / 1024);
        ESP_LOGI(TAG, "  SPIFFS total:     %6zu B (%4zu KB)", spiffs_total, spiffs_total / 1024);
        ESP_LOGI(TAG, "  SPIFFS used:      %6zu B (%4zu KB)", spiffs_used, spiffs_used / 1024);
        ESP_LOGI(TAG, "  SPIFFS free:      %6zu B (%4zu KB)",
                 spiffs_total - spiffs_used, (spiffs_total - spiffs_used) / 1024);
    } else {
        ESP_LOGI(TAG, "  total size: %zu bytes", total_size);
    }
    ESP_LOGI(TAG, "================================================");
}

/* 格式化存储分区：格式化 SPIFFS */
esp_err_t logs_storage_backend_format(void) {
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_ANY,
                                                           "storage");
    if (!part) {
        ESP_LOGE(TAG, "storage partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase storage partition: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Storage partition erased, will reformat on next mount");
    return ESP_OK;
}
