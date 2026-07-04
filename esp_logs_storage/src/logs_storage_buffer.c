/**
 * @file logs_storage_buffer.c
 * @brief 内存缓冲区：USB MSC 模式下暂存日志
 *
 * 职责：
 * - USB 模式下 FAT 已交给主机，APP 端无法写盘
 * - 日志暂存到单链表（按时间顺序追加到尾部）
 * - 主机弹出后由 flush_to_disk 一次性写回磁盘
 *
 * 缓冲区不持锁，调用者（worker / exit_usb_mode）需自行管理并发。
 */

#include "logs_storage_internal.h"
#include "logs_storage_rotation.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "LOG_MGR";

/* 重置缓冲区计数器（链表节点由 deinit 释放）。 */
void logs_storage_buffer_init(void) {
    g_logs_storage.buffer_head = NULL;
    g_logs_storage.buffer_tail = NULL;
    g_logs_storage.buffer_count = 0;
    g_logs_storage.buffer_bytes = 0;
}

/* 释放整个缓冲区链表并清零计数。 */
void logs_storage_buffer_deinit(void) {
    logs_storage_buffer_node_t *node = g_logs_storage.buffer_head;
    while (node) {
        logs_storage_buffer_node_t *next = node->next;
        free(node);
        node = next;
    }
    g_logs_storage.buffer_head = NULL;
    g_logs_storage.buffer_tail = NULL;
    g_logs_storage.buffer_count = 0;
    g_logs_storage.buffer_bytes = 0;
}

/* 追加一条日志到缓冲区链表尾部。分配失败时丢弃日志并返回 false。 */
bool logs_storage_buffer_push(const char *message, int64_t timestamp_ms) {
    logs_storage_buffer_node_t *node = malloc(sizeof(logs_storage_buffer_node_t));
    if (!node) {
        ESP_LOGW(TAG, "Buffer alloc failed, dropping log entry");
        return false;
    }
    node->entry.timestamp_ms = timestamp_ms;
    strncpy(node->entry.message, message, sizeof(node->entry.message) - 1);
    node->entry.message[sizeof(node->entry.message) - 1] = '\0';
    node->next = NULL;

    if (g_logs_storage.buffer_tail) {
        g_logs_storage.buffer_tail->next = node;
        g_logs_storage.buffer_tail = node;
    } else {
        g_logs_storage.buffer_head = node;
        g_logs_storage.buffer_tail = node;
    }
    g_logs_storage.buffer_count++;
    g_logs_storage.buffer_bytes += sizeof(logs_storage_buffer_node_t);
    return true;
}

/* 将缓冲区所有日志写入磁盘并释放节点。
 * 写入过程中按 max_file_size 自动轮转；调用者需持有 log_mutex。 */
void logs_storage_buffer_flush_to_disk(void) {
    if (g_logs_storage.buffer_count == 0) {
        return;
    }

    ESP_LOGI(TAG, "Flushing %d buffered entries to disk", (int)g_logs_storage.buffer_count);

    if (!g_logs_storage.current_log_file) {
        if (!rotate_log_file_unlocked()) {
            ESP_LOGE(TAG, "Failed to create log file for flush");
            return;
        }
    }

    logs_storage_buffer_node_t *node = g_logs_storage.buffer_head;
    while (node) {
        logs_storage_buffer_node_t *next = node->next;

        if (g_logs_storage.current_log_file) {
            long cur_pos = ftell(g_logs_storage.current_log_file);
            logs_storage_rotation_config_t cfg;
            logs_storage_rotation_config_get(&cfg);
            if (cur_pos > (long)cfg.max_file_size_bytes) {
                rotate_log_file_unlocked();
            }
            if (g_logs_storage.current_log_file) {
                fprintf(g_logs_storage.current_log_file, "[+%lld ms] %s\n",
                        (long long)node->entry.timestamp_ms, node->entry.message);
            }
        }

        free(node);
        node = next;
    }

    if (g_logs_storage.current_log_file) {
        fflush(g_logs_storage.current_log_file);
    }
    g_logs_storage.buffer_head = NULL;
    g_logs_storage.buffer_tail = NULL;
    size_t flushed = g_logs_storage.buffer_count;
    g_logs_storage.buffer_count = 0;
    g_logs_storage.buffer_bytes = 0;
    ESP_LOGI(TAG, "Flushed %d buffered entries to disk", (int)flushed);
}
