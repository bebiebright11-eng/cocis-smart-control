#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>
#include "driver/gpio.h"

#define SDCARD_MOUNT "/sdcard"

/* Mount the card on the SPI bus that rfid_bus_init() already started.
 * Call it after the RFID readers are added and before the lanes start. */
bool sdcard_init(gpio_num_t cs_pin);

/* Is the card mounted and usable? */
bool sdcard_is_ready(void);

/* Start-up test: counts the starts in /sdcard/boot.txt and logs the number. */
void sdcard_selftest(void);

#endif