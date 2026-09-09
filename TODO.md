# TODO

## Compatibility

- [ ] Reject xHCI controllers with HCIVERSION < 1.0 before any controller initialization.
- [ ] Keep optional/later xHCI features capability-detected rather than inferred solely from version.

## Consolidation (current)

- [ ] Consolidate V20–V27 into one self-contained xhci_bridge_init() with a portable platform operations layer.
- [ ] Combine halt → reset → CNR clear → capability validation → DMA allocation → DCBAA/command/event rings → readback → safe teardown into one program.
- [ ] Create a proper DMA-allocation layer with explicit CPU and device addresses; current tests use physical page addresses directly.
- [ ] Check and provision scratchpad buffers when the controller requires them.
- [ ] Retain all controller-referenced pages for the bridge lifetime.
- [ ] Clear xHCI pointers before freeing those pages during teardown.
- [ ] Correct and fold in V27's event-ring setup; it currently frees memory that its registers still reference.
- [ ] Start the controller and poll a real event ring.

## DMA / safety

- [ ] Determine how UEFI leaves xHCI DMA/IOMMU state on the Toshiba.
- [ ] Define a DMA mapping/allocation interface with explicit CPU and device addresses.
- [ ] Define buffer lifetime and controller-pointer teardown rules.
- [ ] Verify that active tests remain confined to designated sacrificial hardware.

## Fixed two-device bring-up

- [ ] Define the versioned known_hid_device template.
- [ ] UEFI snapshot of keyboard/mouse device facts.
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

- [ ] Intel xHCI (Toshiba sacrificial hardware).
- [ ] Second Intel generation and AMD xHCI (later, non-sacrificial).
- [ ] USB 2 HID and USB 3 HID.
- [ ] Devices behind a USB hub (later, out of initial scope).
