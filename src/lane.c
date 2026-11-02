#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "lane.h"
#include "registry.h"
#include "indicator.h"
#include "eventlog.h"
#include "schedule.h"

static const char *TAG = "lane";

/* ---------- Settings ---------- */
#define SAMPLE_MS           10      /* how often the beam is looked at              */
#define POLL_MS             100     /* how often the reader is looked at            */
#define MISSES_TO_RELEASE   3       /* polls with no card before a new tap counts   */
#define DOOR1_WAIT_MS       10000   /* door 1 stays open this long waiting for beam */
#define BEAM_MAX_HOLD_MS    3000    /* beam held longer than this = blocked         */
#define BEAM_CONFIRM_MS     1500    /* wait for a SECOND cut = second person        */
#define DOOR2_OPEN_MS       3000    /* door 2 stays open so the person can pass     */
#define ALARM_MS            5000    /* alarm length (beam alarms)                   */
#define CARD_ALARM_MS       3000    /* buzzer for a suspicious card                 */
#define TAP_QUEUE_LEN       4       /* virtual taps waiting for a lane              */

typedef struct {
    lane_config_t cfg;
    bool          present;          /* a card is being held on the reader */
    int           misses;
    QueueHandle_t tap_queue;        /* virtual taps from the menu */
} lane_t;

static lane_t *s_entry_lane = NULL;
static lane_t *s_exit_lane  = NULL;

/* ---------- The beam (a button in Wokwi: pressed = 0 = cut) ---------- */
static bool beam_cut_now(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

/* Cut seen twice, 10 ms apart (a simple debounce). */
static bool beam_cut(gpio_num_t pin)
{
    if (!beam_cut_now(pin)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    return beam_cut_now(pin);
}

/* Wait up to timeout_ms for the beam to be cut. true = it was cut. */
static bool wait_for_cut(gpio_num_t pin, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += SAMPLE_MS) {
        if (beam_cut(pin)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
    return false;
}

/* Wait up to timeout_ms for the beam to be clear. true = it was cleared. */
static bool wait_for_release(gpio_num_t pin, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += SAMPLE_MS) {
        if (!beam_cut_now(pin)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
    return false;
}

/* ---------- Signals ---------- */
static void signal_denied(void)
{
    led_set(LED_RED, true);
    buzzer_beep(400);
    led_set(LED_RED, false);
}

/* Which refusals are real alarms (NULL = just a refusal). */
static const char *alarm_reason(access_result_t res)
{
    switch (res) {
    case ACCESS_UNKNOWN_CARD:   return "unknown card";
    case ACCESS_BLOCKED:        return "blocked (lost) card used";
    case ACCESS_ALREADY_INSIDE: return "card used while owner already inside";
    case ACCESS_NOT_INSIDE:     return "exit tap but card not inside";
    case ACCESS_SUSPENDED:      return "suspended card used (reserve out)";
    default:                    return NULL;
    }
}

/* A suspicious card at the reader: buzzer and red light, and it is recorded. */
static void signal_card_alarm(const char *lane, const access_info_t *p, const char *reason)
{
    ESP_LOGE(TAG, "%s ALARM: %s (card %s of %s)", lane, reason, p->card_label, p->name);
    eventlog_add(EV_ALARM, p, reason);

    led_set(LED_RED, true);
    buzzer_set(true);
    vTaskDelay(pdMS_TO_TICKS(CARD_ALARM_MS));
    buzzer_set(false);
    led_set(LED_RED, false);
}

/* Door 2 stays shut, door 1 stays open so people can back out. Nobody counted. */
static void raise_alarm(lane_t *l, const access_info_t *p, const char *reason)
{
    const lane_config_t *c = &l->cfg;

    ESP_LOGE(TAG, "%s ALARM: %s (card %s of %s, %s)",
             c->name, reason, p->card_label, p->name, p->reg_no);
    eventlog_add(EV_ALARM, p, reason);

    door_close(c->door_second);          /* make sure the far door is shut */
    led_set(LED_GREEN, false);
    led_set(LED_RED, true);
    buzzer_set(true);

    vTaskDelay(pdMS_TO_TICKS(ALARM_MS));

    buzzer_set(false);
    led_set(LED_RED, false);
    door_close(c->door_first);
    led_set(LED_YELLOW, false);

    ESP_LOGW(TAG, "%s: alarm over, nobody was counted", c->name);
}

/* ---------- One person passing through ---------- */
static void run_pass(lane_t *l, const access_info_t *p)
{
    const lane_config_t *c = &l->cfg;

    if (c->is_entry) {
        ESP_LOGI(TAG, "%s: Welcome, %s (%s), card %s", c->name, p->name, p->reg_no, p->card_label);
    } else {
        ESP_LOGI(TAG, "%s: Goodbye, %s (%s), card %s", c->name, p->name, p->reg_no, p->card_label);
    }

    led_set(LED_GREEN, true);

    /* 1. door 1 opens */
    if (!door_open(c->door_first)) {
        ESP_LOGE(TAG, "%s: could not open door 1", c->name);
        led_set(LED_GREEN, false);
        return;
    }
    led_set(LED_YELLOW, true);
    ESP_LOGI(TAG, "%s: door 1 open - one person may pass, then cut the beam", c->name);

    /* 2. wait for the beam */
    if (!wait_for_cut(c->beam_pin, DOOR1_WAIT_MS)) {
        ESP_LOGW(TAG, "%s: nobody passed, door 1 closed, nobody counted", c->name);
        door_close(c->door_first);
        led_set(LED_YELLOW, false);
        led_set(LED_GREEN, false);
        return;
    }
    ESP_LOGI(TAG, "%s: beam cut", c->name);

    /* 3. the cut must end (someone standing in the beam = blocked) */
    if (!wait_for_release(c->beam_pin, BEAM_MAX_HOLD_MS)) {
        raise_alarm(l, p, "beam blocked too long");
        led_set(LED_GREEN, false);
        return;
    }

    /* 4. a second cut straight after = a second person */
    if (wait_for_cut(c->beam_pin, BEAM_CONFIRM_MS)) {
        raise_alarm(l, p, "beam cut twice: two people (tailgating)");
        led_set(LED_GREEN, false);
        return;
    }

    /* 5. exactly one person: close door 1, count, open door 2 */
    door_close(c->door_first);
    led_set(LED_YELLOW, false);

    registry_set_inside(p->uid, c->is_entry);
    eventlog_add(c->is_entry ? EV_ENTRY : EV_EXIT, p, c->name);
    ESP_LOGI(TAG, "%s: ONE person confirmed: %s (%s). People in the building: %d",
             c->name, p->name, p->reg_no, registry_count_inside());

    if (door_open(c->door_second)) {
        vTaskDelay(pdMS_TO_TICKS(DOOR2_OPEN_MS));
        door_close(c->door_second);
    }
    led_set(LED_GREEN, false);
    ESP_LOGI(TAG, "%s: ready for the next person", c->name);
}

/* ---------- A card was tapped ---------- */
static void handle_scan(lane_t *l, const rfid_uid_t *uid)
{
    const lane_config_t *c = &l->cfg;
    access_info_t p;

    access_result_t res = c->is_entry
        ? registry_check_entry(uid->bytes, &p)
        : registry_check_exit(uid->bytes, &p);

    if (res == ACCESS_UNKNOWN_CARD) {
        memset(&p, 0, sizeof(p));
        snprintf(p.card_label, sizeof(p.card_label), "?");
        snprintf(p.name, sizeof(p.name), "%02X:%02X:%02X:%02X",
                 uid->bytes[0], uid->bytes[1], uid->bytes[2], uid->bytes[3]);
    }

    if (res != ACCESS_GRANTED) {
        ESP_LOGW(TAG, "%s: ACCESS DENIED for %s (%s), card %s: %s",
                 c->name, p.name, p.reg_no, p.card_label, access_result_text(res));

        const char *why = alarm_reason(res);
        if (why != NULL) {
            signal_card_alarm(c->name, &p, why);
        } else {
            eventlog_add(EV_DENIED, &p, access_result_text(res));
            signal_denied();
        }
        return;
    }

    /* the card is fine, but is this reader allowed to work right now? */
    if (!schedule_lane_allowed(c->is_entry)) {
        ESP_LOGW(TAG, "%s: ACCESS DENIED for %s (%s), card %s: building closed",
                 c->name, p.name, p.reg_no, p.card_label);
        eventlog_add(EV_DENIED, &p, "building closed");
        signal_denied();
        return;
    }

    run_pass(l, &p);
}

/* ---------- The lane's own task ---------- */
static void lane_task(void *arg)
{
    lane_t *l = (lane_t *)arg;

    for (;;) {
        rfid_uid_t uid;

        if (xQueueReceive(l->tap_queue, &uid, 0) == pdTRUE) {
            /* a virtual tap from the menu */
            ESP_LOGI(TAG, "%s: virtual tap received", l->cfg.name);
            handle_scan(l, &uid);
        } else if (rfid_read_uid(l->cfg.reader, &uid)) {
            l->misses = 0;
            if (!l->present) {
                l->present = true;          /* one tap = one action */
                handle_scan(l, &uid);
            }
        } else if (l->present) {
            if (++l->misses >= MISSES_TO_RELEASE) {
                l->present = false;
                l->misses  = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* ---------- Start a lane ---------- */
bool lane_start(const lane_config_t *cfg)
{
    if (cfg == NULL || cfg->reader == NULL) {
        return false;
    }

    lane_t *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        return false;
    }
    l->cfg = *cfg;
    l->tap_queue = xQueueCreate(TAP_QUEUE_LEN, sizeof(rfid_uid_t));
    if (l->tap_queue == NULL) {
        free(l);
        return false;
    }

    if (cfg->is_entry) {
        s_entry_lane = l;
    } else {
        s_exit_lane = l;
    }

    gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << cfg->beam_pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in_cfg);

    char tname[16];
    snprintf(tname, sizeof(tname), "lane_%s", cfg->name);
    xTaskCreate(lane_task, tname, 5120, l, 5, NULL);

    ESP_LOGI(TAG, "%s lane started (beam on GPIO%d)", cfg->name, cfg->beam_pin);
    return true;
}

bool lane_virtual_tap(bool entry_lane, const uint8_t uid[4])
{
    lane_t *l = entry_lane ? s_entry_lane : s_exit_lane;
    if (l == NULL || l->tap_queue == NULL) {
        return false;
    }

    rfid_uid_t u;
    memcpy(u.bytes, uid, 4);
    return xQueueSend(l->tap_queue, &u, 0) == pdTRUE;
}