#include <stdio.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/projdefs.h"
#include "hal/lcd_types.h"
#include "hal/uart_types.h"
#include "modbus.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"

// Constants
static char const TAG[] = "main";

#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define RED   0xFFFF0000

// On the CATT to RS485 and CAN adapter port "1" is RS485 and port "B" is CAN bus, only port "2" can be used by this
// example On the CATT to RS485 and RS422 adapter both port "1" and port "2" can be used, set the define below to select
// the second port
// #define PORT2

#ifndef PORT2
#define PIN_TX  34
#define PIN_RX  5
#define PIN_DIR 3
#else
#define PIN_TX  4
#define PIN_RX  15
#define PIN_DIR 2
#endif

// Global variables
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};
static QueueHandle_t                input_event_queue    = NULL;

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage partition
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %d", res);
            return;
        }
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %d", res);
        return;
    }

    // Initialize the Board Support Package
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
        return;
    }

    // Convert ESP-IDF color format into PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565:
            format = PAX_BUF_16_565RGB;
            break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888:
            format = PAX_BUF_24_888RGB;
            break;
        default:
            break;
    }

    // Convert BSP display rotation format into PAX orientation type
    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t      orientation      = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
            orientation = PAX_O_ROT_CCW;
            break;
        case BSP_DISPLAY_ROTATION_180:
            orientation = PAX_O_ROT_HALF;
            break;
        case BSP_DISPLAY_ROTATION_270:
            orientation = PAX_O_ROT_CW;
            break;
        case BSP_DISPLAY_ROTATION_0:
        default:
            orientation = PAX_O_UPRIGHT;
            break;
    }

    // Initialize graphics stack
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Main section of the app

    ESP_ERROR_CHECK(
        modbus_driver_install(UART_NUM_0, PIN_RX, PIN_TX, PIN_DIR, 9600, UART_PARITY_DISABLE, UART_STOP_BITS_1));

    pax_simple_rect(&fb, WHITE, 0, 0, pax_buf_get_width(&fb), 72);
    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Starting...");
    blit();

    while (1) {
        for (uint8_t i = 0; i <= 0x0F; i++) {

            esp_err_t write_coils_res = modbus_write_multiple_coils(UART_NUM_0, 0x01, 0, 4, (uint8_t[]){i}, 1000);
            printf("Writing coils (0x%02x): %s\r\n", i, esp_err_to_name(write_coils_res));

            uint8_t   coils          = 0;
            esp_err_t read_coils_res = modbus_read_coils(UART_NUM_0, 0x01, 0, 4, &coils, 1000);
            printf("Reading coils       : %s = 0x%02x\r\n", esp_err_to_name(read_coils_res), coils);

            uint8_t   inputs          = 0;
            esp_err_t read_inputs_res = modbus_read_discrete_inputs(UART_NUM_0, 0x01, 0, 4, &inputs, 1000);
            printf("Reading inputs      : %s = 0x%02x\r\n", esp_err_to_name(read_inputs_res), inputs);

            printf("\r\n");

            pax_simple_rect(&fb, WHITE, 0, 0, pax_buf_get_width(&fb), 72);
            pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Result:");
            char text[64];
            snprintf(text, sizeof(text), "Writing coils (0x%02x): %s", i, esp_err_to_name(write_coils_res));
            pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 18, text);
            snprintf(text, sizeof(text), "Reading coils       : %s = 0x%02x", esp_err_to_name(read_coils_res), coils);
            pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 36, text);
            snprintf(text, sizeof(text), "Reading inputs      : %s = 0x%02x", esp_err_to_name(read_inputs_res), inputs);
            pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 54, text);
            blit();

            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}
