# v6.1.120 — `drivers/pwm/pwm-imx27.c`

## Problem

The automated merge for **linux-stable v6.1.120** triggered **VERIFICATION CRITICAL** on this file: the resolver produced **duplicate logic / declarations** around **`pwm_imx27_apply()`** while integrating mainline’s **ERR051198** workaround (period calculation, **`local_irq_save` / `local_irq_restore`**, **`writel_relaxed`**, etc.).

After inspection of the tree:

- **`pwm_imx27_apply()`** already matched **stable v6.1.120** for the ERR051198 path (no duplicated function body in the final sources).
- Leftovers from the messy merge included an **initialized but unused `spinlock_t`** (**`imx->lock`**): mainline’s fix uses **IRQ masking** (`local_irq_save`) rather than that spinlock, so the lock was **dead code**.

## Fix

- **Removed** unused **`#include <linux/spinlock.h>`**, **`spinlock_t lock`** from **`struct pwm_imx27_chip`**, **`spin_lock_init(&imx->lock)`**, and redundant **`imx->duty_cycle = 0`** in probe (**`devm_kzalloc`** already zeroes the struct).

**Intentional differences vs `v6.1.120` upstream** (NXP fork), unchanged:

- Optional **`32k`** clock in **`pwm_imx27_clk_prepare_enable`** / **`pwm_imx27_clk_disable_unprepare`** / probe.
- **`pwm_imx27_wait_fifo_slot`**: NXP keeps **`fifoav >= MX3_PWMSR_FIFOAV_3WORDS`** and **`msleep(period_ms * (fifoav - 2))`** instead of mainline’s **`== 4WORDS`** / **`msleep(period_ms)`**.

## Reference

- Merge log: `merge-logs/merge_6.1.120_2026-05-04T10-43-47.711521.log`
- Upstream reference tag: **`v6.1.120`** — `drivers/pwm/pwm-imx27.c`
