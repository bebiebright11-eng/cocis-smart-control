#ifndef LANE_H
#define LANE_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "rfid.h"
#include "door.h"

/* Everything that makes one lane (entry or exit). */
typedef struct {
    const char   *name;         /* "ENTRY" or "EXIT" for the logs          */
    bool          is_entry;     /* true = people come in, false = go out   */
    rfid_reader_t reader;       /* this lane's card reader                 */
    gpio_num_t    beam_pin;     /* this lane's beam (button in Wokwi)      */
    door_id_t     door_first;   /* the door next to the reader             */
    door_id_t     door_second;  /* the door at the other end               */
} lane_config_t;

/* Starts the lane's own task. Call once per lane. */
bool lane_start(const lane_config_t *cfg);

/* Virtual tap: feeds a card ID into the entry or exit lane exactly as if
 * the reader had read it. Used by the Simulation tools menu. */
bool lane_virtual_tap(bool entry_lane, const uint8_t uid[4]);

#endif