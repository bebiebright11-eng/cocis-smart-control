#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <stdbool.h>

/* Set the defaults (07:00 to 18:00, alarm 5 min before closing). Call once, before the lanes start. */
void schedule_init(void);

/* Replace the defaults with what is saved on the SD card (if any). */
void schedule_load(void);

/* Start the task that sounds the closing alarm. Call once, after schedule_init. */
void schedule_start(void);

/* Times are in minutes since midnight (07:00 = 420). false = open must be before close. */
bool schedule_set(int open_min, int close_min);
void schedule_get(int *open_min, int *close_min);

/* Closing alarm: minutes before closing (0 = off, at most 120).
 * false = not allowed (must be shorter than the opening hours). */
bool schedule_set_alarm(int minutes);
int  schedule_get_alarm(void);

/* Is the building open right now? true if the clock is not set (cannot judge). */
bool schedule_is_open_now(void);

/* May a card be accepted at this reader right now?
 * true while the building is open, or while the guard's window for this reader is open. */
bool schedule_lane_allowed(bool is_entry);

/* The guard opens a reader for some minutes (1 to 240). 0 closes the window now.
 * false = minutes out of range. */
bool schedule_activate(bool entry, int minutes);

/* Seconds left in the guard's window for this reader (0 = no window). */
int schedule_window_left_sec(bool entry);

/* Minutes left until closing, but only inside the alarm period. -1 = not in it. */
int schedule_closing_in(void);

/* Write the schedule to the SD card if it changed (called by the store task). */
bool schedule_save_if_dirty(void);

#endif