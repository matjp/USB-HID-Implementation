# Gate 6 Design — UEFI-Selected Keyboard to Address Device

Status: design only; no V32 implementation is authorized by this document alone.

## 1. Design authority

Gate 6 follows the current project architecture and decisions: UEFI is the authoritative USB discovery provider; the bridge establishes fresh xHCI state; the Toshiba Satellite P50 is the sacrificial hardware target; V29/V30/V31 are cumulative implementation gates; and no hardware test is permitted before source and CI review.

**UEFI discovers and selects the keyboard. The V32 bridge consumes that selected record and does not rediscover USB devices. The bridge does not reuse UEFI xHCI runtime state.**

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

1. **UEFI discovery producer** — may enumerate `EFI_USB_IO_PROTOCOL`, inspect USB device paths/descriptors, select exactly one keyboard, and construct the bounded handoff.
2. **V32 bridge consumer** — receives the selected handoff, copies the required fields into bridge-local state, and performs all active xHCI programming.

The bridge consumer must not enumerate `EFI_USB_IO_PROTOCOL`, search for another keyboard, reconstruct USB topology to find the device, or use USB handle counts as a discovery mechanism.

The handoff is a constrained discovery record, not permission to skip the xHCI device state machine.

## 5. Handoff contract and ownership lifetime

The handoff is a bounded, versioned value object. For this EFI experiment its transport is:

`discovery producer -> in-memory handoff -> bridge-local copy`

No file, disk write, UEFI variable, NVRAM state or speculative cross-application channel is required.

The producer must finish all USB discovery reads before ownership is released to the bridge.

### 5.1 Required selected-device information

The selected keyboard record must provide, when derivable:

- controller PCI segment/bus/device/function identity;
- root port;
- USB speed as a discovery/evidence value;
- VID/PID;
- configuration value;
- interface number and HID boot-keyboard protocol;
- interrupt-IN endpoint and descriptor facts for later gates;
- USB device descriptor information useful to later EP0 sizing/configuration work.

Unavailable optional values remain explicitly unavailable; the bridge must not invent platform-specific values.

### 5.2 Initial EP0 packet-size rule

The USB device descriptor's `bMaxPacketSize0` is not the value that V32 should automatically place into the initial EP0 Context used by Address Device.

For Gate 6's Low-/Full-Speed scope, the initial Input EP0 Context used by Address Device shall use the xHCI-defined default control-endpoint Max Packet Size of **8 bytes**. The xHCI specification defines this speed-dependent default and states that the device descriptor can be read afterward to discover the actual Full-Speed value when necessary. citeturn727860search24turn727860search25

Consequently:

- Low-Speed: initial EP0 Max Packet Size = 8 bytes.
- Full-Speed: initial EP0 Max Packet Size = 8 bytes; later code may read `bMaxPacketSize0` and, if different, update EP0 using the appropriate subsequent xHCI mechanism.

Gate 6 does not perform that later descriptor read or EP0 update. The UEFI handoff may retain the descriptor value as evidence for later gates, but V32 must not require it to construct Address Device.

## 6. UEFI USB-stack ownership handoff

UEFI USB discovery and direct bridge xHCI programming must not operate concurrently against the same controller.

After discovery and handoff-copy validation, Gate 6 must call Boot Services `DisconnectController()` on the selected xHCI controller handle with `DriverImageHandle = NULL` and `ChildHandle = NULL`. This disconnects all drivers managing the controller and destroys its children. The call must succeed before any active bridge xHCI MMIO reconfiguration is permitted. citeturn874430search24

All USB discovery reads must occur before disconnect. After successful disconnect, the bridge must not use `EFI_USB_IO_PROTOCOL`, UEFI USB timers, UEFI-created rings, contexts, DMA buffers, slot IDs or USB device address state.

If disconnect fails or the bridge cannot establish that the selected UEFI controller stack is quiesced, Gate 6 stops before active xHCI writes.

The future resident-service ownership model across `ExitBootServices` is a separate architecture task.

## 7. PCI controller binding and platform state

The controller is selected through PCI discovery, never by a machine-specific BDF. The handoff identifies the controller path sufficiently to bind the bridge to the same xHCI controller that was disconnected.

The controller handle used for `DisconnectController()` and the `EFI_PCI_IO_PROTOCOL` used for bridge MMIO/DMA must be verified to refer to the same controller.

After disconnect, the bridge must query PCI attributes through `EFI_PCI_IO_PROTOCOL.Attributes()` and ensure the required PCI Memory and Bus Master attributes are enabled and supported before DMA or active xHCI operation. The original attribute state must be retained and any attributes newly enabled by the bridge must be restored during safe teardown while the controller is halted and non-DMA.

A valid xHCI MMIO BAR may be either 32-bit or 64-bit. V29's 64-bit-BAR restriction was experiment-specific and is not a Gate 6 requirement.

## 8. DMA and MMIO platform contract

The existing `efi/xhci_bridge/efi_platform.c` is scaffolding and must not be reused unchanged. V32 must preserve the proven V29/V30/V31 platform contract:

- allocate controller-referenced memory with `EFI_PCI_IO_PROTOCOL.AllocateBuffer()`;
- map with `EfiPciIoOperationBusMasterCommonBuffer`;
- program xHCI only with returned device-visible DMA addresses;
- retain exact allocation size/page count and mapping handle for teardown;
- honor xHCI alignment explicitly;
- use conservative 32-bit MMIO accesses and split 64-bit register accesses where required by the established implementation;
- retain DMA mappings until all controller references have been eliminated.

V32 must not silently replace this with identity-mapped `AllocatePages()` memory or native 64-bit MMIO access merely because a generic platform abstraction exists.

## 9. Cumulative V32 sequence

V32 is cumulative and must carry forward the applicable V29/V30/V31 implementation rather than creating an isolated replacement.

### Phase A — Discovery and ownership

1. Identify the xHCI controller handle and PCI location.
2. Run the UEFI discovery producer.
3. Require exactly one selected HID boot keyboard.
4. Validate the handoff magic/version/size and selected record.
5. Copy the selected record into bridge-local state.
6. Finish all UEFI USB discovery reads.
7. Snapshot required PCI attributes.
8. Call `DisconnectController(controller, NULL, NULL)`.
9. Require successful controller/child disconnect.

### Phase B — Fresh controller initialization

10. Bind the same xHCI controller through PCI identity/path.
11. Revalidate controller identity against the handoff and disconnected controller handle.
12. Validate the MMIO BAR type; accept valid 32-bit or 64-bit BARs.
13. Ensure PCI Memory and Bus Master attributes required by the bridge are enabled.
14. Read HCIVERSION and reject versions below 1.0.
15. Preserve the proven V29 halt/reset/CNR handling.
16. Preserve the proven DMA, scratchpad, CONFIG, DCBAAP, CRCR and event-ring setup.
17. Preserve V30/V31 controller-start and polled-completion machinery.

### Phase C — Selected USB2 port

18. Keep CPU interrupt delivery disabled.
19. Start the controller and verify `HCH=0`.
20. Select only the handoff-provided root port.
21. Read its live PORTSC.
22. Require the port to be connected.
23. Identify the **Supported Protocol Capability** whose Port Offset/Port Count range contains the selected root port; use that capability as the port's protocol/slot-type description. Do not use the first protocol capability merely because it appears first in the extended-capability list. Linux and iPXE both model protocol capability lookup by port range. citeturn568292search0turn568292search2
24. Read the live port speed from PORTSC.
25. Accept Low Speed or Full Speed only; reject High Speed and SuperSpeed.
26. Do not scan other ports.

### Phase D — USB2-compatible port reset

27. Before asserting Port Reset, identify and acknowledge only the pre-existing reset-change indication needed to make the subsequent reset completion observable; in particular, clear a stale `PRC` using the required RW1C write semantics without rewriting unrelated PORTSC state.
28. Apply only the required USB2 PORTSC reset operation and required change-bit handling while preserving unrelated state.
29. Poll for reset completion/change.
30. Consume and validate the corresponding Port Status Change Event by Port ID using the existing polled primary event ring.
31. Re-read PORTSC.
32. Require the device to remain connected.
33. Validate the expected enabled/U0 post-reset state for the selected USB2 port.
34. Use the post-reset live PORTSC speed as the authoritative Slot Context Speed.

### Phase E — Enable Slot

35. Obtain the Slot Type from the Supported Protocol Capability covering the selected root port.
36. Issue exactly one Enable Slot command with that Slot Type.
37. Ring Doorbell 0 only.
38. Poll the primary event ring with CPU interrupts disabled.
39. Require one matching Command Completion Event with Success completion code.
40. Validate the command TRB pointer and record the newly returned Slot ID.
41. Never reuse a Slot ID from UEFI or V31.

### Phase F — Fresh Slot and EP0 contexts

42. Determine context size from `HCCPARAMS1.CTXSZ`.
43. Allocate fresh Input and Output Device Context memory and a fresh EP0 Transfer Ring using the V29 DMA contract.
44. Initialize memory from zero with required alignment and context stride.
45. Populate the DCBAA slot entry with the fresh Output Device Context device-visible address.
46. Set Input Control Context Add Context flags for Slot and EP0 only; no Drop Context flags.
47. Construct the Input Slot Context with Context Entries = 1, selected Root Hub Port, live post-reset Speed, and zero hub-parent fields for this direct-attach/no-hub gate.
48. Construct the Input EP0 Context as a Control endpoint.
49. Set initial EP0 Max Packet Size to **8 bytes**.
50. Set Max Burst Size = 0 and MaxPStreams = 0.
51. Set the control endpoint's Average TRB Length to the xHCI-defined value of 8.
52. Initialize the EP0 Transfer Ring Dequeue Pointer to the fresh ring with DCS = 1.
53. Set the remaining required initial EP0 control fields to their specification-defined values.

### Phase G — Address Device

54. Build exactly one Address Device command for the fresh Slot ID.
55. Point the command at the fresh Input Device Context.
56. Ring the command doorbell.
57. Poll the primary event ring.
58. Require the matching Command Completion Event with Success completion code.
59. Validate event Slot ID and Command TRB Pointer.
60. Validate the resulting Output Slot Context has the expected Addressed state and a non-zero USB device address.
61. Do not issue GET_DESCRIPTOR, SET_CONFIGURATION, Configure Endpoint, Evaluate Context, HID Set Protocol, or interrupt-IN transfers in Gate 6.

### Phase H — Safe recovery

62. Halt the controller and confirm `HCH=1`.
63. Reset the controller and confirm reset completion/CNR clear and halted state.
64. Clear CRCR, DCBAAP, CONFIG, event-ring/interrupter references and every other controller pointer established by V29–V32.
65. Only after all controller references are eliminated, unmap and free DMA.
66. Restore any PCI attributes enabled by the bridge.
67. Return success only after all teardown succeeds.

If the controller cannot be confirmed halted after a submitted command, DMA mappings must remain live and the existing non-returning fatal recovery path must be used. Memory must never be freed while xHCI may still reference it.

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

## 11. Acceptance evidence

A successful V32 run must prove, without hard-coded Toshiba port/BDF values:

- UEFI selected exactly one keyboard before active xHCI writes;
- the selected controller identity was bound and the UEFI controller stack was successfully disconnected;
- the bridge made a private copy of the selected device facts and performed no bridge-side USB discovery;
- PCI Memory and Bus Master attributes were valid for bridge operation;
- the controller was freshly halted/reset and initialized using the cumulative V29/V30/V31 machinery;
- the selected root port was connected;
- the selected root port's Supported Protocol Capability was matched by port range;
- live PORTSC speed was Low or Full Speed;
- stale reset-change state was distinguished from the new reset completion indication;
- USB2-compatible port reset completed and the matching Port Status Change Event was consumed/validated;
- the post-reset live speed was used for Slot Context;
- Enable Slot used the Slot Type belonging to the selected root port's Supported Protocol Capability and returned a fresh Slot ID;
- fresh Input/Output contexts and DCBAA linkage were established;
- the initial Address Device EP0 Max Packet Size was exactly 8 bytes;
- Address Device completed successfully;
- the Output Slot Context reached Addressed state and has a non-zero USB device address;
- CPU interrupt delivery remained disabled;
- controller-referenced DMA remained live until all references were cleared;
- controller halt/reset recovery succeeded;
- all controller pointers were cleared before DMA release;
- PCI attributes were restored when the bridge changed them.

## 12. Implementation-review gate

Before V32 source is created, review the planned implementation against:

1. the complete current project documentation;
2. UEFI specifications for USB I/O, Driver Model, PCI I/O, controller disconnect and DMA mapping;
3. xHCI specification for USB2 port reset, Supported Protocol Capabilities, command/event rings, Enable Slot, Device Contexts and Address Device;
4. Linux xhci-hcd as an implementation cross-check;
5. coreboot/libpayload xHCI code as an implementation cross-check;
6. proven V29/V30/V31 sources and their Toshiba hardware results.

The review must explicitly establish that:

- discovery and bridge operation are separate stages;
- UEFI USB-stack ownership is released before active bridge xHCI writes;
- V32 preserves the proven DMA/ring/controller lifecycle rather than substituting the incomplete scaffold;
- the bridge binds to the UEFI-selected controller rather than the first matching xHCI controller;
- the selected root port is used directly and no port scan is performed;
- the port's Supported Protocol Capability is selected by port range;
- stale PRC is cleared/handled before the reset so the resulting Port Status Change Event is attributable to the new reset;
- Low/Full Speed is determined from live xHCI PORTSC state;
- initial Address Device EP0 Max Packet Size is the 8-byte default, not the later descriptor value;
- the Address Device contexts contain only the Slot and EP0 contexts required at this stage;
- all command completions are polled with CPU interrupts disabled;
- failure paths cannot free DMA while xHCI may still reference it.

No CI build or Toshiba hardware test is authorized until this review passes.

## 13. Technical references

The xHCI specification defines the Address Device EP0 default Max Packet Size and the later Full-Speed descriptor/update path. citeturn727860search24turn727860search25

Linux and iPXE locate Supported Protocol Capabilities by matching the port offset/count range rather than assuming the first protocol capability applies to every port. citeturn568292search0turn568292search2

UEFI defines `DisconnectController()` so a NULL `DriverImageHandle` disconnects all drivers managing the controller and a NULL `ChildHandle` destroys all children. citeturn874430search24
