#ifndef ROOM_TASK_H
#define ROOM_TASK_H

#include <stdbool.h>
#include "driver/gpio.h"

/* Starts one room: sets up its pins and starts its own task.
 * Call it once per room (room_id 1 or 2). */
void room_start(int room_id, gpio_num_t pin_beam_a, gpio_num_t pin_beam_b, gpio_num_t pin_light);

/* Safe to call from any other task (each room has its own mutex). */
int  room_headcount_get(int room_id);

/* Guard correction, for example from the menu. */
void room_headcount_set(int room_id, int value);

/* true if the room's light is on right now. */
bool room_light_is_on(int room_id);

#endif