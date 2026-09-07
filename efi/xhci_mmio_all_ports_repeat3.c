#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define DELAY_SEC 5
#define PORTSC_BASE 0x400
#define PORTSC_STRIDE 0x10
#define PASSES 3

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v)
{
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}

static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v)
{
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32,
                             0, (UINT64)off, 1, v);
}

static void done(UINT32 reads)
{
    Print(u"\r\nREAD-ONLY / %u PORTSC MMIO READS / %u PASSES / NO DMA / NO WRITES\r\n",
          reads, PASSES);
    Print(u"EXIT %u SEC...\r\n", DELAY_SEC);
    uefi_call_wrapper(BS->Stall, 1, (UINTN)DELAY_SEC * 1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_HANDLE *hs = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_STATUS s;
    UINTN n = 0, i;
    UINTN seg = 0, bus = 0, dev = 0, fun = 0;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT32 cap0 = 0, hcsparams1 = 0;
    UINT32 max_ports, caplen, op_base, port_off;
    UINT64 bar;
    UINT32 reads = 0;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V16 / ALL PORTSC 3X REPEAT\r\n");

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) {
        Print(u"PCI ENUM FAIL %r\r\n", s);
        done(reads);
        return s;
    }

    for (i = 0; i < n; ++i) {
        EFI_PCI_IO_PROTOCOL *q = NULL;
        UINT32 qcls = 0;
        UINTN sg = 0, b = 0, d = 0, f = 0;

        s = uefi_call_wrapper(BS->OpenProtocol, 6, hs[i], &PciGuid,
                              (void **)&q, image, NULL,
                              EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s))
            continue;

        if (EFI_ERROR(uefi_call_wrapper(q->GetLocation, 5, q,
                                         &sg, &b, &d, &f)))
            continue;

        if (EFI_ERROR(cfg32(q, 8, &qcls)))
            continue;

        if (((qcls >> 24) & 0xff) != 0x0c ||
            ((qcls >> 16) & 0xff) != 0x03 ||
            ((qcls >> 8) & 0xff) != 0x30)
            continue;

        p = q;
        seg = sg;
        bus = b;
        dev = d;
        fun = f;
        break;
    }

    if (!p) {
        Print(u"xHCI NOT FOUND\r\n");
        if (hs)
            FreePool(hs);
        done(reads);
        return EFI_NOT_FOUND;
    }

    cfg32(p, 0, &id);
    cfg32(p, 8, &cls);
    cfg32(p, 0x10, &bar0);
    cfg32(p, 0x14, &bar1);

    bar = ((UINT64)bar1 << 32) | ((UINT64)bar0 & ~0xFULL);

    Print(u"PCI %04x:%02x:%02x.%x %04x:%04x\r\n",
          (UINT32)seg, (UINT32)bus, (UINT32)dev, (UINT32)fun,
          id & 0xffff, id >> 16);
    Print(u"BAR0=%08x:%08x BASE=%016lx\r\n", bar1, bar0, bar);

    if ((bar0 & 1) != 0 || bar == 0) {
        Print(u"BAD MEMORY BAR\r\n");
        if (hs)
            FreePool(hs);
        done(reads);
        return EFI_DEVICE_ERROR;
    }

    s = mmio32(p, 0, &cap0);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[000]=FAIL %r\r\n", s);
        if (hs)
            FreePool(hs);
        done(reads);
        return s;
    }

    caplen = cap0 & 0xff;
    op_base = caplen;

    Print(u"MMIO[000]=%08x CAPLEN=%02x VER=%04x\r\n",
          cap0, caplen, (cap0 >> 16) & 0xffff);

    s = mmio32(p, 4, &hcsparams1);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[004]=FAIL %r\r\n", s);
        if (hs)
            FreePool(hs);
        done(reads);
        return s;
    }

    max_ports = (hcsparams1 >> 24) & 0xff;
    Print(u"MMIO[004]=%08x MAXPORTS=%u\r\n", hcsparams1, max_ports);

    for (UINT32 pass = 1; pass <= PASSES; ++pass) {
        Print(u"PASS %u\r\n", pass);

        for (UINT32 port = 1; port <= max_ports; ++port) {
            UINT32 portsc = 0;

            port_off = op_base + PORTSC_BASE + ((port - 1) * PORTSC_STRIDE);
            s = mmio32(p, port_off, &portsc);
            if (EFI_ERROR(s)) {
                Print(u"PORT%u MMIO[%03x]=FAIL %r\r\n",
                      port, port_off, s);
                if (hs)
                    FreePool(hs);
                done(reads);
                return s;
            }

            ++reads;
            Print(u"PORT%02u MMIO[%03x]=%08x CCS=%u PED=%u PP=%u PLS=%u\r\n",
                  port, port_off, portsc,
                  portsc & 1,
                  (portsc >> 1) & 1,
                  (portsc >> 9) & 1,
                  (portsc >> 5) & 0xf);
        }
    }

    if (hs)
        FreePool(hs);

    done(reads);
    return EFI_SUCCESS;
}
