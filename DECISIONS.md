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

## D006 — UEFI is not the USB runtime architecture

UEFI services may be useful during boot, but a persistent UEFI virtual serial service over arbitrary USB ports is not currently considered a suitable general architecture.

## D007 — Coreboot is a code reference, not a firmware replacement

Coreboot/libpayload xHCI code may be reused or adapted as a source, but the target Dell cannot be flashed with coreboot and firmware replacement is not part of the project.
