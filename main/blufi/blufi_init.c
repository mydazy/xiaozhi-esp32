/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "blufi_init.h"
#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_err.h"
#include "esp_log.h"
#include <stdio.h>
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif
#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "console/console.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

#ifdef CONFIG_BT_BLUEDROID_ENABLED
esp_err_t esp_blufi_host_init(void) {
  int ret;
  ret = esp_bluedroid_init();
  if (ret) {
    BLUFI_ERROR("%s init bluedroid failed: %s", __func__, esp_err_to_name(ret));
    return ESP_FAIL;
  }

  ret = esp_bluedroid_enable();
  if (ret) {
    BLUFI_ERROR("%s init bluedroid failed: %s", __func__, esp_err_to_name(ret));
    return ESP_FAIL;
  }
  BLUFI_INFO("BD ADDR: " ESP_BD_ADDR_STR,
             ESP_BD_ADDR_HEX(esp_bt_dev_get_address()));

  return ESP_OK;
}

esp_err_t esp_blufi_host_deinit(void) {
  int ret;
  ret = esp_blufi_profile_deinit();
  if (ret != ESP_OK) {
    return ret;
  }

  ret = esp_bluedroid_disable();
  if (ret) {
    BLUFI_ERROR("%s deinit bluedroid failed: %s", __func__,
                esp_err_to_name(ret));
    return ESP_FAIL;
  }

  ret = esp_bluedroid_deinit();
  if (ret) {
    BLUFI_ERROR("%s deinit bluedroid failed: %s", __func__,
                esp_err_to_name(ret));
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t esp_blufi_gap_register_callback(void) {
  int rc;
  rc = esp_ble_gap_register_callback(esp_blufi_gap_event_handler);
  if (rc) {
    return rc;
  }
  return esp_blufi_profile_init();
}

esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *example_callbacks) {
  esp_err_t ret = ESP_OK;

  ret = esp_blufi_host_init();
  if (ret) {
    BLUFI_ERROR("%s initialise host failed: %s", __func__,
                esp_err_to_name(ret));
    return ret;
  }

  ret = esp_blufi_register_callbacks(example_callbacks);
  if (ret) {
    BLUFI_ERROR("%s blufi register failed, error code = %x", __func__, ret);
    return ret;
  }

  ret = esp_blufi_gap_register_callback();
  if (ret) {
    BLUFI_ERROR("%s gap register failed, error code = %x", __func__, ret);
    return ret;
  }

  return ESP_OK;
}

#endif /* CONFIG_BT_BLUEDROID_ENABLED */

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
esp_err_t esp_blufi_controller_init() {
  esp_err_t ret = ESP_OK;
#if CONFIG_IDF_TARGET_ESP32
  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
#endif

  BLUFI_INFO("Initializing BT controller...");
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ret = esp_bt_controller_init(&bt_cfg);
  if (ret) {
    BLUFI_ERROR("%s initialize bt controller failed: %s", __func__,
                esp_err_to_name(ret));
    return ret;
  }
  BLUFI_INFO("BT controller initialized");

  ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret) {
    BLUFI_ERROR("%s enable bt controller failed: %s", __func__,
                esp_err_to_name(ret));
    return ret;
  }
  BLUFI_INFO("BT controller enabled");

  return ret;
}
#endif

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
esp_err_t esp_blufi_controller_deinit() {
  esp_err_t ret = ESP_OK;
  ret = esp_bt_controller_disable();
  if (ret) {
    BLUFI_ERROR("%s disable bt controller failed: %s", __func__,
                esp_err_to_name(ret));
    return ret;
  }

  ret = esp_bt_controller_deinit();
  if (ret) {
    BLUFI_ERROR("%s deinit bt controller failed: %s", __func__,
                esp_err_to_name(ret));
    return ret;
  }

  return ret;
}
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
void ble_store_config_init(void);

// BLE Host 任务句柄，用于等待任务结束
static TaskHandle_t s_ble_host_task_handle = NULL;
static volatile bool s_ble_host_task_running = false;
static bool s_blufi_profile_inited = false;

static void blufi_on_reset(int reason) {
  MODLOG_DFLT(ERROR, "Resetting state; reason=%d", reason);
  s_blufi_profile_inited = false;
}

static void blufi_on_sync(void) {
  if (!s_blufi_profile_inited) {
    esp_blufi_profile_init();
    s_blufi_profile_inited = true;
    BLUFI_INFO("Blufi profile initialized");
  }
}

void bleprph_host_task(void *param) {
  ESP_LOGI("BLUFI_EXAMPLE", "BLE Host Task Started");
  s_ble_host_task_running = true;

  /* This function will return only when nimble_port_stop() is executed */
  nimble_port_run();

  s_ble_host_task_running = false;
  s_ble_host_task_handle = NULL;

  nimble_port_freertos_deinit();
}

esp_err_t esp_blufi_host_init(void) {
  esp_err_t err;

  BLUFI_INFO("Initializing NimBLE host...");

  // 等控制器完全就绪后再起 host 栈
  vTaskDelay(pdMS_TO_TICKS(100));

  // 注意：IDF 5.5 的 esp_nimble_init() 内部已完成 os_mempool_module_init /
  BLUFI_INFO("Calling esp_nimble_init()...");
  err = esp_nimble_init();
  if (err != ESP_OK) {
    BLUFI_ERROR("esp_nimble_init() failed: %s (0x%x)", esp_err_to_name(err),
                err);
    return ESP_FAIL;
  }
  BLUFI_INFO("NimBLE stack initialized");

  /* Initialize the NimBLE host configuration. */
  ble_hs_cfg.reset_cb = blufi_on_reset;
  ble_hs_cfg.sync_cb = blufi_on_sync;
  ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  ble_hs_cfg.sm_io_cap = 4;
#ifdef CONFIG_EXAMPLE_BONDING
  ble_hs_cfg.sm_bonding = 1;
#endif
#ifdef CONFIG_EXAMPLE_MITM
  ble_hs_cfg.sm_mitm = 1;
#endif
#ifdef CONFIG_EXAMPLE_USE_SC
  ble_hs_cfg.sm_sc = 1;
#else
  ble_hs_cfg.sm_sc = 0;
#ifdef CONFIG_EXAMPLE_BONDING
  ble_hs_cfg.sm_our_key_dist = 1;
  ble_hs_cfg.sm_their_key_dist = 1;
#endif
#endif

  int rc;
  rc = esp_blufi_gatt_svr_init();
  assert(rc == 0);

  /* XXX Need to have template for store */
  ble_store_config_init();

  // 【MTU优化】设置首选MTU为247字节（BLE 4.2+最大值）
  // 解决Android设备WiFi列表显示不完整问题
  // - 默认MTU 23字节，20个热点需要分10+片，Android BLE队列易溢出
  // - 增大到247字节后，只需2-3片，大幅降低丢包风险
  // - iOS自动协商到~185字节，Android可协商到247字节
  extern int ble_att_set_preferred_mtu(uint16_t mtu);
  rc = ble_att_set_preferred_mtu(247);
  if (rc == 0) {
    BLUFI_INFO("✅ MTU首选值已设置为 247 字节（实际MTU由对端协商决定）");
  } else {
    BLUFI_ERROR("❌ MTU设置失败: %d", rc);
  }

  esp_blufi_btc_init();

  err = esp_nimble_enable(bleprph_host_task);
  if (err) {
    BLUFI_ERROR("%s failed: %s", __func__, esp_err_to_name(err));
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t esp_blufi_host_deinit(void) {
  esp_err_t ret = ESP_OK;

  // 重置 profile 初始化标志，下次 sync 时重新初始化
  s_blufi_profile_inited = false;

  // 1. 反初始化 Blufi profile（重置 blufi_env.enabled 标志）
  ret = esp_blufi_profile_deinit();
  if (ret != ESP_OK) {
    BLUFI_ERROR("esp_blufi_profile_deinit failed: %s", esp_err_to_name(ret));
  }

  // 2. 停止 NimBLE Host 任务
  ret = nimble_port_stop();
  if (ret != 0) {
    BLUFI_ERROR("nimble_port_stop failed: %d", ret);
  }

  // 3. 等待 BLE Host 任务完全结束（最多等待 2 秒）
  int wait_count = 0;
  while (s_ble_host_task_running && wait_count < 40) {
    vTaskDelay(pdMS_TO_TICKS(50));
    wait_count++;
  }
  if (s_ble_host_task_running) {
    BLUFI_ERROR("BLE Host task did not exit in time");
  }

  // 4. 反初始化 NimBLE 栈
  ret = esp_nimble_deinit();
  if (ret != ESP_OK) {
    BLUFI_ERROR("esp_nimble_deinit failed: %s", esp_err_to_name(ret));
  }

  // 5. 反初始化 Blufi BTC 层
  esp_blufi_btc_deinit();

  return ESP_OK;
}

esp_err_t esp_blufi_gap_register_callback(void) { return ESP_OK; }

esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *example_callbacks) {
  esp_err_t ret = ESP_OK;

  ret = esp_blufi_register_callbacks(example_callbacks);
  if (ret) {
    BLUFI_ERROR("%s blufi register failed, error code = %x", __func__, ret);
    return ret;
  }

  ret = esp_blufi_gap_register_callback();
  if (ret) {
    BLUFI_ERROR("%s gap register failed, error code = %x", __func__, ret);
    return ret;
  }

  ret = esp_blufi_host_init();
  if (ret) {
    BLUFI_ERROR("%s initialise host failed: %s", __func__,
                esp_err_to_name(ret));
    return ret;
  }

  return ret;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
