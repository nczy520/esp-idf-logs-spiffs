#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化日志管理模块
 * @return true 成功，false 失败
 */
bool logs_storage_init(void);

/**
 * @brief 反初始化，关闭文件并卸载 storage
 */
void logs_storage_deinit(void);

typedef enum {
    LOGS_STORAGE_LEVEL_INFO = 0,
    LOGS_STORAGE_LEVEL_WARN = 1,
    LOGS_STORAGE_LEVEL_ERROR = 2
} logs_storage_level_t;

/**
 * @brief 设置最低日志级别
 */
void logs_storage_set_level(logs_storage_level_t level);

/**
 * @brief 写入一条 INFO 级别日志
 */
void logs_storage_write(const char *format, ...);

/**
 * @brief 写入一条指定级别的日志
 */
void logs_storage_write_level(logs_storage_level_t level, const char *format, ...);

/**
 * @brief 格式化 storage 分区
 */
esp_err_t logs_storage_format(void);

/**
 * @brief 暴露 storage 分区为 USB MSC U 盘
 * @note  日志记录不会停止，U 盘模式下日志暂存在内存中。
 *        主机端"安全弹出"后，内存日志自动写入磁盘并重启恢复。
 */
void logs_storage_usb_msc_init(void);

/**
 * @brief 停止 USB MSC，恢复磁盘日志记录
 */
void logs_storage_usb_msc_deinit(void);

/**
 * @brief 查询当前是否处于 USB MSC 模式
 */
bool logs_storage_is_usb_mode_active(void);

/**
 * @brief 强制退出 USB 模式（用于重启前）
 * @note  通知弹出监控任务跳过检测，避免复位过程中的死循环。
 */
void logs_storage_force_exit_usb_mode(void);

#ifdef __cplusplus
}
#endif
