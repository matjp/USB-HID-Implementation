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

## D014 — Gate 6 starts with one known pre-connected HID keyboard interface

Gate 6 uses one pre-connected USB HID keyboard interface discovered by UEFI before active xHCI reconfiguration. The physical device may be a composite receiver, including a combined wireless keyboard/mouse dongle. The UEFI discovery snapshot identifies the live keyboard interface and its root port/endpoint facts. The xHCI bridge must still perform normal port-state/reset, Enable Slot, Address Device, and subsequent device setup; discovery facts are hints and validation inputs, not authority to skip controller/device setup. Mouse traffic remains out of scope until keyboard bring-up is stable.

## D015 — Keep Gate 6 command scope incremental

Gate 6 must not combine keyboard bring-up with general USB functionality. The implementation advances through small observable stages: identify the discovered root port, reset it, enable one slot, build/address the device context, retrieve only the descriptors needed to identify/configure the keyboard, select the HID boot interface, set configuration/protocol, and stop before continuous report polling unless the preceding stage has passed. Each command completion is validated before the next command is issued.

## D016 — No CPU interrupts during initial Gate 6 bring-up

The first Gate 6 implementation continues the V31 polling model. Command and transfer completion events are consumed by polling the event ring with CPU interrupt delivery disabled. Interrupt routing and MSI/MSI-X are deferred until the command/transfer path is proven on the Toshiba.

## D017 — Every gate is cumulative, not an isolated experiment

Each numbered test version must build on the latest proven implementation and carry forward all applicable previously passed gates. A new version may add or tighten one new gate, but it must not replace, bypass, or independently reimplement previously proven controller lifecycle, DMA, discovery, ring, completion, safety, or teardown behavior. In particular, V32 must include the proven V28 UEFI discovery phase plus the proven V29, V30, and V31 controller functionality before adding Gate 6 device bring-up.

The required progression is therefore cumulative: **V28 discovery → V29 halted initialization → V30 controller run/halt/recovery → V31 Enable Slot/completion → V32 discovery + all prior gates + Port Reset/Address Device → later versions extend the same implementation.** A gate result only proves the cumulative implementation represented by that version; isolated replacement tests do not count as progression of the implementation.
