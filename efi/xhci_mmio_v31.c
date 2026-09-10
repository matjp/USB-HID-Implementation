#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define XHCI_MIN_VERSION 0x0100U
#define CMD_RUN 0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE 0x00000004U
#define CMD_HSEE 0x00000008U
#define STS_HCH 0x00000001U
#define STS_HSE 0x00000004U
#define STS_CNR 0x00000800U
#define IMAN_IE 0x00000002U
#define IMAN_IP 0x00000001U
#define CRCR_RCS 0x00000001ULL
#define CRCR_CRR 0x00000008U
#define TRB_CYCLE 0x00000001U
#define TRB_TYPE_MASK 0x0000fc00U
#define TRB_TYPE_SHIFT 10U
#define TRB_ENABLE_SLOT 9U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 0x00000002U
#define TRB_CCE 33U
#define CC_SUCCESS 1U
#define ERST_ADDR_MASK 0xffffffffffffffc0ULL
#define ERDP_ADDR_MASK 0xfffffffffffffff0ULL
#define MAX_SCRATCHPADS 1024U
#define CMD_RING_TRBS 256U
#define EVENT_RING_TRBS 16U
#define COMMON_TIMEOUT_MS 10000U

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static const CHAR16 *fail_stage = u"NONE";
static const CHAR16 *fail_op = u"NONE";
static EFI_STATUS fail_status = EFI_SUCCESS;

struct dma_obj {
    VOID *host;
    VOID *map;
    EFI_PHYSICAL_ADDRESS dev;
    UINTN pages;
    BOOLEAN live;
};

static void remember_failure(const CHAR16 *stage, const CHAR16 *op, EFI_STATUS s) {
    if (!EFI_ERROR(fail_status)) {
        fail_stage = stage;
        fail_op = op;
        fail_status = s;
    }
}

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}

static EFI_STATUS mmio16(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT16 *v) {
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint16, 0, (UINT64)off, 1, v);
}

static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32, 0, (UINT64)off, 1, v);
}

static EFI_STATUS mmio64_split(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 *v) {
    UINT32 lo, hi;
    EFI_STATUS s = mmio32(p, off, &lo);
    if (EFI_ERROR(s)) return s;
    s = mmio32(p, off + 4U, &hi);
    if (EFI_ERROR(s)) return s;
    *v = ((UINT64)hi << 32) | lo;
    return EFI_SUCCESS;
}

static EFI_STATUS mmio_write32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 v) {
    return uefi_call_wrapper(p->Mem.Write, 6, p, EfiPciIoWidthUint32, 0, (UINT64)off, 1, &v);
}

static EFI_STATUS mmio_write64_split(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 v) {
    EFI_STATUS s;
    UINT32 lo = (UINT32)v;
    UINT32 hi = (UINT32)(v >> 32);
    s = mmio_write32(p, off, lo);
    if (EFI_ERROR(s)) return s;
    return mmio_write32(p, off + 4U, hi);
}

static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL *p, UINTN pages, struct dma_obj *d) {
    EFI_STATUS s;
    UINTN bytes;
    if (!pages || pages > ((UINTN)-1) / 4096U) return EFI_BAD_BUFFER_SIZE;
    d->host = NULL;
    d->map = NULL;
    d->dev = 0;
    d->pages = pages;
    d->live = FALSE;
    bytes = pages * 4096U;
    s = uefi_call_wrapper(p->AllocateBuffer, 6, p, AllocateAnyPages,
                           EfiBootServicesData, pages, &d->host, 0);
    if (EFI_ERROR(s)) return s;
    s = uefi_call_wrapper(BS->SetMem, 3, d->host, bytes, 0);
    if (EFI_ERROR(s)) {
        uefi_call_wrapper(p->FreeBuffer, 3, p, pages, d->host);
        d->host = NULL;
        return s;
    }
    {
        UINTN map_bytes = bytes;
        s = uefi_call_wrapper(p->Map, 6, p,
                               EfiPciIoOperationBusMasterCommonBuffer,
                               d->host, &map_bytes, &d->dev, &d->map);
        if (EFI_ERROR(s) || map_bytes != bytes) {
            EFI_STATUS x = EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
            if (!EFI_ERROR(s) && d->map)
                uefi_call_wrapper(p->Unmap, 2, p, d->map);
            uefi_call_wrapper(p->FreeBuffer, 3, p, pages, d->host);
            d->host = NULL;
            d->map = NULL;
            d->dev = 0;
            return x;
        }
    }
    d->live = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL *p, struct dma_obj *d) {
    EFI_STATUS s = EFI_SUCCESS, t;
    if (!d->live) return EFI_SUCCESS;
    if (d->map) {
        t = uefi_call_wrapper(p->Unmap, 2, p, d->map);
        if (EFI_ERROR(t)) s = t;
    }
    if (d->host) {
        t = uefi_call_wrapper(p->FreeBuffer, 3, p, d->pages, d->host);
        if (EFI_ERROR(t) && !EFI_ERROR(s)) s = t;
    }
    d->host = NULL;
    d->map = NULL;
    d->dev = 0;
    d->live = FALSE;
    return s;
}

static EFI_STATUS wait_hch(EFI_PCI_IO_PROTOCOL *p, UINT32 opbase, BOOLEAN halted,
                           UINTN loops, UINT32 *status, UINT32 *reads) {
    UINTN i;
    EFI_STATUS s;
    UINT32 expected = halted ? STS_HCH : 0U;
    for (i = 0; i < loops; ++i) {
        s = mmio32(p, opbase + 4U, status);
        ++(*reads);
        if (EFI_ERROR(s)) return s;
        if ((*status & STS_HCH) == expected) return EFI_SUCCESS;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS wait_cnr_clear(EFI_PCI_IO_PROTOCOL *p, UINT32 opbase,
                                 UINTN loops, UINT32 *status, UINT32 *reads) {
    UINTN i;
    EFI_STATUS s;
    for (i = 0; i < loops; ++i) {
        s = mmio32(p, opbase + 4U, status);
        ++(*reads);
        if (EFI_ERROR(s)) return s;
        if (!(*status & STS_CNR)) return EFI_SUCCESS;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS reset_controller(EFI_PCI_IO_PROTOCOL *p, UINT32 opbase,
                                   UINT32 *cmd, UINT32 *status,
                                   UINT32 *reads, UINT32 *writes) {
    EFI_STATUS s;
    UINTN i;
    s = mmio32(p, opbase, cmd);
    ++(*reads);
    if (EFI_ERROR(s)) return s;
    *cmd &= ~(CMD_RUN | CMD_INTE | CMD_HSEE);
    *cmd |= CMD_RESET;
    s = mmio_write32(p, opbase, *cmd);
    ++(*writes);
    if (EFI_ERROR(s)) return s;
    for (i = 0; i < 1000U; ++i) {
        s = mmio32(p, opbase, cmd);
        ++(*reads);
        if (EFI_ERROR(s)) return s;
        if (!(*cmd & CMD_RESET)) break;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    if (*cmd & CMD_RESET) return EFI_TIMEOUT;
    return wait_cnr_clear(p, opbase, COMMON_TIMEOUT_MS, status, reads);
}

static UINT32 extcap_next(UINT32 v) {
    return ((v >> 8) & 0xffU) * 4U;
}

static EFI_STATUS find_slot_type(EFI_PCI_IO_PROTOCOL *p, UINT32 hcc1,
                                 UINT32 *slot_type, UINT32 *reads) {
    UINT32 off = ((hcc1 >> 16) & 0xffffU) * 4U;
    UINT32 seen = 0;
    while (off && seen++ < 64U) {
        UINT32 h, d1, d2, d3;
        EFI_STATUS s = mmio32(p, off, &h);
        ++(*reads);
        if (EFI_ERROR(s)) return s;
        if ((h & 0xffU) == 2U) {
            s = mmio32(p, off + 12U, &d3);
            ++(*reads);
            if (EFI_ERROR(s)) return s;
            *slot_type = d3 & 0x1fU;
            return EFI_SUCCESS;
        }
        d1 = d2 = 0;
        (void)d1;
        (void)d2;
        off += extcap_next(h);
    }
    *slot_type = 0;
    return EFI_SUCCESS;
}

static void fatal_dma_running(void) {
    Print(u"\r\nFATAL: XHCI NOT CONFIRMED HALTED\r\n");
    Print(u"DMA MAPPINGS RETAINED / NO FREE / MANUAL RECOVERY REQUIRED\r\n");
    for (;;) uefi_call_wrapper(BS->Stall, 1, 1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_STATUS s = EFI_SUCCESS, ts;
    UINTN n = 0, i;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT32 cap0 = 0, hcs1 = 0, hcs2 = 0, hcc1 = 0;
    UINT32 opbase = 0, dbbase = 0, rtbase = 0, irbase = 0;
    UINT32 cmd = 0, status = 0, config = 0, iman = 0;
    UINT32 maxslots = 0, scratchpads = 0, pagesize_reg = 0;
    UINTN page_shift = 0, xhci_pagesize = 0, scratch_pages = 0;
    UINT32 slot_type = 0, ext_reads = 0;
    UINT32 reads = 0, writes = 0;
    UINT32 event_dw0 = 0, event_dw1 = 0, event_dw2 = 0, event_dw3 = 0;
    UINT32 event_type = 0, completion = 0, slot_id = 0;
    UINT64 event_cmd_ptr = 0, erstba_rd = 0, erdp_rd = 0, dcbaa_rd = 0;
    UINT32 db_value = 0;
    BOOLEAN controller_halted = FALSE;
    BOOLEAN dma_live = FALSE;
    BOOLEAN command_submitted = FALSE;
    BOOLEAN event_consumed = FALSE;
    BOOLEAN reset_done = FALSE;
    UINT64 *dcbaa = NULL, *scratch_array = NULL, *cmd_ring = NULL;
    UINT64 *erst = NULL, *event_ring = NULL;
    struct dma_obj dcbaa_dma = {0}, scratch_array_dma = {0}, cmd_dma = {0};
    struct dma_obj erst_dma = {0}, event_dma = {0};
    struct dma_obj *scratch_dma = NULL;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V31 / ENABLE SLOT COMMAND + COMPLETION\r\n");
    Print(u"UEFI DMA MAP -> INIT -> RUN -> ENABLE SLOT -> POLL EVENT -> RESET\r\n");
    Print(u"ONE COMMAND / ONE DOORBELL / NO CPU INTERRUPTS / NO USB TRANSFER\r\n");

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) { remember_failure(u"PCI", u"LOCATE PCI IO", s); goto out; }
    for (i = 0; i < n; ++i) {
        EFI_PCI_IO_PROTOCOL *q = NULL;
        if (EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol, 6, hs[i], &PciGuid,
                                         (void **)&q, image, NULL,
                                         EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
        if (EFI_ERROR(cfg32(q, 8, &cls)) || EFI_ERROR(cfg32(q, 0, &id))) continue;
        if (((cls >> 24) & 0xffU) == 0x0cU && ((cls >> 16) & 0xffU) == 0x03U &&
            ((cls >> 8) & 0xffU) == 0x30U) { p = q; break; }
    }
    if (!p) { s = EFI_NOT_FOUND; remember_failure(u"PCI", u"FIND XHCI", s); goto out; }

    s = cfg32(p, 0x10, &bar0); if (EFI_ERROR(s)) { remember_failure(u"PCI", u"READ BAR0", s); goto out; }
    if ((bar0 & 1U) || ((bar0 >> 1) & 3U) != 2U) { s = EFI_UNSUPPORTED; remember_failure(u"PCI", u"REQUIRE 64-BIT BAR", s); goto out; }
    s = cfg32(p, 0x14, &bar1); if (EFI_ERROR(s)) { remember_failure(u"PCI", u"READ BAR1", s); goto out; }
    s = mmio32(p, 0, &cap0); ++reads; if (EFI_ERROR(s)) { remember_failure(u"CAPS", u"READ CAPLENGTH", s); goto out; }
    opbase = cap0 & 0xffU;
    { UINT16 ver = 0;
      s = mmio16(p, 2, &ver); ++reads;
      if (EFI_ERROR(s)) { remember_failure(u"CAPS", u"READ HCIVERSION", s); goto out; }
      if (ver < XHCI_MIN_VERSION) { s = EFI_UNSUPPORTED; remember_failure(u"CAPS", u"VALIDATE HCIVERSION", s); goto out; }
      Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x BAR=%08x/%08x OPBASE=%02x\r\n",
            ver >> 8, ver & 255U, id & 0xffffU, id >> 16, bar0, bar1, opbase);
    }
    s = mmio32(p, 4, &hcs1); ++reads; if (EFI_ERROR(s)) { remember_failure(u"CAPS", u"READ HCSPARAMS1", s); goto out; }
    s = mmio32(p, 8, &hcs2); ++reads; if (EFI_ERROR(s)) { remember_failure(u"CAPS", u"READ HCSPARAMS2", s); goto out; }
    s = mmio32(p, 0x10, &hcc1); ++reads; if (EFI_ERROR(s)) { remember_failure(u"CAPS", u"READ HCCPARAMS1", s); goto out; }
    s = mmio32(p, opbase + 4U, &status); ++reads; if (EFI_ERROR(s)) { remember_failure(u"CAPS", u"READ USBSTS", s); goto out; }
    maxslots = hcs1 & 0xffU;
    scratchpads = (((hcs2 >> 21) & 0x1fU) << 5) | ((hcs2 >> 27) & 0x1fU);
    s = mmio32(p, opbase + 8U, &pagesize_reg); ++reads;
    if (EFI_ERROR(s) || !pagesize_reg) { s = EFI_UNSUPPORTED; remember_failure(u"CAPS", u"READ PAGESIZE", s); goto out; }
    while (page_shift < 32U && !(pagesize_reg & (1U << page_shift))) ++page_shift;
    if (page_shift >= 32U) { s = EFI_UNSUPPORTED; remember_failure(u"CAPS", u"FIND PAGESIZE", s); goto out; }
    xhci_pagesize = (UINTN)1U << (12U + page_shift);
    if (xhci_pagesize < 4096U || xhci_pagesize > (1U << 20)) { s = EFI_UNSUPPORTED; remember_failure(u"CAPS", u"VALIDATE PAGESIZE", s); goto out; }
    scratch_pages = xhci_pagesize / 4096U;
    Print(u"CAPS: SLOTS=%u SCRATCHPADS=%u PAGESIZE=%u HCH=%u CNR=%u\r\n",
          maxslots, scratchpads, (UINT32)xhci_pagesize, status & STS_HCH ? 1 : 0, status & STS_CNR ? 1 : 0);
    if (!maxslots || scratchpads > MAX_SCRATCHPADS) { s = EFI_UNSUPPORTED; remember_failure(u"CAPS", u"VALIDATE CAPS", s); goto out; }

    s = mmio32(p, 0x14, &dbbase); ++reads; if (EFI_ERROR(s)) { remember_failure(u"CAPS", u"READ DBOFF", s); goto out; }
    dbbase &= 0xfffffffcU;
    s = mmio32(p, 0x18, &rtbase); ++reads; if (EFI_ERROR(s)) { remember_failure(u"CAPS", u"READ RTSOFF", s); goto out; }
    rtbase &= 0xffffffe0U;
    irbase = rtbase + 0x20U;
    s = find_slot_type(p, hcc1, &slot_type, &ext_reads);
    reads += ext_reads;
    if (EFI_ERROR(s)) { remember_failure(u"CAPS", u"FIND PROTOCOL SLOT TYPE", s); goto out; }
    Print(u"CAPS: DBOFF=%08x RTSOFF=%08x SLOT-TYPE=%u\r\n", dbbase, rtbase, slot_type);

    s = mmio32(p, opbase, &cmd); ++reads; if (EFI_ERROR(s)) { remember_failure(u"HALT", u"READ USBCMD", s); goto out; }
    if (!(status & STS_HCH)) {
        cmd &= ~(CMD_RUN | CMD_INTE | CMD_HSEE);
        s = mmio_write32(p, opbase, cmd); ++writes;
        if (EFI_ERROR(s)) { remember_failure(u"HALT", u"CLEAR RUN", s); goto out; }
        s = wait_hch(p, opbase, TRUE, 1000U, &status, &reads);
        if (EFI_ERROR(s)) { remember_failure(u"HALT", u"WAIT HCH", s); goto out; }
    }
    controller_halted = TRUE;

    s = reset_controller(p, opbase, &cmd, &status, &reads, &writes);
    if (EFI_ERROR(s)) { remember_failure(u"RESET", u"RESET/CNR", s); goto out; }
    reset_done = TRUE;
    controller_halted = TRUE;

    s = dma_alloc(p, 1, &dcbaa_dma); if (EFI_ERROR(s)) { remember_failure(u"DMA", u"ALLOC DCBAA", s); goto out; }
    s = dma_alloc(p, scratchpads ? 1 : 1, &scratch_array_dma); if (EFI_ERROR(s)) { remember_failure(u"DMA", u"ALLOC SCRATCHPAD ARRAY", s); goto out; }
    s = dma_alloc(p, 1, &cmd_dma); if (EFI_ERROR(s)) { remember_failure(u"DMA", u"ALLOC COMMAND RING", s); goto out; }
    s = dma_alloc(p, 1, &event_dma); if (EFI_ERROR(s)) { remember_failure(u"DMA", u"ALLOC EVENT RING", s); goto out; }
    s = dma_alloc(p, 1, &erst_dma); if (EFI_ERROR(s)) { remember_failure(u"DMA", u"ALLOC ERST", s); goto out; }
    dma_live = TRUE;

    scratch_dma = NULL;
    if (scratchpads) {
        s = uefi_call_wrapper(BS->AllocatePool, 3, EfiBootServicesData,
                              scratchpads * sizeof(struct dma_obj), (void **)&scratch_dma);
        if (EFI_ERROR(s)) { remember_failure(u"DMA", u"ALLOC SCRATCH DESCRIPTORS", s); goto out; }
        uefi_call_wrapper(BS->SetMem, 3, scratch_dma, scratchpads * sizeof(struct dma_obj), 0);
        for (i = 0; i < scratchpads; ++i) {
            s = dma_alloc(p, scratch_pages, &scratch_dma[i]);
            if (EFI_ERROR(s)) { remember_failure(u"DMA", u"ALLOC SCRATCHPAD", s); goto out; }
            if (scratch_dma[i].dev & ((UINT64)xhci_pagesize - 1ULL)) {
                s = EFI_BAD_BUFFER_SIZE; remember_failure(u"DMA", u"VALIDATE SCRATCHPAD ALIGN", s); goto out;
            }
        }
    }

    dcbaa = (UINT64 *)dcbaa_dma.host;
    scratch_array = (UINT64 *)scratch_array_dma.host;
    cmd_ring = (UINT64 *)cmd_dma.host;
    event_ring = (UINT64 *)event_dma.host;
    erst = (UINT64 *)erst_dma.host;
    dcbaa[0] = scratch_array_dma.dev;
    for (i = 0; i < scratchpads; ++i) scratch_array[i] = scratch_dma[i].dev;

    cmd_ring[0] = 0;
    cmd_ring[1] = 0;
    cmd_ring[2] = 0;
    cmd_ring[3] = TRB_CYCLE | (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT) | ((slot_type & 0x1fU) << 16);
    cmd_ring[(CMD_RING_TRBS - 1U) * 2U] = cmd_dma.dev;
    cmd_ring[(CMD_RING_TRBS - 1U) * 2U + 1U] = 0;
    cmd_ring[(CMD_RING_TRBS - 1U) * 2U + 2U] = 0;
    cmd_ring[(CMD_RING_TRBS - 1U) * 2U + 3U] = TRB_CYCLE | TRB_LINK_TOGGLE | (TRB_LINK << TRB_TYPE_SHIFT);

    erst[0] = event_dma.dev;
    erst[1] = 0;
    erst[2] = EVENT_RING_TRBS;
    erst[3] = 0;
    uefi_call_wrapper(BS->SetMem, 3, event_dma.host, 4096U, 0);

    s = mmio_write64_split(p, opbase + 0x30U, dcbaa_dma.dev); writes += 2;
    if (EFI_ERROR(s)) { remember_failure(u"INIT", u"WRITE DCBAAP", s); goto out; }
    s = mmio_write32(p, opbase + 0x38U, 1U); ++writes;
    if (EFI_ERROR(s)) { remember_failure(u"INIT", u"WRITE CONFIG", s); goto out; }
    s = mmio_write64_split(p, opbase + 0x18U, cmd_dma.dev | CRCR_RCS); writes += 2;
    if (EFI_ERROR(s)) { remember_failure(u"INIT", u"WRITE CRCR", s); goto out; }
    s = mmio_write32(p, irbase + 0x08U, 1U); ++writes;
    if (EFI_ERROR(s)) { remember_failure(u"INIT", u"WRITE ERSTSZ", s); goto out; }
    s = mmio_write64_split(p, irbase + 0x10U, erst_dma.dev & ERST_ADDR_MASK); writes += 2;
    if (EFI_ERROR(s)) { remember_failure(u"INIT", u"WRITE ERSTBA", s); goto out; }
    s = mmio_write64_split(p, irbase + 0x18U, event_dma.dev & ERDP_ADDR_MASK); writes += 2;
    if (EFI_ERROR(s)) { remember_failure(u"INIT", u"WRITE ERDP", s); goto out; }
    s = mmio32(p, opbase + 0x30U, (UINT32 *)&dcbaa_rd); ++reads;
    if (EFI_ERROR(s)) { remember_failure(u"INIT", u"READ DCBAAP", s); goto out; }
    s = mmio64_split(p, irbase + 0x10U, &erstba_rd); reads += 2;
    if (EFI_ERROR(s) || (erstba_rd & ERST_ADDR_MASK) != (erst_dma.dev & ERST_ADDR_MASK)) { s = EFI_DEVICE_ERROR; remember_failure(u"INIT", u"VERIFY ERSTBA", s); goto out; }
    s = mmio64_split(p, irbase + 0x18U, &erdp_rd); reads += 2;
    if (EFI_ERROR(s) || (erdp_rd & ERDP_ADDR_MASK) != (event_dma.dev & ERDP_ADDR_MASK)) { s = EFI_DEVICE_ERROR; remember_failure(u"INIT", u"VERIFY ERDP", s); goto out; }

    s = mmio32(p, irbase, &iman); ++reads; if (EFI_ERROR(s)) { remember_failure(u"RUN", u"READ IMAN", s); goto out; }
    s = mmio_write32(p, irbase, (iman & ~IMAN_IE) | (iman & IMAN_IP)); ++writes;
    if (EFI_ERROR(s)) { remember_failure(u"RUN", u"DISABLE INTERRUPT", s); goto out; }
    s = mmio32(p, opbase, &cmd); ++reads; if (EFI_ERROR(s)) { remember_failure(u"RUN", u"READ USBCMD", s); goto out; }
    cmd &= ~(CMD_INTE | CMD_HSEE);
    s = mmio_write32(p, opbase, cmd | CMD_RUN); ++writes;
    if (EFI_ERROR(s)) { remember_failure(u"RUN", u"SET RUN", s); goto out; }
    controller_halted = FALSE;
    s = wait_hch(p, opbase, FALSE, 1000U, &status, &reads);
    if (EFI_ERROR(s)) { remember_failure(u"RUN", u"WAIT HCH CLEAR", s); goto out; }

    command_submitted = TRUE;
    db_value = 0;
    s = mmio_write32(p, dbbase, db_value); ++writes;
    if (EFI_ERROR(s)) { remember_failure(u"COMMAND", u"RING HOST COMMAND DOORBELL", s); goto out; }
    Print(u"ENABLE SLOT: DOORBELL=0 COMMAND-TRB=%016lx SLOT-TYPE=%u\r\n", cmd_dma.dev, slot_type);

    for (i = 0; i < 5000U; ++i) {
        s = mmio32(p, opbase + 4U, &status); ++reads;
        if (EFI_ERROR(s)) { remember_failure(u"EVENT", u"READ USBSTS", s); goto out; }
        if (status & STS_HSE) { s = EFI_DEVICE_ERROR; remember_failure(u"EVENT", u"HOST CONTROLLER ERROR", s); goto out; }
        s = mmio32(p, 0x20U + rtbase, &iman); ++reads;
        if (EFI_ERROR(s)) { remember_failure(u"EVENT", u"READ IMAN", s); goto out; }
        if (iman & IMAN_IP) {
            s = mmio32(p, 0x00U + 0x20U + rtbase, &event_dw0); ++reads; if (EFI_ERROR(s)) { remember_failure(u"EVENT", u"READ EVENT DW0", s); goto out; }
            s = mmio32(p, 0x04U + 0x20U + rtbase, &event_dw1); ++reads; if (EFI_ERROR(s)) { remember_failure(u"EVENT", u"READ EVENT DW1", s); goto out; }
            s = mmio32(p, 0x08U + 0x20U + rtbase, &event_dw2); ++reads; if (EFI_ERROR(s)) { remember_failure(u"EVENT", u"READ EVENT DW2", s); goto out; }
            s = mmio32(p, 0x0cU + 0x20U + rtbase, &event_dw3); ++reads; if (EFI_ERROR(s)) { remember_failure(u"EVENT", u"READ EVENT DW3", s); goto out; }
            event_type = (event_dw3 & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
            completion = (event_dw2 >> 24) & 0xffU;
            slot_id = (event_dw3 >> 24) & 0xffU;
            event_cmd_ptr = ((UINT64)event_dw1 << 32) | event_dw0;
            if ((event_dw3 & TRB_CYCLE) && event_type == TRB_CCE) break;
        }
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    if (!(event_dw3 & TRB_CYCLE) || event_type != TRB_CCE) {
        s = EFI_TIMEOUT; remember_failure(u"EVENT", u"WAIT COMMAND COMPLETION", s); goto out;
    }
    if (event_cmd_ptr != cmd_dma.dev || completion != CC_SUCCESS || slot_id == 0 || slot_id > maxslots) {
        s = EFI_DEVICE_ERROR; remember_failure(u"EVENT", u"VALIDATE COMMAND COMPLETION", s); goto out;
    }
    event_consumed = TRUE;
    s = mmio_write64_split(p, irbase + 0x18U, (event_dma.dev + 16U) & ERDP_ADDR_MASK); writes += 2;
    if (EFI_ERROR(s)) { remember_failure(u"EVENT", u"ADVANCE ERDP", s); goto out; }
    s = mmio32(p, irbase, &iman); ++reads; if (EFI_ERROR(s)) { remember_failure(u"EVENT", u"READ IMAN AFTER EVENT", s); goto out; }
    if (iman & IMAN_IP) {
        s = mmio_write32(p, irbase, IMAN_IP); ++writes;
        if (EFI_ERROR(s)) { remember_failure(u"EVENT", u"ACK IMAN IP", s); goto out; }
    }
    Print(u"COMMAND COMPLETION: TYPE=%u CODE=%u SLOT=%u PTR=%016lx PASS\r\n", event_type, completion, slot_id, event_cmd_ptr);

    s = mmio32(p, opbase, &cmd); ++reads; if (EFI_ERROR(s)) { remember_failure(u"HALT", u"READ USBCMD", s); goto out; }
    s = mmio_write32(p, opbase, cmd & ~(CMD_RUN | CMD_INTE | CMD_HSEE)); ++writes;
    if (EFI_ERROR(s)) { remember_failure(u"HALT", u"CLEAR RUN", s); goto out; }
    s = wait_hch(p, opbase, TRUE, 10000U, &status, &reads);
    if (EFI_ERROR(s)) { remember_failure(u"HALT", u"CONFIRM HCH", s); fatal_dma_running(); }
    controller_halted = TRUE;
    s = reset_controller(p, opbase, &cmd, &status, &reads, &writes);
    if (EFI_ERROR(s)) { remember_failure(u"RESET", u"RECOVERY RESET", s); goto out; }
    reset_done = TRUE;
    if (status & STS_HCH) controller_halted = TRUE;
    else { controller_halted = FALSE; s = EFI_DEVICE_ERROR; remember_failure(u"RESET", u"VERIFY HCH", s); goto out; }

    s = mmio_write64_split(p, opbase + 0x18U, 0); writes += 2;
    if (EFI_ERROR(s)) { remember_failure(u"TEARDOWN", u"CLEAR CRCR", s); goto out; }
    s = mmio_write64_split(p, opbase + 0x30U, 0); writes += 2;
    if (EFI_ERROR(s)) { remember_failure(u"TEARDOWN", u"CLEAR DCBAAP", s); goto out; }
    s = mmio_write32(p, opbase + 0x38U, 0); ++writes;
    if (EFI_ERROR(s)) { remember_failure(u"TEARDOWN", u"CLEAR CONFIG", s); goto out; }
    s = mmio_write32(p, irbase + 0x08U, 0); ++writes;
    if (EFI_ERROR(s)) { remember_failure(u"TEARDOWN", u"CLEAR ERSTSZ", s); goto out; }
    s = mmio_write64_split(p, irbase + 0x10U, 0); writes += 2;
    if (EFI_ERROR(s)) { remember_failure(u"TEARDOWN", u"CLEAR ERSTBA", s); goto out; }
    s = mmio_write64_split(p, irbase + 0x18U, 0); writes += 2;
    if (EFI_ERROR(s)) { remember_failure(u"TEARDOWN", u"CLEAR ERDP", s); goto out; }

out:
    if (!EFI_ERROR(s) && !controller_halted && command_submitted) fatal_dma_running();
    if (dma_live && controller_halted) {
        if (scratch_dma) for (i = 0; i < scratchpads; ++i) dma_free(p, &scratch_dma[i]);
        if (scratch_dma) uefi_call_wrapper(BS->FreePool, 1, scratch_dma);
        ts = dma_free(p, &erst_dma); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &event_dma); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &cmd_dma); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &scratch_array_dma); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &dcbaa_dma); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        dma_live = FALSE;
    } else if (scratch_dma && !controller_halted) {
        fatal_dma_running();
    }
    if (hs) FreePool(hs);
    if (EFI_ERROR(s)) {
        Print(u"\r\nV31 ENABLE SLOT: FAIL RESULT=%r\r\n", s);
        Print(u"FAIL STAGE=%s OP=%s STATUS=%r\r\n", fail_stage, fail_op, fail_status);
    } else {
        Print(u"\r\nV31 ENABLE SLOT: PASS SLOT=%u\r\n", slot_id);
        Print(u"COMMANDS=1 DOORBELLS=1 CPU-INTERRUPTS=0 EVENTS=%u\r\n", event_consumed ? 1U : 0U);
        Print(u"RESET RECOVERY PASS / ALL CONTROLLER POINTERS CLEARED BEFORE DMA RELEASE\r\n");
    }
    Print(u"MMIO READS=%u WRITES=%u RESULT=%r\r\n", reads, writes, s);
    Print(u"EXIT 5 SEC...\r\n");
    uefi_call_wrapper(BS->Stall, 1, 5000000);
    return s;
}
