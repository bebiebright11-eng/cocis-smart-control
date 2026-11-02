#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "alerts.h"
#include "registry.h"
#include "room_task.h"
#include "indicator.h"
#include "eventlog.h"

static const char *TAG = "alerts";

#define CHECK_MS      500     /* how often the numbers are compared            */
#define CONFIRM_MS    3000    /* the problem must last this long               */
#define GROUP_GAP_MS  400     /* the short pause between one T T T and the next */

static SemaphoreHandle_t s_lock = NULL;
static alert_kind_t      s_kind = ALERT_NONE;
static bool              s_ack  = false;

/* Read the live numbers. */
static void read_numbers(alerts_status_t *s)
{
    s->building = registry_count_inside();
    s->room1    = room_headcount_get(1);
    s->room2    = room_headcount_get(2);
    s->light1   = room_light_is_on(1);
    s->light2   = room_light_is_on(2);
}

/* Decide which alert (if any) these numbers mean. */
static alert_kind_t judge(const alerts_status_t *s)
{
    int rooms = s->room1 + s->room2;

    if (s->building <= 0) {
        if (rooms > 0 || s->light1 || s->light2) {
            return ALERT_EMPTY_BUILDING;
        }
    } else if (rooms > s->building) {
        return ALERT_ROOMS_OVER;
    }
    return ALERT_NONE;
}

/* T T T */
static void three_beeps(void)
{
    for (int i = 0; i < 3; i++) {
        buzzer_beep(150);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void log_alert(alert_kind_t k, const alerts_status_t *s)
{
    access_info_t ev;
    char note[64];

    memset(&ev, 0, sizeof(ev));
    snprintf(ev.card_label, sizeof(ev.card_label), "ALERT");
    snprintf(ev.name, sizeof(ev.name), "system");
    snprintf(ev.reg_no, sizeof(ev.reg_no), "-");

    if (k == ALERT_ROOMS_OVER) {
        snprintf(note, sizeof(note), "rooms %d > building %d",
                 s->room1 + s->room2, s->building);
    } else {
        snprintf(note, sizeof(note), "building 0, rooms %d, light %s",
                 s->room1 + s->room2,
                 (s->light1 || s->light2) ? "on" : "off");
    }
    eventlog_add(EV_ALARM, &ev, note);
}

static void alerts_task(void *arg)
{
    (void)arg;
    alert_kind_t candidate = ALERT_NONE;   /* what the numbers say right now      */
    int          bad_ms    = 0;            /* how long it has said the same thing */

    vTaskDelay(pdMS_TO_TICKS(5000));       /* let the saved data load first */

    for (;;) {
        alerts_status_t s;
        read_numbers(&s);
        alert_kind_t now = judge(&s);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        alert_kind_t active = s_kind;
        bool         ack    = s_ack;
        xSemaphoreGive(s_lock);

        if (now == ALERT_NONE) {
            if (active != ALERT_NONE) {
                ESP_LOGI(TAG, "alert cleared: the numbers agree again");
            }
            candidate = ALERT_NONE;
            bad_ms    = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_kind = ALERT_NONE;
            s_ack  = false;
            xSemaphoreGive(s_lock);
        } else {
            if (now != candidate) {
                candidate = now;
                bad_ms    = 0;
            } else {
                bad_ms += CHECK_MS;
            }

            if (active != now && bad_ms >= CONFIRM_MS) {
                /* a new alert */
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_kind = now;
                s_ack  = false;
                xSemaphoreGive(s_lock);

                ESP_LOGE(TAG, "ALERT: %s", alerts_long_text(now));
                log_alert(now, &s);
                three_beeps();
                vTaskDelay(pdMS_TO_TICKS(GROUP_GAP_MS));
                continue;                  /* straight on to the next T T T */
            }

            if (active == now && !ack) {
                /* not silenced yet: T T T again, after a short pause */
                three_beeps();
                vTaskDelay(pdMS_TO_TICKS(GROUP_GAP_MS));
                continue;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
    }
}

void alerts_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(alerts_task, "alerts_task", 4096, NULL, 2, NULL);
}

void alerts_status(alerts_status_t *out)
{
    memset(out, 0, sizeof(*out));
    read_numbers(out);

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        out->kind         = s_kind;
        out->acknowledged = s_ack;
        xSemaphoreGive(s_lock);
    }
}

bool alerts_acknowledge(void)
{
    bool had = false;

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_kind != ALERT_NONE) {
            s_ack = true;
            had   = true;
        }
        xSemaphoreGive(s_lock);
    }
    return had;
}

const char *alerts_short_text(alert_kind_t k)
{
    switch (k) {
    case ALERT_NONE:           return "none";
    case ALERT_EMPTY_BUILDING: return "ROOMS ACTIVE, BUILDING EMPTY";
    case ALERT_ROOMS_OVER:     return "MORE PEOPLE IN ROOMS THAN IN BUILDING";
    }
    return "?";
}

const char *alerts_long_text(alert_kind_t k)
{
    switch (k) {
    case ALERT_NONE:
        return "No alert.";
    case ALERT_EMPTY_BUILDING:
        return "The building shows nobody inside, but a room has people or a light on.";
    case ALERT_ROOMS_OVER:
        return "The rooms hold more people than the building. Someone got in without a card.";
    }
    return "?";
}