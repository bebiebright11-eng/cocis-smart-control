#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "store.h"
#include "sdcard.h"
#include "registry.h"
#include "room_task.h"
#include "eventlog.h"
#include "schedule.h"

#define ROOMS_FILE  SDCARD_MOUNT "/rooms.txt"
#define SAVE_EVERY_MS 1000

static const char *TAG = "store";

/* what is saved on the card right now (index 1 and 2 = Room 1 and Room 2) */
static int s_saved[3] = { -1, -1, -1 };

void store_load_rooms(void)
{
    if (!sdcard_is_ready()) {
        return;
    }

    FILE *f = fopen(ROOMS_FILE, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "no saved room headcounts yet");
        return;
    }

    int r1 = -1, r2 = -1;
    int n = fscanf(f, "ROOMS|%d|%d", &r1, &r2);
    fclose(f);

    if (n != 2 || r1 < 0 || r1 > 500 || r2 < 0 || r2 > 500) {
        ESP_LOGW(TAG, "the saved room headcounts are damaged: ignoring them");
        return;
    }

    room_headcount_set(1, r1);
    room_headcount_set(2, r2);
    s_saved[1] = r1;
    s_saved[2] = r2;
    ESP_LOGI(TAG, "room headcounts LOADED from the SD card: Room 1 = %d, Room 2 = %d", r1, r2);
}

static void save_rooms_if_changed(void)
{
    int r1 = room_headcount_get(1);
    int r2 = room_headcount_get(2);

    if (r1 == s_saved[1] && r2 == s_saved[2]) {
        return;
    }

    FILE *f = fopen(ROOMS_FILE, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "could not save the room headcounts");
        return;
    }
    fprintf(f, "ROOMS|%d|%d\n", r1, r2);
    fclose(f);

    s_saved[1] = r1;
    s_saved[2] = r2;
    ESP_LOGI(TAG, "room headcounts saved: Room 1 = %d, Room 2 = %d", r1, r2);
}

static void store_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAVE_EVERY_MS));
        registry_save_if_dirty();
        save_rooms_if_changed();
        schedule_save_if_dirty();
        eventlog_flush();
    }
}

void store_start(void)
{
    if (!sdcard_is_ready()) {
        ESP_LOGW(TAG, "no SD card: nothing will be saved");
        return;
    }
    xTaskCreate(store_task, "store_task", 5120, NULL, 2, NULL);
    ESP_LOGI(TAG, "saving to the SD card is on");
}