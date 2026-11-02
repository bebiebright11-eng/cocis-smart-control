#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "schedule.h"
#include "sdcard.h"
#include "wallclock.h"
#include "indicator.h"
#include "eventlog.h"
#include "registry.h"

#define SCHED_FILE     SDCARD_MOUNT "/schedule.txt"
#define DEFAULT_OPEN   (7 * 60)
#define DEFAULT_CLOSE  (18 * 60)
#define DEFAULT_ALARM  5
#define MAX_ALARM_MIN  120
#define MAX_WINDOW_MIN 240

/* The closing alarm: (beep-beep, beep-beep) repeated ROUNDS times */
#define ALARM_ROUNDS   3        /* how many times the whole pattern repeats */
#define BEEP_MS        150      /* one beep                                 */
#define BEEP_GAP_MS    150      /* gap between the two beeps of a pair      */
#define PAIR_GAP_MS    350      /* gap between the two pairs of a round     */
#define ROUND_GAP_MS   900      /* gap between rounds                       */

static const char *TAG = "schedule";

static SemaphoreHandle_t s_lock  = NULL;
static int               s_open  = DEFAULT_OPEN;
static int               s_close = DEFAULT_CLOSE;
static int               s_alarm = DEFAULT_ALARM;
static bool              s_dirty = false;

/* the guard's windows: index 0 = exit reader, 1 = entry reader */
static bool     s_win_on[2];
static uint32_t s_win_until[2];

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void schedule_init(void)
{
    s_lock  = xSemaphoreCreateMutex();
    s_open  = DEFAULT_OPEN;
    s_close = DEFAULT_CLOSE;
    s_alarm = DEFAULT_ALARM;
    s_dirty = false;
    memset(s_win_on, 0, sizeof(s_win_on));
    ESP_LOGI(TAG, "default schedule: 07:00 to 18:00, closing alarm %d min before", DEFAULT_ALARM);
}

void schedule_load(void)
{
    if (!sdcard_is_ready()) {
        return;
    }

    FILE *f = fopen(SCHED_FILE, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "no saved schedule yet: using the default");
        return;
    }

    int o = -1, c = -1, a = DEFAULT_ALARM;
    int n = fscanf(f, "SCHED|%d|%d|%d", &o, &c, &a);
    fclose(f);

    if (n < 2 || o < 0 || c > 1439 || o >= c) {
        ESP_LOGW(TAG, "the saved schedule is damaged: using the default");
        return;
    }
    if (n < 3 || a < 0 || a > MAX_ALARM_MIN) {
        a = DEFAULT_ALARM;              /* an older file without the alarm */
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_open  = o;
    s_close = c;
    s_alarm = a;
    s_dirty = false;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "schedule LOADED from the SD card: %02d:%02d to %02d:%02d, alarm %d min",
             o / 60, o % 60, c / 60, c % 60, a);
}

bool schedule_set(int open_min, int close_min)
{
    if (open_min < 0 || close_min > 1439 || open_min >= close_min) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_open  = open_min;
    s_close = close_min;
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return true;
}

void schedule_get(int *open_min, int *close_min)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *open_min  = s_open;
    *close_min = s_close;
    xSemaphoreGive(s_lock);
}

bool schedule_set_alarm(int minutes)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (minutes >= 0 && minutes <= MAX_ALARM_MIN && minutes < (s_close - s_open)) {
        s_alarm = minutes;
        s_dirty = true;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

int schedule_get_alarm(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int a = s_alarm;
    xSemaphoreGive(s_lock);
    return a;
}

bool schedule_is_open_now(void)
{
    if (!wallclock_is_set()) {
        return true;                    /* no clock yet: cannot judge */
    }

    struct tm t;
    wallclock_get(&t);
    int now = t.tm_hour * 60 + t.tm_min;

    int o, c;
    schedule_get(&o, &c);
    return (now >= o && now < c);
}

/* ---------- The guard's windows ---------- */
int schedule_window_left_sec(bool entry)
{
    int i = entry ? 1 : 0;
    int left = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_win_on[i]) {
        int32_t diff = (int32_t)(s_win_until[i] - now_ms());
        if (diff <= 0) {
            s_win_on[i] = false;        /* the time ran out: it closes itself */
        } else {
            left = (int)((diff + 999) / 1000);
        }
    }
    xSemaphoreGive(s_lock);
    return left;
}

bool schedule_activate(bool entry, int minutes)
{
    if (minutes < 0 || minutes > MAX_WINDOW_MIN) {
        return false;
    }
    int i = entry ? 1 : 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (minutes == 0) {
        s_win_on[i] = false;
    } else {
        s_win_on[i]    = true;
        s_win_until[i] = now_ms() + (uint32_t)minutes * 60000u;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGW(TAG, "%s reader window: %d min", entry ? "ENTRY" : "EXIT", minutes);
    return true;
}

bool schedule_lane_allowed(bool is_entry)
{
    if (schedule_is_open_now()) {
        return true;
    }
    return schedule_window_left_sec(is_entry) > 0;
}

/* ---------- The closing alarm ---------- */
int schedule_closing_in(void)
{
    if (!wallclock_is_set()) {
        return -1;
    }

    struct tm t;
    wallclock_get(&t);
    int now = t.tm_hour * 60 + t.tm_min;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int o = s_open, c = s_close, a = s_alarm;
    xSemaphoreGive(s_lock);

    if (a <= 0 || now < o) {
        return -1;
    }
    if (now >= c - a && now < c) {
        return c - now;
    }
    return -1;
}

/* (beep-beep, beep-beep) repeated ALARM_ROUNDS times */
static void play_closing_alarm(void)
{
    for (int round = 0; round < ALARM_ROUNDS; round++) {
        for (int pair = 0; pair < 2; pair++) {
            buzzer_beep(BEEP_MS);
            vTaskDelay(pdMS_TO_TICKS(BEEP_GAP_MS));
            buzzer_beep(BEEP_MS);
            vTaskDelay(pdMS_TO_TICKS(PAIR_GAP_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(ROUND_GAP_MS));
    }
}

static void alarm_task(void *arg)
{
    (void)arg;
    int last_key = -1;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!wallclock_is_set()) {
            continue;
        }

        struct tm t;
        wallclock_get(&t);
        int now = t.tm_hour * 60 + t.tm_min;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        int o = s_open, c = s_close, a = s_alarm;
        xSemaphoreGive(s_lock);

        if (a <= 0) {
            continue;
        }
        int at = c - a;
        if (at < o || now != at) {
            continue;
        }

        /* once per day: the same minute of the same day never fires twice */
        int key = (t.tm_year * 366 + t.tm_yday) * 1440 + now;
        if (key == last_key) {
            continue;
        }
        last_key = key;

        ESP_LOGW(TAG, "CLOSING ALARM: the building closes in %d minute(s)", a);

        access_info_t ev;
        char note[40];
        memset(&ev, 0, sizeof(ev));
        snprintf(ev.card_label, sizeof(ev.card_label), "SCH");
        snprintf(ev.name, sizeof(ev.name), "system");
        snprintf(ev.reg_no, sizeof(ev.reg_no), "-");
        snprintf(note, sizeof(note), "closing alarm, %d min left", a);
        eventlog_add(EV_CORRECT, &ev, note);

        play_closing_alarm();
    }
}

void schedule_start(void)
{
    xTaskCreate(alarm_task, "alarm_task", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "closing alarm watcher started");
}

/* ---------- Saving ---------- */
bool schedule_save_if_dirty(void)
{
    if (!sdcard_is_ready()) {
        return false;
    }

    int  o = 0, c = 0, a = 0;
    bool need = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dirty) {
        need    = true;
        s_dirty = false;
        o = s_open;
        c = s_close;
        a = s_alarm;
    }
    xSemaphoreGive(s_lock);

    if (!need) {
        return true;
    }

    bool ok = false;
    FILE *f = fopen(SCHED_FILE, "w");
    if (f != NULL) {
        ok = (fprintf(f, "SCHED|%d|%d|%d\n", o, c, a) > 0);
        if (fclose(f) != 0) {
            ok = false;
        }
    }

    if (ok) {
        ESP_LOGI(TAG, "schedule saved: %02d:%02d to %02d:%02d, alarm %d min",
                 o / 60, o % 60, c / 60, c % 60, a);
    } else {
        ESP_LOGE(TAG, "could not save the schedule");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dirty = true;                 /* try again next time */
        xSemaphoreGive(s_lock);
    }
    return ok;
}