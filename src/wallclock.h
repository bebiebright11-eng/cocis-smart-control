#ifndef WALLCLOCK_H
#define WALLCLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Start the RTC chip and, if it holds a valid time, load it into the system clock. */
void wallclock_init(void);

/* Set the date and time (also written into the RTC chip). false = invalid values. */
bool wallclock_set(int year, int month, int day, int hour, int minute);

/* Has the time been set (from the RTC or by hand)? */
bool wallclock_is_set(void);

/* The current local time. */
void wallclock_get(struct tm *out);

/* "YYYY-MM-DD HH:MM:SS" or "(time not set)". */
void wallclock_format(char *buf, size_t len);

#endif