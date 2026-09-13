# Gate 6 Design — UEFI-Selected Keyboard to Address Device

Status: **DESIGN RESTARTED. V32 source and obsolete V32 debug workflow have been removed. No V32 implementation is authorized until the design review passes.**

## 1. Design authority

Gate 6 follows the project architecture and decisions: UEFI is the authoritative USB discovery provider; the bridge establishes fresh xHCI state; the Toshiba Satellite P50 is the sacrificial hardware target; V29/V30/V31 are cumulative implementation gates; and no hardware test is permitted before source and CI/source review.

**UEFI discovers and selects the keyboard. The V32 bridge consumes that selected record and does not rediscover USB devices. The bridge does not reuse UEFI xHCI runtime state.**

The stronger handoff rule is now explicit: **UEFI resolves every controller/device fact that can be resolved before ownership transfer, including exact BAR-relative xHCI MMIO offsets. The bridge consumes those resolved values rather than reconstructing them from topology facts.**

## 2. Gate objective

Prove the first live-device xHCI sequence for exactly one pre-connected HID boot-protocol keyboard:

`UEFI discovery -> UEFI USB-stack quiesce -> fresh xHCI initialization -> USB2-compatible port reset -> Enable Slot -> fresh Slot/EP0 contexts -> Address Device -> clean teardown`

Gate 6 stops immediately after successful Address Device completion and validation. No keyboard report transfer is attempted.

Gate 6 deliberately supports only a selected keyboard operating at Low Speed or Full Speed on the controller's USB2-compatible path. High-Speed, SuperSpeed, USB3 warm-reset handling, hubs, hot-plug and general-speed support are outside this gate.

## 3. Fixed hardware precondition

The hardware test uses the Toshiba Satellite P50. Exactly one intended external wired keyboard is connected before boot and remains connected throughout the test.

The physical device may be a composite HID receiver, but only the UEFI-selected keyboard interface is exercised. No hub, second HID device, arbitrary hot-plug or general USB inventory is introduced.

The observed Toshiba port 4 from V28 is evidence only; no physical or xHCI port number is hard-coded.

## 4. UEFI discovery boundary

The V32 EFI image has two logical stages:

1. **UEFI discovery producer** — may enumerate `EFI_USB_IO_PROTOCOL`, inspect USB device paths/descriptors, select exactly one keyboard, resolve controller/MMIO facts, and construct the bounded handoff.
2. **V32 bridge consumer** — receives the selected handoff, copies the required fields into bridge-local state, and performs all active xHCI programming.

The bridge consumer must not enumerate `EFI_USB_IO_PROTOCOL`, search for another keyboard, reconstruct USB topology to find the device, or derive controller register locations from the root port.

The handoff is a constrained discovery record, not permission to skip the xHCI device state machine.

## 5. Handoff contract and ownership lifetime

The handoff is a bounded, versioned value object. For this EFI experiment its transport is:

`discovery producer -> in-memory handoff -> bridge-local copy`

No file, disk write, UEFI variable, NVRAM state or speculative cross-application channel is required.

The producer must finish all USB discovery reads and all pre-disconnect fact resolution before ownership is released to the bridge.

### 5.1 Required selected-device information

The selected keyboard record must provide, when derivable:

- controller PCI segment/bus/device/function identity;
- xHCI root port, using xHCI's one-based root-port numbering;
- USB speed as a discovery/evidence value;
- VID/PID;
- configuration value;
- interface number and HID boot-keyboard protocol;
- interrupt-IN endpoint and descriptor facts for later gates;
- USB device descriptor information useful to later EP0 sizing/configuration work.

Unavailable optional values remain explicitly unavailable; the bridge must not invent platform-specific values.

### 5.2 Exact controller MMIO handoff

V3 of the handoff adds exact BAR-relative controller register locations resolved by UEFI before `DisconnectController()`:

- `operational_offset`;
- `usbcmd_offset`;
- `usbsts_offset`;
- `crcr_offset`;
- `dcbaap_offset`;
- `config_offset`;
- `doorbell_offset`;
- `runtime_offset`;
- `interrupter0_offset`;
- `iman_offset`;
- `erstsz_offset`;
- `erstba_offset`;
- `erdp_offset`;
- `portsc_offset` for the selected root port;
- `slot_type` from the Supported Protocol Capability covering the selected port.

The bridge consumes these offsets directly. In particular it must **not** reconstruct `PORTSC` as `operational + 0x400 + (root_port - 1) * 0x10`.

These are BAR-relative offsets because the bridge uses `EFI_PCI_IO_PROTOCOL.Mem.Read/Write`, whose register argument is relative to the PCI memory BAR. Physical or virtual MMIO addresses are therefore not transferred as the portable handoff value.

The remaining post-disconnect reads of fixed xHCI capability registers are not USB discovery. If the project later adopts a literal zero-arithmetic MMIO policy, those capability-register offsets should also be handed over explicitly.

### 5.3 Root-port numbering conversion

`EFI_USB_DEVICE_PATH.ParentPortNumber` is zero-based. xHCI root-port numbering used by PORTSC and Slot Context is one-based. The UEFI producer converts the device-path value exactly once (`ParentPortNumber + 1`) when constructing the handoff. The bridge receives the resulting xHCI root-port value and performs no second conversion.

This conversion is a producer-side interpretation of the UEFI device path; it is not bridge-side USB discovery.

### 5.4 Initial EP0 packet-size rule

The USB device descriptor's `bMaxPacketSize0` is not the value that V32 should automatically place into the initial EP0 Context used by Address Device.

For Gate 6's Low-/Full-Speed scope, the initial Input EP0 Context used by Address Device shall use the xHCI-defined default control-endpoint Max Packet Size of **8 bytes**. The UEFI descriptor value is retained only as discovery/evidence for later descriptor/configuration work.

Consequently:

- Low-Speed: initial EP0 Max Packet Size = 8 bytes.
- Full-Speed: initial EP0 Max Packet Size = 8 bytes; later code may read `bMaxPacketSize0` and update EP0 using the appropriate subsequent xHCI mechanism.

Gate 6 performs no pre-Address-Device descriptor transfer.

## 6. UEFI USB-stack ownership handoff

UEFI USB discovery and direct bridge xHCI programming must not operate concurrently against the same controller.

After discovery and handoff-copy validation, Gate 6 must call Boot Services `DisconnectController()` on the selected xHCI controller handle with `DriverImageHandle = NULL` and `ChildHandle = NULL`. The call must succeed before active bridge xHCI MMIO reconfiguration is permitted.

All USB discovery reads must occur before disconnect. After successful disconnect, the bridge must not use `EFI_USB_IO_PROTOCOL`, UEFI USB timers, UEFI-created rings, contexts, DMA buffers, slot IDs or USB device address state.

The future resident-service ownership model across `ExitBootServices` is a separate architecture task.

## 7. PCI controller binding and platform state

The controller is selected through PCI discovery, never by a machine-specific BDF. The handoff identifies the controller path sufficiently to bind the bridge to the same xHCI controller that was disconnected.

The controller handle used for `DisconnectController()` and the `EFI_PCI_IO_PROTOCOL` used for bridge MMIO/DMA must be verified to refer to the same controller.

After disconnect, the bridge must query PCI attributes through `EFI_PCI_IO_PROTOCOL.Attributes()`. Required Memory and Bus Master attributes must be enabled before active operation. Original attributes must be retained and bridge changes restored during safe teardown while the controller is halted and non-DMA.

A valid xHCI MMIO BAR may be either 32-bit or 64-bit. V29's 64-bit-BAR restriction was experiment-specific and is not a Gate 6 requirement.

## 8. DMA and MMIO platform contract

The portable DMA contract is through `EFI_PCI_IO_PROTOCOL`:

- allocate controller-referenced memory with `AllocateBuffer()`;
- map it with `EfiPciIoOperationBusMasterCommonBuffer`;
- program xHCI only with the returned device-visible `DeviceAddress`;
- retain mappings while xHCI may reference them;
- clear all controller references before unmapping/freeing.

Gate 6 deliberately uses **32-bit DMA generically**, rather than rejecting controllers without `HCCPARAMS1.AC64`. Before DMA allocation, the bridge disables `EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE` when supported/active. Each mapped `DeviceAddress` is explicitly checked to be at or below `0xffffffff`. A controller with AC64=0 is therefore not inherently unsupported.

This does not make the test machine-specific: the UEFI PCI I/O protocol is the platform DMA abstraction boundary. The bridge must not assume identity mapping or a particular IOMMU/VT-d configuration.

## 9. Cumulative V32 sequence

V32 is cumulative and carries forward the applicable V29/V30/V31 implementation rather than creating an isolated replacement.

### Phase A — Discovery and ownership

1. Identify the xHCI controller handle and PCI location.
2. Run the UEFI discovery producer.
3. Require exactly one selected HID boot keyboard.
4. Resolve the selected controller's exact MMIO offsets and protocol Slot Type.
5. Validate the handoff magic/version/size and selected record.
6. Copy the selected record into bridge-local state.
7. Finish all UEFI USB discovery and resolution work.
8. Snapshot required PCI attributes.
9. Call `DisconnectController(controller, NULL, NULL)`.
10. Require successful controller/child disconnect.

### Phase B — Fresh controller initialization

11. Bind the same xHCI controller through PCI identity/path.
12. Revalidate controller identity against the handoff and disconnected controller handle.
13. Accept valid 32-bit or 64-bit MMIO BARs.
14. Ensure PCI Memory and Bus Master attributes required by the bridge are enabled.
15. Read HCIVERSION and reject versions below 1.0.
16. Preserve the proven V29 halt/reset/CNR handling.
17. Preserve the proven DMA, scratchpad, CONFIG, DCBAAP, CRCR and event-ring setup.
18. Preserve V30/V31 controller-start and polled-completion machinery.

### Phase C — Selected USB2 port

19. Keep CPU interrupt delivery disabled.
20. Start the controller and verify `HCH=0`.
21. Use only the handoff-provided root port and `portsc_offset`.
22. Read live PORTSC.
23. Require the port to be connected.
24. Use the UEFI-resolved Slot Type belonging to the Supported Protocol Capability covering the selected port.
25. Read live port speed from PORTSC.
26. Accept Low Speed or Full Speed only; reject High Speed and SuperSpeed.
27. Do not scan other ports or perform protocol-capability discovery in the bridge.

### Phase D — USB2-compatible port reset

28. Before asserting Port Reset, handle any pre-existing reset-change (`PRC`) indication so later completion can be attributed to this reset.
29. Apply only the required USB2 PORTSC reset operation and required change-bit handling while preserving unrelated state.
30. Poll for reset completion/change.
31. Consume and validate the corresponding Port Status Change Event by Port ID using the existing polled primary event ring.
32. Re-read PORTSC.
33. Require the device to remain connected and validate the expected enabled/U0 post-reset state.
34. Use the post-reset live PORTSC speed as the authoritative Slot Context Speed.

### Phase E — Enable Slot

35. Issue exactly one Enable Slot command using the handoff-provided Slot Type.
36. Ring Doorbell 0 only.
37. Poll the primary event ring with CPU interrupts disabled.
38. Require one matching Command Completion Event with Success completion code.
39. Validate the command TRB pointer and record the newly returned Slot ID.
40. Never reuse a Slot ID from UEFI or V31.

### Phase F — Fresh Slot and EP0 contexts

41. Determine context size from HCCPARAMS1.CTXSZ.
42. Allocate fresh Input and Output Device Context memory and a fresh EP0 Transfer Ring using the DMA contract.
43. Initialize memory from zero with required alignment and context stride.
44. Populate the DCBAA slot entry with the fresh Output Device Context device-visible address.
45. Set Input Control Context Add Context flags for Slot and EP0 only; no Drop Context flags.
46. Construct the Input Slot Context with Context Entries = 1, selected Root Hub Port, live post-reset Speed, and zero hub-parent fields for this direct-attach/no-hub gate.
47. Construct the Input EP0 Context as a Control endpoint.
48. Set initial EP0 Max Packet Size to 8 bytes.
49. Set Max Burst Size = 0 and MaxPStreams = 0.
50. Set Average TRB Length = 8.
51. Initialize the EP0 Transfer Ring Dequeue Pointer to the fresh ring with DCS = 1.
52. Set the remaining required initial EP0 control fields to specification-defined values.

### Phase G — Address Device

53. Build exactly one Address Device command for the fresh Slot ID.
54. Point the command at the fresh Input Device Context.
55. Ring the command doorbell.
56. Poll the primary event ring.
57. Require the matching Command Completion Event with Success completion code.
58. Validate event Slot ID and Command TRB Pointer.
59. Validate the resulting Output Slot Context has the expected Addressed state and a non-zero USB device address.
60. Do not issue GET_DESCRIPTOR, SET_CONFIGURATION, Configure Endpoint, Evaluate Context, HID Set Protocol, or interrupt-IN transfers in Gate 6.

### Phase H — Safe recovery

61. Halt the controller and confirm `HCH=1`.
62. Reset the controller and confirm reset completion/CNR clear and halted state.
63. Clear CRCR, DCBAAP, CONFIG, event-ring/interrupter references and every other controller pointer established by V29–V32.
64. Only after all controller references are eliminated, unmap and free DMA.
65. Restore any PCI attributes changed by the bridge.
66. Return success only after teardown succeeds.

If the controller cannot be confirmed halted after a submitted command, DMA mappings remain live and the existing non-returning fatal recovery path is used. Memory must never be freed while xHCI may still reference it.

## 10. Deliberately excluded

Gate 6 does not implement:

- High-Speed or SuperSpeed device operation;
- USB3 warm-reset handling;
- hubs;
- hot-plug/disconnect state machines;
- arbitrary USB discovery;
- descriptor reads;
- SET_CONFIGURATION;
- Configure Endpoint;
- Evaluate Context;
- HID Set Protocol;
- keyboard report polling;
- mouse traffic;
- MSI/MSI-X;
- CPU interrupt handlers;
- continuous event pumping.

## 11. CI and acceptance evidence

A successful V32 run must prove, without hard-coded Toshiba port/BDF values:

- UEFI selected exactly one keyboard before active xHCI writes;
- the selected controller identity was bound and the UEFI controller stack was successfully disconnected;
- the bridge made a private copy of the selected device facts and performed no bridge-side USB discovery;
- UEFI resolved the controller MMIO offsets and selected-port PORTSC offset before disconnect;
- the bridge used the supplied offsets directly;
- PCI Memory and Bus Master attributes were valid for bridge operation;
- the controller was freshly halted/reset and initialized using cumulative V29/V30/V31 machinery;
- the selected root port was connected;
- the supplied Slot Type corresponds to the selected port's Supported Protocol Capability;
- live PORTSC speed was Low or Full Speed;
- stale reset-change state was distinguished from the new reset completion indication;
- USB2-compatible port reset completed and the matching Port Status Change Event was consumed/validated;
- the post-reset live speed was used for Slot Context;
- Enable Slot returned a valid fresh Slot ID;
- fresh Input/Output contexts and DCBAA linkage were established;
- initial Address Device EP0 Max Packet Size was exactly 8 bytes;
- Address Device completed successfully;
- the Output Slot Context reached Addressed state with a non-zero USB device address;
- CPU interrupt delivery remained disabled;
- controller-referenced DMA remained live until all references were cleared;
- controller halt/reset recovery succeeded;
- all controller pointers were cleared before DMA release;
- PCI attributes were restored when the bridge changed them.

The QEMU workflow must treat the EFI application's reported `RESULT: Success` as the functional acceptance criterion. A QEMU process that merely boots the image is not sufficient evidence of a passing gate.

The V32 application ends in a `PRESS ANY KEY` state after printing its result. CI waits for the result screen, captures it, sends a key to release the EFI image, and fails the workflow unless OCR finds `RESULT: Success` on the first result screen.

## 12. Current V32 failure and investigation rule

The first CI run using the functional-result check reached the EFI application and correctly failed the workflow. Run `34751530661` reported:

`RESULT: FAIL STAGE=PORT OP=HANDOFF PORT STATUS=Device Error`

This failure occurs at the handoff root-port bounds check, before PORTSC connection testing, port reset, Enable Slot or Address Device. It is therefore **not** evidence that the QEMU PORTSC register is disconnected or that the reset sequence is wrong.

The current investigation is diagnostic only. Before changing port-number semantics or adding bridge discovery, print and inspect the handoff root port and the controller-reported maximum port count. The existing design rule remains:

`EFI USB device-path ParentPortNumber (zero-based) -> UEFI producer converts once to xHCI root port (one-based) -> bridge consumes that value directly.`

No hardware test is authorized until this CI failure is understood and a subsequent source/CI review passes.

## 13. Implementation-review requirements

Before a hardware test, review the V32 source against:

1. complete current project documentation;
2. UEFI specifications for USB I/O, Driver Model, PCI I/O, controller disconnect and DMA mapping;
3. xHCI specification for USB2 port reset, Supported Protocol Capabilities, command/event rings, Enable Slot, Device Contexts and Address Device;
4. Linux xhci-hcd as an implementation cross-check;
5. coreboot/libpayload xHCI code as an implementation cross-check;
6. proven V29/V30/V31 sources and Toshiba hardware results.

The review must explicitly establish:

- discovery and bridge operation are separate;
- all pre-disconnect UEFI resolution is complete before bridge xHCI writes;
- the bridge consumes exact handoff MMIO offsets rather than reconstructing them from topology;
- UEFI USB-stack ownership is released before active bridge xHCI writes;
- V32 preserves the proven DMA/ring/controller lifecycle;
- the bridge binds to the UEFI-selected controller rather than an arbitrary first-match controller;
- the selected root port is used directly and no port scan is performed;
- the selected protocol capability/Slot Type is resolved by port range in UEFI;
- stale PRC is handled before reset completion is interpreted;
- Low/Full Speed is determined from live PORTSC state;
- initial Address Device EP0 Max Packet Size is the 8-byte default;
- all command completions are polled with CPU interrupts disabled;
- DMA cannot be freed while xHCI may still reference it;
- generic 32-bit DMA operation works whether AC64 is set or clear.

No Toshiba hardware test is authorized until this review and the QEMU functional test pass.
