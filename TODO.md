# TODO

## Compatibility

- [ ] Reject xHCI controllers with HCIVERSION < 1.0 before any controller initialization.
- [ ] Keep optional/later xHCI features capability-detected rather than inferred solely from version.

## V29 / DMA contract — Gate 3 complete

- [x] Run V29 on the Toshiba and record the EFI_PCI_IO Map() device addresses.
- [x] Confirm scratchpad count and allocation behavior.
- [x] Confirm CONFIG/DCBAAP/CRCR/ERST state while halted, using only specification-defined meaningful readback.
- [x] Confirm teardown clears every controller pointer before DMA unmap/free.

## Gate 4 — Controller start without commands — current

- [ ] Record Toshiba IOMMU/VT-d state and relevant UEFI DMA ownership/mapping conditions before starting xHCI.
- [ ] Perform design review against the applicable xHCI specification and project staged plan.
- [ ] Perform implementation review against xHCI spec, coreboot/libpayload, Linux xhci-hcd, and UEFI/GNU-EFI.
- [ ] Build and verify the exact source/binary provenance in CI.
- [ ] Perform post-build sanity review before hardware test.
- [ ] Start the controller without submitting a command or ringing a doorbell.
- [ ] Observe running state with CPU interrupts disabled.
- [ ] Verify event-ring/DMA lifetime while running.
- [ ] Verify halt/reset recovery is repeatable.

## UEFI -> service handoff

- [ ] Freeze the versioned keyboard/mouse-only handoff ABI after V28 review.
- [ ] Define how the service receives the handoff after ExitBootServices.
- [ ] Define ownership semantics for any UEFI-created resources; default is no live xHCI resource inheritance.

## Consolidation

- [ ] Consolidate V20–V27 into one self-contained xhci_bridge_init() with a portable platform operations layer.
- [x] Combine halt → reset → CNR clear → capability validation → DMA allocation → DCBAA/command/event rings → readback → safe teardown into one program for Gate 3.
- [x] Create a DMA-allocation path with explicit CPU and device addresses for controller-referenced memory; V29 uses EFI_PCI_IO_PROTOCOL mapping rather than assuming identity mapping.
- [x] Check and provision scratchpad buffers when the controller requires them for Gate 3.
- [x] Retain all controller-referenced pages until controller pointers are cleared.
- [x] Clear xHCI pointers before freeing those pages during teardown.
- [x] Correct and fold in V27's event-ring setup for the halted preparation gate.
- [ ] Start the controller and poll a real event ring (Gate 4/5).

## DMA / safety

- [ ] Determine how UEFI leaves xHCI DMA/IOMMU state on the Toshiba before Run/Stop.
- [x] Define/use a DMA mapping interface with explicit CPU and device addresses for V29.
- [x] Define buffer lifetime and controller-pointer teardown rules for V29.
- [x] Verify that active V29 DMA testing remained confined to designated sacrificial hardware.

## Fixed two-device bring-up

- [ ] Define the versioned known_hid_device template.
- [x] UEFI snapshot of keyboard/mouse device facts.
- [ ] Port reset for the known root port.
- [ ] Enable Slot, Address Device, Configure Endpoint using hint values.
- [ ] Verify slot/device context is valid.

## Runtime event pump and serial conversion

- [ ] Keep one interrupt-IN transfer queued for keyboard.
- [ ] Keep one interrupt-IN transfer queued for mouse.
- [ ] Poll the event ring (no CPU interrupts initially).
- [ ] Decode boot-protocol report and serialize into input frame.
- [ ] Queue next transfer immediately after successful completion.
- [ ] Restart complete fixed two-device setup on failure.
- [ ] Define serial frame format and virtual serial FIFO.

## Service boundary

- [ ] Define kernel-visible input event structure and queue semantics.
- [ ] Define IPC mechanism.
- [ ] Document service lifetime model.

## Testing

- [x] Intel xHCI (Toshiba sacrificial hardware) halted initialization preparation.
- [ ] Second Intel generation and AMD xHCI (later, non-sacrificial).
- [ ] USB 2 HID and USB 3 HID.
- [ ] Devices behind a USB hub (later, out of initial scope).
