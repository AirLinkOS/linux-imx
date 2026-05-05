# v6.1.81 — `drivers/pci/controller/dwc/pci-layerscape-ep.c`

## Problem

The automated merge of **linux-stable v6.1.81** into this NXP **linux-imx** tree mishandled `pci-layerscape-ep.c`. The merge tool’s own verification pass reported **VERIFICATION CRITICAL**.

Mainline **6.1.81** Reworks Layerscape PCIe **endpoint** handling: it adds **PEX_PF0_CONFIG / PEX_PF0_CFG_READY**, saves the full **Link Capabilities** value in a **`lnkcap`** field (via **PCI_EXP_LNKCAP**), and updates the **IRQ handler** to restore **LNKCAP** after link-down / hot reset, set **CFG_READY**, and call **`dw_pcie_ep_linkup()`**.

The bad merge:

- Inserted a second copy of **register macros inside `struct ls_pcie_ep`**, which is invalid C and **dropped the real struct members** (`irq`, `lnkcap`, `big_endian`) that the rest of the file still uses.
- Left **probe** code that referenced **NXP-only** `max_speed` / `max_width` fields that mainline had replaced with **`lnkcap`**, so the result was internally inconsistent and would not build or would mis-model the device.

## Fix

The driver was realigned with **`v6.1.81`’s** `pci-layerscape-ep.c` for all **mainline** behavior:

- All **#define**s at **file scope** only; **`struct ls_pcie_ep`** contains **`irq`**, **`u32 lnkcap`**, **`bool big_endian`**.
- **Probe** caches **`pcie->lnkcap`** using **`dw_pcie_find_capability()`** and **`PCI_EXP_LNKCAP`** (no separate max speed/width fields).
- **Event handler** matches stable: clear **PME** status, on link-up restore **LNKCAP**, set **PEX_PF0_CFG_READY**, **dw_pcie_ep_linkup()**.

**NXP carry-overs** (intentional):

- **`fsl,ls1028a-pcie-ep`** in the **of_device_id** table (not in stable’s list).
- **64-bit `dma_set_mask_and_coherent()`** after the **big-endian** property read, with **`#include <linux/dma-mapping.h>`**.

## Reference

- Merge log: `merge-logs/merge_6.1.81_2026-04-30T11-50-20.450481.log`
- Upstream reference tag: **`v6.1.81`** — `drivers/pci/controller/dwc/pci-layerscape-ep.c`
