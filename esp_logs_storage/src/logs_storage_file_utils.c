/**
 * @file logs_storage_file_utils.c
 * @brief 日志文件工具：文件名解析、枚举、排序、清理、编号、FAT 空间查询
 *
 * 职责：
 * - 解析 / 生成日志文件名（log_000001.log 形式）
 * - 枚举并按编号排序现存日志文件
 * - 删除最旧日志以释放空间或限制文件数量
 * - 查询 FAT 总容量与剩余空间
 * - 计算下一个可用日志编号（含 999999 回绕）
 *
 * 所有 *_unlocked 函数要求调用者先持有 g_logs_storage.log_mutex。
 */

#include "logs_storage_internal.h"
#include "logs_storage_rotation.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "ff.h"

static const char *TAG = "LOG_MGR";

/* ===================== FAT 空间查询 ===================== */

/* 获取 FAT 分区的总容量和剩余空间（字节）。
 * 使用 fs->ssize 而非固定的 FF_MIN_SS，确保 4KB 扇区下容量计算正确。
 * 注意：这里返回的 total 是 FAT 数据区总大小（簇总数 × 簇大小），
 * 不包含 FAT 表、保留扇区、根目录等元数据占用的空间，
 * 因此会小于 flash 分区的物理大小。 */
bool get_fat_free_bytes(const char *path, size_t *out_total, size_t *out_free) {
    FATFS *fs;
    DWORD free_clusters;
    FRESULT res = f_getfree(path, &free_clusters, &fs);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "f_getfree failed: %d", res);
        return false;
    }
    size_t total_clusters = fs->n_fatent - 2;
    size_t cluster_size = (size_t)fs->csize * (size_t)fs->ssize;
    *out_total = total_clusters * cluster_size;
    *out_free = (size_t)free_clusters * cluster_size;

    /* 打印 FAT 详细参数，便于排查容量计算问题 */
    ESP_LOGD(TAG, "FAT params: ssize=%d, csize=%d, n_fatent=%lu, free_clusters=%lu, "
             "cluster_size=%d, total_data=%d KB, free_data=%d KB",
             (int)fs->ssize, (int)fs->csize, (unsigned long)fs->n_fatent,
             (unsigned long)free_clusters, (int)cluster_size,
             (int)(*out_total / 1024), (int)(*out_free / 1024));
    return true;
}

/* ===================== 日志文件名解析与排序 ===================== */

/* 从文件名中提取编号：log_000001.log → 1，非日志文件返回 -1 */
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

/* qsort 比较函数：按日志文件编号升序排列 */
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

/* 获取所有日志文件路径，按编号升序排列。
 * 返回动态分配的字符串数组，调用者需逐个 free 并 free 数组本身。 */
esp_err_t get_sorted_log_files_unlocked(char ***out_paths, int *out_count) {
    DIR *dir = opendir(STORAGE_BASE_PATH);
    if (!dir) {
        *out_paths = NULL;
        *out_count = 0;
        return ESP_ERR_NOT_FOUND;
    }

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
            }
        }
    }
    closedir(dir);

    if (count == 0) {
        free(files);
        *out_paths = NULL;
        *out_count = 0;
        return ESP_OK;
    }

    qsort(files, count, sizeof(char *), compare_log_number);
    *out_paths = files;
    *out_count = count;
    return ESP_OK;
}

/* 删除最旧的日志文件，直到保留 keep_count 个或释放 need_free_bytes 字节。
 * 返回实际删除的文件数。 */
int delete_oldest_logs_unlocked(int keep_count, size_t need_free_bytes) {
    char **files = NULL;
    int file_count = 0;
    if (get_sorted_log_files_unlocked(&files, &file_count) != ESP_OK || file_count == 0) {
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

/* 确保磁盘剩余空间不少于 required_bytes 字节。
 * 若不足则循环删除最旧的日志文件，直到空间够用或无法继续清理。
 * 调用者必须先持有 log_mutex。 */
bool ensure_free_space_unlocked(size_t required_bytes) {
    logs_storage_rotation_config_t cfg;
    logs_storage_rotation_config_get(&cfg);

    size_t total = 0;
    size_t free_bytes = 0;
    if (!get_fat_free_bytes(STORAGE_BASE_PATH, &total, &free_bytes)) {
        return false;
    }

    char **files = NULL;
    int file_count = 0;
    esp_err_t err = get_sorted_log_files_unlocked(&files, &file_count);
    if (err != ESP_OK || file_count == 0) {
        if (files) free(files);
        if (free_bytes >= required_bytes) {
            return true;
        }
        ESP_LOGW(TAG, "No existing log files, but not enough free space: %d bytes, need %d bytes", (int)free_bytes, (int)required_bytes);
        return false;
    }
    for (int i = 0; i < file_count; i++) free(files[i]);
    free(files);

    if (free_bytes >= required_bytes) return true;

    ESP_LOGW(TAG, "Low free space: %d bytes, need %d bytes", (int)free_bytes, (int)required_bytes);

    size_t need_to_free = required_bytes - free_bytes;
    int prev_deleted = -1;
    int total_deleted = 0;
    while (free_bytes < required_bytes) {
        int deleted = delete_oldest_logs_unlocked(0, need_to_free);
        total_deleted += deleted;
        if (deleted == 0) break;

        if (!get_fat_free_bytes(STORAGE_BASE_PATH, &total, &free_bytes)) break;
        need_to_free = required_bytes - free_bytes;
        if (need_to_free <= 0) break;

        if (deleted == prev_deleted) break;
        prev_deleted = deleted;
    }

    if (free_bytes >= required_bytes) return true;

    ESP_LOGE(TAG, "Insufficient space after cleanup: %d bytes, need %d bytes", (int)free_bytes, (int)required_bytes);
    return false;
}

/* 强制执行日志文件数量上限：删除多余的最旧文件，只保留 max_log_files 个。
 * 调用者必须先持有 log_mutex。 */
void enforce_max_file_count_unlocked(void) {
    logs_storage_rotation_config_t cfg;
    logs_storage_rotation_config_get(&cfg);
    delete_oldest_logs_unlocked((int)cfg.max_log_files, 0);
}

/* ===================== 文件编号与路径生成 ===================== */

/* 生成形如 "/storage/log_000001.log" 的日志文件路径。
 * out_path 缓冲区需至少 64 字节。 */
void generate_log_path(char *out_path, int number) {
    sprintf(out_path, "%s/%s%0*d%s", STORAGE_BASE_PATH, LOG_FILE_PREFIX,
            FILENAME_NUM_DIGITS, number, LOG_FILE_EXT);
}

/* 计算下一个可用的日志文件编号（max+1）。
 * 当编号达到上限 999999 时，回绕并探测最小的未占用编号。
 * 调用者必须先持有 log_mutex。 */
int get_next_log_number_unlocked(void) {
    char **files = NULL;
    int file_count = 0;
    int max_num = 0;
    if (get_sorted_log_files_unlocked(&files, &file_count) == ESP_OK) {
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

    /* Wraparound: find the smallest unused number by probing file existence.
     * enforce_max_file_count_unlocked() runs before this in the rotation path,
     * so free slots are guaranteed when max_log_files < 999999. */
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
