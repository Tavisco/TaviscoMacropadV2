#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/i2c_master.h"
//#include "font8x8_basic.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"
#define TAG "MAIN"


void app_main(void)
{
    printf("Hello world!\n");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    printf("Initialize I2C bus");
    ssd1306_Init();


    for (int i = 127; i >= 0; i--) {
        printf("Restarting in %d seconds...\n", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
        ssd1306_SetCursor(0, 0);
        ssd1306_WriteString("Ola mundo", Font_7x10, White);
        ssd1306_SetCursor(0, 20);
        ssd1306_WriteString("Programado em C", Font_7x10, White);
        ssd1306_SetCursor(0, 30);
        ssd1306_WriteString("raiz", Font_7x10, White);

        ssd1306_SetCursor(0, 50);
        ssd1306_WriteString("Restarting in", Font_7x10, White);
        ssd1306_SetCursor(0, 60);
        char *hello_world = (char*)malloc(15 * sizeof(char));
        sprintf(hello_world, "%d seconds...", i);
        ssd1306_WriteString(hello_world, Font_7x10, White);

        ssd1306_UpdateScreen();
    }
    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
}