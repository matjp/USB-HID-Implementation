#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include "xhci_platform_ops.h"

#define CMD_RUN   0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE  0x00000004U
#define STS_HCH   0x00000001U
#define STS_CNR   0x00000800U
#define CAPLENGTH_MASK 0x000000FFU
#define HCS1_MAXSLOTS_MASK 0x000000FFU
#define CONFIG_MAXSLOTS_MASK 0x000000FFU
#define CRCR_RCS 0x00000001ULL
#define CRCR_ADDR_MASK 0xFFFFFFFFFFFFFFC0ULL
#define ERST_ADDR_MASK 0xFFFFFFFFFFFFFFC0ULL
#define ERDP_ADDR_MASK 0xFFFFFFFFFFFFFFF0ULL
#define ERST_SEGMENT_TRBS 16U

struct xhci_bridge_state {
    const struct xhci_platform_ops *ops;
    void *ctx;
    uint32_t vendor_id, device_id;
    uint64_t bar;
    uint32_t caplength, opbase, rtsoff, max_slots, max_ports, hcs1;
    void *dcbaa_cpu; uint64_t dcbaa_dev;
    void *cr_cpu; uint64_t cr_dev;
    void *event_cpu; uint64_t event_dev;
    void *erst_cpu; uint64_t erst_dev;
    uint64_t dcbaap, crcr, erstba, erdp;
    int initialized;
};

static int pci_discover(const struct xhci_platform_ops *ops, void *ctx,
                         struct xhci_bridge_state *s) {
    (void)ops; (void)ctx; (void)s;
    return -1;
}

int xhci_bridge_init(const struct xhci_platform_ops *ops, void *ctx,
                     const struct known_hid_device *hints,
                     unsigned num_hints) {
    (void)hints; (void)num_hints;
    if (!ops || !ctx) return -1;
    return -1;
}

void xhci_bridge_shutdown(const struct xhci_platform_ops *ops, void *ctx) {
    (void)ops; (void)ctx;
}