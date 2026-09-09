#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include "xhci_platform_ops.h"

/**
 * GNU-EFI platform operations implementation
 * Wraps EFI_PCI_IO_PROTOCOL for MMIO and EFI_BOOT_SERVICES for DMA
 */

struct efi_platform_ctx {
    EFI_PCI_IO_PROTOCOL *pci_io;
    EFI_HANDLE image;
    EFI_SYSTEM_TABLE *st;
};

static int efi_read32(void *ctx, uint32_t off, uint32_t *v) {
    struct efi_platform_ctx *c = (struct efi_platform_ctx *)ctx;
    return uefi_call_wrapper(c->pci_io->Mem.Read, 6, c->pci_io,
                             EfiPciIoWidthUint32, 0, (UINT64)off, 1, v);
}

static int efi_write32(void *ctx, uint32_t off, uint32_t v) {
    struct efi_platform_ctx *c = (struct efi_platform_ctx *)ctx;
    return uefi_call_wrapper(c->pci_io->Mem.Write, 6, c->pci_io,
                             EfiPciIoWidthUint32, 0, (UINT64)off, 1, &v);
}

static int efi_read64(void *ctx, uint32_t off, uint64_t *v) {
    struct efi_platform_ctx *c = (struct efi_platform_ctx *)ctx;
    return uefi_call_wrapper(c->pci_io->Mem.Read, 6, c->pci_io,
                             EfiPciIoWidthUint64, 0, (UINT64)off, 1, v);
}

static int efi_write64(void *ctx, uint32_t off, uint64_t v) {
    struct efi_platform_ctx *c = (struct efi_platform_ctx *)ctx;
    return uefi_call_wrapper(c->pci_io->Mem.Write, 6, c->pci_io,
                             EfiPciIoWidthUint64, 0, (UINT64)off, 1, &v);
}

static int efi_dma_alloc(void *ctx, size_t size, size_t align,
                         void **cpu_addr, uint64_t *dev_addr) {
    struct efi_platform_ctx *c = (struct efi_platform_ctx *)ctx;
    EFI_PHYSICAL_ADDRESS phys;
    EFI_STATUS s;
    
    (void)align; /* EFI pages are 4K aligned by default */
    
    /* Allocate pages - round up to page count */
    UINTN pages = (size + 4095) / 4096;
    if (pages == 0) pages = 1;
    
    s = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                          EfiBootServicesData, pages, &phys);
    if (EFI_ERROR(s)) return (int)s;
    
    *cpu_addr = (void *)(UINTN)phys;
    *dev_addr = (uint64_t)phys; /* Identity mapping for now */
    
    /* Zero the memory */
    uefi_call_wrapper(BS->SetMem, 3, *cpu_addr, pages * 4096, 0);
    
    return 0;
}

static void efi_dma_free(void *ctx, void *cpu_addr, size_t size) {
    (void)ctx;
    (void)size;
    /* EFI requires page count for FreePages */
    EFI_PHYSICAL_ADDRESS phys = (EFI_PHYSICAL_ADDRESS)(UINTN)cpu_addr;
    uefi_call_wrapper(BS->FreePages, 2, phys, 1);
}

static void efi_delay_us(void *ctx, uint32_t usec) {
    (void)ctx;
    /* EFI Stall takes microseconds */
    uefi_call_wrapper(BS->Stall, 1, usec);
}

static void efi_log(void *ctx, const char *msg) {
    (void)ctx;
    /* Convert to CHAR16 for Print */
    CHAR16 buf[256];
    unsigned i;
    for (i = 0; i < sizeof(buf)-1 && msg[i]; i++) {
        buf[i] = (CHAR16)msg[i];
    }
    buf[i] = 0;
    Print(buf);
}

const struct xhci_platform_ops *efi_platform_ops(void) {
    static const struct xhci_platform_ops ops = {
        .read32 = efi_read32,
        .write32 = efi_write32,
        .read64 = efi_read64,
        .write64 = efi_write64,
        .dma_alloc = efi_dma_alloc,
        .dma_free = efi_dma_free,
        .delay_us = efi_delay_us,
        .log = efi_log,
    };
    return &ops;
}

struct efi_platform_ctx *efi_platform_ctx_new(EFI_PCI_IO_PROTOCOL *pci_io,
                                                EFI_HANDLE image,
                                                EFI_SYSTEM_TABLE *st) {
    struct efi_platform_ctx *ctx = 
        (struct efi_platform_ctx *)AllocatePool(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->pci_io = pci_io;
    ctx->image = image;
    ctx->st = st;
    return ctx;
}

void efi_platform_ctx_free(struct efi_platform_ctx *ctx) {
    if (ctx) FreePool(ctx);
}