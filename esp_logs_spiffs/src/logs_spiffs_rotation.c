#include "logs_spiffs_rotation.h"

static logs_spiffs_rotation_config_t s_rotation_config = {
    .min_free_space_bytes = 256u * 1024u,
    .max_file_size_bytes = 512u * 1024u,
    .max_log_files = 99u,
    .rotate_threshold_bytes = 8u * 1024u,
    .max_files_open = 5u
};

void logs_spiffs_rotation_config_default(logs_spiffs_rotation_config_t *cfg) {
    if (cfg != NULL) {
        *cfg = s_rotation_config;
    }
}

void logs_spiffs_rotation_config_set(const logs_spiffs_rotation_config_t *cfg) {
    if (cfg != NULL) {
        s_rotation_config = *cfg;
    }
}

void logs_spiffs_rotation_config_get(logs_spiffs_rotation_config_t *cfg) {
    if (cfg != NULL) {
        *cfg = s_rotation_config;
    }
}
