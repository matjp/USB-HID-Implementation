# Implementation Plan

Each gate has a narrow objective and explicit exit criteria. A failed or ambiguous result stops progression until it is recorded and understood.

## Gate 0 — Evidence and platform baseline

Reject any controller reporting HCIVERSION < 1.0 before initialization. Reconcile the experiment history, retain the known-good read-only output, and complete the unknown Toshiba fields in `PLATFORM.md` that affect initialization: firmware, scratchpads, context size, legacy ownership, external test keyboard, boot medium, and recovery procedure.

Exit criteria:

- The binary and machine used for each recorded active result are identifiable.
- The controller reports HCIVERSION >= 1.0; older xHCI revisions are rejected before initialization.
- The controller's required scratchpad count and addressing capability are known.
- There is a documented, repeatable recovery procedure.

## Gate 1 — DMA contract

Create a small DMA abstraction that records the CPU allocation, device-visible address, allocation size, alignment, and release operation. Do not assume that a UEFI physical address is automatically the address usable by xHCI DMA. Use the UEFI PCI I/O mapping facility and treat the returned `DeviceAddress` as authoritative for controller DMA.

The portable contract is platform-neutral: the bridge does not inspect or depend on a particular machine's IOMMU/VT-d configuration. UEFI/PCI firmware owns the platform-specific DMA mapping. The bridge must keep each mapping live for the entire period in which the controller can reference it and must not release it until controller references have been cleared or the controller has been reset into a state where those references are no longer active.

Exit criteria:

- Every controller-programmed pointer is traceable to a live DMA allocation and its UEFI mapping.
- Controller DMA uses only device-visible addresses returned by the UEFI mapping interface.
- Buffer memory remains live until controller references are cleared or a reset makes them invalid.
- No test uses the Dell for DMA work.

## Gate 2 — UEFI HID handoff

V28 validates the versioned UEFI -> service discovery snapshot before active xHCI reconfiguration. UEFI supplies controller identity/capabilities (including the capability/offset/page-size values needed by the service) and only keyboard/mouse discovery facts. V28 is read-only with respect to xHCI MMIO and DMA.

Exit criteria:

- HCIVERSION >= 1.0 is enforced.
- Handoff magic, version, size and two-device bound validate.
- Only boot-protocol HID keyboard/mouse interfaces enter the handoff.
- Non-keyboard/mouse USB handles are excluded.
- Retained devices have an interrupt-IN endpoint.
- UEFI-created live xHCI resources are not inherited.

## Gate 3 — Halted initialization preparation — PASS

V29 completed this gate on the Toshiba Satellite P50. It owns the controller halt/reset transition, waits for CNR to clear, validates HCIVERSION/capabilities, allocates controller data structures through EFI_PCI_IO_PROTOCOL common-buffer DMA mapping, provisions the required scratchpads, programs CONFIG/DCBAAP/CRCR and the primary event-ring registers while halted, verifies the applicable register state, then clears all controller pointers before unmapping/freeing DMA. No Run/Stop, doorbell, command, or transfer is issued.

The verified sequence is:

`halt -> reset -> wait for reset completion -> wait for CNR clear -> validate capabilities -> allocate/map DMA objects -> program CONFIG, DCBAAP, CRCR, and primary-event-ring registers -> verify -> clear pointers -> release memory`

Toshiba V29 result: xHCI 1.00, PCI 8086:8C31, 64-bit BAR 0xF7C00000, 32 slots, 16 scratchpads, AC64=1, HCH=1, CNR=0. CRCR write passed using split low-DWORD/high-DWORD MMIO access; ERSTBA and ERDP readback passed. Final result was `Success` with no failed stage. All controller pointers were cleared before DMA release.

Exit criteria:

- Register values read back correctly where the specification defines meaningful readback.
- The controller remains halted and ready.
- Teardown clears every controller reference before memory is released.

**Gate 3 status: COMPLETE / HARDWARE PASS on Toshiba Satellite P50.**

## Gate 4 — Controller start without commands — NEXT

The Gate 4 design is portable and does **not** require prior discovery of the Toshiba's Linux-visible IOMMU/VT-d state. UEFI is the platform abstraction: the test uses `EFI_PCI_IO_PROTOCOL.AllocateBuffer()` plus `Map(EfiPciIoOperationBusMasterCommonBuffer)` and programs xHCI only with the resulting device-visible addresses. The UEFI DMA mapping/ownership contract, rather than a Toshiba-specific IOMMU configuration, is the prerequisite.

The required design and implementation reviews must still be completed against the applicable xHCI specification, coreboot/libpayload, Linux xhci-hcd, and UEFI/GNU-EFI. Platform-specific DMA behavior is recorded only when needed to explain a UEFI mapping failure or unexpected hardware result; it is not a portability requirement.

Retain valid ring memory and mappings, enable only the required primary event-ring state, start the controller, and observe that it reaches the expected running state. Do not submit a command, ring a doorbell, or enable CPU interrupt delivery. Poll status only; do not consume or acknowledge event-ring entries in Gate 4.

Gate 4 sequence:

`V29 halt/reset/init -> retain DMA mappings -> verify CONFIG/DCBAAP/CRCR/event-ring state -> verify interrupts disabled -> RS=1 -> wait HCH=0 -> observe briefly -> RS=0 -> wait HCH=1 -> reset -> CNR clear -> clear controller pointers -> unmap/free DMA`

Before `RS=1`, explicitly verify `CONFIG=1`, valid mapped `DCBAAP`, valid command-ring state, `ERSTSZ=1`, valid mapped `ERSTBA`/`ERDP`, `IMAN.IE=0`, `USBCMD.EIE=0`, `USBCMD.HSEIE=0`, `HCH=1`, and `CNR=0`. The controller must be allowed to run with all DMA allocations and mappings alive. No Enable Slot, port reset, Address Device, descriptor transfer, USB transfer, MSI/MSI-X setup, CPU interrupt handler, PCI BAR change, IOMMU programming, or doorbell is permitted.

An event-ring write by the running controller is not itself a Gate 4 failure; Gate 4 does not interpret or consume events. The test only establishes that the controller can start, remain a live bus master for the observation window, and recover cleanly.

Exit criteria:

- The controller starts without host-system error or unexpected reset and reaches `HCH=0`.
- The controller remains able to reference its mapped DMA structures for the full running interval.
- CPU interrupt delivery remains disabled.
- No command or doorbell is issued.
- The controller halts on `RS=0`, then resets cleanly with `CNR` returning to 0.
- Every controller DMA pointer is cleared before `Unmap()`/`FreeBuffer()`.
- The sequence is repeatable on the Toshiba without requiring platform-specific IOMMU/VT-d programming.

## Gate 5 — Command-ring completion by polling

Submit one Enable Slot command and poll the event ring for its completion. Keep CPU interrupt delivery disabled; polling isolates event-ring correctness from interrupt routing.

Exit criteria:

- The completion event is valid and yields a slot ID.
- Event dequeue acknowledgement works.
- The controller remains healthy after recovery.

## Gate 6 — One pre-connected wired keyboard

Use a single external wired keyboard connected before boot. Implement root-port identification/reset, Address Device, device and configuration descriptor retrieval, HID boot-interface selection, Set Configuration, and Set Protocol.

Exit criteria:

- Descriptors are decoded from the intended device.
- The selected HID interface is demonstrably boot-protocol capable.

## Gate 7 — Keyboard reports, then mouse

Create an interrupt-IN transfer ring and initially poll the event ring for reports. Translate boot-keyboard reports into a small internal event queue. Add boot-mouse handling only after keyboard operation is stable.

Exit criteria:

- Keyboard make, break, modifier, and rollover behaviour is recorded and correct.
- Mouse support does not change the keyboard path's behaviour.

## Gate 8 — OS integration and portability

Implement the execution model described in `ARCHITECTURE.md`: controller ownership, startup/shutdown across `ExitBootServices`, the USB-service lifetime, the input-event ABI, queue ownership, and backpressure. Only then expand testing to additional hardware, USB 3 devices, and hubs.

Exit criteria:

- The kernel uses only the documented input-service ABI.
- USB/xHCI state has a defined owner for its full lifetime.
- Compatibility testing begins only after the Toshiba happy path is repeatable.
