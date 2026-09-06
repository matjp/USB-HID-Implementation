# Architecture

## Target

Reasonably modern x86-64 PCs with UEFI and xHCI.

## Service boundary

USB/xHCI should preferably live outside the kernel:

```text
UEFI -> USB service -> xHCI -> USB HID
                  |
                  +-> keyboard events
                  `-> mouse events
```

The service exposes a deliberately small ABI to the kernel. The exact IPC mechanism is TBD.

## Minimal USB scope

Initial support should be limited to one keyboard and one mouse, preferably HID boot-protocol devices.

Avoid initially supporting arbitrary USB classes, mass storage, audio, cameras, Bluetooth adapters, and other unrelated devices.

## xHCI layers

```text
USB service
  |
  +-- generic USB layer
  |     +-- descriptors
  |     `-- required control/interrupt transfers
  |
  +-- HID layer
  |     +-- boot keyboard
  |     `-- boot mouse
  |
  `-- xHCI backend
        +-- PCI discovery
        +-- MMIO
        +-- DMA allocation
        +-- command ring
        +-- event ring
        +-- transfer rings
        `-- interrupter
```

The exact decomposition remains subject to implementation experience.

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
