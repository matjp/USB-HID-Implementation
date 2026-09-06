# Minimal x86-64 OS — USB / xHCI Project State

Last updated: 2026-09-07

## Goal

Build a simple x86-64 operating-system kernel targeting reasonably modern PC hardware with UEFI firmware. The immediate hardware goal is USB keyboard + USB mouse, while keeping USB/xHCI implementation out of the kernel if practical.

## Preferred architecture

```text
Kernel
  |
  | small interface
  v
USB service
  |-- xHCI driver
  |-- minimal USB handling
  |-- HID keyboard
  |-- HID mouse
  v
xHCI controller
  |-- keyboard
  `-- mouse
```

The kernel should see an abstract input interface rather than USB/xHCI details.

## Current direction

- Target xHCI as the common modern USB controller.
- Initially support one keyboard and one mouse.
- Prefer USB HID boot-protocol keyboard and mouse.
- Avoid implementing a complete general-purpose USB stack.
- Prefer a separate USB service over putting xHCI code in the kernel.
- UEFI is currently viewed primarily as the boot environment, not as the long-term USB runtime abstraction.
- Coreboot/libpayload is a possible source of reusable xHCI code; coreboot firmware replacement is not required.

## Safety

The current Dell is a dual-boot Linux/Windows machine and its SSD data must not be put at risk.

Read-only xHCI diagnostics are acceptable. Active xHCI initialization and DMA experiments should first move to sacrificial hardware.

Before active DMA experiments, investigate IOMMU/DMA isolation.

## Current phase

Read-only xHCI/PCI diagnostics and architecture investigation.

Next major hardware step: obtain an inexpensive sacrificial x86-64 UEFI machine for active xHCI experiments.
