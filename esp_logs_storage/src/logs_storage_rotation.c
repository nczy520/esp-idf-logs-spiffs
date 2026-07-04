/**
 * @file logs_storage_rotation.c
 * @brief 日志轮转配置（单例）
 *
 * 提供默认值与运行时 get/set 接口。
 * 配置项含义见 logs_storage_rotation.h 字段注释。
 * 注意：set 不是线程安全的，应在 init 之前调用。
 */

#include "logs_storage_rotation.h"

/* 默认配置：256KB 最低剩余 / 512KB 单文件 / 保留 99 个 / 8KB 轮转阈值 / 同时打开 5 个 */
static logs_storage_rotation_config_t s_rotation_config = {
    .min_free_space_bytes = 256u * 1024u,
    .max_file_size_bytes = 512u * 1024u,
    .max_log_files = 99u,
    .rotate_threshold_bytes = 8u * 1024u,
    .max_files_open = 5u
};

/* 拷贝当前配置到 *cfg（与 set/default 共享同一份静态配置）。 */
void logs_storage_rotation_config_default(logs_storage_rotation_config_t *cfg) {
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

/* 读取当前配置到 *cfg。 */
void logs_storage_rotation_config_get(logs_storage_rotation_config_t *cfg) {
    if (cfg != NULL) {
        *cfg = s_rotation_config;
    }
}
