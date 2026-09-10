#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define XHCI_MIN_VERSION 0x0100U
#define CMD_RUN   0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE  0x00000004U
#define STS_HCH    0x00000001U
#define STS_CNR    0x00000800U
#define CRCR_RCS   0x00000001ULL
#define CRCR_ADDR_MASK 0xffffffffffffffc0ULL
#define ERST_ADDR_MASK 0xffffffffffffffc0ULL
#define ERDP_ADDR_MASK 0xfffffffffffffff0ULL
#define ERDP_DCS   0x1ULL
#define MAX_SCRATCHPADS 256U
#define COMMON_PAGES 4U
#define ERST_SEGMENT_TRBS 16U

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,off,1,v);
}
static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,(UINT64)off,1,v);
}
static EFI_STATUS mmio64(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 *v) {
    return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint64,0,(UINT64)off,1,v);
}
static EFI_STATUS mmio_write32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 v) {
    return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,(UINT64)off,1,&v);
}
static EFI_STATUS mmio_write64(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 v) {
    return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint64,0,(UINT64)off,1,&v);
}

static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL *p, UINTN pages, VOID **host,
                            EFI_PHYSICAL_ADDRESS *dev, VOID **mapping) {
    EFI_STATUS s;
    UINTN bytes = pages * 4096;
    s = uefi_call_wrapper(p->AllocateBuffer,6,p,AllocateAnyPages,
                          EfiBootServicesData,pages,host,0);
    if (EFI_ERROR(s)) return s;
    uefi_call_wrapper(BS->SetMem,3,*host,bytes,0);
    s = uefi_call_wrapper(p->Map,6,p,EfiPciIoOperationBusMasterCommonBuffer,
                          *host,&bytes,dev,mapping);
    if (EFI_ERROR(s) || bytes != pages * 4096) {
        if (!EFI_ERROR(s)) uefi_call_wrapper(p->Unmap,2,p,*mapping);
        uefi_call_wrapper(p->FreeBuffer,3,p,pages,*host);
        *host = NULL;
        *mapping = NULL;
        return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
}

static void dma_free(EFI_PCI_IO_PROTOCOL *p, UINTN pages, VOID *host,
                     VOID *mapping) {
    if (mapping) uefi_call_wrapper(p->Unmap,2,p,mapping);
    if (host) uefi_call_wrapper(p->FreeBuffer,3,p,pages,host);
}

static EFI_STATUS wait_status(EFI_PCI_IO_PROTOCOL *p, UINT32 opbase,
                              UINT32 mask, UINT32 expected, UINTN loops,
                              UINT32 *last) {
    UINTN i;
    EFI_STATUS s;
    for (i=0;i<loops;i++) {
        s=mmio32(p,opbase+4,last);
        if(EFI_ERROR(s)) return s;
        if((*last & mask) == expected) return EFI_SUCCESS;
        uefi_call_wrapper(BS->Stall,1,1000);
    }
    return EFI_TIMEOUT;
}

static void finish(EFI_STATUS s, UINT32 reads, UINT32 writes) {
    Print(u"\r\nV29 COMPLETE / HALTED XHCI INITIALIZATION\r\n");
    Print(u"MMIO READS=%u WRITES=%u RESULT=%r\r\n",reads,writes,s);
    Print(u"DMA VIA EFI_PCI_IO MAP / NO RUN / NO DOORBELL / NO COMMANDS\r\n");
    Print(u"ALL CONTROLLER POINTERS CLEARED BEFORE DMA RELEASE\r\n");
    Print(u"EXIT 5 SEC...\r\n");
    uefi_call_wrapper(BS->Stall,1,5000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs=NULL;
    EFI_PCI_IO_PROTOCOL *p=NULL;
    EFI_STATUS s=EFI_SUCCESS;
    UINTN n=0,i;
    UINT32 id=0,cls=0,bar0=0,bar1=0,cap0=0,hcs1=0,hcs2=0,hcc1=0;
    UINT32 opbase=0,rtsoff=0,cmd=0,status=0,config=0;
    UINT32 reads=0,writes=0,maxslots=0,scratchpads=0;
    UINT64 dcbaa_dev=0,crcr_dev=0,event_dev=0,erst_dev=0;
    UINT64 dcbaa_rd=0,crcr_rd=0,erstba_rd=0,erdp_rd=0;
    VOID *common=NULL,*scratch_host[MAX_SCRATCHPADS];
    VOID *common_map=NULL,*scratch_map[MAX_SCRATCHPADS];
    EFI_PHYSICAL_ADDRESS common_dev=0,scratch_dev[MAX_SCRATCHPADS];
    UINT64 *dcbaa;
    UINT64 *erst;
    UINTN scratch_pages=0;
    UINTN t;
    BOOLEAN common_ok=FALSE, halted=FALSE, reset_done=FALSE;
    UINT32 bar_type;

    InitializeLib(image,st);
    uefi_call_wrapper(BS->SetMem,3,scratch_host,sizeof(scratch_host),0);
    uefi_call_wrapper(BS->SetMem,3,scratch_map,sizeof(scratch_map),0);
    uefi_call_wrapper(BS->SetMem,3,scratch_dev,sizeof(scratch_dev),0);

    Print(u"TOSHIBA xHCI V29 / HALTED INITIALIZATION PREPARATION\r\n");
    Print(u"RESET -> CNR CLEAR -> CAPS -> DMA MAP -> CONFIG/DCBAAP/CRCR/ERST\r\n");
    Print(u"NO RUN / NO DOORBELL / NO COMMAND EXECUTION\r\n");

    s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);
    if(EFI_ERROR(s)) goto out;

    for(i=0;i<n;i++) {
        EFI_PCI_IO_PROTOCOL *q=NULL;
        if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,
            (void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
        if(EFI_ERROR(cfg32(q,8,&cls)) || EFI_ERROR(cfg32(q,0,&id))) continue;
        if(((cls>>24)&255)==0x0c && ((cls>>16)&255)==0x03 &&
           ((cls>>8)&255)==0x30) { p=q; break; }
    }
    if(!p) { s=EFI_NOT_FOUND; goto out; }

    if(EFI_ERROR(cfg32(p,0x10,&bar0))) { s=EFI_DEVICE_ERROR; goto out; }
    bar_type=(UINT32)((bar0>>1)&3U);
    if((bar0&1U) || bar_type==1U) { s=EFI_UNSUPPORTED; goto out; }
    if(bar_type==2U) {
        if(EFI_ERROR(cfg32(p,0x14,&bar1))) { s=EFI_DEVICE_ERROR; goto out; }
    }

    if(EFI_ERROR(mmio32(p,0,&cap0))) { s=EFI_DEVICE_ERROR; goto out; }
    reads++;
    opbase=cap0&0xffU;
    if(EFI_ERROR(mmio32(p,0x02,&status))) { s=EFI_DEVICE_ERROR; goto out; }
    reads++;
    {
        UINT16 version=(UINT16)(status&0xffffU);
        if(version<XHCI_MIN_VERSION) { s=EFI_UNSUPPORTED; goto out; }
        Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x BAR=%08x/%08x OPBASE=%02x\r\n",
              version>>8,version&255,id&65535,id>>16,bar0,bar1,opbase);
    }

    if(EFI_ERROR(mmio32(p,0x04,&hcs1)) ||
       EFI_ERROR(mmio32(p,0x08,&hcs2)) ||
       EFI_ERROR(mmio32(p,0x10,&hcc1)) ||
       EFI_ERROR(mmio32(p,opbase,&cmd)) ||
       EFI_ERROR(mmio32(p,opbase+4,&status))) { s=EFI_DEVICE_ERROR; goto out; }
    reads+=5;
    maxslots=hcs1&0xffU;
    scratchpads=(((hcs2>>21)&0x1fU)<<5)|((hcs2>>27)&0x1fU);
    Print(u"CAPS: SLOTS=%u SCRATCHPADS=%u AC64=%u HCH=%u CNR=%u\r\n",
          maxslots,scratchpads,hcc1&1U,status&STS_HCH?1:0,status&STS_CNR?1:0);
    if(!maxslots || scratchpads>MAX_SCRATCHPADS) { s=EFI_UNSUPPORTED; goto out; }

    if(!(status&STS_HCH)) {
        s=mmio_write32(p,opbase,cmd & ~(CMD_RUN|CMD_INTE));
        if(EFI_ERROR(s)) goto out;
        writes++;
        s=wait_status(p,opbase,STS_HCH,STS_HCH,1000,&status);
        reads+=1;
        if(EFI_ERROR(s)) goto out;
        halted=TRUE;
    } else halted=TRUE;

    if(!halted) { s=EFI_DEVICE_ERROR; goto out; }

    s=mmio_write32(p,opbase,(cmd & ~(CMD_RUN|CMD_INTE))|CMD_RESET);
    if(EFI_ERROR(s)) goto out;
    writes++;
    for(t=0;t<1000;t++) {
        s=mmio32(p,opbase,&cmd); reads++;
        if(EFI_ERROR(s)) goto out;
        if(!(cmd&CMD_RESET)) { reset_done=TRUE; break; }
        uefi_call_wrapper(BS->Stall,1,1000);
    }
    if(!reset_done) { s=EFI_TIMEOUT; goto out; }

    s=wait_status(p,opbase,STS_CNR,0,10000,&status);
    reads++;
    if(EFI_ERROR(s)) goto out;
    if(!(status&STS_HCH)) { s=EFI_DEVICE_ERROR; goto out; }

    if(EFI_ERROR(mmio32(p,0x04,&hcs1)) ||
       EFI_ERROR(mmio32(p,0x08,&hcs2)) ||
       EFI_ERROR(mmio32(p,0x10,&hcc1))) { s=EFI_DEVICE_ERROR; goto out; }
    reads+=3;
    maxslots=hcs1&0xffU;
    scratchpads=(((hcs2>>21)&0x1fU)<<5)|((hcs2>>27)&0x1fU);
    Print(u"POST-RESET: SLOTS=%u SCRATCHPADS=%u AC64=%u HCH=1 CNR=0\r\n",
          maxslots,scratchpads,hcc1&1U);
    if(!maxslots || scratchpads>MAX_SCRATCHPADS) { s=EFI_UNSUPPORTED; goto out; }

    s=dma_alloc(p,COMMON_PAGES,&common,&common_dev,&common_map);
    if(EFI_ERROR(s)) goto out;
    common_ok=TRUE;
    dcbaa_dev=common_dev;
    crcr_dev=common_dev+4096;
    event_dev=common_dev+8192;
    erst_dev=common_dev+12288;
    if((dcbaa_dev&63ULL)||(crcr_dev&63ULL)||(event_dev&15ULL)||(erst_dev&63ULL)) {
        s=EFI_BAD_BUFFER_SIZE; goto teardown;
    }

    dcbaa=(UINT64*)common;
    erst=(UINT64*)((UINT8*)common+12288);
    if(scratchpads) {
        UINTN j;
        for(j=0;j<scratchpads;j++) {
            s=dma_alloc(p,1,&scratch_host[j],&scratch_dev[j],&scratch_map[j]);
            if(EFI_ERROR(s)) goto teardown;
            scratch_pages++;
            dcbaa[j]=scratch_dev[j];
        }
        dcbaa[0]=common_dev+0x400; /* temporary placeholder overwritten below */
        /*
         * Scratchpad Array must be a device-visible array of 64-bit pointers.
         * Reuse the first 4 KiB DCBAA page: its first entries are the scratchpad
         * pointers only when scratchpads are required. Reserve 0x400 bytes in
         * that page for the array and leave DCBAA entries above it zero.
         */
        {
            UINT64 *spa=(UINT64*)common;
            for(j=0;j<scratchpads;j++) spa[j]=scratch_dev[j];
            dcbaa[0]=common_dev;
        }
    }

    uefi_call_wrapper(BS->SetMem,3,(UINT8*)common+4096,4096,0);
    uefi_call_wrapper(BS->SetMem,3,(UINT8*)common+8192,4096,0);
    uefi_call_wrapper(BS->SetMem,3,(UINT8*)common+12288,4096,0);
    ((UINT32*)common)[0]=0;
    ((UINT64*)((UINT8*)common+12288))[0]=event_dev;
    ((UINT32*)((UINT8*)common+12288))[2]=ERST_SEGMENT_TRBS;

    s=mmio_write32(p,opbase+0x38,1);
    if(EFI_ERROR(s)) goto teardown;
    writes++;
    s=mmio32(p,opbase+0x38,&config); reads++;
    if(EFI_ERROR(s) || (config&0xffU)!=1) { s=EFI_DEVICE_ERROR; goto teardown; }

    s=mmio_write64(p,opbase+0x30,dcbaa_dev);
    if(EFI_ERROR(s)) goto teardown;
    writes++;
    s=mmio64(p,opbase+0x30,&dcbaa_rd); reads++;
    if(EFI_ERROR(s) || (dcbaa_rd&~63ULL)!=(dcbaa_dev&~63ULL)) { s=EFI_DEVICE_ERROR; goto teardown; }

    s=mmio_write64(p,opbase+0x18,crcr_dev|CRCR_RCS);
    if(EFI_ERROR(s)) goto teardown;
    writes++;
    s=mmio64(p,opbase+0x18,&crcr_rd); reads++;
    if(EFI_ERROR(s) || (crcr_rd&CRCR_ADDR_MASK)!=(crcr_dev&CRCR_ADDR_MASK)) { s=EFI_DEVICE_ERROR; goto teardown; }

    if(EFI_ERROR(mmio32(p,0x18,&rtsoff))) { s=EFI_DEVICE_ERROR; goto teardown; }
    reads++;
    s=mmio_write32(p,rtsoff+0x20+0x08,1);
    if(EFI_ERROR(s)) goto teardown;
    writes++;
    s=mmio_write64(p,rtsoff+0x20+0x10,erst_dev);
    if(EFI_ERROR(s)) goto teardown;
    writes++;
    s=mmio_write64(p,rtsoff+0x20+0x18,event_dev|ERDP_DCS);
    if(EFI_ERROR(s)) goto teardown;
    writes++;

    if(EFI_ERROR(mmio64(p,rtsoff+0x20+0x10,&erstba_rd)) ||
       EFI_ERROR(mmio64(p,rtsoff+0x20+0x18,&erdp_rd))) { s=EFI_DEVICE_ERROR; goto teardown; }
    reads+=2;
    if((erstba_rd&ERST_ADDR_MASK)!=(erst_dev&ERST_ADDR_MASK) ||
       (erdp_rd&ERDP_ADDR_MASK)!=(event_dev&ERDP_ADDR_MASK) ||
       !(erdp_rd&ERDP_DCS)) { s=EFI_DEVICE_ERROR; goto teardown; }

    if(EFI_ERROR(mmio32(p,opbase,&cmd)) ||
       EFI_ERROR(mmio32(p,opbase+4,&status))) { s=EFI_DEVICE_ERROR; goto teardown; }
    reads+=2;
    if((cmd&CMD_RUN) || !(status&STS_HCH) || (status&STS_CNR)) { s=EFI_DEVICE_ERROR; goto teardown; }

    Print(u"V29 HALTED INITIALIZATION: PASS\r\n");
    Print(u"DMA MAP: COMMON=%016lx SCRATCHPADS=%u\r\n",common_dev,scratchpads);
    Print(u"CONFIG=1 DCBAAP=%016lx CRCR=%016lx ERSTBA=%016lx ERDP=%016lx\r\n",
          dcbaa_dev,crcr_dev,erst_dev,event_dev|ERDP_DCS);
    s=EFI_SUCCESS;

teardown:
    /*
     * Clear controller references before unmapping/freeing DMA. The controller
     * is halted and no command/transfer was submitted.
     */
    if(p) {
        mmio_write64(p,opbase+0x18,0); writes++;
        mmio_write64(p,opbase+0x30,0); writes++;
        mmio_write32(p,opbase+0x38,0); writes++;
        if(rtsoff) {
            mmio_write64(p,rtsoff+0x20+0x10,0); writes++;
            mmio_write64(p,rtsoff+0x20+0x18,0); writes++;
            mmio_write32(p,rtsoff+0x20+0x08,0); writes++;
        }
    }
    for(t=0;t<scratch_pages;t++) dma_free(p,1,scratch_host[t],scratch_map[t]);
    if(common_ok) dma_free(p,COMMON_PAGES,common,common_map);

out:
    if(hs) FreePool(hs);
    finish(s,reads,writes);
    return s;
}
