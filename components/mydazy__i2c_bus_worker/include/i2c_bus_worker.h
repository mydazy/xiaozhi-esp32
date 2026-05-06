/*
 * SPDX-FileCopyrightText: 2026 MyDazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C Bus Worker
 * --------------
 * 单线程串行化 I2C 总线访问的轻量调度器，专为 4G RF 共线场景下
 * 多 driver 共享 I2C 总线（音频 codec / 触摸 / sensor / NFC）设计。
 *
 * 设计目标：
 *   1. 协议层 100% 串行 — 杜绝多线程 I2C 竞争 / bus_reset 砸状态机
 *   2. 会话层批量序列保护 — lock_session 包裹多寄存器写不被打断
 *   3. 优先级反转防御 — 单 worker 任务高优先级（推荐 P10 + Core 0）
 *   4. 同步阻塞 API — 与 i2c_master_transmit 行为一致，driver 移植成本低
 *   5. 总线异常自动恢复 — 错误率达阈值由 worker 调度统一 reset
 *
 * 与 IDF 原生 i2c_master 的关系：
 *   - 每个 driver 不再持有 i2c_master_dev_handle_t，改持有 i2c_worker_dev_t*
 *   - worker 内部唯一调用 i2c_master_transmit / receive 等 IDF API
 *   - IDF bus mutex 仍在底层生效，worker 提供的是"会话层"串行
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────
 * 类型与配置
 * ──────────────────────────────────────────────────────────────── */

typedef struct i2c_worker_t*     i2c_worker_handle_t;
typedef struct i2c_worker_dev_t  i2c_worker_dev_t;

/** worker 创建参数 */
typedef struct {
    i2c_master_bus_handle_t bus;            /**< 已存在的 IDF I2C bus */
    uint32_t                task_priority;  /**< 推荐 10（与 audio_output 同位） */
    int                     task_core;      /**< 推荐 0（网络/codec 同核） */
    uint32_t                queue_depth;    /**< 默认 32，溢出 caller 阻塞 */
    uint32_t                stack_size;     /**< 默认 4096，复杂场景可调 6144 */
    uint32_t                err_streak_for_reset; /**< 连续错误触发 bus_reset，默认 3 */
} i2c_worker_config_t;

/** 默认配置宏 */
#define I2C_WORKER_DEFAULT_CONFIG(_bus) {                       \
    .bus                  = (_bus),                              \
    .task_priority        = 10,                                  \
    .task_core            = 0,                                   \
    .queue_depth          = 32,                                  \
    .stack_size           = 4096,                                \
    .err_streak_for_reset = 3,                                   \
}

/** 诊断统计（用户可定期读取上报后台） */
typedef struct {
    uint32_t total_ops;          /**< 累计 op 数 */
    uint32_t total_errors;       /**< 累计失败 op 数 */
    uint32_t bus_reset_count;    /**< 累计 bus_reset 次数 */
    uint32_t max_queue_depth;    /**< 历史最高排队深度 */
    uint32_t timeout_count;      /**< 超时次数 */
    uint64_t last_error_us;      /**< 最近一次错误时间 (esp_timer_get_time) */
} i2c_worker_stats_t;

/* ────────────────────────────────────────────────────────────────
 * 生命周期
 * ──────────────────────────────────────────────────────────────── */

/**
 * 创建 worker（每条 I2C 总线一个 worker）
 * @param cfg     配置
 * @param[out] out 成功时返回 handle
 * @return ESP_OK / ESP_ERR_NO_MEM / ESP_ERR_INVALID_ARG
 */
esp_err_t i2c_worker_create(const i2c_worker_config_t* cfg, i2c_worker_handle_t* out);

/**
 * 销毁 worker（停 task → 释放队列 → 释放 mutex）
 * 调用前所有设备应已 i2c_worker_remove_device
 */
esp_err_t i2c_worker_destroy(i2c_worker_handle_t handle);

/* ────────────────────────────────────────────────────────────────
 * 设备注册
 * ──────────────────────────────────────────────────────────────── */

/**
 * 注册设备到 worker
 * @param handle worker
 * @param addr_7bit 7-bit I2C 地址
 * @param scl_speed_hz I2C 速率（如 100000 / 400000）
 * @return dev / NULL on error
 */
i2c_worker_dev_t* i2c_worker_add_device(
    i2c_worker_handle_t handle,
    uint16_t addr_7bit,
    uint32_t scl_speed_hz);

/**
 * 移除设备（释放底层 i2c_master_dev_handle_t）
 */
esp_err_t i2c_worker_remove_device(i2c_worker_dev_t* dev);

/* ────────────────────────────────────────────────────────────────
 * 单条事务（同步阻塞）
 * ──────────────────────────────────────────────────────────────── */

/**
 * 同步写
 * @param dev 设备
 * @param data 写入数据
 * @param len 长度
 * @param timeout_ms 总超时（含等队列 + 等 worker 完成）
 * @return ESP_OK / ESP_ERR_TIMEOUT / ESP_FAIL
 */
esp_err_t i2c_worker_write(
    i2c_worker_dev_t* dev,
    const void* data,
    size_t len,
    uint32_t timeout_ms);

/**
 * 同步读（无地址）— 部分简单 sensor 直接读 FIFO 用
 */
esp_err_t i2c_worker_read(
    i2c_worker_dev_t* dev,
    void* data,
    size_t len,
    uint32_t timeout_ms);

/**
 * 同步 write+read 复合事务（IDF i2c_master_transmit_receive 等价）
 * 典型用例：写寄存器地址 → 重复 START → 读寄存器值
 */
esp_err_t i2c_worker_write_read(
    i2c_worker_dev_t* dev,
    const void* write_data, size_t write_len,
    void* read_data, size_t read_len,
    uint32_t timeout_ms);

/* ────────────────────────────────────────────────────────────────
 * 会话锁（保护批量寄存器序列）
 * ──────────────────────────────────────────────────────────────── */

/**
 * 锁定会话 — 期间其他 caller 的 op 排队等待
 *
 * 使用模式：
 *   i2c_worker_lock_session(worker, 100);
 *   {
 *       i2c_worker_write(dev_a, ...);
 *       i2c_worker_write(dev_b, ...);
 *       i2c_worker_write(dev_c, ...);
 *   }
 *   i2c_worker_unlock_session(worker);
 *
 * 内部用 recursive mutex，同一 caller 可重入。
 * 必须严格成对调用，遗漏 unlock 会卡住所有其他 driver。
 */
esp_err_t i2c_worker_lock_session(i2c_worker_handle_t handle, uint32_t timeout_ms);
esp_err_t i2c_worker_unlock_session(i2c_worker_handle_t handle);

/* ────────────────────────────────────────────────────────────────
 * 总线恢复
 * ──────────────────────────────────────────────────────────────── */

/**
 * 触发 worker 调度的 bus_reset（不会砸正在传输的其他设备）
 * 调用时机：driver 检测到连续 N 次 I2C 错误。
 * 内部会先排空队列、做 i2c_master_bus_reset、再恢复。
 *
 * 注意：worker 内部已有 err_streak_for_reset 自动触发，业务代码通常不需主动调。
 */
esp_err_t i2c_worker_bus_reset(i2c_worker_handle_t handle);

/* ────────────────────────────────────────────────────────────────
 * 诊断
 * ──────────────────────────────────────────────────────────────── */

/**
 * 读取诊断统计（lock-free，可任何 task 调用）
 */
esp_err_t i2c_worker_get_stats(
    i2c_worker_handle_t handle,
    i2c_worker_stats_t* out);

/**
 * 重置诊断计数（保留 bus_reset_count，量产现场上报后清零）
 */
esp_err_t i2c_worker_reset_stats(i2c_worker_handle_t handle);

#ifdef __cplusplus
}
#endif
