# Gate 6 Handoff V3 — UEFI-resolved xHCI access facts

## Purpose

The Gate 6 architecture is **UEFI discovers; the bridge executes**. The UEFI producer is authoritative for USB discovery and resolves the controller/device information that can be known before ownership is transferred.

The bridge must not reconstruct xHCI addresses from USB discovery facts when UEFI can supply the resolved value.

## V3 handoff additions

The version-3 handoff supplies these controller-relative MMIO facts:

- `operational_offset` — xHCI operational register block offset from the PCI MMIO BAR used by the EFI PCI I/O access layer.
- `doorbell_offset` — exact Host Controller Doorbell 0 offset used for command submission.
- `runtime_offset` — xHCI runtime-register block offset.
- `interrupter0_offset` — exact primary interrupter-0 register block offset.
- `portsc_offset` — exact PORTSC offset for the UEFI-selected root port.
- `slot_type` — Slot Type resolved from the Supported Protocol Capability covering the selected root port.

The handoff also supplies the exact offsets needed for CRCR, DCBAAP, CONFIG, USBCMD/USBSTS and the primary event-ring registers used by V32.

The selected keyboard and optional mouse continue to carry their UEFI device-path and HID interface facts.

## Ownership boundary

All USB discovery, device-path interpretation, root-port conversion, Supported Protocol Capability lookup, and xHCI register-offset resolution happen in the UEFI producer **before** `DisconnectController(controller, NULL, NULL)`.

After disconnect, the bridge consumes the copied handoff values. It may validate that immutable controller facts still match the handoff, but it must not rediscover or recompute them.

In particular, the bridge must not calculate:

`PORTSC = operational_base + 0x400 + (root_port - 1) * 0x10`

It uses `portsc_offset` supplied by UEFI directly.

Likewise it uses the supplied doorbell/runtime/interrupter offsets directly.

## Why offsets rather than physical addresses?

The current EFI bridge uses `EFI_PCI_IO_PROTOCOL.Mem.Read/Write`, whose register argument is an offset within a PCI memory BAR. Therefore the portable handoff value is the **exact BAR-relative MMIO offset**, not a firmware-specific virtual or physical mapping.

The PCI controller identity/path remains part of the handoff so the bridge can prove that the PCI I/O handle it uses is the controller selected by UEFI.

## Root-port numbering

`EFI_USB_DEVICE_PATH.ParentPortNumber` is zero-based. xHCI root-port numbering used by PORTSC and Slot Context is one-based. The producer converts the selected UEFI device-path value exactly once (`ParentPortNumber + 1`) when constructing the handoff. The bridge consumes the resulting xHCI root-port value directly.

## What remains bridge-owned

The bridge still creates and owns state that does not exist until the fresh xHCI instance is initialized:

- command/event rings;
- DCBAA and device contexts;
- EP0 transfer ring;
- scratchpad allocations;
- DMA mappings and device-visible addresses;
- fresh Slot ID and USB device address;
- live PORTSC speed after controller start/reset.

These are runtime state, not UEFI discovery facts, and are intentionally not inherited from UEFI.

## DMA portability rule

Gate 6 uses EFI_PCI_IO_PROTOCOL as the DMA abstraction boundary and deliberately operates with **32-bit device-visible DMA addresses**. It does not reject a controller merely because HCCPARAMS1.AC64 is clear.

Before controller-referenced DMA allocation, the bridge disables `EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE` when active/supported. Each `Map(...BusMasterCommonBuffer...)` result must have a `DeviceAddress` no greater than `0xffffffff`. The allocation and mapping remain live until xHCI references are cleared.

This is a generic portability constraint, not a Toshiba-specific workaround. The bridge must not assume identity-mapped physical memory or a particular IOMMU/VT-d configuration.

## CI acceptance rule

The EFI image ends with `PRESS ANY KEY` after printing its result. CI waits for the result screen, captures it before sending a key, then releases the EFI image. QEMU boot/process success is not sufficient: CI must fail unless the captured result contains `RESULT: Success`.

The current V32 run reached the result stage but failed at:

`RESULT: FAIL STAGE=PORT OP=HANDOFF PORT STATUS=Device Error`

This is a failure at the handoff root-port bounds check, before live PORTSC connection testing. The next diagnostic revision should print the handoff root-port value and controller maximum-port value before this check. Do not change the port-numbering rule or add bridge discovery until those values are understood.
