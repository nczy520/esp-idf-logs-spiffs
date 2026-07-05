/**
 * @file logs_storage.c
 * @brief esp_logs_storage 公共 API 入口
 *
 * 纯队列驱动，线程安全。
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */

#include "logs_storage.h"
#include "logs_storage_internal.h"
#include "logs_storage_rotation.h"
#include "esp_log.h"

static const char *TAG = "LOG_STORAGE";

bool logs_storage_init(void) {
    if (!logs_storage_backend_init()) {
        ESP_LOGE(TAG, "Backend init failed");
        return false;
    }

    if (!logs_storage_worker_start()) {
        ESP_LOGE(TAG, "Worker start failed");
        logs_storage_backend_deinit();
        return false;
    }

    logs_storage_list_existing();
    ESP_LOGI(TAG, "Log storage initialized");
    return true;
}

void logs_storage_deinit(void) {
    logs_storage_worker_stop();
    logs_storage_backend_deinit();
    ESP_LOGI(TAG, "Log storage deinitialized");
}

bool logs_storage_write(logs_storage_level_t level, const char *message) {
    return logs_storage_worker_enqueue_message(level, message);
}

bool logs_storage_write_level(logs_storage_level_t level, const char *format, ...) {
    if (!format) {
        return false;
    }
    
    va_list args;
    va_start(args, format);
    
    char message[LOGS_STORAGE_MAX_MESSAGE_LEN];
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    return logs_storage_worker_enqueue_message(level, message);
}

void logs_storage_set_level(logs_storage_level_t level) {
    logs_storage_worker_set_level(level);
}

logs_storage_level_t logs_storage_get_level(void) {
    return logs_storage_worker_get_level();
}

void logs_storage_set_console_output(bool enable) {
    logs_storage_worker_set_console_output(enable);
}

bool logs_storage_get_console_output(void) {
    return logs_storage_worker_get_console_output();
}

void logs_storage_list_files(void) {
    logs_storage_list_existing();
}

esp_err_t logs_storage_format(void) {
    return logs_storage_backend_format();
}
