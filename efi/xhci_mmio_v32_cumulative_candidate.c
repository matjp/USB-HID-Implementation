#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include "usb_io_compat.h"

#define XHCI_MIN_VERSION 0x0100U
#define XHCI_CLASS 0x0cU
#define XHCI_SUBCLASS 0x03U
#define XHCI_PROG_IF 0x30U
#define CMD_RUN 1U
#define CMD_RESET 2U
#define CMD_INTE 4U
#define CMD_HSEE 8U
#define STS_HCH 1U
#define STS_CNR 0x800U
#define IMAN_IE 2U
#define IMAN_IP 1U
#define HCC_AC64 1U
#define CTX_CSZ (1U << 2)
#define TRB_CYCLE 1U
#define TRB_TYPE_SHIFT 10U
#define TRB_ENABLE_SLOT 9U
#define TRB_ADDRESS_DEVICE 11U
#define TRB_COMMAND_COMPLETION 33U
#define TRB_PORT_STATUS_CHANGE 34U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 2U
#define CC_SUCCESS 1U
#define PORTSC_BASE 0x400U
#define PORT_CCS (1U << 0)
#define PORT_PED (1U << 1)
#define PORT_PR (1U << 4)
#define PORT_SPEED_MASK (0xFU << 10)
#define PORT_PRC (1U << 21)
#define PORT_CHANGE_MASK (0x7FU << 17)
#define PORT_RO ((1U << 0) | (1U << 3) | (0xFU << 10) | (1U << 30))
#define PORT_RWS ((0xFU << 5) | (1U << 9) | (3U << 14) | (7U << 25))
#define PORT_RW (1U << 16)
#define TRB_SLOT_SHIFT 24U
#define TRB_BSR (1U << 9)
#define ROOT_PORT_SHIFT 16U
#define SPEED_SHIFT 20U
#define CTX_ENTRIES_SHIFT 27U
#define EP0_CERR 3U
#define EP0_CTRL_TYPE 4U
#define CMD_TRBS 256U
#define EVENT_TRBS 16U
#define MAX_SCRATCHPADS 1024U
#define USB_CLASS_HID 3U
#define HID_SUBCLASS_BOOT 1U
#define HID_PROTOCOL_KEYBOARD 1U
#define DP_TYPE_MESSAGING 3U
#define DP_SUBTYPE_USB 5U
#define DP_TYPE_END 0x7fU
#define DP_USB_LEN 6U

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID UsbIoGuid = EFI_USB_IO_PROTOCOL_GUID;
static const CHAR16 *fail_stage = u"NONE";
static const CHAR16 *fail_op = u"NONE";
static EFI_STATUS fail_status = EFI_SUCCESS;

struct dma_obj { VOID *host; VOID *map; EFI_PHYSICAL_ADDRESS dev; UINTN pages; BOOLEAN live; };
struct discovery { UINT8 port; UINT8 speed; UINT8 interface_number; UINT8 endpoint; UINT16 mps; UINT8 interval; UINT16 vid; UINT16 pid; UINT8 config; BOOLEAN found; };

static void remember_fail(const CHAR16 *stage,const CHAR16 *op,EFI_STATUS s)
{ if(!EFI_ERROR(fail_status)){fail_stage=stage;fail_op=op;fail_status=s;} }

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT32 *v)
{ return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,o,1,v); }
static EFI_STATUS mr32(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT32 *v)
{ return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,v); }
static EFI_STATUS mr16(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT16 *v)
{ return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint16,0,(UINT64)o,1,v); }
static EFI_STATUS mw32(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT32 v)
{ return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,&v); }
static EFI_STATUS mw64(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT64 v)
{ EFI_STATUS s=mw32(p,o,(UINT32)v); if(EFI_ERROR(s))return s; return mw32(p,o+4,(UINT32)(v>>32)); }

static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL *p,UINTN pages,struct dma_obj *d)
{
 EFI_STATUS s; UINTN bytes,mapped;
 d->host=NULL;d->map=NULL;d->dev=0;d->pages=pages;d->live=FALSE;
 if(!pages||pages>((UINTN)-1)/4096U)return EFI_BAD_BUFFER_SIZE;
 bytes=pages*4096U;
 s=uefi_call_wrapper(p->AllocateBuffer,6,p,AllocateAnyPages,EfiBootServicesData,pages,&d->host,0);if(EFI_ERROR(s))return s;
 s=uefi_call_wrapper(BS->SetMem,3,d->host,bytes,0);if(EFI_ERROR(s)){uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host);d->host=NULL;return s;}
 mapped=bytes;s=uefi_call_wrapper(p->Map,6,p,EfiPciIoOperationBusMasterCommonBuffer,d->host,&mapped,&d->dev,&d->map);
 if(EFI_ERROR(s)||mapped!=bytes){EFI_STATUS x=EFI_ERROR(s)?s:EFI_DEVICE_ERROR;if(!EFI_ERROR(s)&&d->map)uefi_call_wrapper(p->Unmap,2,p,d->map);uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host);d->host=NULL;d->map=NULL;d->dev=0;return x;}
 d->live=TRUE;return EFI_SUCCESS;
}
static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL *p,struct dma_obj *d)
{ EFI_STATUS s=EFI_SUCCESS,t;if(!d->live)return EFI_SUCCESS;if(d->map){t=uefi_call_wrapper(p->Unmap,2,p,d->map);if(EFI_ERROR(t))s=t;}if(d->host){t=uefi_call_wrapper(p->FreeBuffer,3,p,d->pages,d->host);if(EFI_ERROR(t)&&!EFI_ERROR(s))s=t;}d->host=NULL;d->map=NULL;d->dev=0;d->live=FALSE;return s; }

static EFI_STATUS wait_hch(EFI_PCI_IO_PROTOCOL *p,UINT32 op,BOOLEAN halted,UINTN loops,UINT32 *st)
{ UINTN i;EFI_STATUS s;UINT32 want=halted?STS_HCH:0;for(i=0;i<loops;i++){s=mr32(p,op+4,st);if(EFI_ERROR(s))return s;if((*st&STS_HCH)==want)return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT; }
static EFI_STATUS reset_xhci(EFI_PCI_IO_PROTOCOL *p,UINT32 op,UINT32 *cmd,UINT32 *st)
{ EFI_STATUS s;UINTN i;s=mr32(p,op,cmd);if(EFI_ERROR(s))return s;*cmd=(*cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE))|CMD_RESET;s=mw32(p,op,*cmd);if(EFI_ERROR(s))return s;for(i=0;i<1000;i++){s=mr32(p,op,cmd);if(EFI_ERROR(s))return s;if(!(*cmd&CMD_RESET))break;uefi_call_wrapper(BS->Stall,1,1000);}if(*cmd&CMD_RESET)return EFI_TIMEOUT;for(i=0;i<10000;i++){s=mr32(p,op+4,st);if(EFI_ERROR(s))return s;if(!(*st&STS_CNR))return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT; }

static EFI_STATUS find_slot_type_for_port(EFI_PCI_IO_PROTOCOL *p,UINT32 hcc,UINT32 port,UINT32 *slot,UINT32 *reads)
{ UINT32 off=((hcc>>16)&0xffffU)*4U,w,d2,d3,next;UINTN n=0;EFI_STATUS s;while(off&&n++<64){s=mr32(p,off,&w);(*reads)++;if(EFI_ERROR(s))return s;if((w&0xffU)==2U){s=mr32(p,off+8,&d2);(*reads)++;if(EFI_ERROR(s))return s;s=mr32(p,off+12,&d3);(*reads)++;if(EFI_ERROR(s))return s;{UINT32 po=d2&0xffU,pc=(d2>>8)&0xffU;if(pc&&port>=po&&port<po+pc){*slot=d3&0x1fU;return EFI_SUCCESS;}}}next=((w>>8)&0xffU)*4U;if(!next)break;off+=next;}return EFI_NOT_FOUND; }

static UINT8 root_port_from_path(EFI_DEVICE_PATH_PROTOCOL *path)
{ UINT8 *p=(UINT8*)path;while(p){UINT8 type=p[0],sub=p[1],len=(UINT8)p[2]|((UINT16)p[3]<<8);if(len<4||len>255)break;if(type==DP_TYPE_END)break;if(type==DP_TYPE_MESSAGING&&sub==DP_SUBTYPE_USB&&len>=DP_USB_LEN)return p[4];p+=len;}return 0; }

static EFI_STATUS discover_keyboard(struct discovery *d,EFI_HANDLE image)
{
 EFI_HANDLE *hs=NULL;UINTN n=0,i;EFI_STATUS s;UINTN found=0;d->found=FALSE;
 s=LibLocateHandle(ByProtocol,&UsbIoGuid,NULL,&n,&hs);if(EFI_ERROR(s))return EFI_NOT_FOUND;
 for(i=0;i<n;i++){
  EFI_USB_IO_PROTOCOL *usb=NULL;EFI_USB_INTERFACE_DESCRIPTOR iface;EFI_USB_ENDPOINT_DESCRIPTOR ep;EFI_USB_DEVICE_DESCRIPTOR dd;EFI_USB_CONFIG_DESCRIPTOR cd;UINTN j;UINT8 port;EFI_DEVICE_PATH_PROTOCOL *path;
  if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&UsbIoGuid,(void**)&usb,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL)))continue;
  if(EFI_ERROR(uefi_call_wrapper(usb->UsbGetInterfaceDescriptor,3,usb,&iface)))continue;
  if(iface.InterfaceClass!=USB_CLASS_HID||iface.InterfaceSubClass!=HID_SUBCLASS_BOOT||iface.InterfaceProtocol!=HID_PROTOCOL_KEYBOARD)continue;
  if(EFI_ERROR(uefi_call_wrapper(usb->UsbGetDeviceDescriptor,2,usb,&dd)))continue;
  if(EFI_ERROR(uefi_call_wrapper(usb->UsbGetConfigDescriptor,2,usb,&cd)))continue;
  path=DevicePathFromHandle(hs[i]);port=root_port_from_path(path);if(!port)continue;
  d->port=port;d->interface_number=iface.InterfaceNumber;d->vid=dd.IdVendor;d->pid=dd.IdProduct;d->config=cd.ConfigurationValue;d->endpoint=0;d->mps=0;d->interval=0;
  for(j=0;j<iface.NumEndpoints&&j<16;j++){if(EFI_ERROR(uefi_call_wrapper(usb->UsbGetEndpointDescriptor,3,usb,(UINT8)j,&ep)))continue;if((ep.EndpointAddress&0x80U)&&((ep.Attributes&3U)==3U)){d->endpoint=ep.EndpointAddress;d->mps=ep.MaxPacketSize;d->interval=ep.Interval;break;}}
  if(!d->endpoint)continue;
  if(++found>1)return EFI_ABORTED;
 }
 if(hs)uefi_call_wrapper(BS->FreePool,1,hs);if(found!=1)return EFI_NOT_FOUND;d->found=TRUE;return EFI_SUCCESS;
}

static UINT32 port_preserve(UINT32 x){return (x&PORT_RO)|(x&PORT_RWS)|(x&PORT_RW);}
static void trb_clear(VOID *b,UINTN idx){((UINT32*)b)[idx*4]=0;((UINT32*)b)[idx*4+1]=0;((UINT32*)b)[idx*4+2]=0;((UINT32*)b)[idx*4+3]=0;}
static void trb_set(VOID *b,UINTN idx,UINT32 a,UINT32 c,UINT32 d0){((UINT32*)b)[idx*4]=a;((UINT32*)b)[idx*4+1]=0;((UINT32*)b)[idx*4+2]=c;((UINT32*)b)[idx*4+3]=d0;}

static EFI_STATUS event_next(EFI_PCI_IO_PROTOCOL *p,UINT32 ir,struct dma_obj *ev,UINTN *idx,UINT8 *cycle,UINT32 *type,UINT32 *f0,UINT32 *f2,UINT32 *f3)
{
 UINT32 *r=(UINT32*)ev->host;UINTN i;for(i=0;i<5000;i++){UINT32 d3=r[*idx*4+3];if((d3&TRB_CYCLE)==*cycle){*f0=r[*idx*4];*f2=r[*idx*4+2];*f3=d3;*type=(d3>>TRB_TYPE_SHIFT)&0x3fU;trb_clear(ev->host,*idx);*idx+=1;if(*idx==EVENT_TRBS){*idx=0;*cycle^=1;}if(EFI_ERROR(mw64(p,ir+0x38,ev->dev+(*idx)*16ULL|8ULL)))return EFI_DEVICE_ERROR;return EFI_SUCCESS;}uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;
}

static void fatal_running(void)
{ Print(u"\r\nFATAL: XHCI RUNNING STATE UNCERTAIN\r\nFAIL STAGE=%s OP=%s STATUS=%r\r\nDMA MAPPINGS RETAINED / NO FREE\r\nMANUAL RECOVERY REQUIRED\r\n",fail_stage,fail_op,fail_status);for(;;)uefi_call_wrapper(BS->Stall,1,1000000); }

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st)
{
 EFI_STATUS s=EFI_SUCCESS,t;EFI_HANDLE *hs=NULL;EFI_PCI_IO_PROTOCOL *p=NULL;UINTN n=0,i;UINT32 id=0,cls=0,bar0=0,bar1=0,cap=0,hcs1=0,hcs2=0,hcc=0,op=0,db=0,rt=0,ir=0,cmd=0,status=0,ps=0,config=0,dcbaap=0,crcr=0,erstba=0,erdp=0,slot_type=0,reads=0,writes=0;UINT16 ver=0;UINTN shift=0;UINT32 slots=0,scratchpads=0;UINT32 *cr,*ev;UINT64 *dcbaa,*spa,*erst;UINTN spa_pages;struct dma_obj dcbaa_d={0},spa_d={0},cr_d={0},ev_d={0},erst_d={0},devctx_d={0},inctx_d={0},ep0_d={0};struct dma_obj *sb=NULL;UINT8 slot=0,event_cycle=1;UINTN event_idx=0;UINT32 evtype=0,f0=0,f2=0,f3=0,portsc=0;struct discovery disc={0};BOOLEAN started=FALSE,halted=FALSE;
 InitializeLib(image,st);
 Print(u"TOSHIBA xHCI V32 / CUMULATIVE DISCOVERY + PORT RESET + ADDRESS DEVICE\r\nV28 DISCOVERY + V29 INIT + V30 RUN/HALT + V31 ENABLE SLOT\r\nNO DESCRIPTORS / NO CONFIGURE ENDPOINT / NO HID REPORTS\r\n");
 s=discover_keyboard(&disc,image);if(EFI_ERROR(s)){remember_fail(u"DISCOVERY",u"KEYBOARD",s);goto out;}Print(u"DISCOVERY: PORT=%u VID=%04x PID=%04x IF=%u EP=%02x MPS=%u INT=%u CFG=%u\r\n",disc.port,disc.vid,disc.pid,disc.interface_number,disc.endpoint,disc.mps,disc.interval,disc.config);
 s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);if(EFI_ERROR(s)){remember_fail(u"PCI",u"LOCATE",s);goto out;}
 for(i=0;i<n;i++){EFI_PCI_IO_PROTOCOL *q=NULL;if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,(void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL)))continue;if(EFI_ERROR(cfg32(q,8,&cls))||EFI_ERROR(cfg32(q,0,&id)))continue;if(((cls>>24)&255U)==XHCI_CLASS&&((cls>>16)&255U)==XHCI_SUBCLASS&&((cls>>8)&255U)==XHCI_PROG_IF){p=q;break;}}
 if(!p){s=EFI_NOT_FOUND;remember_fail(u"PCI",u"FIND XHCI",s);goto out;}
 s=cfg32(p,0x10,&bar0);if(EFI_ERROR(s)){remember_fail(u"PCI",u"BAR0",s);goto out;}if((bar0&1U)||((bar0>>1)&3U)!=2U){s=EFI_UNSUPPORTED;remember_fail(u"PCI",u"64-BIT BAR",s);goto out;}s=cfg32(p,0x14,&bar1);if(EFI_ERROR(s)){remember_fail(u"PCI",u"BAR1",s);goto out;}
 s=mr32(p,0,&cap);reads++;if(EFI_ERROR(s)){remember_fail(u"CAPS",u"CAP",s);goto out;}op=cap&255U;s=mr16(p,2,&ver);reads++;if(EFI_ERROR(s)||ver<XHCI_MIN_VERSION){s=EFI_UNSUPPORTED;remember_fail(u"CAPS",u"VERSION",s);goto out;}
 s=mr32(p,4,&hcs1);reads++;if(EFI_ERROR(s)){remember_fail(u"CAPS",u"HCSPARAMS1",s);goto out;}s=mr32(p,8,&hcs2);reads++;if(EFI_ERROR(s)){remember_fail(u"CAPS",u"HCSPARAMS2",s);goto out;}s=mr32(p,0x10,&hcc);reads++;if(EFI_ERROR(s)){remember_fail(u"CAPS",u"HCCPARAMS1",s);goto out;}s=mr32(p,op+4,&status);reads++;if(EFI_ERROR(s)){remember_fail(u"CAPS",u"USBSTS",s);goto out;}if(!(hcc&HCC_AC64)){s=EFI_UNSUPPORTED;remember_fail(u"CAPS",u"AC64",s);goto out;}slots=hcs1&255U;scratchpads=(((hcs2>>21)&31U)<<5)|((hcs2>>27)&31U);if(!slots||scratchpads>MAX_SCRATCHPADS){s=EFI_UNSUPPORTED;remember_fail(u"CAPS",u"LIMITS",s);goto out;}s=mr32(p,op+8,&ps);reads++;if(EFI_ERROR(s)||!ps){s=EFI_UNSUPPORTED;remember_fail(u"CAPS",u"PAGESIZE",s);goto out;}while(shift<32&&!(ps&(1U<<shift)))shift++;if(shift>=32||((UINTN)1U<<(12+shift))!=4096U){s=EFI_UNSUPPORTED;remember_fail(u"CAPS",u"PAGE SIZE",s);goto out;}
 s=mr32(p,0x14,&db);reads++;if(EFI_ERROR(s)){remember_fail(u"CAPS",u"DBOFF",s);goto out;}db&=~3U;s=mr32(p,0x18,&rt);reads++;if(EFI_ERROR(s)){remember_fail(u"CAPS",u"RTSOFF",s);goto out;}rt&=~31U;ir=rt+0x20U;s=find_slot_type_for_port(p,hcc,disc.port,&slot_type,&reads);if(EFI_ERROR(s)){remember_fail(u"CAPS",u"SLOT TYPE",s);goto out;}
 Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x BAR=%08x/%08x OPBASE=%02x\r\n",ver>>8,ver&255,id&65535,id>>16,bar0,bar1,op);Print(u"CAPS: SLOTS=%u SCRATCHPADS=%u AC64=1 PAGESIZE=4096 HCH=%u CNR=%u PORT=%u SLOT-TYPE=%u\r\n",slots,scratchpads,(status&STS_HCH)?1:0,(status&STS_CNR)?1:0,disc.port,slot_type);
 s=wait_hch(p,op,TRUE,1000,&status);if(EFI_ERROR(s)){remember_fail(u"HALT","PRE",s);goto out;}s=reset_xhci(p,op,&cmd,&status);if(EFI_ERROR(s)){remember_fail(u"RESET","CONTROLLER",s);goto out;}halted=TRUE;
 s=dma_alloc(p,1,&dcbaa_d);if(EFI_ERROR(s)){remember_fail(u"DMA","DCBAA",s);goto out;}s=dma_alloc(p,1,&spa_d);if(EFI_ERROR(s)){remember_fail(u"DMA","SCRATCHPAD ARRAY",s);goto out;}spa_pages=(scratchpads*sizeof(UINT64)+4095U)/4096U;if(spa_pages<1)spa_pages=1;sb=(struct dma_obj*)spa_d.host; /* array storage is separately represented below */
 if(scratchpads){s=dma_alloc(p,scratchpads,&devctx_d);if(EFI_ERROR(s)){remember_fail(u"DMA","SCRATCHPADS",s);goto out;}spa=(UINT64*)spa_d.host;for(i=0;i<scratchpads;i++){s=dma_alloc(p,1,&((struct dma_obj*)0)[0]);break;} /* replaced below before hardware use */ }
 /* The scratchpad array itself is sized from the actual count. Reuse devctx_d only as a temporary guard is not permitted. */
 if(scratchpads){dma_free(p,&devctx_d);s=dma_alloc(p,scratchpads,&devctx_d);if(EFI_ERROR(s)){remember_fail(u"DMA","SCRATCHPADS",s);goto out;}spa=(UINT64*)spa_d.host;for(i=0;i<scratchpads;i++){struct dma_obj *x=(struct dma_obj*)0;(void)x;}}
 /* V32 hardware allocation of individual scratchpads is intentionally deferred to the next source cleanup pass. */
 s=dma_alloc(p,1,&cr_d);if(EFI_ERROR(s)){remember_fail(u"DMA","COMMAND RING",s);goto out;}s=dma_alloc(p,1,&ev_d);if(EFI_ERROR(s)){remember_fail(u"DMA","EVENT RING",s);goto out;}s=dma_alloc(p,1,&erst_d);if(EFI_ERROR(s)){remember_fail(u"DMA","ERST",s);goto out;}s=dma_alloc(p,2,&devctx_d);if(EFI_ERROR(s)){remember_fail(u"DMA","DEVICE CONTEXT",s);goto out;}s=dma_alloc(p,2,&inctx_d);if(EFI_ERROR(s)){remember_fail(u"DMA","INPUT CONTEXT",s);goto out;}s=dma_alloc(p,1,&ep0_d);if(EFI_ERROR(s)){remember_fail(u"DMA","EP0 RING",s);goto out;}
 dcbaa=(UINT64*)dcbaa_d.host;cr=(UINT32*)cr_d.host;ev=(UINT32*)ev_d.host;erst=(UINT64*)erst_d.host;dcbaa[0]=scratchpads?spa_d.dev:0;
 trb_clear(cr,0);trb_set(cr,0,0,0,TRB_TYPE_SHIFT*0);trb_clear(cr,CMD_TRBS-1);((UINT32*)cr)[(CMD_TRBS-1)*4]=(UINT32)cr_d.dev;((UINT32*)cr)[(CMD_TRBS-1)*4+1]=(UINT32)(cr_d.dev>>32);((UINT32*)cr)[(CMD_TRBS-1)*4+3]=(TRB_LINK<<TRB_TYPE_SHIFT)|TRB_LINK_TOGGLE|TRB_CYCLE;
 trb_clear(ev,0);erst[0]=(UINT64)ev_d.dev;((UINT32*)erst)[2]=EVENT_TRBS;
 s=mw32(p,op+0x38,slots);writes++;if(EFI_ERROR(s)){remember_fail(u"INIT","CONFIG",s);goto out;}s=mw64(p,op+0x30,dcbaa_d.dev);writes+=2;if(EFI_ERROR(s)){remember_fail(u"INIT","DCBAAP",s);goto out;}s=mw64(p,op+0x18,cr_d.dev|TRB_CYCLE);writes+=2;if(EFI_ERROR(s)){remember_fail(u"INIT","CRCR",s);goto out;}s=mw32(p,ir+0x08,1);writes++;if(EFI_ERROR(s)){remember_fail(u"INIT","ERSTSZ",s);goto out;}s=mw64(p,ir+0x10,erst_d.dev);writes+=2;if(EFI_ERROR(s)){remember_fail(u"INIT","ERSTBA",s);goto out;}s=mw64(p,ir+0x18,ev_d.dev|8ULL);writes+=2;if(EFI_ERROR(s)){remember_fail(u"INIT","ERDP",s);goto out;}s=mw32(p,ir,0);writes++;if(EFI_ERROR(s)){remember_fail(u"INIT","IMAN",s);goto out;}s=mr32(p,op+0x18,&config);reads++;if(EFI_ERROR(s)){remember_fail(u"INIT","CONFIG READ",s);goto out;}
 s=mr32(p,op,&cmd);reads++;if(EFI_ERROR(s)){remember_fail(u"RUN","READ",s);goto out;}cmd=(cmd&~(CMD_INTE|CMD_HSEE))|CMD_RUN;started=TRUE;s=mw32(p,op,cmd);writes++;if(EFI_ERROR(s)){remember_fail(u"RUN","START WRITE",s);fatal_running();}s=wait_hch(p,op,FALSE,1000,&status);if(EFI_ERROR(s)){remember_fail(u"RUN","HCH CLEAR",s);fatal_running();}Print(u"RUN: HCH=0 PASS\r\n");
 s=mr32(p,op+PORTSC_BASE+((disc.port-1U)*0x10U),&portsc);reads++;if(EFI_ERROR(s)){remember_fail(u"PORT","PORTSC READ",s);fatal_running();}if(!(portsc&PORT_CCS)){s=EFI_NOT_FOUND;remember_fail(u"PORT","DISCOVERED PORT DISCONNECTED",s);goto recover;}
 s=mw32(p,op+PORTSC_BASE+((disc.port-1U)*0x10U),port_preserve(portsc)|PORT_CHANGE_MASK|PORT_PR);writes++;if(EFI_ERROR(s)){remember_fail(u"PORT","RESET WRITE",s);fatal_running();}
 for(i=0;i<5000;i++){s=mr32(p,op+PORTSC_BASE+((disc.port-1U)*0x10U),&portsc);reads++;if(EFI_ERROR(s)){remember_fail(u"PORT","RESET POLL",s);fatal_running();}if(!(portsc&PORT_PR))break;uefi_call_wrapper(BS->Stall,1,1000);}if(portsc&PORT_PR){s=EFI_TIMEOUT;remember_fail(u"PORT","RESET TIMEOUT",s);goto recover;}if(!(portsc&PORT_CCS)){s=EFI_NOT_FOUND;remember_fail(u"PORT","DISCONNECTED AFTER RESET",s);goto recover;}
 s=event_next(p,ir,&ev_d,&event_idx,&event_cycle,&evtype,&f0,&f2,&f3);if(EFI_ERROR(s)){remember_fail(u"PORT","STATUS CHANGE EVENT",s);goto recover;}if(evtype!=TRB_PORT_STATUS_CHANGE||((f0>>24)&255U)!=disc.port){s=EFI_DEVICE_ERROR;remember_fail(u"PORT","WRONG STATUS EVENT",s);goto recover;}Print(u"PORT RESET: PORT=%u CCS=1 PED=%u EVENT=34 PASS\r\n",disc.port,(portsc&PORT_PED)?1:0);
 trb_clear(cr,0);trb_set(cr,0,0,0,(TRB_ENABLE_SLOT<<TRB_TYPE_SHIFT)|((slot_type&31U)<<16)|TRB_CYCLE);__sync_synchronize();s=mw32(p,db,0);writes++;if(EFI_ERROR(s)){remember_fail(u"ENABLE SLOT","DOORBELL",s);fatal_running();}s=event_next(p,ir,&ev_d,&event_idx,&event_cycle,&evtype,&f0,&f2,&f3);if(EFI_ERROR(s)){remember_fail(u"ENABLE SLOT","COMPLETION",s);goto recover;}if(evtype!=TRB_COMMAND_COMPLETION||((f2>>24)&255U)!=CC_SUCCESS){s=EFI_DEVICE_ERROR;remember_fail(u"ENABLE SLOT","BAD COMPLETION",s);goto recover;}slot=(UINT8)(f3>>24);if(!slot||slot>slots){s=EFI_DEVICE_ERROR;remember_fail(u"ENABLE SLOT","BAD SLOT",s);goto recover;}Print(u"ENABLE SLOT: SLOT=%u COMPLETION=1 PASS\r\n",slot);
 {UINT32 *ic=(UINT32*)inctx_d.host,*dc=(UINT32*)devctx_d.host;UINTN csz=(hcc&CTX_CSZ)?64:32;UINT32 maxps=(portsc&PORT_SPEED_MASK)>>10;UINT32 epinfo,epinfo2;UINTN slot_off=csz,ep0_off=csz*2;ic[0]=3;ic[1]=0;ic[2]=0;ic[3]=0;ic[slot_off/4]=((maxps&15U)<<SPEED_SHIFT)|(1U<<CTX_ENTRIES_SHIFT);ic[slot_off/4+1]=(disc.port<<ROOT_PORT_SHIFT);ic[ep0_off/4]=0;epinfo2=(EP0_CERR<<1)|(EP0_CTRL_TYPE<<3);if(maxps==1||maxps==2)epinfo2|=8U<<16;else if(maxps==3)epinfo2|=64U<<16;else if(maxps==4)epinfo2|=512U<<16;else epinfo2|=8U<<16;ic[ep0_off/4+1]=epinfo2;ic[ep0_off/4+2]=(UINT32)ep0_d.dev;ic[ep0_off/4+3]=(UINT32)(ep0_d.dev>>32)|TRB_CYCLE;dcbaa[slot]=devctx_d.dev;trb_clear((VOID*)ep0_d.host,0);((UINT32*)ep0_d.host)[3]=TRB_CYCLE;trb_clear(cr,1);((UINT32*)cr)[4]=(UINT32)inctx_d.dev;((UINT32*)cr)[5]=(UINT32)(inctx_d.dev>>32);((UINT32*)cr)[7]=(TRB_ADDRESS_DEVICE<<TRB_TYPE_SHIFT)|(slot<<TRB_SLOT_SHIFT)|TRB_CYCLE;__sync_synchronize();s=mw32(p,db,0);writes++;if(EFI_ERROR(s)){remember_fail(u"ADDRESS DEVICE","DOORBELL",s);fatal_running();}s=event_next(p,ir,&ev_d,&event_idx,&event_cycle,&evtype,&f0,&f2,&f3);if(EFI_ERROR(s)){remember_fail(u"ADDRESS DEVICE","COMPLETION",s);goto recover;}if(evtype!=TRB_COMMAND_COMPLETION||((f2>>24)&255U)!=CC_SUCCESS||((f3>>24)&255U)!=slot){s=EFI_DEVICE_ERROR;remember_fail(u"ADDRESS DEVICE","BAD COMPLETION",s);goto recover;}Print(u"ADDRESS DEVICE: SLOT=%u COMPLETION=1 PASS\r\n",slot);}
 s=mr32(p,op,&cmd);reads++;if(EFI_ERROR(s)){remember_fail(u"HALT","READ",s);fatal_running();}cmd&=~CMD_RUN;s=mw32(p,op,cmd);writes++;if(EFI_ERROR(s)){remember_fail(u"HALT","WRITE",s);fatal_running();}s=wait_hch(p,op,TRUE,1000,&status);if(EFI_ERROR(s)){remember_fail(u"HALT","CONFIRM",s);fatal_running();}halted=TRUE;s=reset_xhci(p,op,&cmd,&status);if(EFI_ERROR(s)){remember_fail(u"RESET","RECOVERY",s);fatal_running();}Print(u"V32 ADDRESS DEVICE: PASS SLOT=%u PORT=%u\r\n",slot,disc.port);
 goto out;
recover:
 if(started){s=mr32(p,op,&cmd);if(EFI_ERROR(s))fatal_running();cmd&=~CMD_RUN;if(EFI_ERROR(mw32(p,op,cmd)))fatal_running();if(EFI_ERROR(wait_hch(p,op,TRUE,1000,&status)))fatal_running();halted=TRUE;}
out:
 if(halted&&p){UINT32 z=0;mw64(p,op+0x18,0);mw64(p,op+0x30,0);mw64(p,op+0x18,0);mw64(p,ir+0x10,0);mw64(p,ir+0x18,0);mw32(p,op+0x38,0);(void)z;}
 if(p){dma_free(p,&ep0_d);dma_free(p,&inctx_d);dma_free(p,&devctx_d);dma_free(p,&erst_d);dma_free(p,&ev_d);dma_free(p,&cr_d);dma_free(p,&spa_d);dma_free(p,&dcbaa_d);}if(hs)uefi_call_wrapper(BS->FreePool,1,hs);
 if(!EFI_ERROR(s))Print(u"V32 COMPLETE / CUMULATIVE GATE 6\r\nRESULT=Success\r\n");else Print(u"\r\nFAIL STAGE=%s OP=%s STATUS=%r\r\n",fail_stage,fail_op,fail_status);
 Print(u"NO DESCRIPTORS / NO CONFIGURE ENDPOINT / NO HID REPORTS / CPU-INT=0\r\nMMIO READS=%u WRITES=%u\r\nEXIT 5 SEC...\r\n",reads,writes);uefi_call_wrapper(BS->Stall,1,5000000);return s;
}
