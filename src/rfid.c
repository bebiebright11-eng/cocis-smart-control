#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "rfid.h"

static const char *TAG = "rfid";

/* ---------- Chip registers (the chip's "settings drawers") ---------- */
#define REG_COMMAND    0x01
#define REG_COM_IRQ    0x04
#define REG_ERROR      0x06
#define REG_FIFO_DATA  0x09
#define REG_FIFO_LVL   0x0A
#define REG_BIT_FRAME  0x0D
#define REG_COLL       0x0E
#define REG_MODE       0x11
#define REG_TX_CTRL    0x14
#define REG_TX_ASK     0x15
#define REG_T_MODE     0x2A
#define REG_T_PRESC    0x2B
#define REG_T_RELOAD_H 0x2C
#define REG_T_RELOAD_L 0x2D
#define REG_VERSION    0x37

#define CMD_IDLE       0x00
#define CMD_TRANSCEIVE 0x0C
#define CMD_SOFT_RESET 0x0F

#define RFID_HOST      SPI2_HOST
#define FIFO_MAX_READ  16

struct rfid_reader {
    spi_device_handle_t spi;
    gpio_num_t          rst;
};

/* ---------- Talking to the chip over SPI ---------- */
static void reg_write(rfid_reader_t r, uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {
        .length  = 16,
        .flags   = SPI_TRANS_USE_TXDATA,
        .tx_data = { (uint8_t)((reg << 1) & 0x7E), val },
    };
    spi_device_polling_transmit(r->spi, &t);
}

static uint8_t reg_read(rfid_reader_t r, uint8_t reg)
{
    spi_transaction_t t = {
        .length  = 16,
        .flags   = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 },
    };
    spi_device_polling_transmit(r->spi, &t);
    return t.rx_data[1];
}

static void reg_set_mask(rfid_reader_t r, uint8_t reg, uint8_t mask)
{
    reg_write(r, reg, reg_read(r, reg) | mask);
}

static void reg_clr_mask(rfid_reader_t r, uint8_t reg, uint8_t mask)
{
    reg_write(r, reg, reg_read(r, reg) & (uint8_t)~mask);
}

/* Read n bytes from the FIFO in ONE SPI message:
 * send the read-address n times, then a 0; the answers come back one
 * byte later (rx[0] is junk, rx[1..n] is the data). */
static void fifo_read(rfid_reader_t r, uint8_t *dst, uint8_t n)
{
    if (n == 0) {
        return;
    }
    if (n > FIFO_MAX_READ) {
        n = FIFO_MAX_READ;
    }

    uint8_t tx[FIFO_MAX_READ + 1];
    uint8_t rx[FIFO_MAX_READ + 1];
    memset(tx, ((REG_FIFO_DATA << 1) & 0x7E) | 0x80, n);
    tx[n] = 0x00;
    memset(rx, 0, sizeof(rx));

    spi_transaction_t t = {
        .length    = (size_t)(n + 1) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(r->spi, &t);
    memcpy(dst, &rx[1], n);
}

/* ---------- Send bytes to the card, collect its answer ---------- */
static bool transceive(rfid_reader_t r, const uint8_t *tx, uint8_t tx_len,
                       uint8_t last_bits, uint8_t *rx, uint8_t *rx_len)
{
    reg_write(r, REG_COMMAND, CMD_IDLE);
    reg_write(r, REG_COM_IRQ, 0x7F);          /* clear old flags */
    reg_write(r, REG_FIFO_LVL, 0x80);         /* empty the FIFO  */
    for (uint8_t i = 0; i < tx_len; i++) {
        reg_write(r, REG_FIFO_DATA, tx[i]);
    }
    reg_write(r, REG_BIT_FRAME, last_bits);
    reg_write(r, REG_COMMAND, CMD_TRANSCEIVE);
    reg_set_mask(r, REG_BIT_FRAME, 0x80);     /* start sending */

    /* wait at most about 30 ms (60 checks, 0.5 ms apart) */
    bool done = false;
    for (int i = 0; i < 60; i++) {
        uint8_t irq = reg_read(r, REG_COM_IRQ);
        if (irq & 0x30) { done = true; break; }   /* answer received */
        if (irq & 0x01) { break; }                /* chip timer ran out: no card */
        esp_rom_delay_us(500);
    }
    reg_clr_mask(r, REG_BIT_FRAME, 0x80);

    if (!done) {
        return false;
    }
    if (reg_read(r, REG_ERROR) & 0x13) {          /* parity / protocol / overflow */
        return false;
    }

    uint8_t n = reg_read(r, REG_FIFO_LVL);
    if (n > *rx_len) {
        n = *rx_len;
    }
    fifo_read(r, rx, n);
    *rx_len = n;
    return true;
}

/* "Is anyone there?"  (card answers with 2 bytes) */
static bool card_request(rfid_reader_t r)
{
    uint8_t cmd = 0x26;                           /* REQA */
    uint8_t atqa[2];
    uint8_t len = sizeof(atqa);
    reg_clr_mask(r, REG_COLL, 0x80);
    return transceive(r, &cmd, 1, 7, atqa, &len) && len == 2;
}

/* "Tell me your ID."  (card answers with 4 ID bytes + 1 check byte) */
static bool card_get_id(rfid_reader_t r, uint8_t id[4])
{
    uint8_t cmd[2] = { 0x93, 0x20 };
    uint8_t buf[5] = { 0 };
    uint8_t len = sizeof(buf);
    if (!transceive(r, cmd, 2, 0, buf, &len) || len != 5) {
        return false;
    }
    if ((buf[0] ^ buf[1] ^ buf[2] ^ buf[3]) != buf[4]) {
        return false;                             /* check byte wrong: bad read */
    }
    memcpy(id, buf, 4);
    return true;
}

/* ---------- Public functions ---------- */
bool rfid_bus_init(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso)
{
    spi_bus_config_t bus = {
        .sclk_io_num     = sck,
        .mosi_io_num     = mosi,
        .miso_io_num     = miso,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t err = spi_bus_initialize(RFID_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

rfid_reader_t rfid_reader_add(gpio_num_t cs, gpio_num_t rst)
{
    rfid_reader_t r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->rst = rst;

    /* hardware reset pulse on the RST pin */
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << rst,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 1000000,
        .mode           = 0,
        .spics_io_num   = cs,
        .queue_size     = 1,
    };
    if (spi_bus_add_device(RFID_HOST, &dev, &r->spi) != ESP_OK) {
        ESP_LOGE(TAG, "could not add reader (CS=%d)", cs);
        free(r);
        return NULL;
    }

    reg_write(r, REG_COMMAND, CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* chip settings: a 25 ms answer timer, 100% ASK, CRC preset */
    reg_write(r, REG_T_MODE,     0x80);
    reg_write(r, REG_T_PRESC,    0xA9);
    reg_write(r, REG_T_RELOAD_H, 0x03);
    reg_write(r, REG_T_RELOAD_L, 0xE8);
    reg_write(r, REG_TX_ASK,     0x40);
    reg_write(r, REG_MODE,       0x3D);

    /* antenna on */
    if ((reg_read(r, REG_TX_CTRL) & 0x03) != 0x03) {
        reg_set_mask(r, REG_TX_CTRL, 0x03);
    }

    ESP_LOGI(TAG, "reader added: CS=GPIO%d RST=GPIO%d chip version=0x%02X",
             cs, rst, reg_read(r, REG_VERSION));
    return r;
}

bool rfid_read_uid(rfid_reader_t r, rfid_uid_t *uid)
{
    if (r == NULL || uid == NULL) {
        return false;
    }
    if (!card_request(r)) {
        return false;                             /* nobody there */
    }
    return card_get_id(r, uid->bytes);
}