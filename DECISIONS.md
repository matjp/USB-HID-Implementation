# Design Decisions

## D001 — Keep USB/xHCI out of the kernel

The preferred architecture places USB/xHCI in a separate service and gives the kernel a small input interface.

## D002 — Target xHCI

Modern PCs commonly expose USB through xHCI. The implementation should discover an xHCI controller through PCI rather than hard-code a machine-specific PCI address.

## D003 — Narrow USB scope

The first implementation targets one keyboard and one mouse rather than general USB device support.

## D004 — Prefer HID boot protocol initially

Boot-protocol USB HID keyboard/mouse support should minimize HID parsing complexity.

## D005 — Do not experiment with production DMA

The Dell XPS 8950 is a dual-boot production system. Active xHCI/DMA work moves to sacrificial hardware first.

## D006 — UEFI hands off device facts, not live xHCI state

UEFI performs device enumeration and passes a small device-template snapshot (known_hid_device) to the bridge. The bridge does not inherit or reuse UEFI's device slot IDs, command/transfer/event ring pointers, DCBAA, device-context addresses, DMA buffers, controller run state, or current USB address. UEFI's initial discovery contributes the root port, interface, endpoint, packet size, polling interval, and configuration value. The bridge verifies the expected device is connected to that root port after reset; the snapshot is a constrained recipe, not authority to skip xHCI setup.

## D007 — Coreboot is a code reference, not a firmware replacement

Coreboot/libpayload xHCI code may be reused or adapted as a source, but the target Dell cannot be flashed with coreboot and firmware replacement is not part of the project.

## D008 — Consolidate V20–V27 into a self-contained xhci_bridge_init()

The existing V03–V27 experiments prove individual register transitions but do not accumulate: each EFI binary starts in a fresh firmware-owned controller state. Rather than adding another isolated diagnostic branch, the next work consolidates halt → reset → DMA allocation → DCBAA/command/event rings → readback → safe teardown into one reusable xhci_bridge_init() with a portable platform operations layer.

## D009 — Start with empty device hints

The first xhci_bridge_init() test runs with zero device hints — full controller initialization, DMA allocation, ring setup, and safe teardown only. Keyboard and mouse hints are added only after clean initialization is repeatable.

## D010 — Support xHCI 1.0 and later only

The project has a hard compatibility boundary at xHCI 1.0. A controller is supported only when its HCIVERSION is >= 1.0. xHCI 0.x/0.96 controllers are explicitly unsupported and must be rejected before controller initialization.

This boundary removes the legacy 0.96 compatibility path from the project while retaining capability discovery for optional features and later xHCI revisions. The xHCI specification is the normative baseline; Linux, coreboot/libpayload, and EDK2 are implementation cross-references only.

## D011 — UEFI is the authoritative keyboard/mouse discovery provider

The service shall consume the maximum useful keyboard/mouse discovery information supplied by UEFI and shall not receive a general USB-device inventory. Only boot-protocol HID keyboard and mouse interfaces enter the handoff. The handoff is a versioned discovery snapshot, not a transfer of ownership of UEFI-created xHCI rings, contexts, DMA buffers, slot IDs, or live controller state.

## D012 — UEFI is the portable DMA abstraction boundary

The xHCI bridge must not depend on a particular machine's IOMMU/VT-d configuration. Platform-specific DMA addressing/remapping is delegated to EFI_PCI_IO_PROTOCOL: controller-referenced memory is allocated with AllocateBuffer(), mapped with EfiPciIoOperationBusMasterCommonBuffer, and programmed into xHCI using only the returned device-visible addresses. Mappings remain live while xHCI may DMA and are released only after controller references are cleared. IOMMU/VT-d state may be recorded for diagnosis, but it is not a normal portability prerequisite.

## D013 — Poll command completions before enabling CPU interrupts

The first command-ring test uses exactly one Enable Slot command and polls the primary event ring rather than enabling CPU interrupt delivery. This isolates command-ring, doorbell, event-ring cycle-state, completion-event parsing, and ERDP acknowledgement from MSI/MSI-X/interrupt-handler behavior. CPU interrupts remain disabled until command/event correctness is established.

## D014 — Gate 6 uses an authoritative UEFI-selected keyboard

Gate 6 begins with a UEFI discovery producer selecting one in-scope HID boot keyboard before active xHCI reconfiguration. The selected keyboard record is authoritative as the discovery result: the bridge consumes that record and does not perform a second `EFI_USB_IO_PROTOCOL` keyboard-discovery scan. The physical device may be a composite receiver, including a combined wireless keyboard/mouse dongle. The bridge must still perform normal port-state/reset, Enable Slot, Address Device, and subsequent device setup. Mouse traffic remains out of scope until keyboard bring-up is stable.

## D015 — Keep Gate 6 command scope incremental

Gate 6 must not combine keyboard bring-up with general USB functionality. The implementation advances through small observable stages: identify the selected root port, reset it, enable one slot, build/address the device context, retrieve only the descriptors needed to identify/configure the keyboard, select the HID boot interface, set configuration/protocol, and stop before continuous report polling unless the preceding stage has passed. Each command completion is validated before the next command is issued.

## D016 — No CPU interrupts during initial Gate 6 bring-up

The first Gate 6 implementation continues the V31 polling model. Command and transfer completion events are consumed by polling the event ring with CPU interrupt delivery disabled. Interrupt routing and MSI/MSI-X are deferred until the command/transfer path is proven on the Toshiba.

## D017 — Every gate is cumulative, not an isolated experiment

Each numbered test version must build on the latest proven implementation and carry forward all applicable previously passed gates. A new version may add or tighten one new gate, but it must not replace, bypass, or independently reimplement previously proven controller lifecycle, DMA, discovery, ring, completion, safety, or teardown behavior. Gate 6 is cumulative in functionality, but its USB discovery and xHCI bridge stages are explicitly separated within the test image.

The required progression is therefore cumulative: **V28 UEFI discovery -> V29 halted initialization -> V30 controller run/halt/recovery -> V31 Enable Slot/completion -> new V32 authoritative handoff consumer + all prior gates + Port Reset/Address Device -> later versions extend the same implementation.** A gate result only proves the cumulative implementation represented by that version; isolated replacement tests do not count as progression of the implementation.

## D018 — Gate 6 handoff transport is in-memory for the EFI experiment

The Gate 6 test image contains two logical stages: a UEFI discovery producer and a V32 bridge consumer. The producer constructs a bounded versioned handoff in memory and passes it directly to the bridge consumer. The bridge copies the selected device facts into bridge-local state before active xHCI reconfiguration and then operates without enumerating `EFI_USB_IO_PROTOCOL` handles.

This deliberately avoids introducing a disk file, UEFI variable, firmware NVRAM dependency, or speculative cross-application transport into the hardware experiment. The same logical handoff ABI may later be transported by the boot/OS handoff mechanism when the resident service architecture is implemented; that is a separate design task.

## D019 — Gate 6 binds the bridge to the UEFI-selected controller

The handoff should identify the PCI controller path associated with the selected USB device sufficiently to bind the bridge to the corresponding xHCI controller. The bridge may discover xHCI controllers through PCI, as required by D002, but it must not simply select the first matching xHCI controller when the UEFI handoff identifies another controller. No machine-specific PCI address is hard-coded.

UEFI PCI device paths define a path through PCI device/function nodes, and `EFI_PCI_IO_PROTOCOL.GetLocation()` can provide the PCI segment, bus, device and function for a controller. These are suitable discovery constraints rather than assumptions about a fixed platform address.

## D020 — Gate 6 is limited to USB2-compatible Low/Full Speed HID

The first live-device Gate 6 path deliberately supports only a keyboard whose selected xHCI port is operating on the USB2-compatible path at Low Speed or Full Speed. High-Speed and SuperSpeed device operation, including USB3-specific warm-reset handling, are outside Gate 6. The implementation must read the live xHCI PORTSC speed after controller start/reset and use that live value for xHCI context construction; the UEFI-discovered speed is an expectation/evidence value rather than the final authority. A keyboard plugged into a USB3-capable connector is not treated as a SuperSpeed device merely because of the connector; the device's negotiated xHCI port state determines the path.

## D021 — UEFI USB-stack ownership must be quiesced before bridge xHCI writes

UEFI USB discovery and direct bridge xHCI programming must not operate concurrently against the same controller. After the discovery producer has copied and validated the bounded handoff, Gate 6 must stop the UEFI USB host-controller/bus-driver stack for the selected xHCI controller before the bridge performs any active xHCI MMIO reconfiguration.

The portable mechanism selected for this EFI experiment is the UEFI Driver Model `DisconnectController()` operation on the selected xHCI controller handle, with `DriverImageHandle=NULL` and `ChildHandle=NULL`. This disconnects all drivers managing the controller and destroys all children before they are disconnected. The implementation must verify that the disconnect succeeds before proceeding. After that point the bridge must not use `EFI_USB_IO_PROTOCOL`, UEFI USB timers, or UEFI-created xHCI runtime resources. The bridge then establishes fresh xHCI controller/device state and its own DMA mappings.

This is a controller-ownership/quiesce transition, not reuse of UEFI runtime state. The future resident-service architecture requires a separate lifetime/ownership design around `ExitBootServices`; this decision is specifically for the Gate 6 EFI experiment.

## D022 — Gate 6 uses the xHCI default EP0 packet size for Address Device

For the initial Address Device command, the bridge must use the xHCI-defined speed-dependent default Max Packet Size for EP0. Within Gate 6's Low-/Full-Speed scope this value is 8 bytes. The USB device descriptor's `bMaxPacketSize0` is not substituted into the initial Address Device context. A later gate may read the descriptor and, for a Full-Speed device when the actual value differs, update the EP0 context using the appropriate xHCI mechanism. Gate 6 performs no pre-Address-Device descriptor transfer.

## D023 — Enable Slot uses the protocol capability covering the selected port

Gate 6 must locate the Supported Protocol Capability whose Port Offset/Port Count range contains the selected root port and use that capability's protocol/Slot Type for the Enable Slot command. The bridge must not assume that the first Supported Protocol Capability in the extended-capability list applies to the selected port. This is a portability requirement for controllers exposing multiple protocol ranges.

## D024 — Reset completion must be attributable to this reset

Before issuing the USB2 Port Reset operation, Gate 6 must handle any pre-existing reset-change (`PRC`) state so that the later Port Status Change Event and PRC transition can be attributed to the reset issued by V32. The operation must use the PORTSC RW1C semantics for change bits and preserve unrelated state. A stale PRC indication must not be mistaken for successful completion of the new reset.
