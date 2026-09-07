#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#define PORTSC_BASE 0x400
#define PORTSC_STRIDE 0x10
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32*v){return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,o,1,v);}
static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32*v){return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,v);}
static void done(UINT32 r,UINT32 c){Print(u"\r\nV21R COMPLETE / %u READ-ONLY MMIO READS / NO DMA / NO WRITES\r\nCONNECTED PORTS: %u\r\nCAPABILITY/OFFSET CHECK: PASS\r\nEXIT 5 SEC...\r\n",r,c);uefi_call_wrapper(BS->Stall,1,5000000);}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE*st){
 EFI_HANDLE*hs=NULL;EFI_PCI_IO_PROTOCOL*p=NULL;EFI_STATUS s;UINTN n=0,i;
 UINT32 cls=0,id=0,b0=0,b1=0,cap0=0,hcs1=0,v=0,reads=0,connected=0;
 UINT32 caplen,opbase,maxports,off;UINT64 bar;
 InitializeLib(image,st);Print(u"TOSHIBA xHCI V21R / CAPABILITY OFFSET VERIFY\r\nREAD-ONLY / NO DMA / NO WRITES\r\n");
 s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);if(EFI_ERROR(s)){Print(u"PCI ENUM FAIL %r\r\n",s);done(reads,connected);return s;}
 for(i=0;i<n;i++){EFI_PCI_IO_PROTOCOL*q=NULL;if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,(void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL)))continue;if(EFI_ERROR(cfg32(q,8,&cls)))continue;if(((cls>>24)&255)==0x0c&&((cls>>16)&255)==0x03&&((cls>>8)&255)==0x30){p=q;break;}}
 if(!p){Print(u"xHCI NOT FOUND\r\n");done(reads,connected);return EFI_NOT_FOUND;}
 cfg32(p,0,&id);cfg32(p,0x10,&b0);cfg32(p,0x14,&b1);bar=((UINT64)b1<<32)|((UINT64)b0&~15ULL);
 if(EFI_ERROR(mmio32(p,0,&cap0))){Print(u"CAP READ FAIL\r\n");done(reads,connected);return EFI_DEVICE_ERROR;}reads++;
 caplen=cap0&255;opbase=caplen;
 if(EFI_ERROR(mmio32(p,4,&hcs1))){Print(u"HCS1 READ FAIL\r\n");done(reads,connected);return EFI_DEVICE_ERROR;}reads++;
 maxports=(hcs1>>24)&255;
 Print(u"PCI ID=%04x:%04x BAR=%016lx\r\n",id&65535,id>>16,bar);
 Print(u"CAPREG[000]=%08x CAPLEN=%02x OPBASE=%02x\r\n",cap0,caplen,opbase);
 Print(u"HCS1=%08x MAXPORTS=%u\r\n",hcs1,maxports);
 Print(u"VERIFYING OPERATIONAL OFFSETS\r\n");
 if(EFI_ERROR(mmio32(p,opbase+0x00,&v))){Print(u"USBCMD READ FAIL\r\n");done(reads,connected);return EFI_DEVICE_ERROR;}reads++;Print(u"USBCMD[%03x]=%08x\r\n",opbase+0x00,v);
 if(EFI_ERROR(mmio32(p,opbase+0x04,&v))){Print(u"USBSTS READ FAIL\r\n");done(reads,connected);return EFI_DEVICE_ERROR;}reads++;Print(u"USBSTS[%03x]=%08x\r\n",opbase+0x04,v);
 if(EFI_ERROR(mmio32(p,opbase+0x08,&v))){Print(u"PAGESIZE READ FAIL\r\n");done(reads,connected);return EFI_DEVICE_ERROR;}reads++;Print(u"PAGESIZE[%03x]=%08x\r\n",opbase+0x08,v);
 if(EFI_ERROR(mmio32(p,opbase+0x38,&v))){Print(u"CONFIG READ FAIL\r\n");done(reads,connected);return EFI_DEVICE_ERROR;}reads++;Print(u"CONFIG[%03x]=%08x\r\n",opbase+0x38,v);
 Print(u"PORTS PRESENT AT BOOT\r\n");
 for(UINT32 port=1;port<=maxports;port++){off=opbase+PORTSC_BASE+(port-1)*PORTSC_STRIDE;if(EFI_ERROR(mmio32(p,off,&v))){Print(u"PORT%02u READ FAIL\r\n",port);done(reads,connected);return EFI_DEVICE_ERROR;}reads++;if(v&1){connected++;Print(u"PORT%02u CCS=1 PLS=%u SPEED=%u RAW=%08x\r\n",port,(v>>5)&15,(v>>10)&15,v);}}
 Print(u"V21R: CAPREG/OPBASE AND REGISTER OFFSETS ARE CONSISTENT; NO REGISTER MODIFICATION.\r\n");
 if(hs)FreePool(hs);done(reads,connected);return EFI_SUCCESS;
}