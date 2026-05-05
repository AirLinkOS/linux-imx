# v6.1.125 — `drivers/gpu/drm/bridge/adv7511/adv7511_drv.c` (and related)

## Problem

**v6.1.125** had merge conflicts in:

- `drivers/gpu/drm/bridge/adv7511/adv7511_drv.c`
- `drivers/iio/gyro/fxas21002c_core.c`

The **build** failed; logs also showed the automation could not parse compiler errors reliably.

### ADV7511 (`adv7511_drv.c`)

Stable **v6.1.125** moves **`adv7533_attach_dsi()`** from bridge attach time into **`adv7511_probe()`** (after **`adv7511_audio_init()`**) and adds **`err_unregister_audio`** so failures unwind **audio** and **drm_bridge** before other cleanup — part of the **DSI / `host_node` lifetime** fix.

The bad merge:

- Left **`adv7533_attach_dsi()`** in **`adv7511_bridge_attach()`** while also resolving conflicts in probe, risking **double attach** and diverging from stable.
- Concatenated **`err_of_node_put:`** with a large **`CONFIG_OF_DYNAMIC`** cleanup block in the wrong place, including **use of `remote_node` after `of_node_put(remote_node)`** in **`dev_warn`** (use-after-free in the warning path).
- Omitted the stable **probe** sequence (**DSI attach** + **`goto err_unregister_audio`**) and the **`err_unregister_audio`** label block.

### FXAS21002C (`fxas21002c_core.c`)

No source fix was required: **`fxas21002c_trigger_handler`** already matched the stable resolution (single **`regmap_bulk_read`** path, **`out_pm_put`**, **`iio_push_to_buffers_with_timestamp`**, etc.).

## Fix

### `adv7511_drv.c`

- **`adv7511_bridge_attach()`**: removed **`adv7533_attach_dsi()`** so DSI is **only** attached from **probe**, matching **v6.1.125**.
- **`adv7511_probe()`**: after **`adv7511_audio_init()`**, added **`adv7533_attach_dsi()`** for **ADV7533 / ADV7535** with **`goto err_unregister_audio`** on failure.
- Added **`err_unregister_audio:`** → **`adv7511_audio_exit()`**, **`drm_bridge_remove()`**, then the existing **`err_unregister_cec`** chain, **`uninit_regulators`**, **`err_of_node_put`**, **`return ret`**.
- Removed the broken **`#if CONFIG_OF_DYNAMIC`** block at the end of the error path and the unused graph / changeset locals at the top of probe.

**Note:** NXP’s optional **“detach remote OF graph endpoint on probe failure”** behavior was **dropped** in this cleanup so the driver matches **stable 6.1.125** semantics and avoids broken refcounting. It can be reintroduced later as a small helper with correct **`of_node`** lifetime rules.

## Reference

- Merge log: `merge-logs/merge_6.1.125_2026-05-05T16-20-39.553725.log`
- JSON: `merge-logs/merge_6.1.125_2026-05-05T16-20-39.553725.json`
- Upstream reference tag: **`v6.1.125`** — `drivers/gpu/drm/bridge/adv7511/adv7511_drv.c`
