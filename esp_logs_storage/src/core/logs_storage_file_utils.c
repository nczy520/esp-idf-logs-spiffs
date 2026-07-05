/**
 * @file logs_storage_file_utils.c
 * @brief 日志文件工具：文件名解析、枚举、排序、清理、编号、空间查询
 *
 * 所有函数由 worker 任务独占调用，不需要任何锁保护。
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */

#include "logs_storage_internal.h"
#include "logs_storage_rotation.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "LOG_MGR";

/* ===================== SPIFFS 空间查询 ===================== */

bool get_storage_free_bytes(const char *path, size_t *out_total, size_t *out_free) {
    (void)path;
    size_t total = 0, used = 0;
    esp_err_t err = esp_spiffs_info("storage", &total, &used);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_spiffs_info failed: %s", esp_err_to_name(err));
        return false;
    }
    *out_total = total;
    *out_free = total - used;
    return true;
}

/* ===================== 日志文件名解析与排序 ===================== */

static int extract_log_number(const char *filename) {
    if (!filename || strncmp(filename, LOG_FILE_PREFIX, strlen(LOG_FILE_PREFIX)) != 0) {
        return -1;
    }
    const char *num_start = filename + strlen(LOG_FILE_PREFIX);
    char *endptr;
    long num = strtol(num_start, &endptr, 10);
    if (endptr == num_start || strcmp(endptr, LOG_FILE_EXT) != 0) {
        return -1;
    }
    return (int)num;
}

static int compare_log_number(const void *a, const void *b) {
    const char *path_a = *(const char **)a;
    const char *path_b = *(const char **)b;
    const char *name_a = strrchr(path_a, '/');
    const char *name_b = strrchr(path_b, '/');
    name_a = name_a ? name_a + 1 : path_a;
    name_b = name_b ? name_b + 1 : path_b;
    int num_a = extract_log_number(name_a);
    int num_b = extract_log_number(name_b);
    if (num_a < 0 || num_b < 0) {
        return strcmp(name_a, name_b);
    }
    return num_a - num_b;
}

/* ===================== 日志文件枚举与清理 ===================== */

esp_err_t get_sorted_log_files(char ***out_paths, int *out_count) {
    ESP_LOGI(TAG, "=== get_sorted_log_files: 开始遍历目录 %s ===", STORAGE_BASE_PATH);
    DIR *dir = opendir(STORAGE_BASE_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "=== get_sorted_log_files: 无法打开目录 %s (errno=%d) ===", STORAGE_BASE_PATH, errno);
        *out_paths = NULL;
        *out_count = 0;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "=== get_sorted_log_files: 成功打开目录 ===");

    char **files = NULL;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            const char *name = entry->d_name;
            if (extract_log_number(name) >= 0) {
                char *full_path = malloc(strlen(STORAGE_BASE_PATH) + strlen(name) + 2);
                if (!full_path) {
                    for (int i = 0; i < count; i++) free(files[i]);
                    free(files);
                    closedir(dir);
                    *out_paths = NULL;
                    *out_count = 0;
                    return ESP_ERR_NO_MEM;
                }
                sprintf(full_path, "%s/%s", STORAGE_BASE_PATH, name);

                char **tmp = realloc(files, (count + 1) * sizeof(char *));
                if (!tmp) {
                    free(full_path);
                    for (int i = 0; i < count; i++) free(files[i]);
                    free(files);
                    closedir(dir);
                    *out_paths = NULL;
                    *out_count = 0;
                    return ESP_ERR_NO_MEM;
                }
                files = tmp;
                files[count++] = full_path;
                ESP_LOGD(TAG, "找到日志文件: %s (全路径: %s)", name, full_path);
            }
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGW(TAG, "=== get_sorted_log_files: 目录中没有找到日志文件 ===");
        free(files);
        *out_paths = NULL;
        *out_count = 0;
        return ESP_OK;
    }

    qsort(files, count, sizeof(char *), compare_log_number);
    ESP_LOGI(TAG, "=== get_sorted_log_files: 成功排序 %d 个文件 ===", count);
    *out_paths = files;
    *out_count = count;
    return ESP_OK;
}

int delete_oldest_logs(int keep_count, size_t need_free_bytes) {
    char **files = NULL;
    int file_count = 0;
    if (get_sorted_log_files(&files, &file_count) != ESP_OK || file_count == 0) {
        if (files) free(files);
        return 0;
    }

    int deleted = 0;
    size_t freed_bytes = 0;
    int files_to_delete = file_count - keep_count;
    if (files_to_delete <= 0) {
        for (int i = 0; i < file_count; i++) free(files[i]);
        free(files);
        return 0;
    }

    for (int i = 0; i < file_count; i++) {
        if ((file_count - deleted) <= keep_count && freed_bytes >= need_free_bytes) {
            break;
        }
        if (deleted >= files_to_delete && freed_bytes >= need_free_bytes) {
            break;
        }

        struct stat st;
        size_t file_size = 0;
        if (stat(files[i], &st) == 0) {
            file_size = st.st_size;
        }
        if (remove(files[i]) == 0) {
            freed_bytes += file_size;
            deleted++;
            ESP_LOGI(TAG, "Deleted old log: %s (%ld bytes)", files[i], (long)file_size);
        } else {
            ESP_LOGW(TAG, "Failed to delete %s", files[i]);
        }
        free(files[i]);
    }

    for (int i = deleted; i < file_count; i++) {
        free(files[i]);
    }
    free(files);
    return deleted;
}

bool ensure_free_space(size_t required_bytes) {
    logs_storage_rotation_config_t cfg;
    logs_storage_rotation_config_get(&cfg);

    size_t total = 0;
    size_t free_bytes = 0;
    if (!get_storage_free_bytes(STORAGE_BASE_PATH, &total, &free_bytes)) {
        return false;
    }

    char **files = NULL;
    int file_count = 0;
    esp_err_t err = get_sorted_log_files(&files, &file_count);
    if (err != ESP_OK || file_count == 0) {
        if (files) free(files);
        if (free_bytes >= required_bytes) {
            return true;
        }
        ESP_LOGW(TAG, "No existing log files, but not enough free space: %d bytes, need %d bytes",
                 (int)free_bytes, (int)required_bytes);
        return false;
    }
    for (int i = 0; i < file_count; i++) free(files[i]);
    free(files);

    if (free_bytes >= required_bytes) return true;

    ESP_LOGW(TAG, "Low free space: %d bytes, need %d bytes", (int)free_bytes, (int)required_bytes);

    size_t need_to_free = required_bytes - free_bytes;
    int prev_deleted = -1;
    while (free_bytes < required_bytes) {
        int deleted = delete_oldest_logs(0, need_to_free);
        if (deleted == 0) break;

        if (!get_storage_free_bytes(STORAGE_BASE_PATH, &total, &free_bytes)) break;
        need_to_free = required_bytes - free_bytes;
        if (need_to_free <= 0) break;

        if (deleted == prev_deleted) break;
        prev_deleted = deleted;
    }

    if (free_bytes >= required_bytes) return true;

    ESP_LOGE(TAG, "Insufficient space after cleanup: %d bytes, need %d bytes",
             (int)free_bytes, (int)required_bytes);
    return false;
}

void enforce_max_file_count(void) {
    logs_storage_rotation_config_t cfg;
    logs_storage_rotation_config_get(&cfg);
    delete_oldest_logs((int)cfg.max_log_files, 0);
}

/* ===================== 文件编号与路径生成 ===================== */

void generate_log_path(char *out_path, int number) {
    sprintf(out_path, "%s/%s%0*d%s", STORAGE_BASE_PATH, LOG_FILE_PREFIX,
            FILENAME_NUM_DIGITS, number, LOG_FILE_EXT);
}

int get_next_log_number(void) {
    char **files = NULL;
    int file_count = 0;
    int max_num = 0;
    if (get_sorted_log_files(&files, &file_count) == ESP_OK) {
        for (int i = 0; i < file_count; i++) {
            const char *name = strrchr(files[i], '/');
            name = name ? name + 1 : files[i];
            int num = extract_log_number(name);
            if (num > max_num) max_num = num;
            free(files[i]);
        }
        free(files);
    }

    if (max_num < 999999) {
        return max_num + 1;
    }

    for (int candidate = 1; candidate <= 999999; ++candidate) {
        char path[64];
        generate_log_path(path, candidate);
        struct stat st;
        if (stat(path, &st) != 0) {
            return candidate;
        }
    }
    ESP_LOGW(TAG, "All log number slots in use, reusing 1");
    return 1;
}
