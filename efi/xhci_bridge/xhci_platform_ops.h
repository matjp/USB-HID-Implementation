#ifndef XHCI_PLATFORM_OPS_H
#define XHCI_PLATFORM_OPS_H

#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

/**
 * Platform operations for xHCI bridge - abstracts hardware access
 * This allows the same bridge core to run on GNU-EFI, UEFI bridge, or kernel
 */
struct xhci_platform_ops {
    // PCI I/O operations
    int  (*read32)(void *ctx, uint32_t offset, uint32_t *value);
    int  (*write32)(void *ctx, uint32_t offset, uint32_t value);
    int  (*read64)(void *ctx, uint32_t offset, uint64_t *value);
    int  (*write64)(void *ctx, uint32_t offset, uint64_t value);

    // DMA allocation/free - returns CPU and device addresses
    int  (*dma_alloc)(void *ctx, size_t size, size_t align,
                      void **cpu_addr, uint64_t *device_addr);
    void (*dma_free)(void *ctx, void *cpu_addr, size_t size);

    // Timing and logging
    void (*delay_us)(void *ctx, uint32_t usec);
    void (*log)(void *ctx, const char *message);
};

#endif // XHCI_PLATFORM_OPS_H