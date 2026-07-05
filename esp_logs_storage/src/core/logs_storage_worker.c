/**
 * @file logs_storage_worker.c
 * @brief 日志写入 worker 任务与队列管理
 *
 * 纯队列驱动架构：
 * - 前端线程只负责入队（无锁快速路径）
 * - 后端 worker 任务串行处理所有操作
 * - 所有文件 I/O 由 worker 独占执行，零竞争
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */

#include "logs_storage_internal.h"
#include "logs_storage_rotation.h"
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "LOG_MGR";

logs_storage_context_t g_logs_storage = {0};
static logs_storage_level_t s_current_level = CONFIG_LOGS_STORAGE_LOG_LEVEL;
#ifdef CONFIG_LOGS_STORAGE_CONSOLE_OUTPUT
static volatile bool s_console_output_enabled = CONFIG_LOGS_STORAGE_CONSOLE_OUTPUT;
#else
static volatile bool s_console_output_enabled = false;
#endif

/* 日志级别过滤：level >= 当前级别才记录。
 * 使用原子操作避免 set/get 时的开销。 */
static bool logs_storage_should_emit(logs_storage_level_t level) {
    logs_storage_level_t current = __atomic_load_n(&s_current_level, __ATOMIC_SEQ_CST);
    return level >= current;
}

/* 控制台同步输出日志 */
static void console_output_log(logs_storage_level_t level, const char *message) {
    if (!__atomic_load_n(&s_console_output_enabled, __ATOMIC_SEQ_CST)) {
        return;
    }

    switch (level) {
        case LOGS_STORAGE_LEVEL_DEBUG:
            ESP_LOGD(TAG, "[SYNC] %s", message);
            break;
        case LOGS_STORAGE_LEVEL_INFO:
            ESP_LOGI(TAG, "[SYNC] %s", message);
            break;
        case LOGS_STORAGE_LEVEL_WARN:
            ESP_LOGW(TAG, "[SYNC] %s", message);
            break;
        case LOGS_STORAGE_LEVEL_ERROR:
            ESP_LOGE(TAG, "[SYNC] %s", message);
            break;
        default:
            break;
    }
}

/* 创建队列项：必须先清零整个结构，否则残留数据会导致未定义行为 */
static void queue_item_init(logs_storage_queue_item_t *item, logs_storage_op_type_t op_type) {
    memset(item, 0, sizeof(*item));
    item->op_type = op_type;
}

/* Worker 主任务：串行处理队列中的所有操作 */
static void logs_storage_worker_task(void *arg) {
    (void)arg;

    logs_storage_queue_item_t batch[LOGS_STORAGE_BATCH_MAX_ITEMS];
    size_t batch_count = 0;
    TickType_t last_flush = xTaskGetTickCount();

    while (true) {
        logs_storage_queue_item_t item;
        BaseType_t received = xQueueReceive(g_logs_storage.log_queue, &item,
                                            pdMS_TO_TICKS(LOGS_STORAGE_BATCH_FLUSH_INTERVAL_MS));

        /* 检查是否为特殊操作请求 */
        if (received == pdTRUE) {
            /* 获取操作类型 */
            logs_storage_op_type_t op_type = item.op_type;

            if (g_logs_storage.shutdown_requested) {
                /* flush 残留数据后退出 */
                goto flush_batch;
            }

            /* 根据操作类型分发处理 */
            switch (op_type) {
                case OP_GET_STATS:
                    /* 空间统计查询（单条处理） */
                    if (item.data.get_stats.result) {
                        size_t total = 0, free_bytes = 0;
                        if (get_storage_free_bytes(STORAGE_BASE_PATH, &total, &free_bytes)) {
                            item.data.get_stats.result->total_bytes = total;
                            item.data.get_stats.result->used_bytes = total - free_bytes;
                            item.data.get_stats.result->free_bytes = free_bytes;
                        } else {
                            item.data.get_stats.result->total_bytes = 0;
                            item.data.get_stats.result->used_bytes = 0;
                            item.data.get_stats.result->free_bytes = 0;
                        }
                    }
                    last_flush = xTaskGetTickCount();
                    continue;

                case OP_LIST_FILES:
                    /* 列出文件（单条处理） */
                    if (item.data.list_files.out_paths && item.data.list_files.out_count) {
                        get_sorted_log_files(item.data.list_files.out_paths, item.data.list_files.out_count);
                    }
                    last_flush = xTaskGetTickCount();
                    continue;

                case OP_CLEAR_ALL:
                    /* 清除所有日志 */
                    if (g_logs_storage.current_log_file) {
                        fclose(g_logs_storage.current_log_file);
                        g_logs_storage.current_log_file = NULL;
                    }
                    ensure_free_space(0); /* 触发清理 */
                    logs_storage_rotate_file();
                    last_flush = xTaskGetTickCount();
                    continue;

                case OP_ROTATE:
                    /* 手动触发轮转 */
                    if (g_logs_storage.current_log_file) {
                        fclose(g_logs_storage.current_log_file);
                        g_logs_storage.current_log_file = NULL;
                    }
                    logs_storage_rotate_file();
                    last_flush = xTaskGetTickCount();
                    continue;

                case OP_WRITE_LOG:
                    /* 普通日志写入，加入批量队列 */
                    if (logs_storage_should_emit(item.data.write_log.level)) {
                        strncpy(batch[batch_count].data.write_log.message,
                                item.data.write_log.message, sizeof(batch[batch_count].data.write_log.message) - 1);
                        batch[batch_count].data.write_log.message[sizeof(batch[batch_count].data.write_log.message) - 1] = '\0';
                        batch[batch_count].data.write_log.level = item.data.write_log.level;
                        batch[batch_count].op_type = OP_WRITE_LOG;
                        batch_count++;
                    }
                    break;

                default:
                    break;
            }
        }

        /* 检查批量满或超时 */
        if (batch_count > 0) {
            if (batch_count >= LOGS_STORAGE_BATCH_MAX_ITEMS ||
                (xTaskGetTickCount() - last_flush) >= pdMS_TO_TICKS(LOGS_STORAGE_BATCH_FLUSH_INTERVAL_MS)) {
                goto flush_batch;
            }
        }

        /* 检查退出信号 */
        if (g_logs_storage.shutdown_requested && batch_count == 0) {
            break;
        }

        continue;

flush_batch:
        /* 批量 flush */
        if (batch_count > 0 && g_logs_storage.current_log_file == NULL) {
            if (!ensure_free_space(0) || !logs_storage_rotate_file()) {
                ESP_LOGW(TAG, "Failed to persist queued log entry: rotation error");
                batch_count = 0;
                last_flush = xTaskGetTickCount();
                continue;
            }
        }

        int64_t ms = esp_timer_get_time() / 1000;
        for (size_t i = 0; i < batch_count; ++i) {
            /* 检查文件大小并轮转 */
            if (g_logs_storage.current_log_file) {
                long cur_pos = ftell(g_logs_storage.current_log_file);
                logs_storage_rotation_config_t cfg;
                logs_storage_rotation_config_get(&cfg);
                if (cur_pos > (long)cfg.max_file_size_bytes) {
                    ESP_LOGI(TAG, "Log file size %ld bytes, rotating", cur_pos);
                    fclose(g_logs_storage.current_log_file);
                    g_logs_storage.current_log_file = NULL;
                }
            }

            /* 确保有可用的文件句柄 */
            if (!g_logs_storage.current_log_file) {
                if (!ensure_free_space(0) || !logs_storage_rotate_file()) {
                    ESP_LOGW(TAG, "Failed to persist queued log entry: rotation error");
                    continue;
                }
            }

            fprintf(g_logs_storage.current_log_file, "[+%lld ms] %s\n", (long long)ms,
                    batch[i].data.write_log.message);
            
            /* 控制台同步输出 */
            console_output_log(batch[i].data.write_log.level, batch[i].data.write_log.message);
        }
        fflush(g_logs_storage.current_log_file);
        
        /* 更新当前日志文件大小缓存（避免 Web API 使用 ftell） */
        if (g_logs_storage.current_log_file) {
            g_logs_storage.current_log_size = ftell(g_logs_storage.current_log_file);
        }
        
        ESP_LOGD(TAG, "[+%lld ms] flushed %d entries (size: %ld bytes)", (long long)ms, (int)batch_count, g_logs_storage.current_log_size);
        batch_count = 0;
        last_flush = xTaskGetTickCount();
    }

    /* 退出前 flush 残留数据 */
    if (batch_count > 0) {
        int64_t ms = esp_timer_get_time() / 1000;
        for (size_t i = 0; i < batch_count; ++i) {
            if (g_logs_storage.current_log_file) {
                fprintf(g_logs_storage.current_log_file, "[+%lld ms] %s\n", (long long)ms,
                        batch[i].data.write_log.message);
            }
        }
        fflush(g_logs_storage.current_log_file);
    }

    if (g_logs_storage.log_task_done != NULL) {
        xSemaphoreGive(g_logs_storage.log_task_done);
    }
    vTaskDelete(NULL);
}

/* 启动 worker：创建队列、信号量和任务 */
bool logs_storage_worker_start(void) {
    if (g_logs_storage.log_queue != NULL) {
        return true;
    }

    g_logs_storage.log_queue = xQueueCreate(LOGS_STORAGE_QUEUE_LENGTH, sizeof(logs_storage_queue_item_t));
    if (g_logs_storage.log_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create log queue");
        return false;
    }

    g_logs_storage.log_task_done = xSemaphoreCreateBinary();
    if (g_logs_storage.log_task_done == NULL) {
        vQueueDelete(g_logs_storage.log_queue);
        g_logs_storage.log_queue = NULL;
        ESP_LOGE(TAG, "Failed to create log task done semaphore");
        return false;
    }

    g_logs_storage.shutdown_requested = false;

    BaseType_t ret = xTaskCreate(logs_storage_worker_task, "log_worker",
                                 LOGS_STORAGE_TASK_STACK_WORDS, NULL,
                                 LOGS_STORAGE_TASK_PRIORITY, &g_logs_storage.log_task);
    if (ret != pdPASS) {
        vSemaphoreDelete(g_logs_storage.log_task_done);
        vQueueDelete(g_logs_storage.log_queue);
        g_logs_storage.log_queue = NULL;
        g_logs_storage.log_task_done = NULL;
        ESP_LOGE(TAG, "Failed to create log worker task");
        return false;
    }

    ESP_LOGI(TAG, "Log worker started");
    return true;
}

/* 通知 worker 停止：置位 shutdown_requested 并发送空消息唤醒阻塞的队列接收，
 * 然后等待 worker 释放 log_task_done。 */
void logs_storage_worker_stop(void) {
    g_logs_storage.shutdown_requested = true;
    if (g_logs_storage.log_queue != NULL) {
        logs_storage_queue_item_t shutdown_item = {0};
        shutdown_item.op_type = OP_WRITE_LOG;
        xQueueSend(g_logs_storage.log_queue, &shutdown_item, portMAX_DELAY);
    }

    if (g_logs_storage.log_task != NULL && g_logs_storage.log_task_done != NULL) {
        xSemaphoreTake(g_logs_storage.log_task_done, portMAX_DELAY);
    }

    g_logs_storage.log_task = NULL;
}

/* 入队一条已格式化的日志消息。队列满时丢弃并返回 false */
bool logs_storage_worker_enqueue_message(logs_storage_level_t level, const char *message) {
    if (g_logs_storage.log_queue == NULL || g_logs_storage.shutdown_requested) {
        return false;
    }

    if (!logs_storage_should_emit(level)) {
        return true;
    }

    logs_storage_queue_item_t item;
    queue_item_init(&item, OP_WRITE_LOG);
    item.data.write_log.level = level;
    strncpy(item.data.write_log.message, message, sizeof(item.data.write_log.message) - 1);
    item.data.write_log.message[sizeof(item.data.write_log.message) - 1] = '\0';

    if (xQueueSend(g_logs_storage.log_queue, &item, pdMS_TO_TICKS(LOGS_STORAGE_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Log queue full, dropping log entry");
        return false;
    }

    return true;
}

/* 入队一条 printf 风格的格式化日志 */
bool logs_storage_worker_enqueue_formatted(logs_storage_level_t level, const char *format, ...) {
    if (g_logs_storage.log_queue == NULL || g_logs_storage.shutdown_requested) {
        return false;
    }

    if (!logs_storage_should_emit(level)) {
        return true;
    }

    va_list args;
    va_start(args, format);
    char message[LOGS_STORAGE_MAX_MESSAGE_LEN];
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    logs_storage_queue_item_t item;
    queue_item_init(&item, OP_WRITE_LOG);
    item.data.write_log.level = level;
    strncpy(item.data.write_log.message, message, sizeof(item.data.write_log.message) - 1);
    item.data.write_log.message[sizeof(item.data.write_log.message) - 1] = '\0';

    if (xQueueSend(g_logs_storage.log_queue, &item, pdMS_TO_TICKS(LOGS_STORAGE_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Log queue full, dropping log entry");
        return false;
    }

    return true;
}

/* 请求清除所有日志 */
void logs_storage_worker_request_clear_all(void) {
    if (g_logs_storage.log_queue == NULL || g_logs_storage.shutdown_requested) {
        return;
    }

    logs_storage_queue_item_t item;
    queue_item_init(&item, OP_CLEAR_ALL);
    xQueueSend(g_logs_storage.log_queue, &item, pdMS_TO_TICKS(LOGS_STORAGE_QUEUE_TIMEOUT_MS));
}

/* 请求触发轮转 */
void logs_storage_worker_request_rotate(void) {
    if (g_logs_storage.log_queue == NULL || g_logs_storage.shutdown_requested) {
        return;
    }

    logs_storage_queue_item_t item;
    queue_item_init(&item, OP_ROTATE);
    xQueueSend(g_logs_storage.log_queue, &item, pdMS_TO_TICKS(LOGS_STORAGE_QUEUE_TIMEOUT_MS));
}

/* 原子更新当前日志级别 */
void logs_storage_worker_set_level(logs_storage_level_t level) {
    __atomic_store_n(&s_current_level, level, __ATOMIC_SEQ_CST);
}

/* 原子读取当前日志级别 */
logs_storage_level_t logs_storage_worker_get_level(void) {
    return __atomic_load_n(&s_current_level, __ATOMIC_SEQ_CST);
}

/* 设置控制台同步输出开关 */
void logs_storage_worker_set_console_output(bool enable) {
    __atomic_store_n(&s_console_output_enabled, enable, __ATOMIC_SEQ_CST);
    if (enable) {
        ESP_LOGI(TAG, "Console output enabled");
    } else {
        ESP_LOGI(TAG, "Console output disabled");
    }
}

/* 获取控制台同步输出状态 */
bool logs_storage_worker_get_console_output(void) {
    return __atomic_load_n(&s_console_output_enabled, __ATOMIC_SEQ_CST);
}
