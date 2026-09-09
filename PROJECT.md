# Minimal x86-64 OS — USB / xHCI Project State

Last updated: 2026-09-09

## Goal

Build a simple x86-64 OS kernel targeting modern PCs with UEFI firmware. Immediate hardware goal: USB keyboard + USB mouse via a reusable xHCI service boundary. The kernel sees an abstract input interface, not USB/xHCI details. xHCI lives outside the kernel in a portable service with a platform abstraction layer.

## Current direction

- Target xHCI discovered via PCI class; hard compatibility boundary: xHCI 1.0 or later (HCIVERSION >= 1.0) only; xHCI 0.x/0.96 is explicitly unsupported. Fixed two-device scope (one external keyboard + one mouse).
- Prefer HID boot protocol; no hubs, hot-plug, arbitrary descriptors, or arbitrary HID report parsing.
- UEFI performs device enumeration and passes a small device-template snapshot to the bridge; the bridge never inherits live UEFI xHCI state.
- Portable xHCI bridge core with platform ops layer: GNU-EFI test now, UEFI bridge later, kernel later if chosen.
- Require 64-bit MMIO BAR; treat BAR width, assigned address, and xHCI DMA capability as separate properties.
- Coreboot/libpayload and Linux xHCI driver are cross-reference sources only.

## DMA safety

Before any Run/Stop or DMA, investigate the Toshiba's IOMMU/VT-d state and UEFI DMA mapping behavior. Active xHCI/DMA experiments use the Toshiba (sacrificial) only; the Dell XPS 8950 is read-only.

## Working records

- `ARCHITECTURE.md`: system design, service boundary, bridge layers, and ABI details.
- `DECISIONS.md`: design decisions log.
- `EXPERIMENTS.md`: chronological evidence log for hardware experiments.
- `TODO.md`: gated task list with acceptance criteria.
