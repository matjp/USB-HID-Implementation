# Experiment Log

## Current status

- **E001 (Dell XPS 8950)**: Read-only diagnostic completed. Reports xHCI 1.20, 64 slots, 25 ports, controller was running. No writes performed.
- **E002 (Toshiba Satellite P50)**: Read-only capability/DMA probe completed. Reports xHCI 1.00, 32 slots, 18 ports, controller running. Supported Protocol capability documented.
- **E003–E004**: Read-only BAR/MIOM isolation tests planned but not yet executed.
- **Historical series V03–V27**: Source implementations exist. V27 is a draft and must not be booted until reset ownership, scratchpad handling, DMA address translation, and teardown lifetime are reviewed and corrected.

## Recording rules

An experiment is marked **completed** only when its result was observed on hardware and its output or a faithful transcript is retained. A source file or Git commit records implementation work, not a hardware result. Each active experiment must record its machine, boot medium, exact binary revision, preconditions, observed output, recovery action, and whether a power cycle was required.

## Project focus

**Consolidation task**: Transform V24–V27 into one self-contained `xhci_bridge_init()` with a portable platform operations layer. The next experimental test (V28) runs this routine with **empty device hints** — halt → reset → CNR clear → capability validation → DMA allocation → DCBAA/command/event rings → readback → safe teardown — still issues no commands and touches no USB device.

**DMA safety**: Before any Run/Stop or DMA, investigate the Toshiba's IOMMU/VT-d state and UEFI DMA mapping behavior. Active xHCI/DMA experiments use the Toshiba (sacrificial) only; the Dell XPS 8950 is read-only.

**Fixed two-device bring-up**: Use a versioned `known_hid_device` snapshot from UEFI to reset the known root port, enable Slot, Address Device, and Configure Endpoint while keeping the scope to keyboard and mouse only. No hubs, hot-plug, or arbitrary descriptors.

## Safety rule

Any experiment that starts/configures xHCI DMA should be performed on sacrificial hardware first.

## Platform for future work

- E003–E004 (read-only MMIO BAR tests) are intentionally between PCI configuration probing and MMIO. If they succeed while E003 fails, the evidence points at the EFI MMIO access path rather than PCI enumeration or BAR interpretation.
