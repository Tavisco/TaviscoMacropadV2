#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/i2c_master.h"
// #include "font8x8_basic.h"
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
#include "freertos/queue.h"
#include "bluetooth_task.h"

#define NEOPIXEL_PIN GPIO_NUM_48
#define MAIN_TAG "Main"
#define SETUP_TAG "Setup"
#define SCREENSAVER_TAG "Screen Saver"
#define UI_TAG "UI"
#define APP_BUTTON (GPIO_NUM_0) // Use BOOT signal by default
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

// Mode related variables
const char *modes[] = {"IDE", "Git", "Docker", "Numpad", "IoT", "Osu!", "Arrowpad", "WASD", "Multimedia", "Wiggler"};
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
QueueHandle_t switch_event_queue;

/************* TinyUSB descriptors ****************/

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

/**
 * @brief HID report descriptor
 *
 * In this example we implement Keyboard + Mouse HID device,
 * so we must define both report descriptors
 */
const uint8_t hid_report_descriptor[] = {
	TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
	TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_ITF_PROTOCOL_MOUSE))};

/**
 * @brief String descriptor
 */
const char *hid_string_descriptor[5] = {
	// array of pointer to string descriptors
	(char[]){0x09, 0x04},  // 0: is supported language is English (0x0409)
	"Tavisco",			   // 1: Manufacturer
	"Macropad V2",		   // 2: Product
	"2",				   // 3: Serials, should use chip ID
	"Tavisco Macropad V2", // 4: HID
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
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
	(void)instance;
	(void)report_id;
	(void)report_type;
	(void)buffer;
	(void)reqlen;

	return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
}

uint8_t this_sw_state[SW_COUNT];
uint8_t last_sw_state[SW_COUNT];

uint8_t rowcol_to_index(uint8_t row, uint8_t col)
{
	if (row >= SW_MATRIX_NUM_ROWS || col >= SW_MATRIX_NUM_COLS)
		return 0;
	return row * SW_MATRIX_NUM_COLS + col;
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
	if (xSemaphoreTake(kbscan_mutex, pdMS_TO_TICKS(KBSCAN_MUTEX_TIMEOUT_MS)) == pdFALSE)
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
	const float step_count = 50.0f;					   // Number of steps for the entire effect
	const float step_delay = duration_ms / step_count; // Time per step

	for (int i = 0; i <= step_count; i++)
	{
		// Smooth fade-in and fade-out in a single pass
		float phase = (float)i / step_count * M_PI; // 0 to π for one breath
		uint8_t brightness = (uint8_t)(min_brightness + (max_brightness * sin(phase)));

		tNeopixel pixel = {0, NP_RGB(0, 0, brightness)}; // Green with varying brightness
		neopixel_SetPixel(neopixel, &pixel, 1);
		vTaskDelay(pdMS_TO_TICKS(step_delay));
	}

	// Turn off after the effect is completed
	tNeopixel off_pixel = {0, NP_RGB(0, 0, 0)};
	neopixel_SetPixel(neopixel, &off_pixel, 1);
}

void draw_key_lines(void)
{
	ssd1306_Line(0, 36, 127, 36, White); // horizontal lines
	ssd1306_Line(0, 60, 127, 60, White);
	ssd1306_Line(0, 83, 127, 83, White);
	ssd1306_Line(0, 106, 127, 106, White);

	ssd1306_Line(32, 17, 32, 127, White); // vertical lines
	ssd1306_Line(64, 17, 64, 127, White);
	ssd1306_Line(96, 17, 96, 127, White);
}

void draw_keypad(const char *keys[10][4])
{
	draw_key_lines();

	int x_positions[4] = {0, 34, 66, 98};
	int y_positions[10] = {17, 17, 41, 41, 64, 64, 87, 87, 109, 109};

	for (int row = 0; row < 10; row += 2)
	{
		for (int col = 0; col < 4; col++)
		{
			if (keys[row][col])
			{
				// if there is second row
				if (keys[row + 1][col])
				{
					int text_width = strlen(keys[row + 1][col]) * 6; // Fixed 6x8 font
					int x = x_positions[col] + (31 - text_width) / 2;
					ssd1306_SetCursor(x, y_positions[row] + 10);
					ssd1306_WriteString((char *)keys[row + 1][col], Font_6x8, White);
					text_width = strlen(keys[row][col]) * 6; // Fixed 6x8 font
					x = x_positions[col] + (33 - text_width) / 2;
					ssd1306_SetCursor(x, y_positions[row]);
				}
				else
				{
					int text_width = strlen(keys[row][col]) * 6; // Fixed 6x8 font
					int x = x_positions[col] + (33 - text_width) / 2;
					ssd1306_SetCursor(x, y_positions[row] + 5);
				}
				ssd1306_WriteString((char *)keys[row][col], Font_6x8, White);
			}
		}
	}
}

void draw_custom_mouse_wiggler(void)
{
	if (mouse_wiggler_enabled)
	{
		ssd1306_SetCursor(48, 56);
		ssd1306_WriteString("ON", Font_16x26, White);
	}
	else
	{
		ssd1306_SetCursor(40, 56);
		ssd1306_WriteString("OFF", Font_16x26, White);
	}

	ssd1306_SetCursor(13, 119);
	ssd1306_WriteString("Any key to toggle", Font_6x8, White);
}

void draw_custom_multimedia(void)
{
	draw_key_lines();

	ssd1306_DrawBitmap(10, 112, icon_volume, 16, 12, White);
	ssd1306_DrawBitmap(43, 114, icon_prev, 16, 8, White);
	ssd1306_DrawBitmap(72, 114, icon_play, 16, 8, White);
	ssd1306_DrawBitmap(82, 114, icon_pause, 16, 8, White);
	ssd1306_DrawBitmap(108, 114, icon_next, 16, 8, White);
}

void draw_current_mode(void)
{
	ssd1306_FillRectangle(0, 0, 72, 14, Black);
	ssd1306_SetCursor(0, 3);
	ssd1306_WriteString((char *)modes[current_mode], Font_7x10, White);
	ssd1306_FillRectangle(0, 16, 128, 128, Black);

	if (current_mode == MODE_NUMPAD)
	{
		const char *keys[10][4] = {
			{"+", "-", "/", "*"},
			{NULL, NULL, NULL, NULL},

			{"=", "7", "8", "9"},
			{NULL, NULL, NULL, NULL},

			{",", "4", "5", "6"},
			{NULL, NULL, NULL, NULL},

			{"ENT", "1", "2", "3"},
			{"ER", NULL, NULL, NULL},

			{"000", "00", "0", "."},
			{NULL, NULL, NULL, NULL},
		};
		draw_keypad(keys);
	}

	if (current_mode == MODE_MULTIMEDIA) {
		draw_custom_multimedia();
	}

	// if (current_mode == MODE_OSU)
	// {
	// 	draw_custom_osu();
	// }

	if (current_mode == MODE_MOUSE_WIGGLER)
	{
		draw_custom_mouse_wiggler();
	}

	if (current_mode == MODE_GIT)
	{
		const char *keys[10][4] = {
			{NULL, NULL, NULL, NULL},
			{NULL, NULL, NULL, NULL},

			{"Mrg", NULL, NULL, NULL},
			{"dvlp", NULL, NULL, NULL},

			{"Chkt", "Push", "Stsh", "Stsh"},
			{"IIM", "IIM", NULL, "Pop"},

			{"New", "Chkt", "Pull", "Push"},
			{"IIM", "dev", NULL, NULL},

			{"Diff", "Sta", "Add", "Com"},
			{NULL, "tus", ".", "mit"},
		};
		draw_keypad(keys);
	}

	if (current_mode == MODE_DOCKER) {
		const char *keys[10][4] = {
			{"Do", NULL, NULL, NULL},
			{"it!", NULL, NULL, NULL},

			{"Prune", NULL, NULL, NULL},
			{NULL, NULL, NULL, NULL},

			{NULL, "Tor", NULL, "Tree"},
			{NULL, "chic", NULL, "cko"},

			{NULL, "PS", "NVIM", "DCL"},
			{NULL, NULL, NULL, NULL},

			{"DCU", "DCD", "DCP", "DCUD"},
			{NULL, NULL, NULL, NULL},
		};
	    draw_keypad(keys);
	}

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

	if (current_mode == MODE_ARROWPAD) {
		const char *keys[10][4] = {
			{"Esc", "Up", NULL, NULL},
			{NULL, NULL, NULL, NULL},

			{"<-", "Down", "->", NULL},
			{NULL, NULL, NULL, NULL},

			{"Left", NULL, NULL, NULL},
			{"Ctrl", NULL, NULL, NULL},

			{NULL, "Esc", "Up", NULL},
			{NULL, NULL, NULL, NULL},

			{"Left", "<-", "Down", "->"},
			{"Ctrl", NULL, NULL, NULL},
		};
		draw_keypad(keys);
	}

	if (current_mode == MODE_IDE)
	{
		const char *keys[10][4] = {
			{NULL, NULL, NULL, "GoTo"},
			{NULL, NULL, NULL, "Test"},

			{"Dup", "Del", "Refs", "Splt"},
			{"Line", "Line", NULL, NULL},

			{"Move", "Side", "Impl", "Splt"},
			{"Up", "bar", NULL, "MoveR"},

			{"Move", "Comm", "Re", "Run"},
			{"Down", "ent", "name", NULL},

			{"Fmt", "Org", "Term", "Com"},
			{"Code", "Imprt", "inal", "pile"},
		};
		draw_keypad(keys);
	}

	ssd1306_UpdateScreen();
}

void reset_variables(void)
{
	mouse_wiggler_enabled = false; // TODO: Should be false
}

void draw_ui(void)
{
	ESP_LOGI(UI_TAG, "Drawing UI\r\n");
	ssd1306_Fill(Black);
	ssd1306_Line(0, 15, 128, 15, White);

	if (tud_mounted())
	{
		ssd1306_DrawBitmap(109, 3, icon_usb, 16, 9, White);
	}
	else
	{
		if (bluetooth_status == BT_CONNECTED)
		{
			ssd1306_SetCursor(109, 3);
			ssd1306_WriteString("BT", Font_7x10, White);
		} else {
			ssd1306_SetCursor(109, 3);
			ssd1306_WriteString("?", Font_7x10, White);
		}
	}

	draw_current_mode();
}

void update_last_interaction(void)
{
	last_interaction_us = esp_timer_get_time();

	if (is_in_low_brightness_mode)
	{
		ssd1306_SetContrast(254);
		is_in_low_brightness_mode = false;
	}

	if (is_in_screensaver_mode)
	{
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
	if (direction == 0)
	{
		return;
	}

	update_last_interaction();

	current_mode += direction;
	if (current_mode == MODE_COUNT)
	{
		current_mode = 0;
	}

	if (current_mode < 0)
	{
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
		if (!mouse_wiggler_enabled)
		{
			vTaskDelay(pdMS_TO_TICKS(15));
			continue;
		}

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

		if (current_x == 255 && x_is_forward)
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

static void IRAM_ATTR rotary_isr_handler(void *arg)
{
	rotary_encoder_t *encoder = (rotary_encoder_t *)arg;
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	encoder->triggered = 1; // Mark it as triggered
	encoder->wait_until = esp_timer_get_time() + DEBOUNCE_EDGE;

	// Wake up the processing task
	vTaskNotifyGiveFromISR(encoder->task_handle, &xHigherPriorityTaskWoken);

	if (xHigherPriorityTaskWoken)
	{
		portYIELD_FROM_ISR(); // Force a context switch if needed
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

void rotaryTask(void *arg)
{
	rotary_encoder_t *encoder = (rotary_encoder_t *)arg;
	encoder->task_handle = xTaskGetCurrentTaskHandle(); // Store task handle for notifications

	while (true)
	{
		// Wait for an interrupt notification
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Block until notified
		esp_rom_delay_us(115);
		rotary_task(encoder); // Process encoder movement
		if (encoder->triggered && encoder->dir != 0)
		{
			change_current_mode(encoder->dir);
		}
	}
}

void scan_matrix_task()
{
	// set up initial state so it won't fire if a key is held on power-on
	sw_scan();
	memcpy(last_sw_state, this_sw_state, SW_COUNT);
	while(1)
	{
		vTaskDelay(pdMS_TO_TICKS(INPUT_TASK_FREQ_MS));

		sw_scan();

		for (uint8_t i = 0; i < SW_COUNT; i++)
		{
			if(this_sw_state[i] == 1 && last_sw_state[i] == 0)
			{
				switch_event_t sw_event = 
				{
					.id = i,
					.type = SW_EVENT_SHORT_PRESS,
				};
				xQueueSend(switch_event_queue, &sw_event, 0);
			}
		}
		memcpy(last_sw_state, this_sw_state, SW_COUNT);
	}
}

void send_hid_report(uint8_t hid, uint8_t modifier)
{
	uint8_t keycode[6] = {hid};
	tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, modifier, keycode);
	vTaskDelay(pdMS_TO_TICKS(13));
	tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);
	vTaskDelay(pdMS_TO_TICKS(13));
}

void send_string_report(char *str)
{
	for (const char* p = str; *p != '\0'; p++) {
		uint8_t const conv_table[128][2] =  { HID_ASCII_TO_KEYCODE };
		
		uint8_t keycode[6] = { 0 };
		uint8_t modifier   = 0;
		
		uint8_t index = (uint8_t)(*p);

		if ( conv_table[index][0] ) modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
		keycode[0] = conv_table[index][1];

		tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, modifier, keycode);

		vTaskDelay(pdMS_TO_TICKS(13));

		uint8_t empty_keys[6] = {0, 0, 0, 0, 0, 0};
		tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, empty_keys);

		// Slightly longer delay to ensure the key release is registered
		vTaskDelay(pdMS_TO_TICKS(13));

		str++;
	}
}

void send_keystroke(keymap_t keymap)
{
	if (keymap.hid != 0)
	{
		send_hid_report(keymap.hid, keymap.modifier);
	} else if (keymap.string != NULL)
	{
		send_string_report(keymap.string);
	}
}

keymap_t map_to_function(uint8_t key_id)
{
	keymap_t keymap;
		
	if (key_id < 8) {
		// Directly map to F13-F20
		keymap.hid = HID_KEY_F13 + key_id;
		keymap.modifier = 0;
	} else if (key_id < 16) {
		// Map to F13-F18 with Left Shift modifier
		keymap.hid = HID_KEY_F13 + (key_id - 8);
		keymap.modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
	} else {
		// Map to F13-F18 with Left Control modifier
		keymap.hid = HID_KEY_F13 + (key_id - 16);
		keymap.modifier = KEYBOARD_MODIFIER_LEFTCTRL;
	}

	return keymap;
}

void handle_sw_event(switch_event_t* this_sw_event)
{
	if (this_sw_event->id >= ENCODER_1_BTN){
		return;
	}

	uint8_t row = this_sw_event->id / 4;  // Integer division gives the row
	uint8_t col = this_sw_event->id % 4;  // Modulo gives the column

	// Safety check to prevent out-of-bounds access
	if (row >= 5 && col >= 4)
	{
		ESP_LOGE("MATRIX",  "Tried to handle invalid key!");
		return;
	}

	ESP_LOGI("MATRIX", "Pressed row [%u] col [%u]", row, col);

	// const keymap_t keys[5][4] = {
	// 	{{"", 0, 0}, {"", 0, 0}, {"", 0, 0}, {"", 0, 0}},
	// 	{{"", 0, 0}, {"", 0, 0}, {"", 0, 0}, {"", 0, 0}},
	// 	{{"", 0, 0}, {"", 0, 0}, {"", 0, 0}, {"", 0, 0}},
	// 	{{"", 0, 0}, {"", 0, 0}, {"", 0, 0}, {"", 0, 0}},
	// 	{{"", 0, 0}, {"", 0, 0}, {"", 0, 0}, {"", 0, 0}},
	// };

	switch (current_mode)
	{
	case MODE_NUMPAD: {
		const keymap_t keys[5][4] = {
			{{"+", 0, 0}, {"-", 0, 0}, {"/", 0, 0}, {"*", 0, 0}},
			{{"=", 0, 0}, {"7", 0, 0}, {"8", 0, 0}, {"9", 0, 0}},
			{{",", 0, 0}, {"4", 0, 0}, {"5", 0, 0}, {"6", 0, 0}},
			{{NULL, HID_KEY_ENTER, 0}, {"1", 0, 0}, {"2", 0, 0}, {"3", 0, 0}},
			{{"000", 0, 0}, {"00", 0, 0}, {"0", 0, 0}, {".", 0, 0}},
		};
		send_keystroke(keys[row][col]);
		break;
	}

	case MODE_GIT: {
		const keymap_t keys[5][4] = {
			{{"", 0, 0}, {"", 0, 0}, {"", 0, 0}, {"", 0, 0}},
			{{"", 0, 0}, {"", 0, 0}, {"", 0, 0}, {"", 0, 0}},
			{{"git checkout feature/IIM-", 0, 0}, {"git push -u origin feature/IIM-", 0, 0}, {"git stash", 0, 0}, {"git stash pop", 0, 0}},
			{{"git checkout -b feature/IIM-", 0, 0}, {"git checkout develop", 0, 0}, {"git pull", 0, 0}, {"git push", 0, 0}},
			{{"git diff", 0, 0}, {"git status", 0, 0}, {"git add .", 0, 0}, {"git commit -m \" \" ", 0, 0}},
		};
		send_keystroke(keys[row][col]);
		break;
	}

	case MODE_DOCKER: {
		const keymap_t keys[5][4] = {
			{{"docker compose pull && docker compose down && docker compose up -d", 0, 0}, {"", 0, 0}, {"", 0, 0}, {"", 0, 0}},
			{{"docker system prune -a", 0, 0}, {"", 0, 0}, {"", 0, 0}, {"", 0, 0}},
			{{"", 0, 0}, {"cd ~/composes/torchic", 0, 0}, {"", 0, 0}, {"cd ~/composes/treecko", 0, 0}},
			{{"", 0, 0}, {"docker ps", 0, 0}, {"nvim compose.yml", 0, 0}, {"docker compose logs -f", 0, 0}},
			{{"docker compose up", 0, 0}, {"docker compose down", 0, 0}, {"docker compose pull", 0, 0}, {"docker compose up -d", 0, 0}},
		};
		send_keystroke(keys[row][col]);
		break;
	}

	case MODE_IDE: {
		send_keystroke(map_to_function(this_sw_event->id));
		break;
	}

	case MODE_MOUSE_WIGGLER: {
		mouse_wiggler_enabled = !mouse_wiggler_enabled;
		draw_ui();
		break;
	}

	case MODE_ARROWPAD: {
		const keymap_t keys[5][4] = {
			{{NULL, HID_KEY_ESCAPE, 0}, {NULL, HID_KEY_ARROW_UP, 0}, {NULL, 0, 0}, {NULL, 0, 0}},
			{{NULL, HID_KEY_ARROW_LEFT, 0}, {NULL, HID_KEY_ARROW_DOWN, 0}, {NULL, HID_KEY_ARROW_RIGHT, 0}, {NULL, 0, 0}},
			{{NULL, HID_KEY_CONTROL_LEFT, 0}, {NULL, 0, 0}, {NULL, 0, 0}, {NULL, 0, 0}},
			{{NULL, 0, 0}, {NULL, HID_KEY_ESCAPE, 0}, {NULL, HID_KEY_ARROW_UP, 0}, {NULL, 0, 0}},
			{{NULL, HID_KEY_CONTROL_LEFT, 0}, {NULL, HID_KEY_ARROW_LEFT, 0}, {NULL, HID_KEY_ARROW_DOWN, 0}, {NULL, HID_KEY_ARROW_RIGHT, 0}},
		};
		send_keystroke(keys[row][col]);
	}

	default:
		break;
	}
}

void macropad_task()
{
	while(1)
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		switch_event_t sw_event = { 0 };

		if (xQueueReceive(switch_event_queue, &sw_event, 0) == pdTRUE)
		{
			update_last_interaction();
			handle_sw_event(&sw_event);
		}
	}  
}

void setup_gpio()
{
	const gpio_config_t sw_matrix_col_config = {
        .pin_bit_mask = BIT64(SWM_COL0_GPIO) | \
                BIT64(SWM_COL1_GPIO) | \
                BIT64(SWM_COL2_GPIO) | \
                BIT64(SWM_COL3_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = false,
        .pull_down_en = false,
    };
    ESP_ERROR_CHECK(gpio_config(&sw_matrix_col_config));
    sw_matrix_col_reset();
    const gpio_config_t sw_matrix_row_config = {
        .pin_bit_mask = BIT64(SWM_ROW0_GPIO) | \
                BIT64(SWM_ROW1_GPIO) | \
                BIT64(SWM_ROW2_GPIO) | \
                BIT64(SWM_ROW3_GPIO) | \
                BIT64(SWM_ROW4_GPIO),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = false,
        .pull_down_en = true,
    };
    ESP_ERROR_CHECK(gpio_config(&sw_matrix_row_config));

	const gpio_config_t button_config = {
        .pin_bit_mask = BIT64(GPIO_ENCODER_1_BTN) | \
                BIT64(GPIO_ENCODER_2_BTN) | \
                BIT64(GPIO_ENCODER_1_A) | \
                BIT64(GPIO_ENCODER_1_B) | \
				BIT64(GPIO_ENCODER_2_A) | \
                BIT64(GPIO_ENCODER_2_B),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = true,
        .pull_down_en = false,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));
}

void app_main(void)
{
	ESP_LOGI(SETUP_TAG, "\r\n\r\n=-=-=- Welcome to TaviscoMacropad V2! -=-=-=\r\n");
	neopixel = neopixel_Init(1, NEOPIXEL_PIN);
	tNeopixel startup_pixels[] =
		{
			{0, NP_RGB(0, 255, 0)}, /* green */
			{0, NP_RGB(0, 0, 0)},	/* green */
		};

	if (NULL == neopixel)
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

	ESP_LOGI(SETUP_TAG, "Initializing Keyboard Matrix");
	switch_event_queue = xQueueCreate(SWITCH_EVENT_QUEUE_SIZE, sizeof(switch_event_t));
	kbscan_mutex = xSemaphoreCreateMutex();

	ESP_LOGI(SETUP_TAG, "Initializing USB");
	const tinyusb_config_t tusb_cfg = {
		.device_descriptor = NULL,
		.string_descriptor = hid_string_descriptor,
		.string_descriptor_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]),
		.external_phy = false,
		.configuration_descriptor = hid_configuration_descriptor,
	};
	ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
	vTaskDelay(pdMS_TO_TICKS(1500)); // Is this really necessary?

	ESP_LOGI(SETUP_TAG, "Initializing BLE");
	my_bt_init();

	ESP_LOGI(SETUP_TAG, "Initializing UI");
	neopixel_SetPixel(neopixel, &startup_pixels[1], 1);
	draw_ui();
	reset_variables();
	last_interaction_us = esp_timer_get_time();

	ESP_LOGI(SETUP_TAG, "Initialization done! Starting tasks...");
	xTaskCreatePinnedToCore(rotaryTask, "RotaryTask", 4096, (void *)&encoder1, 1, NULL, 1);
	xTaskCreate(mouse_wiggler_task, "MouseWigglerTask", 4096, NULL, 1, NULL);
	xTaskCreatePinnedToCore(screensave_task, "ScreensaveTask", 4096, NULL, 1, NULL, 1);
	xTaskCreate(scan_matrix_task, "ScanMatrixTask", 4096, NULL, 1, NULL);
	xTaskCreate(macropad_task, "Macropadtask", 4096, NULL, 1, NULL);
	ESP_LOGI(SETUP_TAG, "All tasks started! The Macropad is ready.");
}
