# Experiment Log

## Compatibility boundary

All active and future controller experiments in this project require HCIVERSION >= 1.0. xHCI 0.x/0.96 controllers are explicitly unsupported and must be rejected before any controller initialization or active experiment.

## Current status

- **E001 (Dell XPS 8950)**: Read-only diagnostic completed. Reports xHCI 1.20, 64 slots, 25 ports, controller was running. No writes performed.
- **E002 (Toshiba Satellite P50)**: Read-only capability/DMA probe completed. Reports xHCI 1.00, 32 slots, 18 ports, controller running. Supported Protocol capability documented.
- **E003–E004**: Read-only BAR/MIOM isolation tests planned but not yet executed.
- **Historical series V03–V27**: Source implementations exist. V27 is a draft and must not be booted until reset ownership, scratchpad handling, DMA address translation, and teardown lifetime are reviewed and corrected.


- **U20 (Toshiba Satellite P50)**: V28/U20 UEFI→service HID handoff validation completed successfully. Controller reports xHCI 1.00, PCI 8086:8C31, BAR 0xF7C00000, OPBASE 0xB0; capabilities: 32 slots, 8 interrupters, 19 ports, AC64=1, HIGH=1, CNR=0. One physical Microsoft VID 045E/PID 07B2 composite USB device on port 4 exposed two supported HID interfaces: keyboard IF=0, EP=0x81, MPS=8, interval=4, report descriptor 75 bytes; mouse IF=1, EP=0x82, MPS=10, interval=1, report descriptor 223 bytes. UEFI reported 6 USB I/O handles, 2 HID candidates, 2 handoff devices. Handoff magic=0x48494458, version=1, size=744. Result: Success. No direct xHCI MMIO writes, no service DMA, no port reset; discovery remained UEFI-only. This validates the UEFI→service discovery boundary and keyboard/mouse-only filtering on the Toshiba.

## Recording rules

An experiment is marked **completed** only when its result was observed on hardware and its output or a faithful transcript is retained. A source file or Git commit records implementation work, not a hardware result. Each active experiment must record its machine, boot medium, exact binary revision, preconditions, observed output, recovery action, and whether a power cycle was required.

- **V28**: UEFI -> service HID handoff discovery test added. V28 is read-only: no xHCI MMIO writes, no DMA allocation, no port reset, no xHCI ring setup.

- **V29**: halted xHCI initialization preparation artifact. Uses EFI_PCI_IO_PROTOCOL common-buffer allocation/mapping for controller-referenced memory; performs halt/reset/CNR-clear and programs CONFIG, DCBAAP, CRCR and primary event-ring registers while halted. No Run/Stop, doorbell, command, or transfer. Hardware execution is on Toshiba only and remains subject to the DMA safety gate.

## Project focus

**Consolidation task**: Transform V24–V27 into one self-contained `xhci_bridge_init()` with a portable platform operations layer. The next experimental test (V28) runs this routine with **empty device hints** — halt → reset → CNR clear → capability validation → DMA allocation → DCBAA/command/event rings → readback → safe teardown — still issues no commands and touches no USB device.

**DMA safety**: Before any Run/Stop or DMA, investigate the Toshiba's IOMMU/VT-d state and UEFI DMA mapping behavior. Active xHCI/DMA experiments use the Toshiba (sacrificial) only; the Dell XPS 8950 is read-only.

**Fixed two-device bring-up**: Use a versioned `known_hid_device` snapshot from UEFI to reset the known root port, enable Slot, Address Device, and Configure Endpoint while keeping the scope to keyboard and mouse only. No hubs, hot-plug, or arbitrary descriptors.

## Safety rule

Any experiment that starts/configures xHCI DMA should be performed on sacrificial hardware first.

## Platform for future work

- E003–E004 (read-only MMIO BAR tests) are intentionally between PCI configuration probing and MMIO. If they succeed while E003 fails, the evidence points at the EFI MMIO access path rather than PCI enumeration or BAR interpretation.
