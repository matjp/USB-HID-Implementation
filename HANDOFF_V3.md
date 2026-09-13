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

## Fixed conversion rule

UEFI's `EFI_USB_DEVICE_PATH.ParentPortNumber` is zero-based. xHCI root-port numbering used by PORTSC and Slot Context is one-based. The producer therefore converts the UEFI device-path port exactly once when constructing the handoff. The bridge receives the resulting xHCI root-port value and does not perform that conversion again.

## Gate 6 acceptance evidence

A successful run must print the V3 handoff facts, including the exact `PORTSC` offset and `SLOT_TYPE`, and then show that the bridge used those supplied values for the live port and command path.

No hardware test is considered a pass merely because the image boots. The functional QEMU run must reach and report the V32 result stage.
