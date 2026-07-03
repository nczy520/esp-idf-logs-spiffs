# esp-idf-logs-spiffs

一个面向 ESP-IDF 的 SPIFFS 日志组件，提供自动轮转、空间控制和线程安全写入能力，适合在设备端把运行日志落盘到 SPIFFS 分区。

## 组件特性

- 自动创建并轮转日志文件
- 超过大小时自动切换文件
- 低空间时自动清理旧日志
- 线程安全写入
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

void app_main(void) {
    if (!logs_spiffs_init()) {
        return;
    }

    logs_spiffs_write("Hello from ESP-IDF component");
}
```

## API

- logs_spiffs_init()
- logs_spiffs_write()
- logs_spiffs_deinit()
- logs_spiffs_format()

## 许可证

GPL-3.0-or-later
