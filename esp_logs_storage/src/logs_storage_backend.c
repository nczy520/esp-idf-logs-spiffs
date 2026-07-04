/**
 * @file logs_storage_backend.c
 * @brief 存储后端核心：FAT 挂载、日志轮转、写入入口
 *
 * 职责：
 * - 挂载 FAT 文件系统（基于 wear levelling + TinyUSB MSC 存储）
 * - 日志文件轮转（关闭旧文件 → 腾空间 → 创建新文件）
 * - 提供写入/列举/格式化的对外入口
 *
 * 文件工具（解析/枚举/清理/编号）见 logs_storage_file_utils.c
 * 内存缓冲（USB 模式暂存）见 logs_storage_buffer.c
 * USB 模式切换（FAT 卸载/弹出监控）见 logs_storage_usb.c
 */

#include "logs_storage_internal.h"
#include "logs_storage_rotation.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "tinyusb.h"

static const char *TAG = "LOG_MGR";

/* 日志文件轮转：关闭当前文件，腾出空间并执行数量限制，然后创建新文件。
 * 成功后 current_log_file 指向新打开的文件，current_log_path 填入新路径。
 * 调用者必须先持有 log_mutex。 */
bool rotate_log_file_unlocked(void) {
    logs_storage_rotation_config_t cfg;
    logs_storage_rotation_config_get(&cfg);

    if (g_logs_storage.current_log_file) {
        fclose(g_logs_storage.current_log_file);
        g_logs_storage.current_log_file = NULL;
    }

    if (!ensure_free_space_unlocked(cfg.rotate_threshold_bytes)) {
        ESP_LOGE(TAG, "Cannot create new log file, insufficient space");
        return false;
    }

    enforce_max_file_count_unlocked();

    int next_num = get_next_log_number_unlocked();
    generate_log_path(g_logs_storage.current_log_path, next_num);
    g_logs_storage.current_log_file = fopen(g_logs_storage.current_log_path, "a");
    if (!g_logs_storage.current_log_file) {
        ESP_LOGE(TAG, "Failed to create log file: %s", g_logs_storage.current_log_path);
        return false;
    }

    setvbuf(g_logs_storage.current_log_file, NULL, _IOLBF, 256);
    ESP_LOGI(TAG, "New log file: %s (number %d)", g_logs_storage.current_log_path, next_num);
    return true;
}

/* 初始化存储后端：挂载 FAT 文件系统并创建首个日志文件。
 * - 若 MSC 存储已存在（例如 USB 退出后重新初始化），仅切换挂载点到 APP 端。
 * - 首次调用会查找 storage 分区，挂载 wear levelling，并创建 MSC 存储。
 * 内部获取 log_mutex，调用者无需自行加锁。 */
bool logs_storage_backend_init(void) {
    if (xSemaphoreTake(g_logs_storage.log_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    logs_storage_rotation_config_t cfg;
    logs_storage_rotation_config_get(&cfg);

    /* 若 MSC 存储已存在（例如 usb_msc_deinit 后重新 init），仅切换挂载点并创建日志文件 */
    if (g_logs_storage.msc_storage != NULL) {
        tinyusb_msc_set_storage_mount_point(g_logs_storage.msc_storage, TINYUSB_MSC_STORAGE_MOUNT_APP);

        size_t total = 0;
        size_t free_bytes = 0;
        if (get_fat_free_bytes(STORAGE_BASE_PATH, &total, &free_bytes)) {
            size_t used = total - free_bytes;
            ESP_LOGI(TAG, "FAT mounted, total: %d KB, used: %d KB, free: %d KB", (int)(total / 1024), (int)(used / 1024), (int)(free_bytes / 1024));
        }

        if (!rotate_log_file_unlocked()) {
            ESP_LOGE(TAG, "Failed to create initial log file");
            xSemaphoreGive(g_logs_storage.log_mutex);
            return false;
        }
        xSemaphoreGive(g_logs_storage.log_mutex);
        return true;
    }

    /* 首次初始化：查找分区 → 挂载 WL → 创建 MSC 存储（自动挂载 FAT 到 APP） */
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_ANY,
                                                           "storage");
    if (!part) {
        ESP_LOGE(TAG, "storage partition not found");
        xSemaphoreGive(g_logs_storage.log_mutex);
        return false;
    }

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t err = wl_mount(part, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount failed: %s", esp_err_to_name(err));
        xSemaphoreGive(g_logs_storage.log_mutex);
        return false;
    }

    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = STORAGE_BASE_PATH,
            .config = {
                .format_if_mount_failed = true,
                .max_files = (size_t)cfg.max_files_open,
                .allocation_unit_size = 4096,
            },
        },
        .medium.wl_handle = wl_handle,
    };

    err = tinyusb_msc_new_storage_spiflash(&storage_cfg, &g_logs_storage.msc_storage);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_spiflash failed: %s", esp_err_to_name(err));
        xSemaphoreGive(g_logs_storage.log_mutex);
        return false;
    }
    ESP_LOGI(TAG, "MSC storage created successfully");

    size_t total = 0;
    size_t free_bytes = 0;
    if (get_fat_free_bytes(STORAGE_BASE_PATH, &total, &free_bytes)) {
        size_t used = total - free_bytes;
        ESP_LOGI(TAG, "FAT mounted, total: %d KB, used: %d KB, free: %d KB", (int)(total / 1024), (int)(used / 1024), (int)(free_bytes / 1024));
    } else {
        ESP_LOGE(TAG, "Failed to get FAT free space");
    }

    if (!rotate_log_file_unlocked()) {
        ESP_LOGE(TAG, "Failed to create initial log file");
        xSemaphoreGive(g_logs_storage.log_mutex);
        return false;
    }

    xSemaphoreGive(g_logs_storage.log_mutex);
    return true;
}

/* 反初始化：刷新并关闭当前日志文件。
 * 不卸载 FAT 或销毁 MSC 存储——切换到 USB 模式由 logs_storage_usb_msc_init() 完成。 */
void logs_storage_backend_deinit(void) {
    if (xSemaphoreTake(g_logs_storage.log_mutex, portMAX_DELAY) == pdTRUE) {
        if (g_logs_storage.current_log_file) {
            fflush(g_logs_storage.current_log_file);
            fclose(g_logs_storage.current_log_file);
            g_logs_storage.current_log_file = NULL;
        }
        /* 不卸载 FAT 或删除 MSC 存储，仅关闭文件。
         * 切换到 USB 由 logs_storage_usb_msc_init() 完成。 */
        xSemaphoreGive(g_logs_storage.log_mutex);
    }
}

/* 将单条日志写入磁盘：必要时先轮转，再追加 "[+ts ms] message\n"。
 * 内部获取 log_mutex；线程安全。 */
bool logs_storage_write_line(const char *message, int64_t timestamp) {
    if (xSemaphoreTake(g_logs_storage.log_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (!g_logs_storage.current_log_file && !rotate_log_file_unlocked()) {
        xSemaphoreGive(g_logs_storage.log_mutex);
        return false;
    }

    logs_storage_rotation_config_t cfg;
    logs_storage_rotation_config_get(&cfg);

    if (!ensure_free_space_unlocked(cfg.min_free_space_bytes)) {
        ESP_LOGW(TAG, "Insufficient space, dropping log entry");
        xSemaphoreGive(g_logs_storage.log_mutex);
        return false;
    }

    long cur_pos = ftell(g_logs_storage.current_log_file);
    if (cur_pos > (long)cfg.max_file_size_bytes) {
        ESP_LOGI(TAG, "Log file size %ld bytes, rotating", cur_pos);
        if (!rotate_log_file_unlocked()) {
            ESP_LOGE(TAG, "Rotation failed, dropping log");
            xSemaphoreGive(g_logs_storage.log_mutex);
            return false;
        }
    }

    fprintf(g_logs_storage.current_log_file, "[+%lld ms] %s\n", (long long)timestamp, message);
    fflush(g_logs_storage.current_log_file);
    xSemaphoreGive(g_logs_storage.log_mutex);
    return true;
}

/* 启动时枚举并打印所有现存日志文件（按编号升序），便于调试定位。
 * 同时汇总：逻辑大小之和、按簇对齐的磁盘占用、与 FAT 已用空间对比。 */
void logs_storage_list_existing(void) {
    if (xSemaphoreTake(g_logs_storage.log_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    char **files = NULL;
    int file_count = 0;
    if (get_sorted_log_files_unlocked(&files, &file_count) != ESP_OK || file_count == 0) {
        ESP_LOGI(TAG, "No log files found");
        if (files) free(files);
        xSemaphoreGive(g_logs_storage.log_mutex);
        return;
    }

    /* 获取 FAT 簇大小和已用空间，用于对比 */
    size_t fat_total = 0, fat_free = 0;
    size_t cluster_size = 0;
    bool fat_ok = false;
    FATFS *fs = NULL;
    DWORD free_clusters;
    if (f_getfree(STORAGE_BASE_PATH, &free_clusters, &fs) == FR_OK) {
        cluster_size = (size_t)fs->csize * (size_t)fs->ssize;
        fat_total = (fs->n_fatent - 2) * cluster_size;
        fat_free = (size_t)free_clusters * cluster_size;
        fat_ok = true;
    }

    size_t total_logical = 0;   /* 所有文件逻辑大小之和（stat st_size） */
    size_t total_physical = 0;  /* 所有文件按簇对齐的实际磁盘占用 */

    ESP_LOGI(TAG, "========== Existing log files (sorted) ==========");
    for (int i = 0; i < file_count; i++) {
        const char *path = files[i];
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        struct stat st;
        if (stat(path, &st) == 0) {
            total_logical += (size_t)st.st_size;
            if (cluster_size > 0) {
                size_t phy = ((size_t)st.st_size + cluster_size - 1) / cluster_size * cluster_size;
                total_physical += phy;
                ESP_LOGI(TAG, "  /storage/%s  %8ld B  (phy %6zu B = %3zu cl)",
                         name, (long)st.st_size, phy, phy / cluster_size);
            } else {
                ESP_LOGI(TAG, "  /storage/%s  %8ld B", name, (long)st.st_size);
            }
        } else {
            ESP_LOGI(TAG, "  /storage/%s  (size unknown)", name);
        }
        free(files[i]);
    }
    free(files);

    if (fat_ok && cluster_size > 0) {
        size_t fat_used = fat_total - fat_free;
        size_t meta_overhead = fat_used > total_physical ? fat_used - total_physical : 0;
        ESP_LOGI(TAG, "---- summary ----");
        ESP_LOGI(TAG, "  files: %d", file_count);
        ESP_LOGI(TAG, "  logical size:   %6zu B (%4zu KB)", total_logical, total_logical / 1024);
        ESP_LOGI(TAG, "  physical (cl):  %6zu B (%4zu KB, %zu cl @ %zu B)",
                 total_physical, total_physical / 1024, total_physical / cluster_size, cluster_size);
        ESP_LOGI(TAG, "  FAT data total: %6zu B (%4zu KB)", fat_total, fat_total / 1024);
        ESP_LOGI(TAG, "  FAT data used:  %6zu B (%4zu KB)", fat_used, fat_used / 1024);
        ESP_LOGI(TAG, "  FAT data free:  %6zu B (%4zu KB)", fat_free, fat_free / 1024);
        ESP_LOGI(TAG, "  slack (meta):   %6zu B (%4zu KB)  [FAT表+目录+未统计文件]",
                 meta_overhead, meta_overhead / 1024);
    } else {
        ESP_LOGI(TAG, "  total logical:  %zu bytes", total_logical);
    }
    ESP_LOGI(TAG, "================================================");
    xSemaphoreGive(g_logs_storage.log_mutex);
}

/* 格式化存储分区（重建 FAT 文件系统）。
 * 必须在 logs_storage_init() 之后调用；会清除所有现有日志。 */
esp_err_t logs_storage_backend_format(void) {
    if (g_logs_storage.msc_storage == NULL) {
        ESP_LOGE(TAG, "Storage not initialized, call logs_storage_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    return tinyusb_msc_format_storage(g_logs_storage.msc_storage);
}
