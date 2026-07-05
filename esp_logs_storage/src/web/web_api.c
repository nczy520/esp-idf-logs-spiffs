/**
 * @file web_api.c
 * @brief HTTP API 处理器（RESTful API）
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */
#include "logs_storage_internal.h"
#include "esp_spiffs.h"
#include <sys/stat.h>
#include <errno.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <lwip/sockets.h>

static const char *TAG = "LOG_WEB_API";

/* 将 IP 地址转换为字符串 */
static void ip_to_string(uint32_t ip, char *str, size_t len) {
    snprintf(str, len, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xff),
             (unsigned)((ip >> 16) & 0xff),
             (unsigned)((ip >> 8) & 0xff),
             (unsigned)(ip & 0xff));
}

/* 获取存储空间统计信息 */
static esp_err_t api_stats_handler(httpd_req_t *req) {
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get SPIFFS info");
    }

    // 获取文件数量
    int file_count = 0;
    char **file_paths = NULL;
    get_sorted_log_files(&file_paths, &file_count);
    if (file_paths) {
        for (int i = 0; i < file_count; i++) {
            free(file_paths[i]);
        }
        free(file_paths);
    }

    char out[512];
    snprintf(out, sizeof(out),
             "{\"total_bytes\":%llu,\"used_bytes\":%llu,\"free_bytes\":%llu,\"usage_percent\":%.1f,\"file_count\":%d}",
             (unsigned long long)total, (unsigned long long)used, (unsigned long long)(total - used),
             total > 0 ? (double)used * 100.0 / (double)total : 0.0, file_count);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* 列出日志文件 */
static esp_err_t api_list_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, ">>> API LIST: 收到文件列表请求");

    char **file_paths = NULL;
    int file_count = 0;

    if (get_sorted_log_files(&file_paths, &file_count) != ESP_OK || file_count == 0) {
        ESP_LOGW(TAG, "=== API LIST: No files found ===");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        if (file_paths) {
            for (int i = 0; i < file_count; i++) {
                if (file_paths[i]) free(file_paths[i]);
            }
            free(file_paths);
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== API LIST: Found %d files ===", file_count);

    // 直接构建 JSON，避免复杂的内存管理
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    
    bool first = true;
    for (int i = 0; i < file_count; i++) {
        if (!file_paths[i]) continue;
        
        const char *full_path = file_paths[i];
        const char *name = strrchr(full_path, '/');
        name = name ? name + 1 : full_path;
        
        struct stat st;
        int stat_result = stat(full_path, &st);
        if (stat_result == 0) {
            // SPIFFS may report size 0 for open files, use fallback
            long file_size = st.st_size;
            if (file_size == 0) {
                // Try to get size by opening the file
                FILE *fp = fopen(full_path, "r");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    file_size = ftell(fp);
                    fclose(fp);
                    ESP_LOGD(TAG, "File %s: stat size=0, actual size=%ld", name, file_size);
                }
            }
            
            char entry[256];
            snprintf(entry, sizeof(entry), 
                     "%s{\"name\":\"%s\",\"size\":%ld,\"mtime\":%ld}",
                     first ? "" : ",", name, file_size, (long)st.st_mtime);
            httpd_resp_sendstr_chunk(req, entry);
            first = false;
            ESP_LOGD(TAG, "API LIST: added %s (%ld bytes)", name, file_size);
        } else {
            ESP_LOGW(TAG, "Skipping %s (stat failed: errno=%d)", name, errno);
        }
    }
    
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);  // 结束响应

    // 清理文件路径数组
    for (int i = 0; i < file_count; i++) {
        if (file_paths[i]) free(file_paths[i]);
    }
    free(file_paths);

    return ESP_OK;
}

/* 下载日志文件 - 使用查询参数 ?file=xxx */
static esp_err_t api_download_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, ">>> API DOWNLOAD: uri=%s", req->uri);
    
    /* 从查询参数中获取文件名 */
    char filename[64] = {0};
    esp_err_t ret = httpd_req_get_url_query_str(req, filename, sizeof(filename));
    if (ret != ESP_OK || strlen(filename) == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
    }
    
    /* 解析 file 参数 */
    char file_param[64] = {0};
    ret = httpd_query_key_value(filename, "file", file_param, sizeof(file_param));
    if (ret != ESP_OK || strlen(file_param) == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid file parameter");
    }

    char path[128];
    snprintf(path, sizeof(path), "/storage/%s", file_param);
    ESP_LOGI(TAG, ">>> API DOWNLOAD: path=%s", path);

    struct stat st;
    if (stat(path, &st) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    char disposition[128];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", file_param);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_send(req, NULL, st.st_size);

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

/* 清除所有日志 */
static esp_err_t api_clear_handler(httpd_req_t *req) {
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST only");
    }

    ESP_LOGI(TAG, "Clearing all logs and restarting...");

    // 1. 暂停日志记录（关闭当前文件）
    if (g_logs_storage.current_log_file) {
        fclose(g_logs_storage.current_log_file);
        g_logs_storage.current_log_file = NULL;
    }

    // 2. 删除所有日志文件
    char **file_paths = NULL;
    int file_count = 0;
    if (get_sorted_log_files(&file_paths, &file_count) == ESP_OK) {
        for (int i = 0; i < file_count; i++) {
            if (file_paths[i]) {
                ESP_LOGI(TAG, "Deleting: %s", file_paths[i]);
                unlink(file_paths[i]);
                free(file_paths[i]);
            }
        }
        free(file_paths);
    }

    // 3. 发送响应
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"files_cleared\":true,\"rebooting\":true}", HTTPD_RESP_USE_STRLEN);

    // 4. 重启设备
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

/* 重启设备 */
static esp_err_t api_restart_handler(httpd_req_t *req) {
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST only");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"rebooting\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* 检查客户端连接状态 */
static esp_err_t api_connect_handler(httpd_req_t *req) {
    (void)req;
    return httpd_resp_send(req, "{\"connected\":false}", HTTPD_RESP_USE_STRLEN);
}

/* 获取系统信息 */
static esp_err_t api_system_handler(httpd_req_t *req) {
    char system_info[512];
    int pos = 0;

    pos += snprintf(system_info + pos, sizeof(system_info) - pos,
                   "{\"model\":\"ESP32\",\"version\":\"1.1.2\",\"sdk\":\"%s\"",
                   esp_get_idf_version());

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        char ip_str[32];
        ip_to_string(ip_info.ip.addr, ip_str, sizeof(ip_str));
        pos += snprintf(system_info + pos, sizeof(system_info) - pos,
                       ",\"ip\":\"%s\"", ip_str);
    }

    int64_t timestamp = esp_timer_get_time() / 1000;
    snprintf(system_info + pos, sizeof(system_info) - pos,
             ",\"timestamp\":%lld}", (long long)timestamp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, system_info, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* 注册所有 API 路由 */
bool web_api_register_routes(httpd_handle_t server) {
    httpd_uri_t uris[] = {
        { .uri = "/api/stats",     .method = HTTP_GET,  .handler = api_stats_handler, .user_ctx = NULL },
        { .uri = "/api/list",      .method = HTTP_GET,  .handler = api_list_handler,     .user_ctx = NULL },
        { .uri = "/api/download",  .method = HTTP_GET,  .handler = api_download_handler, .user_ctx = NULL },
        { .uri = "/api/clear",     .method = HTTP_POST, .handler = api_clear_handler,    .user_ctx = NULL },
        { .uri = "/api/restart",   .method = HTTP_POST, .handler = api_restart_handler,  .user_ctx = NULL },
        { .uri = "/api/connect",   .method = HTTP_GET,  .handler = api_connect_handler,  .user_ctx = NULL },
        { .uri = "/api/system",    .method = HTTP_GET,  .handler = api_system_handler,   .user_ctx = NULL },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register API route: %s", uris[i].uri);
            return false;
        }
    }
    return true;
}