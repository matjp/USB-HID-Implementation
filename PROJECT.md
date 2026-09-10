# Minimal x86-64 OS — USB / xHCI Project State

Last updated: 2026-09-11

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
- **DMA portability boundary: the bridge does not inspect or depend on a platform-specific IOMMU/VT-d configuration. UEFI/EFI_PCI_IO_PROTOCOL owns platform-specific DMA mapping. Controller-referenced memory is allocated with `AllocateBuffer()` and mapped with `EfiPciIoOperationBusMasterCommonBuffer`; xHCI receives only the returned device-visible addresses, and mappings remain live until controller references are cleared.**
- Coreboot/libpayload and Linux xHCI driver are cross-reference sources only.

## V29 status — Gate 3 PASS on Toshiba

V29 successfully completed the halted xHCI initialization preparation gate on the Toshiba Satellite P50 using build commit `141a245dfa6f2580b956f8e3e1f89f30f770baf9`. The test reported xHCI 1.00, PCI 8086:8C31, 64-bit BAR 0xF7C00000, 32 slots, 16 scratchpads, AC64=1, HCH=1 and CNR=0 after reset. EFI_PCI_IO_PROTOCOL Map() supplied the device-visible DMA addresses for the controller-referenced memory. CONFIG, DCBAAP, CRCR and the primary event-ring registers were programmed and verified while halted. CRCR programming used the required split low-DWORD/high-DWORD MMIO access and no longer attempts invalid CRCR pointer readback verification.

Observed V29 result: `RESULT=Success`, `FAIL STAGE=NONE`, with 27 MMIO reads and 14 MMIO writes. Teardown cleared all controller pointers before DMA release. The controller was never started: no Run/Stop, doorbell, command execution, interrupt delivery, or USB transfer occurred. This is a hardware PASS for Gate 3 and does not yet validate a running controller or command completion.

## V30 status — Gate 4 PASS on Toshiba

V30 successfully proved the controller-start gate on the Toshiba Satellite P50. The test reused the V29 UEFI DMA contract, started the xHCI with valid CONFIG/DCBAAP/CRCR/event-ring state, kept CPU interrupt delivery disabled, observed `HCH=0`, then halted and reset the controller. No command, doorbell, USB transfer, or CPU interrupt occurred, and the event ring remained empty during the observation window.

Observed V30 result: `RUN: HCH=0 PASS`, `HALT: HCH=1 PASS`, `RESET: CNR=0 HCH=1 PASS`, `COMMANDS=0`, `DOORBELLS=0`, `CPU-INTERRUPTS=0`, `EVENTS=0`, `RESULT=Success`, `FAIL STAGE=NONE`. All controller pointers were cleared before DMA release. This is a hardware PASS for Gate 4 and establishes the safe baseline for command-ring operation.

## V31 status — Gate 5 PASS on Toshiba

V31 successfully completed the first command-ring/event-ring gate on the Toshiba Satellite P50. The keyboard was intentionally disconnected. The controller reported xHCI 1.00, 32 slots, 16 scratchpads, 4096-byte page size, Doorbell Offset 0x3000, Runtime Offset 0x2000 and Protocol Slot Type 0. Exactly one Enable Slot command was issued and exactly one Doorbell 0 write was made. The event ring produced exactly one Command Completion Event: type 33, Completion Code 1 (Success), Slot ID 1, and Command TRB Pointer matching the submitted command TRB device address `0x00000000C6803000`.

Observed result: `V31 ENABLE SLOT: PASS SLOT=1`, `COMMANDS=1`, `DOORBELLS=1`, `CPU-INTERRUPTS=0`, `EVENTS=1`, `RESET RECOVERY PASS`, `RESULT=Success`, `MMIO READS=66`, `WRITES=29`. All controller pointers were cleared before DMA release. This establishes the command-ring → controller → event-ring completion path on the Toshiba without involving a USB device.

## Gate 6 status — CURRENT

Gate 6 introduces exactly one pre-connected wired keyboard. V28's UEFI discovery snapshot supplies the expected root port/interface/endpoint facts, but those facts are constrained hints and validation inputs rather than authority to skip normal xHCI device setup. The bridge must independently reset the expected root port, enable a fresh slot, allocate/program device contexts, issue Address Device, retrieve the required live descriptors through EP0 control transfers, select the intended HID boot interface, set configuration, configure the xHCI endpoint, and set HID boot protocol.

The first Gate 6 implementation remains deliberately narrow: no mouse traffic, hubs, hot-plug, arbitrary HID report parsing, MSI/MSI-X, CPU interrupts, or continuous report polling. Command and transfer completions remain polled from the event ring. The Slot ID from V31 is not reused; every run starts from a fresh reset and obtains a new slot.

### Gate 6 review status

The Gate 6 design review is complete. The normative xHCI requirements establish the Enable Slot → Address Device → device configuration lifecycle and require software to sequence later commands from their completion events. UEFI common-buffer DMA remains the portable DMA boundary: the bridge uses mapped device addresses and keeps mappings live until controller references are safely eliminated.

Before implementation, the remaining mandatory review points are: PORTSC reset/change-bit handling; context-size and device-context layout; DCBAA slot entry and EP0 context; Address Device fields; control-transfer Setup/Data/Status TRBs; descriptor bounds and endpoint validation; separate USB SET_CONFIGURATION and xHCI Configure Endpoint operations; HID Set Protocol timing; and conservative DMA lifetime on every failure path.

The development loop remains: **design review → implementation review → commit → build/CI → post-build review → Toshiba hardware test only when all applicable gates pass.** No Gate 6 hardware test is authorized from documentation alone.

## DMA safety

V29, V30 and V31 are successful hardware tests using EFI_PCI_IO_PROTOCOL DMA mapping. Active xHCI/DMA experiments use the Toshiba (sacrificial) only; the Dell XPS 8950 is read-only. The portability requirement is that UEFI resolves the platform's DMA-addressing/remapping details and supplies device-visible addresses through `Map()`. Platform IOMMU/VT-d state may be recorded as diagnostic evidence if a mapping or running-controller failure requires explanation, but it is not a project dependency.

## Working records

- `ARCHITECTURE.md`: system design, service boundary, bridge layers, and ABI details.
- `DECISIONS.md`: design decisions log.
- `EXPERIMENTS.md`: chronological evidence log for hardware experiments and Gate 6 review.
- `IMPLEMENTATION_PLAN.md`: gated implementation and acceptance criteria.
- `TODO.md`: gated task list.

### Mandatory xHCI implementation cross-check
Every xHCI test that changes controller state or implements xHCI data structures must be reviewed against both the applicable xHCI specification and the coreboot/libpayload xHCI implementation. Coreboot is a practical implementation cross-check, not a substitute for the normative specification. Reviews must explicitly check controller reset/initialization sequencing, capability/register interpretation, PAGESIZE, DMA addressing, DCBAA, scratchpad buffers, command/event rings, and controller ownership assumptions where relevant.

### Minimal-driver scope rule
The Linux/coreboot cross-check is for correctness and edge-case discovery only. It must not expand the project's scope. This project remains a deliberately minimal xHCI/USB HID implementation: adopt only the hardware behavior and safeguards required by our stated design, and do not import unrelated Linux/coreboot features, abstractions, device classes, power-management support, quirks, or general USB functionality merely because those implementations contain them.

### Mandatory pre-build implementation review loop
Before committing any new or revised xHCI test artifact, perform an implementation review before attempting a build. The review must cross-check the relevant implementation against the xHCI specification, coreboot/libpayload, Linux xhci-hcd, and UEFI/GNU-EFI where applicable. It must check register semantics and ordering, alignment/page-size requirements, DMA mapping and address-width handling, xHCI data-structure layout, allocation/free symmetry, failure-path cleanup, error propagation, and hardware-specific assumptions. Only after the implementation review passes should the change be committed and built. CI is then a build/API/linkage verification step, not the primary implementation-correctness review. A successful build does not imply that the implementation is correct. After CI succeeds, perform a post-build sanity review before hardware testing.

The normal development loop is therefore: **design review → implementation review → commit → build/CI → post-build review → hardware test only when all applicable gates pass.**