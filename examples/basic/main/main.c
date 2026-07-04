#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "logs_spiffs.h"
#include "logs_spiffs_rotation.h"

static const char *TAG = "logs_spiffs_example";

void app_main(void)
{
    ESP_LOGI(TAG, "1) configure rotation");
    logs_spiffs_rotation_config_t cfg;
    logs_spiffs_rotation_config_default(&cfg);
    cfg.max_file_size_bytes = 256u * 1024u;
    cfg.max_log_files = 10u;
    cfg.min_free_space_bytes = 128u * 1024u;
    cfg.rotate_threshold_bytes = 32u * 1024u;
    logs_spiffs_rotation_config_set(&cfg);

    ESP_LOGI(TAG, "2) init");
    if (!logs_spiffs_init()) {
        ESP_LOGE(TAG, "Failed to initialize logger");
        return;
    }

    ESP_LOGI(TAG, "3) set level to WARN");
    logs_spiffs_set_level(LOGS_SPIFFS_LEVEL_WARN);

    ESP_LOGI(TAG, "4) write default log (INFO, will be filtered out)");
    logs_spiffs_write("Hello from logs_spiffs example");

    ESP_LOGI(TAG, "5) write level-specific log (WARN/ERROR will be persisted)");
    logs_spiffs_write_level(LOGS_SPIFFS_LEVEL_WARN, "This is a warning example");
    logs_spiffs_write_level(LOGS_SPIFFS_LEVEL_ERROR, "This is an error example");

    ESP_LOGI(TAG, "6) deinit");
    logs_spiffs_deinit();

    /* Uncomment the following line to format the SPIFFS partition.
     * NOTE: logs_spiffs_format() calls esp_restart() internally on success,
     * so any code after it will never execute. */
    /* (void)logs_spiffs_format(); */
}
