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
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "icons.h"
#include "macropad_conf.h"
#include "rotary_encoder.h"
#include "esp_timer.h"
#include "neopixel.h"
#include <math.h>
#include <semphr.h>


#define NEOPIXEL_PIN GPIO_NUM_48
#define MAIN_TAG "Main"
#define SETUP_TAG "Setup"
#define SCREENSAVER_TAG "Screen Saver"
#define UI_TAG "UI"
#define APP_BUTTON (GPIO_NUM_0) // Use BOOT signal by default
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

// Mode related variables
const char *modes[]= {"IDE", "Git", "Docker", "Numpad", "IoT", "Osu!", "Arrowpad", "WASD", "Multimedia", "Wiggler"};
int8_t current_mode = MODE_IDE;
bool mouse_wiggler_enabled = true;

// Encoders
rotary_encoder_t encoder1;

// Neopixel
tNeopixelContext neopixel;

// Screensaver
int64_t last_interaction_us = 0;
bool is_in_screensaver_mode = false;
bool is_in_low_brightness_mode = false;

// Keyboard
SemaphoreHandle_t kbscan_mutex;

/************* TinyUSB descriptors ****************/

#define TUSB_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

/**
 * @brief HID report descriptor
 *
 * In this example we implement Keyboard + Mouse HID device,
 * so we must define both report descriptors
 */
const uint8_t hid_report_descriptor[] = {
	TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
	TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_ITF_PROTOCOL_MOUSE))
};

/**
 * @brief String descriptor
 */
const char* hid_string_descriptor[5] = {
	// array of pointer to string descriptors
	(char[]){0x09, 0x04},  // 0: is supported language is English (0x0409)
	"Tavisco",             // 1: Manufacturer
	"Macropad V2",      // 2: Product
	"2",              // 3: Serials, should use chip ID
	"Tavisco Macropad V2",  // 4: HID
};

/**
 * @brief Configuration descriptor
 *
 * This is a simple configuration descriptor that defines 1 configuration and 1 HID interface
 */
static const uint8_t hid_configuration_descriptor[] = {
	// Configuration number, interface count, string index, total length, attribute, power in mA
	TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

	// Interface number, string index, boot protocol, report descriptor len, EP In address, size & polling interval
	TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

/********* TinyUSB HID callbacks ***************/

// Invoked when received GET HID REPORT DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
	// We use only one interface and one HID report descriptor, so we can ignore parameter 'instance'
	return hid_report_descriptor;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
	(void) instance;
	(void) report_id;
	(void) report_type;
	(void) buffer;
	(void) reqlen;

	return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
}

typedef enum {
	MOUSE_DIR_RIGHT,
	MOUSE_DIR_DOWN,
	MOUSE_DIR_LEFT,
	MOUSE_DIR_UP,
	MOUSE_DIR_MAX,
} mouse_dir_t;

#define DISTANCE_MAX        125
#define DELTA_SCALAR        5

static void mouse_draw_square_next_delta(int8_t *delta_x_ret, int8_t *delta_y_ret)
{
	static mouse_dir_t cur_dir = MOUSE_DIR_RIGHT;
	static uint32_t distance = 0;

	// Calculate next delta
	if (cur_dir == MOUSE_DIR_RIGHT) {
		*delta_x_ret = DELTA_SCALAR;
		*delta_y_ret = 0;
	} else if (cur_dir == MOUSE_DIR_DOWN) {
		*delta_x_ret = 0;
		*delta_y_ret = DELTA_SCALAR;
	} else if (cur_dir == MOUSE_DIR_LEFT) {
		*delta_x_ret = -DELTA_SCALAR;
		*delta_y_ret = 0;
	} else if (cur_dir == MOUSE_DIR_UP) {
		*delta_x_ret = 0;
		*delta_y_ret = -DELTA_SCALAR;
	}

	// Update cumulative distance for current direction
	distance += DELTA_SCALAR;
	// Check if we need to change direction
	if (distance >= DISTANCE_MAX) {
		distance = 0;
		cur_dir++;
		if (cur_dir == MOUSE_DIR_MAX) {
			cur_dir = 0;
		}
	}
}

static void app_send_hid_demo(void)
{
	// Keyboard output: Send key 'a/A' pressed and released
	ESP_LOGI(MAIN_TAG, "Sending Keyboard report");
	uint8_t keycode[6] = {HID_KEY_A};
	tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, keycode);
	vTaskDelay(pdMS_TO_TICKS(50));
	tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);

	// Mouse output: Move mouse cursor in square trajectory
	ESP_LOGI(MAIN_TAG, "Sending Mouse report");
	int8_t delta_x;
	int8_t delta_y;
	for (int i = 0; i < (DISTANCE_MAX / DELTA_SCALAR) * 4; i++) {
		// Get the next x and y delta in the draw square pattern
		mouse_draw_square_next_delta(&delta_x, &delta_y);
		tud_hid_mouse_report(HID_ITF_PROTOCOL_MOUSE, 0x00, delta_x, delta_y, 0, 0);
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}


uint8_t this_sw_state[SW_COUNT];
uint8_t last_sw_state[SW_COUNT];
uint32_t last_press_ts[SW_COUNT];

uint8_t rowcol_to_index(uint8_t row, uint8_t col)
{
	if(row >= SW_MATRIX_NUM_ROWS || col >= SW_MATRIX_NUM_COLS)
		return 0;
	return row*SW_MATRIX_NUM_COLS + col;
}

void scan_row(uint8_t *sw_buf, uint8_t this_col)
{
	sw_buf[rowcol_to_index(0, this_col)] = gpio_get_level(SWM_ROW0_GPIO);
	sw_buf[rowcol_to_index(1, this_col)] = gpio_get_level(SWM_ROW1_GPIO);
	sw_buf[rowcol_to_index(2, this_col)] = gpio_get_level(SWM_ROW2_GPIO);
	sw_buf[rowcol_to_index(3, this_col)] = gpio_get_level(SWM_ROW3_GPIO);
	sw_buf[rowcol_to_index(4, this_col)] = gpio_get_level(SWM_ROW4_GPIO);
}

void sw_matrix_col_reset(void)
{
	gpio_set_level(SWM_COL0_GPIO, 0);
	gpio_set_level(SWM_COL1_GPIO, 0);
	gpio_set_level(SWM_COL2_GPIO, 0);
	gpio_set_level(SWM_COL3_GPIO, 0);
}

void sw_scan(void)
{
	if(xSemaphoreTake(kbscan_mutex, pdMS_TO_TICKS(KBSCAN_MUTEX_TIMEOUT_MS)) == pdFALSE)
    	return;

	gpio_set_level(SWM_COL0_GPIO, 1);
	gpio_set_level(SWM_COL1_GPIO, 0);
	gpio_set_level(SWM_COL2_GPIO, 0);
	gpio_set_level(SWM_COL3_GPIO, 0);
	vTaskDelay(pdMS_TO_TICKS(1));
	scan_row(this_sw_state, 0);

	gpio_set_level(SWM_COL0_GPIO, 0);
	gpio_set_level(SWM_COL1_GPIO, 1);
	gpio_set_level(SWM_COL2_GPIO, 0);
	gpio_set_level(SWM_COL3_GPIO, 0);
	vTaskDelay(pdMS_TO_TICKS(1));
	scan_row(this_sw_state, 1);

	gpio_set_level(SWM_COL0_GPIO, 0);
	gpio_set_level(SWM_COL1_GPIO, 0);
	gpio_set_level(SWM_COL2_GPIO, 1);
	gpio_set_level(SWM_COL3_GPIO, 0);
	vTaskDelay(pdMS_TO_TICKS(1));
	scan_row(this_sw_state, 2);

	gpio_set_level(SWM_COL0_GPIO, 0);
	gpio_set_level(SWM_COL1_GPIO, 0);
	gpio_set_level(SWM_COL2_GPIO, 0);
	gpio_set_level(SWM_COL3_GPIO, 1);
	vTaskDelay(pdMS_TO_TICKS(1));
	scan_row(this_sw_state, 3);

	sw_matrix_col_reset(); // need time to settle, do not remove
	vTaskDelay(pdMS_TO_TICKS(1));

	this_sw_state[ENCODER_1_BTN] = 1 - gpio_get_level(GPIO_ENCODER_1_BTN);
	this_sw_state[ENCODER_2_BTN] = 1 - gpio_get_level(GPIO_ENCODER_2_BTN);

	xSemaphoreGive(kbscan_mutex);
}

void neopixel_BreatheEffect(tNeopixelContext neopixel, uint16_t duration_ms)
{
	const uint8_t max_brightness = 255;
	const uint8_t min_brightness = 0;
	const float step_count = 50.0f;  // Number of steps for the entire effect
	const float step_delay = duration_ms / step_count; // Time per step

	for (int i = 0; i <= step_count; i++)
	{
		// Smooth fade-in and fade-out in a single pass
		float phase = (float)i / step_count * M_PI; // 0 to π for one breath
		uint8_t brightness = (uint8_t)(min_brightness + (max_brightness * sin(phase)));

		tNeopixel pixel = {0, NP_RGB(brightness, 0, 0)}; // Green with varying brightness
		neopixel_SetPixel(neopixel, &pixel, 1);
		vTaskDelay(pdMS_TO_TICKS(step_delay));
	}

	// Turn off after the effect is completed
	tNeopixel off_pixel = {0, NP_RGB(0, 0, 0)};
	neopixel_SetPixel(neopixel, &off_pixel, 1);
}

void draw_key_lines(void) {
	ssd1306_Line(0,36,127,36,White); // horizontal lines
	ssd1306_Line(0,60,127,60,White);
	ssd1306_Line(0,83,127,83,White);
	ssd1306_Line(0,106,127,106,White);
  
	ssd1306_Line(32,17,32,127,White); // vertical lines
	ssd1306_Line(64,17,64,127,White);
	ssd1306_Line(96,17,96,127,White);
}

void draw_keypad(const char *keys[10][4]) {
	draw_key_lines();

	int x_positions[4] = {0, 34, 66, 98};
	int y_positions[10] = {17, 17, 41, 41, 64, 64, 87, 87, 109, 109};

	for (int row = 0; row < 10; row+=2) {
		for (int col = 0; col < 4; col++) {
			if (keys[row][col]) {
				// if there is second row
				if (keys[row+1][col]) {
					int text_width = strlen(keys[row+1][col]) * 6; // Fixed 6x8 font
					int x = x_positions[col] + (31 - text_width) / 2;
					ssd1306_SetCursor(x, y_positions[row]+10);
					ssd1306_WriteString((char *)keys[row+1][col], Font_6x8, White);
					text_width = strlen(keys[row][col]) * 6; // Fixed 6x8 font
					x = x_positions[col] + (33 - text_width) / 2;
					ssd1306_SetCursor(x, y_positions[row]);
				} else {
					int text_width = strlen(keys[row][col]) * 6; // Fixed 6x8 font
					int x = x_positions[col] + (33 - text_width) / 2;
					ssd1306_SetCursor(x, y_positions[row]+5);
				}
				ssd1306_WriteString((char *)keys[row][col], Font_6x8, White);
			}
		}
	}
}

void draw_custom_mouse_wiggler(void)
{
	if (mouse_wiggler_enabled) {
		ssd1306_SetCursor(48, 17);
		ssd1306_WriteString("ON", Font_16x26, White);
	} else {
		ssd1306_SetCursor(40, 17);
		ssd1306_WriteString("OFF", Font_16x26, White);
	}
	
	ssd1306_SetCursor(10, 56);
	ssd1306_WriteString("Any key to toggle", Font_6x8, White);
}

void draw_current_mode(void) 
{
	ssd1306_FillRectangle(0, 0, 72, 14, Black);
	ssd1306_SetCursor(0, 3);
	ssd1306_WriteString((char *)modes[current_mode], Font_7x10, White);
	ssd1306_FillRectangle(0, 16, 128, 128, Black);

	// if (current_mode == MODE_NUMPAD) {
	// 	draw_custom_numpad();
	// }

	// if (current_mode == MODE_MULTIMEDIA) {
	// 	draw_custom_multimedia();
	// }

	// if (current_mode == MODE_OSU)
	// {
	// 	draw_custom_osu();
	// }

	if (current_mode == MODE_MOUSE_WIGGLER)
	{
		draw_custom_mouse_wiggler();
	}

	// if (current_mode == MODE_GIT) {
	//     const char *keys[3][3] = {
	//         {nullptr,	"Stash",	"St pop"},
	//         {"Diff",	"Pull", 	"Push"},
	//         {"Status",	"Add .",	"Commit"}
	//     };
	//     draw_keypad(keys);
	// }

	// if (current_mode == MODE_DOCEKR) {
	//     const char *keys[3][3] = {
	//         {"Torchic",	"DCU",	"Treecko"},
	//         {"PS",		"NVIM",	"DCL"},
	//         {"DCD",		"DCP",	"DCUD"}
	//     };
	//     draw_keypad(keys);
	// }

	// if (current_mode == MODE_ARROWPAD) {
	//     const char *keys[3][3] = {
	//         {"Esc",		"Up",		nullptr},
	//         {"Left",	"Down", 	"Right"},
	//         {"L CTRL",	nullptr,	"Space"}
	//     };
	//     draw_keypad(keys);
	// }

	// if (current_mode == MODE_WASD) {
	//     const char *keys[3][3] = {
	//         {"Esc",		"W",		nullptr},
	//         {"A",		"S", 		"D"},
	//         {"L CTRL",	nullptr,	"Space"}
	//     };
	//     draw_keypad(keys);
	// }

	if (current_mode == MODE_IDE) {
		const char *keys[10][4] = {
			{NULL,		NULL,		NULL,		NULL},
			{NULL,		NULL,		NULL,		NULL},

			{NULL,		"Del",		"Refs",		"Splt"},
			{NULL,		"Line"	,	NULL,		NULL},

			{"Move",	"Side",		"Impl",		"Splt"},
			{"Up",		"bar",		NULL,		"MoveR"},

			{"Move",	"Comm",		"Re",		"Run"},
			{"Down",	"ent",		"name",		NULL},

			{"Fmt",		"Org",		"Term",		"Com"},
			{"Code",	"Imprt",	"inal",		"pile"},
		};
		draw_keypad(keys);
	}

	// if (current_mode == MODE_IDE_2) {
	//     const char *keys[3][3] = {
	//         {nullptr,	nullptr,	"Refs"},
	//         {nullptr,	nullptr,	"SpMov R"},
	//         {nullptr,	"Rename",	"Splt R"}
	//     };
	//     draw_keypad(keys);
	// }

	ssd1306_UpdateScreen();
}

void reset_variables(void)
{
	mouse_wiggler_enabled = true; // TODO: Should be false
}

void draw_ui(void)
{
	ESP_LOGI(UI_TAG, "Drawing UI\r\n");
	ssd1306_Fill(Black);
	// oled_screen.drawLine(0,15,128,15,WHITE);
	ssd1306_Line(0, 15, 128, 15, White);

	if (tud_mounted())
	{
		ssd1306_DrawBitmap(109, 3, icon_usb, 16, 9, White);
		// oled_screen.OLEDBitmap(109, 3, 16, 9, icon_usb, false, sizeof(icon_usb)/sizeof(uint8_t));
	} else {
		ssd1306_SetCursor(109, 3);
		ssd1306_WriteString("N", Font_7x10, White);
	}

	draw_current_mode();
}

void update_last_interaction(void)
{
	last_interaction_us = esp_timer_get_time();

	if (is_in_low_brightness_mode) {
		ssd1306_SetContrast(254);
		is_in_low_brightness_mode = false;
	}

	if (is_in_screensaver_mode) {
		ESP_LOGI(SCREENSAVER_TAG, "Exiting from screensave mode!");
		ssd1306_SetDisplayOn(1);
		ssd1306_SetContrast(254);
		draw_ui();
		is_in_screensaver_mode = false;
		is_in_low_brightness_mode = false;
	}
}

void draw_splash(void)
{
	ssd1306_DrawBitmap(14, 0, tavisco, 100, 100, White);
	ssd1306_SetCursor(43, 105);
	ssd1306_WriteString("Tavisco", Font_7x10, White);
	ssd1306_SetCursor(39, 116);
	ssd1306_WriteString("Macropad", Font_7x10, White);
	ssd1306_SetCursor(112, 118);
	ssd1306_WriteString("V2", Font_6x8, White);
	ssd1306_UpdateScreen();
}

void change_current_mode(int8_t direction)
{
	if (direction == 0) {
		return;
	}

	update_last_interaction();

	current_mode += direction;
	if (current_mode == MODE_COUNT) {
		current_mode = 0;
	}

	if (current_mode < 0) {
		current_mode = MODE_COUNT - 1;
	}

	reset_variables();
	draw_current_mode();
	ESP_LOGI(MAIN_TAG, "Changed mode to [%s], count[%i], direction: [%i]\r\n", modes[current_mode], current_mode, direction);
	vTaskDelay(pdMS_TO_TICKS(150));
}

void screensave_task()
{
	static int64_t last_blip_off_us = 0;

	while (1)
	{
		int64_t now = esp_timer_get_time();

		if (is_in_screensaver_mode)
		{
			if (now - last_blip_off_us > BLIP_FREQUENCY_S)
			{
				neopixel_BreatheEffect(neopixel, 3750);
				last_blip_off_us = esp_timer_get_time();
			}
		}
		else
		{
			int64_t time_since_last_interaction = now - last_interaction_us;
			bool should_be_in_screensave = time_since_last_interaction > SCREENSAVER_TIME_S;
			bool should_be_in_min_brightness = time_since_last_interaction > (SCREENSAVER_TIME_S / 2);

			if (should_be_in_min_brightness && !is_in_low_brightness_mode && !should_be_in_screensave)
			{
				ESP_LOGI(SCREENSAVER_TAG, "Entering low brightness mode");
				for (uint8_t i = 254; i > 0; i--) 
				{
					ssd1306_SetContrast(i);
					vTaskDelay(pdMS_TO_TICKS(10));
				}
				is_in_low_brightness_mode = true;
			}

			if (should_be_in_screensave)
			{
				ESP_LOGI(SCREENSAVER_TAG, "Entering screensave mode");
				is_in_screensaver_mode = true;
				ssd1306_SetDisplayOn(0);
				last_blip_off_us = now;
			}
		}

		vTaskDelay(pdMS_TO_TICKS(15));
	}
}

void mouse_wiggler_task()
{
	while (1)
	{
		if (current_mode != MODE_MOUSE_WIGGLER)
		{
			vTaskDelay(pdMS_TO_TICKS(15));
			continue;
		}

		// bool const keys_pressed = keyboard.update(current_mode);
		// if (keys_pressed)
		// {
		// 	mouse_wiggler_enabled = !mouse_wiggler_enabled;
		// 	sleep_ms(150);
		// 	draw_ui();
		// 	update_last_interaction();
		// }

		// if (!mouse_wiggler_enabled)
		// 	return;

		// skip if hid is not ready yet
		if (!tud_hid_ready())
		{
			vTaskDelay(pdMS_TO_TICKS(15));
			continue;
		}

		static int16_t current_x = 0;
		static int8_t delta_x = 1;
		static bool x_is_forward = true;
		static int8_t delta_y = 0;

		if (current_x == 256 && x_is_forward)
		{
			delta_x = -1;
			x_is_forward = false;
		}

		if (current_x == 0 && !x_is_forward)
		{
			delta_x = 1;
			x_is_forward = true;
		}

		current_x += delta_x;
		tud_hid_mouse_report(HID_ITF_PROTOCOL_MOUSE, 0x00, delta_x, delta_y, 0, 0);
		vTaskDelay(pdMS_TO_TICKS(15));
	}
}

static void IRAM_ATTR rotary_isr_handler(void *arg) {
	rotary_encoder_t *encoder = (rotary_encoder_t *)arg;
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	encoder->triggered = 1;  // Mark it as triggered
	encoder->wait_until = esp_timer_get_time() + DEBOUNCE_EDGE;

	// Wake up the processing task
	vTaskNotifyGiveFromISR(encoder->task_handle, &xHigherPriorityTaskWoken);

	if (xHigherPriorityTaskWoken) {
		portYIELD_FROM_ISR();  // Force a context switch if needed
	}
}

void setup_encoders()
{
	encoder1.gpio_a = GPIO_ENCODER_2_A;
	encoder1.gpio_b = GPIO_ENCODER_2_B;
	encoder1.min_value = 1;
	encoder1.max_value = 5;
	encoder1.factor = 1;
	encoder1.current_value = 1;

	// Install ISR service if not already done
	ESP_ERROR_CHECK(gpio_set_intr_type(GPIO_ENCODER_2_A, GPIO_INTR_ANYEDGE));
	ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_EDGE));
	ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_ENCODER_2_A, rotary_isr_handler, (void *)&encoder1));
}

void rotaryTask(void *arg) {
	rotary_encoder_t *encoder = (rotary_encoder_t *)arg;
	encoder->task_handle = xTaskGetCurrentTaskHandle(); // Store task handle for notifications

	while (true) {
		// Wait for an interrupt notification
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Block until notified
		esp_rom_delay_us(115);
		rotary_task(encoder); // Process encoder movement
		if (encoder->triggered && encoder->dir != 0) {
			change_current_mode(encoder->dir);
		}
	}
}

void mainTask() {
	while (1) {
		// Keys Task
		// screensave_task();
		// mouse_wiggler_task();

		if (tud_mounted()) {
			static bool send_hid_data = false;
			if (send_hid_data) {
				app_send_hid_demo();
				draw_ui();
			}
			send_hid_data = !gpio_get_level(APP_BUTTON);
		}
		vTaskDelay(pdMS_TO_TICKS(15));
	}
}

void setup_gpio()
{
	const gpio_num_t GPIOS[]= {SWM_COL0_GPIO, SWM_COL1_GPIO, SWM_COL2_GPIO, SWM_COL3_GPIO, SWM_ROW0_GPIO, SWM_ROW1_GPIO, SWM_ROW2_GPIO, SWM_ROW3_GPIO, SWM_ROW4_GPIO, GPIO_ENCODER_1_BTN, GPIO_ENCODER_2_BTN, GPIO_ENCODER_1_A, GPIO_ENCODER_1_B, GPIO_ENCODER_2_A, GPIO_ENCODER_2_B};

	for (uint8_t i = 0; i < ARRAY_SIZE(GPIOS); i++)
	{
		const gpio_config_t boot_button_config = {
			.pin_bit_mask = BIT64(GPIOS[i]),
			.mode = GPIO_MODE_INPUT,
			.intr_type = GPIO_INTR_DISABLE,
			.pull_up_en = true,
			.pull_down_en = false,
		};
		ESP_ERROR_CHECK(gpio_config(&boot_button_config));
		vTaskDelay(pdMS_TO_TICKS(5));
	}
}

void app_main(void)
{
	ESP_LOGI(SETUP_TAG, "\r\n\r\n=-=-=- Welcome to TaviscoMacropad V2! -=-=-=\r\n");
	neopixel = neopixel_Init(1, NEOPIXEL_PIN);
	tNeopixel startup_pixels[] =
	{
		{ 0, NP_RGB(0,  255, 0) }, /* green */
		{ 0, NP_RGB(0,  0, 0) }, /* green */
	};
 
	if(NULL == neopixel)
	{
	   ESP_LOGE(SETUP_TAG, "[%s] Initialization failed\n", __func__);
	} 
	neopixel_SetPixel(neopixel, &startup_pixels[0], 1);

	ESP_LOGI(SETUP_TAG, "Initializing OLED");
	ssd1306_Init();
	draw_splash();

	ESP_LOGI(SETUP_TAG, "Initializing GPIO");
	setup_gpio();

	ESP_LOGI(SETUP_TAG, "Initializing Rotary Encoder");
	setup_encoders();

	ESP_LOGI(SETUP_TAG, "Initializing USB");
	const tinyusb_config_t tusb_cfg = {
		.device_descriptor = NULL,
		.string_descriptor = hid_string_descriptor,
		.string_descriptor_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]),
		.external_phy = false,
		.configuration_descriptor = hid_configuration_descriptor,
	};
	ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
	vTaskDelay(pdMS_TO_TICKS(1500));

	ESP_LOGI(SETUP_TAG, "Initializing UI");
	neopixel_SetPixel(neopixel, &startup_pixels[1], 1);
	draw_ui();
	last_interaction_us = esp_timer_get_time();
	ESP_LOGI(SETUP_TAG, "All done! Entering main loop");
	
	// xTaskCreatePinnedToCore(mainTask, "MainTask", 4096, NULL, 1, NULL, 1);
	xTaskCreatePinnedToCore(rotaryTask, "RotaryTask", 4096, (void *)&encoder1, 1, NULL, 1);
	xTaskCreatePinnedToCore(mouse_wiggler_task, "MouseWigglerTask", 4096, NULL, 1, NULL, 1);
	xTaskCreatePinnedToCore(screensave_task, "ScreensaveTask", 4096, NULL, 1, NULL, 1);
}
