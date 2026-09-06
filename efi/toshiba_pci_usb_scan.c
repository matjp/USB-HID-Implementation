#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_STATUS s;
    EFI_HANDLE *h = NULL;
    UINTN n = 0, i, printed = 0;

    InitializeLib(image, st);
    Print(u"TOSHIBA P50 / PCI USB SCAN\r\n");
    Print(u"READ ONLY - NO CONFIG/MMIO WRITES\r\n");

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &h);
    if (EFI_ERROR(s)) {
        Print(u"PCI I/O ENUM FAILED %r\r\n", s);
        goto done;
    }

    Print(u"PCI I/O=%u  USB DEVICES:\r\n", n);
    for (i = 0; i < n; ++i) {
        EFI_PCI_IO_PROTOCOL *p = NULL;
        UINTN seg,bus,dev,fun;
        UINT32 id, cls, bar0 = 0, bar1 = 0;
        UINT8 base, sub, prog;

        s = uefi_call_wrapper(BS->OpenProtocol, 6, h[i], &PciGuid,
                              (void **)&p, image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s)) continue;
        if (EFI_ERROR(uefi_call_wrapper(p->GetLocation, 5, p, &seg, &bus, &dev, &fun))) continue;
        if (EFI_ERROR(cfg32(p, 0, &id)) || EFI_ERROR(cfg32(p, 8, &cls))) continue;

        base = (UINT8)(cls >> 24); sub = (UINT8)(cls >> 16); prog = (UINT8)(cls >> 8);
        if (base != 0x0c || sub != 0x03) continue;

        cfg32(p, 0x10, &bar0);
        cfg32(p, 0x14, &bar1);
        Print(u"%04x:%02x:%02x.%x %04x:%04x %02x:%02x:%02x B=%08x:%08x",
              (UINT32)seg,(UINT32)bus,(UINT32)dev,(UINT32)fun,
              id & 0xffff, id >> 16, base, sub, prog, bar1, bar0);
        if (prog == 0x30) Print(u" XHCI");
        else if (prog == 0x20) Print(u" EHCI");
        Print(u"\r\n");
        ++printed;
    }
    if (!printed) Print(u"NONE\r\n");

done:
    Print(u"\r\nNO WRITES. PAUSE 30s...\r\n");
    uefi_call_wrapper(BS->Stall, 1, (UINTN)30000000);
    if (h) FreePool(h);
    return EFI_SUCCESS;
}
