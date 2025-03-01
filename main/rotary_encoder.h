#ifndef ROTARY_ENCODER_H
#define ROTARY_ENCODER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"  // <-- ADD THIS
#include "driver/gpio.h"


#define DEBOUNCE_EDGE   100
#define DEBOUNCE_REPORT 250

typedef struct {
    uint8_t gpio_a;
    uint8_t gpio_b;
    int min_value;
    int max_value;
    int current_value;
    int factor;
    int triggered;
    int dir;
    int64_t wait_until;
    int64_t wait_until_trigger;
    uint8_t last_a;
    uint8_t last_filtered_a;
    uint8_t filtered_a;
    TaskHandle_t task_handle;  // <-- NOW IT'S DEFINED
} rotary_encoder_t;

void setup_encoders(void);
void rotary_task(rotary_encoder_t *encoder);

#endif // ROTARY_ENCODER_H