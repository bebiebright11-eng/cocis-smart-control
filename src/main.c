#include "esp_log.h"

#include "room_task.h"
#include "rfid.h"
#include "registry.h"
#include "door.h"
#include "indicator.h"
#include "lane.h"
#include "menu.h"
#include "wallclock.h"
#include "sdcard.h"
#include "store.h"
#include "eventlog.h"
#include "schedule.h"
#include "alerts.h"

static const char *TAG = "main";

void app_main(void)
{
    /*            room  beam A       beam B       light      */
    room_start(1, GPIO_NUM_6,  GPIO_NUM_7,  GPIO_NUM_1);
    room_start(2, GPIO_NUM_38, GPIO_NUM_39, GPIO_NUM_2);

    wallclock_init();       /* read the time from the RTC chip */
    registry_init();        /* the sample data (replaced below if a saved copy exists) */
    schedule_init();        /* default opening hours 07:00 to 18:00 */
    doors_init();
    indicators_init();

    /* shared SPI wires: SCK=12, MOSI=11, MISO=13 */
    rfid_bus_init(GPIO_NUM_12, GPIO_NUM_11, GPIO_NUM_13);

    rfid_reader_t entry_reader = rfid_reader_add(GPIO_NUM_10, GPIO_NUM_8);   /* CS 10, RST 8  */
    rfid_reader_t exit_reader  = rfid_reader_add(GPIO_NUM_9,  GPIO_NUM_14);  /* CS 9,  RST 14 */

    if (entry_reader == NULL || exit_reader == NULL) {
        ESP_LOGE(TAG, "a reader failed to start");
        return;
    }

    /* the SD card shares the SPI wires; its own chip-select is GPIO 37 */
    sdcard_init(GPIO_NUM_37);
    sdcard_selftest();

    registry_load_from_sd();    /* people, cards, who is inside */
    store_load_rooms();         /* headcounts of Room 1 and Room 2 */
    schedule_load();            /* opening hours and alarm saved by the guard */
    eventlog_init();            /* the history of entries, exits, alarms... */
    store_start();              /* from now on, changes are saved automatically */
    schedule_start();           /* the closing alarm watcher */
    alerts_start();             /* rooms versus building watcher */

    lane_config_t entry = {
        .name        = "ENTRY",
        .is_entry    = true,
        .reader      = entry_reader,
        .beam_pin    = GPIO_NUM_4,
        .door_first  = DOOR_ENTRY_1,     /* outside door */
        .door_second = DOOR_ENTRY_2,     /* inside door  */
    };

    lane_config_t exit_lane = {
        .name        = "EXIT",
        .is_entry    = false,
        .reader      = exit_reader,
        .beam_pin    = GPIO_NUM_5,
        .door_first  = DOOR_EXIT_1,      /* inside door  */
        .door_second = DOOR_EXIT_2,      /* outside door */
    };

    lane_start(&entry);
    lane_start(&exit_lane);

    menu_start();
}