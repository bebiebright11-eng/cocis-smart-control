#ifndef EVENTLOG_H
#define EVENTLOG_H

#include "registry.h"

#define EVENT_LINE_MAX 160

typedef struct {
    char text[EVENT_LINE_MAX];
} event_line_t;

typedef enum {
    EV_ENTRY,       /* one person counted in                     */
    EV_EXIT,        /* one person counted out                    */
    EV_DENIED,      /* a card was refused (note = why)           */
    EV_ALARM,       /* tailgating, blocked beam, suspicious card */
    EV_BLOCKED,     /* a card was reported lost and blocked      */
    EV_CORRECT,     /* a guard corrected a room headcount        */
} event_type_t;

/* One period a person spent inside (used by the occupancy history). */
typedef struct {
    char name[32];
    char key[24];     /* registration number, or GUEST-<card> */
    char in[20];      /* "YYYY-MM-DD HH:MM:SS", or "?" = no entry on record */
    char out[20];     /* time, "" = still inside, "?" = no exit on record */
} stay_t;

/* Create the event queue. Call once, before the lanes start. */
void eventlog_init(void);

/* Record an event now. Quick: it only queues the line. */
void eventlog_add(event_type_t type, const access_info_t *p, const char *note);

/* Write the queued events to the SD card (called by the store task). */
void eventlog_flush(void);

/* The latest events, ready to print. They are kept in a ring of 'max' lines:
 * print ring[(first + i) % max] for i = 0 .. count-1. Returns the count,
 * or -1 if there is no log yet. */
int eventlog_recent(event_line_t *ring, int max, int *first);

/* Everyone who was inside at any time on 'date' ("YYYY-MM-DD").
 * Fills out[], returns how many, or -1 if there is no log yet.
 * *dropped = stays that did not fit. */
int eventlog_stays_on(const char *date, stay_t *out, int max, int *dropped);

/* Events of one type ("ALARM", "BLOCKED"...; NULL = any) on one date
 * (NULL = any), as full-time lines, in a ring like eventlog_recent. */
int eventlog_query(const char *type, const char *date,
                   event_line_t *ring, int max, int *first);

#endif