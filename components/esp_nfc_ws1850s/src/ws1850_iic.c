#include <stdio.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "ws1850_iic.h"

#define I2C_NFC_TIMEOUT_VALUE_MS (100)

// 可配置的 GPIO 引脚（由 C++ 层通过 ws_iic_set_pins 设置）
gpio_num_t g_nfc_reset_pin = GPIO_NUM_NC;
gpio_num_t g_nfc_irq_pin = GPIO_NUM_NC;

void ws_iic_set_pins(gpio_num_t reset_pin, gpio_num_t irq_pin) {
    g_nfc_reset_pin = reset_pin;
    g_nfc_irq_pin = irq_pin;
}

static i2c_master_bus_handle_t ws_bus_handel;
static i2c_master_dev_handle_t nfc_dev_handle;

static uint32_t i2c_frequency = 100 * 1000;


void ws_iic_detect(void) // 扫描iic总线0设备
{

  for (uint8_t addr = 0x00; addr < 0x7F; addr++)
  {
    if (i2c_master_probe(ws_bus_handel, addr, 100) == ESP_OK)
    {
      printf("Found device at 0x%02X\n", addr);
    }
  }

  if (i2c_master_probe(ws_bus_handel, 0X28, 100) == ESP_OK)
  {
    printf("NFC device read succ...\n");
  }
  else
  {
    printf("NFC device read fail...\n");
  }
}

void WS1850_iic_dev_init(void)
{
  // NFC芯片
  i2c_device_config_t i2c_nfc_dev_conf = {
      .scl_speed_hz = i2c_frequency,
      .device_address = 0X28,
  };

  if (i2c_master_bus_add_device(ws_bus_handel, &i2c_nfc_dev_conf, &nfc_dev_handle) != ESP_OK)
  {
    printf("NFC device add failed\n");
    return;
  }
}

void ws_iic_init(i2c_master_bus_handle_t bus_handle)
{
  if (bus_handle == NULL)
  {
    ESP_LOGE("ws_iic", "I2C bus handle is null, NFC init aborted");
    return;
  }

  ws_bus_handel = bus_handle;
  WS1850_iic_dev_init();
  // ws_iic_detect();
}


#if 0
esp_err_t ws_write_reg(uint8_t reg, uint8_t val)
{
  uint8_t buf[2] = {reg, val};

  return i2c_master_transmit(
      nfc_dev_handle,
      buf,
      sizeof(buf),
      I2C_NFC_TIMEOUT_VALUE_MS);
}

uint8_t ws_read_reg(uint8_t reg)
{
  uint8_t val = 0;
  esp_err_t ret;

  ret = i2c_master_transmit_receive(
      nfc_dev_handle,
      &reg,
      1,
      &val,
      1,
      I2C_NFC_TIMEOUT_VALUE_MS);

  if (ret != ESP_OK)
  {
    return 0;
  }

  return val;
}

#else

esp_err_t ws_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };

    esp_err_t err=i2c_master_transmit(
        nfc_dev_handle,
        buf,
        2,
        I2C_NFC_TIMEOUT_VALUE_MS);

    if (err !=ESP_OK)
    {
      ESP_LOGE("ws write","sent error, reg:0X%02X",reg);
    }
    return err;
}

uint8_t ws_read_reg(uint8_t reg)
{
    uint8_t val = 0;

    esp_err_t ret = i2c_master_transmit_receive(
        nfc_dev_handle,
        &reg,
        1,
        &val,
        1,
        I2C_NFC_TIMEOUT_VALUE_MS);

    if (ret != ESP_OK)
    {
       ESP_LOGE("ws read","sent error, reg:0X%02X",reg);
       return 0xFF;
    }

    return val;
}

#endif


/**
 ****************************************************************
 * @brief SetBitMask() 
 *
 * 将寄存器的某些bit位值1
 *
 * @param: reg 寄存器地址
 * @param: mask 需要置位的bit位
 ****************************************************************
 */
void SetBitMask(unsigned char reg, unsigned char mask)
{
  char tmp = 0x0;
  tmp = ws_read_reg(reg);
  esp_err_t err = ws_write_reg(reg, tmp | mask); // set bit mask
  // printf("SetBitMask ret: %d", err);
}

/**
 ****************************************************************
 * @brief ClearBitMask() 
 *
 * 将寄存器的某些bit位清0
 *
 * @param: reg 寄存器地址
 * @param: mask 需要清0的bit位
 ****************************************************************
 */
void ClearBitMask(unsigned char reg, unsigned char mask)
{
  uint8_t tmp = 0x0;
  tmp = ws_read_reg(reg);
  esp_err_t err = ws_write_reg(reg, tmp & ~mask); // clear bit mask //将数据的位清零
  // printf("ClearBitMask ret: %d", err);
}

void WriteRawRC(unsigned char Address, unsigned char value)
{
  ws_write_reg(Address, value);
}

unsigned char ReadRawRC(unsigned char Address)
{
  return ws_read_reg(Address);
}
