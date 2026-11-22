/* ps2.h - PS/2 Controller and Mouse Driver */
#ifndef PS2_H
#define PS2_H

#include <stdint.h>

/* PS/2 Controller ports */
#define PS2_DATA_PORT    0x60
#define PS2_CMD_PORT     0x64

/* PS/2 Controller commands */
#define PS2_CMD_READ_CONFIG    0x20
#define PS2_CMD_WRITE_CONFIG   0x60
#define PS2_CMD_DISABLE_AUX    0xA7
#define PS2_CMD_ENABLE_AUX     0xA8
#define PS2_CMD_TEST_AUX       0xA9
#define PS2_CMD_DISABLE_KBD    0xAD
#define PS2_CMD_ENABLE_KBD     0xAE

/* PS/2 Mouse commands */
#define PS2_MOUSE_CMD_RESET       0xFF
#define PS2_MOUSE_CMD_RESEND      0xFE
#define PS2_MOUSE_CMD_SET_DEFAULT 0xF6
#define PS2_MOUSE_CMD_DISABLE     0xF5
#define PS2_MOUSE_CMD_ENABLE      0xF4
#define PS2_MOUSE_CMD_SET_RATE    0xF3
#define PS2_MOUSE_CMD_SET_RES     0xE8
#define PS2_MOUSE_CMD_GET_ID      0xF2
#define PS2_MOUSE_CMD_SAMPLE      0xF0

/* Initialization function */
int ps2_init(void);

#endif /* PS2_H */
