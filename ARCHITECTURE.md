# Architecture

## Target

Reasonably modern x86-64 PCs with UEFI and xHCI.

## Service boundary

```text
UEFI -> USB service -> xHCI -> USB HID
                   |
                   +-> keyboard events
                   `-> mouse events
```

UEFI performs device enumeration and passes a small device-template snapshot to the bridge. The bridge does not inherit live UEFI xHCI state (rings, contexts, DMA buffers, slot IDs, or run state).

The service exposes a deliberately small ABI to the kernel. The exact IPC mechanism is TBD.

## xHCI bridge design

```text
UEFI discovery layer
  +-- keyboard/mouse device-template snapshot
  +-- root port, speed, config, interface, endpoint, packet size, interval

xHCI bridge core (portable)
  +-- PCI discovery
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

The bridge verifies the expected device is connected to that root port after reset; the snapshot is a constrained recipe, not authority to skip xHCI setup.

## Minimal USB scope

Initial support is limited to one keyboard and one mouse, preferably HID boot-protocol devices.

Out of scope: hubs, hot-plug, arbitrary descriptors, multiple configurations, arbitrary HID report parsing, CPU interrupt routing, and disconnect state machines.

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