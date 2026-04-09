#ifndef __WS1860_IIC_H__
#define __WS1860_IIC_H__


#include "nfc.h"
#include "stdio.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"


#define NFC_RESET_PIN    GPIO_NUM_14
#define NFC_IRQ_PIN      GPIO_NUM_48


#define WS_GPIO_OUTPUT_PIN_SEL ((1ULL << NFC_RESET_PIN))
#define WS_GPIO_INTPUT_PIN_SEL ((1ULL << NFC_IRQ_PIN))



#define SET_NFC_RST   gpio_set_level(NFC_RESET_PIN, 1);

#define CLR_NFC_RST   gpio_set_level(NFC_RESET_PIN, 0);


void ws_iic_init(i2c_master_bus_handle_t bus_handle);

unsigned char IIC_ReadOneByte(unsigned char Address);
void SetBitMask(unsigned char reg,unsigned char mask);
void ClearBitMask(unsigned char reg,unsigned char mask);
void WriteRawRC(unsigned char Address, unsigned char value);
unsigned char ReadRawRC(unsigned char Address);

#endif