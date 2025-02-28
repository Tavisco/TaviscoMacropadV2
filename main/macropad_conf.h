// Encoders 
#define GPIO_ENCODER_1_A		GPIO_NUM_6
#define GPIO_ENCODER_1_B		GPIO_NUM_7

// Modes	
#define MODE_COUNT			11

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
#define MODE_IDE_2			10


// Preferences - Screensaver
#define SCREENSAVER_TIME_S	15 * 1000000 // 60 seconds * X minutes
#define BLIP_FREQUENCY_S	3 * 1000000	// blip every X seconds
#define BLIP_DURATION_MS 	750 * 1000	// blip stays on for X ms