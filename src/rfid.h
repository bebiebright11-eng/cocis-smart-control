#ifndef RFID_H
#define RFID_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

#define RFID_UID_LEN 4

typedef struct { uint8_t bytes[RFID_UID_LEN]; } rfid_uid_t;

typedef struct rfid_reader *rfid_reader_t;   /* one reader (details hidden) */

/* Call once: sets up the shared SPI wires. */
bool rfid_bus_init(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso);

/* Call once per reader: each reader has its own CS and RST pin. */
rfid_reader_t rfid_reader_add(gpio_num_t cs, gpio_num_t rst);

/* true = a card was present and its ID is in *uid. */
bool rfid_read_uid(rfid_reader_t r, rfid_uid_t *uid);

#endif