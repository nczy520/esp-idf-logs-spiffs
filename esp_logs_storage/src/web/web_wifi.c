/**
 * @file web_wifi.c
 * @brief WiFi AP 管理模块
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */
#include "logs_storage_internal.h"
#include "logs_storage_web_config.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "LOG_WEB_WIFI";
static volatile bool s_ap_running = false;
extern volatile int s_sta_count;
extern void (*s_client_connected_cb)(void);

/* WiFi 事件处理 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    (void)arg; (void)event_data;
    if (event_base != WIFI_EVENT) return;

    switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED:
            s_sta_count++;
            ESP_LOGI(TAG, "Client connected, total clients: %d", s_sta_count);
            if (s_client_connected_cb) {
                s_client_connected_cb();
            }
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_sta_count > 0) {
                s_sta_count--;
            }
            ESP_LOGI(TAG, "Client disconnected, total clients: %d", s_sta_count);
            break;
    }
}

/* 启动 WiFi AP */
bool web_wifi_start_ap(const logs_storage_web_config_t *cfg) {
    esp_err_t err;

    // 初始化 NVS
    err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }

    // 初始化网络接口
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    // 创建事件循环
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create failed: %s", esp_err_to_name(err));
        return false;
    }

    // 创建 WiFi AP 网络接口
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg_init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg_init);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return false;
    }

    // 注册事件处理器
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event_handler_register failed: %s", esp_err_to_name(err));
        return false;
    }

    // 设置为 AP 模式
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode failed: %s", esp_err_to_name(err));
        return false;
    }

    // 配置 AP
    wifi_config_t ap_config = {0};
    
    // 生成 SSID
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "LOGS-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    
    // 设置密码
    const char *password = cfg->ap_password;
    size_t password_len = strlen(password);
    if (password_len < 8 || password_len > 63) {
        ESP_LOGW(TAG, "Password length %d is invalid, using default", (int)password_len);
        password = "12345678";
        password_len = strlen(password);
    }
    strncpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.password[sizeof(ap_config.ap.password) - 1] = '\0';
    
    // 设置其他参数
    ap_config.ap.channel = cfg->ap_channel;
    ap_config.ap.max_connection = cfg->ap_max_connections;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    
    // 设置 beacon interval (推荐 100)
    ap_config.ap.beacon_interval = 100;

    ESP_LOGI(TAG, "WiFi AP Config: SSID=%s, password=%s, channel=%d, max_conn=%d",
             ap_config.ap.ssid, ap_config.ap.password, cfg->ap_channel, cfg->ap_max_connections);

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(err));
        return false;
    }

    s_ap_running = true;
    ESP_LOGI(TAG, "WiFi AP started successfully");
    return true;
}

/* 停止 WiFi AP */
void web_wifi_stop(void) {
    if (!s_ap_running) return;
    esp_wifi_stop();
    s_ap_running = false;
    ESP_LOGI(TAG, "WiFi AP stopped");
}
