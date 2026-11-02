#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

#include "sdcard.h"
#include "wallclock.h"

static const char *TAG = "sdcard";

static bool           s_ready = false;
static sdmmc_card_t  *s_card  = NULL;

bool sdcard_init(gpio_num_t cs_pin)
{
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,     /* a blank card gets formatted (simulation only) */
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SPI2_HOST;          /* the bus the RFID readers already use */
    host.max_freq_khz = 4000;               /* slow and safe */

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = cs_pin;

    esp_err_t err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card NOT mounted (%s). The system still works, but nothing is saved.",
                 esp_err_to_name(err));
        s_ready = false;
        return false;
    }

    s_ready = true;
    uint64_t mb = ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024);
    ESP_LOGI(TAG, "SD card mounted: %s, %llu MB", s_card->cid.name, (unsigned long long)mb);
    return true;
}

bool sdcard_is_ready(void)
{
    return s_ready;
}

void sdcard_selftest(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "SELFTEST skipped: no card");
        return;
    }

    int boots = 0;
    FILE *f = fopen(SDCARD_MOUNT "/boot.txt", "r");
    if (f != NULL) {
        if (fscanf(f, "%d", &boots) != 1) {
            boots = 0;
        }
        fclose(f);
    }
    boots++;

    f = fopen(SDCARD_MOUNT "/boot.txt", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "SELFTEST failed: cannot write boot.txt");
        return;
    }
    fprintf(f, "%d\n", boots);
    fclose(f);

    char when[24];
    wallclock_format(when, sizeof(when));
    ESP_LOGI(TAG, "SELFTEST: this is start number %d at %s", boots, when);
    ESP_LOGI(TAG, "If this number grows after every stop and run, files survive restarts.");
}