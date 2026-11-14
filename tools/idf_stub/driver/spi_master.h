#pragma once
#include "../esp_err.h"
#include <stdint.h>

typedef int spi_host_device_t;
#define SPI2_HOST 2
typedef int spi_dma_chan_t;
#define SPI_DMA_CH_AUTO 3

typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int data4_io_num, data5_io_num, data6_io_num, data7_io_num;
    int max_transfer_sz;
    uint32_t flags;
    int intr_flags;
} spi_bus_config_t;

typedef struct spi_device_t *spi_device_handle_t;

typedef struct spi_transaction_t spi_transaction_t;
typedef void (*transaction_cb_t)(spi_transaction_t *trans);

typedef struct {
    uint8_t command_bits, address_bits, dummy_bits, mode;
    int clock_source;
    uint16_t duty_cycle_pos, cs_ena_pretrans;
    uint8_t cs_ena_posttrans;
    int clock_speed_hz, input_delay_ns, sample_point, spics_io_num;
    uint32_t flags;
    int queue_size;
    transaction_cb_t pre_cb;
    transaction_cb_t post_cb;
} spi_device_interface_config_t;

typedef struct spi_transaction_t {
    uint32_t flags;
    size_t cmd, addr, length, rxlength;
    void *user;
    const void *tx_buffer;
    void *rx_buffer;
    uint8_t tx_data[4];
} spi_transaction_t;

#define SPI_TRANS_USE_TXDATA (1 << 5)

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

static inline esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *b, spi_dma_chan_t d) { (void)h;(void)b;(void)d; return ESP_OK; }
static inline esp_err_t spi_bus_add_device(spi_host_device_t h, const spi_device_interface_config_t *c, spi_device_handle_t *out) { (void)h;(void)c;(void)out; return ESP_OK; }
static inline esp_err_t spi_bus_remove_device(spi_device_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t) { (void)h;(void)t; return ESP_OK; }
static inline esp_err_t spi_device_queue_trans(spi_device_handle_t h, spi_transaction_t *t, int ticks) { (void)h;(void)t;(void)ticks; return ESP_OK; }
static inline esp_err_t spi_device_get_trans_result(spi_device_handle_t h, spi_transaction_t **out, int ticks) { (void)h;(void)out;(void)ticks; return ESP_OK; }
