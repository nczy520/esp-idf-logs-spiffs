/**
 * @file web_dns_server.c
 * @brief DNS 服务器 - 将所有域名解析指向设备 IP（Captive Portal 核心）
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */
#include "logs_storage_internal.h"
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LOG_DNS";
static TaskHandle_t s_dns_task = NULL;
static volatile bool s_dns_running = false;

/* DNS 请求头部 */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
} dns_header_t;

/* DNS 响应记录 */
typedef struct __attribute__((packed)) {
    uint16_t name;      // 压缩指针
    uint16_t type;      // A记录 = 1
    uint16_t class;     // IN = 1
    uint32_t ttl;       // TTL
    uint16_t data_len;  // 数据长度
    uint32_t ip;        // IP地址
} dns_answer_t;

static void dns_server_task(void *arg) {
    (void)arg;
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started on port 53");
    s_dns_running = true;

    uint8_t rx_buffer[512];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (s_dns_running) {
        ssize_t len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                               (struct sockaddr *)&client_addr, &client_len);
        if (len < 0) {
            continue;
        }

        if (len < sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *header = (dns_header_t *)rx_buffer;
        
        /* 解析查询部分 */
        uint16_t flags = ntohs(header->flags);
        uint16_t questions = ntohs(header->questions);
        
        /* 只处理标准查询 (QR=0, OPCODE=0) */
        if ((flags & 0x8000) != 0 || (flags & 0x7800) != 0) {
            continue;
        }

        /* 跳过查询名称，找到查询结束位置 */
        uint8_t *query_ptr = rx_buffer + sizeof(dns_header_t);
        uint8_t *query_end = query_ptr;
        
        /* 跳过所有查询名称 */
        for (int i = 0; i < questions; i++) {
            while (query_end < rx_buffer + len) {
                uint8_t label_len = *query_end;
                if (label_len == 0) {
                    query_end++;  // 跳过终止符
                    break;
                }
                if ((label_len & 0xC0) == 0xC0) {
                    query_end += 2;  // 压缩指针
                    break;
                }
                query_end += label_len + 1;
            }
            query_end += 4;  // 跳过 TYPE 和 CLASS
        }

        /* 构建响应 */
        header->flags = htons(0x8180);  // 标准响应，无错误
        header->answers = htons(questions);
        header->authority = htons(0);
        header->additional = htons(0);

        /* 在查询后添加响应 */
        dns_answer_t *answer = (dns_answer_t *)query_end;
        answer->name = htons(0xC00C);    // 压缩指针，指向第一个查询名称
        answer->type = htons(1);         // A记录
        answer->class = htons(1);        // IN
        answer->ttl = htonl(300);        // TTL 5分钟
        answer->data_len = htons(4);     // IPv4 长度
        answer->ip = htonl(0xC0A80401);  // 192.168.4.1

        size_t response_len = (uint8_t *)(answer + 1) - rx_buffer;
        
        sendto(sock, rx_buffer, response_len, 0,
               (struct sockaddr *)&client_addr, client_len);
    }

    close(sock);
    vTaskDelete(NULL);
}

/* 启动 DNS 服务器 */
void web_dns_start(void) {
    if (s_dns_task != NULL) {
        return;
    }
    
    s_dns_running = true;
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &s_dns_task);
    ESP_LOGI(TAG, "DNS server starting...");
}

/* 停止 DNS 服务器 */
void web_dns_stop(void) {
    s_dns_running = false;
    s_dns_task = NULL;
    ESP_LOGI(TAG, "DNS server stopped");
}
