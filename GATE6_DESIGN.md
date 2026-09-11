# Gate 6 Design — UEFI-Selected Keyboard to Address Device

Status: design only; no V32 implementation is authorized by this document alone.

## 1. Design authority

This design is derived from the complete current project documentation: `PROJECT.md`, `ARCHITECTURE.md`, `DECISIONS.md`, `EXPERIMENTS.md`, `IMPLEMENTATION_PLAN.md`, `PLATFORM.md`, and `TODO.md`, together with the existing UEFI handoff code and the proven V29/V30/V31 implementations.

The normative rule for the implementation is:

**UEFI is the authoritative discovery provider for the fixed keyboard/mouse scope. V32 consumes the already-selected keyboard handoff; it does not rediscover the keyboard. V32 must nevertheless create fresh xHCI controller/device state and must not inherit UEFI-owned xHCI runtime state.**

This distinguishes two separate responsibilities:

- UEFI owns USB discovery and supplies the selected device's useful facts.
- V32 owns the fresh xHCI controller/device programming needed to reach the live device.

Therefore `UEFI authority` does not mean `reuse UEFI slot/ring/context/address state`.

## 2. Gate objective

Prove the first live-device xHCI sequence for exactly one pre-connected wired HID keyboard on the Toshiba Satellite P50:

`UEFI-selected keyboard -> fresh controller initialization -> root-port reset -> Enable Slot -> fresh device context -> Address Device -> clean teardown`

The first implementation stops immediately after successful Address Device completion and validation.

No keyboard report transfer is attempted by this gate.

## 3. Fixed precondition

The hardware test uses the Toshiba Satellite P50, the designated sacrificial xHCI platform.

Exactly one intended external wired keyboard is connected before boot and remains connected through the test. The project scope permits the physical device to be a composite device or receiver, but V32 exercises only the keyboard interface selected by UEFI.

No hub, hot-plug, arbitrary USB inventory, or second HID device is introduced for this gate.

The test must record the actual UEFI handoff values before active xHCI writes, including at minimum:

- selected keyboard identity within the handoff
- root port, when supplied
- USB speed, when supplied
- configuration value, when supplied
- interface number/protocol
- interrupt-IN endpoint and endpoint facts
- EP0 / device packet-size information
- controller identity/capability and register-offset facts supplied by the handoff, when present

The hardware is never expected to have a hard-coded port number in the implementation. The observed Toshiba port 4 from V28 is evidence for the test machine, not an implementation constant.

## 4. UEFI discovery boundary

The V32 EFI application must begin from the UEFI-produced handoff/device selection.

It must **not**:

- enumerate the complete `EFI_USB_IO_PROTOCOL` inventory and choose a keyboard itself;
- independently search for another HID keyboard;
- reject the selected keyboard because a secondary rediscovery test disagrees with the handoff;
- reconstruct USB topology merely to decide whether the keyboard exists;
- require a particular number of USB handles, USB nodes, or PCI-path nodes beyond what is necessary to interpret the already-supplied handoff.

It may consume UEFI-provided descriptors and path data already present in the handoff. It may validate that required handoff fields are structurally usable for the xHCI operation being attempted. Such checks validate the handoff data; they are not a second keyboard-discovery algorithm.

The selected keyboard is therefore the **input object** to V32, not a candidate that V32 must rediscover.

## 5. Controller selection

The xHCI controller is still selected through PCI discovery rather than a machine-specific PCI address, consistent with the project architecture and prior gates.

The implementation must confirm the controller meets the project compatibility boundary (`HCIVERSION >= 1.0`) and capability requirements before active initialization.

Where the UEFI handoff already supplies controller identity/capability facts, those values are consumed as useful discovery state. Live xHCI register state remains independently read and controlled by V32 because V32 is establishing fresh ownership of the controller programming state.

## 6. Cumulative controller sequence

V32 must carry forward the proven V29, V30 and V31 machinery rather than creating a replacement implementation.

### Phase A — UEFI handoff

1. Obtain the already-selected keyboard handoff.
2. Validate handoff structure/version/size and the selected keyboard record.
3. Copy the selected keyboard facts into V32's local device-template state.
4. Do not enumerate USB handles to rediscover the keyboard.
5. Do not use any UEFI-created xHCI slot ID, device address, command ring, transfer ring, event ring, context, or DMA buffer.

### Phase B — Controller preparation

6. Discover the corresponding xHCI controller through PCI class.
7. Read/validate HCIVERSION and required capabilities.
8. Halt the controller if necessary.
9. Reset the controller.
10. Wait for reset completion and `CNR=0`.
11. Preserve the V29/V30/V31 controller initialization, scratchpad handling, command ring, and primary event-ring setup.
12. Allocate every controller-referenced object using the established UEFI `EFI_PCI_IO_PROTOCOL` common-buffer DMA contract.
13. Program controller pointers using the UEFI-mapped device-visible addresses.

### Phase C — Start and select the discovered port

14. Keep CPU interrupt delivery disabled.
15. Start the controller and verify `HCH=0` as established by V30.
16. Select the root port from the UEFI handoff's selected keyboard record.
17. Read that port's PORTSC state.
18. Verify the port is currently connected before attempting reset.
19. Do not scan all ports to locate a different connected device.

### Phase D — Port reset

20. Perform the xHCI root-port reset appropriate to the port's reported USB protocol/speed path.
21. Preserve unrelated PORTSC state and modify only explicitly required operation/change bits.
22. Poll for the protocol-appropriate reset completion/change indication.
23. Consume and validate the resulting Port Status Change Event through the same polled primary event-ring mechanism used by V31.
24. Re-read PORTSC and validate the expected post-reset state.
25. If the expected device connection is no longer present, fail; do not select another port.

### Phase E — Enable Slot

26. Obtain the controller-declared slot/protocol information needed by the Enable Slot command.
27. Issue exactly one Enable Slot command.
28. Ring only Doorbell 0.
29. Poll the primary event ring.
30. Require exactly one matching Command Completion Event with Success completion code.
31. Record the returned Slot ID.
32. The Slot ID is fresh per V32 run and is never inherited from UEFI or V31.

### Phase F — Fresh device context

33. Determine context size from the controller capability.
34. Allocate the required Input Device Context, Output Device Context, and EP0 transfer ring through the existing DMA abstraction.
35. Initialize them from zeroed memory with the correct alignment and context stride.
36. Create the DCBAA slot entry using the fresh Output Device Context device-visible address.
37. Initialize only the Slot Context and EP0 Input Context fields required for Address Device.
38. Use the selected keyboard handoff's root port and speed as the inputs to the Slot Context.
39. Use the UEFI-supplied EP0 packet-size information where available/usable; otherwise use the device information already established by the project/test's permitted discovery state rather than guessing a machine-specific value.
40. Initialize EP0 transfer-ring dequeue state and cycle state correctly.

### Phase G — Address Device

41. Build exactly one Address Device command for the newly returned Slot ID.
42. Point it to the fresh Input Device Context.
43. Ring the command doorbell.
44. Poll the primary event ring for the corresponding Command Completion Event.
45. Validate the completion event type, completion code, slot ID, and command TRB pointer.
46. Validate the resulting Output Slot Context state required for the addressed/default state.
47. Do not issue descriptors, SET_CONFIGURATION, Configure Endpoint, HID Set Protocol, or interrupt-IN transfers in this gate.

### Phase H — Safe recovery and teardown

48. Halt the controller and confirm `HCH=1`.
49. Reset the controller and confirm `CNR=0` and halted state as required by the existing recovery path.
50. Clear CRCR, DCBAAP, CONFIG, event-ring/interrupter references, and any other controller pointers established by V29–V32.
51. Only after controller references have been eliminated, unmap and free all DMA mappings.
52. Free all ordinary Boot Services allocations.
53. Return success only after the complete teardown succeeds.

If the controller is active after a command/transfer and cannot be confirmed halted, V32 must not free controller-referenced memory. It must enter the project's existing non-returning fatal recovery path or an equally conservative state.

## 7. What “use as much UEFI information as possible” means

The handoff should minimize duplicated discovery work.

For the selected keyboard, V32 should use all useful facts already supplied by UEFI that directly constrain the xHCI bring-up, including root-port identity, speed, configuration/interface information, interrupt-IN endpoint facts, packet sizes/intervals, and controller facts supplied by the handoff.

V32 must not treat those facts as permission to skip the xHCI state machine. In particular, UEFI's prior configured state does not substitute for V32's own Enable Slot, Device Context, Address Device, or later Configure Endpoint operations.

The correct pattern is therefore:

`consume UEFI facts -> establish fresh xHCI state -> use the UEFI-selected device identity to select the port -> perform the normal xHCI device lifecycle`

not:

`enumerate again -> choose a device -> reconstruct UEFI's discovery result -> start xHCI`

## 8. Scope deliberately excluded from this gate

The following are not part of the first V32 implementation:

- mouse traffic;
- hubs;
- hot-plug/disconnect state machines;
- arbitrary USB device discovery;
- arbitrary HID report parsing;
- descriptor/configuration transfers beyond what is required by a later gate;
- SET_CONFIGURATION;
- Configure Endpoint;
- HID Set Protocol;
- keyboard report polling;
- MSI/MSI-X;
- CPU interrupt handlers;
- continuous event pumping.

## 9. Acceptance evidence

A successful V32 run must make the following observable without relying on hard-coded Toshiba values:

- UEFI selected the keyboard before active xHCI reconfiguration.
- The selected keyboard's UEFI facts were consumed directly.
- No second keyboard-discovery scan was used.
- The selected root port was connected.
- Port reset completed and its resulting port-status change was consumed/validated.
- Enable Slot returned a fresh Slot ID.
- Fresh device contexts were allocated and linked through the DCBAA.
- Address Device completed successfully for that Slot ID.
- The Output Slot Context reached the expected addressed/default state.
- CPU interrupt delivery remained disabled.
- All controller-referenced DMA objects remained live until controller references were cleared.
- Controller halt/reset recovery completed successfully.
- All controller pointers were cleared before DMA release.

## 10. Required review before implementation

Before any V32 source is written, the implementation design must be checked against:

1. the complete project documentation;
2. the applicable UEFI specification sections for USB I/O, device paths, PCI I/O and DMA mapping;
3. the applicable xHCI specification sections for controller reset, ports, command ring, event ring, Enable Slot, device contexts and Address Device;
4. Linux xhci-hcd as an implementation cross-check;
5. coreboot/libpayload xHCI code as an implementation cross-check;
6. the proven V29/V30/V31 source and observed hardware results.

The review must explicitly prove that V32 preserves the cumulative implementation and that the UEFI handoff remains the discovery authority without transferring UEFI-owned xHCI runtime state.

No CI build or Toshiba hardware test is authorized until that implementation review passes.
