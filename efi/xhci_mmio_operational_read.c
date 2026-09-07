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
    Print(u"\r\nREAD-ONLY / 3 MMIO READS / NO DMA\r\n");
    Print(u"EXIT %u SEC...\r\n", DELAY_SEC);
    uefi_call_wrapper(BS->Stall, 1, (UINTN)DELAY_SEC * 1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_STATUS s;
    UINTN n = 0, i;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT32 cap0 = 0, usbcmd = 0, usbsts = 0, portsc1 = 0;
    UINT32 caplen, op_base, port1_off;
    UINT64 bar;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V12 / OPERATIONAL READ\r\n");

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) {
        Print(u"PCI ENUM FAIL %r\r\n", s);
        done();
        return s;
    }

    for (i = 0; i < n; ++i) {
        EFI_PCI_IO_PROTOCOL *q = NULL;
        UINTN seg, bus, dev, fun;

        s = uefi_call_wrapper(BS->OpenProtocol, 6, hs[i], &PciGuid,
                              (void **)&q, image, NULL,
                              EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s))
            continue;

        if (EFI_ERROR(uefi_call_wrapper(q->GetLocation, 5, q,
                                         &seg, &bus, &dev, &fun)))
            continue;

        if (EFI_ERROR(cfg32(q, 8, &cls)))
            continue;

        if (((cls >> 24) & 0xff) != 0x0c ||
            ((cls >> 16) & 0xff) != 0x03 ||
            ((cls >> 8) & 0xff) != 0x30)
            continue;

        p = q;
        break;
    }

    if (!p) {
        Print(u"xHCI NOT FOUND\r\n");
        if (hs)
            FreePool(hs);
        done();
        return EFI_NOT_FOUND;
    }

    cfg32(p, 0, &id);
    cfg32(p, 0x10, &bar0);
    cfg32(p, 0x14, &bar1);

    bar = ((UINT64)bar1 << 32) | ((UINT64)bar0 & ~0xFULL);

    Print(u"PCI %04x:%02x:%02x.%x %04x:%04x\r\n",
          0u, 0u, 0u, 0u, id & 0xffff, id >> 16);
    Print(u"BAR0=%08x:%08x BASE=%016lx\r\n", bar1, bar0, bar);

    if ((bar0 & 1) != 0 || bar == 0) {
        Print(u"BAD MEMORY BAR\r\n");
        if (hs)
            FreePool(hs);
        done();
        return EFI_DEVICE_ERROR;
    }

    s = mmio32(p, 0, &cap0);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[000]=FAIL %r\r\n", s);
        if (hs)
            FreePool(hs);
        done();
        return s;
    }

    caplen = cap0 & 0xff;
    op_base = caplen;

    Print(u"MMIO[000]=%08x CAPLEN=%02x VER=%04x\r\n",
          cap0, caplen, (cap0 >> 16) & 0xffff);
    Print(u"OPBASE=%04x (BAR + CAPLEN)\r\n", op_base);

    s = mmio32(p, op_base + 0x00, &usbcmd);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[%03x]=FAIL %r\r\n", op_base, s);
        if (hs)
            FreePool(hs);
        done();
        return s;
    }
    Print(u"MMIO[%03x]=%08x USBCMD\r\n", op_base, usbcmd);

    s = mmio32(p, op_base + 0x04, &usbsts);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[%03x]=FAIL %r\r\n", op_base + 4, s);
        if (hs)
            FreePool(hs);
        done();
        return s;
    }
    Print(u"MMIO[%03x]=%08x USBSTS HALT=%u CNR=%u HCE=%u\r\n",
          op_base + 4, usbsts,
          usbsts & 1,
          (usbsts >> 11) & 1,
          (usbsts >> 12) & 1);

    port1_off = op_base + 0x400;

    s = mmio32(p, port1_off, &portsc1);
    if (EFI_ERROR(s)) {
        Print(u"MMIO[%03x]=FAIL %r\r\n", port1_off, s);
        if (hs)
            FreePool(hs);
        done();
        return s;
    }

    Print(u"MMIO[%03x]=%08x PORTSC1 CCS=%u PED=%u PP=%u PLS=%u\r\n",
          port1_off, portsc1,
          portsc1 & 1,
          (portsc1 >> 1) & 1,
          (portsc1 >> 9) & 1,
          (portsc1 >> 5) & 0xf);

    if (hs)
        FreePool(hs);
    done();
    return EFI_SUCCESS;
}
