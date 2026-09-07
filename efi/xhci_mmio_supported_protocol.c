#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define DELAY_SEC 5

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}

static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32,
                             0, (UINT64)off, 1, v);
}

static void done(void) {
    Print(u"\r\nREAD-ONLY / 6 MMIO READS / NO DMA\r\n");
    Print(u"EXIT %u SEC...\r\n", DELAY_SEC);
    uefi_call_wrapper(BS->Stall, 1, (UINTN)DELAY_SEC * 1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_STATUS s;
    UINTN n = 0, i, seg, bus, dev, fun;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT32 cap0 = 0, hcs1 = 0, hcc1 = 0;
    UINT32 ext0 = 0, ext1 = 0, ext2 = 0;
    UINT64 bar;
    UINT32 xecp, ext_off;
    UINT32 port_off, port_count;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V11 / SUPPORTED PROTOCOL
");

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) {
        Print(u"PCI ENUM FAIL %r
", s);
        done();
        return s;
    }

    for (i = 0; i < n; ++i) {
        EFI_PCI_IO_PROTOCOL *q = NULL;
        UINTN sg, b, d, f;

        s = uefi_call_wrapper(BS->OpenProtocol, 6, hs[i], &PciGuid,
                              (void **)&q, image, NULL,
                              EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s)) continue;
        if (EFI_ERROR(uefi_call_wrapper(q->GetLocation, 5, q, &sg, &b, &d, &f)))
            continue;
        if (EFI_ERROR(cfg32(q, 8, &cls))) continue;

        if (((cls >> 24) & 0xff) != 0x0c ||
            ((cls >> 16) & 0xff) != 0x03 ||
            ((cls >> 8) & 0xff) != 0x30)
            continue;

        p = q; seg = sg; bus = b; dev = d; fun = f;
        break;
    }

    if (!p) {
        Print(u"xHCI NOT FOUND
");
        if (hs) FreePool(hs);
        done();
        return EFI_NOT_FOUND;
    }

    cfg32(p, 0, &id);
    cfg32(p, 0x10, &bar0);
    cfg32(p, 0x14, &bar1);

    bar = ((UINT64)bar1 << 32) | ((UINT64)bar0 & ~0xFULL);

    Print(u"PCI %04x:%02x:%02x.%x %04x:%04x
",
          (UINT32)seg, (UINT32)bus, (UINT32)dev, (UINT32)fun,
          id & 0xffff, id >> 16);
    Print(u"BAR0=%08x:%08x BASE=%016lx
", bar1, bar0, bar);

    if ((bar0 & 1) != 0 || bar == 0) {
        Print(u"BAD MEMORY BAR
");
        if (hs) FreePool(hs);
        done();
        return EFI_DEVICE_ERROR;
    }

    s = mmio32(p, 0, &cap0);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[000]=FAIL %r
", s);
        if (hs) FreePool(hs);
        done();
        return s;
    }
    Print(u"MMIO[000]=%08x CAPLEN=%02x VER=%04x
",
          cap0, cap0 & 0xff, (cap0 >> 16) & 0xffff);

    s = mmio32(p, 4, &hcs1);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[004]=FAIL %r
", s);
        if (hs) FreePool(hs);
        done();
        return s;
    }
    Print(u"MMIO[004]=%08x SLOTS=%u INTR=%u PORTS=%u
",
          hcs1, hcs1 & 0xff, (hcs1 >> 8) & 0x7ff, (hcs1 >> 24) & 0xff);

    s = mmio32(p, 0x10, &hcc1);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[010]=FAIL %r
", s);
        if (hs) FreePool(hs);
        done();
        return s;
    }

    xecp = (hcc1 >> 16) & 0xffff;
    Print(u"MMIO[010]=%08x AC64=%u CSZ=%u XECP=%04x
",
          hcc1, hcc1 & 1, (hcc1 >> 2) & 1, xecp);

    if (xecp == 0) {
        Print(u"NO EXT CAPABILITIES
");
        if (hs) FreePool(hs);
        done();
        return EFI_SUCCESS;
    }

    ext_off = xecp << 2;

    s = mmio32(p, ext_off, &ext0);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[%04x]=FAIL %r
", ext_off, s);
        if (hs) FreePool(hs);
        done();
        return s;
    }

    Print(u"MMIO[%04x]=%08x ID=%02x NEXT=%02x VAL=%04x
",
          ext_off, ext0, ext0 & 0xff, (ext0 >> 8) & 0xff, ext0 >> 16);

    s = mmio32(p, ext_off + 4, &ext1);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[%04x]=FAIL %r
", ext_off + 4, s);
        if (hs) FreePool(hs);
        done();
        return s;
    }

    Print(u"MMIO[%04x]=%08x ASCII=%c%c%c%c
",
          ext_off + 4, ext1,
          (char)(ext1 & 0xff), (char)((ext1 >> 8) & 0xff),
          (char)((ext1 >> 16) & 0xff), (char)((ext1 >> 24) & 0xff));

    s = mmio32(p, ext_off + 8, &ext2);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[%04x]=FAIL %r
", ext_off + 8, s);
        if (hs) FreePool(hs);
        done();
        return s;
    }

    port_off = ext2 & 0xff;
    port_count = (ext2 >> 8) & 0xff;

    Print(u"MMIO[%04x]=%08x PORTOFF=%u PORTS=%u
",
          ext_off + 8, ext2, port_off, port_count);

    if (hs) FreePool(hs);
    done();
    return EFI_SUCCESS;
}
