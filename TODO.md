# TODO

## Compatibility

- [ ] Reject xHCI controllers with HCIVERSION < 1.0 before any controller initialization.
- [ ] Keep optional/later xHCI features capability-detected rather than inferred solely from version.

## V29 / DMA contract — Gate 3 complete

- [x] Run V29 on the Toshiba and record the EFI_PCI_IO Map() device addresses.
- [x] Confirm scratchpad count and allocation behavior.
- [x] Confirm CONFIG/DCBAAP/CRCR/ERST state while halted, using only specification-defined meaningful readback.
- [x] Confirm teardown clears every controller pointer before DMA unmap/free.

## Gate 4 — Controller start without commands — complete

- [x] Define the portable DMA prerequisite: UEFI `EFI_PCI_IO_PROTOCOL` resolves platform-specific DMA/IOMMU mapping; do not make Toshiba VT-d state a test dependency.
- [x] Complete Gate 4 design review against the applicable xHCI specification and project staged plan.
- [x] Complete Gate 4 implementation review requirements against xHCI spec, coreboot/libpayload, Linux xhci-hcd, and UEFI/GNU-EFI before implementation.
- [x] Build and verify the exact source/binary provenance in CI.
- [x] Perform post-build sanity review before hardware test.
- [x] Start the controller without submitting a command or ringing a doorbell.
- [x] Observe running state with CPU interrupts disabled.
- [x] Verify event-ring/DMA lifetime while running.
- [x] Verify halt/reset recovery is repeatable on Toshiba.

## Gate 5 — One Enable Slot command + polled completion event — current

- [x] Define the command-ring design: one Enable Slot TRB, command PCS cycle state, Link TRB with Toggle Cycle.
- [x] Define the Protocol Slot Type lookup from the xHCI Supported Protocol capability.
- [x] Define the event-ring polling design with CPU interrupts disabled.
- [x] Define Command Completion Event validation: type, Success completion code, Slot ID, command TRB pointer.
- [x] Define ERDP advancement and IMAN.IP acknowledgement.
- [x] Preserve DMA mappings until controller halt/reset is confirmed.
- [ ] Complete implementation review against xHCI spec, coreboot/libpayload, Linux xhci-hcd, and UEFI/GNU-EFI.
- [ ] Build and verify exact source/binary provenance in CI.
- [ ] Perform post-build sanity review.
- [ ] Run V31 on Toshiba and observe one Enable Slot completion event.
- [ ] Verify returned Slot ID and command TRB pointer.
- [ ] Verify halt/reset recovery and DMA teardown after command completion.

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
- [x] Start the controller and poll its running state without commands (Gate 4).
- [ ] Submit one command and poll a real completion event (Gate 5).

## DMA / safety

- [x] Define/use a DMA mapping interface with explicit CPU and device addresses for V29/V30.
- [x] Define buffer lifetime and controller-pointer teardown rules through a running controller.
- [x] Verify that active V29/V30 DMA testing remained confined to designated sacrificial hardware.
- [ ] Record Toshiba IOMMU/VT-d state only if required to diagnose a UEFI mapping or running-controller failure; it is not a Gate 4 prerequisite.

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

- [x] Intel xHCI (Toshiba sacrificial hardware) halted initialization preparation — Gate 3.
- [x] Intel xHCI controller start without commands (Toshiba sacrificial hardware) — Gate 4.
- [ ] Intel xHCI one Enable Slot command + polled completion event — Gate 5.
- [ ] Second Intel generation and AMD xHCI (later, non-sacrificial).
- [ ] USB 2 HID and USB 3 HID.
- [ ] Devices behind a USB hub (later, out of initial scope).
