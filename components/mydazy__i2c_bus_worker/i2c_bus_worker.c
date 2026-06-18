/*
 * SPDX-FileCopyrightText: 2026 MyDazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C Bus Worker 实现
 *
 * 工作原理：
 *   ┌─ caller A ─┐                    ┌─ worker task ─┐
 *   │ submit op  ├──► xQueueSend ────►│ xQueueReceive ├──► i2c_master_*
 *   │ wait sem   │◄── xSemaphoreGive──┤ 执行 op       │
 *   └────────────┘                    │ 给 result_sem │
 *                                     └───────────────┘
 *
 *   每个 op 携带 result_sem（caller 栈上 binary semaphore），
 *   caller 阻塞 take，worker 完成后 give。
 *
 *   单 task 单队列 = 100% 串行 = 杜绝 5 driver 跨 transaction 竞争。
 */

#include "i2c_bus_worker.h"

#include <string.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#define TAG "i2c_worker"

/* ────────────────────────────────────────────────────────────────
 * 内部数据结构
 * ──────────────────────────────────────────────────────────────── */

typedef enum {
    OP_WRITE = 0,
    OP_READ,
    OP_WRITE_READ,
    OP_BUS_RESET,
    OP_QUIT,            /* 优雅停 worker */
} op_type_t;


typedef struct {
    SemaphoreHandle_t   sem;        /* 堆上 binary semaphore（非栈 static）*/
    esp_err_t           result;     /* worker 写回结果 */
    _Atomic int         refcount;   /* 2 → 1 → 0(free) */
    uint8_t            *wbuf;        /* write 数据堆拷贝（脱离 caller 栈）*/
    uint8_t            *rbuf;        /* read 数据堆缓冲（worker 写，caller 成功后拷回）*/
} op_ctx_t;

typedef struct {
    op_type_t           type;
    i2c_worker_dev_t   *dev;
    const void         *write_data;
    size_t              write_len;
    void               *read_data;
    size_t              read_len;
    uint32_t            timeout_ms;     /* 单次 i2c_master 超时（不含等队列） */
    SemaphoreHandle_t   result_sem;     /* = ctx->sem（worker give）*/
    esp_err_t          *result_out;     /* = &ctx->result（worker 写）*/
    op_ctx_t           *ctx;            /* 堆上下文；OP_QUIT 为 NULL */
} op_t;

/* 释放一份引用；减到 0 时真正销毁。worker 与 caller 各调一次。 */
static void op_ctx_release(op_ctx_t *ctx)
{
    if (!ctx) return;
    if (atomic_fetch_sub(&ctx->refcount, 1) == 1) {   /* 旧值为 1 → 我是最后一个 */
        if (ctx->sem)  vSemaphoreDelete(ctx->sem);
        if (ctx->wbuf) free(ctx->wbuf);
        if (ctx->rbuf) free(ctx->rbuf);
        free(ctx);
    }
}

struct i2c_worker_dev_t {
    i2c_worker_handle_t       worker;
    i2c_master_dev_handle_t   dev_handle;
    uint16_t                  addr_7bit;
    uint32_t                  scl_speed_hz;
};

struct i2c_worker_t {
    i2c_master_bus_handle_t   bus;
    QueueHandle_t             queue;
    SemaphoreHandle_t         session_mutex;     /* recursive，会话锁 */
    TaskHandle_t              task;
    atomic_bool               running;

    uint32_t                  err_streak_for_reset;
    uint32_t                  err_streak;        /* 仅 worker task 访问 */

    /* bus_reset 节流：避免设备层 NACK 反复触发 reset 风暴。
       连续 reset ≥ kBusResetBurstLimit 次后冷却 kBusResetCooldownMs ms。 */
    uint32_t                  consecutive_resets; /* 仅 worker task 访问 */
    uint64_t                  last_reset_us;      /* 仅 worker task 访问 */

    /* 诊断（worker task 写，外部 lock-free 读） */
    atomic_uint               total_ops;
    atomic_uint               total_errors;
    atomic_uint               bus_reset_count;
    atomic_uint               max_queue_depth;
    atomic_uint               timeout_count;
    atomic_uint_fast64_t      last_error_us;
};

/* bus_reset 节流参数（避免风暴） */
#define BUS_RESET_BURST_LIMIT      3       /* 连续 N 次 reset */
#define BUS_RESET_COOLDOWN_MS      5000    /* 后冷却 5 s 不再 reset */
#define BUS_RESET_BURST_WINDOW_US  500000  /* 500 ms 内连续 = burst */

/* 给 .c 内部使用的 typedef（头文件只有 i2c_worker_handle_t 指针 typedef） */
typedef struct i2c_worker_t i2c_worker_t;

/* ────────────────────────────────────────────────────────────────
 * Worker task 主循环
 * ──────────────────────────────────────────────────────────────── */

static esp_err_t worker_execute_op(i2c_worker_t *w, const op_t *op)
{
    esp_err_t ret = ESP_FAIL;

    switch (op->type) {
    case OP_WRITE:
        if (op->dev && op->dev->dev_handle) {
            ret = i2c_master_transmit(op->dev->dev_handle,
                                       (const uint8_t *)op->write_data,
                                       op->write_len,
                                       op->timeout_ms);
        }
        break;
    case OP_READ:
        if (op->dev && op->dev->dev_handle) {
            ret = i2c_master_receive(op->dev->dev_handle,
                                      (uint8_t *)op->read_data,
                                      op->read_len,
                                      op->timeout_ms);
        }
        break;
    case OP_WRITE_READ:
        if (op->dev && op->dev->dev_handle) {
            ret = i2c_master_transmit_receive(op->dev->dev_handle,
                                               (const uint8_t *)op->write_data,
                                               op->write_len,
                                               (uint8_t *)op->read_data,
                                               op->read_len,
                                               op->timeout_ms);
        }
        break;
    case OP_BUS_RESET:
        ret = i2c_master_bus_reset(w->bus);
        atomic_fetch_add(&w->bus_reset_count, 1);
        ESP_LOGW(TAG, "bus_reset triggered (streak=%u, ret=%d)", w->err_streak, (int)ret);
        w->err_streak = 0;
        break;
    case OP_QUIT:
        ret = ESP_OK;
        break;
    }
    return ret;
}

static void worker_task(void *arg)
{
    i2c_worker_t *w = (i2c_worker_t *)arg;
    op_t op;

    ESP_LOGI(TAG, "worker task started (core=%d, prio=%u)",
             xPortGetCoreID(), uxTaskPriorityGet(NULL));

    while (atomic_load(&w->running)) {
        if (xQueueReceive(w->queue, &op, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (op.type == OP_QUIT) {
            if (op.result_sem) xSemaphoreGive(op.result_sem);
            break;
        }

        /* 排队深度水位 */
        UBaseType_t depth = uxQueueMessagesWaiting(w->queue) + 1;
        uint32_t cur_max = atomic_load(&w->max_queue_depth);
        if ((uint32_t)depth > cur_max) {
            atomic_store(&w->max_queue_depth, (uint32_t)depth);
        }

        /* 错误连击达阈值 → bus_reset，但加节流避免风暴：
         *   - 连续 reset ≥ BUS_RESET_BURST_LIMIT（500ms 内）后冷却 5s
         *   - 冷却期间 streak 仍清零（不让"已知有问题"反复刷屏） */
        if (w->err_streak >= w->err_streak_for_reset) {
            uint64_t now = (uint64_t)esp_timer_get_time();
            bool in_burst_window = (now - w->last_reset_us) < BUS_RESET_BURST_WINDOW_US;
            if (!in_burst_window) {
                w->consecutive_resets = 0;   /* 离开 burst 窗口，重置计数 */
            }

            if (w->consecutive_resets >= BUS_RESET_BURST_LIMIT) {
                /* 冷却期：不再 reset，仅清 streak 让 driver 继续 retry */
                if ((now - w->last_reset_us) < (uint64_t)BUS_RESET_COOLDOWN_MS * 1000ULL) {
                    w->err_streak = 0;
                    /* 不打日志避免刷屏 */
                } else {
                    /* 冷却结束，可以再 reset 一次 */
                    w->consecutive_resets = 0;
                }
            }

            if (w->consecutive_resets < BUS_RESET_BURST_LIMIT) {
                esp_err_t reset_ret = i2c_master_bus_reset(w->bus);
                atomic_fetch_add(&w->bus_reset_count, 1);
                w->consecutive_resets++;
                w->last_reset_us = now;
                ESP_LOGW(TAG, "auto bus_reset (streak=%u, burst=%u/%d, ret=%d)",
                         w->err_streak, w->consecutive_resets, BUS_RESET_BURST_LIMIT, (int)reset_ret);
                w->err_streak = 0;
                vTaskDelay(pdMS_TO_TICKS(50));   /* 50ms settling（原 2ms 太短） */

                if (w->consecutive_resets >= BUS_RESET_BURST_LIMIT) {
                    ESP_LOGW(TAG, "bus_reset burst limit reached → cooldown %d ms",
                             BUS_RESET_COOLDOWN_MS);
                }
            }
        }

        esp_err_t ret = worker_execute_op(w, &op);
        atomic_fetch_add(&w->total_ops, 1);

        if (ret != ESP_OK) {
            atomic_fetch_add(&w->total_errors, 1);
            atomic_store(&w->last_error_us, (uint64_t)esp_timer_get_time());
            if (ret == ESP_ERR_TIMEOUT) {
                atomic_fetch_add(&w->timeout_count, 1);
            }
            if (op.type != OP_BUS_RESET) {
                w->err_streak++;
            }
        } else {
            w->err_streak = 0;
        }

        if (op.result_out) *op.result_out = ret;
        if (op.result_sem) xSemaphoreGive(op.result_sem);
        if (op.ctx) op_ctx_release(op.ctx);   /* worker 释放自己那份引用（give 后 ctx 仍有效）*/
    }

    ESP_LOGI(TAG, "worker task exiting");
    vTaskDelete(NULL);
}

/* ────────────────────────────────────────────────────────────────
 * 提交 op（caller 阻塞等结果）
 * ──────────────────────────────────────────────────────────────── */

static esp_err_t submit_and_wait(i2c_worker_handle_t w, op_t *op, uint32_t total_timeout_ms)
{
    /* 共享上下文堆分配——caller 超时放弃后栈帧回退也不影响 worker（堆不随栈失效）。
       refcount=2：caller 与 worker 各持一份，谁后释放谁 free。 */
    op_ctx_t *ctx = (op_ctx_t *)heap_caps_calloc(
        1, sizeof(op_ctx_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ctx) return ESP_ERR_NO_MEM;
    ctx->sem = xSemaphoreCreateBinary();
    if (!ctx->sem) { free(ctx); return ESP_ERR_NO_MEM; }
    ctx->result = ESP_FAIL;
    atomic_init(&ctx->refcount, 2);

    /* write 数据拷进堆：worker 用 ctx->wbuf 而非 caller 栈缓冲，超时后仍安全 */
    if (op->write_data && op->write_len) {
        ctx->wbuf = (uint8_t *)heap_caps_malloc(op->write_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!ctx->wbuf) { vSemaphoreDelete(ctx->sem); free(ctx); return ESP_ERR_NO_MEM; }
        memcpy(ctx->wbuf, op->write_data, op->write_len);
        op->write_data = ctx->wbuf;
    }
    /* read 走堆缓冲，成功后再拷回 caller（超时放弃则不拷，由 free 回收） */
    void  *caller_read = op->read_data;
    size_t caller_rlen = op->read_len;
    if (op->read_data && op->read_len) {
        ctx->rbuf = (uint8_t *)heap_caps_malloc(op->read_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!ctx->rbuf) {
            if (ctx->wbuf) free(ctx->wbuf);
            vSemaphoreDelete(ctx->sem); free(ctx);
            return ESP_ERR_NO_MEM;
        }
        op->read_data = ctx->rbuf;
    }
    op->result_sem = ctx->sem;
    op->result_out = &ctx->result;
    op->ctx        = ctx;

    TickType_t tick_total = pdMS_TO_TICKS(total_timeout_ms);
    TickType_t tick_send  = pdMS_TO_TICKS(10);
    if (tick_send > tick_total) tick_send = (tick_total > 1) ? (tick_total / 4) : 1;
    TickType_t tick_wait  = (tick_total > tick_send) ? (tick_total - tick_send) : 1;

    if (xQueueSend(w->queue, op, tick_send) != pdTRUE) {
        /* 没进队列，worker 永不持有这份 ctx：caller 独占，把 worker 那份引用收回再释放 */
        atomic_store(&ctx->refcount, 1);
        op_ctx_release(ctx);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(ctx->sem, tick_wait) != pdTRUE) {
        /* 诊断字段（队列深度 + 累计错误）方便量产现场追踪是 worker 被饿还是硬件卡住 */
        UBaseType_t qd = uxQueueMessagesWaiting(w->queue);
        ESP_LOGE(TAG, "submit timeout — worker may still be executing "
                      "(queue=%u, errs=%u, total_ops=%u)",
                 (unsigned)qd,
                 atomic_load(&w->total_errors),
                 atomic_load(&w->total_ops));
        /* 兜底再等 200 ms（若 worker 已开始 i2c_master_*，会在硬件超时后退出） */
        if (xSemaphoreTake(ctx->sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGE(TAG, "worker stuck > 200ms — caller abandons (worker owns ctx, no UAF)");
            /* caller 放弃：只释放自己这份引用。worker 仍持一份，待其完成时释放，
               最后释放者 free——ctx 在堆上不随本函数返回失效，杜绝 UAF。 */
            op_ctx_release(ctx);
            return ESP_ERR_TIMEOUT;
        }
    }

    /* 拿到 sem = worker 已 give 且不会再碰 ctx → 安全读 result / 拷回 read 数据 */
    esp_err_t op_result = ctx->result;
    if (caller_read && caller_rlen && op_result == ESP_OK) {
        memcpy(caller_read, ctx->rbuf, caller_rlen);
    }
    op_ctx_release(ctx);
    return op_result;
}

/* ────────────────────────────────────────────────────────────────
 * Public API — 生命周期
 * ──────────────────────────────────────────────────────────────── */

esp_err_t i2c_worker_create(const i2c_worker_config_t *cfg, i2c_worker_handle_t *out)
{
    if (!cfg || !out || !cfg->bus) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    i2c_worker_t *w = (i2c_worker_t *)heap_caps_calloc(
        1, sizeof(i2c_worker_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!w) return ESP_ERR_NO_MEM;

    w->bus = cfg->bus;
    w->err_streak_for_reset = cfg->err_streak_for_reset ? cfg->err_streak_for_reset : 3;

    uint32_t qd = cfg->queue_depth ? cfg->queue_depth : 32;
    w->queue = xQueueCreate(qd, sizeof(op_t));
    if (!w->queue) goto fail;

    w->session_mutex = xSemaphoreCreateRecursiveMutex();
    if (!w->session_mutex) goto fail;

    atomic_store(&w->running, true);

    uint32_t stack = cfg->stack_size ? cfg->stack_size : 4096;
    BaseType_t ok = xTaskCreatePinnedToCore(
        worker_task, "i2c_worker", stack, w,
        cfg->task_priority ? cfg->task_priority : 10,
        &w->task,
        cfg->task_core);
    if (ok != pdPASS) goto fail;

    *out = w;
    ESP_LOGI(TAG, "created (queue_depth=%u, prio=%u, core=%d, err_streak=%u)",
             qd, cfg->task_priority, cfg->task_core, w->err_streak_for_reset);
    return ESP_OK;

fail:
    if (w->session_mutex) vSemaphoreDelete(w->session_mutex);
    if (w->queue)         vQueueDelete(w->queue);
    free(w);
    return ESP_ERR_NO_MEM;
}

esp_err_t i2c_worker_destroy(i2c_worker_handle_t w)
{
    if (!w) return ESP_ERR_INVALID_ARG;

    /* 投递 QUIT op 让 worker 优雅退出 */
    StaticSemaphore_t sem_buf;
    SemaphoreHandle_t sem = xSemaphoreCreateBinaryStatic(&sem_buf);
    op_t quit = { .type = OP_QUIT, .result_sem = sem };
    atomic_store(&w->running, false);
    xQueueSend(w->queue, &quit, pdMS_TO_TICKS(500));
    xSemaphoreTake(sem, pdMS_TO_TICKS(1000));
    vSemaphoreDelete(sem);

    /* worker_task 已 vTaskDelete(NULL) */
    vQueueDelete(w->queue);
    vSemaphoreDelete(w->session_mutex);
    free(w);
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────
 * Public API — 设备注册
 * ──────────────────────────────────────────────────────────────── */

i2c_worker_dev_t *i2c_worker_add_device(
    i2c_worker_handle_t w, uint16_t addr_7bit, uint32_t scl_speed_hz)
{
    if (!w) return NULL;

    i2c_worker_dev_t *dev = (i2c_worker_dev_t *)heap_caps_calloc(
        1, sizeof(i2c_worker_dev_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!dev) return NULL;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr_7bit,
        .scl_speed_hz    = scl_speed_hz,
    };
    if (i2c_master_bus_add_device(w->bus, &dev_cfg, &dev->dev_handle) != ESP_OK) {
        free(dev);
        return NULL;
    }
    dev->worker       = w;
    dev->addr_7bit    = addr_7bit;
    dev->scl_speed_hz = scl_speed_hz;
    return dev;
}

esp_err_t i2c_worker_remove_device(i2c_worker_dev_t *dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->dev_handle) i2c_master_bus_rm_device(dev->dev_handle);
    free(dev);
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────
 * Public API — 单条事务
 * ──────────────────────────────────────────────────────────────── */

esp_err_t i2c_worker_write(
    i2c_worker_dev_t *dev, const void *data, size_t len, uint32_t timeout_ms)
{
    if (!dev || !data) return ESP_ERR_INVALID_ARG;
    op_t op = {
        .type = OP_WRITE,
        .dev = dev,
        .write_data = data,
        .write_len  = len,
        .timeout_ms = timeout_ms,
    };
    return submit_and_wait(dev->worker, &op, timeout_ms + 50);
}

esp_err_t i2c_worker_read(
    i2c_worker_dev_t *dev, void *data, size_t len, uint32_t timeout_ms)
{
    if (!dev || !data) return ESP_ERR_INVALID_ARG;
    op_t op = {
        .type = OP_READ,
        .dev = dev,
        .read_data = data,
        .read_len  = len,
        .timeout_ms = timeout_ms,
    };
    return submit_and_wait(dev->worker, &op, timeout_ms + 50);
}

esp_err_t i2c_worker_write_read(
    i2c_worker_dev_t *dev,
    const void *write_data, size_t write_len,
    void *read_data, size_t read_len,
    uint32_t timeout_ms)
{
    if (!dev || !write_data || !read_data) return ESP_ERR_INVALID_ARG;
    op_t op = {
        .type = OP_WRITE_READ,
        .dev = dev,
        .write_data = write_data,
        .write_len  = write_len,
        .read_data  = read_data,
        .read_len   = read_len,
        .timeout_ms = timeout_ms,
    };
    return submit_and_wait(dev->worker, &op, timeout_ms + 50);
}

/* ────────────────────────────────────────────────────────────────
 * Public API — 会话锁
 * ──────────────────────────────────────────────────────────────── */

esp_err_t i2c_worker_lock_session(i2c_worker_handle_t w, uint32_t timeout_ms)
{
    if (!w) return ESP_ERR_INVALID_ARG;
    return (xSemaphoreTakeRecursive(w->session_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t i2c_worker_unlock_session(i2c_worker_handle_t w)
{
    if (!w) return ESP_ERR_INVALID_ARG;
    return (xSemaphoreGiveRecursive(w->session_mutex) == pdTRUE)
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* ────────────────────────────────────────────────────────────────
 * Public API — 总线恢复
 * ──────────────────────────────────────────────────────────────── */

esp_err_t i2c_worker_bus_reset(i2c_worker_handle_t w)
{
    if (!w) return ESP_ERR_INVALID_ARG;
    op_t op = { .type = OP_BUS_RESET, .timeout_ms = 100 };
    return submit_and_wait(w, &op, 500);
}

/* ────────────────────────────────────────────────────────────────
 * Public API — 诊断
 * ──────────────────────────────────────────────────────────────── */

esp_err_t i2c_worker_get_stats(i2c_worker_handle_t w, i2c_worker_stats_t *out)
{
    if (!w || !out) return ESP_ERR_INVALID_ARG;
    out->total_ops       = atomic_load(&w->total_ops);
    out->total_errors    = atomic_load(&w->total_errors);
    out->bus_reset_count = atomic_load(&w->bus_reset_count);
    out->max_queue_depth = atomic_load(&w->max_queue_depth);
    out->timeout_count   = atomic_load(&w->timeout_count);
    out->last_error_us   = atomic_load(&w->last_error_us);
    return ESP_OK;
}

esp_err_t i2c_worker_reset_stats(i2c_worker_handle_t w)
{
    if (!w) return ESP_ERR_INVALID_ARG;
    atomic_store(&w->total_ops, 0);
    atomic_store(&w->total_errors, 0);
    atomic_store(&w->max_queue_depth, 0);
    atomic_store(&w->timeout_count, 0);
    atomic_store(&w->last_error_us, 0);
    /* 保留 bus_reset_count（量产现场上报后清零）*/
    return ESP_OK;
}
