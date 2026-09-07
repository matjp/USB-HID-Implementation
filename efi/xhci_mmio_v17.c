#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define EXIT_SEC 5
#define PORTSC_BASE 0x400
#define PORTSC_STRIDE 0x10
#define PASSES 3
#define PAGE_PORTS 19
#define PAGE_PAUSE_SEC 3

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}
static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32, 0, (UINT64)off, 1, v);
}
static void pause_page(void) {
    uefi_call_wrapper(BS->Stall, 1, (UINTN)PAGE_PAUSE_SEC * 1000000);
}
static void finish(UINT32 reads) {
    Print(u"\r\nV17 COMPLETE / %u READ-ONLY PORTSC MMIO READS / NO DMA / NO WRITES\r\n", reads);
    Print(u"EXIT %u SEC...\r\n", EXIT_SEC);
    uefi_call_wrapper(BS->Stall, 1, (UINTN)EXIT_SEC * 1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_STATUS s;
    UINTN n = 0, i;
    UINT32 id=0, bar0=0, bar1=0, cap0=0, hcs1=0;
    UINT32 caplen, max_ports, op_base, off, portsc, reads=0;
    UINT64 bar;
    UINT32 pass, port;
    UINTN page;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V17 / 57 READ PAGINATED\r\n");

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) { Print(u"PCI ENUM FAIL %r\r\n", s); finish(reads); return s; }

    for (i=0; i<n; ++i) {
        EFI_PCI_IO_PROTOCOL *q=NULL; UINT32 cls=0;
        s = uefi_call_wrapper(BS->OpenProtocol, 6, hs[i], &PciGuid, (void **)&q,
                              image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s)) continue;
        if (EFI_ERROR(cfg32(q, 8, &cls))) continue;
        if (((cls>>24)&0xff)!=0x0c || ((cls>>16)&0xff)!=0x03 || ((cls>>8)&0xff)!=0x30) continue;
        p=q; break;
    }
    if (!p) { Print(u"xHCI NOT FOUND\r\n"); if(hs) FreePool(hs); finish(reads); return EFI_NOT_FOUND; }

    cfg32(p,0,&id); cfg32(p,0x10,&bar0); cfg32(p,0x14,&bar1);
    bar=((UINT64)bar1<<32)|((UINT64)bar0&~0xFULL);
    Print(u"PCI ID=%04x:%04x BAR=%016lx\r\n", id&0xffff, id>>16, bar);
    if ((bar0&1)||!bar) { Print(u"BAD MEMORY BAR\r\n"); if(hs)FreePool(hs); finish(reads); return EFI_DEVICE_ERROR; }

    s=mmio32(p,0,&cap0);
    if(EFI_ERROR(s)){Print(u"MMIO[000]=FAIL %r\r\n",s);if(hs)FreePool(hs);finish(reads);return s;}
    caplen=cap0&0xff; op_base=caplen;
    Print(u"MMIO[000]=%08x CAPLEN=%02x VER=%04x\r\n",cap0,caplen,(cap0>>16)&0xffff);

    s=mmio32(p,4,&hcs1);
    if(EFI_ERROR(s)){Print(u"MMIO[004]=FAIL %r\r\n",s);if(hs)FreePool(hs);finish(reads);return s;}
    max_ports=(hcs1>>24)&0xff;
    Print(u"MMIO[004]=%08x MAXPORTS=%u\r\n",hcs1,max_ports);

    for(pass=1; pass<=PASSES; ++pass) {
        for(page=0; page<((max_ports+PAGE_PORTS-1)/PAGE_PORTS); ++page) {
            UINT32 first=(UINT32)(page*PAGE_PORTS+1);
            UINT32 last=first+PAGE_PORTS-1;
            if(last>max_ports) last=max_ports;
            Print(u"\r\nPASS %u/3  PAGE %u/%u  PORTS %u-%u\r\n",
                  pass,(UINT32)page+1,(UINT32)((max_ports+PAGE_PORTS-1)/PAGE_PORTS),first,last);
            for(port=first;port<=last;++port) {
                off=op_base+PORTSC_BASE+((port-1)*PORTSC_STRIDE);
                s=mmio32(p,off,&portsc);
                if(EFI_ERROR(s)){Print(u"PORT%02u MMIO[%03x]=FAIL %r\r\n",port,off,s);if(hs)FreePool(hs);finish(reads);return s;}
                ++reads;
                Print(u"PORT%02u MMIO[%03x]=%08x CCS=%u PED=%u PP=%u PLS=%u\r\n",
                      port,off,portsc,portsc&1,(portsc>>1)&1,(portsc>>9)&1,(portsc>>5)&0xf);
            }
            if(pass<PASSES) pause_page();
        }
    }
    if(hs) FreePool(hs);
    finish(reads);
    return EFI_SUCCESS;
}
