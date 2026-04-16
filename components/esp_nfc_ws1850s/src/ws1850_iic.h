#ifndef __WS1850_IIC_H__
#define __WS1850_IIC_H__

#include "nfc.h"
#include <stdio.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"

// GPIO 引脚由外部配置，不再硬编码
extern gpio_num_t g_nfc_reset_pin;
extern gpio_num_t g_nfc_irq_pin;

#define SET_NFC_RST   do { if (g_nfc_reset_pin != GPIO_NUM_NC) gpio_set_level(g_nfc_reset_pin, 1); } while(0)
#define CLR_NFC_RST   do { if (g_nfc_reset_pin != GPIO_NUM_NC) gpio_set_level(g_nfc_reset_pin, 0); } while(0)

void ws_iic_init(i2c_master_bus_handle_t bus_handle);
void ws_iic_set_pins(gpio_num_t reset_pin, gpio_num_t irq_pin);

unsigned char IIC_ReadOneByte(unsigned char Address);
void SetBitMask(unsigned char reg, unsigned char mask);
void ClearBitMask(unsigned char reg, unsigned char mask);
void WriteRawRC(unsigned char Address, unsigned char value);
unsigned char ReadRawRC(unsigned char Address);

#endif
