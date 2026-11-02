#ifndef DOOR_H
#define DOOR_H

#include <stdbool.h>

/* The two doors of one lane are neighbours: 0 and 1 are the entry lane,
 * 2 and 3 are the exit lane. (door ^ 1 gives the other door of the lane.) */
typedef enum {
    DOOR_ENTRY_1 = 0,    /* entry lane, outside door */
    DOOR_ENTRY_2 = 1,    /* entry lane, inside door  */
    DOOR_EXIT_1  = 2,    /* exit lane, inside door   */
    DOOR_EXIT_2  = 3,    /* exit lane, outside door  */
    DOOR_COUNT
} door_id_t;

/* Set up the 4 servos. All doors start closed. */
void doors_init(void);

/* Open a door. Returns false (and does nothing) if the other door of the
 * same lane is open: both doors must never be open together. */
bool door_open(door_id_t d);

/* Close a door. */
void door_close(door_id_t d);

/* Close every door (used by the emergency and alarm logic later). */
void doors_close_all(void);

bool door_is_open(door_id_t d);

#endif