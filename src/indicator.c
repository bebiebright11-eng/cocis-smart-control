#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "indicator.h"

#define BUZZER_PIN      GPIO_NUM_21
#define BUZZER_FREQ_HZ  2000
#define BUZZER_DUTY_ON  512      /* half of 1024 (10-bit) = a clear tone */

static const gpio_num_t s_led_pin[] = {
    GPIO_NUM_40,    /* LED_GREEN  */
    GPIO_NUM_41,    /* LED_RED    */
    GPIO_NUM_42,    /* LED_YELLOW */
};

void indicators_init(void)
{
    for (int i = 0; i < 3; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_led_pin[i],
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&cfg);
        gpio_set_level(s_led_pin[i], 0);
    }

    /* The buzzer needs a tone, so it uses PWM (a second timer, 2 kHz). */
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = BUZZER_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = BUZZER_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_4,      /* channels 0-3 are the doors */
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
}

void led_set(led_t led, bool on)
{
    gpio_set_level(s_led_pin[led], on ? 1 : 0);
}

void buzzer_set(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, on ? BUZZER_DUTY_ON : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4);
}

void buzzer_beep(int ms)
{
    buzzer_set(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    buzzer_set(false);
}