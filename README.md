# esp-idf-logs-spiffs

[![Component](https://img.shields.io/badge/ESP--IDF-6.0%2B-blue)](https://docs.espressif.com/projects/esp-idf/)
[![License](https://img.shields.io/badge/license-Apache--2.0-green)](LICENSE)
[![Target](https://img.shields.io/badge/target-ESP32--S3-orange)](https://www.espressif.com/en/products/socs/esp32-s3)

一个面向 ESP-IDF 的 SPIFFS 日志组件，提供自动轮转、空间控制和线程安全写入能力，适合在设备端把运行日志落盘到 SPIFFS 分区。

## 组件特性

- 自动创建并轮转日志文件
- 超过大小时自动切换文件
- 低空间时自动清理旧日志
- 通过队列和后台工作线程串行化写入，避免多线程同时写入 SPIFFS 文件造成竞争
- 代码按模块拆分，便于维护：存储层、工作线程层、公共接口层
- 可直接作为 ESP-IDF 组件被其他项目引用

## 在项目中使用

在你的应用项目的 idf_component.yml 中添加依赖：

```yaml
dependencies:
  nczy520/esp-idf-logs-spiffs:
    git: https://github.com/nczy520/esp-idf-logs-spiffs.git
    version: "*"
```

然后在应用代码中调用：

```c
#include "logs_spiffs.h"
#include "logs_spiffs_rotation.h"

void app_main(void) {
    logs_spiffs_rotation_config_t cfg;
    logs_spiffs_rotation_config_default(&cfg);
    cfg.max_file_size_bytes = 256u * 1024u;
    cfg.max_log_files = 10u;
    cfg.min_free_space_bytes = 128u * 1024u;
    cfg.rotate_threshold_bytes = 32u * 1024u;
    logs_spiffs_rotation_config_set(&cfg);

    if (!logs_spiffs_init()) {
        return;
    }

    logs_spiffs_set_level(LOGS_SPIFFS_LEVEL_WARN);
    logs_spiffs_write("This is an INFO log, it will be filtered out");
    logs_spiffs_write_level(LOGS_SPIFFS_LEVEL_WARN, "This warning is persisted");
    logs_spiffs_write_level(LOGS_SPIFFS_LEVEL_ERROR, "This error is persisted");
}
```

### 说明

- `logs_spiffs_set_level()` 用来设置最低记录级别，低于该级别的消息会被直接丢弃。
- `logs_spiffs_write()` 会写入默认级别的 INFO 日志。
- `logs_spiffs_write_level()` 可显式指定 `INFO/WARN/ERROR`。
- `logs_spiffs_rotation_config_*()` 可以调整轮转阈值、最大文件大小、保留文件数和最低剩余空间。

## 示例工程

仓库中已提供一个最小示例，目录为 [examples/basic](examples/basic)。

在 ESP-IDF 环境中运行：

```bash
cd examples/basic
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

示例会演示：

- 设置轮转配置
- 初始化 SPIFFS 日志组件
- 使用不同级别的日志写入
- 观察日志文件被生成和轮转

## API

- logs_spiffs_init()
- logs_spiffs_set_level()
- logs_spiffs_write()
- logs_spiffs_write_level()
- logs_spiffs_deinit()
- logs_spiffs_format()
- logs_spiffs_rotation_config_default()
- logs_spiffs_rotation_config_set()
- logs_spiffs_rotation_config_get()

## 许可证

GPL-3.0-or-later
