/**
 * @file logs_storage_worker.c
 * @brief 日志写入 worker 任务与队列管理
 *
 * 职责：
 * - 维护 FreeRTOS 队列接收外部线程的日志条目
 * - 后台批量消费：满 8 条或 200ms 超时则 flush
 * - 根据 usb_mode_active 选择写入磁盘或内存缓冲
 * - 提供日志级别过滤（原子读，无锁）
 */

#include "logs_storage_internal.h"
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "LOG_MGR";

logs_storage_context_t g_logs_storage = {0};
static logs_storage_level_t s_current_level = LOGS_STORAGE_LEVEL_INFO;

/* 日志级别过滤：level >= 当前级别才记录。
 * 使用 __atomic 原子操作避免 set/get 时的互斥锁开销。 */
static bool logs_storage_should_emit(logs_storage_level_t level) {
    logs_storage_level_t current = __atomic_load_n(&s_current_level, __ATOMIC_SEQ_CST);
    return level >= current;
}

/* Worker 主任务：批量消费队列，按 usb_mode_active 切换写入目标。
 * 退出前必须 flush 残留批次，并释放 log_task_done 信号量通知 worker_stop。 */
static void logs_storage_worker_task(void *arg) {
    (void)arg;

    logs_storage_queue_item_t batch[LOGS_STORAGE_BATCH_MAX_ITEMS];
    size_t batch_count = 0;
    TickType_t last_flush = xTaskGetTickCount();

    while (true) {
        logs_storage_queue_item_t item;
        if (xQueueReceive(g_logs_storage.log_queue, &item, pdMS_TO_TICKS(LOGS_STORAGE_BATCH_FLUSH_INTERVAL_MS)) != pdTRUE) {
            if (batch_count > 0) {
                goto flush_batch;
            }
            if (g_logs_storage.shutdown_requested) {
                break;
            }
            continue;
        }

        if (g_logs_storage.shutdown_requested) {
            if (batch_count > 0) {
                goto flush_batch;
            }
            break;
        }

        if (!logs_storage_should_emit(item.level)) {
            continue;
        }

        batch[batch_count++] = item;
        if (batch_count >= LOGS_STORAGE_BATCH_MAX_ITEMS) {
            goto flush_batch;
        }

        if ((xTaskGetTickCount() - last_flush) >= pdMS_TO_TICKS(LOGS_STORAGE_BATCH_FLUSH_INTERVAL_MS)) {
            goto flush_batch;
        }

        continue;

flush_batch:
        if (batch_count == 0) {
            last_flush = xTaskGetTickCount();
            continue;
        }

        int64_t ms = esp_timer_get_time() / 1000;
        if (g_logs_storage.usb_mode_active) {
            /* USB 模式：写入内存缓冲区，不碰磁盘 */
            for (size_t i = 0; i < batch_count; ++i) {
                logs_storage_buffer_push(batch[i].message, ms);
            }
        } else {
            for (size_t i = 0; i < batch_count; ++i) {
                if (!logs_storage_write_line(batch[i].message, ms)) {
                    ESP_LOGW(TAG, "Failed to persist queued log entry");
                }
            }
            ESP_LOGD(TAG, "[+%lld ms] flushed %d entries", (long long)ms, (int)batch_count);
        }
        batch_count = 0;
        last_flush = xTaskGetTickCount();
    }

    if (batch_count > 0) {
        int64_t ms = esp_timer_get_time() / 1000;
        if (g_logs_storage.usb_mode_active) {
            for (size_t i = 0; i < batch_count; ++i) {
                logs_storage_buffer_push(batch[i].message, ms);
            }
        } else {
            for (size_t i = 0; i < batch_count; ++i) {
                if (!logs_storage_write_line(batch[i].message, ms)) {
                    ESP_LOGW(TAG, "Failed to persist queued log entry");
                }
            }
        }
    }

    if (g_logs_storage.log_task_done != NULL) {
        xSemaphoreGive(g_logs_storage.log_task_done);
    }
    vTaskDelete(NULL);
}

/* 创建 mutex、队列、shutdown 信号量并启动 worker 任务。
 * 任一资源分配失败都会回滚已分配的资源。 */
bool logs_storage_worker_start(void) {
    g_logs_storage.log_mutex = xSemaphoreCreateMutex();
    if (g_logs_storage.log_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }

    g_logs_storage.log_queue = xQueueCreate(LOGS_STORAGE_QUEUE_LENGTH, sizeof(logs_storage_queue_item_t));
    if (g_logs_storage.log_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create log queue");
        vSemaphoreDelete(g_logs_storage.log_mutex);
        g_logs_storage.log_mutex = NULL;
        return false;
    }

    g_logs_storage.log_task_done = xSemaphoreCreateBinary();
    if (g_logs_storage.log_task_done == NULL) {
        ESP_LOGE(TAG, "Failed to create shutdown semaphore");
        vQueueDelete(g_logs_storage.log_queue);
        vSemaphoreDelete(g_logs_storage.log_mutex);
        g_logs_storage.log_queue = NULL;
        g_logs_storage.log_mutex = NULL;
        return false;
    }

    g_logs_storage.shutdown_requested = false;
    BaseType_t task_created = xTaskCreate(logs_storage_worker_task, "logs_storage_worker",
                                          LOGS_STORAGE_TASK_STACK_WORDS, NULL,
                                          LOGS_STORAGE_TASK_PRIORITY, &g_logs_storage.log_task);
    if (task_created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create worker task");
        vSemaphoreDelete(g_logs_storage.log_task_done);
        g_logs_storage.log_task_done = NULL;
        vQueueDelete(g_logs_storage.log_queue);
        g_logs_storage.log_queue = NULL;
        vSemaphoreDelete(g_logs_storage.log_mutex);
        g_logs_storage.log_mutex = NULL;
        return false;
    }

    return true;
}

/* 通知 worker 停止：置位 shutdown_requested 并发送空消息唤醒阻塞的队列接收，
 * 然后等待 worker 释放 log_task_done。资源销毁交给 deinit 路径。 */
void logs_storage_worker_stop(void) {
    g_logs_storage.shutdown_requested = true;
    if (g_logs_storage.log_queue != NULL) {
        logs_storage_queue_item_t shutdown_item = {0};
        xQueueSend(g_logs_storage.log_queue, &shutdown_item, portMAX_DELAY);
    }

    if (g_logs_storage.log_task != NULL && g_logs_storage.log_task_done != NULL) {
        xSemaphoreTake(g_logs_storage.log_task_done, portMAX_DELAY);
    }

    /* Leave deletion of semaphores/queue/mutex to the deinit path.
     * Here we only wait for the worker task to finish and clear the task handle. */
    g_logs_storage.log_task = NULL;
}

/* 入队一条已格式化的日志消息。队列满时丢弃并返回 false。 */
bool logs_storage_worker_enqueue_message(logs_storage_level_t level, const char *message) {
    if (g_logs_storage.log_queue == NULL || g_logs_storage.shutdown_requested) {
        return false;
    }

    if (!logs_storage_should_emit(level)) {
        return true;
    }

    logs_storage_queue_item_t item;
    item.level = level;
    strncpy(item.message, message, sizeof(item.message) - 1);
    item.message[sizeof(item.message) - 1] = '\0';

    if (xQueueSend(g_logs_storage.log_queue, &item, pdMS_TO_TICKS(LOGS_STORAGE_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Log queue full, dropping log entry");
        return false;
    }

    return true;
}

/* 入队一条 printf 风格的格式化日志。截断到 MAX_MESSAGE_LEN。
 * 队列满时丢弃并返回 false。 */
bool logs_storage_worker_enqueue_formatted(logs_storage_level_t level, const char *format, ...) {
    if (g_logs_storage.log_queue == NULL || g_logs_storage.shutdown_requested) {
        return false;
    }

    if (!logs_storage_should_emit(level)) {
        return true;
    }

    logs_storage_queue_item_t item;
    item.level = level;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(item.message, sizeof(item.message), format, args);
    va_end(args);

    if (written < 0 || written >= (int)sizeof(item.message)) {
        item.message[sizeof(item.message) - 1] = '\0';
    }

    if (xQueueSend(g_logs_storage.log_queue, &item, pdMS_TO_TICKS(LOGS_STORAGE_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Log queue full, dropping log entry");
        return false;
    }

    return true;
}

/* 原子更新当前日志级别。 */
void logs_storage_worker_set_level(logs_storage_level_t level) {
    __atomic_store_n(&s_current_level, level, __ATOMIC_SEQ_CST);
}

/* 原子读取当前日志级别。 */
logs_storage_level_t logs_storage_worker_get_level(void) {
    return __atomic_load_n(&s_current_level, __ATOMIC_SEQ_CST);
}
