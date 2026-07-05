/**
 * @file web_main.c
 * @brief HTTP 服务器主控制模块（协调 WiFi、API、UI、DNS）
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */
#include "logs_storage_internal.h"
#include "logs_storage_web_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "web_constants.h"

static const char *TAG = "LOG_WEB_MAIN";
static httpd_handle_t s_http_server = NULL;
static EventGroupHandle_t s_events = NULL;
/* 共享变量 - 非 static 以便其他模块访问 */
volatile int s_sta_count = 0;
void (*s_client_connected_cb)(void) = NULL;
static bool s_web_ui_forced = false;

/* 外部声明 */
extern bool web_wifi_start_ap(const logs_storage_web_config_t *cfg);
extern void web_wifi_stop(void);
extern bool web_api_register_routes(httpd_handle_t server);
extern bool web_ui_register_routes(httpd_handle_t server);
extern void web_dns_start(void);
extern void web_dns_stop(void);

/* 客户端连接回调 - 强制打开WEB界面 */
static void client_connected_callback(void) {
    if (s_events == NULL) return;
    xEventGroupSetBits(s_events, BIT_CLIENT_CONNECTED);
    if (!s_web_ui_forced) {
        s_web_ui_forced = true;
        ESP_LOGI(TAG, "Forcing WEB UI redirect for new client");
    }
}

/* 空闲监控任务 */
static void idle_monitor_task(void *arg) {
    (void)arg;
    logs_storage_web_config_t cfg;
    logs_storage_web_config_get(&cfg);

    ESP_LOGI(TAG, "Idle monitor: will close AP after %lu ms without client",
             (unsigned long)cfg.idle_timeout_ms);

    while (true) {
        if (xEventGroupWaitBits(s_events, BIT_SHUTDOWN_REQUESTED,
                                pdFALSE, pdTRUE, pdMS_TO_TICKS(1000)) & BIT_SHUTDOWN_REQUESTED) {
            break;
        }
        if (s_sta_count > 0) continue;

        TickType_t start = xTaskGetTickCount();
        TickType_t timeout_ticks = pdMS_TO_TICKS(cfg.idle_timeout_ms);

        while (s_sta_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if ((xTaskGetTickCount() - start) >= timeout_ticks) {
                ESP_LOGW(TAG, "No client connected for %lu ms, closing AP + HTTP",
                         (unsigned long)cfg.idle_timeout_ms);
                logs_storage_web_stop();
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

/* 启动 HTTP 服务器 */
static httpd_handle_t start_http_server(const logs_storage_web_config_t *cfg) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = cfg->http_port;
    config.ctrl_port = 32768;
    config.max_open_sockets = cfg->ap_max_connections + 1;
    config.lru_purge_enable = true;
    config.max_resp_headers = 256;
    config.max_uri_handlers = 24;  // 增加最大 URI 处理器数量

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", cfg->http_port);
        return NULL;
    }

    if (!web_ui_register_routes(server) || !web_api_register_routes(server)) {
        ESP_LOGE(TAG, "Failed to register routes");
        httpd_stop(server);
        return NULL;
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", cfg->http_port);
    return server;
}

/* 启动 Web 服务 */
void logs_storage_web_start(void) {
    logs_storage_web_config_t cfg;
    logs_storage_web_config_get(&cfg);

    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            ESP_LOGE(TAG, "Failed to create event group");
            return;
        }
    }

    xEventGroupClearBits(s_events, BIT_CLIENT_CONNECTED | BIT_SHUTDOWN_REQUESTED);
    s_web_ui_forced = false;
    s_sta_count = 0;

    // 设置客户端连接回调
    s_client_connected_cb = client_connected_callback;

    // 启动 WiFi AP
    if (!web_wifi_start_ap(&cfg)) {
        ESP_LOGE(TAG, "Failed to start WiFi AP");
        return;
    }

    // 启动 DNS 服务器（Captive Portal 核心）
    web_dns_start();

    // 启动 HTTP 服务器
    s_http_server = start_http_server(&cfg);
    if (s_http_server == NULL) {
        web_wifi_stop();
        web_dns_stop();
        return;
    }

    // 启动空闲监控任务
    xTaskCreate(idle_monitor_task, "web_idle", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Web service started. Connect to WiFi AP to auto-open web page");
}

/* 停止 Web 服务 */
void logs_storage_web_stop(void) {
    if (s_events) {
        xEventGroupSetBits(s_events, BIT_SHUTDOWN_REQUESTED);
        xEventGroupClearBits(s_events, BIT_CLIENT_CONNECTED);
    }

    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }

    web_dns_stop();
    web_wifi_stop();
    s_web_ui_forced = false;
}

/* 获取客户端连接数量（供 API 使用） */
int web_get_client_count(void) {
    return s_sta_count;
}
