#include "ml307_gnss_at_test.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/uart.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define ML307_AT_UART UART_NUM_1
#define ML307_AT_RX_BUF_SIZE 4096
#define ML307_GNSS_REPORT_INTERVAL_US (1000 * 1000)

typedef struct {
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    int baud_rate;
} ml307_gnss_at_test_config_t;

typedef struct {
    bool valid;
    double latitude;
    double longitude;
    int satellites;
    double hdop;
    char utc_time[16];
} ml307_gnss_fix_t;

static ml307_gnss_at_test_config_t g_cfg;
static ml307_gnss_at_test_report_callback_t g_report_callback = NULL;
static ml307_gnss_fix_t g_last_fix;
static int64_t g_last_report_us = 0;

static void copy_text(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static int split_nmea_fields(char *sentence, char *fields[], int max_fields) {
    int count = 0;
    char *cursor = sentence;

    if (sentence == NULL || fields == NULL || max_fields <= 0) {
        return 0;
    }

    while (count < max_fields) {
        fields[count++] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == NULL) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }

    return count;
}

static bool nmea_to_decimal_degrees(const char *raw, char hemisphere, double *out_value) {
    char *end_ptr = NULL;
    double nmea_value;
    int degrees;
    double minutes;
    double decimal_value;

    if (raw == NULL || raw[0] == '\0' || out_value == NULL) {
        return false;
    }

    nmea_value = strtod(raw, &end_ptr);
    if (end_ptr == raw) {
        return false;
    }

    degrees = (int)(nmea_value / 100.0);
    minutes = nmea_value - ((double)degrees * 100.0);
    decimal_value = (double)degrees + (minutes / 60.0);

    hemisphere = (char)toupper((unsigned char)hemisphere);
    if (hemisphere == 'S' || hemisphere == 'W') {
        decimal_value = -decimal_value;
    }

    *out_value = decimal_value;
    return true;
}

static void report_fix_if_needed(void) {
    char report[128];
    int64_t now_us;

    if (!g_last_fix.valid) {
        return;
    }

    now_us = esp_timer_get_time();
    if ((now_us - g_last_report_us) < ML307_GNSS_REPORT_INTERVAL_US) {
        return;
    }

    if (g_last_fix.satellites > 0 && g_last_fix.hdop > 0.0) {
        snprintf(report, sizeof(report), "Lat:%.6f Lon:%.6f S:%d H:%.1f",
                 g_last_fix.latitude, g_last_fix.longitude,
                 g_last_fix.satellites, g_last_fix.hdop);
    } else {
        snprintf(report, sizeof(report), "Lat:%.6f Lon:%.6f",
                 g_last_fix.latitude, g_last_fix.longitude);
    }

    printf("GPS FIX: %s\n", report);
    if (g_report_callback != NULL) {
        g_report_callback(report);
    }
    g_last_report_us = now_us;
}

static void parse_rmc_sentence(char *sentence) {
    char *fields[16] = {0};
    int field_count;
    double latitude;
    double longitude;

    field_count = split_nmea_fields(sentence, fields, 16);
    if (field_count < 7) {
        return;
    }
    if (fields[2] == NULL || fields[2][0] != 'A') {
        return;
    }
    if (!nmea_to_decimal_degrees(fields[3], fields[4][0], &latitude)) {
        return;
    }
    if (!nmea_to_decimal_degrees(fields[5], fields[6][0], &longitude)) {
        return;
    }

    g_last_fix.valid = true;
    g_last_fix.latitude = latitude;
    g_last_fix.longitude = longitude;
    copy_text(g_last_fix.utc_time, sizeof(g_last_fix.utc_time), fields[1]);
    report_fix_if_needed();
}

static void parse_gga_sentence(char *sentence) {
    char *fields[16] = {0};
    int field_count;
    double latitude;
    double longitude;

    field_count = split_nmea_fields(sentence, fields, 16);
    if (field_count < 9) {
        return;
    }
    if (fields[6] == NULL || fields[6][0] == '\0' || fields[6][0] == '0') {
        return;
    }
    if (!nmea_to_decimal_degrees(fields[2], fields[3][0], &latitude)) {
        return;
    }
    if (!nmea_to_decimal_degrees(fields[4], fields[5][0], &longitude)) {
        return;
    }

    g_last_fix.valid = true;
    g_last_fix.latitude = latitude;
    g_last_fix.longitude = longitude;
    g_last_fix.satellites = atoi(fields[7]);
    g_last_fix.hdop = strtod(fields[8], NULL);
    copy_text(g_last_fix.utc_time, sizeof(g_last_fix.utc_time), fields[1]);
    report_fix_if_needed();
}

static void parse_nmea_sentence(const char *line) {
    char sentence[256];
    char *checksum = NULL;

    if (line == NULL || line[0] != '$') {
        return;
    }

    copy_text(sentence, sizeof(sentence), line);
    checksum = strchr(sentence, '*');
    if (checksum != NULL) {
        *checksum = '\0';
    }
    if (strlen(sentence) < 6) {
        return;
    }

    if (strncmp(sentence + 3, "RMC", 3) == 0) {
        parse_rmc_sentence(sentence);
    } else if (strncmp(sentence + 3, "GGA", 3) == 0) {
        parse_gga_sentence(sentence);
    }
}

static bool response_complete(const char *buf, size_t len) {
    if (len == 0 || buf == NULL) {
        return false;
    }
    if (strstr(buf, "\r\nOK\r\n") != NULL) {
        return true;
    }
    if (strstr(buf, "\r\nERROR\r\n") != NULL) {
        return true;
    }
    if (strstr(buf, "+CME ERROR:") != NULL) {
        return true;
    }
    return false;
}

static void send_cmd_and_print(const char *cmd, int timeout_ms) {
    char resp[1024];
    size_t total = 0;
    resp[0] = '\0';

    uart_flush_input(ML307_AT_UART);
    uart_write_bytes(ML307_AT_UART, cmd, strlen(cmd));  // 把命令发出去，下面超时等待

    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        int len = uart_read_bytes(ML307_AT_UART, (uint8_t *)&resp[total],
                                  sizeof(resp) - 1 - total, pdMS_TO_TICKS(200)); // 缓冲区这次还剩多少字节可以安全写入
        if (len > 0) {
            total += (size_t)len;
            resp[total] = '\0';
            if (response_complete(resp, total)) {
                break;
            }
        }
    }

    printf("AT[%s] ->\n%s\n", cmd, resp[0] ? resp : "(no response)");
}

static void stream_and_print_lines(void) {
    char line[256];
    size_t line_len = 0;
    uint8_t rx_buf[128];

    while (true) {
        int len = uart_read_bytes(ML307_AT_UART, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(200));
        if (len <= 0) {
            continue;
        }
        for (int i = 0; i < len; ++i) {
            char ch = (char)rx_buf[i];
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    printf("RX: %s\n", line);
                    parse_nmea_sentence(line);
                    line_len = 0;
                }
                continue;
            }
            if (line_len < sizeof(line) - 1) {
                line[line_len++] = ch;
            } else {
                line_len = 0;
            }
        }
    }
}

static void ml307_gnss_at_test_task(void *arg) {
    const ml307_gnss_at_test_config_t *cfg = (const ml307_gnss_at_test_config_t *)arg;
    if (cfg == NULL) {
        vTaskDelete(NULL);
        return;
    }

    if (!uart_is_driver_installed(ML307_AT_UART)) {
        uart_config_t uart_config = {
            .baud_rate = cfg->baud_rate,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        uart_driver_install(ML307_AT_UART, ML307_AT_RX_BUF_SIZE, 0, 0, NULL, 0);
        uart_param_config(ML307_AT_UART, &uart_config);
        uart_set_pin(ML307_AT_UART, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    send_cmd_and_print("AT+MGNSSCFG=\"nmea/mask\",63\r\n", 1000);
    vTaskDelay(pdMS_TO_TICKS(200));
    send_cmd_and_print("AT+MGNSSLOC=1\r\n", 1000);
    vTaskDelay(pdMS_TO_TICKS(200));
    send_cmd_and_print("AT+MGNSS=1\r\n", 1000);
    stream_and_print_lines();
}

void ml307_gnss_at_test_set_report_callback(ml307_gnss_at_test_report_callback_t callback) {
    g_report_callback = callback;
}

void ml307_gnss_at_test_start(gpio_num_t tx_pin, gpio_num_t rx_pin, int baud_rate) {
    g_cfg.tx_pin = tx_pin;
    g_cfg.rx_pin = rx_pin;
    g_cfg.baud_rate = baud_rate;
    memset(&g_last_fix, 0, sizeof(g_last_fix));
    g_last_report_us = 0;
    vTaskDelay(pdMS_TO_TICKS(100));
    xTaskCreate(ml307_gnss_at_test_task, "ml307_gnss_at_test", 4096, &g_cfg, 5, NULL);
}
