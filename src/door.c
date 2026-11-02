#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "door.h"

static const char *TAG = "door";

/* Servo pulse lengths: 0.5 ms = 0 degrees, 1.5 ms = 90 degrees. */
#define PULSE_CLOSED_US   500
#define PULSE_OPEN_US     1500

/* PWM: 50 Hz (one pulse every 20 ms), 14-bit counter (16384 steps). */
#define SERVO_FREQ_HZ     50
#define SERVO_RES_BITS    LEDC_TIMER_14_BIT
#define SERVO_PERIOD_US   20000
#define SERVO_STEPS       16384

static const gpio_num_t s_pin[DOOR_COUNT] = {
    GPIO_NUM_15,    /* DOOR_ENTRY_1 */
    GPIO_NUM_16,    /* DOOR_ENTRY_2 */
    GPIO_NUM_17,    /* DOOR_EXIT_1  */
    GPIO_NUM_18,    /* DOOR_EXIT_2  */
};

static const char *s_name[DOOR_COUNT] = {
    "entry door 1 (outside)",
    "entry door 2 (inside)",
    "exit door 1 (inside)",
    "exit door 2 (outside)",
};

static bool              s_open[DOOR_COUNT];
static SemaphoreHandle_t s_lock = NULL;

/* Turn a pulse length (microseconds) into the PWM duty number. */
static uint32_t pulse_to_duty(uint32_t pulse_us)
{
    return (pulse_us * SERVO_STEPS) / SERVO_PERIOD_US;
}

static void servo_move(door_id_t d, uint32_t pulse_us)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)d, pulse_to_duty(pulse_us));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)d);
}

void doors_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_RES_BITS,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    for (int i = 0; i < DOOR_COUNT; i++) {
        ledc_channel_config_t ch = {
            .gpio_num   = s_pin[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = (ledc_channel_t)i,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = pulse_to_duty(PULSE_CLOSED_US),
            .hpoint     = 0,
        };
        ledc_channel_config(&ch);
        s_open[i] = false;
    }

    ESP_LOGI(TAG, "4 doors ready (all closed)");
}

bool door_open(door_id_t d)
{
    if (d < 0 || d >= DOOR_COUNT) {
        return false;
    }

    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    door_id_t other = (door_id_t)(d ^ 1);
    if (s_open[other]) {
        ESP_LOGE(TAG, "REFUSED: %s cannot open while %s is open",
                 s_name[d], s_name[other]);
    } else {
        servo_move(d, PULSE_OPEN_US);
        s_open[d] = true;
        ESP_LOGI(TAG, "%s OPEN", s_name[d]);
        ok = true;
    }

    xSemaphoreGive(s_lock);
    return ok;
}

void door_close(door_id_t d)
{
    if (d < 0 || d >= DOOR_COUNT) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    servo_move(d, PULSE_CLOSED_US);
    if (s_open[d]) {
        ESP_LOGI(TAG, "%s CLOSED", s_name[d]);
    }
    s_open[d] = false;
    xSemaphoreGive(s_lock);
}

void doors_close_all(void)
{
    for (int i = 0; i < DOOR_COUNT; i++) {
        door_close((door_id_t)i);
    }
}

bool door_is_open(door_id_t d)
{
    if (d < 0 || d >= DOOR_COUNT) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_open[d];
    xSemaphoreGive(s_lock);
    return v;
}