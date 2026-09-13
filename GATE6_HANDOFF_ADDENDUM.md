# Gate 6 Handoff Addendum

This addendum is part of the Gate 6 design and clarifies the handoff boundary in `GATE6_DESIGN.md` and `HANDOFF_V4.md`.

## Mandatory rule

**UEFI must hand over the maximum useful resolved xHCI/device configuration state that can be established before ownership transfer for the selected keyboard and mouse. The bridge must not rediscover or reconstruct information that UEFI has already resolved.**

This is stronger than merely handing over a root port, VID/PID and endpoint hint.

### UEFI must resolve, where available

For the controller:

- PCI identity/path;
- HCIVERSION and relevant HCS/HCC capability facts;
- operational/command/status/CRCR/DCBAAP/CONFIG offsets;
- Doorbell 0 and primary runtime/interrupter/event-ring offsets;
- relevant Supported Protocol Capability ranges and Slot Types;
- other immutable controller facts needed by the bridge.

For **each** selected HID device:

- complete relevant UEFI USB device path;
- controller association;
- xHCI one-based root port;
- exact BAR-relative PORTSC offset;
- covering Supported Protocol Capability facts;
- Slot Type;
- discovered speed evidence;
- VID/PID and useful descriptor facts;
- configuration/interface/HID facts;
- interrupt-IN endpoint and descriptor facts;
- other immutable device/topology facts useful to later xHCI setup.

Keyboard and mouse records must carry their own resolved port/access facts. A single controller-wide `portsc_offset` or `slot_type` is insufficient for a two-device handoff.

### Bridge must create, not inherit

The handoff does not transfer live UEFI xHCI runtime state. The bridge creates fresh command/event rings, DCBAA, contexts, transfer rings, DMA mappings, Slot IDs and USB address state. Mutable live PORTSC state is read by the bridge after fresh controller initialization.

### No discovery leakage

After handoff, the bridge must not:

- enumerate `EFI_USB_IO_PROTOCOL` to find devices;
- search other ports for a keyboard or mouse;
- parse USB device paths to discover topology;
- rediscover Supported Protocol Capabilities to determine Slot Type;
- calculate PORTSC from the root-port number when UEFI has supplied the exact offset.

The UEFI-to-xHCI port conversion occurs exactly once in the UEFI producer.

### Gate 6 scope

The first live test still exercises only the selected keyboard and stops after Address Device. If a mouse is present, UEFI must place its resolved record in the handoff now, but mouse traffic remains a later gate. No redesign of the discovery boundary should be required to add mouse operation later.
