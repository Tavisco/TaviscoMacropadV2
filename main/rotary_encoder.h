#include <stdint.h>

#define DEBOUNCE_EDGE   100
#define DEBOUNCE_REPORT 250

typedef struct ROTARY_ENCODER_DATA
{
    uint8_t gpio_a, gpio_b;       // required
    int16_t min_value, max_value; // required
    int8_t factor;                // required
    uint8_t triggered;  
    uint32_t wait_until, wait_until_trigger;
    uint8_t filtered_a, last_a, last_filtered_a;
    int8_t dir;
    int16_t current_value;      
} rotary_encoder_t;


void rotary_task(rotary_encoder_t *data);