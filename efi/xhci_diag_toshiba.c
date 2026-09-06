#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define TARGET_BUS 0
#define TARGET_DEV 20
#define TARGET_FUN 0
#define BOOT_DELAY_SECONDS 30
#define XHCI_PORTSC_BASE 0x400
#define XHCI_PORTSC_STRIDE 0x10
#define XHCI_MAX_PORTS 255

static EFI_GUID PciIoGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS read_cfg32(EFI_PCI_IO *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}
static EFI_STATUS read_mem32(EFI_PCI_IO *p, UINT64 off, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32, 0, off, 1, v);
}

static EFI_STATUS find_xhci(EFI_HANDLE image, EFI_PCI_IO **out) {
    EFI_STATUS st; EFI_HANDLE *hs = NULL; UINTN n = 0, i;
    st = LibLocateHandle(ByProtocol, &PciIoGuid, NULL, &n, &hs);
    if (EFI_ERROR(st)) return st;
    for (i = 0; i < n; i++) {
        EFI_PCI_IO *p = NULL; UINTN seg, bus, dev, fun; UINT32 id, cls;
        st = uefi_call_wrapper(BS->OpenProtocol, 6, hs[i], &PciIoGuid,
                               (void **)&p, image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(st)) continue;
        if (EFI_ERROR(uefi_call_wrapper(p->GetLocation, 5, p, &seg, &bus, &dev, &fun))) continue;
        if (bus != TARGET_BUS || dev != TARGET_DEV || fun != TARGET_FUN) continue;
        if (EFI_ERROR(read_cfg32(p, 0, &id)) || EFI_ERROR(read_cfg32(p, 8, &cls))) continue;
        if (((cls >> 24) & 0xff) == 0x0c && ((cls >> 16) & 0xff) == 0x03 &&
            ((cls >> 8) & 0xff) == 0x30) {
            *out = p; return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

static void pause_exit(void) {
    Print(u"\r\nREAD-ONLY: no xHCI registers written.\r\nEXIT IN %u SECONDS...\r\n", BOOT_DELAY_SECONDS);
    uefi_call_wrapper(BS->Stall, 1, (UINTN)BOOT_DELAY_SECONDS * 1000000);
}

static void print_port(EFI_PCI_IO *p, UINT32 opbase, UINT32 port) {
    UINT32 v;
    EFI_STATUS st = read_mem32(p, (UINT64)opbase + XHCI_PORTSC_BASE +
                               (UINT64)(port - 1) * XHCI_PORTSC_STRIDE, &v);
    if (EFI_ERROR(st)) { Print(u"P%02u=ERR ", port); return; }
    Print(u"P%02u:%08x[%u%u%u%uS%u%u%u] ", port, v,
          v&1, (v>>1)&1, (v>>4)&1, (v>>9)&1, (v>>10)&0xf,
          (v>>17)&1, (v>>21)&1);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_PCI_IO *p = NULL; EFI_STATUS rc; UINTN seg,bus,dev,fun;
    UINT32 id, cls, bar0, bar1, cap, hcs1, hcs2, hcs3, hcc1, dboff, rtsoff;
    UINT32 usbcmd, usbsts, pagesize, dnctrl, crcrlo, crcrhi, dcbaalo, dcbaahi, config;
    UINT32 opbase, nports; UINT64 bar;

    InitializeLib(image, st);
    Print(u"TOSHIBA P50 xHCI READ-ONLY DIAG\r\n");
    Print(u"PCI 00:%02x.%x  CLASS 0c/03/30\r\n", TARGET_DEV, TARGET_FUN);

    rc = find_xhci(image, &p);
    if (EFI_ERROR(rc)) { Print(u"xHCI NOT FOUND: %r\r\n", rc); pause_exit(); return rc; }
    rc = uefi_call_wrapper(p->GetLocation, 5, p, &seg, &bus, &dev, &fun);
    if (EFI_ERROR(rc)) { Print(u"GetLocation: %r\r\n", rc); pause_exit(); return rc; }
    read_cfg32(p, 0, &id); read_cfg32(p, 8, &cls); read_cfg32(p, 0x10, &bar0); read_cfg32(p, 0x14, &bar1);
    bar = ((UINT64)bar1 << 32) | ((UINT64)bar0 & ~0xFULL);
    Print(u"PCI %04x:%02x:%02x.%x VID:DID=%04x:%04x BAR0=%08x:%08x\r\n",
          seg,bus,dev,fun,id&0xffff,id>>16,bar1,bar0);
    Print(u"BAR=0x%016lx CLASS=%02x:%02x:%02x\r\n", bar,
          cls>>24,(cls>>16)&0xff,(cls>>8)&0xff);

    if (EFI_ERROR(read_mem32(p,0,&cap)) || EFI_ERROR(read_mem32(p,4,&hcs1)) ||
        EFI_ERROR(read_mem32(p,8,&hcs2)) || EFI_ERROR(read_mem32(p,0xc,&hcs3)) ||
        EFI_ERROR(read_mem32(p,0x10,&hcc1)) || EFI_ERROR(read_mem32(p,0x14,&dboff)) ||
        EFI_ERROR(read_mem32(p,0x18,&rtsoff))) {
        Print(u"CAPABILITY READ FAILED\r\n"); pause_exit(); return EFI_DEVICE_ERROR;
    }
    opbase = cap & 0xff; nports = (hcs1 >> 24) & 0xff;
    UINT16 ver = (cap >> 16) & 0xffff;
    UINT32 xecp = ((hcc1 >> 16) & 0xffff) << 2;
    Print(u"CAP len=%02x ver=%04x SLOTS=%u INTR=%u PORTS=%u 64BIT=%u\r\n",
          opbase,ver,hcs1&0xff,(hcs1>>8)&0x7ff,nports,hcc1&1);
    Print(u"DBOFF=%08x RTSOFF=%08x xECP=%04x HCS2=%08x\r\n",dboff,rtsoff,xecp,hcs2);

    if (EFI_ERROR(read_mem32(p,opbase+0x00,&usbcmd)) || EFI_ERROR(read_mem32(p,opbase+0x04,&usbsts)) ||
        EFI_ERROR(read_mem32(p,opbase+0x08,&pagesize)) || EFI_ERROR(read_mem32(p,opbase+0x14,&dnctrl)) ||
        EFI_ERROR(read_mem32(p,opbase+0x18,&crcrlo)) || EFI_ERROR(read_mem32(p,opbase+0x1c,&crcrhi)) ||
        EFI_ERROR(read_mem32(p,opbase+0x30,&dcbaalo)) || EFI_ERROR(read_mem32(p,opbase+0x34,&dcbaahi)) ||
        EFI_ERROR(read_mem32(p,opbase+0x38,&config))) {
        Print(u"OPERATIONAL READ FAILED\r\n"); pause_exit(); return EFI_DEVICE_ERROR;
    }
    Print(u"OP CMD=%08x STS=%08x PAGE=%08x CFG=%08x\r\n",usbcmd,usbsts,pagesize,config);
    Print(u"RING CRCR=%08x:%08x DCBAAP=%08x:%08x DN=%08x\r\n",crcrhi,crcrlo,dcbaahi,dcbaalo,dnctrl);
    Print(u"STATE RUN=%u HCH=%u HSE=%u EINT=%u PCD=%u CNR=%u HCE=%u\r\n",
          usbcmd&1,(usbsts>>0)&1,(usbsts>>2)&1,(usbsts>>3)&1,(usbsts>>4)&1,(usbsts>>11)&1,(usbsts>>12)&1);

    Print(u"PORTS\r\n");
    if (nports > XHCI_MAX_PORTS) nports = XHCI_MAX_PORTS;
    for (UINT32 port=1; port<=nports; port++) {
        print_port(p,opbase,port);
        if ((port % 4) == 0) Print(u"\r\n");
    }
    if (nports % 4) Print(u"\r\n");

    Print(u"EXTCAP");
    for (UINT32 off=xecp,count=0; off && count<24; count++) {
        UINT32 hdr,v;
        if (EFI_ERROR(read_mem32(p,off,&hdr))) { Print(u" @%x=ERR",off); break; }
        UINT8 eid=hdr&0xff, next=(hdr>>8)&0xff;
        Print(u" %04x:%u",off,eid);
        if (eid==1 && !EFI_ERROR(read_mem32(p,off+4,&v))) Print(u"(B%u/O%u)",(v>>16)&1,(v>>24)&1);
        if (!next) break;
        off += (UINT32)next*4;
    }
    Print(u"\r\n");
    pause_exit();
    return EFI_SUCCESS;
}
