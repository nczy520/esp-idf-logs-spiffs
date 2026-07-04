#include "logs_spiffs_internal.h"
#include "logs_spiffs_rotation.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "LOG_MGR";

#define SPIFFS_BASE_PATH      "/spiffs"
#define LOG_FILE_PREFIX       "log_"
#define LOG_FILE_EXT          ".log"
#define FILENAME_NUM_DIGITS   6

static bool rotate_log_file_unlocked(void);
static bool ensure_free_space_unlocked(size_t required_bytes);
static void enforce_max_file_count_unlocked(void);
static int delete_oldest_logs_unlocked(int keep_count, size_t need_free_bytes);
static int get_next_log_number_unlocked(void);
static void generate_log_path(char *out_path, int number);
static int extract_log_number(const char *filename);
static int compare_log_number(const void *a, const void *b);
static esp_err_t get_sorted_log_files_unlocked(char ***out_paths, int *out_count);

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

static esp_err_t get_sorted_log_files_unlocked(char ***out_paths, int *out_count) {
    DIR *dir = opendir(SPIFFS_BASE_PATH);
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
                char *full_path = malloc(strlen(SPIFFS_BASE_PATH) + strlen(name) + 2);
                if (!full_path) {
                    for (int i = 0; i < count; i++) free(files[i]);
                    free(files);
                    closedir(dir);
                    *out_paths = NULL;
                    *out_count = 0;
                    return ESP_ERR_NO_MEM;
                }
                sprintf(full_path, "%s/%s", SPIFFS_BASE_PATH, name);

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

static int delete_oldest_logs_unlocked(int keep_count, size_t need_free_bytes) {
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

static bool ensure_free_space_unlocked(size_t required_bytes) {
    logs_spiffs_rotation_config_t cfg;
    logs_spiffs_rotation_config_get(&cfg);

    size_t total = 0, used = 0;
    if (esp_spiffs_info(NULL, &total, &used) != ESP_OK) return false;
    size_t free_bytes = total - used;
    if (free_bytes >= required_bytes) return true;

    size_t need_to_free = required_bytes - free_bytes;
    int prev_deleted = -1;
    while (free_bytes < required_bytes) {
        /* delete_oldest_logs_unlocked already handles the empty/keep cases:
         * it returns 0 when there is nothing to delete, which breaks below. */
        int deleted = delete_oldest_logs_unlocked((int)cfg.max_log_files, need_to_free);
        if (deleted == 0) break;

        if (esp_spiffs_info(NULL, &total, &used) != ESP_OK) break;
        free_bytes = total - used;
        need_to_free = required_bytes - free_bytes;
        if (need_to_free <= 0) break;

        if (deleted == prev_deleted) break;
        prev_deleted = deleted;
    }
    return free_bytes >= required_bytes;
}

static void enforce_max_file_count_unlocked(void) {
    logs_spiffs_rotation_config_t cfg;
    logs_spiffs_rotation_config_get(&cfg);
    delete_oldest_logs_unlocked((int)cfg.max_log_files, 0);
}

static int get_next_log_number_unlocked(void) {
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

static void generate_log_path(char *out_path, int number) {
    sprintf(out_path, "%s/%s%0*d%s", SPIFFS_BASE_PATH, LOG_FILE_PREFIX,
            FILENAME_NUM_DIGITS, number, LOG_FILE_EXT);
}

static bool rotate_log_file_unlocked(void) {
    logs_spiffs_rotation_config_t cfg;
    logs_spiffs_rotation_config_get(&cfg);

    if (g_logs_spiffs.current_log_file) {
        fclose(g_logs_spiffs.current_log_file);
        g_logs_spiffs.current_log_file = NULL;
    }

    if (!ensure_free_space_unlocked(cfg.rotate_threshold_bytes)) {
        ESP_LOGE(TAG, "Cannot create new log file, insufficient space");
        return false;
    }

    enforce_max_file_count_unlocked();

    int next_num = get_next_log_number_unlocked();
    generate_log_path(g_logs_spiffs.current_log_path, next_num);
    g_logs_spiffs.current_log_file = fopen(g_logs_spiffs.current_log_path, "a");
    if (!g_logs_spiffs.current_log_file) {
        ESP_LOGE(TAG, "Failed to create log file: %s", g_logs_spiffs.current_log_path);
        return false;
    }

    setvbuf(g_logs_spiffs.current_log_file, NULL, _IOLBF, 256);
    ESP_LOGI(TAG, "New log file: %s (number %d)", g_logs_spiffs.current_log_path, next_num);
    return true;
}

bool logs_spiffs_storage_init(void) {
    if (xSemaphoreTake(g_logs_spiffs.log_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    logs_spiffs_rotation_config_t cfg;
    logs_spiffs_rotation_config_get(&cfg);

    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = NULL,
        .max_files = (size_t)cfg.max_files_open,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        xSemaphoreGive(g_logs_spiffs.log_mutex);
        return false;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted, total: %d KB, used: %d KB", (int)(total / 1024), (int)(used / 1024));
    }

    if (!rotate_log_file_unlocked()) {
        ESP_LOGE(TAG, "Failed to create initial log file");
        esp_vfs_spiffs_unregister(NULL);
        xSemaphoreGive(g_logs_spiffs.log_mutex);
        return false;
    }

    xSemaphoreGive(g_logs_spiffs.log_mutex);
    return true;
}

void logs_spiffs_storage_deinit(void) {
    if (xSemaphoreTake(g_logs_spiffs.log_mutex, portMAX_DELAY) == pdTRUE) {
        if (g_logs_spiffs.current_log_file) {
            fflush(g_logs_spiffs.current_log_file);
            fclose(g_logs_spiffs.current_log_file);
            g_logs_spiffs.current_log_file = NULL;
        }
        esp_vfs_spiffs_unregister(NULL);
        xSemaphoreGive(g_logs_spiffs.log_mutex);
    }
}

bool logs_spiffs_storage_write_line(const char *message, int64_t timestamp) {
    if (xSemaphoreTake(g_logs_spiffs.log_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (!g_logs_spiffs.current_log_file && !rotate_log_file_unlocked()) {
        xSemaphoreGive(g_logs_spiffs.log_mutex);
        return false;
    }

    logs_spiffs_rotation_config_t cfg;
    logs_spiffs_rotation_config_get(&cfg);

    if (!ensure_free_space_unlocked(cfg.min_free_space_bytes)) {
        ESP_LOGW(TAG, "Insufficient space, dropping log entry");
        xSemaphoreGive(g_logs_spiffs.log_mutex);
        return false;
    }

    long cur_pos = ftell(g_logs_spiffs.current_log_file);
    if (cur_pos > (long)cfg.max_file_size_bytes) {
        ESP_LOGI(TAG, "Log file size %ld bytes, rotating", cur_pos);
        if (!rotate_log_file_unlocked()) {
            ESP_LOGE(TAG, "Rotation failed, dropping log");
            xSemaphoreGive(g_logs_spiffs.log_mutex);
            return false;
        }
    }

    fprintf(g_logs_spiffs.current_log_file, "[+%lld ms] %s\n", (long long)timestamp, message);
    fflush(g_logs_spiffs.current_log_file);
    xSemaphoreGive(g_logs_spiffs.log_mutex);
    return true;
}

void logs_spiffs_storage_list_existing(void) {
    char **files = NULL;
    int file_count = 0;
    if (get_sorted_log_files_unlocked(&files, &file_count) != ESP_OK || file_count == 0) {
        ESP_LOGI(TAG, "No log files found");
        if (files) free(files);
        return;
    }

    ESP_LOGI(TAG, "========== Existing log files (sorted) ==========");
    for (int i = 0; i < file_count; i++) {
        const char *path = files[i];
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        struct stat st;
        if (stat(path, &st) == 0) {
            ESP_LOGI(TAG, "  /spiffs/%s  (%ld bytes)", name, (long)st.st_size);
        } else {
            ESP_LOGI(TAG, "  /spiffs/%s  (size unknown)", name);
        }
        free(files[i]);
    }
    free(files);
    ESP_LOGI(TAG, "================================================");
}

esp_err_t logs_spiffs_storage_format(void) {
    return esp_spiffs_format(NULL);
}
