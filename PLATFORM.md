# Platform Facts

## Project compatibility rule

The project supports only xHCI controllers reporting HCIVERSION >= 1.0. xHCI 0.x/0.96 controllers are explicitly unsupported and must be rejected before controller initialization. This is a project-level compatibility boundary, not a claim that every later xHCI feature is available; optional capabilities remain capability-detected.

This file contains observed platform facts and their evidence. It is not a list of assumptions. Unknown fields remain explicitly unknown until measured.

## Toshiba Satellite P50 — active xHCI test platform

Role: designated sacrificial machine for active xHCI experiments.

| Property | Value | Evidence/status |
| --- | --- | --- |
| xHCI PCI function | 00:14.0, Intel 8086:8c31 | Observed by E002/V29 |
| xHCI version | 1.00 | Observed by E002/V29; meets project minimum >= 1.0 |
| MMIO BAR | 0xF7C00000, 64-bit BAR encoding | Observed by E002/V29 |
| MaxSlots | 32 | Observed by E002/V29 |
| MaxInterrupters | 19 | Observed by E002 |
| MaxPorts | 18 | Observed by E002/V29 |
| AC64 | supported | Observed by E002/V29 |
| Scratchpad count | 16 | Observed and successfully provisioned/mapped by V29 |
| Context size | unknown | Record before device-context work |
| xHCI legacy ownership | unknown | Record before controller-running work |
| IOMMU/VT-d state | unknown | Record before Gate 4 controller start |
| EFI_PCI_IO DMA mapping | confirmed functional for V29 controller-referenced memory | V29 hardware result; device-visible addresses were obtained through Map() |
| External keyboard and port | unknown | Record before enumeration work |
| Boot medium and recovery steps | unknown | Record before next active test |

### V29 DMA observations

V29 successfully mapped and programmed controller-referenced memory while the controller remained halted. Observed device-visible addresses were:

- COMMAND/DCBAA: `0x00000000C67FF000`
- Scratchpad array: `0x00000000C6B00000`
- CRCR: `0x00000000C6B01000`
- ERDP: `0x00000000C6B02000`
- ERSTBA: `0x00000000C6B04000`

These observations establish that the EFI_PCI_IO_PROTOCOL mapping path is usable for the halted preparation gate. They do not yet establish the Toshiba's IOMMU/VT-d policy or prove safe DMA while the controller is running.

## Dell XPS 8950 — observation-only platform

The Dell is a production dual-boot machine. It must not be used for active xHCI, ownership, DMA, ring, interrupt, reset, or PCI-configuration experiments without an explicit review and acceptance.

| Property | Value | Evidence/status |
| --- | --- | --- |
| xHCI PCI function | 00:14.0, Intel 8086:7ae0 | E001 |
| xHCI version | 1.20 | E001; meets project minimum >= 1.0 |
| MMIO BAR | 0x4202120000, 64 KiB, 64-bit | E001 |
| MaxSlots / MaxInterrupters / MaxPorts | 64 / 8 / 25 | E001 |
| AC64 | supported | E001 |
| IOMMU group | 5 | Host observation; not a DMA-isolation conclusion |
