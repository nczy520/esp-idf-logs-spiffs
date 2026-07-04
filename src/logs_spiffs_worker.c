#include "logs_spiffs_internal.h"
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "LOG_MGR";

logs_spiffs_context_t g_logs_spiffs = {0};
static logs_spiffs_level_t s_current_level = LOGS_SPIFFS_LEVEL_INFO;

static bool logs_spiffs_should_emit(logs_spiffs_level_t level) {
    /* Atomic read so set_level() can be called from any thread without a lock. */
    logs_spiffs_level_t current = __atomic_load_n(&s_current_level, __ATOMIC_SEQ_CST);
    return level >= current;
}

static void logs_spiffs_worker_task(void *arg) {
    (void)arg;

    logs_spiffs_queue_item_t batch[LOGS_SPIFFS_BATCH_MAX_ITEMS];
    size_t batch_count = 0;
    TickType_t last_flush = xTaskGetTickCount();

    while (true) {
        logs_spiffs_queue_item_t item;
        if (xQueueReceive(g_logs_spiffs.log_queue, &item, pdMS_TO_TICKS(LOGS_SPIFFS_BATCH_FLUSH_INTERVAL_MS)) != pdTRUE) {
            if (batch_count > 0) {
                goto flush_batch;
            }
            if (g_logs_spiffs.shutdown_requested) {
                break;
            }
            continue;
        }

        if (g_logs_spiffs.shutdown_requested) {
            break;
        }

        if (!logs_spiffs_should_emit(item.level)) {
            continue;
        }

        batch[batch_count++] = item;
        if (batch_count >= LOGS_SPIFFS_BATCH_MAX_ITEMS) {
            goto flush_batch;
        }

        if ((xTaskGetTickCount() - last_flush) >= pdMS_TO_TICKS(LOGS_SPIFFS_BATCH_FLUSH_INTERVAL_MS)) {
            goto flush_batch;
        }

        continue;

flush_batch:
        if (batch_count == 0) {
            last_flush = xTaskGetTickCount();
            continue;
        }

        int64_t ms = esp_timer_get_time() / 1000;
        for (size_t i = 0; i < batch_count; ++i) {
            if (!logs_spiffs_storage_write_line(batch[i].message, ms)) {
                ESP_LOGW(TAG, "Failed to persist queued log entry");
            }
            /* Console echo is DEBUG-only to avoid flooding serial output in
             * production. Enable with: esp_log_level_set("LOG_MGR", ESP_LOG_DEBUG); */
            ESP_LOGD(TAG, "[+%lld ms] %s", (long long)ms, batch[i].message);
        }
        batch_count = 0;
        last_flush = xTaskGetTickCount();
    }

    if (batch_count > 0) {
        int64_t ms = esp_timer_get_time() / 1000;
        for (size_t i = 0; i < batch_count; ++i) {
            if (!logs_spiffs_storage_write_line(batch[i].message, ms)) {
                ESP_LOGW(TAG, "Failed to persist queued log entry");
            }
            ESP_LOGD(TAG, "[+%lld ms] %s", (long long)ms, batch[i].message);
        }
    }

    if (g_logs_spiffs.log_task_done != NULL) {
        xSemaphoreGive(g_logs_spiffs.log_task_done);
    }
    vTaskDelete(NULL);
}

bool logs_spiffs_worker_start(void) {
    g_logs_spiffs.log_mutex = xSemaphoreCreateMutex();
    if (g_logs_spiffs.log_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }

    g_logs_spiffs.log_queue = xQueueCreate(LOGS_SPIFFS_QUEUE_LENGTH, sizeof(logs_spiffs_queue_item_t));
    if (g_logs_spiffs.log_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create log queue");
        vSemaphoreDelete(g_logs_spiffs.log_mutex);
        g_logs_spiffs.log_mutex = NULL;
        return false;
    }

    g_logs_spiffs.log_task_done = xSemaphoreCreateBinary();
    if (g_logs_spiffs.log_task_done == NULL) {
        ESP_LOGE(TAG, "Failed to create shutdown semaphore");
        vQueueDelete(g_logs_spiffs.log_queue);
        vSemaphoreDelete(g_logs_spiffs.log_mutex);
        g_logs_spiffs.log_queue = NULL;
        g_logs_spiffs.log_mutex = NULL;
        return false;
    }

    g_logs_spiffs.shutdown_requested = false;
    BaseType_t task_created = xTaskCreate(logs_spiffs_worker_task, "logs_spiffs_worker",
                                          LOGS_SPIFFS_TASK_STACK_WORDS, NULL,
                                          LOGS_SPIFFS_TASK_PRIORITY, &g_logs_spiffs.log_task);
    if (task_created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create worker task");
        vSemaphoreDelete(g_logs_spiffs.log_task_done);
        g_logs_spiffs.log_task_done = NULL;
        vQueueDelete(g_logs_spiffs.log_queue);
        g_logs_spiffs.log_queue = NULL;
        vSemaphoreDelete(g_logs_spiffs.log_mutex);
        g_logs_spiffs.log_mutex = NULL;
        return false;
    }

    return true;
}

void logs_spiffs_worker_stop(void) {
    g_logs_spiffs.shutdown_requested = true;
    if (g_logs_spiffs.log_queue != NULL) {
        logs_spiffs_queue_item_t shutdown_item = {0};
        xQueueSend(g_logs_spiffs.log_queue, &shutdown_item, portMAX_DELAY);
    }

    if (g_logs_spiffs.log_task != NULL && g_logs_spiffs.log_task_done != NULL) {
        xSemaphoreTake(g_logs_spiffs.log_task_done, portMAX_DELAY);
    }

    if (g_logs_spiffs.log_queue != NULL) {
        vQueueDelete(g_logs_spiffs.log_queue);
        g_logs_spiffs.log_queue = NULL;
    }

    if (g_logs_spiffs.log_task_done != NULL) {
        vSemaphoreDelete(g_logs_spiffs.log_task_done);
        g_logs_spiffs.log_task_done = NULL;
    }

    if (g_logs_spiffs.log_mutex != NULL) {
        vSemaphoreDelete(g_logs_spiffs.log_mutex);
        g_logs_spiffs.log_mutex = NULL;
    }

    g_logs_spiffs.log_task = NULL;
}

bool logs_spiffs_worker_enqueue_message(logs_spiffs_level_t level, const char *message) {
    if (g_logs_spiffs.log_queue == NULL || g_logs_spiffs.shutdown_requested) {
        return false;
    }

    if (!logs_spiffs_should_emit(level)) {
        return true;
    }

    logs_spiffs_queue_item_t item;
    item.level = level;
    strncpy(item.message, message, sizeof(item.message) - 1);
    item.message[sizeof(item.message) - 1] = '\0';

    if (xQueueSend(g_logs_spiffs.log_queue, &item, pdMS_TO_TICKS(LOGS_SPIFFS_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Log queue full, dropping log entry");
        return false;
    }

    return true;
}

bool logs_spiffs_worker_enqueue_formatted(logs_spiffs_level_t level, const char *format, ...) {
    if (g_logs_spiffs.log_queue == NULL || g_logs_spiffs.shutdown_requested) {
        return false;
    }

    if (!logs_spiffs_should_emit(level)) {
        return true;
    }

    logs_spiffs_queue_item_t item;
    item.level = level;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(item.message, sizeof(item.message), format, args);
    va_end(args);

    if (written < 0 || written >= (int)sizeof(item.message)) {
        item.message[sizeof(item.message) - 1] = '\0';
    }

    if (xQueueSend(g_logs_spiffs.log_queue, &item, pdMS_TO_TICKS(LOGS_SPIFFS_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Log queue full, dropping log entry");
        return false;
    }

    return true;
}

void logs_spiffs_worker_set_level(logs_spiffs_level_t level) {
    __atomic_store_n(&s_current_level, level, __ATOMIC_SEQ_CST);
}

logs_spiffs_level_t logs_spiffs_worker_get_level(void) {
    return __atomic_load_n(&s_current_level, __ATOMIC_SEQ_CST);
}
