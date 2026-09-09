#ifndef XHCI_BRIDGE_H
#define XHCI_BRIDGE_H

#include "xhci_platform_ops.h"

/**
 * Device template snapshot from UEFI - describes a known HID device
 * This is a constrained recipe, not authority to skip xHCI setup
 */
struct known_hid_device {
    uint8_t  root_port;
    uint8_t  speed;
    uint8_t  configuration_value;
    uint8_t  interface_number;
    uint8_t  interrupt_in_endpoint;
    uint16_t max_packet_size;
    uint8_t  interval;
    uint8_t  kind;  /* 1 = keyboard, 2 = mouse */
};

/**
 * Initialize the xHCI bridge
 * 
 * @param ops    Platform operations (MMIO, DMA, timing, logging)
 * @param ctx    Platform context (passed to ops callbacks)
 * @param hints  Array of known device hints (can be empty/null)
 * @param num_hints Number of hints in array
 * @return 0 on success, negative error code on failure
 * 
 * For V28 (first build): pass hints=NULL, num_hints=0
 * This performs: halt -> reset -> CNR clear -> DMA allocation ->
 * DCBAA/command/event rings -> readback -> safe teardown
 * Still issues no commands and touches no USB device.
 */
int xhci_bridge_init(const struct xhci_platform_ops *ops, 
                     void *ctx, 
                     const struct known_hid_device *hints, 
                     unsigned num_hints);

/**
 * Shut down the xHCI bridge - reverse of init
 * Safely frees all resources and returns controller to safe state
 */
void xhci_bridge_shutdown(const struct xhci_platform_ops *ops, void *ctx);

#endif // XHCI_BRIDGE_H