#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/i2c.h"
#include "esp_log.h"

#include "wallclock.h"

static const char *TAG = "wallclock";

#define RTC_SDA_PIN   35
#define RTC_SCL_PIN   36
#define RTC_PORT      I2C_NUM_0
#define RTC_ADDR      0x68        /* DS1307 */
#define RTC_TIMEOUT   pdMS_TO_TICKS(100)

static bool s_is_set  = false;
static bool s_rtc_ok  = false;    /* the I2C bus started */

static uint8_t to_bcd(int v)     { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int     from_bcd(uint8_t b) { return ((b >> 4) * 10) + (b & 0x0F); }

/* Read the chip. false = chip missing, stopped, or holding nonsense. */
static bool rtc_read(struct tm *t)
{
    uint8_t reg = 0x00;
    uint8_t d[7];

    if (i2c_master_write_read_device(RTC_PORT, RTC_ADDR, &reg, 1, d, 7, RTC_TIMEOUT) != ESP_OK) {
        return false;
    }
    if (d[0] & 0x80) {
        return false;             /* clock-halt bit: the chip is stopped */
    }

    memset(t, 0, sizeof(*t));
    t->tm_sec  = from_bcd(d[0] & 0x7F);
    t->tm_min  = from_bcd(d[1] & 0x7F);
    t->tm_hour = from_bcd(d[2] & 0x3F);
    t->tm_mday = from_bcd(d[4] & 0x3F);
    t->tm_mon  = from_bcd(d[5] & 0x1F) - 1;
    t->tm_year = from_bcd(d[6]) + 100;      /* chip holds 00-99 = 2000-2099 */

    if (t->tm_year + 1900 < 2024 || t->tm_mon < 0 || t->tm_mon > 11 ||
        t->tm_mday < 1 || t->tm_mday > 31 || t->tm_hour > 23 ||
        t->tm_min > 59 || t->tm_sec > 59) {
        return false;
    }
    return true;
}

static bool rtc_write(const struct tm *t)
{
    uint8_t d[8];
    d[0] = 0x00;                              /* start at the seconds register */
    d[1] = to_bcd(t->tm_sec) & 0x7F;          /* bit 7 = 0: the clock runs */
    d[2] = to_bcd(t->tm_min);
    d[3] = to_bcd(t->tm_hour);                /* 24-hour mode */
    d[4] = (uint8_t)(t->tm_wday + 1);
    d[5] = to_bcd(t->tm_mday);
    d[6] = to_bcd(t->tm_mon + 1);
    d[7] = to_bcd(t->tm_year - 100);

    return i2c_master_write_to_device(RTC_PORT, RTC_ADDR, d, sizeof(d), RTC_TIMEOUT) == ESP_OK;
}

void wallclock_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = RTC_SDA_PIN,
        .scl_io_num       = RTC_SCL_PIN,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    if (i2c_param_config(RTC_PORT, &cfg) != ESP_OK ||
        i2c_driver_install(RTC_PORT, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed to start: the RTC cannot be used");
        return;
    }
    s_rtc_ok = true;

    struct tm t;
    if (rtc_read(&t)) {
        t.tm_isdst = 0;
        time_t secs = mktime(&t);
        struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_is_set = true;
        ESP_LOGI(TAG, "RTC time loaded: %04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        ESP_LOGW(TAG, "RTC has no valid time yet. Set the clock from the menu once.");
    }
}

bool wallclock_set(int year, int month, int day, int hour, int minute)
{
    if (year < 2024 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59) {
        return false;
    }

    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year  = year - 1900;
    t.tm_mon   = month - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = minute;
    t.tm_isdst = 0;

    time_t secs = mktime(&t);
    if (secs == (time_t)-1) {
        return false;
    }

    struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_is_set = true;

    if (s_rtc_ok) {
        struct tm back;
        localtime_r(&secs, &back);            /* fills the weekday */
        if (rtc_write(&back)) {
            ESP_LOGI(TAG, "time written into the RTC chip");
        } else {
            ESP_LOGW(TAG, "could not write to the RTC chip");
        }
    }
    return true;
}

bool wallclock_is_set(void)
{
    return s_is_set;
}

void wallclock_get(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
}

void wallclock_format(char *buf, size_t len)
{
    if (!s_is_set) {
        snprintf(buf, len, "(time not set)");
        return;
    }
    struct tm t;
    wallclock_get(&t);
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}