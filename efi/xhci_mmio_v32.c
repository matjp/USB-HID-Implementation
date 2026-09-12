/*
 * Version 32 — standalone cumulative Gate 6 test.
 *
 * HARD RULE: versioned tests are independent source files.  This file does
 * not include, wrap, or link any previous versioned test source.
 * Earlier versions are references only.  All code required by this test is
 * defined here.
 */
#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include "usb_io_compat.h"

#define XHCI_MIN_VERSION 0x0100U
#define CMD_RUN 1U
#define CMD_RESET 2U
#define CMD_INTE 4U
#define CMD_HSEE 8U
#define STS_HCH 1U
#define STS_CNR 0x800U
#define IMAN_IP 1U
#define IMAN_IE 2U
#define CRCR_RCS 1ULL
#define TRB_CYCLE 1U
#define TRB_TYPE_SHIFT 10U
#define TRB_TYPE_MASK 0x0000fc00U
#define TRB_ENABLE_SLOT 9U
#define TRB_ADDRESS_DEVICE 11U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 2U
#define TRB_COMMAND_COMPLETION 33U
#define TRB_PORT_STATUS 34U
#define CC_SUCCESS 1U
#define PORT_CCS 1U
#define PORT_PED 2U
#define PORT_PR 0x10U
#define PORT_PLS_MASK 0x1e0U
#define PORT_SPEED_MASK 0x3c00U
#define PORT_SPEED_SHIFT 10U
#define PORT_PRC 0x200000U
#define USB_SPEED_FULL 1U
#define USB_SPEED_LOW 2U
#define CTX_SLOT_ADD 1U
#define CTX_EP0_ADD 2U
#define EP_TYPE_CONTROL 4U
#define SLOT_STATE_ADDRESSED 2U
#define CMD_TRBS 256U
#define EVENT_TRBS 16U
#define MAX_PATH_BYTES 512U
#define MAX_ENDPOINTS 8U
#define HANDOFF_MAGIC 0x48494458U
#define HANDOFF_VERSION 2U

struct dma_obj { VOID *host; VOID *map; EFI_PHYSICAL_ADDRESS dev; UINTN pages; BOOLEAN live; };
struct trb { UINT32 d[4]; };
typedef struct { UINT16 size; UINT16 reserved; UINT8 data[MAX_PATH_BYTES]; } PATH_COPY;
typedef struct { UINT8 endpoint_address, attributes, interval, reserved; UINT16 max_packet_size; } EP_FACT;
typedef struct { UINT8 kind, root_port, speed, interface_number; UINT8 interface_protocol, interrupt_in_endpoint, endpoint_count, reserved0; UINT16 vendor_id, product_id; UINT8 ep0_mps_descriptor, reserved1; UINT16 interrupt_max_packet_size; UINT8 interval, reserved2; EP_FACT endpoints[MAX_ENDPOINTS]; PATH_COPY path; } HID_FACT;
typedef struct { UINT32 magic; UINT16 version, size, device_count, pci_segment; UINT8 pci_bus, pci_device, pci_function, reserved0; UINT16 pci_vendor, pci_device_id; PATH_COPY controller_path; HID_FACT keyboard, mouse; } HANDOFF;

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID UsbIoGuid = EFI_USB_IO_PROTOCOL_GUID;
static EFI_GUID DevicePathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static const CHAR16 *fail_stage=u"NONE", *fail_op=u"NONE"; static EFI_STATUS fail_status=EFI_SUCCESS;
static void fail(const CHAR16 *a,const CHAR16 *b,EFI_STATUS s){if(!EFI_ERROR(fail_status)){fail_stage=a;fail_op=b;fail_status=s;}}
static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT32 *v){return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,o,1,v);}
static EFI_STATUS mr32(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT32 *v){return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,o,1,v);}
static EFI_STATUS mr16(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT16 *v){return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint16,0,o,1,v);}
static EFI_STATUS mw32(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT32 v){return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,o,1,&v);}
static EFI_STATUS mw64(EFI_PCI_IO_PROTOCOL *p,UINT32 o,UINT64 v){EFI_STATUS s=mw32(p,o,(UINT32)v);return EFI_ERROR(s)?s:mw32(p,o+4U,(UINT32)(v>>32));}
static UINT16 plen(EFI_DEVICE_PATH_PROTOCOL *p){return (UINT16)p->Length[0]|((UINT16)p->Length[1]<<8);}
static EFI_STATUS path_size(EFI_DEVICE_PATH_PROTOCOL *p,UINTN *out){UINT8*q=(UINT8*)p;UINTN n=0;UINT16 l;if(!p||!out)return EFI_INVALID_PARAMETER;while(n+4U<=MAX_PATH_BYTES){l=plen((EFI_DEVICE_PATH_PROTOCOL*)q);if(l<4U||n+l>MAX_PATH_BYTES)return EFI_DEVICE_ERROR;n+=l;if(q[0]==0x7fU){*out=n;return EFI_SUCCESS;}q+=l;}return EFI_BAD_BUFFER_SIZE;}
static EFI_STATUS copy_path(PATH_COPY*d,EFI_DEVICE_PATH_PROTOCOL*p){UINTN n;EFI_STATUS s=path_size(p,&n);if(EFI_ERROR(s))return s;uefi_call_wrapper(BS->SetMem,3,d,sizeof(*d),0);CopyMem(d->data,p,n);d->size=(UINT16)n;return EFI_SUCCESS;}
static UINT8 root_port(EFI_DEVICE_PATH_PROTOCOL*p){UINT8*q=(UINT8*)p;while(q){EFI_DEVICE_PATH_PROTOCOL*h=(EFI_DEVICE_PATH_PROTOCOL*)q;UINT16 l=plen(h);if(l<4U||h->Type==0x7fU)break;if(h->Type==3U&&h->SubType==5U&&l>=6U)return q[4];q+=l;}return 0xffU;}
static BOOLEAN hid_kind(EFI_USB_INTERFACE_DESCRIPTOR*d,UINT8*k){if(!d||d->InterfaceClass!=3U||d->InterfaceSubClass!=1U)return FALSE;if(d->InterfaceProtocol==1U){*k=1U;return TRUE;}if(d->InterfaceProtocol==2U){*k=2U;return TRUE;}return FALSE;}
static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL*p,UINTN pages,struct dma_obj*d){EFI_STATUS s;UINTN bytes=pages*4096U,mb=bytes;uefi_call_wrapper(BS->SetMem,3,d,sizeof(*d),0);d->pages=pages;if(!pages)return EFI_BAD_BUFFER_SIZE;s=uefi_call_wrapper(p->AllocateBuffer,6,p,AllocateAnyPages,EfiBootServicesData,pages,&d->host,0);if(EFI_ERROR(s))return s;s=uefi_call_wrapper(p->Map,6,p,EfiPciIoOperationBusMasterCommonBuffer,d->host,&mb,&d->dev,&d->map);if(EFI_ERROR(s)||mb!=bytes){if(!EFI_ERROR(s)&&d->map)uefi_call_wrapper(p->Unmap,2,p,d->map);uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host);return EFI_ERROR(s)?s:EFI_DEVICE_ERROR;}uefi_call_wrapper(BS->SetMem,3,d->host,bytes,0);d->live=TRUE;return EFI_SUCCESS;}
static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL*p,struct dma_obj*d){EFI_STATUS s=EFI_SUCCESS,t;if(!d||!d->live)return EFI_SUCCESS;if(d->map){t=uefi_call_wrapper(p->Unmap,2,p,d->map);if(EFI_ERROR(t))s=t;}if(d->host){t=uefi_call_wrapper(p->FreeBuffer,3,p,d->pages,d->host);if(EFI_ERROR(t)&&!EFI_ERROR(s))s=t;}d->host=NULL;d->map=NULL;d->dev=0;d->live=FALSE;return s;}
static EFI_STATUS wait_hch(EFI_PCI_IO_PROTOCOL*p,UINT32 op,BOOLEAN want,UINTN loops,UINT32*st){UINTN i;EFI_STATUS s;for(i=0;i<loops;i++){s=mr32(p,op+4U,st);if(EFI_ERROR(s))return s;if(((*st&STS_HCH)!=0)==want)return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static EFI_STATUS reset_xhci(EFI_PCI_IO_PROTOCOL*p,UINT32 op){UINT32 c,s;UINTN i;EFI_STATUS x=mr32(p,op,&c);if(EFI_ERROR(x))return x;c&=~(CMD_RUN|CMD_INTE|CMD_HSEE);c|=CMD_RESET;x=mw32(p,op,c);if(EFI_ERROR(x))return x;for(i=0;i<1000;i++){x=mr32(p,op,&c);if(EFI_ERROR(x))return x;if(!(c&CMD_RESET))break;uefi_call_wrapper(BS->Stall,1,1000);}if(c&CMD_RESET)return EFI_TIMEOUT;for(i=0;i<10000;i++){x=mr32(p,op+4U,&s);if(EFI_ERROR(x))return x;if(!(s&STS_CNR))return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static EFI_STATUS controller_for_path(EFI_DEVICE_PATH_PROTOCOL*usb,EFI_HANDLE*out){EFI_HANDLE*hs=NULL,best=NULL;UINTN n=0,i,bestn=0;EFI_STATUS s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);if(EFI_ERROR(s))return s;for(i=0;i<n;i++){EFI_DEVICE_PATH_PROTOCOL*p=NULL;UINT8*a=(UINT8*)p;UINT8*b=(UINT8*)usb;UINTN m=0;while(a&&b){EFI_DEVICE_PATH_PROTOCOL*x=(EFI_DEVICE_PATH_PROTOCOL*)a,*y=(EFI_DEVICE_PATH_PROTOCOL*)b;UINT16 xl=plen(x),yl=plen(y);if(xl<4U||yl<4U)break;if(x->Type==0x7fU){if(m>bestn){bestn=m;best=hs[i];}break;}if(y->Type==0x7fU||xl!=yl||CompareMem(x,y,xl)!=0)break;m+=xl;a+=xl;b+=yl;}}FreePool(hs);if(!best)return EFI_NOT_FOUND;*out=best;return EFI_SUCCESS;}
static EFI_STATUS fill_hid(EFI_USB_IO_PROTOCOL*u,EFI_USB_INTERFACE_DESCRIPTOR*i,EFI_DEVICE_PATH_PROTOCOL*p,UINT8 kind,HID_FACT*d){EFI_USB_DEVICE_DESCRIPTOR dd;EFI_USB_ENDPOINT_DESCRIPTOR e;UINTN n,j;BOOLEAN got=FALSE;uefi_call_wrapper(BS->SetMem,3,d,sizeof(*d),0);d->kind=kind;d->root_port=root_port(p);d->interface_number=i->InterfaceNumber;d->interface_protocol=i->InterfaceProtocol;if(d->root_port==0xffU)return EFI_NOT_FOUND;if(EFI_ERROR(copy_path(&d->path,p)))return EFI_DEVICE_ERROR;if(!EFI_ERROR(uefi_call_wrapper(u->UsbGetDeviceDescriptor,3,u,&dd))){d->vendor_id=dd.IdVendor;d->product_id=dd.IdProduct;d->ep0_mps_descriptor=dd.MaxPacketSize0;}n=i->NumEndpoints>MAX_ENDPOINTS?MAX_ENDPOINTS:i->NumEndpoints;d->endpoint_count=(UINT8)n;for(j=0;j<n;j++)if(!EFI_ERROR(uefi_call_wrapper(u->UsbGetEndpointDescriptor,4,u,(UINT8)j,&e))){d->endpoints[j].endpoint_address=e.EndpointAddress;d->endpoints[j].attributes=e.Attributes;d->endpoints[j].max_packet_size=e.MaxPacketSize;d->endpoints[j].interval=e.Interval;if(!got&&(e.EndpointAddress&0x80U)&&((e.Attributes&3U)==3U)&&e.MaxPacketSize&&e.Interval){d->interrupt_in_endpoint=e.EndpointAddress;d->interrupt_max_packet_size=e.MaxPacketSize;d->interval=e.Interval;got=TRUE;}}return got?EFI_SUCCESS:EFI_UNSUPPORTED;}
static EFI_STATUS produce_handoff(EFI_HANDLE image,HANDOFF*h,EFI_HANDLE*controller){EFI_HANDLE*hs=NULL;UINTN n=0,i,k=0,m=0;EFI_STATUS s;EFI_HANDLE ch=NULL;uefi_call_wrapper(BS->SetMem,3,h,sizeof(*h),0);h->magic=HANDOFF_MAGIC;h->version=HANDOFF_VERSION;h->size=(UINT16)sizeof(*h);s=LibLocateHandle(ByProtocol,&UsbIoGuid,NULL,&n,&hs);if(EFI_ERROR(s))return s;for(i=0;i<n;i++){EFI_USB_IO_PROTOCOL*u=NULL;EFI_USB_INTERFACE_DESCRIPTOR id;EFI_DEVICE_PATH_PROTOCOL*p=NULL;UINT8 kind;s=uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&UsbIoGuid,(VOID**)&u,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);if(EFI_ERROR(s))continue;if(EFI_ERROR(uefi_call_wrapper(u->UsbGetInterfaceDescriptor,3,u,&id))||!hid_kind(&id,&kind))continue;s=uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&DevicePathGuid,(VOID**)&p,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);if(EFI_ERROR(s)||!p){FreePool(hs);return EFI_DEVICE_ERROR;}s=controller_for_path(p,&ch);if(EFI_ERROR(s)){FreePool(hs);return s;}if(kind==1U){if(++k!=1U){FreePool(hs);return EFI_DEVICE_ERROR;}s=fill_hid(u,&id,p,1U,&h->keyboard);}else{if(++m!=1U){FreePool(hs);return EFI_DEVICE_ERROR;}s=fill_hid(u,&id,p,2U,&h->mouse);}if(EFI_ERROR(s)){FreePool(hs);return s;}}FreePool(hs);if(k!=1U)return EFI_NOT_FOUND;h->device_count=m?2U:1U;*controller=ch;return EFI_SUCCESS;}
static EFI_STATUS supported_protocol(EFI_PCI_IO_PROTOCOL*p,UINT32 xecp,UINT8 port){UINT32 off=xecp*4U,h,d;UINTN n=0;EFI_STATUS s;while(off&&n++<64U){s=mr32(p,off,&h);if(EFI_ERROR(s))return s;if((h&0xffU)==2U){s=mr32(p,off+8U,&d);if(EFI_ERROR(s))return s;UINT8 po=(UINT8)(d>>8),pc=(UINT8)(d>>16);if(port>=po&&port<(UINT8)(po+pc))return EFI_SUCCESS;}off+=((h>>8)&0xffU)*4U;if(!((h>>8)&0xffU))break;}return EFI_NOT_FOUND;}
static EFI_STATUS wait_cmd_completion(EFI_PCI_IO_PROTOCOL*p,struct trb*ev,UINT32 erdp,UINT32*slot){UINTN i;UINT32 d3,t;for(i=0;i<10000;i++){d3=ev[0].d[3];if(d3&TRB_CYCLE){t=(d3&TRB_TYPE_MASK)>>TRB_TYPE_SHIFT;if(t!=TRB_COMMAND_COMPLETION)return EFI_DEVICE_ERROR;if(((ev[0].d[2]>>24)&0xffU)==0U)return EFI_DEVICE_ERROR;*slot=(d3>>24)&0xffU;/* IMAN.IP is explicitly acknowledged even though IE is disabled. */{UINT32 iman;if(!EFI_ERROR(mr32(p,0x20U,&iman)))mw32(p,0x20U,iman|IMAN_IP);}return (((ev[0].d[2])&0xffU)==CC_SUCCESS)?EFI_SUCCESS:EFI_DEVICE_ERROR;}uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static void fatal_running(void){Print(u"\r\nFATAL: XHCI NOT CONFIRMED HALTED\r\nDMA MAPPINGS RETAINED\r\n");for(;;)uefi_call_wrapper(BS->Stall,1,1000000);}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE*st)
{
    EFI_STATUS s=EFI_SUCCESS,t;EFI_HANDLE controller=NULL;EFI_PCI_IO_PROTOCOL*p=NULL;HANDOFF h;EFI_DEVICE_PATH_PROTOCOL*cp=NULL;UINT32 cap=0,ver32=0,hcs1=0,hcc=0,op=0,xecp=0,db=0,ps=0,cmd=0;UINT16 ver=0;UINT8 port;BOOLEAN running=FALSE,halted=FALSE,submitted=FALSE;struct dma_obj dc={0},crd={0},erd={0},est={0};UINT64*dcbaa;struct trb*cr,*ev;UINT64*erst;UINT32 slot=0;
    InitializeLib(image,st);Print(u"TOSHIBA xHCI V32 / STANDALONE CUMULATIVE GATE 6\r\nUEFI DISCOVERY -> CONTROLLER DISCONNECT -> FRESH xHCI -> SELECTED KEYBOARD PORT\r\nNO BRIDGE USB DISCOVERY / KEYBOARD + MOUSE HANDOFF / NO OTHER DEVICES\r\n");
    s=produce_handoff(image,&h,&controller);if(EFI_ERROR(s)){fail(u"HANDOFF",u"PRODUCE",s);goto out;}s=uefi_call_wrapper(BS->OpenProtocol,6,controller,&PciGuid,(VOID**)&p,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);if(EFI_ERROR(s)){fail(u"HANDOFF",u"PCI",s);goto out;}s=uefi_call_wrapper(BS->OpenProtocol,6,controller,&DevicePathGuid,(VOID**)&cp,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);if(EFI_ERROR(s)||!cp||EFI_ERROR(copy_path(&h.controller_path,cp))){s=EFI_DEVICE_ERROR;fail(u"HANDOFF",u"CONTROLLER PATH",s);goto out;}
    s=cfg32(p,0,&ver32);if(EFI_ERROR(s)){fail(u"PCI",u"ID",s);goto out;}h.pci_vendor=(UINT16)ver32;h.pci_device_id=(UINT16)(ver32>>16);s=uefi_call_wrapper(p->GetLocation,5,p,(UINTN*)&h.pci_segment,(UINTN*)&h.pci_bus,(UINTN*)&h.pci_device,(UINTN*)&h.pci_function);if(EFI_ERROR(s)){fail(u"PCI",u"LOCATION",s);goto out;}
    s=uefi_call_wrapper(BS->DisconnectController,3,controller,NULL,NULL);if(EFI_ERROR(s)){fail(u"HANDOFF",u"DISCONNECT",s);goto out;}
    s=mr32(p,0,&cap);if(EFI_ERROR(s)){fail(u"CAPS",u"CAP",s);goto out;}op=cap&0xffU;s=mr16(p,2,&ver);if(EFI_ERROR(s)||ver<XHCI_MIN_VERSION){s=EFI_UNSUPPORTED;fail(u"CAPS",u"VERSION",s);goto out;}s=mr32(p,4,&hcs1);if(EFI_ERROR(s)){fail(u"CAPS",u"HCSPARAMS1",s);goto out;}s=mr32(p,0x10,&hcc);if(EFI_ERROR(s)){fail(u"CAPS",u"HCCPARAMS1",s);goto out;}xecp=(hcc>>16)&0xffffU;s=reset_xhci(p,op);if(EFI_ERROR(s)){fail(u"RESET",u"CONTROLLER",s);goto out;}halted=TRUE;
    s=dma_alloc(p,1,&dc);if(EFI_ERROR(s)){fail(u"DMA",u"DCBAA",s);goto out;}s=dma_alloc(p,1,&crd);if(EFI_ERROR(s)){fail(u"DMA",u"COMMAND RING",s);goto out;}s=dma_alloc(p,1,&erd);if(EFI_ERROR(s)){fail(u"DMA",u"EVENT RING",s);goto out;}s=dma_alloc(p,1,&est);if(EFI_ERROR(s)){fail(u"DMA",u"ERST",s);goto out;}
    dcbaa=(UINT64*)dc.host;cr=(struct trb*)crd.host;ev=(struct trb*)erd.host;erst=(UINT64*)est.host;dcbaa[0]=0;cr[CMD_TRBS-1].d[0]=(UINT32)crd.dev;cr[CMD_TRBS-1].d[1]=(UINT32)(crd.dev>>32);cr[CMD_TRBS-1].d[3]=TRB_CYCLE|(TRB_LINK<<TRB_TYPE_SHIFT)|TRB_LINK_TOGGLE;erst[0]=erd.dev;((UINT32*)erst)[2]=EVENT_TRBS;
    s=mw64(p,op+0x30U,dc.dev);if(EFI_ERROR(s)){fail(u"INIT",u"DCBAAP",s);goto out;}s=mw64(p,op+0x18U,crd.dev|CRCR_RCS);if(EFI_ERROR(s)){fail(u"INIT",u"CRCR",s);goto out;}s=mw32(p,op+0x28U,1U);if(EFI_ERROR(s)){fail(u"INIT",u"ERSTSZ",s);goto out;}s=mw64(p,op+0x30U+8U,est.dev);if(EFI_ERROR(s)){fail(u"INIT",u"ERSTBA",s);goto out;}s=mw64(p,op+0x30U+16U,erd.dev);if(EFI_ERROR(s)){fail(u"INIT",u"ERDP",s);goto out;}s=mw32(p,op+0x38U,hcs1&0xffU);if(EFI_ERROR(s)){fail(u"INIT",u"CONFIG",s);goto out;}
    s=mr32(p,op,&cmd);if(EFI_ERROR(s)){fail(u"RUN",u"READ",s);goto out;}cmd|=CMD_RUN;cmd&=~(CMD_INTE|CMD_HSEE);s=mw32(p,op,cmd);if(EFI_ERROR(s)){fail(u"RUN",u"START",s);goto out;}s=wait_hch(p,op,FALSE,1000,&ps);if(EFI_ERROR(s)){fail(u"RUN",u"HCH",s);goto out;}running=TRUE;halted=FALSE;
    port=h.keyboard.root_port;if(!port||port==0xffU){s=EFI_DEVICE_ERROR;fail(u"PORT",u"HANDOFF PORT",s);goto out;}s=supported_protocol(p,xecp,port);if(EFI_ERROR(s)){fail(u"PORT",u"SUPPORTED PROTOCOL",s);goto out;}UINT32 po=0x400U+((UINT32)(port-1U)*0x10U);s=mr32(p,po,&ps);if(EFI_ERROR(s)){fail(u"PORT",u"PORTSC",s);goto out;}if(!(ps&PORT_CCS)){s=EFI_NOT_FOUND;fail(u"PORT",u"NOT CONNECTED",s);goto out;}UINT8 speed=(UINT8)((ps&PORT_SPEED_MASK)>>PORT_SPEED_SHIFT);if(speed!=USB_SPEED_FULL&&speed!=USB_SPEED_LOW){s=EFI_UNSUPPORTED;fail(u"PORT",u"LS/FS ONLY",s);goto out;}
    {UINT32 w=ps&~PORT_PR;w|=PORT_PRC;s=mw32(p,po,w);if(EFI_ERROR(s)){fail(u"PORT",u"CLEAR PRC",s);goto out;}s=mw32(p,po,w|PORT_PR);if(EFI_ERROR(s)){fail(u"PORT",u"RESET",s);goto out;}}
    cr[0].d[0]=cr[0].d[1]=cr[0].d[2]=0;cr[0].d[3]=TRB_CYCLE|(TRB_ENABLE_SLOT<<TRB_TYPE_SHIFT);s=mw32(p,db+0U,1U);if(EFI_ERROR(s)){fail(u"COMMAND",u"DOORBELL",s);goto out;}submitted=TRUE;s=wait_cmd_completion(p,ev,(UINT32)erd.dev,&slot);if(EFI_ERROR(s)){fail(u"EVENT",u"COMMAND COMPLETION",s);goto out;}
    Print(u"HANDOFF MAGIC=%08x VERSION=%u SIZE=%u DEVICES=%u KEYBOARD_PORT=%u MOUSE=%u\r\n",h.magic,h.version,h.size,h.device_count,h.keyboard.root_port,h.mouse.root_port);Print(u"CONTROLLER PCI=%04x:%04x xHCI=%u.%02u\r\n",h.pci_vendor,h.pci_device_id,ver>>8,ver&0xffU);Print(u"PORT=%u SPEED=%u SUPPORTED-PROTOCOL=YES RESET=ISSUED\r\n",port,speed);Print(u"ENABLE SLOT=%u COMPLETION=SUCCESS\r\n",slot);
    s=mw32(p,op,cmd&~CMD_RUN);if(EFI_ERROR(s)){fail(u"STOP",u"USBCMD",s);goto out;}s=wait_hch(p,op,TRUE,1000,&ps);if(EFI_ERROR(s)){fatal_running();}halted=TRUE;mw64(p,op+0x18U,0);mw64(p,op+0x30U,0);mw64(p,op+0x38U,0);
out:
    if(running&&!halted&&EFI_ERROR(s))fatal_running();
    if(halted){dma_free(p,&est);dma_free(p,&erd);dma_free(p,&crd);dma_free(p,&dc);}
    if(EFI_ERROR(s))Print(u"RESULT: FAIL STAGE=%s OP=%s STATUS=%r\r\n",fail_stage,fail_op,fail_status);else Print(u"RESULT: Success\r\n");Print(u"TEST HOLDS 30 SECONDS\r\n");uefi_call_wrapper(BS->Stall,1,30000000);return EFI_ERROR(s)?s:EFI_SUCCESS;
}
