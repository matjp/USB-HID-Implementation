# Hardware Notes

## Dell XPS 8950

- Intel Alder Lake platform
- Intel Z690/PCH
- UEFI
- SMBIOS 3.4.0
- Dell baseboard 09D2HH, A00

### xHCI

- PCI address: 00:14.0
- Vendor/device: 8086:7ae0
- Intel Alder Lake-S PCH USB 3.2 Gen 2x2 xHCI Controller
- Revision: 11
- PCI class: 0c03, programming interface 30
- BAR0: 0x4202120000
- BAR0 size: 64 KiB
- BAR0: 64-bit, non-prefetchable MMIO
- Linux driver: xhci_hcd / xhci_pci
- IOMMU group: 5

The xHCI MMIO BAR is above 4 GiB. A driver must not assume 32-bit MMIO BAR addresses.

Read-only diagnostic observations:

- CAPLENGTH = 0x80
- HCIVERSION = 0x0120
- MaxSlots = 64
- MaxInterrupters = 8
- MaxPorts = 25
- 64-bit addressing supported
- DBOFF = 0x3000
- RTSOFF = 0x2000
- extended capability pointer = 0x8000
- controller observed RUN=1, HCH=0

A reported DCBAAP was 0x000000007493F000. It was read only and must not be assumed safe to reuse.

A reported CRCR was approximately 0x0000000000000008. It has not been fully interpreted and must not be modified based solely on the diagnostic.

### Root ports

The controller reports 25 root-hub ports. The compact diagnostic showed non-default status for P08, P10, P14 and P24. Their compact representations were:

    P08: 1101S1XY0
    P10: 1101S2XY0
    P14: 1101S1XY0
    P24: 1101S4XY0

The exact meaning of this compact representation and the corresponding raw PORTSC values still need verification.

## Serial devices

### ttyS0

- I/O port: 0x3f8
- IRQ: 4
- UART: 16550A
- Linux base baud: 115200

### ttyS4

- MMIO: 0x4202140000
- IRQ: 16
- UART: 16550A
- Linux reports dw-apb-uart.6
- setserial reports Baud_base: 9600

Associated PCI device 00:1e.0 is Intel Alder Lake-S PCH Serial IO UART #0, 8086:7aa8, BAR0 0x4202140000, 4 KiB, 64-bit MMIO, Linux driver intel-lpss.

Serial is useful as a possible debugging/console interface, but is not assumed to provide keyboard/mouse input.

## PCI discovery

The eventual implementation must discover the xHCI controller rather than assuming 00:14.0. The Dell address is a test-machine observation only.
