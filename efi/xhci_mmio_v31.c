#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define XHCI_MIN_VERSION 0x0100U
#define CMD_RUN 0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE 0x00000004U
#define CMD_HSEE 0x00000008U
#define STS_HCH 0x00000001U
#define STS_HSE 0x00000004U
#define STS_CNR 0x00000800U
#define IMAN_IE 0x00000002U
#define IMAN_IP 0x00000001U
#define CRCR_RCS 0x00000001ULL
#define TRB_CYCLE 0x00000001U
#define TRB_TYPE_SHIFT 10U
#define TRB_TYPE_MASK 0x0000fc00U
#define TRB_ENABLE_SLOT 9U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 0x00000002U
#define TRB_COMMAND_COMPLETION 33U
#define CC_SUCCESS 1U
#define ERST_ADDR_MASK 0xffffffffffffffc0ULL
#define ERDP_ADDR_MASK 0xfffffffffffffff0ULL
#define CMD_TRBS 256U
#define EVENT_TRBS 16U
#define MAX_SCRATCHPADS 1024U

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static const CHAR16 *fail_stage = u"NONE";
static const CHAR16 *fail_op = u"NONE";
static EFI_STATUS fail_status = EFI_SUCCESS;

struct dma_obj { VOID *host; VOID *map; EFI_PHYSICAL_ADDRESS dev; UINTN pages; BOOLEAN live; };

static void fail(const CHAR16 *stage, const CHAR16 *op, EFI_STATUS s) {
    if (!EFI_ERROR(fail_status)) { fail_stage = stage; fail_op = op; fail_status = s; }
}
static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,o,1,v);
}
static EFI_STATUS mr32(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,v);
}
static EFI_STATUS mr16(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT16 *v) {
    return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint16,0,(UINT64)o,1,v);
}
static EFI_STATUS mw32(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT32 v) {
    return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,&v);
}
static EFI_STATUS mr64(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT64 *v) {
    UINT32 lo,hi; EFI_STATUS s=mr32(p,o,&lo); if(EFI_ERROR(s))return s;
    s=mr32(p,o+4,&hi); if(EFI_ERROR(s))return s; *v=((UINT64)hi<<32)|lo; return EFI_SUCCESS;
}
static EFI_STATUS mw64(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT64 v) {
    EFI_STATUS s=mw32(p,o,(UINT32)v); if(EFI_ERROR(s))return s; return mw32(p,o+4,(UINT32)(v>>32));
}
static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL *p, UINTN pages, struct dma_obj *d) {
    EFI_STATUS s; UINTN bytes=pages*4096U, map_bytes=bytes;
    d->host=NULL; d->map=NULL; d->dev=0; d->pages=pages; d->live=FALSE;
    if(!pages || pages>((UINTN)-1)/4096U)return EFI_BAD_BUFFER_SIZE;
    s=uefi_call_wrapper(p->AllocateBuffer,6,p,AllocateAnyPages,EfiBootServicesData,pages,&d->host,0);
    if(EFI_ERROR(s))return s;
    s=uefi_call_wrapper(BS->SetMem,3,d->host,bytes,0);
    if(EFI_ERROR(s)){uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host);d->host=NULL;return s;}
    s=uefi_call_wrapper(p->Map,6,p,EfiPciIoOperationBusMasterCommonBuffer,d->host,&map_bytes,&d->dev,&d->map);
    if(EFI_ERROR(s)||map_bytes!=bytes){
        EFI_STATUS x=EFI_ERROR(s)?s:EFI_DEVICE_ERROR;
        if(!EFI_ERROR(s)&&d->map)uefi_call_wrapper(p->Unmap,2,p,d->map);
        uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host); d->host=NULL;d->map=NULL;d->dev=0;return x;
    }
    d->live=TRUE; return EFI_SUCCESS;
}
static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL *p, struct dma_obj *d) {
    EFI_STATUS s=EFI_SUCCESS,t;
    if(!d->live)return EFI_SUCCESS;
    if(d->map){t=uefi_call_wrapper(p->Unmap,2,p,d->map);if(EFI_ERROR(t))s=t;}
    if(d->host){t=uefi_call_wrapper(p->FreeBuffer,3,p,d->pages,d->host);if(EFI_ERROR(t)&&!EFI_ERROR(s))s=t;}
    d->host=NULL;d->map=NULL;d->dev=0;d->live=FALSE;return s;
}
static EFI_STATUS wait_hch(EFI_PCI_IO_PROTOCOL *p, UINT32 op, BOOLEAN halt, UINTN loops, UINT32 *st, UINT32 *reads) {
    UINTN i; EFI_STATUS s; UINT32 want=halt?STS_HCH:0;
    for(i=0;i<loops;i++){s=mr32(p,op+4,st);(*reads)++;if(EFI_ERROR(s))return s;if((*st&STS_HCH)==want)return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;
}
static EFI_STATUS reset_xhci(EFI_PCI_IO_PROTOCOL *p, UINT32 op, UINT32 *cmd, UINT32 *st, UINT32 *reads, UINT32 *writes) {
    EFI_STATUS s; UINTN i;
    s=mr32(p,op,cmd);(*reads)++;if(EFI_ERROR(s))return s;*cmd&=~(CMD_RUN|CMD_INTE|CMD_HSEE);*cmd|=CMD_RESET;
    s=mw32(p,op,*cmd);(*writes)++;if(EFI_ERROR(s))return s;
    for(i=0;i<1000;i++){s=mr32(p,op,cmd);(*reads)++;if(EFI_ERROR(s))return s;if(!(*cmd&CMD_RESET))break;uefi_call_wrapper(BS->Stall,1,1000);}
    if(*cmd&CMD_RESET)return EFI_TIMEOUT;
    for(i=0;i<10000;i++){s=mr32(p,op+4,st);(*reads)++;if(EFI_ERROR(s))return s;if(!(*st&STS_CNR))return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;
}
static EFI_STATUS slot_type(EFI_PCI_IO_PROTOCOL *p, UINT32 hcc, UINT32 *type, UINT32 *reads) {
    UINT32 off=((hcc>>16)&0xffffU)*4U, h, next; UINTN n=0; EFI_STATUS s;
    while(off && n++<64){s=mr32(p,off,&h);(*reads)++;if(EFI_ERROR(s))return s;if((h&0xffU)==2U){s=mr32(p,off+12U,&h);(*reads)++;if(EFI_ERROR(s))return s;*type=h&0x1fU;return EFI_SUCCESS;}next=((h>>8)&0xffU)*4U;if(!next)break;off+=next;}
    *type=0;return EFI_SUCCESS;
}
static void fatal_running(void){Print(u"\r\nFATAL: XHCI NOT CONFIRMED HALTED\r\nDMA MAPPINGS RETAINED / NO FREE / MANUAL RECOVERY REQUIRED\r\n");for(;;)uefi_call_wrapper(BS->Stall,1,1000000);}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs=NULL; EFI_PCI_IO_PROTOCOL *p=NULL; EFI_STATUS s=EFI_SUCCESS,ts; UINTN n=0,i;
    UINT32 id=0,cls=0,bar0=0,bar1=0,cap=0,hcs1=0,hcs2=0,hcc=0,op=0,db=0,rt=0,ir=0;
    UINT32 cmd=0,status=0,iman=0,maxslots=0,scratchpads=0,ps=0,slot=0,reads=0,writes=0;
    UINT32 event_type=0,completion=0,event_dw0=0,event_dw1=0,event_dw2=0,event_dw3=0; UINT64 event_ptr=0;
    UINT32 ext_reads=0; UINTN shift=0,xpage=0,scratch_pages=0;
    BOOLEAN halted=FALSE,submitted=FALSE,consumed=FALSE,dma_live=FALSE;
    UINT64 *dcbaa=NULL,*spa=NULL,*cr=NULL,*erst=NULL,*ev=NULL; UINT64 erst_rd=0,erdp_rd=0;
    struct dma_obj dcbaa_d={0},spa_d={0},cr_d={0},erst_d={0},ev_d={0}; struct dma_obj *sb=NULL;

    InitializeLib(image,st);
    Print(u"TOSHIBA xHCI V31 / ENABLE SLOT COMMAND + COMPLETION\r\nUEFI DMA MAP -> INIT -> RUN -> ENABLE SLOT -> POLL EVENT -> RESET\r\nONE COMMAND / ONE DOORBELL / NO CPU INTERRUPTS / NO USB TRANSFER\r\n");
    s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);if(EFI_ERROR(s)){fail(u"PCI",u"LOCATE",s);goto out;}
    for(i=0;i<n;i++){EFI_PCI_IO_PROTOCOL *q=NULL;if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,(void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL)))continue;if(EFI_ERROR(cfg32(q,8,&cls))||EFI_ERROR(cfg32(q,0,&id)))continue;if(((cls>>24)&255)==0x0c&&((cls>>16)&255)==3&&((cls>>8)&255)==0x30){p=q;break;}}
    if(!p){s=EFI_NOT_FOUND;fail(u"PCI",u"FIND XHCI",s);goto out;}
    s=cfg32(p,0x10,&bar0);if(EFI_ERROR(s)){fail(u"PCI",u"BAR0",s);goto out;}if((bar0&1U)||((bar0>>1)&3U)!=2U){s=EFI_UNSUPPORTED;fail(u"PCI",u"64-BIT BAR",s);goto out;}
    s=cfg32(p,0x14,&bar1);if(EFI_ERROR(s)){fail(u"PCI",u"BAR1",s);goto out;}s=mr32(p,0,&cap);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"CAPLENGTH",s);goto out;}op=cap&0xffU;
    {UINT16 ver=0;s=mr16(p,2,&ver);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"HCIVERSION",s);goto out;}if(ver<XHCI_MIN_VERSION){s=EFI_UNSUPPORTED;fail(u"CAPS",u"VERSION",s);goto out;}Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x BAR=%08x/%08x OPBASE=%02x\r\n",ver>>8,ver&255,id&65535,id>>16,bar0,bar1,op);}
    s=mr32(p,4,&hcs1);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"HCSPARAMS1",s);goto out;}s=mr32(p,8,&hcs2);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"HCSPARAMS2",s);goto out;}s=mr32(p,0x10,&hcc);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"HCCPARAMS1",s);goto out;}s=mr32(p,op+4,&status);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"USBSTS",s);goto out;}
    maxslots=hcs1&255U;scratchpads=(((hcs2>>21)&31U)<<5)|((hcs2>>27)&31U);s=mr32(p,op+8,&ps);reads++;if(EFI_ERROR(s)||!ps){s=EFI_UNSUPPORTED;fail(u"CAPS",u"PAGESIZE",s);goto out;}while(shift<32U&&!(ps&(1U<<shift)))shift++;if(shift>=32U){s=EFI_UNSUPPORTED;fail(u"CAPS",u"PAGE BIT",s);goto out;}xpage=(UINTN)1U<<(12U+shift);if(xpage>0x100000U){s=EFI_UNSUPPORTED;fail(u"CAPS",u"PAGE SIZE",s);goto out;}scratch_pages=xpage/4096U;
    Print(u"CAPS: SLOTS=%u SCRATCHPADS=%u PAGESIZE=%u HCH=%u CNR=%u\r\n",maxslots,scratchpads,(UINT32)xpage,status&STS_HCH?1:0,status&STS_CNR?1:0);if(!maxslots||scratchpads>MAX_SCRATCHPADS){s=EFI_UNSUPPORTED;fail(u"CAPS",u"LIMITS",s);goto out;}
    s=mr32(p,0x14,&db);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"DBOFF",s);goto out;}db&=~3U;s=mr32(p,0x18,&rt);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"RTSOFF",s);goto out;}rt&=~0x1fU;ir=rt+0x20U;
    s=slot_type(p,hcc,&slot,&ext_reads);reads+=ext_reads;if(EFI_ERROR(s)){fail(u"CAPS",u"SLOT TYPE",s);goto out;}Print(u"CAPS: DBOFF=%08x RTSOFF=%08x SLOT-TYPE=%u\r\n",db,rt,slot);
    s=mr32(p,op,&cmd);reads++;if(EFI_ERROR(s)){fail(u"HALT",u"USBCMD",s);goto out;}if(!(status&STS_HCH)){s=mw32(p,op,cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE));writes++;if(EFI_ERROR(s)){fail(u"HALT",u"STOP",s);goto out;}s=wait_hch(p,op,TRUE,1000,&status,&reads);if(EFI_ERROR(s)){fail(u"HALT",u"HCH",s);goto out;}}halted=TRUE;
    s=reset_xhci(p,op,&cmd,&status,&reads,&writes);if(EFI_ERROR(s)){fail(u"RESET",u"RESET/CNR",s);goto out;}
    if(!scratchpads)scratch_pages=1;
    s=dma_alloc(p,1,&dcbaa_d);if(EFI_ERROR(s)){fail(u"DMA",u"DCBAA",s);goto out;}s=dma_alloc(p,1,&spa_d);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCH ARRAY",s);goto out;}s=dma_alloc(p,1,&cr_d);if(EFI_ERROR(s)){fail(u"DMA",u"COMMAND RING",s);goto out;}s=dma_alloc(p,1,&ev_d);if(EFI_ERROR(s)){fail(u"DMA",u"EVENT RING",s);goto out;}s=dma_alloc(p,1,&erst_d);if(EFI_ERROR(s)){fail(u"DMA",u"ERST",s);goto out;}dma_live=TRUE;
    if(scratchpads){s=uefi_call_wrapper(BS->AllocatePool,3,EfiBootServicesData,scratchpads*sizeof(struct dma_obj),(void**)&sb);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCH DESCRIPTORS",s);goto out;}uefi_call_wrapper(BS->SetMem,3,sb,scratchpads*sizeof(struct dma_obj),0);for(i=0;i<scratchpads;i++){s=dma_alloc(p,scratch_pages,&sb[i]);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCHPAD",s);goto out;}if(sb[i].dev&((UINT64)xpage-1ULL)){s=EFI_BAD_BUFFER_SIZE;fail(u"DMA",u"SCRATCH ALIGN",s);goto out;}}}
    dcbaa=(UINT64*)dcbaa_d.host;spa=(UINT64*)spa_d.host;cr=(UINT64*)cr_d.host;ev=(UINT64*)ev_d.host;erst=(UINT64*)erst_d.host;dcbaa[0]=scratchpads?spa_d.dev:0;for(i=0;i<scratchpads;i++)spa[i]=sb[i].dev;
    cr[0]=0;cr[1]=0;cr[2]=0;cr[3]=TRB_CYCLE|(TRB_ENABLE_SLOT<<TRB_TYPE_SHIFT)|((slot&31U)<<16);cr[(CMD_TRBS-1)*4+0]=(UINT32)cr_d.dev;cr[(CMD_TRBS-1)*4+1]=(UINT32)(cr_d.dev>>32);cr[(CMD_TRBS-1)*4+2]=0;cr[(CMD_TRBS-1)*4+3]=TRB_CYCLE|TRB_LINK_TOGGLE|(TRB_LINK<<TRB_TYPE_SHIFT);
    erst[0]=ev_d.dev;erst[1]=0;erst[2]=EVENT_TRBS;erst[3]=0;
    s=mw64(p,op+0x30,dcbaa_d.dev);writes+=2;if(EFI_ERROR(s)){fail(u"INIT",u"DCBAAP",s);goto out;}s=mw32(p,op+0x38,1);writes++;if(EFI_ERROR(s)){fail(u"INIT",u"CONFIG",s);goto out;}s=mw64(p,op+0x18,cr_d.dev|CRCR_RCS);writes+=2;if(EFI_ERROR(s)){fail(u"INIT",u"CRCR",s);goto out;}s=mw32(p,ir+8,1);writes++;if(EFI_ERROR(s)){fail(u"INIT",u"ERSTSZ",s);goto out;}s=mw64(p,ir+0x10,erst_d.dev&ERST_ADDR_MASK);writes+=2;if(EFI_ERROR(s)){fail(u"INIT",u"ERSTBA",s);goto out;}s=mw64(p,ir+0x18,ev_d.dev&ERDP_ADDR_MASK);writes+=2;if(EFI_ERROR(s)){fail(u"INIT",u"ERDP",s);goto out;}
    s=mr64(p,ir+0x10,&erst_rd);reads+=2;if(EFI_ERROR(s)||(erst_rd&ERST_ADDR_MASK)!=(erst_d.dev&ERST_ADDR_MASK)){s=EFI_DEVICE_ERROR;fail(u"INIT",u"VERIFY ERSTBA",s);goto out;}s=mr64(p,ir+0x18,&erdp_rd);reads+=2;if(EFI_ERROR(s)||(erdp_rd&ERDP_ADDR_MASK)!=(ev_d.dev&ERDP_ADDR_MASK)){s=EFI_DEVICE_ERROR;fail(u"INIT",u"VERIFY ERDP",s);goto out;}
    s=mr32(p,ir,&iman);reads++;if(EFI_ERROR(s)){fail(u"RUN",u"IMAN",s);goto out;}s=mw32(p,ir,(iman&~IMAN_IE)|(iman&IMAN_IP));writes++;if(EFI_ERROR(s)){fail(u"RUN",u"DISABLE IRQ",s);goto out;}s=mr32(p,op,&cmd);reads++;if(EFI_ERROR(s)){fail(u"RUN",u"USBCMD",s);goto out;}s=mw32(p,op,(cmd&~(CMD_INTE|CMD_HSEE))|CMD_RUN);writes++;if(EFI_ERROR(s)){fail(u"RUN",u"START",s);goto out;}halted=FALSE;s=wait_hch(p,op,FALSE,1000,&status,&reads);if(EFI_ERROR(s)){fail(u"RUN",u"HCH CLEAR",s);goto out;}
    s=mw32(p,db,0);writes++;if(EFI_ERROR(s)){fail(u"COMMAND",u"DOORBELL",s);goto out;}submitted=TRUE;Print(u"ENABLE SLOT: DOORBELL=0 COMMAND-TRB=%016lx SLOT-TYPE=%u\r\n",cr_d.dev,slot);
    for(i=0;i<10000;i++){
        UINT32 *e=(UINT32*)ev_d.host; event_dw0=e[0];event_dw1=e[1];event_dw2=e[2];event_dw3=e[3];
        if((event_dw3&TRB_CYCLE)&&((event_dw3>>TRB_TYPE_SHIFT)&63U)==TRB_COMMAND_COMPLETION)break;
        uefi_call_wrapper(BS->Stall,1,1000);
    }
    event_type=(event_dw3&TRB_TYPE_MASK)>>TRB_TYPE_SHIFT;completion=(event_dw2>>24)&255U;slot_id=(event_dw3>>24)&255U;event_ptr=((UINT64)event_dw1<<32)|event_dw0;
    if(event_type!=TRB_COMMAND_COMPLETION||(event_dw3&TRB_CYCLE)==0){s=EFI_TIMEOUT;fail(u"EVENT",u"COMMAND COMPLETION",s);goto out;}if(completion!=CC_SUCCESS||!slot_id||slot_id>maxslots||event_ptr!=cr_d.dev){s=EFI_DEVICE_ERROR;fail(u"EVENT",u"VALIDATE COMPLETION",s);goto out;}consumed=TRUE;Print(u"COMMAND COMPLETION: TYPE=%u CODE=%u SLOT=%u PTR=%016lx PASS\r\n",event_type,completion,slot_id,event_ptr);
    s=mw64(p,ir+0x18,(ev_d.dev+16U)&ERDP_ADDR_MASK);writes+=2;if(EFI_ERROR(s)){fail(u"EVENT",u"ADVANCE ERDP",s);goto out;}s=mr32(p,ir,&iman);reads++;if(EFI_ERROR(s)){fail(u"EVENT",u"IMAN AFTER",s);goto out;}if(iman&IMAN_IP){s=mw32(p,ir,IMAN_IP);writes++;if(EFI_ERROR(s)){fail(u"EVENT",u"ACK IP",s);goto out;}}
    s=mr32(p,op,&cmd);reads++;if(EFI_ERROR(s)){fail(u"HALT",u"USBCMD",s);goto out;}s=mw32(p,op,cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE));writes++;if(EFI_ERROR(s)){fail(u"HALT",u"STOP",s);goto out;}s=wait_hch(p,op,TRUE,10000,&status,&reads);if(EFI_ERROR(s)){fail(u"HALT",u"CONFIRM HCH",s);fatal_running();}halted=TRUE;
    s=reset_xhci(p,op,&cmd,&status,&reads,&writes);if(EFI_ERROR(s)){fail(u"RESET",u"RECOVERY",s);goto out;}
    s=mw64(p,op+0x18,0);writes+=2;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"CRCR",s);goto out;}s=mw64(p,op+0x30,0);writes+=2;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"DCBAAP",s);goto out;}s=mw32(p,op+0x38,0);writes++;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"CONFIG",s);goto out;}s=mw32(p,ir+8,0);writes++;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"ERSTSZ",s);goto out;}s=mw64(p,ir+0x10,0);writes+=2;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"ERSTBA",s);goto out;}s=mw64(p,ir+0x18,0);writes+=2;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"ERDP",s);goto out;}
    
out:
    if(EFI_ERROR(s)&&submitted&&!halted)fatal_running();
    if(dma_live&&halted){if(sb)for(i=0;i<scratchpads;i++)dma_free(p,&sb[i]);if(sb)uefi_call_wrapper(BS->FreePool,1,sb);ts=dma_free(p,&erst_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&ev_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&cr_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&spa_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&dcbaa_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;dma_live=FALSE;}
    if(hs)FreePool(hs);
    if(EFI_ERROR(s)){Print(u"\r\nV31 ENABLE SLOT: FAIL RESULT=%r\r\nFAIL STAGE=%s OP=%s STATUS=%r\r\n",s,fail_stage,fail_op,fail_status);}else{Print(u"\r\nV31 ENABLE SLOT: PASS SLOT=%u\r\nCOMMANDS=1 DOORBELLS=1 CPU-INTERRUPTS=0 EVENTS=%u\r\nRESET RECOVERY PASS / ALL CONTROLLER POINTERS CLEARED BEFORE DMA RELEASE\r\n",slot_id,consumed?1U:0U);}Print(u"MMIO READS=%u WRITES=%u RESULT=%r\r\nEXIT 5 SEC...\r\n",reads,writes,s);uefi_call_wrapper(BS->Stall,1,5000000);return s;
}
