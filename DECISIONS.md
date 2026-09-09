# Design Decisions

## D001 — Keep USB/xHCI out of the kernel

The preferred architecture places USB/xHCI in a separate service and gives the kernel a small input interface.

## D002 — Target xHCI

Modern PCs commonly expose USB through xHCI. The implementation should discover an xHCI controller through PCI rather than hard-code a machine-specific PCI address.

## D003 — Narrow USB scope

The first implementation targets one keyboard and one mouse rather than general USB device support.

## D004 — Prefer HID boot protocol initially

Boot-protocol USB HID keyboard/mouse support should minimize HID parsing complexity.

## D005 — Do not experiment with production DMA

The Dell XPS 8950 is a dual-boot production system. Active xHCI/DMA work moves to sacrificial hardware first.

## D006 — UEFI hands off device facts, not live xHCI state

UEFI performs device enumeration and passes a small device-template snapshot (known_hid_device) to the bridge. The bridge does not inherit or reuse UEFI's device slot IDs, command/transfer/event ring pointers, DCBAA, device-context addresses, DMA buffers, controller run state, or current USB address. UEFI's initial discovery contributes the root port, interface, endpoint, packet size, polling interval, and configuration value. The bridge verifies the expected device is connected to that root port after reset; the snapshot is a constrained recipe, not authority to skip xHCI setup.

## D007 — Coreboot is a code reference, not a firmware replacement

Coreboot/libpayload xHCI code may be reused or adapted as a source, but the target Dell cannot be flashed with coreboot and firmware replacement is not part of the project.

## D008 — Consolidate V20–V27 into a self-contained xhci_bridge_init()

The existing V03–V27 experiments prove individual register transitions but do not accumulate: each EFI binary starts in a fresh firmware-owned controller state. Rather than adding another isolated diagnostic branch, the next work consolidates halt → reset → CNR clear → DMA allocation → DCBAA/command/event rings → readback → safe teardown into one reusable `xhci_bridge_init()` with a portable platform operations layer.

## D009 — Start with empty device hints

The first `xhci_bridge_init()` test runs with zero device hints — full controller initialization, DMA allocation, ring setup, and safe teardown only. Keyboard and mouse hints are added only after clean initialization is repeatable.


## D010 — Support xHCI 1.0 and later only

The project has a hard compatibility boundary at xHCI 1.0. A controller is supported only when its HCIVERSION is >= 1.0. xHCI 0.x/0.96 controllers are explicitly unsupported and must be rejected before controller initialization.

This boundary removes the legacy 0.96 compatibility path from the project while retaining capability discovery for optional features and later xHCI revisions. The xHCI specification is the normative baseline; Linux, coreboot/libpayload, and EDK2 are implementation cross-references only.


## D011 — UEFI is the authoritative keyboard/mouse discovery provider

The service shall consume the maximum useful keyboard/mouse discovery information supplied by UEFI and shall not receive a general USB-device inventory. Only boot-protocol HID keyboard and mouse interfaces enter the handoff. The handoff is a versioned discovery snapshot, not a transfer of ownership of UEFI-created xHCI rings, contexts, DMA buffers, slot IDs, or live controller state.
