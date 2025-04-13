#include "bluetooth_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

// #include "input_task.h"

#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "esp_hidd.h"
#include "esp_hid_gap.h"

// #include "ui_task.h"
// #include "shared.h"

volatile uint8_t bluetooth_status;
volatile uint32_t bt_pin_code;

static const char *TAG = "BT_TASK";

typedef struct
{
    TaskHandle_t task_hdl;
    esp_hidd_dev_t *hid_dev;
    uint8_t protocol_mode;
    uint8_t *buffer;
} local_param_t;

static local_param_t s_ble_hid_param = {0};

const unsigned char hid_descriptor_map[] = {
    // Report ID 1: Keyboard
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x19, 0xE0,        //   Usage Minimum (0xE0)
    0x29, 0xE7,        //   Usage Maximum (0xE7)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x06,        //   Report Count (3)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x94,        //   Logical Maximum (0x94) originally 65, 73 supports F13 - F24, 94 for japanese and korean language keys
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x29, 0x94,        //   Usage Maximum (0x94) originally 65, 73 supports F13 - F24
    0x81, 0x00,        //   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              // End Collection
    // Report ID 2: Media Keys
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x05, 0x0C,        //   Usage Page (Consumer)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x09, 0xB5,        //   Usage (Scan Next Track)
    0x09, 0xB6,        //   Usage (Scan Previous Track)
    0x09, 0xB7,        //   Usage (Stop)
    0x09, 0xB8,        //   Usage (Eject)
    0x09, 0xCD,        //   Usage (Play/Pause)
    0x09, 0xE2,        //   Usage (Mute)
    0x09, 0xE9,        //   Usage (Volume Increment)
    0x09, 0xEA,        //   Usage (Volume Decrement)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,               // End Collection
    // Report ID 3: Mouse
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (0x01)
    0x29, 0x03,        //     Usage Maximum (0x03)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x01,        //     Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              //   End Collection
    0x09, 0x3C,        //   Usage (Motion Wakeup)
    0x05, 0xFF,        //   Usage Page (Reserved 0xFF)
    0x09, 0x01,        //   Usage (0x01)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x02,        //   Report Count (2)
    0xB1, 0x22,        //   Feature (Data,Var,Abs,No Wrap,Linear,No Preferred State,No Null Position,Non-volatile)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0xB1, 0x01,        //   Feature (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0xC0,              // End Collection
};

static esp_hid_raw_report_map_t ble_report_maps[] = {
    {
        .data = hid_descriptor_map,
        .len = sizeof(hid_descriptor_map)
    }
};

// static esp_hid_device_config_t ble_hid_config = {
//     .vendor_id          = 0x7A10,
//     .product_id         = 0xCAFE,
//     .version            = 0x0002,
//     .device_name        = "Tavisco Macropad V2",
//     .manufacturer_name  = "Espressif",
//     .serial_number      = "0000000001",
//     .report_maps        = ble_report_maps,
//     .report_maps_len    = 1
// };

static esp_hid_device_config_t ble_hid_config = {
    .vendor_id          = 0x16C0,
    .product_id         = 0x05DF,
    .version            = 0x0100,
    .device_name        = "TaviscoMacropad",
    .manufacturer_name  = "Espressif",
    .serial_number      = "1234567890",
    .report_maps        = ble_report_maps,
    .report_maps_len    = 1
};


void ble_hid_task_start_up(void)
{
    bluetooth_status = BT_CONNECTED;
    ESP_LOGI(TAG, "ACTUALLY CONENCTED!");
    // uint8_t BT_HID_BUF_SIZE = 6;
    // uint8_t bt_hid_buf[BT_HID_BUF_SIZE];
    // memset(bt_hid_buf, 0, BT_HID_BUF_SIZE);
    // bt_hid_buf[0] = 0; // modifier
    // bt_hid_buf[1] = 0; // reserved
    // bt_hid_buf[2] = 0x51;
    // ble_kb_send(bt_hid_buf);

    // draw_ui();
}

void ble_hid_task_shut_down(void)
{
    ;
}

#define HID_RPT_ID_CC_IN        1   // Consumer Control input report ID
#define HID_CC_IN_RPT_LEN       7   // Consumer Control input report Len

void ble_kb_send(uint8_t* hid_buf)
{
    if (s_ble_hid_param.hid_dev == NULL)
    {
        ESP_LOGE(TAG, "DEVICE IS NULL! CANNOT SEND!");
        return;
    }
    esp_err_t ret;
    ret = esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, HID_RPT_ID_CC_IN, hid_buf, HID_CC_IN_RPT_LEN);
    ESP_ERROR_CHECK(ret);
}

void ble_mouse_send(uint8_t* hid_buf)
{
    esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 3, hid_buf, 4);
}

void ble_mk_send(uint8_t* hid_buf)
{
    esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 2, hid_buf, 2);
}

static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;
    static const char *TAG = "HID_DEV_BLE";
    esp_err_t ret;

    switch (event) {
    case ESP_HIDD_START_EVENT: {
        ESP_LOGI(TAG, "START");
        ret = esp_hid_ble_gap_adv_start();
        ESP_ERROR_CHECK(ret);
        break;
    }
    case ESP_HIDD_CONNECT_EVENT: {
        ESP_LOGI(TAG, "Starting to CONNECT");
        break;
    }
    case ESP_HIDD_PROTOCOL_MODE_EVENT: {
        ESP_LOGI(TAG, "PROTOCOL MODE[%u]: %s", param->protocol_mode.map_index, param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;
    }
    case ESP_HIDD_CONTROL_EVENT: {
        ESP_LOGI(TAG, "CONTROL[%u]: %sSUSPEND", param->control.map_index, param->control.control ? "EXIT_" : "");
        if (param->control.control)
        {
            // exit suspend
        }
        else
        {
            // suspend
        }
    break;
    }
    case ESP_HIDD_OUTPUT_EVENT: {
        ESP_LOGI(TAG, "OUTPUT[%u]: %8s ID: %2u, Len: %d, Data:", param->output.map_index, esp_hid_usage_str(param->output.usage), param->output.report_id, param->output.length);
        ESP_LOG_BUFFER_HEX(TAG, param->output.data, param->output.length);
        break;
    }
    case ESP_HIDD_FEATURE_EVENT: {
        ESP_LOGI(TAG, "FEATURE[%u]: %8s ID: %2u, Len: %d, Data:", param->feature.map_index, esp_hid_usage_str(param->feature.usage), param->feature.report_id, param->feature.length);
        ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
        break;
    }
    case ESP_HIDD_DISCONNECT_EVENT: {
        ESP_LOGI(TAG, "DISCONNECT: %s", esp_hid_disconnect_reason_str(esp_hidd_dev_transport_get(param->disconnect.dev), param->disconnect.reason));
        esp_hid_ble_gap_adv_start();
        bluetooth_status = BT_DISCOVERABLE;
        break;
    }
    case ESP_HIDD_STOP_EVENT: {
        ESP_LOGI(TAG, "STOP");
        bluetooth_status = BT_DISCOVERABLE;
        break;
    }
    default:
        ESP_LOGI(TAG, "UNKOWN EVENT");
        break;
    }
    return;
}

void my_bt_init(void)
{
    esp_err_t ret;

    esp_log_level_set("BT_SMP", ESP_LOG_DEBUG);
    esp_log_level_set("BT_GATT", ESP_LOG_DEBUG);
    esp_log_level_set("BT_APPL", ESP_LOG_DEBUG);


    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "setting hid gap, mode:%d", HID_DEV_MODE);
    ret = esp_hid_gap_init(HID_DEV_MODE);
    ESP_ERROR_CHECK(ret);

    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, ble_hid_config.device_name);
    ESP_ERROR_CHECK(ret);
    if ((ret = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler)) != ESP_OK) {
        ESP_LOGE(TAG, "GATTS register callback failed: %d", ret);
        return;
    }
    ESP_LOGI(TAG, "setting ble device");
    ESP_ERROR_CHECK(esp_hidd_dev_init(&ble_hid_config, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &s_ble_hid_param.hid_dev));
    bluetooth_status = BT_DISCOVERABLE;
}
