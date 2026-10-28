#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Button bits. The layout matches the Pro Controller wire format:
 *   report byte 3 = mask & 0xFF         (Y X B A SR SL R ZR)
 *   report byte 4 = (mask >> 8) & 0xFF  (Minus Plus RS LS Home Capture)
 *   report byte 5 = (mask >> 16) & 0xFF (Down Up Right Left SR SL L ZL)
 */
#define PRO_BTN_Y       (1UL << 0)
#define PRO_BTN_X       (1UL << 1)
#define PRO_BTN_B       (1UL << 2)
#define PRO_BTN_A       (1UL << 3)
#define PRO_BTN_R       (1UL << 6)
#define PRO_BTN_ZR      (1UL << 7)
#define PRO_BTN_MINUS   (1UL << 8)
#define PRO_BTN_PLUS    (1UL << 9)
#define PRO_BTN_RSTICK  (1UL << 10)
#define PRO_BTN_LSTICK  (1UL << 11)
#define PRO_BTN_HOME    (1UL << 12)
#define PRO_BTN_CAPTURE (1UL << 13)
#define PRO_BTN_DOWN    (1UL << 16)
#define PRO_BTN_UP      (1UL << 17)
#define PRO_BTN_RIGHT   (1UL << 18)
#define PRO_BTN_LEFT    (1UL << 19)
#define PRO_BTN_L       (1UL << 22)
#define PRO_BTN_ZL      (1UL << 23)

typedef enum {
    ProUsbDisconnected, // no host / not enumerated
    ProUsbEnumerated, // host configured us, waiting for the Switch handshake
    ProUsbHandshake, // handshake in progress
    ProUsbActive, // streaming input reports
} ProUsbStatus;

/** Switch USB to the Pro Controller interface and start the worker. */
void pro_usb_start(void);

/** Restore the previous USB mode and free everything. */
void pro_usb_stop(void);

/** Set the controller state. Stick values are -1, 0 or +1 (full tilt). */
void pro_usb_set_input(uint32_t buttons, int8_t lx, int8_t ly, int8_t rx, int8_t ry);

ProUsbStatus pro_usb_status(void);
