# Implementation Plan

Each gate has a narrow objective and explicit exit criteria. A failed or ambiguous result stops progression until it is recorded and understood.

## Cumulative implementation rule

**Every numbered test version is cumulative.** A new version must start from the latest proven implementation and carry forward all applicable previously passed gates. It may add the next gate's functionality, but it must not replace, bypass, or independently reimplement previously proven controller lifecycle, UEFI discovery, DMA, ring, completion, safety, or teardown behavior.

The progression is:

`V28 discovery -> V29 halted initialization -> V30 controller run/halt/recovery -> V31 Enable Slot/completion -> V32 discovery + V29/V30/V31 + Port Reset/Address Device -> later versions extend the same implementation`

A hardware result is evidence for the **cumulative implementation** in that version, not merely for the newly added operation. An isolated replacement test does not constitute progression of the implementation.

## Gate 0 — Evidence and platform baseline

Reject any controller reporting HCIVERSION < 1.0 before initialization. Reconcile the experiment history, retain the known-good read-only output, and complete the unknown Toshiba fields in `PLATFORM.md` that affect initialization: firmware, scratchpads, context size, legacy ownership, external test keyboard, boot medium, and recovery procedure.

Exit criteria:

- The binary and machine used for each recorded active result are identifiable.
- The controller reports HCIVERSION >= 1.0; older xHCI revisions are rejected before controller initialization.
- The controller's required scratchpad count and addressing capability are known.
- There is a documented, repeatable recovery procedure.

## Gate 1 — DMA contract

Create a small DMA abstraction that records the CPU allocation, device-visible address, allocation size, alignment, and release operation. Do not assume that a UEFI physical address is automatically the address usable by xHCI DMA. Use the UEFI PCI I/O mapping facility and treat the returned DeviceAddress as authoritative for controller DMA.

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

Toshiba V29 result: xHCI 1.00, PCI 8086:8C31, 64-bit BAR 0xF7C00000, 32 slots, 16 scratchpads, AC64=1, HCH=1, CNR=0. CRCR write passed using split low-DWORD/high-DWORD MMIO access; ERSTBA and ERDP readback passed. Final result was Success with no failed stage. All controller pointers were cleared before DMA release.

**Gate 3 status: COMPLETE / HARDWARE PASS on Toshiba Satellite P50.**

## Gate 4 — Controller start without commands — PASS

V30 completed this gate on the Toshiba Satellite P50. The test reused the V29 UEFI DMA contract, initialized valid controller-referenced memory, disabled CPU interrupt delivery, set Run/Stop, waited for HCH=0, observed the running controller, then halted and reset it. No command, doorbell, USB transfer, or CPU interrupt was generated.

Toshiba V30 result: RUN: HCH=0 PASS, HALT: HCH=1 PASS, RESET: CNR=0 HCH=1 PASS, COMMANDS=0, DOORBELLS=0, CPU-INTERRUPTS=0, EVENTS=0, RESULT=Success, FAIL STAGE=NONE. All controller pointers were cleared before DMA release.

**Gate 4 status: COMPLETE / HARDWARE PASS on Toshiba Satellite P50.**

## Gate 5 — One Enable Slot command + polled completion event — PASS

V31 completed this gate on the Toshiba Satellite P50. It issued exactly one Enable Slot command using the controller-declared Protocol Slot Type, rang only Host Controller Doorbell 0, and polled the primary event ring for the corresponding Command Completion Event. CPU interrupt delivery remained disabled.

Observed V31 result: xHCI 1.00, PCI 8086:8C31, 32 slots, 16 scratchpads, 4096-byte page size, Protocol Slot Type 0. Exactly one command and one doorbell were issued. The completion event was type 33 with Completion Code 1 (Success), Slot ID 1, and Command TRB Pointer equal to the submitted command TRB address 0x00000000C6803000. CPU-INTERRUPTS=0, EVENTS=1, reset recovery passed, and all controller pointers were cleared before DMA release.

Reference: Intel xHCI Specification, command completion/event-ring requirements; Linux xhci-hcd initialization and command-ring implementation cross-check.

**Gate 5 status: COMPLETE / HARDWARE PASS on Toshiba Satellite P50.**

## Gate 6 — One pre-connected HID keyboard interface — CURRENT

Gate 6 is cumulative. V32 must retain the complete proven V28/V29/V30/V31 implementation and add the first live USB-device operations. The target is one pre-connected USB HID boot-protocol keyboard interface. The physical device may be a composite receiver, including a combined wireless keyboard/mouse dongle; only the keyboard interface is exercised in this gate.

V28's UEFI discovery phase runs at the beginning of V32 and identifies the live keyboard interface, its root-hub port, VID/PID, configuration/interface facts, and interrupt-IN endpoint facts before the bridge reconfigures xHCI. Those facts are constrained hints and validation inputs, not authority to skip normal xHCI device setup. V32 must use the discovered root port; it must not hard-code a physical or xHCI port number.

The cumulative V32 sequence is:

`UEFI discovery -> halt -> reset -> CNR clear -> validate caps -> allocate/map DMA -> program CONFIG/DCBAAP/CRCR/primary event ring -> start xHCI -> verify HCH=0 -> verify discovered root port connected -> port reset -> consume/validate port-status change -> Enable Slot -> completion -> allocate/initialize fresh device contexts -> DCBAA[slot] -> Address Device -> completion -> halt/reset -> clear controller pointers -> release DMA`

This first cumulative V32 stops after Address Device. Descriptors, SET_CONFIGURATION, Configure Endpoint, HID Set Protocol, keyboard reports, mouse traffic, hubs, hot-plug, MSI/MSI-X, CPU interrupt handlers, and continuous report polling remain later work.

### Gate 6 implementation-review requirements

Before V32 is committed, review the implementation against the xHCI specification, USB HID/USB control-transfer requirements, coreboot/libpayload, Linux xhci-hcd, and UEFI/GNU-EFI. Specifically check:

1. **Cumulative reuse:** V32 must contain the proven V29 halt/reset/DMA/ring initialization, V30 RUN/halt/recovery, and V31 Enable Slot/event-ring completion behavior rather than reimplementing isolated substitutes.
2. **UEFI discovery:** discover the keyboard interface before active xHCI reconfiguration; identify the matching root port from the UEFI USB device path; identify the keyboard interface and interrupt-IN endpoint; use those facts to select the target port; do not hard-code port 4.
3. **Port reset/state:** correct PORTSC reset sequencing, change-bit handling, reset completion detection, and avoidance of unintended writes to unrelated PORTSC bits.
4. **Slot/context:** context-size selection from HCCPARAMS1, 64-byte/32-byte context layout as applicable, alignment, DCBAA slot indexing, scratchpad/DCBAA lifetime, and Input Control Context fields.
5. **Address Device:** correct Input Slot/EP0 contexts, Route String/Root Hub Port/Speed fields, Max Packet Size 0, Transfer Ring Dequeue Pointer, and command completion handling. Do not reuse the V31 Slot ID after reset; the slot is per-run state.
6. **DMA:** all contexts, rings, and controller-referenced buffers use UEFI common-buffer mapping and device-visible addresses. Mappings remain live until controller halt/reset and all references are cleared.
7. **Failure safety:** after any command submission, if the controller cannot be confirmed halted, do not free DMA mappings; enter the existing non-returning fatal recovery path or an equally conservative recovery path.
8. **Interrupt isolation:** keep CPU interrupt delivery disabled throughout V32; poll the event ring directly as in V31.

### Gate 6 acceptance criteria

- V32 output proves UEFI discovery selected the actual keyboard interface and root port before xHCI reconfiguration.
- The discovered root port is connected when the controller is started.
- Port reset completes cleanly and the corresponding port-status change is consumed/validated.
- Enable Slot completion returns a valid fresh Slot ID.
- Address Device completes successfully and the output device context reaches the expected addressed state.
- No hard-coded target port is used.
- No mouse traffic, descriptor transfers, endpoint configuration, HID protocol change, hubs, hot-plug, CPU interrupts, MSI/MSI-X, or continuous report polling is introduced.
- On every error path, DMA mappings remain live until controller references are safely eliminated.
- The controller can be halted/reset and all controller pointers cleared before DMA release.

**Gate 6 status: RESTARTED. All prior V32.x Gate 6 source files have been removed. A new cumulative V32 implementation is under review against the Intel xHCI specification, Linux xhci-hcd, coreboot/libpayload, and UEFI/GNU-EFI; CI and hardware testing remain pending.**

## Gate 7 — Keyboard reports, then mouse

Create an interrupt-IN transfer ring and initially poll the event ring for reports. Translate boot-keyboard reports into a small internal event queue. Add boot-mouse handling only after keyboard operation is stable.

Exit criteria:

- Keyboard make, break, modifier, and rollover behaviour is recorded and correct.
- Mouse support does not change the keyboard path's behaviour.

## Gate 8 — OS integration and portability

Implement the execution model described in ARCHITECTURE.md: controller ownership, startup/shutdown across ExitBootServices, the USB-service lifetime, the input-event ABI, queue ownership, and backpressure. Only then expand testing to additional hardware, USB 3 devices, and hubs.

Exit criteria:

- The kernel uses only the documented input-service ABI.
- USB/xHCI state has a defined owner for its full lifetime.
- Compatibility testing begins only after the Toshiba happy path is repeatable.
