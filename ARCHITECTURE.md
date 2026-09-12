# Architecture

## Target

Reasonably modern x86-64 PCs with UEFI and xHCI 1.0 or later. xHCI 0.x/0.96 controllers are explicitly unsupported.

## xHCI compatibility boundary

The bridge supports controllers reporting HCIVERSION >= 1.0. Controllers reporting an xHCI revision below 1.0 are rejected before controller initialization. The xHCI version establishes the minimum programming model; optional capabilities and later-version features remain capability-detected rather than assumed.

## Service boundary

```text
UEFI -> USB service -> xHCI -> USB HID
                   |
                   +-> keyboard events
                   `-> mouse events
```

UEFI performs device enumeration and produces a bounded device-template snapshot for the bridge. The bridge consumes that selected discovery result; it does not inherit live UEFI xHCI state (rings, contexts, DMA buffers, slot IDs, or run state) and does not rediscover the USB device itself.

During the current EFI test phase, the UEFI discovery producer and bridge consumer are two explicit logical stages in the same EFI image. A future resident-service/OS implementation may transport the same logical handoff through the boot/OS handoff mechanism. The transport mechanism for that later stage is not yet finalized.

The service exposes a deliberately small ABI to the kernel. The exact IPC mechanism is TBD.

## UEFI -> service handoff

UEFI is the authoritative discovery provider for the fixed keyboard/mouse scope. The handoff is versioned and bounded to two devices. It contains xHCI identity/capability facts (including HCSPARAMS1/2/3, HCCPARAMS1, DBOFF, RTSOFF, PAGESIZE and observed HCH/CNR state) plus, for each retained boot-protocol HID keyboard or mouse, root port when derivable, controller path/identity when derivable, VID/PID, configuration value, interface number/protocol, normalized EP0 packet-size information, interrupt-IN endpoint, endpoint descriptors, and the HID report descriptor when retrievable. Non-keyboard/mouse USB devices are excluded.

The handoff is a discovery snapshot only. It does not transfer ownership of UEFI-created rings, contexts, DMA buffers, slot IDs, device addresses, or controller run state. The bridge creates fresh xHCI state and uses the handoff as constrained device/controller selection input.

## xHCI bridge design

```text
UEFI discovery layer
  +-- keyboard/mouse device-template snapshot
  +-- controller identity/path
  +-- root port, speed, config, interface, endpoint, packet size, interval

xHCI bridge core (portable)
  +-- PCI discovery and controller binding
  +-- MMIO
  +-- DMA allocation
  +-- controller halt/reset/init
  +-- command ring
  +-- event ring
  +-- transfer rings
  +-- interrupter
  +-- fixed two-device bring-up
  +-- event pump
  +-- HID boot-protocol decode
  +-- serial frame conversion

platform operations layer
  |-- GNU-EFI test application now
  |-- resident UEFI bridge later
  `-- kernel later, if chosen
```

## Device-template snapshot

```c
struct known_hid_device {
    uint16_t controller_segment;
    uint8_t  controller_bus;
    uint8_t  controller_device;
    uint8_t  controller_function;
    uint8_t  root_port;
    uint8_t  speed;
    uint8_t  configuration_value;
    uint8_t  interface_number;
    uint8_t  interrupt_in_endpoint;
    uint16_t max_packet_size;
    uint8_t  interval;
    uint8_t  kind;  /* keyboard or mouse */
};
```

The controller location fields are a binding constraint derived from UEFI discovery, not a machine-specific constant. The bridge verifies that the selected xHCI controller is the controller associated with the handoff before active reconfiguration.

The bridge verifies the expected device is connected to the selected root port after reset; the snapshot is a constrained recipe, not authority to skip xHCI setup.

## Minimal USB scope

Initial support is limited to one keyboard and one mouse, preferably HID boot-protocol devices.

Out of scope: hubs, hot-plug, arbitrary descriptors, multiple configurations, arbitrary HID report parsing, CPU interrupt routing, disconnect state machines, and any requirement for a fixed PCI or USB port number.

## Conceptual input event

```c
struct input_event {
    uint8_t  type;
    uint8_t  code;
    int16_t  x;
    int16_t  y;
    uint8_t  buttons;
};
```

This is illustrative, not yet a finalized ABI.
