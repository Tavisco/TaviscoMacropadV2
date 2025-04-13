#ifndef __BLUETOOTH_TASK_H
#define __BLUETOOTH_TASK_H

#ifdef __cplusplus
 extern "C" {
#endif 

#include <stdint.h>
#include <string.h>

void my_bt_init(void);

#define BT_DISABLED 0
#define BT_DISCOVERABLE 1
#define BT_CONNECTED 2

extern volatile uint8_t bluetooth_status;
extern volatile uint32_t bt_pin_code;
void ble_kb_send(uint8_t* hid_buf);
void ble_mouse_send(uint8_t* hid_buf);
void ble_mk_send(uint8_t* hid_buf);

#ifdef __cplusplus
}
#endif

#endif


