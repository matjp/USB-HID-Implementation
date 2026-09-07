#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define PORTSC_BASE 0x400
#define PORTSC_STRIDE 0x10
#define POLL_MS 500
#define POLLS 60

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}
static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32, 0, (UINT64)off, 1, v);
}
static void finish(UINT32 reads, UINT32 events) {
    Print(u"\r\nV19 COMPLETE / %u READ-ONLY PORTSC MMIO READS / NO DMA / NO WRITES\r\n", reads);
    Print(u"PORT EVENTS DETECTED: %u\r\n", events);
    Print(u"EXIT 5 SEC...\r\n");
    uefi_call_wrapper(BS->Stall, 1, 5000000);
}
static UINT32 state(UINT32 v) {
    return v & 0x00000FFFU;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs=NULL; EFI_PCI_IO_PROTOCOL *p=NULL; EFI_STATUS s;
    UINTN n=0,i; UINT32 cls=0,id=0,bar0=0,bar1=0,cap0=0,hcs1=0;
    UINT32 caplen,op_base,max_ports,off,v,prev[256],reads=0,events=0,poll,port;
    UINT64 bar;

    InitializeLib(image, st);
    for(i=0;i<256;i++) prev[i]=0;
    Print(u"TOSHIBA xHCI V19 / ATTACH-DETACH OBSERVER\r\n");
    Print(u"READ-ONLY / NO DMA / NO WRITES\r\n");

    s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);
    if(EFI_ERROR(s)){Print(u"PCI ENUM FAIL %r\r\n",s);finish(reads,events);return s;}
    for(i=0;i<n;i++){
        EFI_PCI_IO_PROTOCOL *q=NULL;
        if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,(void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
        if(EFI_ERROR(cfg32(q,8,&cls))) continue;
        if(((cls>>24)&255)==0x0c && ((cls>>16)&255)==0x03 && ((cls>>8)&255)==0x30){p=q;break;}
    }
    if(!p){Print(u"xHCI NOT FOUND\r\n");if(hs)FreePool(hs);finish(reads,events);return EFI_NOT_FOUND;}

    cfg32(p,0,&id); cfg32(p,0x10,&bar0); cfg32(p,0x14,&bar1);
    bar=((UINT64)bar1<<32)|((UINT64)bar0&~0xFULL);
    s=mmio32(p,0,&cap0); if(EFI_ERROR(s)){Print(u"CAP READ FAIL %r\r\n",s);if(hs)FreePool(hs);finish(reads,events);return s;}
    caplen=cap0&255; op_base=caplen;
    s=mmio32(p,4,&hcs1); if(EFI_ERROR(s)){Print(u"HCS1 READ FAIL %r\r\n",s);if(hs)FreePool(hs);finish(reads,events);return s;}
    max_ports=(hcs1>>24)&255; if(max_ports>255)max_ports=255;
    Print(u"PCI ID=%04x:%04x BAR=%016lx\r\n",id&65535,id>>16,bar);
    Print(u"CAPLEN=%02x MAXPORTS=%u\r\n",caplen,max_ports);
    Print(u"ARMING BASELINE; CONNECT/DISCONNECT A USB DEVICE DURING THE OBSERVATION WINDOW.\r\n");

    for(port=1;port<=max_ports;port++){
        off=op_base+PORTSC_BASE+(port-1)*PORTSC_STRIDE;
        if(EFI_ERROR(mmio32(p,off,&v))){Print(u"PORT%02u READ FAIL\r\n",port);if(hs)FreePool(hs);finish(reads,events);return EFI_DEVICE_ERROR;}
        prev[port]=state(v);reads++;
    }
    Print(u"BASELINE CAPTURED / %u PORTS\r\n",max_ports);
    Print(u"OBSERVING %u POLLS @ %u MS (%u SEC)\r\n",POLLS,POLL_MS,(POLLS*POLL_MS)/1000);

    for(poll=1;poll<=POLLS;poll++){
        for(port=1;port<=max_ports;port++){
            off=op_base+PORTSC_BASE+(port-1)*PORTSC_STRIDE;
            if(EFI_ERROR(mmio32(p,off,&v))){Print(u"PORT%02u READ FAIL\r\n",port);if(hs)FreePool(hs);finish(reads,events);return EFI_DEVICE_ERROR;}
            reads++;
            if(state(v)!=prev[port]){
                UINT32 old=prev[port], now=state(v);
                events++;
                Print(u"EVENT %u POLL %u PORT%02u %03x -> %03x CCS %u->%u PLS %u->%u\r\n",
                      events,poll,port,old,now,old&1,now&1,(old>>5)&15,(now>>5)&15);
                prev[port]=now;
            }
        }
        uefi_call_wrapper(BS->Stall,1,(UINTN)POLL_MS*1000);
    }
    if(hs)FreePool(hs); finish(reads,events); return EFI_SUCCESS;
}
