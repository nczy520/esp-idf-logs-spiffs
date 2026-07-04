/**
 * @file logs_storage.c
 * @brief 日志存储公共 API 实现
 *
 * 提供日志初始化、写入、级别控制、USB 模式切换等公共接口。
 * 实际的磁盘 I/O 由 logs_storage_backend.c 完成，
 * 异步写入由 logs_storage_worker.c 中的 Worker 任务完成。
 */

#include "logs_storage.h"
#include "logs_storage_internal.h"
#include <stdarg.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"

static const char *TAG = "LOG_MGR";

extern bool logs_storage_backend_init(void);
extern void logs_storage_backend_deinit(void);
extern esp_err_t logs_storage_backend_format(void);

esp_err_t logs_storage_format(void) {
    ESP_LOGI(TAG, "Formatting storage partition...");
    esp_err_t ret = logs_storage_backend_format();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Format successful");
    } else {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * 初始化日志系统：
 * 1. 初始化内存缓冲区（USB 模式使用）
 * 2. 启动 Worker 任务（消费队列，批量写入）
 * 3. 初始化后端（挂载 FAT，创建初始日志文件）
 * 4. 列出现有日志文件，写入启动标记
 */
bool logs_storage_init(void) {
    logs_storage_buffer_init();

    if (!logs_storage_worker_start()) {
        return false;
    }

    if (!logs_storage_backend_init()) {
        logs_storage_worker_stop();
        return false;
    }

    logs_storage_list_existing();
    logs_storage_worker_enqueue_formatted(LOGS_STORAGE_LEVEL_INFO, "[SYSTEM] System started");
    return true;
}

void logs_storage_set_level(logs_storage_level_t level) {
    logs_storage_worker_set_level(level);
}

bool logs_storage_is_usb_mode_active(void) {
    return g_logs_storage.usb_mode_active;
}

/**
 * 强制退出 USB 模式（用于设备重启前调用）。
 *
 * 设置退出标志并等待 USB 弹出监控任务结束，确保监控任务不会在
 * esp_restart() 过程中因 tud_mounted() 返回 false 而误触发
 * exit_usb_mode + 再次 esp_restart 的死循环。
 */
void logs_storage_force_exit_usb_mode(void) {
    ESP_LOGI(TAG, "Force exit USB mode");
    g_logs_storage.usb_monitor_exit = true;
    g_logs_storage.usb_mode_active = false;

    /* 等待监控任务退出，最多等待 3 秒 */
    if (g_logs_storage.usb_monitor_task != NULL) {
        for (int i = 0; i < 60; i++) {
            if (g_logs_storage.usb_monitor_task == NULL) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/**
 * 写入一条 INFO 级别日志（printf 风格格式化）。
 * 日志先入队，由 Worker 任务异步写入磁盘或内存缓冲。
 */
void logs_storage_write(const char *format, ...) {
    if (!format) {
        return;
    }

    va_list args;
    va_start(args, format);
    char message[LOGS_STORAGE_MAX_MESSAGE_LEN];
    int written = vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (written < 0 || written >= (int)sizeof(message)) {
        message[sizeof(message) - 1] = '\0';
    }

    logs_storage_worker_enqueue_message(LOGS_STORAGE_LEVEL_INFO, message);
}

/**
 * 写入一条指定级别的日志（printf 风格格式化）。
 * 低于当前设置级别的日志会被丢弃。
 */
void logs_storage_write_level(logs_storage_level_t level, const char *format, ...) {
    if (!format) {
        return;
    }

    va_list args;
    va_start(args, format);
    char message[LOGS_STORAGE_MAX_MESSAGE_LEN];
    int written = vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (written < 0 || written >= (int)sizeof(message)) {
        message[sizeof(message) - 1] = '\0';
    }

    logs_storage_worker_enqueue_message(level, message);
}

/**
 * 反初始化日志系统：
 * 1. 停止 Worker 任务（等待剩余队列项处理完）
 * 2. 关闭后端（关闭当前日志文件）
 * 3. 若有内存缓冲日志（USB 模式残留），重新挂载 FAT 并 flush
 * 4. 释放缓冲区、队列、信号量
 */
void logs_storage_deinit(void) {
    logs_storage_worker_stop();
    logs_storage_backend_deinit();

    /* 若有缓冲日志（USB 模式），flush 到磁盘 */
    if (g_logs_storage.buffer_count > 0) {
        if (xSemaphoreTake(g_logs_storage.log_mutex, portMAX_DELAY) == pdTRUE) {
            if (!g_logs_storage.current_log_file) {
                extern bool logs_storage_backend_init(void);
                logs_storage_backend_init();
                logs_storage_buffer_flush_to_disk();
                logs_storage_backend_deinit();
            }
            xSemaphoreGive(g_logs_storage.log_mutex);
        }
    }

    logs_storage_buffer_deinit();

    if (g_logs_storage.log_queue != NULL) {
        vQueueDelete(g_logs_storage.log_queue);
        g_logs_storage.log_queue = NULL;
    }

    if (g_logs_storage.log_task_done != NULL) {
        vSemaphoreDelete(g_logs_storage.log_task_done);
        g_logs_storage.log_task_done = NULL;
    }

    if (g_logs_storage.log_mutex != NULL) {
        vSemaphoreDelete(g_logs_storage.log_mutex);
        g_logs_storage.log_mutex = NULL;
    }
}
