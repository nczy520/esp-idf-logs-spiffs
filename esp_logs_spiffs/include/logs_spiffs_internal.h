#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include "logs_spiffs.h"
#include "esp_err.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOGS_SPIFFS_QUEUE_LENGTH          64
#define LOGS_SPIFFS_MAX_MESSAGE_LEN       384
#define LOGS_SPIFFS_TASK_STACK_WORDS      4096
#define LOGS_SPIFFS_TASK_PRIORITY         5
#define LOGS_SPIFFS_QUEUE_TIMEOUT_MS      100
#define LOGS_SPIFFS_BATCH_FLUSH_INTERVAL_MS 200
#define LOGS_SPIFFS_BATCH_MAX_ITEMS       8

typedef struct {
    logs_spiffs_level_t level;
    char message[LOGS_SPIFFS_MAX_MESSAGE_LEN];
} logs_spiffs_queue_item_t;

typedef struct {
    FILE *current_log_file;
    char current_log_path[64];
    SemaphoreHandle_t log_mutex;
    QueueHandle_t log_queue;
    TaskHandle_t log_task;
    SemaphoreHandle_t log_task_done;
    volatile bool shutdown_requested;
} logs_spiffs_context_t;

extern logs_spiffs_context_t g_logs_spiffs;

bool logs_spiffs_storage_init(void);
void logs_spiffs_storage_deinit(void);
bool logs_spiffs_storage_write_line(const char *message, int64_t timestamp);
void logs_spiffs_storage_list_existing(void);
esp_err_t logs_spiffs_storage_format(void);

bool logs_spiffs_worker_start(void);
void logs_spiffs_worker_stop(void);
bool logs_spiffs_worker_enqueue_message(logs_spiffs_level_t level, const char *message);
bool logs_spiffs_worker_enqueue_formatted(logs_spiffs_level_t level, const char *format, ...);
void logs_spiffs_worker_set_level(logs_spiffs_level_t level);
logs_spiffs_level_t logs_spiffs_worker_get_level(void);

#ifdef __cplusplus
}
#endif
