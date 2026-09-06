# TODO

## Immediate

- [ ] Obtain sacrificial x86-64 UEFI machine for active xHCI experiments.
- [ ] Record exact firmware/UEFI version on the Dell.
- [ ] Preserve the current known-good read-only EFI diagnostic.
- [ ] Add complete raw xHCI port register dump to the diagnostic.
- [ ] Decode and document the xHCI extended-capability chain.
- [ ] Establish exact xHCI BIOS/OS ownership state.

## DMA / safety

- [ ] Determine how UEFI leaves xHCI DMA/IOMMU state.
- [ ] Investigate Intel VT-d/IOMMU configuration on test hardware.
- [ ] Define controlled DMA-memory allocation.
- [ ] Ensure experimental xHCI DMA cannot reach production storage-related memory.

## Reusable implementation

- [ ] Inspect Coreboot/libpayload xHCI source in detail.
- [ ] Evaluate PCI assumptions.
- [ ] Evaluate DMA address-width assumptions.
- [ ] Evaluate allocation assumptions.
- [ ] Evaluate interrupt assumptions.
- [ ] Evaluate controller ownership handling.
- [ ] Compare other open-source xHCI implementations.
- [ ] Decide whether to adapt existing code or write a minimal driver.

## Minimal xHCI

- [ ] PCI discovery
- [ ] BAR mapping
- [ ] xHCI capability parsing
- [ ] controller reset
- [ ] DCBAA
- [ ] command ring
- [ ] event ring
- [ ] interrupter
- [ ] controller run
- [ ] root-port handling
- [ ] USB device enumeration
- [ ] USB control transfers
- [ ] USB interrupt transfers
- [ ] HID boot keyboard
- [ ] HID boot mouse

## Service

- [ ] Define kernel/service ABI
- [ ] Define IPC mechanism
- [ ] Implement keyboard event queue
- [ ] Implement mouse event queue
- [ ] Decide service lifetime/ownership model

## Testing

- [ ] Intel xHCI
- [ ] second Intel generation
- [ ] AMD xHCI
- [ ] USB 2 HID
- [ ] USB 3 HID
- [ ] devices behind a USB hub
