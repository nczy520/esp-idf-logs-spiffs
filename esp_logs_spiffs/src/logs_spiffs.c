#include "logs_spiffs.h"
#include "logs_spiffs_internal.h"
#include <stdarg.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"

static const char *TAG = "LOG_MGR";

esp_err_t logs_spiffs_format(void) {
    ESP_LOGI(TAG, "Formatting SPIFFS partition...");
    esp_err_t ret = logs_spiffs_storage_format();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Format successful! Restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool logs_spiffs_init(void) {
    if (!logs_spiffs_worker_start()) {
        return false;
    }

    if (!logs_spiffs_storage_init()) {
        logs_spiffs_worker_stop();
        return false;
    }

    logs_spiffs_storage_list_existing();
    logs_spiffs_worker_enqueue_formatted(LOGS_SPIFFS_LEVEL_INFO, "[SYSTEM] System started");
    return true;
}

void logs_spiffs_set_level(logs_spiffs_level_t level) {
    logs_spiffs_worker_set_level(level);
}

void logs_spiffs_write(const char *format, ...) {
    if (!format) {
        return;
    }

    va_list args;
    va_start(args, format);
    char message[LOGS_SPIFFS_MAX_MESSAGE_LEN];
    int written = vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (written < 0 || written >= (int)sizeof(message)) {
        message[sizeof(message) - 1] = '\0';
    }

    logs_spiffs_worker_enqueue_message(LOGS_SPIFFS_LEVEL_INFO, message);
}

void logs_spiffs_write_level(logs_spiffs_level_t level, const char *format, ...) {
    if (!format) {
        return;
    }

    va_list args;
    va_start(args, format);
    char message[LOGS_SPIFFS_MAX_MESSAGE_LEN];
    int written = vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (written < 0 || written >= (int)sizeof(message)) {
        message[sizeof(message) - 1] = '\0';
    }

    logs_spiffs_worker_enqueue_message(level, message);
}

void logs_spiffs_deinit(void) {
    logs_spiffs_worker_stop();
    logs_spiffs_storage_deinit();
}
