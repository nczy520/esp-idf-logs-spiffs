#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include "logs_storage.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "tinyusb_msc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 队列/任务配置 */
#define LOGS_STORAGE_QUEUE_LENGTH          64   /**< 日志队列容量（条目数） */
#define LOGS_STORAGE_MAX_MESSAGE_LEN       384  /**< 单条日志最大长度（含 '\0'） */
#define LOGS_STORAGE_TASK_STACK_WORDS      8192 /**< worker 任务栈大小（字） */
#define LOGS_STORAGE_TASK_PRIORITY         5    /**< worker 任务优先级 */
#define LOGS_STORAGE_QUEUE_TIMEOUT_MS      100  /**< 入队超时，超时则丢弃日志 */
#define LOGS_STORAGE_BATCH_FLUSH_INTERVAL_MS 200 /**< 批量 flush 间隔（ms） */
#define LOGS_STORAGE_BATCH_MAX_ITEMS       8    /**< 批量最大条目数，达到立即 flush */
#define LOGS_STORAGE_USB_MONITOR_STACK_WORDS  4096 /**< USB 监控任务栈大小（字） */
#define LOGS_STORAGE_USB_MONITOR_PRIORITY     4    /**< USB 监控任务优先级（低于 worker） */
#define LOGS_STORAGE_USB_MONITOR_POLL_MS      500  /**< USB 监控轮询间隔（ms） */

/* 存储路径与文件名常量（被 backend / file_utils 共享） */
#define STORAGE_BASE_PATH      "/storage"
#define LOG_FILE_PREFIX        "log_"
#define LOG_FILE_EXT           ".log"
#define FILENAME_NUM_DIGITS    6

/** 内存缓冲区单条记录：消息+时间戳，USB 模式下暂存 */
typedef struct {
    int64_t timestamp_ms;
    char message[LOGS_STORAGE_MAX_MESSAGE_LEN];
} logs_storage_buffer_entry_t;

/** 内存缓冲区：单链表节点 */
typedef struct logs_storage_buffer_node {
    logs_storage_buffer_entry_t entry;
    struct logs_storage_buffer_node *next;
} logs_storage_buffer_node_t;

/* 队列条目：worker 消费的最小单元，level 用于过滤。 */
typedef struct {
    logs_storage_level_t level;
    char message[LOGS_STORAGE_MAX_MESSAGE_LEN];
} logs_storage_queue_item_t;

/* 全局存储上下文（单例 g_logs_storage）。
 * log_mutex 保护 current_log_file/_path 及 FAT 操作；
 * usb_mode_active 决定 worker 写盘还是写内存缓冲。 */
typedef struct {
    FILE *current_log_file;                  /**< 当前日志文件句柄 */
    char current_log_path[64];               /**< 当前日志文件路径 */
    SemaphoreHandle_t log_mutex;             /**< 保护 FAT 操作与文件句柄 */
    QueueHandle_t log_queue;                 /**< 日志消息队列 */
    TaskHandle_t log_task;                   /**< worker 任务句柄 */
    SemaphoreHandle_t log_task_done;         /**< worker 退出信号量 */
    volatile bool shutdown_requested;        /**< 通知 worker 退出 */
    volatile bool usb_mode_active;           /**< USB MSC 模式标志，true 时日志写入内存缓冲 */
    volatile bool usb_monitor_exit;          /**< 通知 USB 监控任务退出 */
    TaskHandle_t usb_monitor_task;           /**< USB 弹出监控任务句柄 */
    tinyusb_msc_storage_handle_t msc_storage;/**< MSC 存储句柄（FAT + WL） */
    logs_storage_buffer_node_t *buffer_head; /**< 内存缓冲链表头 */
    logs_storage_buffer_node_t *buffer_tail; /**< 内存缓冲链表尾 */
    size_t buffer_count;                     /**< 内存缓冲条目数 */
    size_t buffer_bytes;                     /**< 内存缓冲总字节数 */
} logs_storage_context_t;

extern logs_storage_context_t g_logs_storage;

/* ===== 后端核心（logs_storage_backend.c）===== */
bool logs_storage_backend_init(void);
void logs_storage_backend_deinit(void);
bool logs_storage_write_line(const char *message, int64_t timestamp);
void logs_storage_list_existing(void);
esp_err_t logs_storage_backend_format(void);
/* 轮转：调用者需持有 log_mutex；buffer.c 的 flush_to_disk 也会调用 */
bool rotate_log_file_unlocked(void);

/* ===== 文件工具（logs_storage_file_utils.c）=====
 * 所有 *_unlocked 函数要求调用者先持有 g_logs_storage.log_mutex。 */
bool get_fat_free_bytes(const char *path, size_t *out_total, size_t *out_free);
esp_err_t get_sorted_log_files_unlocked(char ***out_paths, int *out_count);
int  delete_oldest_logs_unlocked(int keep_count, size_t need_free_bytes);
bool ensure_free_space_unlocked(size_t required_bytes);
void enforce_max_file_count_unlocked(void);
void generate_log_path(char *out_path, int number);
int  get_next_log_number_unlocked(void);

/* ===== 内存缓冲区（logs_storage_buffer.c）===== */
/** USB 模式下暂存日志，恢复后 flush 到磁盘 */
void logs_storage_buffer_init(void);
void logs_storage_buffer_deinit(void);
bool logs_storage_buffer_push(const char *message, int64_t timestamp_ms);
void logs_storage_buffer_flush_to_disk(void);

/* ===== USB 模式切换（logs_storage_usb.c）===== */
/** 进入时卸载 FAT 并启用内存缓冲，退出时恢复 FAT 并 flush */
esp_err_t logs_storage_enter_usb_mode(void);
esp_err_t logs_storage_exit_usb_mode(void);

/* ===== Worker 任务（logs_storage_worker.c）===== */
bool logs_storage_worker_start(void);
void logs_storage_worker_stop(void);
bool logs_storage_worker_enqueue_message(logs_storage_level_t level, const char *message);
bool logs_storage_worker_enqueue_formatted(logs_storage_level_t level, const char *format, ...);
void logs_storage_worker_set_level(logs_storage_level_t level);
logs_storage_level_t logs_storage_worker_get_level(void);

#ifdef __cplusplus
}
#endif
