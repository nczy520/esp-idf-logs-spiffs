/**
 * @file logs_storage_web_config.c
 * @brief Web 服务配置：默认值、set/get
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */

#include "logs_storage_web_config.h"
#include "sdkconfig.h"
#include <string.h>

static logs_storage_web_config_t s_web_config = {
    .auto_start         = CONFIG_LOGS_STORAGE_AUTO_START_WEB,
    .idle_timeout_ms    = CONFIG_LOGS_STORAGE_IDLE_TIMEOUT_MS,
    .http_port          = CONFIG_LOGS_STORAGE_WEB_PORT,
    .ap_channel         = CONFIG_LOGS_STORAGE_AP_CHANNEL,
    .ap_max_connections = CONFIG_LOGS_STORAGE_AP_MAX_CONNECTIONS,
    .ap_password        = CONFIG_LOGS_STORAGE_AP_PASSWORD,
};

void logs_storage_web_config_default(logs_storage_web_config_t *cfg) {
    if (cfg != NULL) {
        *cfg = s_web_config;
    }
}

void logs_storage_web_config_set(const logs_storage_web_config_t *cfg) {
    if (cfg != NULL) {
        s_web_config = *cfg;
    }
}

void logs_storage_web_config_get(logs_storage_web_config_t *cfg) {
    if (cfg != NULL) {
        *cfg = s_web_config;
    }
}
