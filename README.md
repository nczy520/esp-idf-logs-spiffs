# esp-idf-logs-spiffs

[![Component](https://img.shields.io/badge/ESP--IDF-6.0%2B-blue)](https://docs.espressif.com/projects/esp-idf/)
[![License](https://img.shields.io/badge/license-Apache--2.0-green)](LICENSE)
[![Target](https://img.shields.io/badge/target-ESP32--S3-orange)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Version](https://img.shields.io/badge/version-1.1.2-blue)](https://github.com/nczy520/esp-idf-logs-spiffs)

一个面向 ESP-IDF 的 SPIFFS 日志组件，提供自动轮转、空间控制和线程安全写入能力，适合在设备端把运行日志落盘到 SPIFFS 分区。

## 项目结构

```
esp-idf-logs-spiffs/
├── esp_logs_spiffs/          # 组件源码（ESP-IDF 组件）
│   ├── include/
│   ├── src/
│   ├── CMakeLists.txt
│   └── idf_component.yml
├── examples/
│   └── basic/                # 示例工程
│       ├── main/
│       ├── CMakeLists.txt
│       └── idf_component.yml
├── LICENSE
└── README.md
```

## 组件特性

- 自动创建并轮转日志文件
- 超过大小时自动切换文件
- 低空间时自动清理旧日志
- 通过队列和后台工作线程串行化写入，避免多线程同时写入 SPIFFS 文件造成竞争
- 批量写入（最多 8 条或 200ms 触发一次 flush），降低 SPIFFS 写放大开销
- 日志级别过滤（INFO / WARN / ERROR）
- 代码按模块拆分，便于维护：存储层、工作线程层、公共接口层
- 可直接作为 ESP-IDF 组件被其他项目引用

## 在项目中使用

在你的应用项目的 `idf_component.yml` 中添加依赖：

```yaml
dependencies:
  esp_logs_spiffs:
    git: https://github.com/nczy520/esp-idf-logs-spiffs.git
    path: esp_logs_spiffs
    version: main
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

- `logs_spiffs_set_level()` 用来设置最低记录级别，低于该级别的消息会被直接丢弃。该接口是线程安全的（原子读写）。
- `logs_spiffs_write()` 会写入默认级别的 INFO 日志。
- `logs_spiffs_write_level()` 可显式指定 `INFO/WARN/ERROR`。
- `logs_spiffs_rotation_config_*()` 可以调整轮转阈值、最大文件大小、保留文件数和最低剩余空间。
- `logs_spiffs_format()` 会擦除整个 SPIFFS 分区并在成功后调用 `esp_restart()` 重启系统，**成功时不会返回**，调用方无需在成功路径上做后续处理。

### 控制台日志输出

组件内部所有状态信息通过 `ESP_LOGI/ESP_LOGW/ESP_LOGE` 输出，TAG 为 `LOG_MGR`。落盘日志的同步回显默认关闭（使用 `ESP_LOGD`），如需在串口实时查看写入的日志内容，可启用：

```c
#include "esp_log.h"
esp_log_level_set("LOG_MGR", ESP_LOG_DEBUG);
```

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

| 函数 | 说明 |
|------|------|
| `logs_spiffs_init()` | 初始化：挂载 SPIFFS，启动 worker，创建初始日志文件 |
| `logs_spiffs_deinit()` | 反初始化：停止 worker，关闭文件，卸载 SPIFFS |
| `logs_spiffs_set_level()` | 设置最低日志级别 |
| `logs_spiffs_write()` | 写入一条 INFO 级别日志 |
| `logs_spiffs_write_level()` | 写入一条指定级别日志 |
| `logs_spiffs_format()` | 擦除 SPIFFS 分区并重启（成功不返回） |
| `logs_spiffs_rotation_config_default()` | 获取默认轮转配置 |
| `logs_spiffs_rotation_config_set()` | 设置当前轮转配置 |
| `logs_spiffs_rotation_config_get()` | 读取当前轮转配置 |

### 默认轮转配置

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `min_free_space_bytes` | 256 KB | 触发清理前的最低剩余空间 |
| `max_file_size_bytes` | 512 KB | 单个日志文件最大尺寸 |
| `max_log_files` | 99 | 保留的日志文件数量上限 |
| `rotate_threshold_bytes` | 8 KB | 创建新文件前需保证的最小可用空间 |
| `max_files_open` | 5 | SPIFFS 允许同时打开的文件数量 |

## 许可证

Apache-2.0
