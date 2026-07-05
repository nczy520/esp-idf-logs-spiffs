/**
 * @file logs_storage_rotation.c
 * @brief 日志轮转配置（单例）
 *
 * 提供默认值与运行时 get/set 接口。
 * 配置项含义见 logs_storage_rotation.h 字段注释。
 * 注意：set 不是线程安全的，应在 init 之前调用。
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */

#include "logs_storage_rotation.h"
#include "sdkconfig.h"

/* 默认配置：从 menuconfig 读取 */
static logs_storage_rotation_config_t s_rotation_config = {
    .min_free_space_bytes = CONFIG_LOGS_STORAGE_MIN_FREE_SPACE,
    .max_file_size_bytes = CONFIG_LOGS_STORAGE_MAX_FILE_SIZE,
    .max_log_files = CONFIG_LOGS_STORAGE_MAX_FILE_COUNT,
    .rotate_threshold_bytes = CONFIG_LOGS_STORAGE_ROTATE_THRESHOLD,
    .max_files_open = 5u
};

/* 拷贝当前配置到 *cfg（与 set/default 共享同一份静态配置）。 */
void logs_storage_rotation_config_default(logs_storage_rotation_config_t *cfg) {
    if (cfg != NULL) {
        *cfg = s_rotation_config;
    }
}

/* 读取当前配置 */
void logs_storage_rotation_config_get(logs_storage_rotation_config_t *cfg) {
    if (cfg != NULL) {
        *cfg = s_rotation_config;
    }
}

/* 覆盖当前配置。非线程安全，应在初始化前调用。 */
void logs_storage_rotation_config_set(const logs_storage_rotation_config_t *cfg) {
    if (cfg != NULL) {
        s_rotation_config = *cfg;
    }
}
