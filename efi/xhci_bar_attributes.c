#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define DELAY_SEC 30

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}

static void done(void) {
    Print(u"\r\nREAD-ONLY / PCI CONFIG+BAR ATTR / NO MMIO\r\n");
    Print(u"EXIT %u SEC...\r\n", DELAY_SEC);
    uefi_call_wrapper(BS->Stall, 1, (UINTN)DELAY_SEC * 1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_STATUS s;
    UINTN n = 0, i, seg = 0, bus = 0, dev = 0, fun = 0;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT64 supports = 0, attrs = 0;
    UINTN sg, b, d, f;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V7 / BAR ATTR ONLY\r\n");

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) {
        Print(u"PCI ENUM FAIL %r\r\n", s);
        done();
        return s;
    }

    for (i = 0; i < n; ++i) {
        EFI_PCI_IO_PROTOCOL *q = NULL;

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
        Print(u"xHCI NOT FOUND\r\n");
        if (hs) FreePool(hs);
        done();
        return EFI_NOT_FOUND;
    }

    cfg32(p, 0, &id);
    cfg32(p, 8, &cls);
    cfg32(p, 0x10, &bar0);
    cfg32(p, 0x14, &bar1);

    Print(u"PCI %04x:%02x:%02x.%x %04x:%04x\r\n",
          (UINT32)seg, (UINT32)bus, (UINT32)dev, (UINT32)fun,
          id & 0xffff, id >> 16);
    Print(u"BAR0=%08x:%08x TYPE=%x\r\n",
          bar1, bar0, (bar0 >> 1) & 3);

    /*
     * GetBarAttributes is a PCI protocol query. We request only the
     * Supports mask and deliberately pass Resources=NULL, so this test
     * does not request the BAR resource-descriptor buffer and does not
     * perform any controller MMIO access.
     */
    s = uefi_call_wrapper(p->GetBarAttributes, 4, p, 0, &supports, NULL);
    if (EFI_ERROR(s)) {
        Print(u"BARATTR[0]=FAIL %r\r\n", s);
    } else {
        Print(u"BARATTR[0] SUPPORTS=%016lx\r\n", supports);
        Print(u"BAR WIDTH=%s\r\n",
              (supports & 64) ? u"64" :
              (supports & 32) ? u"32" : u"?");
    }

    s = uefi_call_wrapper(p->Attributes, 4, p,
                          EfiPciIoAttributeOperationGet, 0, &attrs);
    if (EFI_ERROR(s)) {
        Print(u"ATTR GET=FAIL %r\r\n", s);
    } else {
        Print(u"PCI ATTR=%016lx MEM=%c\r\n",
              attrs,
              (attrs & EFI_PCI_IO_ATTRIBUTE_MEMORY) ? 'Y' : 'N');
    }

    if (hs) FreePool(hs);
    done();
    return EFI_SUCCESS;
}
