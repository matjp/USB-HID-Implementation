# Gate 6 Handoff V3 — Superseded by V4

## Status

**V3 is retained as historical ABI documentation. The active Gate 6 handoff contract is `HANDOFF_V4.md`.**

The V4 contract strengthens the original V3 rule from “UEFI resolves useful facts” to:

> **UEFI hands over the maximum useful resolved xHCI/device configuration state for the selected keyboard and mouse. The bridge creates all live xHCI runtime state afresh and performs no USB discovery.**

## V3 historical scope

V3 introduced UEFI-resolved controller-relative MMIO facts, including:

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
- selected `portsc_offset`;
- selected `slot_type`.

It also established the one-time UEFI `ParentPortNumber + 1` conversion and the rule that the bridge must not reconstruct `PORTSC` from the root port.

## V4 correction

The V3 structure is not sufficient as the final two-device ABI because a single controller-wide `portsc_offset` and `slot_type` cannot represent independent keyboard and mouse root-port/protocol facts.

V4 therefore makes resolved xHCI access/topology facts **per-device**, while also expanding the controller record to carry maximum useful immutable controller capability facts.

For each selected keyboard/mouse, UEFI must resolve, where available:

- complete relevant UEFI USB device path;
- controller association;
- xHCI one-based root port;
- exact BAR-relative `PORTSC` offset;
- covering Supported Protocol Capability and Slot Type;
- speed evidence;
- VID/PID and useful descriptor facts;
- configuration/interface/HID facts;
- interrupt-IN endpoint and descriptor facts;
- other immutable topology/device facts useful to later xHCI setup.

The bridge must not rediscover any of those facts.

## Runtime-state boundary remains unchanged

The bridge still creates all live runtime state afresh:

- command/event rings;
- DCBAA and device contexts;
- transfer rings;
- DMA mappings;
- Slot IDs;
- USB device addresses;
- controller run state.

Mutable live PORTSC state is read by the bridge after fresh controller initialization; UEFI's speed value is evidence, not inherited runtime state.

## Port-numbering rule remains unchanged

`EFI_USB_DEVICE_PATH.ParentPortNumber` is zero-based. UEFI performs the only conversion to xHCI's one-based root-port numbering. The bridge performs no second conversion.

## Gate 6 scope

The first live test still exercises only the selected keyboard and stops after Address Device. If a mouse is present, its resolved V4 record is carried now, but mouse traffic remains a later gate.
