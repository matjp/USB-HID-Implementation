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

## E002 — Read-only xHCI capability/DMA probe on Toshiba Satellite P50

Status: ready to run.

Purpose:

- Re-discover the xHCI controller by PCI class rather than relying on a generation-specific device ID.
- Record the 64-bit PCI BAR and actual assigned MMIO address.
- Decode HCIVERSION, HCSPARAMS, HCCPARAMS1/2, AC64, context size, scratchpad count, doorbell/runtime offsets, and controller state.
- Walk the xHCI extended-capability chain.
- Identify Supported Protocol capabilities and their compatible port ranges.
- Identify USB Legacy Support and USB Debug Capability locations without modifying them.

Safety:

- PCI configuration is read-only.
- xHCI MMIO accesses are read-only.
- No controller reset, ownership change, DMA, rings, interrupts, or doorbell writes.
- 30-second delay before exit.

Important verification note: the Supported Protocol capability's compatible port offset/count are in its third DWORD (capability offset + 0x08), not the preceding DWORD. This was checked against the xHCI specification before implementing E002.

Expected next evidence: compare Toshiba E002 results with the existing Dell observations before deciding whether any controller-initialization experiment is justified.

## Safety rule

Any experiment that starts/configures xHCI DMA should be performed on sacrificial hardware first.
