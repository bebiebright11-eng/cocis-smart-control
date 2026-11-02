#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "eventlog.h"
#include "sdcard.h"

#define EVENTS_FILE   SDCARD_MOUNT "/events.txt"
#define QUEUE_LEN     16
#define MAX_OPEN      64      /* people tracked as "inside" while reading the file */

static const char *TAG = "eventlog";

static QueueHandle_t s_q = NULL;

typedef struct {
    bool used;
    char key[24];
    char name[32];
    char in[20];
} open_t;

static open_t s_open[MAX_OPEN];

static const char *type_text(event_type_t t)
{
    switch (t) {
    case EV_ENTRY:   return "ENTRY";
    case EV_EXIT:    return "EXIT";
    case EV_DENIED:  return "DENIED";
    case EV_ALARM:   return "ALARM";
    case EV_BLOCKED: return "BLOCKED";
    case EV_CORRECT: return "CORRECT";
    }
    return "?";
}

/* Cut a line at every '|' and point f[] at the pieces. Returns how many. */
static int split_fields(char *line, char *f[], int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p != '\0' && n < max; p++) {
        if (*p == '|') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

/* Replace the characters our file uses as separators. */
static void clean(char *s)
{
    for (; *s; s++) {
        if (*s == '|' || *s == '\n' || *s == '\r') {
            *s = ' ';
        }
    }
}

void eventlog_init(void)
{
    s_q = xQueueCreate(QUEUE_LEN, sizeof(event_line_t));
    if (s_q == NULL) {
        ESP_LOGE(TAG, "could not create the event queue");
    }
}

void eventlog_add(event_type_t type, const access_info_t *p, const char *note)
{
    if (s_q == NULL || p == NULL) {
        return;
    }

    /* who counts as "the same person" when the log is read later */
    char key[24];
    if (strcmp(p->reg_no, "GUEST") == 0) {
        snprintf(key, sizeof(key), "GUEST-%s", p->card_label);
    } else if (p->reg_no[0] == '\0' || strcmp(p->reg_no, "-") == 0) {
        snprintf(key, sizeof(key), "%s", p->card_label[0] ? p->card_label : "?");
    } else {
        snprintf(key, sizeof(key), "%s", p->reg_no);
    }

    char name[32];
    char text[48];
    snprintf(name, sizeof(name), "%s", p->name[0] ? p->name : "?");
    snprintf(text, sizeof(text), "%s", note ? note : "");
    clean(key);
    clean(name);
    clean(text);

    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    event_line_t ln;
    int n = snprintf(ln.text, sizeof(ln.text),
                     "%lld|%04d-%02d-%02d %02d:%02d:%02d|%s|%s|%s|%s|%s\n",
                     (long long)now,
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec,
                     type_text(type), key,
                     p->card_label[0] ? p->card_label : "?",
                     name, text);
    if (n >= (int)sizeof(ln.text)) {           /* too long: cut it but keep the line end */
        ln.text[sizeof(ln.text) - 2] = '\n';
        ln.text[sizeof(ln.text) - 1] = '\0';
    }

    if (xQueueSend(s_q, &ln, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full: one event was lost");
    }
}

void eventlog_flush(void)
{
    if (s_q == NULL || !sdcard_is_ready() || uxQueueMessagesWaiting(s_q) == 0) {
        return;
    }

    FILE *f = fopen(EVENTS_FILE, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "could not open the event log for writing");
        return;
    }

    event_line_t ln;
    int written = 0;
    while (xQueueReceive(s_q, &ln, 0) == pdTRUE) {
        fputs(ln.text, f);
        written++;
    }
    fclose(f);
    ESP_LOGI(TAG, "%d event(s) saved to the SD card", written);
}

int eventlog_recent(event_line_t *ring, int max, int *first)
{
    *first = 0;
    if (!sdcard_is_ready()) {
        return -1;
    }
    FILE *f = fopen(EVENTS_FILE, "r");
    if (f == NULL) {
        return -1;
    }

    int  total = 0;
    char line[EVENT_LINE_MAX + 40];
    char *fld[8];

    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        int n = split_fields(line, fld, 8);
        if (n < 6 || strlen(fld[1]) < 19) {
            continue;
        }
        /* a short, readable line: MM-DD HH:MM:SS  TYPE  card  name  note */
        snprintf(ring[total % max].text, EVENT_LINE_MAX, "%s %-7s %-5s %-14.14s %.18s",
                 fld[1] + 5, fld[2], fld[4], fld[5], (n >= 7) ? fld[6] : "");
        total++;
    }
    fclose(f);

    if (total <= max) {
        *first = 0;
        return total;
    }
    *first = total % max;
    return max;
}

/* ---------- Occupancy history ---------- */

static int day_cmp(const char *stamp, const char *date)
{
    return strncmp(stamp, date, 10);        /* compares the YYYY-MM-DD part */
}

/* Keep this stay only if it touches the chosen date. */
static void add_stay(const char *date, stay_t *out, int max, int *n, int *dropped,
                     const char *key, const char *name, const char *in, const char *outts)
{
    bool touches;

    if (in[0] == '?') {                     /* exit with no entry on record */
        touches = (day_cmp(outts, date) == 0);
    } else if (outts[0] == '\0') {          /* still inside */
        touches = (day_cmp(in, date) <= 0);
    } else if (outts[0] == '?') {           /* entry with no exit on record */
        touches = (day_cmp(in, date) == 0);
    } else {
        touches = (day_cmp(in, date) <= 0 && day_cmp(outts, date) >= 0);
    }
    if (!touches) {
        return;
    }
    if (*n >= max) {
        (*dropped)++;
        return;
    }

    stay_t *s = &out[(*n)++];
    snprintf(s->key,  sizeof(s->key),  "%s", key);
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->in,   sizeof(s->in),   "%.19s", in);
    snprintf(s->out,  sizeof(s->out),  "%.19s", outts);
}

int eventlog_stays_on(const char *date, stay_t *out, int max, int *dropped)
{
    *dropped = 0;
    if (!sdcard_is_ready()) {
        return -1;
    }
    FILE *f = fopen(EVENTS_FILE, "r");
    if (f == NULL) {
        return -1;
    }

    memset(s_open, 0, sizeof(s_open));
    int   n = 0;
    char  line[EVENT_LINE_MAX + 40];
    char *fld[8];

    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        int k = split_fields(line, fld, 8);
        if (k < 6 || strlen(fld[1]) < 19) {
            continue;
        }

        bool is_in  = (strcmp(fld[2], "ENTRY") == 0);
        bool is_out = (strcmp(fld[2], "EXIT") == 0);
        if (!is_in && !is_out) {
            continue;
        }

        const char *stamp = fld[1];
        const char *key   = fld[3];
        const char *name  = fld[5];

        int slot = -1, free_slot = -1;
        for (int i = 0; i < MAX_OPEN; i++) {
            if (s_open[i].used && strcmp(s_open[i].key, key) == 0) {
                slot = i;
                break;
            }
        }
        for (int i = 0; i < MAX_OPEN; i++) {
            if (!s_open[i].used) {
                free_slot = i;
                break;
            }
        }

        if (is_in) {
            if (slot >= 0) {                /* came in twice, no exit between */
                add_stay(date, out, max, &n, dropped, key, s_open[slot].name,
                         s_open[slot].in, "?");
            } else if (free_slot >= 0) {
                slot = free_slot;
            } else {
                (*dropped)++;               /* table full */
                continue;
            }
            s_open[slot].used = true;
            snprintf(s_open[slot].key,  sizeof(s_open[slot].key),  "%s", key);
            snprintf(s_open[slot].name, sizeof(s_open[slot].name), "%s", name);
            snprintf(s_open[slot].in,   sizeof(s_open[slot].in),   "%.19s", stamp);
        } else {                            /* an EXIT */
            if (slot >= 0) {
                add_stay(date, out, max, &n, dropped, key, s_open[slot].name,
                         s_open[slot].in, stamp);
                s_open[slot].used = false;
            } else {                        /* left, but no entry on record */
                add_stay(date, out, max, &n, dropped, key, name, "?", stamp);
            }
        }
    }
    fclose(f);

    /* whoever never left is still inside */
    for (int i = 0; i < MAX_OPEN; i++) {
        if (s_open[i].used) {
            add_stay(date, out, max, &n, dropped, s_open[i].key, s_open[i].name,
                     s_open[i].in, "");
        }
    }
    return n;
}

/* ---------- Events of one type ---------- */

int eventlog_query(const char *type, const char *date,
                   event_line_t *ring, int max, int *first)
{
    *first = 0;
    if (!sdcard_is_ready()) {
        return -1;
    }
    FILE *f = fopen(EVENTS_FILE, "r");
    if (f == NULL) {
        return -1;
    }

    int   total = 0;
    char  line[EVENT_LINE_MAX + 40];
    char *fld[8];

    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        int k = split_fields(line, fld, 8);
        if (k < 6 || strlen(fld[1]) < 19) {
            continue;
        }
        if (type != NULL && strcmp(fld[2], type) != 0) {
            continue;
        }
        if (date != NULL && strncmp(fld[1], date, 10) != 0) {
            continue;
        }
        /* time, card, owner, note */
        snprintf(ring[total % max].text, EVENT_LINE_MAX, "%.19s %-5.5s %-12.12s %.40s",
                 fld[1], fld[4], fld[5], (k >= 7) ? fld[6] : "");
        total++;
    }
    fclose(f);

    if (total <= max) {
        *first = 0;
        return total;
    }
    *first = total % max;
    return max;
}