# Implementation Plan

Each gate has a narrow objective and explicit exit criteria. A failed or ambiguous result stops progression until it is recorded and understood.

## Gate 0 — Evidence and platform baseline

Reconcile the experiment history, retain the known-good read-only output, and complete the unknown Toshiba fields in `PLATFORM.md` that affect initialization: firmware, scratchpads, context size, legacy ownership, IOMMU state, external test keyboard, boot medium, and recovery procedure.

Exit criteria:

- The binary and machine used for each recorded active result are identifiable.
- The controller's required scratchpad count and addressing capability are known.
- There is a documented, repeatable recovery procedure.

## Gate 1 — DMA contract

Create a small DMA abstraction that records the CPU allocation, device-visible address, allocation size, alignment, and release operation. Do not assume that a UEFI physical address is automatically the address usable by xHCI DMA. Use the PCI I/O mapping facility or document verified identity mapping.

Exit criteria:

- Every controller-programmed pointer is traceable to a live DMA allocation.
- Buffer memory remains live until controller references are cleared or a reset makes them invalid.
- No test uses the Dell for DMA work.

## Gate 2 — Halted initialization preparation

Replace the draft V27 with one self-contained test:

`halt -> reset -> wait for reset completion -> wait for CNR clear -> validate capabilities -> allocate DMA objects -> program CONFIG, DCBAAP, CRCR, and primary-event-ring registers -> read back -> clear pointers -> release memory`

The test must provision scratchpad infrastructure when required, remain halted, leave interrupts disabled, issue no doorbell, and submit no command.

Exit criteria:

- Register values read back correctly.
- The controller remains halted and ready.
- Teardown clears every controller reference before memory is released.

## Gate 3 — Controller start without commands

Retain valid ring memory, enable only the required event-ring state, start the controller, and observe that it reaches the expected running state. Do not submit a command, ring a doorbell, or enable CPU interrupt delivery.

Exit criteria:

- The controller starts without host-system error or unexpected reset.
- The event-ring and DMA lifetime model remains valid while running.
- Halt/reset recovery is repeatable.

## Gate 4 — Command-ring completion by polling

Submit one Enable Slot command and poll the event ring for its completion. Keep CPU interrupt delivery disabled; polling isolates event-ring correctness from interrupt routing.

Exit criteria:

- The completion event is valid and yields a slot ID.
- Event dequeue acknowledgement works.
- The controller remains healthy after recovery.

## Gate 5 — One pre-connected wired keyboard

Use a single external wired keyboard connected before boot. Implement root-port identification/reset, Address Device, device and configuration descriptor retrieval, HID boot-interface selection, Set Configuration, and Set Protocol.

Exit criteria:

- Descriptors are decoded from the intended device.
- The selected HID interface is demonstrably boot-protocol capable.

## Gate 6 — Keyboard reports, then mouse

Create an interrupt-IN transfer ring and initially poll the event ring for reports. Translate boot-keyboard reports into a small internal event queue. Add boot-mouse handling only after keyboard operation is stable.

Exit criteria:

- Keyboard make, break, modifier, and rollover behaviour is recorded and correct.
- Mouse support does not change the keyboard path's behaviour.

## Gate 7 — OS integration and portability

Implement the execution model described in `ARCHITECTURE.md`: controller ownership, startup/shutdown across `ExitBootServices`, the USB-service lifetime, the input-event ABI, queue ownership, and backpressure. Only then expand testing to additional hardware, USB 3 devices, and hubs.

Exit criteria:

- The kernel uses only the documented input-service ABI.
- USB/xHCI state has a defined owner for its full lifetime.
- Compatibility testing begins only after the Toshiba happy path is repeatable.
