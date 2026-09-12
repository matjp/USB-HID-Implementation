/*
 * Version 32 - standalone cumulative Gate 6 test.
 *
 * HARD RULE: this version is an independent source file. It does not
 * include, wrap, or link any previous versioned test source.
 *
 * UEFI discovers exactly one boot keyboard (and optional matching mouse),
 * hands the exact controller/device paths and facts to this source, then
 * DisconnectController() releases UEFI USB ownership. The active xHCI
 * stage consumes only the selected root port; it performs no USB discovery.
 */
#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include "usb_io_compat.h"

#define XHCI_MIN_VERSION 0x0100U
#define CMD_RUN 0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE 0x00000004U
#define CMD_HSEE 0x00000008U
#define STS_HCH 0x00000001U
#define STS_CNR 0x00000800U
#define IMAN_IP 0x00000001U
#define CRCR_RCS 0x00000001ULL
#define TRB_CYCLE 0x00000001U
#define TRB_TYPE_SHIFT 10U
#define TRB_TYPE_MASK 0x0000fc00U
#define TRB_ENABLE_SLOT 9U
#define TRB_ADDRESS_DEVICE 11U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 2U
#define TRB_COMMAND_COMPLETION 33U
#define TRB_PORT_STATUS 34U
#define CC_SUCCESS 1U
#define PORT_CCS 0x00000001U
#define PORT_PED 0x00000002U
#define PORT_PR 0x00000010U
#define PORT_SPEED_MASK 0x00003c00U
#define PORT_SPEED_SHIFT 10U
#define PORT_PRC 0x00200000U
/* PORTSC neutral-write fields, matching the xHCI/Linux RW/RW1C model. */
#define PORT_RO ((1U<<0) | (1U<<3) | (0xfU<<10) | (1U<<30))
#define PORT_RWS ((0xfU<<5) | (1U<<9) | (0x3U<<14) | (0x7U<<25))
#define USB_SPEED_FULL 1U
#define USB_SPEED_LOW 2U
#define CTX_SLOT_ADD 0x00000001U
#define CTX_EP0_ADD 0x00000002U
#define EP_TYPE_CONTROL 4U
#define SLOT_STATE_ADDRESSED 2U
#define CMD_TRBS 256U
#define EVENT_TRBS 16U
#define MAX_PATH_BYTES 512U
#define MAX_ENDPOINTS 8U
#define HANDOFF_MAGIC 0x48494458U
#define HANDOFF_VERSION 2U
#define XHCI_PORTSC_BASE 0x400U
#define XHCI_PORTSC_STRIDE 0x10U
#define XHCI_IMAN0 0x20U
#define XHCI_ERSTSZ0 0x28U
#define XHCI_ERSTBA0 0x30U
#define XHCI_ERDP0 0x38U
#define XHCI_HCC_CTSCZ 0x00000004U
#define XHCI_HCC_AC64 0x00000001U
#define HCS_MAX_SLOTS(v) ((v) & 0xffU)
#define HCS_MAX_PORTS(v) (((v) >> 24) & 0x7fU)
#define HCS_MAX_SCRATCHPAD(v) ((((v) >> 16) & 0x3e0U) | (((v) >> 27) & 0x1fU))
#define SLOT_SPEED_SHIFT 20U
#define SLOT_CTX_ENTRIES_SHIFT 27U
#define SLOT_ROOT_PORT_SHIFT 16U
#define SLOT_STATE_SHIFT 27U
#define EP0_CERR_SHIFT 1U
#define EP0_TYPE_SHIFT 3U
#define EP0_MPS_SHIFT 16U
#define EP0_AVG_TRB_SHIFT 0U

struct dma_obj { VOID *host; VOID *map; EFI_PHYSICAL_ADDRESS dev; UINTN pages; BOOLEAN live; };
struct trb { UINT32 d[4]; };
typedef struct { UINT16 size; UINT16 reserved; UINT8 data[MAX_PATH_BYTES]; } PATH_COPY;
typedef struct { UINT8 endpoint_address, attributes, interval, reserved; UINT16 max_packet_size; } EP_FACT;
typedef struct { UINT8 kind, root_port, speed, interface_number; UINT8 interface_protocol, interrupt_in_endpoint, endpoint_count, reserved0; UINT16 vendor_id, product_id; UINT8 ep0_mps_descriptor, reserved1; UINT16 interrupt_max_packet_size; UINT8 interval, reserved2; EP_FACT endpoints[MAX_ENDPOINTS]; PATH_COPY path; } HID_FACT;
typedef struct { UINT32 magic; UINT16 version, size, device_count, pci_segment; UINT8 pci_bus, pci_device, pci_function, reserved0; UINT16 pci_vendor, pci_device_id; PATH_COPY controller_path; HID_FACT keyboard, mouse; } HANDOFF;

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID UsbIoGuid = EFI_USB_IO_PROTOCOL_GUID;
static EFI_GUID DevicePathGuid = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
static const CHAR16 *fail_stage=u"NONE", *fail_op=u"NONE"; static EFI_STATUS fail_status=EFI_SUCCESS;
static void fail(const CHAR16 *a,const CHAR16 *b,EFI_STATUS s){if(!EFI_ERROR(fail_status)){fail_stage=a;fail_op=b;fail_status=s;}}
static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32*v){return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,o,1,v);}
static EFI_STATUS mr32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32*v){return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,o,1,v);}
static EFI_STATUS mw32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32 v){return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,o,1,&v);}
static EFI_STATUS mw64(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT64 v){EFI_STATUS s=mw32(p,o,(UINT32)v);return EFI_ERROR(s)?s:mw32(p,o+4U,(UINT32)(v>>32));}
static UINT16 plen(EFI_DEVICE_PATH_PROTOCOL*p){return (UINT16)p->Length[0]|((UINT16)p->Length[1]<<8);}
static EFI_STATUS path_size(EFI_DEVICE_PATH_PROTOCOL*p,UINTN*out){UINT8*q=(UINT8*)p;UINTN n=0;UINT16 l;if(!p||!out)return EFI_INVALID_PARAMETER;while(n+4U<=MAX_PATH_BYTES){l=plen((EFI_DEVICE_PATH_PROTOCOL*)q);if(l<4U||n+l>MAX_PATH_BYTES)return EFI_DEVICE_ERROR;n+=l;if(q[0]==0x7fU){*out=n;return EFI_SUCCESS;}q+=l;}return EFI_BAD_BUFFER_SIZE;}
static EFI_STATUS copy_path(PATH_COPY*d,EFI_DEVICE_PATH_PROTOCOL*p){UINTN n;EFI_STATUS s=path_size(p,&n);if(EFI_ERROR(s))return s;uefi_call_wrapper(BS->SetMem,3,d,sizeof(*d),0);CopyMem(d->data,p,n);d->size=(UINT16)n;return EFI_SUCCESS;}
static UINT8 root_port(EFI_DEVICE_PATH_PROTOCOL*p){UINT8*q=(UINT8*)p;while(q){EFI_DEVICE_PATH_PROTOCOL*h=(EFI_DEVICE_PATH_PROTOCOL*)q;UINT16 l=plen(h);if(l<4U||h->Type==0x7fU)break;if(h->Type==3U&&h->SubType==5U&&l>=6U)return q[4];q+=l;}return 0xffU;}
static BOOLEAN hid_kind(EFI_USB_INTERFACE_DESCRIPTOR*d,UINT8*k){if(!d||d->InterfaceClass!=3U||d->InterfaceSubClass!=1U)return FALSE;if(d->InterfaceProtocol==1U){*k=1U;return TRUE;}if(d->InterfaceProtocol==2U){*k=2U;return TRUE;}return FALSE;}
static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL*p,UINTN pages,struct dma_obj*d){EFI_STATUS s;UINTN bytes=pages*4096U,mapped=bytes;uefi_call_wrapper(BS->SetMem,3,d,sizeof(*d),0);if(!pages)return EFI_BAD_BUFFER_SIZE;d->pages=pages;s=uefi_call_wrapper(p->AllocateBuffer,6,p,AllocateAnyPages,EfiBootServicesData,pages,&d->host,0);if(EFI_ERROR(s))return s;s=uefi_call_wrapper(p->Map,6,p,EfiPciIoOperationBusMasterCommonBuffer,d->host,&mapped,&d->dev,&d->map);if(EFI_ERROR(s)||mapped!=bytes){if(!EFI_ERROR(s)&&d->map)uefi_call_wrapper(p->Unmap,2,p,d->map);uefi_call_wrapper(p->FreeBuffer,3,p,pages,d->host);return EFI_ERROR(s)?s:EFI_DEVICE_ERROR;}uefi_call_wrapper(BS->SetMem,3,d->host,bytes,0);d->live=TRUE;return EFI_SUCCESS;}
static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL*p,struct dma_obj*d){EFI_STATUS s=EFI_SUCCESS,t;if(!d||!d->live)return EFI_SUCCESS;if(d->map){t=uefi_call_wrapper(p->Unmap,2,p,d->map);if(EFI_ERROR(t))s=t;}if(d->host){t=uefi_call_wrapper(p->FreeBuffer,3,p,d->pages,d->host);if(EFI_ERROR(t)&&!EFI_ERROR(s))s=t;}d->host=NULL;d->map=NULL;d->dev=0;d->live=FALSE;return s;}
static EFI_STATUS wait_hch(EFI_PCI_IO_PROTOCOL*p,UINT32 op,BOOLEAN want,UINTN loops,UINT32*st){UINTN i;EFI_STATUS s;for(i=0;i<loops;i++){s=mr32(p,op+4U,st);if(EFI_ERROR(s))return s;if(((*st&STS_HCH)!=0)==want)return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static EFI_STATUS reset_xhci(EFI_PCI_IO_PROTOCOL*p,UINT32 op){UINT32 c,st;UINTN i;EFI_STATUS s=mr32(p,op,&c);if(EFI_ERROR(s))return s;c&=~(CMD_RUN|CMD_INTE|CMD_HSEE);c|=CMD_RESET;s=mw32(p,op,c);if(EFI_ERROR(s))return s;for(i=0;i<1000;i++){s=mr32(p,op,&c);if(EFI_ERROR(s))return s;if(!(c&CMD_RESET))break;uefi_call_wrapper(BS->Stall,1,1000);}if(c&CMD_RESET)return EFI_TIMEOUT;for(i=0;i<10000;i++){s=mr32(p,op+4U,&st);if(EFI_ERROR(s))return s;if(!(st&STS_CNR))return EFI_SUCCESS;uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static UINT32 port_neutral(UINT32 ps){return (ps&PORT_RO)|(ps&PORT_RWS);}
static EFI_STATUS controller_for_path(EFI_HANDLE image,EFI_DEVICE_PATH_PROTOCOL*usb,EFI_HANDLE*out){EFI_HANDLE*hs=NULL,best=NULL;UINTN n=0,i,bestn=0;EFI_STATUS s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);if(EFI_ERROR(s))return s;for(i=0;i<n;i++){EFI_DEVICE_PATH_PROTOCOL*p=NULL;UINT8*a,*b;UINTN m=0;s=uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&DevicePathGuid,(VOID**)&p,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);if(EFI_ERROR(s)||!p)continue;a=(UINT8*)p;b=(UINT8*)usb;while(a&&b){EFI_DEVICE_PATH_PROTOCOL*x=(EFI_DEVICE_PATH_PROTOCOL*)a,*y=(EFI_DEVICE_PATH_PROTOCOL*)b;UINT16 xl=plen(x),yl=plen(y);if(xl<4U||yl<4U||xl!=yl)break;if(x->Type==0x7fU||y->Type==0x7fU)break;if(CompareMem(x,y,xl)!=0)break;m+=xl;a+=xl;b+=yl;}if(m>bestn){bestn=m;best=hs[i];}}FreePool(hs);if(!best)return EFI_NOT_FOUND;*out=best;return EFI_SUCCESS;}
static EFI_STATUS fill_hid(EFI_USB_IO_PROTOCOL*u,EFI_USB_INTERFACE_DESCRIPTOR*i,EFI_DEVICE_PATH_PROTOCOL*p,UINT8 kind,HID_FACT*d){EFI_USB_DEVICE_DESCRIPTOR dd;EFI_USB_ENDPOINT_DESCRIPTOR e;UINTN n,j;BOOLEAN got=FALSE;uefi_call_wrapper(BS->SetMem,3,d,sizeof(*d),0);d->kind=kind;d->root_port=root_port(p);d->interface_number=i->InterfaceNumber;d->interface_protocol=i->InterfaceProtocol;if(d->root_port==0xffU)return EFI_NOT_FOUND;if(EFI_ERROR(copy_path(&d->path,p)))return EFI_DEVICE_ERROR;if(!EFI_ERROR(uefi_call_wrapper(u->UsbGetDeviceDescriptor,3,u,&dd))){d->vendor_id=dd.IdVendor;d->product_id=dd.IdProduct;d->ep0_mps_descriptor=dd.MaxPacketSize0;}n=i->NumEndpoints>MAX_ENDPOINTS?MAX_ENDPOINTS:i->NumEndpoints;d->endpoint_count=(UINT8)n;for(j=0;j<n;j++)if(!EFI_ERROR(uefi_call_wrapper(u->UsbGetEndpointDescriptor,4,u,(UINT8)j,&e))){d->endpoints[j].endpoint_address=e.EndpointAddress;d->endpoints[j].attributes=e.Attributes;d->endpoints[j].max_packet_size=e.MaxPacketSize;d->endpoints[j].interval=e.Interval;if(!got&&(e.EndpointAddress&0x80U)&&((e.Attributes&3U)==3U)&&e.MaxPacketSize&&e.Interval){d->interrupt_in_endpoint=e.EndpointAddress;d->interrupt_max_packet_size=e.MaxPacketSize;d->interval=e.Interval;got=TRUE;}}return got?EFI_SUCCESS:EFI_UNSUPPORTED;}
static EFI_STATUS produce_handoff(EFI_HANDLE image,HANDOFF*h,EFI_HANDLE*controller){EFI_HANDLE*hs=NULL;UINTN n=0,i,k=0,m=0;EFI_STATUS s;EFI_HANDLE ch=NULL;uefi_call_wrapper(BS->SetMem,3,h,sizeof(*h),0);h->magic=HANDOFF_MAGIC;h->version=HANDOFF_VERSION;h->size=(UINT16)sizeof(*h);s=LibLocateHandle(ByProtocol,&UsbIoGuid,NULL,&n,&hs);if(EFI_ERROR(s))return s;for(i=0;i<n;i++){EFI_USB_IO_PROTOCOL*u=NULL;EFI_USB_INTERFACE_DESCRIPTOR id;EFI_DEVICE_PATH_PROTOCOL*p=NULL;UINT8 kind;s=uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&UsbIoGuid,(VOID**)&u,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);if(EFI_ERROR(s))continue;if(EFI_ERROR(uefi_call_wrapper(u->UsbGetInterfaceDescriptor,3,u,&id))||!hid_kind(&id,&kind))continue;s=uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&DevicePathGuid,(VOID**)&p,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);if(EFI_ERROR(s)||!p){FreePool(hs);return EFI_DEVICE_ERROR;}s=controller_for_path(image,p,&ch);if(EFI_ERROR(s)){FreePool(hs);return s;}if(kind==1U){if(++k!=1U){FreePool(hs);return EFI_DEVICE_ERROR;}s=fill_hid(u,&id,p,1U,&h->keyboard);if(EFI_ERROR(s)){FreePool(hs);return s;}*controller=ch;}else{if(++m!=1U){FreePool(hs);return EFI_DEVICE_ERROR;}if(*controller&&ch!=*controller){FreePool(hs);return EFI_DEVICE_ERROR;}s=fill_hid(u,&id,p,2U,&h->mouse);if(EFI_ERROR(s)){FreePool(hs);return s;}}}FreePool(hs);if(k!=1U||!*controller)return EFI_NOT_FOUND;h->device_count=m?2U:1U;return EFI_SUCCESS;}
static EFI_STATUS find_protocol(EFI_PCI_IO_PROTOCOL*p,UINT32 xecp,UINT8 port,UINT8*slot_type){UINT32 off=xecp*4U,h,ports,slot;UINTN n=0;EFI_STATUS s;if(!slot_type)return EFI_INVALID_PARAMETER;while(off&&n++<64U){s=mr32(p,off,&h);if(EFI_ERROR(s))return s;if((h&0xffU)==2U){s=mr32(p,off+8U,&ports);if(EFI_ERROR(s))return s;if(port>=(UINT8)(ports&0xffU)&&port<(UINT8)((ports&0xffU)+((ports>>8)&0xffU))){s=mr32(p,off+12U,&slot);if(EFI_ERROR(s))return s;*slot_type=(UINT8)(slot&0x1fU);return EFI_SUCCESS;}}off+=((h>>8)&0xffU)*4U;if(!((h>>8)&0xffU))break;}return EFI_NOT_FOUND;}
static EFI_STATUS wait_event(EFI_PCI_IO_PROTOCOL*p,UINT32 rt,struct trb*ring,EFI_PHYSICAL_ADDRESS ring_dev,UINT32*index,UINT32*cycle,UINT32 wanted_type,UINT64 wanted_cmd,UINT8 wanted_port,UINT32*slot,UINT32*completion){UINTN loops;UINT32 d3,type,cc,next;UINT64 param;EFI_STATUS s;for(loops=0;loops<10000U;loops++){d3=ring[*index].d[3];if((d3&TRB_CYCLE)==*cycle){type=(d3&TRB_TYPE_MASK)>>TRB_TYPE_SHIFT;if(type!=wanted_type)return EFI_DEVICE_ERROR;param=(UINT64)ring[*index].d[0]|((UINT64)ring[*index].d[1]<<32);cc=(ring[*index].d[2]>>24)&0xffU;if(wanted_type==TRB_COMMAND_COMPLETION){if(param!=wanted_cmd)return EFI_DEVICE_ERROR;if(slot)*slot=(d3>>24)&0xffU;if(completion)*completion=cc;}else if(((d3>>24)&0xffU)!=wanted_port)return EFI_DEVICE_ERROR;next=*index+1U;if(next==EVENT_TRBS){next=0;*cycle^=TRB_CYCLE;}*index=next;s=mw64(p,rt+XHCI_ERDP0,((UINT64)ring_dev+((UINT64)next*sizeof(struct trb)))|8ULL);if(EFI_ERROR(s))return s;s=mw32(p,rt+XHCI_IMAN0,IMAN_IP);if(EFI_ERROR(s))return s;return EFI_SUCCESS;}uefi_call_wrapper(BS->Stall,1,1000);}return EFI_TIMEOUT;}
static void fatal_running(void){Print(u"\r\nFATAL: XHCI NOT CONFIRMED HALTED\r\nDMA MAPPINGS RETAINED\r\n");for(;;)uefi_call_wrapper(BS->Stall,1,1000000);}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE*st)
{
 EFI_STATUS s=EFI_SUCCESS,t;EFI_HANDLE controller=NULL;EFI_PCI_IO_PROTOCOL*p=NULL;HANDOFF h;EFI_DEVICE_PATH_PROTOCOL*cp=NULL;
 UINT32 cap=0,id=0,hcs1=0,hcs2=0,hcc=0,op=0,xecp=0,dboff=0,rtsoff=0,bar0=0,bar1=0,cmd=0,ps=0,po,config=0,scratch_count=0;
 UINT16 ver=0;UINT8 port,speed,slot_type=0;UINT32 slot=0,completion=0,event_index=0,event_cycle=TRB_CYCLE,ctx_size;UINTN seg=0,bus=0,dev=0,fun=0,scratch_i;
 UINT64 old_attrs=0,pci_attrs=0,bar64;BOOLEAN attrs_changed=FALSE,running=FALSE,halted=FALSE;struct dma_obj dc={0},crd={0},erd={0},est={0},inctx={0},outctx={0},ep0={0},scratch_array={0};struct dma_obj scratch_pages[32];UINT64*dcbaa,*scratch,*erst;struct trb*cr,*ev,*ep_ring;UINT8*ic,*oc;UINT64 expected;
 uefi_call_wrapper(BS->SetMem,3,scratch_pages,sizeof(scratch_pages),0);InitializeLib(image,st);
 Print(u"TOSHIBA xHCI V32 / STANDALONE CUMULATIVE GATE 6\r\nUEFI EXACT KEYBOARD+MOUSE HANDOFF -> DISCONNECT -> FRESH xHCI -> ADDRESS DEVICE\r\nNO BRIDGE USB DISCOVERY / USB2 LS+FS ONLY / CPU INTERRUPTS DISABLED\r\n");
 s=produce_handoff(image,&h,&controller);if(EFI_ERROR(s)){fail(u"HANDOFF",u"PRODUCE",s);goto out;}
 s=uefi_call_wrapper(BS->OpenProtocol,6,controller,&PciGuid,(VOID**)&p,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);if(EFI_ERROR(s)){fail(u"HANDOFF",u"PCI",s);goto out;}
 s=uefi_call_wrapper(BS->OpenProtocol,6,controller,&DevicePathGuid,(VOID**)&cp,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);if(EFI_ERROR(s)||!cp||EFI_ERROR(copy_path(&h.controller_path,cp))){s=EFI_DEVICE_ERROR;fail(u"HANDOFF",u"CONTROLLER PATH",s);goto out;}
 s=cfg32(p,0,&id);if(EFI_ERROR(s)){fail(u"PCI",u"ID",s);goto out;}h.pci_vendor=(UINT16)id;h.pci_device_id=(UINT16)(id>>16);
 s=uefi_call_wrapper(p->GetLocation,5,p,&seg,&bus,&dev,&fun);if(EFI_ERROR(s)){fail(u"PCI",u"LOCATION",s);goto out;}if(seg>0xffffU||bus>0xffU||dev>0x1fU||fun>0x7U){s=EFI_DEVICE_ERROR;fail(u"PCI",u"LOCATION RANGE",s);goto out;}h.pci_segment=(UINT16)seg;h.pci_bus=(UINT8)bus;h.pci_device=(UINT8)dev;h.pci_function=(UINT8)fun;
 s=uefi_call_wrapper(p->Attributes,4,p,EfiPciIoAttributeOperationGet,0,&old_attrs);if(EFI_ERROR(s)){fail(u"PCI",u"ATTRIBUTES GET",s);goto out;}pci_attrs=old_attrs;
 if(!(pci_attrs&EFI_PCI_IO_ATTRIBUTE_MEMORY)||!(pci_attrs&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)){s=uefi_call_wrapper(p->Attributes,4,p,EfiPciIoAttributeOperationEnable,EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,&pci_attrs);if(EFI_ERROR(s)){fail(u"PCI",u"ATTRIBUTES ENABLE",s);goto out;}attrs_changed=TRUE;}
 s=uefi_call_wrapper(BS->DisconnectController,3,controller,NULL,NULL);if(EFI_ERROR(s)){fail(u"HANDOFF",u"DISCONNECT",s);goto out;}
 s=cfg32(p,0x10,&bar0);if(EFI_ERROR(s)){fail(u"PCI",u"BAR0",s);goto out;}s=cfg32(p,0x14,&bar1);if(EFI_ERROR(s)){fail(u"PCI",u"BAR1",s);goto out;}if((bar0&1U)||(((bar0>>1)&3U)==1U)||!(bar0&0xfffffff0U)){s=EFI_UNSUPPORTED;fail(u"PCI",u"MMIO BAR",s);goto out;}bar64=(UINT64)(bar0&0xfffffff0U);if(((bar0>>1)&3U)==2U)bar64|=((UINT64)bar1<<32);if(!bar64){s=EFI_DEVICE_ERROR;fail(u"PCI",u"MMIO BAR ZERO",s);goto out;}
 s=mr32(p,0,&cap);if(EFI_ERROR(s)){fail(u"CAPS",u"CAPBASE",s);goto out;}op=cap&0xffU;ver=(UINT16)(cap>>16);if(ver<XHCI_MIN_VERSION){s=EFI_UNSUPPORTED;fail(u"CAPS",u"VERSION",s);goto out;}s=mr32(p,4,&hcs1);if(EFI_ERROR(s)){fail(u"CAPS",u"HCSPARAMS1",s);goto out;}s=mr32(p,8,&hcs2);if(EFI_ERROR(s)){fail(u"CAPS",u"HCSPARAMS2",s);goto out;}s=mr32(p,0x10,&hcc);if(EFI_ERROR(s)){fail(u"CAPS",u"HCCPARAMS1",s);goto out;}s=mr32(p,0x14,&dboff);if(EFI_ERROR(s)){fail(u"CAPS",u"DBOFF",s);goto out;}s=mr32(p,0x18,&rtsoff);if(EFI_ERROR(s)){fail(u"CAPS",u"RTSOFF",s);goto out;}dboff&=~3U;rtsoff&=~0x1fU;xecp=(hcc>>16)&0xffffU;if(!(hcc&XHCI_HCC_AC64)){s=EFI_UNSUPPORTED;fail(u"CAPS",u"AC64",s);goto out;}scratch_count=HCS_MAX_SCRATCHPAD(hcs2);if(scratch_count>32U){s=EFI_UNSUPPORTED;fail(u"CAPS",u"SCRATCHPADS",s);goto out;}
 s=reset_xhci(p,op);if(EFI_ERROR(s)){fail(u"RESET",u"CONTROLLER",s);goto out;}halted=TRUE;
 s=dma_alloc(p,1,&dc);if(EFI_ERROR(s)){fail(u"DMA",u"DCBAA",s);goto out;}s=dma_alloc(p,1,&crd);if(EFI_ERROR(s)){fail(u"DMA",u"COMMAND RING",s);goto out;}s=dma_alloc(p,1,&erd);if(EFI_ERROR(s)){fail(u"DMA",u"EVENT RING",s);goto out;}s=dma_alloc(p,1,&est);if(EFI_ERROR(s)){fail(u"DMA",u"ERST",s);goto out;}s=dma_alloc(p,1,&inctx);if(EFI_ERROR(s)){fail(u"DMA",u"INPUT CONTEXT",s);goto out;}s=dma_alloc(p,1,&outctx);if(EFI_ERROR(s)){fail(u"DMA",u"OUTPUT CONTEXT",s);goto out;}s=dma_alloc(p,1,&ep0);if(EFI_ERROR(s)){fail(u"DMA",u"EP0 RING",s);goto out;}
 if(scratch_count){s=dma_alloc(p,1,&scratch_array);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCHPAD ARRAY",s);goto out;}scratch=(UINT64*)scratch_array.host;for(scratch_i=0;scratch_i<scratch_count;scratch_i++){s=dma_alloc(p,1,&scratch_pages[scratch_i]);if(EFI_ERROR(s)){fail(u"DMA",u"SCRATCHPAD PAGE",s);goto out;}scratch[scratch_i]=scratch_pages[scratch_i].dev;}dcbaa=(UINT64*)dc.host;dcbaa[0]=scratch_array.dev;}else dcbaa=(UINT64*)dc.host;
 cr=(struct trb*)crd.host;ev=(struct trb*)erd.host;erst=(UINT64*)est.host;ep_ring=(struct trb*)ep0.host;cr[CMD_TRBS-1].d[0]=(UINT32)crd.dev;cr[CMD_TRBS-1].d[1]=(UINT32)(crd.dev>>32);cr[CMD_TRBS-1].d[3]=TRB_CYCLE|(TRB_LINK<<TRB_TYPE_SHIFT)|TRB_LINK_TOGGLE;erst[0]=erd.dev;((UINT32*)erst)[2]=EVENT_TRBS;
 s=mw64(p,op+0x18U,crd.dev|CRCR_RCS);if(EFI_ERROR(s)){fail(u"INIT",u"CRCR",s);goto out;}s=mw64(p,op+0x30U,dc.dev);if(EFI_ERROR(s)){fail(u"INIT",u"DCBAAP",s);goto out;}config=HCS_MAX_SLOTS(hcs1);s=mw32(p,op+0x38U,config);if(EFI_ERROR(s)){fail(u"INIT",u"CONFIG",s);goto out;}
 {UINT32 rt=rtsoff+0x20U;s=mw32(p,rt+XHCI_IMAN0,IMAN_IP);if(EFI_ERROR(s)){fail(u"INIT",u"IMAN",s);goto out;}s=mw32(p,rt+XHCI_ERSTSZ0,1U);if(EFI_ERROR(s)){fail(u"INIT",u"ERSTSZ",s);goto out;}s=mw64(p,rt+XHCI_ERSTBA0,est.dev);if(EFI_ERROR(s)){fail(u"INIT",u"ERSTBA",s);goto out;}s=mw64(p,rt+XHCI_ERDP0,erd.dev|8ULL);if(EFI_ERROR(s)){fail(u"INIT",u"ERDP",s);goto out;}}
 s=mr32(p,op,&cmd);if(EFI_ERROR(s)){fail(u"RUN",u"READ",s);goto out;}cmd|=CMD_RUN;cmd&=~(CMD_INTE|CMD_HSEE);s=mw32(p,op,cmd);if(EFI_ERROR(s)){fail(u"RUN",u"START",s);goto out;}s=wait_hch(p,op,FALSE,1000,&ps);if(EFI_ERROR(s)){fail(u"RUN",u"HCH",s);goto out;}running=TRUE;halted=FALSE;
 port=h.keyboard.root_port;if(!port||port==0xffU||port>HCS_MAX_PORTS(hcs1)){s=EFI_DEVICE_ERROR;fail(u"PORT",u"HANDOFF PORT",s);goto out;}s=find_protocol(p,xecp,port,&slot_type);if(EFI_ERROR(s)){fail(u"PORT",u"SUPPORTED PROTOCOL",s);goto out;}po=XHCI_PORTSC_BASE+((UINT32)(port-1U)*XHCI_PORTSC_STRIDE);s=mr32(p,po,&ps);if(EFI_ERROR(s)){fail(u"PORT",u"PORTSC",s);goto out;}if(!(ps&PORT_CCS)){s=EFI_NOT_FOUND;fail(u"PORT",u"NOT CONNECTED",s);goto out;}speed=(UINT8)((ps&PORT_SPEED_MASK)>>PORT_SPEED_SHIFT);if(speed!=USB_SPEED_FULL&&speed!=USB_SPEED_LOW){s=EFI_UNSUPPORTED;fail(u"PORT",u"LS/FS ONLY",s);goto out;}
 s=mw32(p,po,port_neutral(ps)|PORT_PRC);if(EFI_ERROR(s)){fail(u"PORT",u"CLEAR STALE PRC",s);goto out;}s=mw32(p,po,port_neutral(ps)|PORT_PR;if(EFI_ERROR(s)){fail(u"PORT",u"RESET",s);goto out;}
 s=wait_event(p,rtsoff+0x20U,ev,erd.dev,&event_index,&event_cycle,TRB_PORT_STATUS,0,port,NULL,&completion);if(EFI_ERROR(s)){fail(u"PORT",u"RESET EVENT",s);goto out;}s=mr32(p,po,&ps);if(EFI_ERROR(s)){fail(u"PORT",u"POST RESET PORTSC",s);goto out;}if(!(ps&PORT_CCS)||(ps&PORT_PR)||!(ps&PORT_PED)){s=EFI_DEVICE_ERROR;fail(u"PORT",u"POST RESET STATE",s);goto out;}speed=(UINT8)((ps&PORT_SPEED_MASK)>>PORT_SPEED_SHIFT);if(speed!=USB_SPEED_FULL&&speed!=USB_SPEED_LOW){s=EFI_UNSUPPORTED;fail(u"PORT",u"POST RESET SPEED",s);goto out;}
 cr[0].d[0]=cr[0].d[1]=cr[0].d[2]=0;cr[0].d[3]=TRB_CYCLE|((UINT32)TRB_ENABLE_SLOT<<TRB_TYPE_SHIFT)|((UINT32)slot_type<<16);expected=crd.dev;s=mw32(p,dboff,1U);if(EFI_ERROR(s)){fail(u"COMMAND",u"ENABLE SLOT DOORBELL",s);goto out;}s=wait_event(p,rtsoff+0x20U,ev,erd.dev,&event_index,&event_cycle,TRB_COMMAND_COMPLETION,expected,0,&slot,&completion);if(EFI_ERROR(s)||completion!=CC_SUCCESS||!slot||slot>HCS_MAX_SLOTS(hcs1)){s=EFI_DEVICE_ERROR;fail(u"COMMAND",u"ENABLE SLOT COMPLETION",s);goto out;}
 ctx_size=(hcc&XHCI_HCC_CTSCZ)?64U:32U;uefi_call_wrapper(BS->SetMem,3,ic,4096,0);uefi_call_wrapper(BS->SetMem,3,oc,4096,0);uefi_call_wrapper(BS->SetMem,3,ep_ring,4096,0);ep_ring[0].d[3]=TRB_CYCLE;dcbaa=(UINT64*)dc.host;dcbaa[slot]=outctx.dev;
 {UINT32*dw=(UINT32*)ic;UINT32*slotc=(UINT32*)(ic+ctx_size);UINT32*epc=(UINT32*)(ic+(2U*ctx_size));dw[1]=CTX_SLOT_ADD|CTX_EP0_ADD;slotc[0]=((UINT32)speed<<SLOT_SPEED_SHIFT)|(1U<<SLOT_CTX_ENTRIES_SHIFT);slotc[1]=((UINT32)port<<SLOT_ROOT_PORT_SHIFT);slotc[2]=0;slotc[3]=0;epc[0]=(3U<<EP0_CERR_SHIFT);epc[1]=(EP_TYPE_CONTROL<<EP0_TYPE_SHIFT)|(8U<<EP0_MPS_SHIFT);((UINT64*)epc)[1]=ep0.dev|TRB_CYCLE;epc[4]=8U<<EP0_AVG_TRB_SHIFT;}
 cr[1].d[0]=(UINT32)inctx.dev;cr[1].d[1]=(UINT32)(inctx.dev>>32);cr[1].d[2]=0;cr[1].d[3]=TRB_CYCLE|((UINT32)TRB_ADDRESS_DEVICE<<TRB_TYPE_SHIFT)|((UINT32)slot<<24);expected=crd.dev+sizeof(struct trb);s=mw32(p,dboff,1U);if(EFI_ERROR(s)){fail(u"COMMAND",u"ADDRESS DEVICE DOORBELL",s);goto out;}s=wait_event(p,rtsoff+0x20U,ev,erd.dev,&event_index,&event_cycle,TRB_COMMAND_COMPLETION,expected,0,&slot,&completion);if(EFI_ERROR(s)||completion!=CC_SUCCESS){s=EFI_DEVICE_ERROR;fail(u"COMMAND",u"ADDRESS DEVICE COMPLETION",s);goto out;}
 {UINT32*out_slot=(UINT32*)oc;UINT32 state=(out_slot[3]>>SLOT_STATE_SHIFT)&0x1fU;UINT32 addr=out_slot[3]&0xffU;if(state!=SLOT_STATE_ADDRESSED||!addr){s=EFI_DEVICE_ERROR;fail(u"COMMAND",u"ADDRESS DEVICE STATE",s);goto out;}Print(u"HANDOFF MAGIC=%08x VERSION=%u SIZE=%u DEVICES=%u KEYBOARD_PORT=%u MOUSE_PORT=%u\r\n",h.magic,h.version,h.size,h.device_count,h.keyboard.root_port,h.mouse.root_port);Print(u"CONTROLLER PCI=%04x:%04x xHCI=%u.%02u BAR=%08x:%08x\r\n",h.pci_vendor,h.pci_device_id,ver>>8,ver&0xffU,bar1,bar0);Print(u"PORT=%u SPEED=%u SLOT_TYPE=%u RESET_EVENT=OK ENABLE_SLOT=%u\r\n",port,speed,slot_type,slot);Print(u"ADDRESS DEVICE=SUCCESS USB_ADDRESS=%u STATE=ADDRESSED\r\n",addr);}
 s=mw32(p,op,cmd&~CMD_RUN);if(EFI_ERROR(s)){fail(u"STOP",u"USBCMD",s);goto out;}s=wait_hch(p,op,TRUE,1000,&ps);if(EFI_ERROR(s)){fatal_running();}running=FALSE;halted=TRUE;
 /* Clear every controller reference before the DMA mappings are released. */
 mw64(p,op+0x18U,0);mw64(p,op+0x30U,0);mw32(p,op+0x38U,0);mw32(p,rtsoff+0x20U+XHCI_ERSTSZ0,0);mw64(p,rtsoff+0x20U+XHCI_ERSTBA0,0);mw64(p,rtsoff+0x20U+XHCI_ERDP0,0);
 s=reset_xhci(p,op);if(EFI_ERROR(s)){fatal_running();}
 Print(u"RESULT: Success\r\n");
out:
 if(running&&!halted)fatal_running();
 if(halted){dma_free(p,&scratch_array);for(scratch_i=0;scratch_i<scratch_count&&scratch_i<32U;scratch_i++)dma_free(p,&scratch_pages[scratch_i]);dma_free(p,&ep0);dma_free(p,&outctx);dma_free(p,&inctx);dma_free(p,&est);dma_free(p,&erd);dma_free(p,&crd);dma_free(p,&dc);if(attrs_changed){t=uefi_call_wrapper(p->Attributes,4,p,EfiPciIoAttributeOperationSet,old_attrs,NULL);if(EFI_ERROR(t)&&!EFI_ERROR(s))s=t;}}
 if(EFI_ERROR(s)){Print(u"RESULT: FAIL STAGE=%s OP=%s STATUS=%r\r\n",fail_stage,fail_op,fail_status);}
 Print(u"TEST HOLDS 30 SECONDS\r\n");uefi_call_wrapper(BS->Stall,1,30000000);return EFI_ERROR(s)?s:EFI_SUCCESS;
}
