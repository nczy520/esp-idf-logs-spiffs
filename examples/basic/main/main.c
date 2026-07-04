#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logs_storage.h"
#include "logs_storage_rotation.h"

static const char *TAG = "logs_storage_example";

#define BOOT_BTN_GPIO       GPIO_NUM_0
#define BOOT_LONG_PRESS_MS   2000

/* 随机日志模块名 */
static const char *s_random_tags[] = {
    "SENSOR", "WIFI", "MQTT", "BUTTON", "TIMER",
    "DISPLAY", "STORAGE", "NETWORK", "ALARM", "CONFIG"
};
#define RANDOM_TAG_COUNT   (sizeof(s_random_tags) / sizeof(s_random_tags[0]))

/* 模拟的随机事件描述 */
static const char *s_random_events[] = {
    "temperature reading: %.1f C",
    "humidity: %d%%",
    "heartbeat OK, uptime %lu ms",
    "battery voltage: %.2f V",
    "packet sent, size %d bytes",
    "signal strength: -%d dBm",
    "user button pressed (id=%d)",
    "schedule task %s",
    "firmware version %d.%d.%d",
    "error code %d, retrying",
    "wifi reconnected after %d attempts",
    "memory free: %d bytes",
    "boot count: %d",
    "light intensity: %d lux",
    "motor speed: %d RPM"
};
#define RANDOM_EVENT_COUNT  (sizeof(s_random_events) / sizeof(s_random_events[0]))

/**
 * USB 模式下的 BOOT 键监控任务。
 *
 * 长按 BOOT 键 2 秒触发设备重启。
 *
 * 重启前必须先调用 logs_storage_force_exit_usb_mode() 停止 USB 弹出监控任务，
 * 否则 esp_restart() 过程中 USB PHY 关闭会导致 tud_mounted() 返回 false，
 * 被监控任务误判为"主机弹出"，触发 exit_usb_mode + 再次 esp_restart 的死循环。
 *
 * 重启后 app_main() 会在启动时检测 BOOT 键状态，若仍按下则等待释放，
 * 避免立即再次进入 USB 模式。
 */
static void boot_monitor_task(void *arg)
{
    (void)arg;
    /* 等待 USB 模式完全启动，避免误检测 */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "BOOT monitor started, long press 2s to restart");

    while (true) {
        if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
            /* 检测到按下，开始计时 */
            int held_ms = 0;
            while (gpio_get_level(BOOT_BTN_GPIO) == 0 && held_ms < BOOT_LONG_PRESS_MS) {
                vTaskDelay(pdMS_TO_TICKS(100));
                held_ms += 100;
            }

            if (held_ms >= BOOT_LONG_PRESS_MS) {
                ESP_LOGI(TAG, "BOOT long press %dms, restarting...", held_ms);
                /* 先停止 USB 弹出监控任务，避免复位过程中的死循环 */
                logs_storage_force_exit_usb_mode();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * 生成并写入一条随机日志，同时打印到控制台。
 * - 随机选择日志级别（INFO / WARN / ERROR，比例约 7:2:1）
 * - 随机选择模块名（SENSOR / WIFI / MQTT 等）
 * - 随机选择事件模板并填入随机数值
 * - 同时写入存储并通过 ESP_LOGx 输出到串口
 * 模拟真实设备的多样化日志输出。
 */
static void write_random_log(void)
{
    /* 随机选择级别（70% INFO, 20% WARN, 10% ERROR） */
    logs_storage_level_t level;
    int r = rand() % 100;
    if (r < 70) {
        level = LOGS_STORAGE_LEVEL_INFO;
    } else if (r < 90) {
        level = LOGS_STORAGE_LEVEL_WARN;
    } else {
        level = LOGS_STORAGE_LEVEL_ERROR;
    }

    /* 随机选择模块名 */
    const char *tag = s_random_tags[rand() % RANDOM_TAG_COUNT];

    /* 随机选择事件模板，先格式化到缓冲区，再同时写存储和打印 */
    char message[256];
    int ev = rand() % RANDOM_EVENT_COUNT;
    const char *fmt = s_random_events[ev];

    switch (ev) {
    case 0: /* temperature */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, 20.0f + (rand() % 150) / 10.0f);
        break;
    case 1: /* humidity */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 60 + 30);
        break;
    case 2: /* heartbeat uptime */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, (unsigned long)esp_timer_get_time() / 1000);
        break;
    case 3: /* battery voltage */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, 3.0f + (rand() % 70) / 100.0f);
        break;
    case 4: /* packet size */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 1024 + 64);
        break;
    case 5: /* signal strength */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 50 + 40);
        break;
    case 6: /* button id */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 8 + 1);
        break;
    case 7: /* schedule task name */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, (rand() % 2) ? "started" : "finished");
        break;
    case 8: /* firmware version */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 3, rand() % 10, rand() % 20);
        break;
    case 9: /* error code */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 500 + 100);
        break;
    case 10: /* wifi reconnect attempts */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 5 + 1);
        break;
    case 11: /* memory free */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 80000 + 100000);
        break;
    case 12: /* boot count */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 1000);
        break;
    case 13: /* light intensity */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 1000);
        break;
    case 14: /* motor speed */
        snprintf(message, sizeof(message), "[%s] ", tag);
        snprintf(message + strlen(message), sizeof(message) - strlen(message),
                 fmt, rand() % 3000 + 1000);
        break;
    default:
        snprintf(message, sizeof(message), "[%s] unknown event", tag);
        break;
    }

    /* 写入存储 */
    logs_storage_write_level(level, "%s", message);

    /* 同时打印到控制台，按级别选择 ESP_LOG 宏 */
    switch (level) {
    case LOGS_STORAGE_LEVEL_INFO:
        ESP_LOGI(tag, "%s", message);
        break;
    case LOGS_STORAGE_LEVEL_WARN:
        ESP_LOGW(tag, "%s", message);
        break;
    case LOGS_STORAGE_LEVEL_ERROR:
        ESP_LOGE(tag, "%s", message);
        break;
    }
}

/**
 * 示例主流程：
 * 1. 配置 BOOT 键 GPIO
 * 2. 配置日志轮转参数并初始化日志组件
 * 3. 正常模式：每秒写一条随机日志，轮询 BOOT 键
 * 4. 短按 BOOT：进入 USB MSC 模式，日志暂存内存
 * 5. USB 模式下长按 BOOT 2 秒：强制重启
 * 6. 主机安全弹出 U 盘：自动恢复磁盘日志并重启
 */
void app_main(void)
{
    /* 配置 BOOT 键 GPIO 为输入（ESP32 板载按键，低电平有效） */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* 启动时若 BOOT 键按下（长按重启后未释放），等待释放避免再次进入 USB 模式 */
    if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
        ESP_LOGI(TAG, "BOOT pressed at startup, waiting for release...");
        while (gpio_get_level(BOOT_BTN_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 配置日志轮转参数 */
    logs_storage_rotation_config_t cfg;
    logs_storage_rotation_config_default(&cfg);
    cfg.max_file_size_bytes = 256u * 1024u;      /* 单文件最大 256KB */
    cfg.max_log_files = 20u;                      /* 最多保留 20 个日志文件 */
    cfg.min_free_space_bytes = 128u * 1024u;     /* 最低剩余空间 128KB */
    cfg.rotate_threshold_bytes = 32u * 1024u;    /* 轮转阈值 32KB */
    logs_storage_rotation_config_set(&cfg);

    /* 用定时器值作为随机数种子，确保每次上电日志内容不同 */
    srand((unsigned int)esp_timer_get_time());

    if (!logs_storage_init()) {
        ESP_LOGE(TAG, "Failed to initialize logger");
        return;
    }

    ESP_LOGI(TAG, "Logger ready. Press BOOT to enter USB MSC mode");

    while (true) {
        /* 正常模式：写一条随机日志（不同级别、模块、数据内容） */
        write_random_log();

        /* 轮询 BOOT 键（1 秒内每 200ms 检测一次） */
        bool pressed = false;
        for (int i = 0; i < 5; i++) {
            if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
                pressed = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (pressed) {
            /* 等待按键释放（短按） */
            while (gpio_get_level(BOOT_BTN_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            ESP_LOGI(TAG, "Entering USB MSC mode, long press BOOT 2s to restart");
            vTaskDelay(pdMS_TO_TICKS(500));

            /* 启动 BOOT 键监控任务（高优先级，确保能响应长按） */
            xTaskCreate(boot_monitor_task, "boot_mon", 4096, NULL,
                        configMAX_PRIORITIES - 1, NULL);

            /* 进入 USB MSC 模式，主机将看到 U 盘 */
            logs_storage_usb_msc_init();

            /* USB 模式下持续写缓冲日志，直到模式退出（主机弹出或长按重启） */
            while (logs_storage_is_usb_mode_active()) {
                write_random_log();
                vTaskDelay(pdMS_TO_TICKS(2000));
            }

            /* USB 模式退出（主机安全弹出）：缓冲已 flush 到磁盘，重启恢复 USB-Serial/JTAG */
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
}
