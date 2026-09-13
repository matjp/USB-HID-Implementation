# Gate 6 Handoff V4 — Maximum Resolved UEFI → xHCI Bridge Handoff

## Status

**Design authority for the Gate 6 handoff.** V32 implementation must conform to this contract before hardware testing resumes.

## 1. Core rule

The architecture is:

`UEFI USB discovery -> UEFI resolves maximum useful xHCI/device facts -> bounded handoff -> UEFI USB-stack disconnect -> bridge creates fresh xHCI runtime state`

UEFI is the authoritative discovery provider. The bridge is an executor, not a second USB discovery implementation.

The bridge must not search for another keyboard or mouse, enumerate `EFI_USB_IO_PROTOCOL` to rediscover the selected devices, reconstruct USB topology, or derive xHCI register locations from UEFI USB topology when UEFI has already resolved them.

The handoff therefore carries **maximum useful resolved configuration/discovery state**, not merely a small device hint.

## 2. Keyboard and mouse are first-class handoff records

The ABI has one resolved record for the selected keyboard and one resolved record for the selected mouse when present.

Each device record must independently contain every useful device-specific fact that UEFI can resolve before ownership transfer. In particular, a device record carries its own:

- complete bounded UEFI USB device path;
- controller identity/path association;
- xHCI root-port number in xHCI's one-based numbering;
- exact BAR-relative `PORTSC` offset;
- Supported Protocol Capability facts covering that root port;
- Slot Type;
- discovered/evidence USB speed;
- VID/PID and useful USB device-descriptor facts;
- configuration value;
- interface number, class/subclass/protocol and HID boot protocol;
- all useful endpoint descriptor facts, including interrupt-IN endpoint, maximum packet size and interval;
- any other immutable device/topology fact that removes a discovery decision from the bridge.

The keyboard and mouse records must not share a single `portsc_offset` or `slot_type` field. They may be on different root ports even though the current Gate 6 hardware test is expected to use one physical controller.

If a keyboard and mouse are found on different xHCI controllers, the producer must reject the pair for this Gate 6 handoff rather than silently selecting a controller or performing bridge-side discovery. The long-term multi-controller service design is separate.

## 3. Controller-level resolved facts

The controller portion of the handoff should contain every controller fact that UEFI can resolve and that is useful to the bridge without transferring live runtime state.

At minimum this includes:

- PCI segment/bus/device/function;
- PCI vendor/device ID;
- controller device path;
- HCIVERSION;
- HCSParams1/2 facts such as MaxSlots, MaxPorts and scratchpad count;
- HCCPARAMS1 facts such as AC64 and context-size selection;
- page-size information where already resolved;
- exact BAR-relative offsets for the capability-derived operational block, USBCMD, USBSTS, CRCR, DCBAAP and CONFIG;
- exact Doorbell 0 offset;
- exact runtime/interrupter-0 offsets used by the bridge;
- exact primary event-ring register offsets;
- Supported Protocol Capability records/ranges relevant to the selected devices;
- any other immutable controller capability information that removes a discovery or interpretation decision from the bridge.

The bridge may re-read immutable capability registers for consistency checking after ownership transfer. Such reads are validation, not USB discovery. The handoff remains authoritative for resolved topology and access facts.

## 4. Exact path and topology

UEFI owns interpretation of the UEFI USB device path.

For a direct root-port device:

`EFI_USB_DEVICE_PATH.ParentPortNumber (zero-based) -> UEFI converts once -> xHCI Root Hub Port (one-based)`

The bridge receives the resulting xHCI root-port value directly. It does not add one again.

The bridge also receives the exact `portsc_offset` already resolved by UEFI. It must not calculate:

`operational_offset + 0x400 + (root_port - 1) * 0x10`

The complete relevant device path may be retained in the handoff for evidence and future topology use, but the bridge does not parse it to rediscover the root port.

## 5. What is deliberately not handed over

Maximum useful handoff does **not** mean transferring UEFI's live xHCI runtime state.

The bridge must create these afresh:

- controller Run/Stop state;
- command ring;
- event ring;
- DCBAA;
- scratchpad runtime allocations;
- device contexts;
- endpoint transfer rings;
- DMA mappings and device-visible runtime addresses;
- Slot IDs;
- USB device addresses;
- live controller ownership state;
- live post-reset PORTSC state.

UEFI may hand over expected/evidence values for mutable state such as discovered speed, but the bridge reads live state after it owns the freshly initialized controller. Live xHCI state is not inherited.

## 6. Gate 6 device-state boundary

The current hardware gate remains intentionally narrow:

- exactly one pre-connected intended keyboard is required for the first live-device test;
- a composite receiver may also expose a mouse, and the mouse must be represented in the handoff when UEFI discovers it;
- only the keyboard is exercised by the first Gate 6 Address Device test;
- mouse traffic remains a later gate;
- only USB2-compatible Low Speed and Full Speed are supported;
- hubs, High Speed, SuperSpeed, hot-plug and general USB discovery remain excluded.

The ABI nevertheless must be capable of carrying both keyboard and mouse resolved facts now. The later mouse implementation must not require redesigning the discovery boundary.

## 7. Bridge responsibilities after handoff

After successful `DisconnectController()` the bridge:

1. validates the bounded handoff;
2. binds to the exact controller identified by UEFI;
3. establishes PCI Memory/Bus Master state through `EFI_PCI_IO_PROTOCOL`;
4. halts/resets the controller and creates fresh xHCI runtime state;
5. uses the handoff's resolved register offsets directly;
6. uses each selected device record's exact root port, `PORTSC` offset and Slot Type directly;
7. reads mutable live PORTSC state needed for safe operation;
8. performs Port Reset, Enable Slot and Address Device using fresh bridge-owned state;
9. tears down safely and releases DMA only after controller references are eliminated.

No bridge-side USB inventory or topology search is permitted.

## 8. Design principle

The permanent rule is:

> **UEFI hands over the maximum useful resolved xHCI/device configuration state for the selected keyboard and mouse. The bridge creates all live xHCI runtime state afresh and performs no USB discovery.**

This is the boundary V32 must implement.
