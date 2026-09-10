#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define XHCI_MIN_VERSION 0x0100U
#define CMD_RUN 0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE 0x00000004U
#define CMD_HSEE 0x00000008U
#define STS_HCH 0x00000001U
#define STS_CNR 0x00000800U
#define IMAN_IE 0x00000002U
#define IMAN_IP 0x00000001U
#define CRCR_RCS 0x00000001ULL
#define TRB_CYCLE 0x00000001U
#define TRB_TYPE_SHIFT 10U
#define TRB_TYPE_MASK 0x0000fc00U
#define TRB_ENABLE_SLOT 9U
#define TRB_ADDRESS_DEVICE 11U
#define TRB_COMMAND_COMPLETION 33U
#define TRB_PORT_STATUS_CHANGE 34U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 0x00000002U
#define CC_SUCCESS 1U
#define ERST_ADDR_MASK 0xffffffffffffffc0ULL
#define ERDP_ADDR_MASK 0xfffffffffffffff0ULL
#define CMD_TRBS 256U
#define EVENT_TRBS 16U
#define MAX_SCRATCHPADS 1024U
#define EXPECTED_PORT 4U
#define PORTSC_BASE 0x400U
#define PORT_CCS (1U<<0)
#define PORT_PED (1U<<1)
#define PORT_PR (1U<<4)
#define PORT_PLS_MASK (0xfU<<5)
#define PORT_SPEED_MASK (0xfU<<10)
#define PORT_PRC (1U<<21)
#define PORT_CHANGE_MASK ((1U<<17)|(1U<<18)|(0x7fU<<19))
#define PORT_RO ((1U<<0)|(1U<<3)|(0xfU<<10)|(1U<<30))
#define PORT_RWS ((0xfU<<5)|(1U<<9)|(0x3U<<14)|(0x7U<<25))
#define CTX_CSZ (1U<<2)
#define EP0_CERR 3U
#define EP0_CTRL_TYPE 4U
#define SPEED_SHIFT 20U
#define ROOT_PORT_SHIFT 16U
#define CTX_ENTRIES_SHIFT 27U
#define DEV_CTX_PAGES 2U
#define INPUT_CTX_PAGES 2U

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static const CHAR16 *fail_stage=u"NONE",*fail_op=u"NONE";
static EFI_STATUS fail_status=EFI_SUCCESS;

struct dma_obj { VOID *host; VOID *map; EFI_PHYSICAL_ADDRESS dev; UINTN pages; BOOLEAN live; };

static void fail(const CHAR16 *a,const CHAR16 *b,EFI_STATUS s){if(!EFI_ERROR(fail_status)){fail_stage=a;fail_op=b;fail_status=s;}}
static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT32 *v){return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,o,1,v);}
static EFI_STATUS mr32(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT32 *v){return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,v);}
static EFI_STATUS mr16(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT16 *v){return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint16,0,(UINT64)o,1,v);}
static EFI_STATUS mw32(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT32 v){return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,&v);}
static EFI_STATUS mr64(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT64 *v){UINT32 l,h;EFI_STATUS s=mr32(p,o,&l);if(EFI_ERROR(s))return s;s=mr32(p,o+4,&h);if(EFI_ERROR(s))return s;*v=((UINT64)h<<32)|l;return EFI_SUCCESS;}
static EFI_STATUS mw64(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT64 v){EFI_STATUS s=mw32(p,o,(UINT32)v);if(EFI_ERROR(s))return s;return mw32(p,o+4,(UINT32)(v>>32));}
static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL *p,UINTN pages,struct dma_obj *d){EFI_STATUS s;UINTN bytes=pages*4096U,mb=bytes;d->host=NULL;d->map=NULL;d->dev=0;d->pages=pages;d->live=FALSE;if(!pages||pages>((UINTN)-1)/4096U)return EFI_BAD_BUFFER_SIZE;s=uefi_call_wrapper(p->AllocateBuffer,6,p,AllocateAnyPages,EfiBootServicesData,pages,&d->host,0);if(EFI_ERROR(s))return s;s=uefi_call_wrapper(BS->SetMem,3,d->host,bytes,0);if(EFI_ERROR(s)){uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host);d->host=NULL;return s;}s=uefi_call_wrapper(p->Map,6,p,EfiPciIoOperationBusMasterCommonBuffer,d->host,&mb,&d->dev,&d->map);if(EFI_ERROR(s)||mb!=bytes){EFI_STATUS x=EFI_ERROR(s)?s:EFI_DEVICE_ERROR;if(!EFI_ERROR(s)&&d->map)uefi_call_wrapper(p->Unmap,2,p,d->map);uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host);d->host=NULL;d->map=NULL;d->dev=0;return x;}d->live=TRUE;return EFI_SUCCESS;}
static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL *p,struct dma_obj *d){EFI_STATUS s=EFI_SUCCESS,t;if(!d->live)return EFI_SUCCESS;if(d->map){t=uefi_call_wrapper(p->Unmap,2,p,d->map);if(EFI_ERROR(t))s=t;}if(d->host){t=uefi_call_wrapper(p->FreeBuffer,3,p,d->pages,d->host);if(EFI_ERROR(t)&&!EFI_ERROR(s))s=t;}d->host=NULL;d->map=NULL;d->dev=0;d->live=FALSE;return s;}
static EFI_STATUS wait_hch(EFI_PCI_IO_PROTOCOL *p,UINT32 op,BOOLEAN halt,UINTN loops,UINT32 *st,UINT32 *reads){UINTN i;EFI_STATUS s;UINT32 want=halt?STS_HCH:0;for(i=0;i<loops;i++){s=mr32(p,op+4,st);(*reads)++;if(EFI_ERROR(s))return s;if((*st&STS_HCH)==want)return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static EFI_STATUS reset_xhci(EFI_PCI_IO_PROTOCOL *p,UINT32 op,UINT32 *cmd,UINT32 *st,UINT32 *reads,UINT32 *writes){EFI_STATUS s;UINTN i;s=mr32(p,op,cmd);(*reads)++;if(EFI_ERROR(s))return s;*cmd&=~(CMD_RUN|CMD_INTE|CMD_HSEE);*cmd|=CMD_RESET;s=mw32(p,op,*cmd);(*writes)++;if(EFI_ERROR(s))return s;for(i=0;i<1000;i++){s=mr32(p,op,cmd);(*reads)++;if(EFI_ERROR(s))return s;if(!(*cmd&CMD_RESET))break;uefi_call_wrapper(BS->Stall,1,1000);}if(*cmd&CMD_RESET)return EFI_TIMEOUT;for(i=0;i<10000;i++){s=mr32(p,op+4,st);(*reads)++;if(EFI_ERROR(s))return s;if(!(*st&STS_CNR))return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static EFI_STATUS slot_type(EFI_PCI_IO_PROTOCOL *p,UINT32 hcc,UINT32 *type,UINT32 *reads){UINT32 off=((hcc>>16)&0xffffU)*4U,h,next;UINTN n=0;EFI_STATUS s;while(off&&n++<64){s=mr32(p,off,&h);(*reads)++;if(EFI_ERROR(s))return s;if((h&0xffU)==2U){s=mr32(p,off+12,&h);(*reads)++;if(EFI_ERROR(s))return s;*type=h&31U;return EFI_SUCCESS;}next=((h>>8)&255U)*4U;if(!next)break;off+=next;}*type=0;return EFI_SUCCESS;}
static UINT32 port_neutral(UINT32 x){return (x&PORT_RO)|(x&PORT_RWS);}
static void put32(VOID *b,UINTN off,UINT32 v){((UINT32*)b)[off/4]=v;}
static void put64(VOID *b,UINTN off,UINT64 v){put32(b,off,(UINT32)v);put32(b,off+4,(UINT32)(v>>32));}
static UINT32 get32(VOID *b,UINTN off){return ((UINT32*)b)[off/4];}
static void fatal_running(void){Print(u"\r\nFATAL: XHCI NOT CONFIRMED HALTED\r\nDMA MAPPINGS RETAINED / NO FREE / MANUAL RECOVERY REQUIRED\r\n");for(;;)uefi_call_wrapper(BS->Stall,1,1000000);}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st){
 EFI_HANDLE *hs=NULL;EFI_PCI_IO_PROTOCOL *p=NULL;EFI_STATUS s=EFI_SUCCESS,ts;UINTN n=0,i;
 UINT32 id=0,cls=0,bar0=0,bar1=0,cap=0,hcs1=0,hcs2=0,hcc=0,op=0,db=0,rt=0,ir=0;
 UINT32 cmd=0,status=0,iman=0,maxslots=0,scratchpads=0,ps=0,stype=0,reads=0,writes=0,ctxsz=32;
 UINT32 portsc=0,speed=0,port=EXPECTED_PORT,slot=0,event_idx=0,event_type=0,cc=0,ed0=0,ed1=0,ed2=0,ed3=0;UINT64 eptr=0;
 UINT32 ext_reads=0;UINTN shift=0,xpage=0,scratch_pages=0,ctx_bytes=0,input_bytes=0;BOOLEAN halted=FALSE,submitted=FALSE,dma_live=FALSE;
 struct dma_obj dcbaa_d={0},spa_d={0},cr_d={0},ev_d={0},erst_d={0},devctx_d={0},inctx_d={0},ep0_d={0};struct dma_obj *sb=NULL;
 UINT64 *dcbaa=NULL,*spa=NULL,*erst=NULL;UINT32 *cr=NULL;UINT8 *devctx=NULL,*inctx=NULL;UINT32 *ev=NULL;
 InitializeLib(image,st);
 Print(u"TOSHIBA xHCI V32 / PORT RESET + ADDRESS DEVICE\r\nKNOWN RECEIVER PORT 4 / FRESH SLOT + CONTEXTS / KEYBOARD INTERFACE ONLY\r\nNO DESCRIPTORS / NO CONFIGURE ENDPOINT / NO HID REPORTS / NO CPU INTERRUPTS\r\n");
 s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);if(EFI_ERROR(s)){fail(u"PCI",u"LOCATE",s);goto out;}
 for(i=0;i<n;i++){EFI_PCI_IO_PROTOCOL *q=NULL;if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,(void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL)))continue;if(EFI_ERROR(cfg32(q,8,&cls))||EFI_ERROR(cfg32(q,0,&id)))continue;if(((cls>>24)&255)==0x0c&&((cls>>16)&255)==3&&((cls>>8)&255)==0x30){p=q;break;}}
 if(!p){s=EFI_NOT_FOUND;fail(u"PCI",u"FIND XHCI",s);goto out;}
 s=cfg32(p,0x10,&bar0);if(EFI_ERROR(s)){fail(u"PCI",u"BAR0",s);goto out;}if((bar0&1U)||((bar0>>1)&3U)!=2U){s=EFI_UNSUPPORTED;fail(u"PCI",u"64-BIT BAR",s);goto out;}s=cfg32(p,0x14,&bar1);if(EFI_ERROR(s)){fail(u"PCI",u"BAR1",s);goto out;}
 s=mr32(p,0,&cap);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"CAPLENGTH",s);goto out;}op=cap&255U;{UINT16 ver=0;s=mr16(p,2,&ver);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"VERSION",s);goto out;}if(ver<XHCI_MIN_VERSION){s=EFI_UNSUPPORTED;fail(u"CAPS",u"VERSION",s);goto out;}Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x BAR=%08x/%08x OPBASE=%02x\r\n",ver>>8,ver&255,id&65535,id>>16,bar0,bar1,op);}
 s=mr32(p,4,&hcs1);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"HCSPARAMS1",s);goto out;}s=mr32(p,8,&hcs2);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"HCSPARAMS2",s);goto out;}s=mr32(p,0x10,&hcc);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"HCCPARAMS1",s);goto out;}s=mr32(p,op+4,&status);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"USBSTS",s);goto out;}
 maxslots=hcs1&255U;scratchpads=(((hcs2>>21)&31U)<<5)|((hcs2>>27)&31U);ctxsz=(hcc&CTX_CSZ)?64:32;s=mr32(p,op+8,&ps);reads++;if(EFI_ERROR(s)||!ps){s=EFI_UNSUPPORTED;fail(u"CAPS",u"PAGESIZE",s);goto out;}while(shift<32&&!(ps&(1U<<shift)))shift++;if(shift>=32){s=EFI_UNSUPPORTED;fail(u"CAPS",u"PAGE BIT",s);goto out;}xpage=(UINTN)1U<<(12+shift);if(xpage!=4096U){s=EFI_UNSUPPORTED;fail(u"CAPS",u"PAGE SIZE",s);goto out;}scratch_pages=1;
 Print(u"CAPS: SLOTS=%u SCRATCHPADS=%u PAGESIZE=%u CONTEXT=%u HCH=%u CNR=%u\r\n",maxslots,scratchpads,(UINT32)xpage,ctxsz,status&STS_HCH?1:0,status&STS_CNR?1:0);if(!maxslots||scratchpads>MAX_SCRATCHPADS){s=EFI_UNSUPPORTED;fail(u"CAPS",u"LIMITS",s);goto out;}
 s=mr32(p,0x14,&db);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"DBOFF",s);goto out;}db&=~3U;s=mr32(p,0x18,&rt);reads++;if(EFI_ERROR(s)){fail(u"CAPS",u"RTSOFF",s);goto out;}rt&=~31U;ir=rt+0x20U;s=slot_type(p,hcc,&stype,&ext_reads);reads+=ext_reads;if(EFI_ERROR(s)||!stype){s=EFI_UNSUPPORTED;fail(u"CAPS",u"SLOT TYPE",s);goto out;}Print(u"CAPS: DBOFF=%08x RTSOFF=%08x SLOT-TYPE=%u\r\n",db,rt,stype);
 s=mr32(p,op,&cmd);reads++;if(EFI_ERROR(s)){fail(u"HALT",u"USBCMD",s);goto out;}if(!(status&STS_HCH)){s=mw32(p,op,cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE));writes++;if(EFI_ERROR(s)){fail(u"HALT",u"STOP",s);goto out;}s=wait_hch(p,op,TRUE,1000,&status,&reads);if(EFI_ERROR(s)){fail(u"HALT",u"HCH",s);goto out;}}halted=TRUE;s=reset_xhci(p,op,&cmd,&status,&reads,&writes);if(EFI_ERROR(s)){fail(u"RESET",u"RESET/CNR",s);goto out;}if(!(status&STS_HCH)){s=EFI_DEVICE_ERROR;fail(u"RESET",u"HALTED",s);goto out;}
 s=dma_alloc(p,1,&dcbaa_d);if(EFI_ERROR(s)){fail(u"DMA",u"DCBAA",s);goto out;}s=dma_alloc(p,1,&spa_d);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCH ARRAY",s);goto out;}s=dma_alloc(p,1,&cr_d);if(EFI_ERROR(s)){fail(u"DMA",u"COMMAND RING",s);goto out;}s=dma_alloc(p,1,&ev_d);if(EFI_ERROR(s)){fail(u"DMA",u"EVENT RING",s);goto out;}s=dma_alloc(p,1,&erst_d);if(EFI_ERROR(s)){fail(u"DMA",u"ERST",s);goto out;}s=dma_alloc(p,DEV_CTX_PAGES,&devctx_d);if(EFI_ERROR(s)){fail(u"DMA",u"DEVICE CONTEXT",s);goto out;}s=dma_alloc(p,INPUT_CTX_PAGES,&inctx_d);if(EFI_ERROR(s)){fail(u"DMA",u"INPUT CONTEXT",s);goto out;}s=dma_alloc(p,1,&ep0_d);if(EFI_ERROR(s)){fail(u"DMA",u"EP0 RING",s);goto out;}dma_live=TRUE;
 if((dcbaa_d.dev&63)||(devctx_d.dev&63)||(inctx_d.dev&63)||(ep0_d.dev&15)){s=EFI_BAD_BUFFER_SIZE;fail(u"DMA",u"ALIGNMENT",s);goto out;}
 if(scratchpads){s=uefi_call_wrapper(BS->AllocatePool,3,EfiBootServicesData,scratchpads*sizeof(struct dma_obj),(void**)&sb);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCH DESCRIPTORS",s);goto out;}uefi_call_wrapper(BS->SetMem,3,sb,scratchpads*sizeof(struct dma_obj),0);for(i=0;i<scratchpads;i++){s=dma_alloc(p,1,&sb[i]);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCHPAD",s);goto out;}}}
 dcbaa=(UINT64*)dcbaa_d.host;spa=(UINT64*)spa_d.host;cr=(UINT32*)cr_d.host;ev=(UINT32*)ev_d.host;erst=(UINT64*)erst_d.host;devctx=(UINT8*)devctx_d.host;inctx=(UINT8*)inctx_d.host;dcbaa[0]=scratchpads?spa_d.dev:0;for(i=0;i<scratchpads;i++)spa[i]=sb[i].dev;
 cr[0]=0;cr[1]=0;cr[2]=0;cr[3]=TRB_CYCLE|(stype<<16)|(TRB_ENABLE_SLOT<<TRB_TYPE_SHIFT);cr[(CMD_TRBS-1)*4]= (UINT32)cr_d.dev;cr[(CMD_TRBS-1)*4+1]=(UINT32)(cr_d.dev>>32);cr[(CMD_TRBS-1)*4+2]=0;cr[(CMD_TRBS-1)*4+3]=TRB_CYCLE|TRB_LINK_TOGGLE|(TRB_LINK<<TRB_TYPE_SHIFT);
 erst[0]=ev_d.dev;erst[1]=0;erst[2]=EVENT_TRBS;erst[3]=0;
 s=mw64(p,op+0x30,dcbaa_d.dev);writes+=2;if(EFI_ERROR(s)){fail(u"INIT",u"DCBAAP",s);goto out;}s=mw32(p,op+0x38,1);writes++;if(EFI_ERROR(s)){fail(u"INIT",u"CONFIG",s);goto out;}s=mw64(p,op+0x18,cr_d.dev|CRCR_RCS);writes+=2;if(EFI_ERROR(s)){fail(u"INIT",u"CRCR",s);goto out;}s=mw32(p,ir+8,1);writes++;if(EFI_ERROR(s)){fail(u"INIT",u"ERSTSZ",s);goto out;}s=mw64(p,ir+0x10,erst_d.dev&ERST_ADDR_MASK);writes+=2;if(EFI_ERROR(s)){fail(u"INIT",u"ERSTBA",s);goto out;}s=mw64(p,ir+0x18,ev_d.dev&ERDP_ADDR_MASK);writes+=2;if(EFI_ERROR(s)){fail(u"INIT",u"ERDP",s);goto out;}
 s=mr32(p,ir,&iman);reads++;if(EFI_ERROR(s)){fail(u"INIT",u"IMAN",s);goto out;}s=mw32(p,ir,iman&~IMAN_IE);writes++;if(EFI_ERROR(s)){fail(u"INIT",u"IRQ DISABLE",s);goto out;}
 s=mr32(p,op,&cmd);reads++;if(EFI_ERROR(s)){fail(u"RUN",u"USBCMD",s);goto out;}halted=FALSE;s=mw32(p,op,(cmd&~(CMD_INTE|CMD_HSEE))|CMD_RUN);writes++;if(EFI_ERROR(s)){fail(u"RUN",u"START",s);goto out;}s=wait_hch(p,op,FALSE,1000,&status,&reads);if(EFI_ERROR(s)){fail(u"RUN",u"HCH CLEAR",s);goto out;}
 s=mr32(p,op+PORTSC_BASE+((port-1)*0x10),&portsc);reads++;if(EFI_ERROR(s)){fail(u"PORT",u"READ",s);goto out;}if(!(portsc&PORT_CCS)){s=EFI_NOT_FOUND;fail(u"PORT",u"EXPECTED PORT DISCONNECTED",s);goto out;}if(portsc&PORT_PR){s=EFI_DEVICE_ERROR;fail(u"PORT",u"ALREADY RESET",s);goto out;}s=mw32(p,op+PORTSC_BASE+((port-1)*0x10),port_neutral(portsc)|PORT_PR);writes++;if(EFI_ERROR(s)){fail(u"PORT",u"RESET",s);goto out;}Print(u"PORT RESET: PORT=%u BEFORE=%08x\r\n",port,portsc);
 for(i=0;i<10000;i++){s=mr32(p,op+PORTSC_BASE+((port-1)*0x10),&portsc);reads++;if(EFI_ERROR(s))goto out;if((portsc&PORT_PRC)&&!(portsc&PORT_PR)&&((portsc&PORT_PLS_MASK)==0)&&((portsc&PORT_PED)))break;uefi_call_wrapper(BS->Stall,1,1000);}if(i==10000){s=EFI_TIMEOUT;fail(u"PORT",u"RESET COMPLETE",s);goto out;}speed=(portsc&PORT_SPEED_MASK)>>10;if(speed<1||speed>4){s=EFI_DEVICE_ERROR;fail(u"PORT",u"INVALID SPEED",s);goto out;}Print(u"PORT RESET: PORT=%u AFTER=%08x SPEED=%u PED=1 PRC=1 PASS\r\n",port,portsc,speed);
 {UINT32 ack=port_neutral(portsc)|(portsc&PORT_CHANGE_MASK);s=mw32(p,op+PORTSC_BASE+((port-1)*0x10),ack);writes++;if(EFI_ERROR(s)){fail(u"PORT",u"ACK CHANGES",s);goto out;}}
 /* Enable Slot command. */
 s=mw32(p,db,0);writes++;if(EFI_ERROR(s)){fail(u"COMMAND",u"ENABLE SLOT DOORBELL",s);goto out;}submitted=TRUE;Print(u"ENABLE SLOT: DOORBELL=0 COMMAND-TRB=%016lx\r\n",cr_d.dev);
 for(i=0;i<10000;i++){ed0=ev[event_idx*4];ed1=ev[event_idx*4+1];ed2=ev[event_idx*4+2];ed3=ev[event_idx*4+3];event_type=(ed3&TRB_TYPE_MASK)>>TRB_TYPE_SHIFT;if((ed3&TRB_CYCLE)&&event_type==TRB_COMMAND_COMPLETION)break;if((ed3&TRB_CYCLE)&&event_type==TRB_PORT_STATUS_CHANGE){event_idx++;s=mw64(p,ir+0x18,ev_d.dev+(event_idx*16U));writes+=2;if(EFI_ERROR(s)){fail(u"EVENT",u"ADVANCE PORT EVENT",s);goto out;}continue;}uefi_call_wrapper(BS->Stall,1,1000);}if(i==10000){s=EFI_TIMEOUT;fail(u"EVENT",u"ENABLE SLOT COMPLETION",s);goto out;}cc=(ed2>>24)&255U;slot=(ed3>>24)&255U;eptr=((UINT64)ed1<<32)|ed0;if(cc!=CC_SUCCESS||!slot||slot>maxslots||eptr!=cr_d.dev){s=EFI_DEVICE_ERROR;fail(u"EVENT",u"ENABLE SLOT VALIDATE",s);goto out;}event_idx++;s=mw64(p,ir+0x18,ev_d.dev+(event_idx*16U));writes+=2;if(EFI_ERROR(s)){fail(u"EVENT",u"ERDP AFTER SLOT",s);goto out;}Print(u"ENABLE SLOT: COMPLETION CODE=%u SLOT=%u PASS\r\n",cc,slot);
 /* Device Context and EP0 Transfer Ring for Address Device. */
 ctx_bytes=ctxsz*32U;input_bytes=ctxsz*33U;if(ctx_bytes>DEV_CTX_PAGES*4096U||input_bytes>INPUT_CTX_PAGES*4096U){s=EFI_BAD_BUFFER_SIZE;fail(u"CTX",u"SIZE",s);goto out;}
 dcbaa[slot]=devctx_d.dev;for(i=0;i<(DEV_CTX_PAGES*4096U)/4;i++)((UINT32*)devctx)[i]=0;for(i=0;i<(INPUT_CTX_PAGES*4096U)/4;i++)((UINT32*)inctx)[i]=0;for(i=0;i<1024;i++)cr[4+i]=cr[4+i];
 {UINT32 *ep=(UINT32*)ep0_d.host;for(i=0;i<1024;i++)ep[i]=0;ep[(1024-1)*4]= (UINT32)ep0_d.dev;ep[(1024-1)*4+1]=(UINT32)(ep0_d.dev>>32);ep[(1024-1)*4+2]=0;ep[(1024-1)*4+3]=TRB_CYCLE|(TRB_LINK<<TRB_TYPE_SHIFT);}
 {UINT8 *slotc=inctx+ctxsz;UINT8 *epc=inctx+(ctxsz*2U);UINT32 sd=(speed<<SPEED_SHIFT)|(1U<<CTX_ENTRIES_SHIFT);UINT32 sd2=(port<<ROOT_PORT_SHIFT);UINT32 ep0=(EP0_CERR<<1)|(EP0_CTRL_TYPE<<3);UINT32 mps=(speed==3)?64U:((speed==4)?512U:8U);put32(inctx,4,3U);put32(slotc,0,sd);put32(slotc,4,sd2);put32(slotc,8,0);put32(slotc,12,0);put32(epc,0,0);put32(epc,4,ep0|(mps<<16));put64(epc,8,(ep0_d.dev&~0xfULL)|1ULL);put32(epc,16,0);}
 /* Address Device command is TRB 1; the command ring consumer is already there. */
 cr[4]=0;cr[5]=0;cr[6]=0;cr[7]=TRB_CYCLE|(TRB_ADDRESS_DEVICE<<TRB_TYPE_SHIFT)|(slot<<24);cr[4]|=0;cr[5]=(UINT32)(inctx_d.dev);cr[6]=(UINT32)(inctx_d.dev>>32);cr[7]|=(UINT32)(inctx_d.dev>>32); /* corrected below */
 cr[4]=(UINT32)inctx_d.dev;cr[5]=(UINT32)(inctx_d.dev>>32);cr[6]=0;cr[7]=TRB_CYCLE|(TRB_ADDRESS_DEVICE<<TRB_TYPE_SHIFT)|(slot<<24);
 s=mw32(p,db,0);writes++;if(EFI_ERROR(s)){fail(u"ADDRESS",u"DOORBELL",s);goto out;}Print(u"ADDRESS DEVICE: SLOT=%u INPUT-CONTEXT=%016lx EP0-MPS=%u SPEED=%u\r\n",slot,inctx_d.dev,(speed==3)?64U:((speed==4)?512U:8U),speed);
 for(i=0;i<10000;i++){ed0=ev[event_idx*4];ed1=ev[event_idx*4+1];ed2=ev[event_idx*4+2];ed3=ev[event_idx*4+3];event_type=(ed3&TRB_TYPE_MASK)>>TRB_TYPE_SHIFT;if((ed3&TRB_CYCLE)&&event_type==TRB_COMMAND_COMPLETION)break;uefi_call_wrapper(BS->Stall,1,1000);}if(i==10000){s=EFI_TIMEOUT;fail(u"EVENT",u"ADDRESS COMPLETION",s);goto out;}cc=(ed2>>24)&255U;{UINT32 eslot=(ed3>>24)&255U;eptr=((UINT64)ed1<<32)|ed0;if(cc!=CC_SUCCESS||eslot!=slot||eptr!=((UINT64)cr_d.dev+16U)){s=EFI_DEVICE_ERROR;fail(u"EVENT",u"ADDRESS VALIDATE",s);goto out;}}
 event_idx++;s=mw64(p,ir+0x18,ev_d.dev+(event_idx*16U));writes+=2;if(EFI_ERROR(s)){fail(u"EVENT",u"ERDP AFTER ADDRESS",s);goto out;}Print(u"ADDRESS DEVICE: COMPLETION CODE=%u SLOT=%u PTR=%016lx PASS\r\n",cc,slot,eptr);
 s=mr32(p,op,&cmd);reads++;if(EFI_ERROR(s)){fail(u"HALT",u"USBCMD",s);goto out;}s=mw32(p,op,cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE));writes++;if(EFI_ERROR(s)){fail(u"HALT",u"STOP",s);goto out;}s=wait_hch(p,op,TRUE,10000,&status,&reads);if(EFI_ERROR(s)){fail(u"HALT",u"CONFIRM HCH",s);fatal_running();}halted=TRUE;s=reset_xhci(p,op,&cmd,&status,&reads,&writes);if(EFI_ERROR(s)){fail(u"RESET",u"RECOVERY",s);goto out;}
 s=mw64(p,op+0x18,0);writes+=2;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"CRCR",s);goto out;}s=mw64(p,op+0x30,0);writes+=2;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"DCBAAP",s);goto out;}s=mw32(p,op+0x38,0);writes++;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"CONFIG",s);goto out;}s=mw32(p,ir+8,0);writes++;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"ERSTSZ",s);goto out;}s=mw64(p,ir+0x10,0);writes+=2;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"ERSTBA",s);goto out;}s=mw64(p,ir+0x18,0);writes+=2;if(EFI_ERROR(s)){fail(u"TEARDOWN",u"ERDP",s);goto out;}
out:
 if(EFI_ERROR(s)&&!halted)fatal_running();
 if(dma_live&&halted){if(sb)for(i=0;i<scratchpads;i++)dma_free(p,&sb[i]);if(sb)uefi_call_wrapper(BS->FreePool,1,sb);ts=dma_free(p,&ep0_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&inctx_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&devctx_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&erst_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&ev_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&cr_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&spa_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;ts=dma_free(p,&dcbaa_d);if(EFI_ERROR(ts)&&!EFI_ERROR(s))s=ts;dma_live=FALSE;}
 if(hs)FreePool(hs);if(EFI_ERROR(s))Print(u"\r\nV32 GATE6: FAIL RESULT=%r\r\nFAIL STAGE=%s OP=%s STATUS=%r\r\n",s,fail_stage,fail_op,fail_status);else Print(u"\r\nV32 GATE6: PASS SLOT=%u PORT=%u SPEED=%u\r\nCOMMANDS=2 DOORBELLS=2 PORT-RESETS=1 CPU-INTERRUPTS=0 USB-TRANSFERS=0\r\nRESET RECOVERY PASS / ALL CONTROLLER POINTERS CLEARED BEFORE DMA RELEASE\r\n",slot,port,speed);Print(u"MMIO READS=%u WRITES=%u RESULT=%r\r\nEXIT 5 SEC...\r\n",reads,writes,s);uefi_call_wrapper(BS->Stall,1,5000000);return s;
}
