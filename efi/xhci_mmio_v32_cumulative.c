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
#define HCC_AC64 1U
#define CTX_CSZ (1U<<2)
#define TRB_CYCLE 1U
#define TRB_TYPE_SHIFT 10U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 2U
#define TRB_ENABLE_SLOT 9U
#define TRB_ADDRESS_DEVICE 11U
#define TRB_COMMAND_COMPLETION 33U
#define TRB_PORT_STATUS_CHANGE 34U
#define CC_SUCCESS 1U
#define PORTSC_BASE 0x400U
#define PORT_CCS (1U<<0)
#define PORT_PED (1U<<1)
#define PORT_PR (1U<<4)
#define PORT_SPEED_MASK (0xFU<<10)
#define PORT_CHANGE_MASK (0x7FU<<17)
#define PORT_RO ((1U<<0)|(1U<<3)|(0xFU<<10)|(1U<<30))
#define PORT_RWS ((0xFU<<5)|(1U<<9)|(3U<<14)|(7U<<25))
#define PORT_RW (1U<<16)
#define TRB_SLOT_SHIFT 24U
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
#define DP_TYPE_END 0x7FU

static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID UsbIoGuid=EFI_USB_IO_PROTOCOL_GUID;
static const CHAR16 *fail_stage=u"NONE",*fail_op=u"NONE";
static EFI_STATUS fail_status=EFI_SUCCESS;
struct dma_obj{VOID *host;VOID *map;EFI_PHYSICAL_ADDRESS dev;UINTN pages;BOOLEAN live;};
struct discovery{UINT8 port,interface_number,endpoint,interval;UINT16 mps,vid,pid;UINT8 config;BOOLEAN found;};

static void fail(const CHAR16 *a,const CHAR16 *b,EFI_STATUS s){if(!EFI_ERROR(fail_status)){fail_stage=a;fail_op=b;fail_status=s;}}
static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32*v){return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,o,1,v);}
static EFI_STATUS r32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32*v){return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,v);}
static EFI_STATUS r16(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT16*v){return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint16,0,(UINT64)o,1,v);}
static EFI_STATUS w32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32 v){return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,&v);}
static EFI_STATUS w64(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT64 v){EFI_STATUS s=w32(p,o,(UINT32)v);if(EFI_ERROR(s))return s;return w32(p,o+4,(UINT32)(v>>32));}
static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL*p,UINTN pages,struct dma_obj*d){EFI_STATUS s;UINTN bytes,m;d->host=NULL;d->map=NULL;d->dev=0;d->pages=pages;d->live=FALSE;if(!pages||pages>((UINTN)-1)/4096U)return EFI_BAD_BUFFER_SIZE;bytes=pages*4096U;s=uefi_call_wrapper(p->AllocateBuffer,6,p,AllocateAnyPages,EfiBootServicesData,pages,&d->host,0);if(EFI_ERROR(s))return s;s=uefi_call_wrapper(BS->SetMem,3,d->host,bytes,0);if(EFI_ERROR(s)){uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host);return s;}m=bytes;s=uefi_call_wrapper(p->Map,6,p,EfiPciIoOperationBusMasterCommonBuffer,d->host,&m,&d->dev,&d->map);if(EFI_ERROR(s)||m!=bytes){EFI_STATUS x=EFI_ERROR(s)?s:EFI_DEVICE_ERROR;if(!EFI_ERROR(s)&&d->map)uefi_call_wrapper(p->Unmap,2,p,d->map);uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host);return x;}d->live=TRUE;return EFI_SUCCESS;}
static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL*p,struct dma_obj*d){EFI_STATUS s=EFI_SUCCESS,t;if(!d->live)return EFI_SUCCESS;if(d->map){t=uefi_call_wrapper(p->Unmap,2,p,d->map);if(EFI_ERROR(t))s=t;}if(d->host){t=uefi_call_wrapper(p->FreeBuffer,3,p,d->pages,d->host);if(EFI_ERROR(t)&&!EFI_ERROR(s))s=t;}d->host=NULL;d->map=NULL;d->dev=0;d->live=FALSE;return s;}
static EFI_STATUS wait_hch(EFI_PCI_IO_PROTOCOL*p,UINT32 op,BOOLEAN halt,UINTN n,UINT32*st){UINTN i;EFI_STATUS s;UINT32 want=halt?STS_HCH:0;for(i=0;i<n;i++){s=r32(p,op+4,st);if(EFI_ERROR(s))return s;if((*st&STS_HCH)==want)return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static EFI_STATUS reset_xhci(EFI_PCI_IO_PROTOCOL*p,UINT32 op,UINT32*cmd,UINT32*st){UINTN i;EFI_STATUS s=s=r32(p,op,cmd);if(EFI_ERROR(s))return s;*cmd=(*cmd&~(CMD_RUN|CMD_INTE|CMD_HSEE))|CMD_RESET;s=w32(p,op,*cmd);if(EFI_ERROR(s))return s;for(i=0;i<1000;i++){s=r32(p,op,cmd);if(EFI_ERROR(s))return s;if(!(*cmd&CMD_RESET))break;uefi_call_wrapper(BS->Stall,1,1000);}if(*cmd&CMD_RESET)return EFI_TIMEOUT;for(i=0;i<10000;i++){s=r32(p,op+4,st);if(EFI_ERROR(s))return s;if(!(*st&STS_CNR))return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static EFI_STATUS slot_type(EFI_PCI_IO_PROTOCOL*p,UINT32 hcc,UINT32 port,UINT32*out){UINT32 off=((hcc>>16)&0xffffU)*4U,w,a,b,next;UINTN n=0;EFI_STATUS s;while(off&&n++<64){s=r32(p,off,&w);if(EFI_ERROR(s))return s;if((w&0xffU)==2U){s=r32(p,off+8,&a);if(EFI_ERROR(s))return s;s=r32(p,off+12,&b);if(EFI_ERROR(s))return s;if((a&0xffU)&&port>=(a&0xffU)&&port<=(a&0xffU)+((a>>8)&0xffU)-1U){*out=b&0x1fU;return EFI_SUCCESS;}}next=((w>>8)&0xffU)*4U;if(!next)break;off+=next;}return EFI_NOT_FOUND;}
static UINT8 root_port(EFI_DEVICE_PATH_PROTOCOL*path){UINT8*p=(UINT8*)path;while(p){UINT16 len=(UINT16)p[2]|((UINT16)p[3]<<8);if(len<4||len>255)break;if(p[0]==DP_TYPE_END)break;if(p[0]==DP_TYPE_MESSAGING&&p[1]==DP_SUBTYPE_USB&&len>=6)return p[4];p+=len;}return 0;}
static EFI_STATUS discover(struct discovery*d,EFI_HANDLE image){EFI_HANDLE*hs=NULL;UINTN n=0,i,found=0;EFI_STATUS s=LibLocateHandle(ByProtocol,&UsbIoGuid,NULL,&n,&hs);if(EFI_ERROR(s))return EFI_NOT_FOUND;for(i=0;i<n;i++){EFI_USB_IO_PROTOCOL*usb=NULL;EFI_USB_INTERFACE_DESCRIPTOR in;EFI_USB_DEVICE_DESCRIPTOR dd;EFI_USB_CONFIG_DESCRIPTOR cd;EFI_USB_ENDPOINT_DESCRIPTOR ep;EFI_DEVICE_PATH_PROTOCOL*path;UINTN j;UINT8 port;if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&UsbIoGuid,(void**)&usb,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL)))continue;if(EFI_ERROR(uefi_call_wrapper(usb->UsbGetInterfaceDescriptor,3,usb,&in)))continue;if(in.InterfaceClass!=USB_CLASS_HID||in.InterfaceSubClass!=HID_SUBCLASS_BOOT||in.InterfaceProtocol!=HID_PROTOCOL_KEYBOARD)continue;if(EFI_ERROR(uefi_call_wrapper(usb->UsbGetDeviceDescriptor,2,usb,&dd)))continue;if(EFI_ERROR(uefi_call_wrapper(usb->UsbGetConfigDescriptor,2,usb,&cd)))continue;path=DevicePathFromHandle(hs[i]);port=root_port(path);if(!port)continue;d->port=port;d->interface_number=in.InterfaceNumber;d->vid=dd.IdVendor;d->pid=dd.IdProduct;d->config=cd.ConfigurationValue;d->endpoint=0;d->mps=0;d->interval=0;for(j=0;j<in.NumEndpoints&&j<16;j++){if(EFI_ERROR(uefi_call_wrapper(usb->UsbGetEndpointDescriptor,3,usb,(UINT8)j,&ep)))continue;if((ep.EndpointAddress&0x80U)&&((ep.Attributes&3U)==3U)){d->endpoint=ep.EndpointAddress;d->mps=ep.MaxPacketSize;d->interval=ep.Interval;break;}}if(!d->endpoint)continue;if(++found>1){if(hs)uefi_call_wrapper(BS->FreePool,1,hs);return EFI_ABORTED;}}if(hs)uefi_call_wrapper(BS->FreePool,1,hs);if(found!=1)return EFI_NOT_FOUND;d->found=TRUE;return EFI_SUCCESS;}
static UINT32 port_preserve(UINT32 x){return(x&PORT_RO)|(x&PORT_RWS)|(x&PORT_RW);}
static void clear_trb(VOID*b,UINTN i){UINT32*r=(UINT32*)b;r[i*4]=r[i*4+1]=r[i*4+2]=r[i*4+3]=0;}
static EFI_STATUS next_event(EFI_PCI_IO_PROTOCOL*p,UINT32 ir,struct dma_obj*e,UINTN*idx,UINT8*cy,UINT32*type,UINT32*f0,UINT32*f2,UINT32*f3){UINT32*r=(UINT32*)e->host;UINTN n;for(n=0;n<5000;n++){UINT32 d=r[*idx*4+3];if((d&1U)==*cy){*f0=r[*idx*4];*f2=r[*idx*4+2];*f3=d;*type=(d>>10)&0x3fU;clear_trb(e->host,*idx);*idx+=1;if(*idx==EVENT_TRBS){*idx=0;*cy^=1;}return w64(p,ir+0x18,(e->dev+(*idx)*16ULL)|8ULL);}uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static void fatal_running(void){Print(u"\r\nFATAL: XHCI RUNNING STATE UNCERTAIN\r\nFAIL STAGE=%s OP=%s STATUS=%r\r\nDMA MAPPINGS RETAINED / NO FREE\r\nMANUAL RECOVERY REQUIRED\r\n",fail_stage,fail_op,fail_status);for(;;)uefi_call_wrapper(BS->Stall,1,1000000);}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE*st){EFI_STATUS s=EFI_SUCCESS;EFI_HANDLE*hs=NULL;EFI_PCI_IO_PROTOCOL*p=NULL;UINTN n=0,i;UINT32 id=0,cls=0,bar0=0,bar1=0,cap=0,hcs1=0,hcs2=0,hcc=0,op=0,db=0,rt=0,ir=0,cmd=0,status=0,ps=0,slot_t=0,slot=0,portsc=0,evtype=0,f0=0,f2=0,f3=0;UINT16 ver=0;UINT32 slots,scratchpads;UINTN shift=0,spa_pages;struct discovery d={0};struct dma_obj dcbaa={0},spa={0},scratch={0},cr={0},ev={0},erst={0},devctx={0},inctx={0},ep0={0};UINT64*dcbaa_ptr,*spa_ptr,*erst_ptr;UINT32*cr_ptr;UINTN ev_idx=0;UINT8 ev_cy=1;BOOLEAN started=FALSE,halted=FALSE;InitializeLib(image,st);Print(u"TOSHIBA xHCI V32 / CUMULATIVE DISCOVERY + PORT RESET + ADDRESS DEVICE\r\nV28 DISCOVERY + V29 INIT + V30 RUN/HALT + V31 ENABLE SLOT\r\nNO DESCRIPTORS / NO CONFIGURE ENDPOINT / NO HID REPORTS\r\n");
 s=discover(&d,image);if(EFI_ERROR(s)){fail(u"DISCOVERY",u"KEYBOARD",s);goto out;}Print(u"DISCOVERY: PORT=%u VID=%04x PID=%04x IF=%u EP=%02x MPS=%u INT=%u CFG=%u\r\n",d.port,d.vid,d.pid,d.interface_number,d.endpoint,d.mps,d.interval,d.config);
 s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);if(EFI_ERROR(s)){fail(u"PCI",u"LOCATE",s);goto out;}for(i=0;i<n;i++){EFI_PCI_IO_PROTOCOL*q=NULL;if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,(void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL)))continue;if(EFI_ERROR(cfg32(q,8,&cls))||EFI_ERROR(cfg32(q,0,&id)))continue;if(((cls>>24)&255U)==XHCI_CLASS&&((cls>>16)&255U)==XHCI_SUBCLASS&&((cls>>8)&255U)==XHCI_PROG_IF){p=q;break;}}if(!p){s=EFI_NOT_FOUND;fail(u"PCI",u"FIND XHCI",s);goto out;}
 s=cfg32(p,0x10,&bar0);if(EFI_ERROR(s)){fail(u"PCI",u"BAR0",s);goto out;}if((bar0&1U)||((bar0>>1)&3U)!=2U){s=EFI_UNSUPPORTED;fail(u"PCI",u"64-BIT BAR",s);goto out;}s=cfg32(p,0x14,&bar1);if(EFI_ERROR(s)){fail(u"PCI",u"BAR1",s);goto out;}s=r32(p,0,&cap);if(EFI_ERROR(s)){fail(u"CAPS",u"CAP",s);goto out;}op=cap&255U;s=r16(p,2,&ver);if(EFI_ERROR(s)||ver<XHCI_MIN_VERSION){s=EFI_UNSUPPORTED;fail(u"CAPS",u"VERSION",s);goto out;}s=r32(p,4,&hcs1);if(EFI_ERROR(s)){fail(u"CAPS",u"HCSPARAMS1",s);goto out;}s=r32(p,8,&hcs2);if(EFI_ERROR(s)){fail(u"CAPS",u"HCSPARAMS2",s);goto out;}s=r32(p,0x10,&hcc);if(EFI_ERROR(s)){fail(u"CAPS",u"HCCPARAMS1",s);goto out;}s=r32(p,op+4,&status);if(EFI_ERROR(s)){fail(u"CAPS",u"USBSTS",s);goto out;}if(!(hcc&HCC_AC64)){s=EFI_UNSUPPORTED;fail(u"CAPS",u"AC64",s);goto out;}slots=hcs1&255U;scratchpads=(((hcs2>>21)&31U)<<5)|((hcs2>>27)&31U);if(!slots||scratchpads>MAX_SCRATCHPADS){s=EFI_UNSUPPORTED;fail(u"CAPS",u"LIMITS",s);goto out;}s=r32(p,op+8,&ps);if(EFI_ERROR(s)||!ps){s=EFI_UNSUPPORTED;fail(u"CAPS",u"PAGESIZE",s);goto out;}while(shift<32&&!(ps&(1U<<shift)))shift++;if(shift>=32||((UINTN)1U<<(12+shift))!=4096U){s=EFI_UNSUPPORTED;fail(u"CAPS",u"PAGE SIZE",s);goto out;}s=r32(p,0x14,&db);if(EFI_ERROR(s)){fail(u"CAPS",u"DBOFF",s);goto out;}db&=~3U;s=r32(p,0x18,&rt);if(EFI_ERROR(s)){fail(u"CAPS",u"RTSOFF",s);goto out;}rt&=~31U;ir=rt+0x20U;s=slot_type(p,hcc,d.port,&slot_t);if(EFI_ERROR(s)){fail(u"CAPS",u"SLOT TYPE",s);goto out;}Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x BAR=%08x/%08x OPBASE=%02x\r\nCAPS: SLOTS=%u SCRATCHPADS=%u AC64=1 PAGESIZE=4096 HCH=%u CNR=%u PORT=%u SLOT-TYPE=%u\r\n",ver>>8,ver&255,id&65535,id>>16,bar0,bar1,op,slots,scratchpads,(status&1U)?1:0,(status&STS_CNR)?1:0,d.port,slot_t);
 s=wait_hch(p,op,TRUE,1000,&status);if(EFI_ERROR(s)){fail(u"HALT",u"PRE",s);goto out;}s=reset_xhci(p,op,&cmd,&status);if(EFI_ERROR(s)){fail(u"RESET",u"CONTROLLER",s);goto out;}halted=TRUE;
 spa_pages=(scratchpads*sizeof(UINT64)+4095U)/4096U;if(spa_pages<1)spa_pages=1;s=dma_alloc(p,1,&dcbaa);if(EFI_ERROR(s)){fail(u"DMA",u"DCBAA",s);goto out;}s=dma_alloc(p,spa_pages,&spa);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCHPAD ARRAY",s);goto out;}s=dma_alloc(p,scratchpads?scratchpads:1,&scratch);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCHPADS",s);goto out;}s=dma_alloc(p,1,&cr);if(EFI_ERROR(s)){fail(u"DMA",u"COMMAND RING",s);goto out;}s=dma_alloc(p,1,&ev);if(EFI_ERROR(s)){fail(u"DMA",u"EVENT RING",s);goto out;}s=dma_alloc(p,1,&erst);if(EFI_ERROR(s)){fail(u"DMA",u"ERST",s);goto out;}s=dma_alloc(p,2,&devctx);if(EFI_ERROR(s)){fail(u"DMA",u"DEVICE CONTEXT",s);goto out;}s=dma_alloc(p,2,&inctx);if(EFI_ERROR(s)){fail(u"DMA",u"INPUT CONTEXT",s);goto out;}s=dma_alloc(p,1,&ep0);if(EFI_ERROR(s)){fail(u"DMA",u"EP0 RING",s);goto out;}
 dcbaa_ptr=(UINT64*)dcbaa.host;spa_ptr=(UINT64*)spa.host;erst_ptr=(UINT64*)erst.host;cr_ptr=(UINT32*)cr.host;for(i=0;i<scratchpads;i++)spa_ptr[i]=scratch.dev+i*4096ULL;dcbaa_ptr[0]=scratchpads?spa.dev:0;clear_trb(cr.host,0);clear_trb(cr.host,CMD_TRBS-1);cr_ptr[(CMD_TRBS-1)*4]=(UINT32)cr.dev;cr_ptr[(CMD_TRBS-1)*4+1]=(UINT32)(cr.dev>>32);cr_ptr[(CMD_TRBS-1)*4+3]=(TRB_LINK<<TRB_TYPE_SHIFT)|TRB_LINK_TOGGLE|TRB_CYCLE;clear_trb(ev.host,0);erst_ptr[0]=ev.dev;((UINT32*)erst.host)[0]=0;((UINT32*)erst.host)[1]=0;((UINT32*)erst.host)[2]=EVENT_TRBS;
 s=w32(p,op+0x38,1);if(EFI_ERROR(s)){fail(u"INIT",u"CONFIG",s);goto out;}s=w64(p,op+0x30,dcbaa.dev);if(EFI_ERROR(s)){fail(u"INIT",u"DCBAAP",s);goto out;}s=w64(p,op+0x18,cr.dev|1ULL);if(EFI_ERROR(s)){fail(u"INIT",u"CRCR",s);goto out;}s=w32(p,ir+8,1);if(EFI_ERROR(s)){fail(u"INIT",u"ERSTSZ",s);goto out;}s=w64(p,ir+0x10,erst.dev);if(EFI_ERROR(s)){fail(u"INIT",u"ERSTBA",s);goto out;}s=w64(p,ir+0x18,ev.dev|8ULL);if(EFI_ERROR(s)){fail(u"INIT",u"ERDP",s);goto out;}s=w32(p,ir,0);if(EFI_ERROR(s)){fail(u"INIT",u"IMAN",s);goto out;}
 s=r32(p,op,&cmd);if(EFI_ERROR(s)){fail(u"RUN",u"READ",s);goto out;}cmd=(cmd&~(CMD_INTE|CMD_HSEE))|CMD_RUN;started=TRUE;s=w32(p,op,cmd);if(EFI_ERROR(s)){fail(u"RUN",u"START WRITE",s);fatal_running();}s=wait_hch(p,op,FALSE,1000,&status);if(EFI_ERROR(s)){fail(u"RUN",u"HCH CLEAR",s);fatal_running();}Print(u"RUN: HCH=0 PASS\r\n");
 s=r32(p,op+PORTSC_BASE+(d.port-1U)*0x10U,&portsc);if(EFI_ERROR(s)){fail(u"PORT",u"PORTSC READ",s);fatal_running();}if(!(portsc&PORT_CCS)){s=EFI_NOT_FOUND;fail(u"PORT",u"DISCOVERED PORT DISCONNECTED",s);goto recover;}s=w32(p,op+PORTSC_BASE+(d.port-1U)*0x10U,port_preserve(portsc)|PORT_CHANGE_MASK|PORT_PR);if(EFI_ERROR(s)){fail(u"PORT",u"RESET WRITE",s);fatal_running();}for(i=0;i<5000;i++){s=r32(p,op+PORTSC_BASE+(d.port-1U)*0x10U,&portsc);if(EFI_ERROR(s)){fail(u"PORT",u"RESET POLL",s);fatal_running();}if(!(portsc&PORT_PR))break;uefi_call_wrapper(BS->Stall,1,1000);}if(portsc&PORT_PR){s=EFI_TIMEOUT;fail(u"PORT",u"RESET TIMEOUT",s);goto recover;}if(!(portsc&PORT_CCS)){s=EFI_NOT_FOUND;fail(u"PORT",u"DISCONNECTED AFTER RESET",s);goto recover;}s=next_event(p,ir,&ev,&ev_idx,&ev_cy,&evtype,&f0,&f2,&f3);if(EFI_ERROR(s)||evtype!=TRB_PORT_STATUS_CHANGE||((f0>>24)&255U)!=d.port){s=EFI_DEVICE_ERROR;fail(u"PORT",u"STATUS CHANGE EVENT",s);goto recover;}Print(u"PORT RESET: PORT=%u CCS=1 PED=%u EVENT=34 PASS\r\n",d.port,(portsc&PORT_PED)?1:0);
 clear_trb(cr.host,0);cr_ptr[3]=(TRB_ENABLE_SLOT<<TRB_TYPE_SHIFT)|((slot_t&31U)<<16)|TRB_CYCLE;__sync_synchronize();s=w32(p,db,0);if(EFI_ERROR(s)){fail(u"ENABLE SLOT",u"DOORBELL",s);fatal_running();}s=next_event(p,ir,&ev,&ev_idx,&ev_cy,&evtype,&f0,&f2,&f3);if(EFI_ERROR(s)||evtype!=TRB_COMMAND_COMPLETION||((f2>>24)&255U)!=CC_SUCCESS){s=EFI_DEVICE_ERROR;fail(u"ENABLE SLOT",u"COMPLETION",s);goto recover;}slot=(UINT8)(f3>>24);if(!slot||slot>slots){s=EFI_DEVICE_ERROR;fail(u"ENABLE SLOT",u"SLOT ID",s);goto recover;}Print(u"ENABLE SLOT: SLOT=%u COMPLETION=1 PASS\r\n",slot);
 {UINT32*ic=(UINT32*)inctx.host;UINTN csz=(hcc&CTX_CSZ)?64:32,so=csz,eo=csz*2;UINT32 spd=(portsc&PORT_SPEED_MASK)>>10,epinfo2=0,mps=8;if(spd==3)mps=64;else if(spd==4)mps=512;ic[0]=3;ic[1]=0;ic[so/4]=(spd<<SPEED_SHIFT)|(1U<<CTX_ENTRIES_SHIFT);ic[so/4+1]=(d.port<<ROOT_PORT_SHIFT);ic[eo/4]=(EP0_CERR<<1)|(EP0_CTRL_TYPE<<3);ic[eo/4+1]=epinfo2|(mps<<16);clear_trb(ep0.host,0);clear_trb(ep0.host,255);((UINT32*)ep0.host)[255*4]=(UINT32)ep0.dev;((UINT32*)ep0.host)[255*4+1]=(UINT32)(ep0.dev>>32);((UINT32*)ep0.host)[255*4+3]=(TRB_LINK<<TRB_TYPE_SHIFT)|TRB_LINK_TOGGLE|TRB_CYCLE;ic[eo/4+2]=(UINT32)ep0.dev;ic[eo/4+3]=(UINT32)(ep0.dev>>32)|TRB_CYCLE;dcbaa_ptr[slot]=devctx.dev;clear_trb(cr.host,1);cr_ptr[4]=(UINT32)inctx.dev;cr_ptr[5]=(UINT32)(inctx.dev>>32);cr_ptr[7]=(TRB_ADDRESS_DEVICE<<TRB_TYPE_SHIFT)|(slot<<TRB_SLOT_SHIFT)|TRB_CYCLE;__sync_synchronize();s=w32(p,db,0);if(EFI_ERROR(s)){fail(u"ADDRESS DEVICE",u"DOORBELL",s);fatal_running();}s=next_event(p,ir,&ev,&ev_idx,&ev_cy,&evtype,&f0,&f2,&f3);if(EFI_ERROR(s)||evtype!=TRB_COMMAND_COMPLETION||((f2>>24)&255U)!=CC_SUCCESS||((f3>>24)&255U)!=slot){s=EFI_DEVICE_ERROR;fail(u"ADDRESS DEVICE",u"COMPLETION",s);goto recover;}}
 Print(u"ADDRESS DEVICE: SLOT=%u COMPLETION=1 PASS\r\n",slot);s=r32(p,op,&cmd);if(EFI_ERROR(s)){fail(u"HALT",u"READ",s);fatal_running();}cmd&=~CMD_RUN;s=w32(p,op,cmd);if(EFI_ERROR(s)){fail(u"HALT",u"WRITE",s);fatal_running();}s=wait_hch(p,op,TRUE,1000,&status);if(EFI_ERROR(s)){fail(u"HALT",u"CONFIRM",s);fatal_running();}halted=TRUE;s=reset_xhci(p,op,&cmd,&status);if(EFI_ERROR(s)){fail(u"RESET",u"RECOVERY",s);fatal_running();}Print(u"V32 ADDRESS DEVICE: PASS SLOT=%u PORT=%u\r\n",slot,d.port);goto out;
recover:
 if(started){s=r32(p,op,&cmd);if(EFI_ERROR(s))fatal_running();cmd&=~CMD_RUN;if(EFI_ERROR(w32(p,op,cmd)))fatal_running();if(EFI_ERROR(wait_hch(p,op,TRUE,1000,&status)))fatal_running();halted=TRUE;}
out:
 if(halted&&p){w64(p,op+0x30,0);w64(p,op+0x18,0);w64(p,ir+0x10,0);w64(p,ir+0x18,0);w32(p,op+0x38,0);}
 if(p){dma_free(p,&ep0);dma_free(p,&inctx);dma_free(p,&devctx);dma_free(p,&erst);dma_free(p,&ev);dma_free(p,&cr);dma_free(p,&scratch);dma_free(p,&spa);dma_free(p,&dcbaa);}if(hs)uefi_call_wrapper(BS->FreePool,1,hs);
 if(!EFI_ERROR(s))Print(u"V32 COMPLETE / CUMULATIVE GATE 6\r\nRESULT=Success\r\n");else Print(u"\r\nFAIL STAGE=%s OP=%s STATUS=%r\r\n",fail_stage,fail_op,fail_status);Print(u"NO DESCRIPTORS / NO CONFIGURE ENDPOINT / NO HID REPORTS / CPU-INT=0\r\nEXIT 5 SEC...\r\n");uefi_call_wrapper(BS->Stall,1,5000000);return s;}
