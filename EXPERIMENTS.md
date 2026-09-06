# Experiment Log

## E001 — Read-only xHCI diagnostic on Dell XPS 8950

Status: completed.

The GNU-EFI diagnostic successfully booted on the machine using read-only PCI/xHCI inspection and a compact screen-safe output format with an approximately 30-second delay before exit.

Observed xHCI:

- PCI 00:14.0
- Intel 8086:7ae0
- 64 KiB 64-bit BAR at 0x4202120000
- xHCI version 1.20
- 64 slots
- 8 interrupters
- 25 ports
- 64-bit addressing support
- DBOFF 0x3000
- RTSOFF 0x2000
- extended capabilities begin at 0x8000
- controller observed running

No intentional xHCI controller writes were performed.

## Safety rule

Any experiment that starts/configures xHCI DMA should be performed on sacrificial hardware first.
