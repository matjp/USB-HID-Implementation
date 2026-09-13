#ifndef XHCI_HANDOFF_V4_H
#define XHCI_HANDOFF_V4_H

#include <efi.h>

/*
 * Gate 6 Handoff V4
 *
 * UEFI is authoritative for USB discovery and resolves the maximum useful
 * immutable controller/device facts before ownership transfer. The bridge
 * consumes these facts and creates all live xHCI runtime state itself.
 *
 * This header intentionally contains no live xHCI resources: no rings,
 * contexts, DMA mappings, Slot IDs, USB addresses, or controller run state.
 */

#define XHCI_HANDOFF_V4_MAGIC   0x48494458U
#define XHCI_HANDOFF_V4_VERSION 4U
#define XHCI_HANDOFF_V4_MAX_PATH_BYTES 512U
#define XHCI_HANDOFF_V4_MAX_ENDPOINTS 8U
#define XHCI_HANDOFF_V4_MAX_PROTOCOLS 8U

#define XHCI_HID_KIND_KEYBOARD 1U
#define XHCI_HID_KIND_MOUSE    2U

#define XHCI_HANDOFF_DEVICE_PRESENT 0x01U

/* Bounded copy of the complete relevant UEFI device path. */
typedef struct {
    UINT16 size;
    UINT16 reserved;
    UINT8 data[XHCI_HANDOFF_V4_MAX_PATH_BYTES];
} XHCI_HANDOFF_PATH;

typedef struct {
    UINT8 endpoint_address;
    UINT8 attributes;
    UINT8 interval;
    UINT8 reserved;
    UINT16 max_packet_size;
} XHCI_HANDOFF_ENDPOINT;

/* Immutable protocol-capability facts resolved by UEFI for one device port. */
typedef struct {
    UINT8 valid;
    UINT8 major_revision;
    UINT8 minor_revision;
    UINT8 slot_type;
    UINT8 port_offset;
    UINT8 port_count;
    UINT16 reserved;
} XHCI_HANDOFF_PROTOCOL;

/* Maximum useful immutable facts for one selected HID interface. */
typedef struct {
    UINT8 flags;
    UINT8 kind;
    UINT8 root_port;             /* xHCI one-based; UEFI converts exactly once. */
    UINT8 speed_evidence;
    UINT16 vendor_id;
    UINT16 product_id;
    UINT8 configuration_value;
    UINT8 interface_number;
    UINT8 interface_class;
    UINT8 interface_subclass;
    UINT8 interface_protocol;
    UINT8 interrupt_in_endpoint;
    UINT16 interrupt_max_packet_size;
    UINT8 interrupt_interval;
    UINT8 endpoint_count;
    UINT8 ep0_mps_descriptor;
    UINT8 reserved0[3];

    /* Exact BAR-relative offset resolved by UEFI for this device's root port. */
    UINT32 portsc_offset;

    /* Supported Protocol Capability covering this exact root port. */
    XHCI_HANDOFF_PROTOCOL protocol;

    XHCI_HANDOFF_ENDPOINT endpoints[XHCI_HANDOFF_V4_MAX_ENDPOINTS];
    XHCI_HANDOFF_PATH device_path;
} XHCI_HANDOFF_HID_DEVICE;

/* Immutable controller capability/access facts resolved by UEFI. */
typedef struct {
    UINT16 pci_segment;
    UINT8 pci_bus;
    UINT8 pci_device;
    UINT8 pci_function;
    UINT8 reserved0;
    UINT16 pci_vendor;
    UINT16 pci_device_id;

    UINT16 hciversion;
    UINT16 reserved1;
    UINT32 hcsparams1;
    UINT32 hcsparams2;
    UINT32 hccparams1;
    UINT32 pagesize;

    /* Exact BAR-relative register locations used by the bridge. */
    UINT32 operational_offset;
    UINT32 usbcmd_offset;
    UINT32 usbsts_offset;
    UINT32 crcr_offset;
    UINT32 dcbaap_offset;
    UINT32 config_offset;
    UINT32 doorbell0_offset;
    UINT32 runtime_offset;
    UINT32 interrupter0_offset;
    UINT32 iman_offset;
    UINT32 erstsz_offset;
    UINT32 erstba_offset;
    UINT32 erdp_offset;

    XHCI_HANDOFF_PATH controller_path;
} XHCI_HANDOFF_CONTROLLER;

typedef struct {
    UINT32 magic;
    UINT16 version;
    UINT16 size;
    UINT16 device_count;
    UINT16 reserved;

    XHCI_HANDOFF_CONTROLLER controller;
    XHCI_HANDOFF_HID_DEVICE keyboard;
    XHCI_HANDOFF_HID_DEVICE mouse;
} XHCI_HANDOFF_V4;

#endif
