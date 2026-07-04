/**
 * @file logs_storage_usb_msc.c
 * @brief USB MSC 模式入口：安装 TinyUSB 驱动并切换日志后端
 *
 * ESP32-S3 的 USB-OTG 与 USB-Serial/JTAG 共用一根 PHY，二者不能同时在线。
 * 进入 MSC 模式后主机看到 U 盘；退出后必须重启设备才能恢复 COM9 串口。
 * 本文件只负责"打开/关闭 USB 设备侧驱动"，FAT 挂载点切换由 backend.c 完成。
 */

#include "logs_storage_internal.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "LOG_USB";

/* 初始化 USB MSC：先切到 USB 模式（FAT 交给主机 + 启用内存缓冲），
 * 再安装 TinyUSB 设备驱动，主机即可看到 U 盘。
 * 失败时回滚到非 USB 模式。 */
void logs_storage_usb_msc_init(void)
{
    /* 进入 USB 模式：卸载 APP 端 FAT，启用内存缓冲，worker 继续运行 */
    esp_err_t err = logs_storage_enter_usb_mode();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enter USB mode: %s", esp_err_to_name(err));
        return;
    }

    /* 安装 TinyUSB 设备驱动，主机将看到 MSC U 盘 */
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        logs_storage_exit_usb_mode();
        return;
    }

    ESP_LOGI(TAG, "USB MSC initialized, storage exposed to host, logs buffered in memory");
    ESP_LOGI(TAG, "To resume disk logging, safely eject the USB drive on host PC");
}

/* 反初始化 USB MSC：调用 exit_usb_mode 重新挂载 FAT 并 flush 内存缓冲。
 * 不卸载 TinyUSB 驱动——ESP-IDF v6.0 没有合适的反初始化接口，
 * 调用者应在之后调用 esp_restart() 重启设备，重启后 USB-Serial/JTAG（COM9）自动恢复。 */
void logs_storage_usb_msc_deinit(void)
{
    /* 退出 USB 模式：重新挂载 FAT，flush 内存缓冲到磁盘，恢复日志记录。
     * 不卸载 TinyUSB 驱动——调用者应在之后调用 esp_restart() 重启设备，
     * 重启后 USB-Serial/JTAG（COM9）会自动恢复。 */
    esp_err_t err = logs_storage_exit_usb_mode();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to exit USB mode: %s", esp_err_to_name(err));
    }
}
