# Gate 6 Design — UEFI-Selected Keyboard to Address Device

Status: design only; no V32 implementation is authorized by this document alone.

## 1. Design authority

This design is derived from the complete current project documentation: `PROJECT.md`, `ARCHITECTURE.md`, `DECISIONS.md`, `EXPERIMENTS.md`, `IMPLEMENTATION_PLAN.md`, `PLATFORM.md`, and `TODO.md`, together with the existing UEFI handoff code and the proven V29/V30/V31 implementations.

The normative rule for the implementation is:

**UEFI is the authoritative discovery provider for the fixed keyboard/mouse scope. The V32 bridge core consumes the already-selected keyboard handoff; it does not rediscover the keyboard. V32 must nevertheless create fresh xHCI controller/device state and must not inherit UEFI-owned xHCI runtime state.**

This distinguishes two separate responsibilities:

- UEFI discovery owns USB discovery and supplies the selected device's useful facts.
- The V32 bridge core owns the fresh xHCI controller/device programming needed to reach the live device.

Therefore `UEFI authority` does not mean `reuse UEFI slot/ring/context/address state`.

## 2. Gate objective

Prove the first live-device xHCI sequence for exactly one pre-connected wired HID keyboard on the Toshiba Satellite P50:

`UEFI-selected keyboard -> UEFI USB-stack quiesce -> fresh controller initialization -> USB2/low-speed HID port reset -> Enable Slot -> fresh device context -> Address Device -> clean teardown`

The first implementation stops immediately after successful Address Device completion and validation.

No keyboard report transfer is attempted by this gate.

Gate 6 is deliberately scoped to a directly attached HID keyboard that enumerates on the controller's USB2-compatible path at Low Speed or Full Speed. High-Speed/SuperSpeed device operation and USB3-specific reset handling are outside this gate. A keyboard being physically plugged into a modern USB3-capable connector does not by itself make the device a SuperSpeed device; the live xHCI port state determines the protocol/speed path used for this test.

## 3. Fixed precondition

The hardware test uses the Toshiba Satellite P50, the designated sacrificial xHCI platform.

Exactly one intended external wired keyboard is connected before boot and remains connected through the test. The project scope permits the physical device to be a composite device or receiver, but V32 exercises only the keyboard interface selected by UEFI.

No hub, hot-plug, arbitrary USB inventory, or second HID device is introduced for this gate.

The test must record the actual UEFI handoff values before active xHCI writes, including at minimum:

- selected keyboard identity within the handoff
- selected controller PCI segment/bus/device/function identity
- root port, when supplied
- USB speed, when supplied
- configuration value, when supplied
- interface number/protocol
- interrupt-IN endpoint and endpoint facts
- EP0 / device packet-size information
- controller identity/capability and register-offset facts supplied by the handoff, when present

The hardware is never expected to have a hard-coded port number in the implementation. The observed Toshiba port 4 from V28 is evidence for the test machine, not an implementation constant.

## 4. UEFI discovery boundary

The V32 EFI image contains two explicit logical stages even though the Gate 6 test is delivered as one EFI executable:

1. a **UEFI discovery producer** that obtains the authoritative keyboard selection and constructs the bounded handoff; and
2. a **bridge consumer** that receives that handoff as an input object and performs all active xHCI programming.

This two-stage boundary is an implementation boundary, not a claim that a standalone V32 image already has an operating-system service transport. It gives the test a real producer/consumer interface without requiring a file, NVRAM variable, or other persistent storage channel.

The discovery producer may enumerate `EFI_USB_IO_PROTOCOL` handles because USB discovery is its defined responsibility. It must select the keyboard according to the fixed project scope and populate the handoff. The bridge consumer must not repeat that discovery work.

The bridge consumer must **not**:

- enumerate the complete `EFI_USB_IO_PROTOCOL` inventory and choose a keyboard itself;
- independently search for another HID keyboard;
- reject the selected keyboard because a secondary rediscovery test disagrees with the handoff;
- reconstruct USB topology merely to decide whether the keyboard exists;
- require a particular number of USB handles, USB nodes, or PCI-path nodes beyond what is necessary to interpret the already-supplied handoff.

The selected keyboard is therefore the **input object** to the bridge, not a candidate that the bridge must rediscover.

The bridge may validate the handoff's structure and fields for internal consistency and xHCI usability. Those checks validate the producer's data; they are not a second discovery algorithm.

## 5. Gate 6 handoff contract and lifetime

The Gate 6 handoff is a bounded, versioned value object. The producer owns its construction; the bridge owns a private copy of the selected keyboard/device-template data before active xHCI reconfiguration begins.

For this gate, the transport mechanism is intentionally the simplest safe one:

`UEFI discovery producer -> in-memory handoff object -> bridge consumer`

No persistent file, UEFI variable, disk write, or firmware-NVRAM state is required.

The producer must complete discovery and validate the handoff before the bridge performs any xHCI state-changing operation. Once the bridge has copied the selected device facts, it does not retain a dependency on `EFI_USB_IO_PROTOCOL` or on UEFI-created xHCI resources for the Gate 6 xHCI sequence.

### 5.1 UEFI USB-stack ownership handoff

The discovery producer is operating while the firmware USB host-controller driver and USB bus driver own the xHCI controller through the UEFI driver model. The bridge must not reset, stop, or otherwise reconfigure the xHCI controller while those UEFI drivers remain active.

After discovery and handoff-copy validation, the producer must explicitly request the UEFI USB stack to stop managing the selected xHCI controller before the bridge performs any active xHCI MMIO writes. The portable UEFI mechanism for this experiment is the Boot Services `DisconnectController()` operation on the selected controller handle with `DriverImageHandle=NULL` and `ChildHandle=NULL`. Under the UEFI driver model, that requests disconnection of all drivers currently managing the controller and destruction of its children. The implementation must verify that the call returns success before proceeding.

This is an ownership/quiesce transition only. It does not transfer or preserve UEFI xHCI runtime state for reuse. The UEFI stack is being stopped so the bridge can establish fresh xHCI state safely.

The producer must complete all USB discovery reads before this disconnect. After successful disconnect, the bridge must not depend on `EFI_USB_IO_PROTOCOL` operations, UEFI USB timers, or UEFI-created controller rings/contexts/DMA buffers.

If `DisconnectController()` fails or cannot be invoked on the selected controller handle, Gate 6 must stop before any active xHCI reconfiguration.

UEFI defines `DisconnectController()` as the driver-model mechanism for disconnecting drivers from a controller; when both optional driver/child handles are NULL, all drivers managing that controller are disconnected and all of its children are destroyed. citeturn162048search0turn162048search1

### 5.2 Handoff runtime exclusions

The handoff contains discovery facts, not ownership-transfer state. In particular, the following are explicitly excluded from the handoff contract as reusable runtime objects:

- UEFI command rings;
- UEFI event rings;
- UEFI transfer rings;
- UEFI device contexts;
- UEFI DCBAA entries;
- UEFI slot IDs;
- UEFI-assigned USB device state/address;
- UEFI DMA buffers or mappings;
- UEFI controller run state.

For the eventual resident service architecture, the same logical handoff ABI may later be transferred through the boot/OS handoff mechanism. That future transport is not a prerequisite for the Gate 6 EFI experiment and remains a separate architecture task.

### 5.3 Required producer output

The producer must supply enough information that the bridge never has to rediscover the selected keyboard. At minimum this includes:

- controller identity/capability facts needed for the Gate 6 validation;
- selected controller PCI segment/bus/device/function identity;
- selected keyboard root port;
- USB speed or an explicitly marked unavailable value;
- USB device identity needed for evidence;
- configuration value;
- interface number and boot keyboard protocol;
- EP0 maximum packet size in a bridge-usable representation;
- interrupt-IN endpoint address and descriptor facts sufficient for later gates.

The bridge must treat unavailable optional fields as unavailable rather than inventing a machine-specific value.

### 5.4 EP0 packet-size normalization

The handoff must distinguish the USB device descriptor's encoding from the value required by the xHCI Endpoint 0 Context.

The UEFI producer should normalize `bMaxPacketSize0` into the actual EP0 maximum packet size before the bridge consumes it. The bridge must then validate that the value is legal for the selected device speed and use that normalized value when constructing the EP0 context.

A raw USB descriptor byte must not be copied blindly into an xHCI context field where the xHCI field expects the actual packet size.

## 6. Controller selection and binding

The xHCI controller is still selected through PCI discovery rather than a machine-specific PCI address, consistent with the project architecture and prior gates.

The implementation must confirm the controller meets the project compatibility boundary (`HCIVERSION >= 1.0`) and capability requirements before active initialization.

Where the UEFI handoff already supplies controller identity/capability facts, those values are consumed as useful discovery state. Live xHCI register state remains independently read and controlled by V32 because V32 is establishing fresh ownership of the controller programming state.

The handoff must identify the PCI controller path associated with the selected USB device sufficiently to bind the bridge to the corresponding xHCI controller. The bridge may discover xHCI controllers through PCI, but it must not simply select the first matching xHCI controller when the handoff identifies another controller.

The controller handle used for the UEFI ownership/quiesce step and the PCI I/O handle used for bridge MMIO/DMA access must refer to the same controller. The implementation must validate that correspondence before disconnecting the UEFI controller and again before active xHCI initialization.

## 7. Cumulative controller sequence

V32 must carry forward the proven V29, V30 and V31 machinery rather than creating a replacement implementation.

### Phase A — UEFI discovery and ownership handoff

1. Run the UEFI discovery producer for the fixed Gate 6 keyboard scope.
2. Identify the selected keyboard's corresponding xHCI controller handle and PCI location.
3. Require exactly one in-scope keyboard selection.
4. Validate handoff magic/version/size and the selected keyboard record.
5. Normalize and validate EP0 maximum packet size.
6. Copy the selected keyboard facts into V32 bridge-local device-template state.
7. Complete all required UEFI USB discovery reads before controller disconnect.
8. Validate that the controller handle and PCI I/O handle refer to the same xHCI controller.
9. Call `DisconnectController(selected_controller, NULL, NULL)`.
10. Verify the disconnect returns success and treat that successful driver-model operation as the UEFI USB-stack quiesce point.
11. After this point, the bridge does not use `EFI_USB_IO_PROTOCOL` or UEFI USB timers.
12. Do not use any UEFI-created xHCI slot ID, device address, command ring, transfer ring, event ring, context, or DMA buffer.

### Phase B — Controller preparation

13. Revalidate PCI controller identity against the handoff and controller handle used for disconnect.
14. Read/validate HCIVERSION and required capabilities.
15. Halt the controller if necessary.
16. Reset the controller.
17. Wait for reset completion and `CNR=0`.
18. Preserve the V29/V30/V31 controller initialization, scratchpad handling, command ring, and primary event-ring setup.
19. Allocate every controller-referenced object using the established UEFI `EFI_PCI_IO_PROTOCOL` common-buffer DMA contract.
20. Program controller pointers using the UEFI-mapped device-visible addresses.

### Phase C — Start and select the discovered port

21. Keep CPU interrupt delivery disabled.
22. Start the controller and verify `HCH=0` as established by V30.
23. Select the root port from the bridge-local copy of the UEFI handoff's selected keyboard record.
24. Read that port's PORTSC state.
25. Verify the port is currently connected before attempting reset.
26. Determine the live port protocol/speed from xHCI state. Gate 6 accepts Low Speed or Full Speed only; reject a selected keyboard whose live speed is High Speed or SuperSpeed rather than adding a USB3/general-speed path to this gate.
27. Do not scan all ports to locate a different connected device.

### Phase D — Port reset

28. Perform the USB2-compatible root-port reset required for the selected Low-/Full-Speed device.
29. Preserve unrelated PORTSC state and modify only explicitly required operation/change bits.
30. Poll for the reset completion/change indication required for the USB2 path.
31. Consume and validate the resulting Port Status Change Event through the same polled primary event-ring mechanism used by V31.
32. Re-read PORTSC and validate the expected post-reset state.
33. If the expected device connection is no longer present, fail; do not select another port.
34. Use the live post-reset PORTSC speed value, not the pre-reset UEFI speed field, as the authoritative Speed input for the Slot Context. The UEFI speed remains a discovery/evidence value.
35. A matching Port Status Change Event is identified by its Port ID; do not assume that it is the only event on the ring or that unrelated change events cannot exist.

### Phase E — Enable Slot

36. Obtain the controller-declared slot/protocol information needed by the Enable Slot command.
37. Issue exactly one Enable Slot command.
38. Ring only Doorbell 0.
39. Poll the primary event ring.
40. Require exactly one matching Command Completion Event with Success completion code.
41. Record the returned Slot ID.
42. The Slot ID is fresh per V32 run and is never inherited from UEFI or V31.

### Phase F — Fresh device context

43. Determine context size from the controller capability.
44. Allocate the required Input Device Context, Output Device Context, and EP0 transfer ring through the existing DMA abstraction.
45. Initialize them from zeroed memory with the correct alignment and context stride.
46. Create the DCBAA slot entry using the fresh Output Device Context device-visible address.
47. Initialize only the Slot Context and EP0 Input Context fields required for Address Device.
48. Use the selected keyboard handoff's root port together with the live post-reset port speed in the Slot Context.
49. Use the normalized UEFI-supplied EP0 packet-size information.
50. Initialize EP0 transfer-ring dequeue state and cycle state correctly.

The Address Device Input Slot Context must have a valid Route String, Speed, Context Entries = 1, Root Hub Port Number, and Interrupter Target, with the hub-related fields zero for this directly attached no-hub gate. The Input Endpoint 0 Context must be Control type, use the normalized EP0 maximum packet size, have Max Burst Size = 0, Max Primary Streams = 0, Mult = 0, CErr = 3, and point to the EP0 transfer ring with the correct Dequeue Cycle State. The xHCI specification explicitly defines these as validity requirements for Address Device. citeturn162048search32turn265497search33

### Phase G — Address Device

51. Build exactly one Address Device command for the newly returned Slot ID with BSR cleared.
52. Point it to the fresh Input Device Context.
53. Ring the command doorbell.
54. Poll the primary event ring for the corresponding Command Completion Event.
55. Validate the completion event type, completion code, slot ID, and command TRB pointer.
56. Validate the resulting Output Slot Context state: Slot State = Addressed and USB Device Address is non-zero; validate the Output EP0 Context state is Running.
57. Do not issue descriptors, SET_CONFIGURATION, Configure Endpoint, HID Set Protocol, or interrupt-IN transfers in this gate.

The xHCI specification states that with BSR=0, a successful Address Device causes the xHC to issue SET_ADDRESS, copy the input slot/EP0 contexts to the output contexts, set the EP0 state Running, set Slot State to Addressed, assign a non-zero device address, and return a Success completion code. citeturn613620search32

### Phase H — Safe recovery and teardown

58. Halt the controller and confirm `HCH=1`.
59. Reset the controller and confirm `CNR=0` and halted state as required by the existing recovery path.
60. Clear CRCR, DCBAAP, CONFIG, event-ring/interrupter references, and any other controller pointers established by V29–V32.
61. Only after controller references have been eliminated, unmap and free all DMA mappings.
62. Free all ordinary Boot Services allocations.
63. Return success only after the complete teardown succeeds.

If the controller is active after a command/transfer and cannot be confirmed halted, V32 must not free controller-referenced memory. It must enter the project's existing non-returning fatal recovery path or an equally conservative recovery path.

## 8. What “use as much UEFI information as possible” means

The handoff should minimize duplicated discovery work.

For the selected keyboard, V32 should use all useful facts already supplied by UEFI that directly constrain the xHCI bring-up, including root-port identity, speed, configuration/interface information, interrupt-IN endpoint facts, packet sizes/intervals, and controller facts supplied by the handoff.

V32 must not treat those facts as permission to skip the xHCI state machine. In particular, UEFI's prior configured state does not substitute for V32's own Enable Slot, Device Context, Address Device, or later Configure Endpoint operations.

For Gate 6 specifically, the UEFI speed is an expectation/evidence field; the live post-reset PORTSC speed is authoritative for xHCI context construction. The gate remains deliberately limited to Low-/Full-Speed HID and does not implement SuperSpeed reset/state handling.

The correct pattern is therefore:

`UEFI discovery -> bounded handoff -> bridge-local copy -> UEFI USB-stack quiesce -> fresh xHCI state -> use selected root port -> determine live USB2 speed -> normal xHCI device lifecycle`

not:

`bridge enumerates again -> bridge chooses a device -> bridge reconstructs UEFI's discovery result -> bridge starts xHCI`

## 9. Scope deliberately excluded from this gate

The following are not part of the first V32 implementation:

- High-Speed or SuperSpeed keyboard operation;
- USB3-specific reset/warm-reset handling;
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

## 10. Acceptance evidence

A successful V32 run must make the following observable without relying on hard-coded Toshiba values:

- UEFI's discovery producer selected the keyboard before active xHCI reconfiguration.
- The selected keyboard's UEFI facts were copied into bridge-local state and consumed directly.
- The selected controller binding came from the handoff rather than first-match PCI enumeration.
- UEFI's `DisconnectController()` ownership/quiesce operation returned success before any active bridge xHCI writes.
- No bridge-side keyboard-discovery scan was used.
- The selected root port was connected.
- The live port speed was Low Speed or Full Speed.
- Port reset completed using the USB2-compatible path and the corresponding port-status change was consumed/validated.
- The post-reset live PORTSC speed was used for Slot Context construction.
- Enable Slot returned a fresh Slot ID.
- Fresh device contexts were allocated and linked through the DCBAA.
- The Address Device command used BSR=0 and the fresh Input Context.
- Address Device completed successfully for that Slot ID.
- The Output Slot Context reported Slot State = Addressed and a non-zero USB Device Address.
- The Output EP0 Context reported the expected Running state.
- CPU interrupt delivery remained disabled.
- All controller-referenced DMA objects remained live until controller references were cleared.
- Controller halt/reset recovery completed successfully.
- All controller pointers were cleared before DMA release.

## 11. Required implementation review before source creation

Before any V32 source is written, the implementation design must be checked against:

1. the complete project documentation;
2. the applicable UEFI specification sections for USB I/O, device paths, PCI I/O, Driver Model controller disconnect, and DMA mapping;
3. the applicable xHCI specification sections for controller reset, USB2 ports, command ring, event ring, Enable Slot, device contexts and Address Device;
4. Linux xhci-hcd as an implementation cross-check;
5. coreboot/libpayload xHCI code as an implementation cross-check;
6. the proven V29/V30/V31 source and observed hardware results.

The review must explicitly prove that:

- the UEFI discovery producer and V32 bridge consumer are separate logical stages;
- the UEFI USB/xHCI stack is quiesced before bridge xHCI writes;
- the bridge consumes the selected keyboard rather than rediscovering it;
- the bridge uses the live post-reset port speed rather than trusting the pre-reset UEFI speed for xHCI context construction;
- EP0 packet-size data is normalized before xHCI context construction;
- the Address Device Input Context exactly matches the xHCI validity requirements for this directly attached low/full-speed device;
- V32 preserves the cumulative implementation and safety/teardown gates;
- no UEFI-owned xHCI runtime state is reused;
- no USB3-specific implementation is introduced into this low/full-speed keyboard gate.

No CI build or Toshiba hardware test is authorized until that implementation review passes.
