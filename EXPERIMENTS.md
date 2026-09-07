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

Status: completed.

Purpose:

- Re-discover the xHCI controller by PCI class rather than relying on a generation-specific device ID.
- Record the 64-bit PCI BAR and actual assigned MMIO address.
- Decode HCIVERSION, HCSPARAMS, HCCPARAMS1/2, AC64, context size, scratchpad count, doorbell/runtime offsets, and controller state.
- Walk the xHCI extended-capability chain.
- Identify Supported Protocol capabilities and their compatible port ranges.
- Identify USB Legacy Support and USB Debug Capability locations without modifying them.

Observed Toshiba controller:

- PCI 00:14.0
- Intel 8086:8c31
- BAR base 0xF7C00000 (64-bit BAR encoding, but assigned address is below 4 GiB)
- xHCI version 1.00
- 32 slots
- 19 interrupters
- 18 ports
- AC64=1
- controller observed running
- Supported Protocol capability reported the root-port range covering the controller's ports

Safety:

- PCI configuration was read-only.
- xHCI MMIO accesses were read-only.
- No controller reset, ownership change, DMA, rings, interrupts, or doorbell writes.

Important verification note: the Supported Protocol capability's compatible port offset/count are in its third DWORD (capability offset + 0x08), not the preceding DWORD. This was checked against the xHCI specification before implementing E002.

## E003 — One-MMIO-read isolation test on Toshiba Satellite P50

Status: next test.

Purpose:

- Isolate the previously observed instability to the smallest possible MMIO operation.
- Discover the xHCI controller by PCI class.
- Read the PCI BAR through configuration space.
- Perform exactly ONE read through EFI_PCI_IO_PROTOCOL.Mem.Read at BAR0 offset 0.
- Decode only CAPLENGTH and HCIVERSION from that single DWORD.

Safety boundary:

- No PCI configuration writes.
- No xHCI MMIO writes.
- No operational-register reads.
- No port-status reads.
- No extended-capability reads.
- No DMA, rings, interrupts, controller reset, or ownership changes.
- No direct CPU pointer dereference of the BAR.
- 30-second exit delay.

Rationale:

The previous successful Toshiba diagnostics establish that PCI discovery and configuration-space reads work. This experiment deliberately avoids all operational/port/extended MMIO reads so that a failure can be attributed specifically to the first MMIO access path. The EFI PCI I/O protocol is used for the MMIO access rather than directly dereferencing the physical BAR address.

Expected Toshiba result if the MMIO path is healthy:

- PCI 00:14.0 / 8086:8c31
- BAR base 0xF7C00000
- MMIO[000] should encode a plausible xHCI CAPLENGTH and HCIVERSION, consistent with the earlier E002 observation (CAPLENGTH 0x80, version 0x0100).

## Safety rule

Any experiment that starts/configures xHCI DMA should be performed on sacrificial hardware first.
