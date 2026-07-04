#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化日志管理模块（挂载 SPIFFS，创建初始日志文件）
 * @return true 成功，false 失败
 */
bool logs_spiffs_init(void);

/**
 * @brief 反初始化，关闭文件并卸载 SPIFFS
 */
void logs_spiffs_deinit(void);

typedef enum {
    LOGS_SPIFFS_LEVEL_INFO = 0,
    LOGS_SPIFFS_LEVEL_WARN = 1,
    LOGS_SPIFFS_LEVEL_ERROR = 2
} logs_spiffs_level_t;

/**
 * @brief 设置当前日志级别，低于该级别的日志将被忽略
 * @param level 允许写入的最低级别：INFO/WARN/ERROR
 */
void logs_spiffs_set_level(logs_spiffs_level_t level);

/**
 * @brief 写入一条日志（格式化字符串）
 * @param format printf 风格格式字符串
 * @param ... 可变参数
 * @note 线程安全，自动添加时间戳和换行
 */
void logs_spiffs_write(const char *format, ...);

/**
 * @brief 写入一条指定级别的日志
 * @param level 日志级别，支持 INFO/WARN/ERROR
 * @param format printf 风格格式字符串
 * @param ... 可变参数
 */
void logs_spiffs_write_level(logs_spiffs_level_t level, const char *format, ...);

/**
 * @brief 格式化 SPIFFS 分区（擦除所有数据）并返回结果
 * @note 成功时返回 `ESP_OK`，失败时返回相应 `esp_err_t` 错误码。
 *       函数不会自动重启系统，调用方可根据需要决定后续操作。
 * @return `ESP_OK` 表示成功，其他值表示失败
 */
esp_err_t logs_spiffs_format(void);

#ifdef __cplusplus
}
#endif

