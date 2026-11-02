#ifndef ALERTS_H
#define ALERTS_H

#include <stdbool.h>

typedef enum {
    ALERT_NONE,
    ALERT_EMPTY_BUILDING,   /* building shows nobody, but a room has people or a light on */
    ALERT_ROOMS_OVER,       /* the rooms hold more people than the building               */
} alert_kind_t;

typedef struct {
    alert_kind_t kind;          /* the alert that is active now (ALERT_NONE = all fine) */
    bool         acknowledged;  /* the guard silenced it                                */
    int          building;      /* people inside the building                           */
    int          room1;
    int          room2;
    bool         light1;
    bool         light2;
} alerts_status_t;

/* Start the watcher. Call once, after the rooms and the registry are ready. */
void alerts_start(void);

/* Live numbers plus the alert that is active. Safe from any task. */
void alerts_status(alerts_status_t *out);

/* Silence the alarm. Returns true if there was an active alert. */
bool alerts_acknowledge(void);

/* Texts for screens. */
const char *alerts_short_text(alert_kind_t k);
const char *alerts_long_text(alert_kind_t k);

#endif