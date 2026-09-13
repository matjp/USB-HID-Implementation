# V32 Handoff Redesign — Code Requirements

## Purpose

This document records the required V32 source changes following the Gate 6 handoff redesign. It is implementation guidance, not permission to weaken the no-discovery boundary.

## Current V32 audit result

The current `efi/xhci_mmio_v32.c` already has important pieces of the intended architecture:

- UEFI selects HID keyboard/mouse interfaces;
- UEFI converts `ParentPortNumber` to the xHCI one-based root port;
- UEFI resolves controller-relative MMIO offsets;
- UEFI resolves a Supported Protocol Capability and Slot Type;
- UEFI disconnects the selected controller before active bridge programming;
- the bridge creates fresh rings, contexts and DMA state;
- the bridge does not perform a second `EFI_USB_IO_PROTOCOL` discovery scan.

However, the current V32 ABI is still structurally a V3/single-port handoff. It has one controller-wide `portsc_offset` and one controller-wide `slot_type`, and `resolve_xhci()` resolves those values from the keyboard root port. That is not sufficient for the final keyboard+mouse handoff contract.

## Required V32 changes

### 1. Replace the handoff structure with V4

Use `efi/xhci_bridge/xhci_handoff_v4.h` as the ABI definition.

The controller record contains immutable capability/access facts. Each HID device record contains its own resolved xHCI facts.

### 2. Make PORTSC and Slot Type per device

Remove the assumption that one `HANDOFF.portsc_offset` and one `HANDOFF.slot_type` apply to both devices.

Each keyboard/mouse record must carry:

- xHCI one-based root port;
- exact BAR-relative PORTSC offset;
- Supported Protocol Capability covering that port;
- Slot Type from that capability;
- speed evidence;
- complete relevant device path and HID/device facts.

### 3. Resolve both selected devices before disconnect

The UEFI producer must resolve the xHCI facts for the keyboard and, when present, the mouse before ownership transfer.

The two records must be associated with the same controller for this Gate 6 experiment. A second controller must be rejected rather than causing bridge-side controller discovery.

### 4. Bridge consumes resolved values directly

The bridge must use the selected device record's `root_port`, `portsc_offset` and `slot_type` directly.

It must not:

- add one to a root port;
- parse the UEFI device path;
- calculate PORTSC from the root port;
- walk Supported Protocol Capabilities;
- scan USB ports/devices.

### 5. Keep live state bridge-owned

Do not add UEFI Slot IDs, USB addresses, command/event rings, device contexts, transfer rings or DMA mappings to the handoff.

The bridge must continue to establish those afresh.

### 6. Gate 6 execution remains keyboard-only

The ABI must carry the mouse now so the discovery boundary is complete, but the first V32 hardware operation remains the selected keyboard through Address Device. Mouse traffic is later work.

## Required acceptance evidence

V32 source review must demonstrate:

- the V4 handoff is the only source of device topology/access facts after disconnect;
- keyboard and mouse each have independent resolved root-port/PORTSC/Slot-Type data;
- no bridge-side USB discovery exists;
- no port-number conversion occurs in the bridge;
- no PORTSC arithmetic based on root port exists in the bridge;
- no Supported Protocol Capability discovery occurs in the bridge;
- all live xHCI runtime resources remain bridge-owned and freshly allocated.

Hardware testing remains blocked until these source-level requirements are satisfied and CI proves the resulting EFI image contains the reviewed current V32 source.
