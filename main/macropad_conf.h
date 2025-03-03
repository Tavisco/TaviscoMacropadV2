// Encoders 
#define GPIO_ENCODER_1_A		GPIO_NUM_6
#define GPIO_ENCODER_1_B		GPIO_NUM_7

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