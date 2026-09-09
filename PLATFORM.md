# Platform Facts

This file contains observed platform facts and their evidence. It is not a list of assumptions. Unknown fields remain explicitly unknown until measured.

## Toshiba Satellite P50 — active xHCI test platform

Role: designated sacrificial machine for active xHCI experiments.

| Property | Value | Evidence/status |
| --- | --- | --- |
| xHCI PCI function | 00:14.0, Intel 8086:8c31 | Observed by E002 |
| xHCI version | 1.00 | Observed by E002 |
| MMIO BAR | 0xF7C00000, 64-bit BAR encoding | Observed by E002 |
| MaxSlots | 32 | Observed by E002 |
| MaxInterrupters | 19 | Observed by E002 |
| MaxPorts | 18 | Observed by E002 |
| AC64 | supported | Observed by E002 |
| Firmware version | unknown | Record before next active test |
| Scratchpad count | unknown | Record before next active test |
| Context size | unknown | Record before next active test |
| xHCI legacy ownership | unknown | Record before next active test |
| IOMMU/VT-d state | unknown | Record before DMA-capable test |
| External keyboard and port | unknown | Record before enumeration work |
| Boot medium and recovery steps | unknown | Record before next active test |

## Dell XPS 8950 — observation-only platform

The Dell is a production dual-boot machine. It must not be used for active xHCI, ownership, DMA, ring, interrupt, reset, or PCI-configuration experiments without an explicit review and acceptance.

| Property | Value | Evidence/status |
| --- | --- | --- |
| xHCI PCI function | 00:14.0, Intel 8086:7ae0 | E001 |
| xHCI version | 1.20 | E001 |
| MMIO BAR | 0x4202120000, 64 KiB, 64-bit | E001 |
| MaxSlots / MaxInterrupters / MaxPorts | 64 / 8 / 25 | E001 |
| AC64 | supported | E001 |
| IOMMU group | 5 | Host observation; not a DMA-isolation conclusion |
