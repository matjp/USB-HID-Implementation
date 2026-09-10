# Minimal x86-64 OS — USB / xHCI Project State

Last updated: 2026-09-09

## Goal

Build a simple x86-64 OS kernel targeting modern PCs with UEFI firmware. Immediate hardware goal: USB keyboard + USB mouse via a reusable xHCI service boundary. The kernel sees an abstract input interface, not USB/xHCI details. xHCI lives outside the kernel in a portable service with a platform abstraction layer.

## Hardware milestone — V28/U20

V28/U20 successfully validated the explicit UEFI→service HID handoff on the Toshiba Satellite P50. The controller reported xHCI 1.00 (8086:8C31), and UEFI discovered a single composite Microsoft USB device on port 4 exposing both supported HID interfaces: keyboard IF=0 / EP=0x81 and mouse IF=1 / EP=0x82. The handoff contained 2 devices and their report descriptors (75 and 223 bytes) and completed with no direct xHCI MMIO writes, no service DMA, and no port reset. This is the current read-only hardware baseline for the handoff design.

## Current direction

- Target xHCI discovered via PCI class; hard compatibility boundary: xHCI 1.0 or later (HCIVERSION >= 1.0) only; xHCI 0.x/0.96 is explicitly unsupported. Fixed two-device scope (one external keyboard + one mouse).
- Prefer HID boot protocol; no hubs, hot-plug, arbitrary descriptors, or arbitrary HID report parsing.
- **UEFI is the discovery provider. It passes a versioned handoff record containing only keyboard/mouse discovery facts; non-HID USB devices are not exposed to the service.**
- **The handoff carries as much useful discovery state as practical: xHCI identity/capabilities and register offsets/page-size state, controller state, USB device/configuration/interface/endpoint facts, root-port information when available, and the HID report descriptor when UEFI can retrieve it.**
- **The handoff is a discovery snapshot, not an ownership transfer; the service does not inherit UEFI slot IDs, rings, contexts, DMA buffers, or live controller state.**
- UEFI performs device enumeration and passes a small device-template snapshot to the bridge; the bridge never inherits live UEFI xHCI state.
- Portable xHCI bridge core with platform ops layer: GNU-EFI test now, UEFI bridge later, kernel later if chosen.
- Require 64-bit MMIO BAR; treat BAR width, assigned address, and xHCI DMA capability as separate properties.
- Coreboot/libpayload and Linux xHCI driver are cross-reference sources only.

## V29 status

V29 is the next hardware experiment: halted controller initialization with explicit UEFI PCI-I/O DMA mapping. It is designed to replace V27's unverified physical-address assumption by obtaining device-visible addresses through EFI_PCI_IO_PROTOCOL Map() and using common buffers for controller-referenced structures. It remains halted and issues no commands or transfers.

## DMA safety

Before any Run/Stop or DMA, investigate the Toshiba's IOMMU/VT-d state and UEFI DMA mapping behavior. Active xHCI/DMA experiments use the Toshiba (sacrificial) only; the Dell XPS 8950 is read-only.

## Working records

- `ARCHITECTURE.md`: system design, service boundary, bridge layers, and ABI details.
- `DECISIONS.md`: design decisions log.
- `EXPERIMENTS.md`: chronological evidence log for hardware experiments.
- `TODO.md`: gated task list with acceptance criteria.


### Mandatory xHCI implementation cross-check
Every xHCI test that changes controller state or implements xHCI data structures must be reviewed against both the applicable xHCI specification and the coreboot/libpayload xHCI implementation. Coreboot is a practical implementation cross-check, not a substitute for the normative specification. Reviews must explicitly check controller reset/initialization sequencing, capability/register interpretation, PAGESIZE, DMA addressing, DCBAA, scratchpad buffers, command/event rings, and controller ownership assumptions where relevant.

### Minimal-driver scope rule
The Linux/coreboot cross-check is for correctness and edge-case discovery only. It must not expand the project's scope. This project remains a deliberately minimal xHCI/USB HID implementation: adopt only the hardware behavior and safeguards required by our stated design, and do not import unrelated Linux/coreboot features, abstractions, device classes, power-management support, quirks, or general USB functionality merely because those implementations contain them.

### Mandatory pre-build implementation review loop
Before committing any new or revised xHCI test artifact, perform an implementation review before attempting a build. The review must cross-check the relevant implementation against the xHCI specification, coreboot/libpayload, Linux xhci-hcd, and UEFI/GNU-EFI where applicable. It must check register semantics and ordering, alignment/page-size requirements, DMA mapping and address-width handling, xHCI data-structure layout, allocation/free symmetry, failure-path cleanup, error propagation, and hardware-specific assumptions. Only after the implementation review passes should the change be committed and built. CI is then a build/API/linkage verification step, not the primary implementation-correctness review. A successful build does not imply that the implementation is correct. After CI succeeds, perform a post-build sanity review before hardware testing.

The normal development loop is therefore: **design review → implementation review → commit → build/CI → post-build review → hardware test only when all applicable gates pass.**