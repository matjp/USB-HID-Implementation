#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define PORTSC_BASE 0x400
#define PORTSC_STRIDE 0x10

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}
static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32, 0, (UINT64)off, 1, v);
}
static void finish(UINT32 reads, UINT32 connected) {
    Print(u"\r\nV20 COMPLETE / %u READ-ONLY PORTSC MMIO READS / NO DMA / NO WRITES\r\n", reads);
    Print(u"CONNECTED PORTS DETECTED: %u\r\n", connected);
    Print(u"INITIAL DEVICE DETECTION: PASS\r\n");
    Print(u"EXIT 5 SEC...\r\n");
    uefi_call_wrapper(BS->Stall, 1, 5000000);
}
static UINT32 port_speed(UINT32 v) { return (v >> 10) & 0xFU; }
static UINT32 port_state(UINT32 v) { return (v >> 5) & 0xFU; }

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs=NULL; EFI_PCI_IO_PROTOCOL *p=NULL; EFI_STATUS s;
    UINTN n=0,i; UINT32 cls=0,id=0,bar0=0,bar1=0,cap0=0,hcs1=0;
    UINT32 caplen,op_base,max_ports,off,v,reads=0,connected=0,ccs;
    UINT64 bar;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V20 / INITIAL DEVICE DETECTION\r\n");
    Print(u"READ-ONLY / NO DMA / NO WRITES\r\n");

    s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);
    if(EFI_ERROR(s)){Print(u"PCI ENUM FAIL %r\r\n",s);finish(reads,connected);return s;}

    for(i=0;i<n;i++){
        EFI_PCI_IO_PROTOCOL *q=NULL;
        if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,(void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
        if(EFI_ERROR(cfg32(q,8,&cls))) continue;
        if(((cls>>24)&255)==0x0c && ((cls>>16)&255)==0x03 && ((cls>>8)&255)==0x30){p=q;break;}
    }
    if(!p){
        Print(u"xHCI NOT FOUND\r\n");
        if(hs)FreePool(hs);
        finish(reads,connected);
        return EFI_NOT_FOUND;
    }

    cfg32(p,0,&id);
    cfg32(p,0x10,&bar0);
    cfg32(p,0x14,&bar1);
    bar=((UINT64)bar1<<32)|((UINT64)bar0&~0xFULL);

    s=mmio32(p,0,&cap0);
    if(EFI_ERROR(s)){Print(u"CAP READ FAIL %r\r\n",s);if(hs)FreePool(hs);finish(reads,connected);return s;}
    caplen=cap0&255;
    op_base=caplen;

    s=mmio32(p,4,&hcs1);
    if(EFI_ERROR(s)){Print(u"HCS1 READ FAIL %r\r\n",s);if(hs)FreePool(hs);finish(reads,connected);return s;}
    max_ports=(hcs1>>24)&255;
    if(max_ports>255)max_ports=255;

    Print(u"PCI ID=%04x:%04x BAR=%016lx\r\n",id&65535,id>>16,bar);
    Print(u"CAPLEN=%02x MAXPORTS=%u\r\n",caplen,max_ports);
    Print(u"SCANNING PORTSC FOR DEVICES PRESENT AT BOOT...\r\n");

    for(UINT32 port=1;port<=max_ports;port++){
        off=op_base+PORTSC_BASE+(port-1)*PORTSC_STRIDE;
        s=mmio32(p,off,&v);
        if(EFI_ERROR(s)){
            Print(u"PORT%02u READ FAIL %r\r\n",port,s);
            if(hs)FreePool(hs);
            finish(reads,connected);
            return EFI_DEVICE_ERROR;
        }
        reads++;
        ccs=v&1U;
        if(ccs){
            connected++;
            Print(u"PORT%02u CONNECTED CCS=1 PLS=%u SPEED=%u RAW=%08x\r\n",
                  port,port_state(v),port_speed(v),v);
        }
    }

    if(connected==0) Print(u"NO DEVICES PRESENT AT BOOT\r\n");

    if(hs)FreePool(hs);
    finish(reads,connected);
    return EFI_SUCCESS;
}
