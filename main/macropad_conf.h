// Encoders 
#define GPIO_ENCODER_1_A		GPIO_NUM_15
#define GPIO_ENCODER_1_B		GPIO_NUM_16
#define GPIO_ENCODER_1_BTN		GPIO_NUM_17
#define GPIO_ENCODER_2_A		GPIO_NUM_6
#define GPIO_ENCODER_2_B		GPIO_NUM_7
#define GPIO_ENCODER_2_BTN		GPIO_NUM_0

// Modes	
#define MODE_COUNT			10

#define MODE_IDE			0
#define MODE_GIT			1
#define MODE_DOCEKR			2
#define MODE_NUMPAD			3
#define MODE_IOT			4
#define MODE_OSU			5
#define MODE_ARROWPAD		6
#define MODE_WASD			7
#define MODE_MULTIMEDIA		8
#define MODE_MOUSE_WIGGLER	9

// Preferences - Screensaver
#define SCREENSAVER_TIME_S	(60 * 10) * 1000000 // (60 seconds * X minutes) * uS to S
#define BLIP_FREQUENCY_S	5 * 1000000	// blip every X seconds
#define BLIP_DURATION_MS 	750 * 1000	// blip stays on for X ms

// Keyboard
#define SW_MATRIX_NUM_COLS	4
#define SW_MATRIX_NUM_ROWS	5
#define GPIO_BUTTONS_COUNT	5
#define SW_COUNT			(SW_MATRIX_NUM_COLS * SW_MATRIX_NUM_ROWS) + GPIO_BUTTONS_COUNT

#define SWM_COL0_GPIO		GPIO_NUM_10
#define SWM_COL1_GPIO		GPIO_NUM_11
#define SWM_COL2_GPIO		GPIO_NUM_12
#define SWM_COL3_GPIO		GPIO_NUM_13

#define SWM_ROW0_GPIO		GPIO_NUM_18
#define SWM_ROW1_GPIO		GPIO_NUM_8
#define SWM_ROW2_GPIO		GPIO_NUM_3
#define SWM_ROW3_GPIO		GPIO_NUM_46
#define SWM_ROW4_GPIO		GPIO_NUM_9

#define KBSCAN_MUTEX_TIMEOUT_MS 100
#define INPUT_TASK_FREQ_MS 13

#define ENCODER_1_BTN		20
#define ENCODER_2_BTN		21