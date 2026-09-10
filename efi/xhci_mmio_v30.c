#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define XHCI_MIN_VERSION 0x0100U
#define CMD_RUN   0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE  0x00000004U
#define CMD_HSEE  0x00000008U
#define STS_HCH   0x00000001U
#define STS_EINT  0x00000008U
#define STS_HSE   0x00000004U
#define STS_CNR   0x00000800U
#define IMAN_IE   0x00000002U
#define IMAN_IP   0x00000001U
#define CRCR_RCS  0x00000001ULL
#define ERST_ADDR_MASK 0xffffffffffffffc0ULL
#define ERDP_ADDR_MASK 0xfffffffffffffff0ULL
#define TRB_LINK_TYPE (6U<<10)
#define TRB_LINK_TOGGLE (1U<<1)
#define MAX_SCRATCHPADS 1024U
#define COMMON_PAGES 4U
#define ERST_SEGMENT_TRBS 16U

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static const CHAR16 *fail_stage = u"NONE";
static const CHAR16 *fail_op = u"NONE";
static EFI_STATUS fail_status = EFI_SUCCESS;

static void remember_failure(const CHAR16 *stage, const CHAR16 *op, EFI_STATUS s) {
    if (!EFI_ERROR(fail_status)) { fail_stage=stage; fail_op=op; fail_status=s; }
}
static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,off,1,v);
}
static EFI_STATUS mmio16(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT16 *v) {
    return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint16,0,(UINT64)off,1,v);
}
static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,(UINT64)off,1,v);
}
static EFI_STATUS mmio64_split(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 *v) {
    UINT32 lo=0,hi=0; EFI_STATUS s=mmio32(p,off,&lo);
    if(EFI_ERROR(s)) return s; s=mmio32(p,off+4,&hi); if(EFI_ERROR(s)) return s;
    *v=((UINT64)hi<<32)|lo; return EFI_SUCCESS;
}
static EFI_STATUS mmio_write32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 v) {
    return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,(UINT64)off,1,&v);
}
static EFI_STATUS mmio_write64_split(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 v) {
    UINT32 lo=(UINT32)v,hi=(UINT32)(v>>32); EFI_STATUS s=mmio_write32(p,off,lo);
    if(EFI_ERROR(s)) return s; return mmio_write32(p,off+4,hi);
}
static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL *p, UINTN pages, VOID **host,
                            EFI_PHYSICAL_ADDRESS *dev, VOID **mapping) {
    EFI_STATUS s; UINTN bytes=pages*4096U;
    *host=NULL; *mapping=NULL; *dev=0;
    if(!pages || pages>(UINTN)(~(UINTN)0)/4096U) return EFI_BAD_BUFFER_SIZE;
    s=uefi_call_wrapper(p->AllocateBuffer,6,p,AllocateAnyPages,EfiBootServicesData,pages,host,0);
    if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(BS->SetMem,3,*host,bytes,0);
    if(EFI_ERROR(s)){ uefi_call_wrapper(p->FreeBuffer,3,p,pages,*host); *host=NULL; return s; }
    s=uefi_call_wrapper(p->Map,6,p,EfiPciIoOperationBusMasterCommonBuffer,*host,&bytes,dev,mapping);
    if(EFI_ERROR(s)||bytes!=pages*4096U){
        EFI_STATUS x=EFI_ERROR(s)?s:EFI_DEVICE_ERROR;
        if(!EFI_ERROR(s)&&*mapping) uefi_call_wrapper(p->Unmap,2,p,*mapping);
        uefi_call_wrapper(p->FreeBuffer,3,p,pages,*host);
        *host=NULL; *mapping=NULL; *dev=0; return x;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL *p, UINTN pages, VOID *host, VOID *mapping) {
    EFI_STATUS s=EFI_SUCCESS,t;
    if(mapping){ t=uefi_call_wrapper(p->Unmap,2,p,mapping); if(EFI_ERROR(t)) s=t; }
    if(host){ t=uefi_call_wrapper(p->FreeBuffer,3,p,pages,host); if(EFI_ERROR(t)&&!EFI_ERROR(s)) s=t; }
    return s;
}
static EFI_STATUS wait_status(EFI_PCI_IO_PROTOCOL *p, UINT32 opbase, UINT32 mask,
                              UINT32 expected, UINTN loops, UINT32 *last, UINT32 *reads) {
    UINTN i; EFI_STATUS s;
    for(i=0;i<loops;i++){
        s=mmio32(p,opbase+4,last); (*reads)++; if(EFI_ERROR(s)) return s;
        if((*last&mask)==expected) return EFI_SUCCESS;
        uefi_call_wrapper(BS->Stall,1,1000);
    }
    return EFI_TIMEOUT;
}
static void finish(EFI_STATUS s, UINT32 reads, UINT32 writes, UINT32 events) {
    Print(u"\r\nV30 COMPLETE / CONTROLLER START WITHOUT COMMANDS\r\n");
    Print(u"MMIO READS=%u WRITES=%u RESULT=%r\r\n",reads,writes,s);
    Print(u"FAIL STAGE=%s OP=%s STATUS=%r\r\n",fail_stage,fail_op,fail_status);
    Print(u"RUN START/HALT/RESET DMA LIFETIME TEST\r\n");
    Print(u"COMMANDS=0 DOORBELLS=0 CPU-INTERRUPTS=0 EVENTS=%u\r\n",events);
    Print(u"ALL CONTROLLER POINTERS CLEARED BEFORE DMA RELEASE\r\n");
    Print(u"EXIT 5 SEC...\r\n");
    uefi_call_wrapper(BS->Stall,1,5000000);
}
static void fatal_dma_running(void) {
    Print(u"\r\nFATAL: XHCI NOT CONFIRMED HALTED\r\n");
    Print(u"DMA MAPPINGS RETAINED / NO FREE / MANUAL RECOVERY REQUIRED\r\n");
    for(;;) uefi_call_wrapper(BS->Stall,1,1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs=NULL; EFI_PCI_IO_PROTOCOL *p=NULL; EFI_STATUS s=EFI_SUCCESS,ts;
    UINTN n=0,i,j,t; UINT32 id=0,cls=0,bar0=0,bar1=0,cap0=0,hcs1=0,hcs2=0,hcc1=0;
    UINT32 opbase=0,rtsoff=0,cmd=0,status=0,config=0,iman=0,events=0; UINT32 reads=0,writes=0;
    UINT32 maxslots=0,scratchpads=0,pagesize_reg=0; UINTN xhci_pagesize=0,page_shift=0;
    UINTN dcbaa_pages=0,spa_pages=0,total_bytes=0,common_pages=0,scratch_block_pages=0;
    UINT64 dcbaa_dev=0,spa_dev=0,crcr_dev=0,event_dev=0,erst_dev=0,erstba_rd=0,erdp_rd=0,dcbaa_rd=0;
    VOID *common=NULL,*common_map=NULL,*scratch_block=NULL,*scratch_aligned=NULL,*scratch_map=NULL;
    EFI_PHYSICAL_ADDRESS common_dev=0,scratch_dev[MAX_SCRATCHPADS],scratch_aligned_dev=0;
    UINT64 *dcbaa,*erst; BOOLEAN common_ok=FALSE,reset_done=FALSE,ac64=FALSE,controller_halted=FALSE;
    UINT32 bar_type; UINTN observation_us=1500000;

    InitializeLib(image,st);
    uefi_call_wrapper(BS->SetMem,3,scratch_dev,sizeof(scratch_dev),0);
    Print(u"TOSHIBA xHCI V30 / CONTROLLER START WITHOUT COMMANDS\r\n");
    Print(u"UEFI DMA MAP -> INIT -> RUN -> OBSERVE -> HALT -> RESET\r\n");
    Print(u"NO COMMAND / NO DOORBELL / NO CPU INTERRUPTS / NO USB TRANSFER\r\n");

    s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs); if(EFI_ERROR(s)){remember_failure(u"PCI",u"LOCATE PCI IO",s);goto out;}
    for(i=0;i<n;i++){
        EFI_PCI_IO_PROTOCOL *q=NULL;
        s=uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,(void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if(EFI_ERROR(s)) continue;
        if(EFI_ERROR(cfg32(q,8,&cls))||EFI_ERROR(cfg32(q,0,&id))) continue;
        if(((cls>>24)&255)==0x0c&&((cls>>16)&255)==0x03&&((cls>>8)&255)==0x30){p=q;break;}
    }
    if(!p){s=EFI_NOT_FOUND;remember_failure(u"PCI",u"FIND XHCI",s);goto out;}
    s=cfg32(p,0x10,&bar0); if(EFI_ERROR(s)){remember_failure(u"PCI",u"READ BAR0",s);goto out;}
    bar_type=(bar0>>1)&3U; if((bar0&1U)||bar_type!=2U){s=EFI_UNSUPPORTED;remember_failure(u"PCI",u"REQUIRE 64-BIT BAR",s);goto out;}
    s=cfg32(p,0x14,&bar1); if(EFI_ERROR(s)){remember_failure(u"PCI",u"READ BAR1",s);goto out;}
    s=mmio32(p,0,&cap0);reads++;if(EFI_ERROR(s)){remember_failure(u"CAPS",u"READ CAPLENGTH",s);goto out;} opbase=cap0&0xffU;
    { UINT16 ver=0; s=mmio16(p,2,&ver);reads++;if(EFI_ERROR(s)){remember_failure(u"CAPS",u"READ HCIVERSION",s);goto out;}
      if(ver<XHCI_MIN_VERSION){s=EFI_UNSUPPORTED;remember_failure(u"CAPS",u"VALIDATE HCIVERSION",s);goto out;}
      Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x BAR=%08x/%08x OPBASE=%02x\r\n",ver>>8,ver&255,id&65535,id>>16,bar0,bar1,opbase); }
    s=mmio32(p,4,&hcs1);reads++;if(EFI_ERROR(s)){remember_failure(u"CAPS",u"READ HCSPARAMS1",s);goto out;}
    s=mmio32(p,8,&hcs2);reads++;if(EFI_ERROR(s)){remember_failure(u"CAPS",u"READ HCSPARAMS2",s);goto out;}
    s=mmio32(p,0x10,&hcc1);reads++;if(EFI_ERROR(s)){remember_failure(u"CAPS",u"READ HCCPARAMS1",s);goto out;}
    s=mmio32(p,opbase,&cmd);reads++;if(EFI_ERROR(s)){remember_failure(u"CAPS",u"READ USBCMD",s);goto out;}
    s=mmio32(p,opbase+4,&status);reads++;if(EFI_ERROR(s)){remember_failure(u"CAPS",u"READ USBSTS",s);goto out;}
    maxslots=hcs1&0xffU;scratchpads=(((hcs2>>21)&0x1fU)<<5)|((hcs2>>27)&0x1fU);ac64=(hcc1&1U)!=0;
    s=mmio32(p,opbase+8,&pagesize_reg);reads++;if(EFI_ERROR(s)||!pagesize_reg){s=EFI_UNSUPPORTED;remember_failure(u"CAPS",u"READ PAGESIZE",s);goto out;}
    while(page_shift<32U&&!(pagesize_reg&(1U<<page_shift)))page_shift++;
    if(page_shift>=32U){s=EFI_UNSUPPORTED;remember_failure(u"CAPS",u"FIND PAGESIZE",s);goto out;}
    xhci_pagesize=(UINTN)1U<<(12U+page_shift);if(xhci_pagesize<4096U||xhci_pagesize>(1U<<20)){s=EFI_UNSUPPORTED;remember_failure(u"CAPS",u"VALIDATE PAGESIZE",s);goto out;}
    Print(u"CAPS: SLOTS=%u SCRATCHPADS=%u AC64=%u HCH=%u CNR=%u\r\n",maxslots,scratchpads,ac64,status&STS_HCH?1:0,status&STS_CNR?1:0);
    if(!maxslots||scratchpads>MAX_SCRATCHPADS){s=EFI_UNSUPPORTED;remember_failure(u"CAPS",u"VALIDATE CONTROLLER CAPS",s);goto out;}

    if(!(status&STS_HCH)){
        s=mmio_write32(p,opbase,cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE));writes++;if(EFI_ERROR(s)){remember_failure(u"HALT",u"CLEAR RUN/INTERRUPTS",s);goto out;}
        s=wait_status(p,opbase,STS_HCH,STS_HCH,1000,&status,&reads);if(EFI_ERROR(s)){remember_failure(u"HALT",u"WAIT HCH",s);goto out;}
    }
    controller_halted=TRUE;
    s=mmio_write32(p,opbase,(cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE))|CMD_RESET);writes++;if(EFI_ERROR(s)){remember_failure(u"RESET",u"SET RESET",s);goto out;}
    reset_done=FALSE;
    for(t=0;t<1000;t++){s=mmio32(p,opbase,&cmd);reads++;if(EFI_ERROR(s)){remember_failure(u"RESET",u"POLL RESET",s);goto out;}if(!(cmd&CMD_RESET)){reset_done=TRUE;break;}uefi_call_wrapper(BS->Stall,1,1000);}
    if(!reset_done){s=EFI_TIMEOUT;remember_failure(u"RESET",u"WAIT RESET CLEAR",s);goto out;}
    s=wait_status(p,opbase,STS_CNR,0,10000,&status,&reads);if(EFI_ERROR(s)){remember_failure(u"RESET",u"WAIT CNR CLEAR",s);goto out;}
    if(!(status&STS_HCH)){controller_halted=FALSE;s=EFI_DEVICE_ERROR;remember_failure(u"RESET",u"VERIFY HALTED",s);goto out;}

    dcbaa_pages=((maxslots+1U)*sizeof(UINT64)+4095U)/4096U;
    spa_pages=(scratchpads*sizeof(UINT64)+xhci_pagesize-1U)/xhci_pagesize;if(!spa_pages)spa_pages=1;
    total_bytes=dcbaa_pages*4096U+(xhci_pagesize-1U)+spa_pages*xhci_pagesize+4096U+8192U+4096U;
    common_pages=(total_bytes+4095U)/4096U;if(common_pages<COMMON_PAGES)common_pages=COMMON_PAGES;
    if(common_pages>256U){s=EFI_OUT_OF_RESOURCES;remember_failure(u"DMA",u"BOUND COMMON BUFFER",s);goto out;}
    s=dma_alloc(p,common_pages,&common,&common_dev,&common_map);if(EFI_ERROR(s)){remember_failure(u"DMA",u"ALLOC/MAP COMMON",s);goto out;}common_ok=TRUE;
    dcbaa_dev=common_dev;spa_dev=(common_dev+dcbaa_pages*4096U+xhci_pagesize-1U)&~((UINT64)xhci_pagesize-1ULL);
    crcr_dev=spa_dev+spa_pages*xhci_pagesize;event_dev=crcr_dev+4096U;erst_dev=event_dev+8192U;
    if((dcbaa_dev&63ULL)||(spa_dev&63ULL)||(crcr_dev&63ULL)||(event_dev&63ULL)||(erst_dev&63ULL)){s=EFI_BAD_BUFFER_SIZE;remember_failure(u"DMA",u"VALIDATE ALIGNMENT",s);goto teardown;}
    if(!ac64&&(dcbaa_dev>0xffffffffULL||crcr_dev+4095ULL>0xffffffffULL||event_dev+4095ULL>0xffffffffULL||erst_dev+15ULL>0xffffffffULL)){s=EFI_BAD_BUFFER_SIZE;remember_failure(u"DMA",u"VALIDATE 32-BIT DMA",s);goto teardown;}
    dcbaa=(UINT64*)common;erst=(UINT64*)((UINT8*)common+(erst_dev-common_dev));
    s=uefi_call_wrapper(BS->SetMem,3,common,common_pages*4096U,0);if(EFI_ERROR(s)){remember_failure(u"DMA",u"CLEAR COMMON",s);goto teardown;}

    if(scratchpads){
        UINTN bytes=scratchpads*xhci_pagesize, pages=(bytes+4095U)/4096U+1U, addr,aligned,delta,mapbytes;
        if(pages>256U){s=EFI_OUT_OF_RESOURCES;remember_failure(u"SCRATCHPAD",u"BOUND ALLOCATION",s);goto teardown;}
        scratch_block_pages=pages;s=uefi_call_wrapper(p->AllocateBuffer,6,p,AllocateAnyPages,EfiBootServicesData,pages,&scratch_block,0);if(EFI_ERROR(s)){remember_failure(u"SCRATCHPAD",u"ALLOCATE",s);goto teardown;}
        s=uefi_call_wrapper(BS->SetMem,3,scratch_block,pages*4096U,0);if(EFI_ERROR(s)){remember_failure(u"SCRATCHPAD",u"CLEAR",s);goto teardown;}
        addr=(UINTN)scratch_block;aligned=(addr+xhci_pagesize-1U)&~(xhci_pagesize-1U);delta=aligned-addr;if(delta>pages*4096U||bytes>pages*4096U-delta){s=EFI_BAD_BUFFER_SIZE;remember_failure(u"SCRATCHPAD",u"ALIGN RANGE",s);goto teardown;}
        scratch_aligned=(VOID*)aligned;mapbytes=bytes;s=uefi_call_wrapper(p->Map,6,p,EfiPciIoOperationBusMasterCommonBuffer,scratch_aligned,&mapbytes,&scratch_aligned_dev,&scratch_map);if(EFI_ERROR(s)||mapbytes!=bytes){if(!EFI_ERROR(s)&&scratch_map)uefi_call_wrapper(p->Unmap,2,p,scratch_map);scratch_map=NULL;s=EFI_ERROR(s)?s:EFI_DEVICE_ERROR;remember_failure(u"SCRATCHPAD",u"MAP",s);goto teardown;}
        s=uefi_call_wrapper(BS->SetMem,3,scratch_aligned,bytes,0);if(EFI_ERROR(s)){remember_failure(u"SCRATCHPAD",u"CLEAR MAPPED",s);goto teardown;}
        if((scratch_aligned_dev&((UINT64)xhci_pagesize-1ULL))!=0||(!ac64&&scratch_aligned_dev+bytes-1ULL>0xffffffffULL)){s=EFI_BAD_BUFFER_SIZE;remember_failure(u"SCRATCHPAD",u"VALIDATE DEVICE ADDRESS",s);goto teardown;}
        for(j=0;j<scratchpads;j++)scratch_dev[j]=scratch_aligned_dev+(UINT64)j*xhci_pagesize;
        {UINT64 *spa=(UINT64*)((UINT8*)common+(spa_dev-common_dev));for(j=0;j<scratchpads;j++)spa[j]=scratch_dev[j];dcbaa[0]=spa_dev;}
    }

    {UINT32 *link=(UINT32*)((UINT8*)common+(crcr_dev-common_dev)+4080U);link[0]=(UINT32)crcr_dev;link[1]=(UINT32)(crcr_dev>>32);link[2]=TRB_LINK_TYPE;link[3]=TRB_LINK_TOGGLE;}
    ((UINT64*)((UINT8*)common+(erst_dev-common_dev)))[0]=event_dev;((UINT32*)((UINT8*)common+(erst_dev-common_dev)))[2]=ERST_SEGMENT_TRBS;

    s=mmio_write32(p,opbase+0x38,1);writes++;if(EFI_ERROR(s)){remember_failure(u"CONFIG",u"WRITE",s);goto teardown;}
    s=mmio_write64_split(p,opbase+0x30,dcbaa_dev);writes++;if(EFI_ERROR(s)){remember_failure(u"DCBAA",u"WRITE",s);goto teardown;}
    s=mmio64_split(p,opbase+0x30,&dcbaa_rd);reads+=2;if(EFI_ERROR(s)){remember_failure(u"DCBAA",u"READBACK",s);goto teardown;}
    if(dcbaa_rd!=dcbaa_dev){s=EFI_DEVICE_ERROR;remember_failure(u"DCBAA",u"VERIFY READBACK",s);goto teardown;}
    s=mmio_write64_split(p,opbase+0x18,crcr_dev|CRCR_RCS);writes++;if(EFI_ERROR(s)){remember_failure(u"CRCR",u"WRITE",s);goto teardown;}
    s=mmio32(p,0x18,&rtsoff);reads++;if(EFI_ERROR(s)){remember_failure(u"EVENT-RING",u"READ RTSOFF",s);goto teardown;}
    {UINT32 ib=rtsoff+0x20;
        s=mmio_write32(p,ib+0x00,0);writes++;if(EFI_ERROR(s)){remember_failure(u"INTERRUPTER",u"DISABLE IMAN",s);goto teardown;}
        s=mmio_write32(p,ib+0x04,0);writes++;if(EFI_ERROR(s)){remember_failure(u"INTERRUPTER",u"CLEAR IMOD",s);goto teardown;}
        s=mmio_write32(p,ib+0x08,1);writes++;if(EFI_ERROR(s)){remember_failure(u"EVENT-RING",u"WRITE ERSTSZ",s);goto teardown;}
        s=mmio_write64_split(p,ib+0x10,erst_dev);writes++;if(EFI_ERROR(s)){remember_failure(u"EVENT-RING",u"WRITE ERSTBA",s);goto teardown;}
        s=mmio_write64_split(p,ib+0x18,event_dev);writes++;if(EFI_ERROR(s)){remember_failure(u"EVENT-RING",u"WRITE ERDP",s);goto teardown;}
        s=mmio32(p,ib,&iman);reads++;if(EFI_ERROR(s)){remember_failure(u"INTERRUPTER",u"READ IMAN",s);goto teardown;}
        if(iman&IMAN_IE){s=EFI_DEVICE_ERROR;remember_failure(u"INTERRUPTER",u"VERIFY IE CLEAR",s);goto teardown;}
        s=mmio64_split(p,ib+0x10,&erstba_rd);reads+=2;if(EFI_ERROR(s)){remember_failure(u"EVENT-RING",u"READ ERSTBA",s);goto teardown;}
        s=mmio64_split(p,ib+0x18,&erdp_rd);reads+=2;if(EFI_ERROR(s)){remember_failure(u"EVENT-RING",u"READ ERDP",s);goto teardown;}
        if((erstba_rd&ERST_ADDR_MASK)!=(erst_dev&ERST_ADDR_MASK)||(erdp_rd&ERDP_ADDR_MASK)!=(event_dev&ERDP_ADDR_MASK)){s=EFI_DEVICE_ERROR;remember_failure(u"EVENT-RING",u"VERIFY POINTERS",s);goto teardown;}
    }
    s=mmio32(p,opbase+0x38,&config);reads++;if(EFI_ERROR(s)||((config&0xffU)!=1)){s=EFI_DEVICE_ERROR;remember_failure(u"PRE-RUN",u"VERIFY CONFIG",s);goto teardown;}
    s=mmio32(p,opbase,&cmd);reads++;if(EFI_ERROR(s)){remember_failure(u"PRE-RUN",u"READ USBCMD",s);goto teardown;}
    s=mmio32(p,opbase+4,&status);reads++;if(EFI_ERROR(s)){remember_failure(u"PRE-RUN",u"READ USBSTS",s);goto teardown;}
    if((cmd&(CMD_RUN|CMD_INTE|CMD_HSEE))||(status&STS_CNR)||!(status&STS_HCH)){s=EFI_DEVICE_ERROR;remember_failure(u"PRE-RUN",u"VERIFY HALTED/INTERRUPTS",s);goto teardown;}
    Print(u"PRE-RUN: HCH=1 CNR=0 CONFIG=1 IMAN.IE=0 CPU-INT=0\r\n");

    s=mmio_write32(p,opbase,(cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE))|CMD_RUN);writes++;if(EFI_ERROR(s)){remember_failure(u"RUN",u"SET RUN",s);goto teardown;}
    controller_halted=FALSE;
    s=wait_status(p,opbase,STS_HCH,0,2000,&status,&reads);if(EFI_ERROR(s)){remember_failure(u"RUN",u"WAIT HCH CLEAR",s);goto running_failure;}
    Print(u"RUN: HCH=0 PASS\r\n");
    uefi_call_wrapper(BS->Stall,1,observation_us);
    s=mmio32(p,opbase+4,&status);reads++;if(EFI_ERROR(s)){remember_failure(u"RUN",u"READ USBSTS",s);goto stop;}
    if(status&STS_HSE){remember_failure(u"RUN",u"HOST SYSTEM ERROR",EFI_DEVICE_ERROR);s=EFI_DEVICE_ERROR;}

stop:
    ts=mmio32(p,opbase,&cmd);reads++;if(EFI_ERROR(ts)){remember_failure(u"HALT",u"READ USBCMD",ts);if(!EFI_ERROR(s))s=ts;}
    if(!EFI_ERROR(ts)){
        ts=mmio_write32(p,opbase,cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE));writes++;if(EFI_ERROR(ts)){remember_failure(u"HALT",u"CLEAR RUN",ts);if(!EFI_ERROR(s))s=ts;}
    }
    ts=wait_status(p,opbase,STS_HCH,STS_HCH,2000,&status,&reads);if(EFI_ERROR(ts)){remember_failure(u"HALT",u"WAIT HCH",ts);if(!EFI_ERROR(s))s=ts;}
    if(!EFI_ERROR(ts))controller_halted=TRUE;
    if(!controller_halted) goto running_failure;
    if(!EFI_ERROR(s))Print(u"HALT: HCH=1 PASS\r\n");
    ts=mmio32(p,rtsoff+0x20,&iman);reads++;if(!EFI_ERROR(ts)&&((iman&IMAN_IP)!=0))events=1;
    if(!EFI_ERROR(ts)&&events)Print(u"EVENT-RING: activity observed (not consumed)\r\n");
    else if(EFI_ERROR(ts))remember_failure(u"POST-RUN",u"READ IMAN",ts);
    else Print(u"EVENT-RING: no event pending\r\n");
    ts=mmio32(p,opbase,&cmd);reads++;if(EFI_ERROR(ts)){remember_failure(u"HALT",u"READ USBCMD",ts);if(!EFI_ERROR(s))s=ts;}
    ts=mmio32(p,opbase+4,&status);reads++;if(EFI_ERROR(ts)){remember_failure(u"HALT",u"READ USBSTS",ts);if(!EFI_ERROR(s))s=ts;}
    if(!EFI_ERROR(s)&&((cmd&CMD_RUN)||(status&STS_CNR)||!(status&STS_HCH))){s=EFI_DEVICE_ERROR;remember_failure(u"HALT",u"VERIFY HALTED",s);}
    if(controller_halted){
        reset_done=FALSE;
        ts=mmio_write32(p,opbase,CMD_RESET);writes++;if(EFI_ERROR(ts)){remember_failure(u"RESET",u"SET RESET",ts);if(!EFI_ERROR(s))s=ts;}
        if(!EFI_ERROR(ts)){
            for(t=0;t<1000;t++){ts=mmio32(p,opbase,&cmd);reads++;if(EFI_ERROR(ts)){remember_failure(u"RESET",u"POLL RESET",ts);break;}if(!(cmd&CMD_RESET)){reset_done=TRUE;break;}uefi_call_wrapper(BS->Stall,1,1000);}
            if(!reset_done&&!EFI_ERROR(ts)){ts=EFI_TIMEOUT;remember_failure(u"RESET",u"WAIT RESET CLEAR",ts);}
            if(!EFI_ERROR(ts)){
                ts=wait_status(p,opbase,STS_CNR,0,10000,&status,&reads);if(EFI_ERROR(ts))remember_failure(u"RESET",u"WAIT CNR CLEAR",ts);
                if(!EFI_ERROR(ts)&&!(status&STS_HCH)){controller_halted=FALSE;ts=EFI_DEVICE_ERROR;remember_failure(u"RESET",u"VERIFY HALTED",ts);}
            }
            if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;
        }
        if(!EFI_ERROR(s))Print(u"RESET: CNR=0 HCH=1 PASS\r\n");
    }
    if(!EFI_ERROR(s))Print(u"V30 CONTROLLER START: PASS\r\n");
    goto teardown;

running_failure:
    remember_failure(u"FATAL",u"CONTROLLER NOT HALTED",EFI_DEVICE_ERROR);
    fatal_dma_running();

teardown:
    {EFI_STATUS original=s;
      if(common_ok&&!controller_halted) fatal_dma_running();
      if(p&&opbase){
        ts=mmio_write64_split(p,opbase+0x18,0);writes+=EFI_ERROR(ts)?0:1;if(EFI_ERROR(ts))remember_failure(u"CLEANUP",u"CLEAR CRCR",ts);
        ts=mmio_write64_split(p,opbase+0x30,0);writes+=EFI_ERROR(ts)?0:1;if(EFI_ERROR(ts))remember_failure(u"CLEANUP",u"CLEAR DCBAAP",ts);
        ts=mmio_write32(p,opbase+0x38,0);writes+=EFI_ERROR(ts)?0:1;if(EFI_ERROR(ts))remember_failure(u"CLEANUP",u"CLEAR CONFIG",ts);
        if(rtsoff){ts=mmio_write64_split(p,rtsoff+0x20+0x10,0);writes+=EFI_ERROR(ts)?0:1;if(EFI_ERROR(ts))remember_failure(u"CLEANUP",u"CLEAR ERSTBA",ts);
          ts=mmio_write64_split(p,rtsoff+0x20+0x18,0);writes+=EFI_ERROR(ts)?0:1;if(EFI_ERROR(ts))remember_failure(u"CLEANUP",u"CLEAR ERDP",ts);
          ts=mmio_write32(p,rtsoff+0x20+0x08,0);writes+=EFI_ERROR(ts)?0:1;if(EFI_ERROR(ts))remember_failure(u"CLEANUP",u"CLEAR ERSTSZ",ts);}
      }
      if(scratch_map){ts=uefi_call_wrapper(p->Unmap,2,p,scratch_map);if(EFI_ERROR(ts))remember_failure(u"CLEANUP",u"UNMAP SCRATCHPAD",ts);scratch_map=NULL;}
      if(scratch_block){ts=uefi_call_wrapper(p->FreeBuffer,3,p,scratch_block_pages,scratch_block);if(EFI_ERROR(ts))remember_failure(u"CLEANUP",u"FREE SCRATCHPAD",ts);scratch_block=NULL;}
      if(common_ok){ts=dma_free(p,common_pages,common,common_map);if(EFI_ERROR(ts))remember_failure(u"CLEANUP",u"FREE COMMON DMA",ts);common=NULL;common_map=NULL;common_ok=FALSE;}
      s=original;
    }
out:
    if(hs)FreePool(hs);
    if(EFI_ERROR(s)&&!EFI_ERROR(fail_status))remember_failure(u"UNKNOWN",u"UNCLASSIFIED",s);
    if(!EFI_ERROR(s)&&EFI_ERROR(fail_status))s=fail_status;
    finish(s,reads,writes,events);
    return s;
}

/* Provenance test: V30 controller-start gate. */
