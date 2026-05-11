# v6.1.143 — `drivers/media/platform/nxp/imx-jpeg/mxc-jpeg.c`

## Problem

**v6.1.143** had merge conflicts in:

- `drivers/media/platform/nxp/imx-jpeg/mxc-jpeg.c`
- `drivers/tty/serial/imx.c`

The **build** failed with four errors in **`mxc-jpeg.c`**. The automation escalated after one fix attempt.

### `mxc-jpeg.c`

Stable **v6.1.143** models **`struct mxc_jpeg_dev::slot_data`** as a **single** **`struct mxc_jpeg_slot_data`** in **`mxc-jpeg.h`**. Unallocated contexts use **`ctx->slot = -1`**, and runtime code clears or checks **`jpeg->slot_data.used`** without array indexing.

The bad merge kept **NXP multi-slot** call sites from the vendor side:

- **`slot_data[ctx->slot]`** / **`slot_data[slot]`** subscripts on a non-array member.
- **`MXC_MAX_SLOTS`** for timeout guards and as the “slot not allocated yet” sentinel.

Those symbols no longer exist in the merged header, so the driver would not compile.

### `imx.c`

No source fix was required: the conflict resolution kept **NXP’s** **`pm_qos_req`** and **mainline’s** **`rxtl`** as separate **`struct imx_port`** fields.

## Fix

**`mxc-jpeg.c`** was aligned with the **single-slot** layout already present in **`mxc-jpeg.h`**:

- **`mxc_jpeg_job_finish()`**: clear **`jpeg->slot_data.used`** (not **`slot_data[ctx->slot]`**).
- **`mxc_jpeg_dec_irq()`**: test **`jpeg->slot_data.used`**.
- **`mxc_jpeg_device_run_timeout()`**: gate on **`ctx->mxc_jpeg->slot_data.used`** (no **`MXC_MAX_SLOTS`** check).
- **`mxc_jpeg_open()`**: initialize **`ctx->slot = -1`** when no hardware slot is assigned yet.

**Intentional differences vs `v6.1.143` upstream** (NXP fork), unchanged:

- **`sw_reset`** module parameter and passing **`sw_reset`** into **`mxc_jpeg_job_finish()`** instead of a literal **`false`**.
- **`mxc_jpeg_get_plane_size()`** retained for decode payload sizing on multi-plane capture buffers.

## Reference

- Merge log: `merge-logs/merge_6.1.143_2026-05-11T14-37-07.079902.log`
- JSON: `merge-logs/merge_6.1.143_2026-05-11T14-37-07.079902.json`
- Upstream reference tag: **`v6.1.143`** — `drivers/media/platform/nxp/imx-jpeg/mxc-jpeg.c`, `mxc-jpeg.h`
