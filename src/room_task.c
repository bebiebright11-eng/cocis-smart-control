#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "beam_counter.h"
#include "room_task.h"

/* ---------- Settings ---------- */
#define MAX_ROOMS           2
#define SAMPLE_MS           10    /* how often the beams are read          */
#define DEBOUNCE_SAMPLES    3     /* a level must stay the same 3 times    */
#define LIGHT_OFF_DELAY_MS  5000  /* wait this long at 0 people before off */

static const char *TAG = "room";

/* ---------- Data kept for each room ---------- */
typedef struct {
    bool    stable;     /* the clean value we report */
    bool    last_raw;   /* what we read last time    */
    uint8_t count;      /* how many times in a row   */
} debounce_t;

typedef struct {
    int               id;
    gpio_num_t        pin_a;       /* beam A: outer side of the doorway */
    gpio_num_t        pin_b;       /* beam B: inner side of the doorway */
    gpio_num_t        pin_light;   /* the room light                    */

    SemaphoreHandle_t lock;        /* protects headcount */
    int               headcount;

    beam_counter_t    counter;     /* A/B logic (beam_counter.c) */
    debounce_t        da, db;

    bool              lit;
    bool              zero_timer_running;
    uint32_t          zero_since;
} room_t;

static room_t s_rooms[MAX_ROOMS];

static room_t *room_find(int room_id)
{
    if (room_id < 1 || room_id > MAX_ROOMS) {
        return NULL;
    }
    return &s_rooms[room_id - 1];
}

/* ---------- Shared headcount (protected by the room's mutex) ---------- */
int room_headcount_get(int room_id)
{
    room_t *r = room_find(room_id);
    if (r == NULL || r->lock == NULL) {
        return 0;
    }
    xSemaphoreTake(r->lock, portMAX_DELAY);
    int v = r->headcount;
    xSemaphoreGive(r->lock);
    return v;
}

void room_headcount_set(int room_id, int value)
{
    room_t *r = room_find(room_id);
    if (r == NULL || r->lock == NULL) {
        return;
    }
    xSemaphoreTake(r->lock, portMAX_DELAY);
    r->headcount = (value < 0) ? 0 : value;
    xSemaphoreGive(r->lock);
}

bool room_light_is_on(int room_id)
{
    room_t *r = room_find(room_id);
    if (r == NULL) {
        return false;
    }
    return r->lit;
}

/* ---------- Reading a beam ---------- */
/* The ONLY place that knows about the hardware. In Wokwi a pushbutton stands
 * in for a beam. With a real IR beam, only this function changes.
 * Pins use the internal pull-up, so "cut" = the pin reads 0. */
static bool beam_cut(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

/* Debounce: accept a new level only after it was seen DEBOUNCE_SAMPLES times in a row. */
static bool debounce(debounce_t *d, bool raw)
{
    if (raw == d->last_raw) {
        if (d->count < DEBOUNCE_SAMPLES) {
            d->count++;
        }
        if (d->count >= DEBOUNCE_SAMPLES) {
            d->stable = raw;
        }
    } else {
        d->last_raw = raw;
        d->count    = 1;
    }
    return d->stable;
}

/* ---------- Light control ---------- */
static void update_light(room_t *r, int headcount, uint32_t now_ms)
{
    if (headcount > 0) {
        r->zero_timer_running = false;
        if (!r->lit) {
            r->lit = true;
            gpio_set_level(r->pin_light, 1);
            ESP_LOGI(TAG, "Room %d: light ON", r->id);
        }
        return;
    }

    /* headcount is 0: start the grace timer, switch off only when it runs out */
    if (r->lit) {
        if (!r->zero_timer_running) {
            r->zero_timer_running = true;
            r->zero_since = now_ms;
        } else if ((now_ms - r->zero_since) >= LIGHT_OFF_DELAY_MS) {
            r->lit = false;
            r->zero_timer_running = false;
            gpio_set_level(r->pin_light, 0);
            ESP_LOGI(TAG, "Room %d: light OFF (room empty)", r->id);
        }
    }
}

/* ---------- One task per room ---------- */
static void room_task(void *arg)
{
    room_t *r = (room_t *)arg;
    beam_counter_init(&r->counter);

    for (;;) {
        bool a = debounce(&r->da, beam_cut(r->pin_a));
        bool b = debounce(&r->db, beam_cut(r->pin_b));
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        beam_event_t ev = beam_counter_update(&r->counter, a, b, now);

        if (ev == BEAM_EVENT_ENTER) {
            room_headcount_set(r->id, room_headcount_get(r->id) + 1);
            ESP_LOGI(TAG, "Room %d: someone ENTERED. People inside: %d",
                     r->id, room_headcount_get(r->id));
        } else if (ev == BEAM_EVENT_LEAVE) {
            int n = room_headcount_get(r->id);
            if (n > 0) {
                room_headcount_set(r->id, n - 1);
                ESP_LOGI(TAG, "Room %d: someone LEFT. People inside: %d",
                         r->id, room_headcount_get(r->id));
            } else {
                ESP_LOGW(TAG, "Room %d: leave seen but count is already 0 (ignored)", r->id);
            }
        }

        update_light(r, room_headcount_get(r->id), now);

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

/* ---------- Start-up of one room ---------- */
void room_start(int room_id, gpio_num_t pin_beam_a, gpio_num_t pin_beam_b, gpio_num_t pin_light)
{
    room_t *r = room_find(room_id);
    if (r == NULL) {
        ESP_LOGE(TAG, "Bad room id %d", room_id);
        return;
    }

    r->id        = room_id;
    r->pin_a     = pin_beam_a;
    r->pin_b     = pin_beam_b;
    r->pin_light = pin_light;
    r->headcount = 0;
    r->lit       = false;
    r->zero_timer_running = false;
    r->zero_since = 0;
    r->da = (debounce_t){ false, false, 0 };
    r->db = (debounce_t){ false, false, 0 };
    r->lock = xSemaphoreCreateMutex();

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << pin_beam_a) | (1ULL << pin_beam_b),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << pin_light),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_set_level(pin_light, 0);

    char name[16];
    snprintf(name, sizeof(name), "room%d_task", room_id);
    xTaskCreate(room_task, name, 4096, r, 5, NULL);

    ESP_LOGI(TAG, "Room %d started: beam A=GPIO%d, beam B=GPIO%d, light=GPIO%d",
             room_id, pin_beam_a, pin_beam_b, pin_light);
}