/**
 * @file logs_storage_usb.c
 * @brief USB MSC 模式切换：FAT 卸载/挂载、内存缓冲启停、弹出监控
 *
 * 职责：
 * - 进入 USB 模式：卸载 APP 端 FAT，置位 usb_mode_active，启动弹出监控任务
 * - 退出 USB 模式：flush 内存缓冲，清除标志，等待监控任务结束
 * - 弹出监控：三通道检测主机弹出（MSC 回调 / 挂载点切换 / tud_mounted）
 *
 * ESP32-S3 的 USB-OTG 与 USB-Serial/JTAG 共用 PHY，无法共存。
 * 进入 USB MSC 后，需重启设备才能恢复 COM9 串口。
 *
 * 注意：本文件不安装 TinyUSB 设备驱动，那是 logs_storage_usb_msc.c 的职责。
 */

#include "logs_storage_internal.h"
#include "esp_log.h"
#include "esp_system.h"
#include "tinyusb.h"

static const char *TAG = "LOG_MGR";

static volatile bool s_msc_ejected = false;

/* MSC 事件回调：主机弹出时挂载点切回 APP，置位 ejected 标志。
 * 仅在 usb_mode_active 时响应，避免退出后误触发。 */
static void msc_event_callback(tinyusb_msc_storage_handle_t handle,
                                tinyusb_msc_event_t *event,
                                void *arg) {
    (void)handle; (void)arg;

    if (!g_logs_storage.usb_mode_active) {
        return;
    }

    if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        ESP_LOGI(TAG, "Eject detected via MSC callback");
        s_msc_ejected = true;
    }
}

/* USB 弹出监控任务：检测主机安全弹出或断开，恢复磁盘日志并重启设备。
 * ESP32-S3 的 USB-OTG 和 USB-Serial/JTAG 共用 PHY，需重启恢复。 */
static void usb_eject_monitor_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "USB eject monitor started");

    s_msc_ejected = false;
    tinyusb_msc_set_storage_callback(msc_event_callback, NULL);

    /* 等待 USB 枚举完成 */
    int wait_count = 0;
    while (!tud_mounted() && wait_count < 20) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }

    while (!g_logs_storage.usb_monitor_exit && g_logs_storage.usb_mode_active) {
        tinyusb_msc_mount_point_t mp = TINYUSB_MSC_STORAGE_MOUNT_USB;
        esp_err_t err = tinyusb_msc_get_storage_mount_point(g_logs_storage.msc_storage, &mp);

        bool ejected = false;

        if (s_msc_ejected) {
            ejected = true;
        }
        if (!ejected && err == ESP_OK && mp == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            ESP_LOGI(TAG, "Eject detected via mount point switch");
            ejected = true;
        }
        if (!ejected && !tud_mounted()) {
            ESP_LOGI(TAG, "Eject detected via USB unmount");
            ejected = true;
        }

        if (ejected) {
            /* 强制退出时跳过处理，由调用方负责重启。
             * 否则 esp_restart 过程中 PHY 关闭会让 tud_mounted() 误判为弹出，
             * 形成 exit → restart → exit 死循环。 */
            if (g_logs_storage.usb_monitor_exit) {
                break;
            }

            ESP_LOGI(TAG, "USB eject detected, resuming disk logging");
            logs_storage_exit_usb_mode();
            vTaskDelay(pdMS_TO_TICKS(500));
            ESP_LOGI(TAG, "Restarting to restore USB-Serial/JTAG...");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            break;
        }

        if (g_logs_storage.usb_monitor_exit) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(LOGS_STORAGE_USB_MONITOR_POLL_MS));
    }

    g_logs_storage.usb_monitor_task = NULL;
    vTaskDelete(NULL);
}

/* 进入 USB MSC 模式：
 * 1. 关闭当前日志文件
 * 2. 将 MSC 存储挂载点从 APP 切到 USB（FAT 交给主机）
 * 3. 置位 usb_mode_active，worker 后续写入改为内存缓冲
 * 4. 启动 usb_eject_monitor_task 等待主机弹出 */
esp_err_t logs_storage_enter_usb_mode(void) {
    if (g_logs_storage.msc_storage == NULL) {
        ESP_LOGE(TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_logs_storage.log_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (g_logs_storage.current_log_file) {
        fflush(g_logs_storage.current_log_file);
        fclose(g_logs_storage.current_log_file);
        g_logs_storage.current_log_file = NULL;
    }

    esp_err_t err = tinyusb_msc_set_storage_mount_point(g_logs_storage.msc_storage,
                                                        TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch storage to USB: %s", esp_err_to_name(err));
        xSemaphoreGive(g_logs_storage.log_mutex);
        return err;
    }

    g_logs_storage.usb_mode_active = true;
    g_logs_storage.usb_monitor_exit = false;

    xSemaphoreGive(g_logs_storage.log_mutex);

    BaseType_t ok = xTaskCreate(usb_eject_monitor_task, "usb_monitor",
                                LOGS_STORAGE_USB_MONITOR_STACK_WORDS, NULL,
                                LOGS_STORAGE_USB_MONITOR_PRIORITY,
                                &g_logs_storage.usb_monitor_task);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "Failed to create USB monitor task");
    }

    ESP_LOGI(TAG, "USB MSC mode active, logs buffered in memory");
    return ESP_OK;
}

/* 退出 USB MSC 模式：
 * 1. 置位 usb_monitor_exit 通知监控任务退出（避免误判 PHY 关闭为弹出）
 * 2. 清除 usb_mode_active，worker 恢复磁盘写入
 * 3. flush 内存缓冲到磁盘
 * 4. 等待 usb_eject_monitor_task 结束（避免与重启竞争）
 * 注意：FAT 挂载点切回 APP 由下次 backend_init 处理。 */
esp_err_t logs_storage_exit_usb_mode(void) {
    if (g_logs_storage.msc_storage == NULL) {
        ESP_LOGE(TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!g_logs_storage.usb_mode_active) {
        return ESP_OK;
    }

    g_logs_storage.usb_monitor_exit = true;
    g_logs_storage.usb_mode_active = false;

    if (xSemaphoreTake(g_logs_storage.log_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    logs_storage_buffer_flush_to_disk();
    xSemaphoreGive(g_logs_storage.log_mutex);

    if (g_logs_storage.usb_monitor_task != NULL &&
        xTaskGetCurrentTaskHandle() != g_logs_storage.usb_monitor_task) {
        while (g_logs_storage.usb_monitor_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    ESP_LOGI(TAG, "Exited USB MSC mode, buffer flushed");
    return ESP_OK;
}
