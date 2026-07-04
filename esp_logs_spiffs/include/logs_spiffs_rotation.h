#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t min_free_space_bytes;   /**< 触发清理前的最低剩余空间，单位字节 */
    size_t max_file_size_bytes;    /**< 单个日志文件的最大尺寸，单位字节 */
    size_t max_log_files;          /**< 保留的日志文件数量上限 */
    size_t rotate_threshold_bytes; /**< 触发轮转前的最小可用空间阈值，单位字节 */
    size_t max_files_open;         /**< SPIFFS 允许同时打开的文件数量 */
} logs_spiffs_rotation_config_t;

/**
 * @brief 获取默认轮转配置
 */
void logs_spiffs_rotation_config_default(logs_spiffs_rotation_config_t *cfg);

/**
 * @brief 设置当前轮转配置
 */
void logs_spiffs_rotation_config_set(const logs_spiffs_rotation_config_t *cfg);

/**
 * @brief 读取当前轮转配置
 */
void logs_spiffs_rotation_config_get(logs_spiffs_rotation_config_t *cfg);

#ifdef __cplusplus
}
#endif
