#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#define PORTSC_BASE 0x400
#define PORTSC_STRIDE 0x10
#define CMD_RUN 0x1
#define CMD_INTE 0x4
#define STS_HCH 0x1
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32*v){return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,o,1,v);}
static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32*v){return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,v);}
static EFI_STATUS mmio_write32(EFI_PCI_IO_PROTOCOL*p,UINT32 o,UINT32 v){return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,(UINT64)o,1,&v);}
static void done(UINT32 r,EFI_STATUS s){Print(u"\r\nV22 COMPLETE / %u MMIO READS / 2 CONTROL WRITES / NO DMA\r\nRESULT: %r\r\nEXIT 5 SEC...\r\n",r,s);uefi_call_wrapper(BS->Stall,1,5000000);}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE*st){
 EFI_HANDLE*hs=NULL;EFI_PCI_IO_PROTOCOL*p=NULL;EFI_STATUS s;UINTN n=0,i;
 UINT32 cls=0,id=0,b0=0,b1=0,cap0=0,cmd=0,status=0,reads=0,ports=0,off;
 UINT32 opbase,maxports;UINT64 bar;EFI_STATUS result=EFI_SUCCESS;
 InitializeLib(image,st);Print(u"TOSHIBA xHCI V22 / CONTROLLED HALT-RESUME\r\nREAD/WRITE TEST / NO DMA\r\n");
 s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);if(EFI_ERROR(s)){done(reads,s);return s;}
 for(i=0;i<n;i++){EFI_PCI_IO_PROTOCOL*q=NULL;if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,(void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL)))continue;if(EFI_ERROR(cfg32(q,8,&cls)))continue;if(((cls>>24)&255)==0x0c&&((cls>>16)&255)==0x03&&((cls>>8)&255)==0x30){p=q;break;}}
 if(!p){done(reads,EFI_NOT_FOUND);return EFI_NOT_FOUND;}
 cfg32(p,0,&id);cfg32(p,0x10,&b0);cfg32(p,0x14,&b1);bar=((UINT64)b1<<32)|((UINT64)b0&~15ULL);
 if(EFI_ERROR(mmio32(p,0,&cap0))){done(reads,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}reads++;
 opbase=cap0&255;
 if(EFI_ERROR(mmio32(p,opbase,&cmd))){done(reads,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}reads++;
 if(EFI_ERROR(mmio32(p,opbase+4,&status))){done(reads,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}reads++;
 Print(u"PCI ID=%04x:%04x BAR=%016lx OPBASE=%02x\r\n",id&65535,id>>16,bar,opbase);
 Print(u"INITIAL USBCMD=%08x USBSTS=%08x\r\n",cmd,status);
 if(!(cmd&CMD_RUN) || (status&STS_HCH)){Print(u"PRECONDITION FAIL: CONTROLLER NOT RUNNING\r\n");done(reads,EFI_NOT_READY);return EFI_NOT_READY;}
 Print(u"HALTING: CLEAR RUN/INTERRUPT ENABLE BITS\r\n");
 UINT32 halted_cmd=cmd&~(CMD_RUN|CMD_INTE);
 s=mmio_write32(p,opbase,halted_cmd);
 if(EFI_ERROR(s)){Print(u"HALT WRITE FAIL %r\r\n",s);done(reads,s);return s;}
 UINT32 halted=0;BOOLEAN ok=FALSE;
 for(UINTN t=0;t<100;t++){s=mmio32(p,opbase+4,&halted);if(EFI_ERROR(s)){result=s;break;}reads++;if(halted&STS_HCH){ok=TRUE;break;}uefi_call_wrapper(BS->Stall,1,1000);}
 Print(u"AFTER HALT USBCMD=%08x USBSTS=%08x HCH=%u\r\n",halted_cmd,halted,halted&STS_HCH?1:0);
 if(EFI_ERROR(result)||!ok){Print(u"HALT HANDSHAKE FAIL\r\n");if(hs)FreePool(hs);done(reads,EFI_TIMEOUT);return EFI_TIMEOUT;}
 Print(u"RESUMING: RESTORE ORIGINAL USBCMD\r\n");
 s=mmio_write32(p,opbase,cmd);
 if(EFI_ERROR(s)){Print(u"RESUME WRITE FAIL %r\r\n",s);done(reads,s);return s;}
 BOOLEAN running=FALSE;UINT32 resumed=0;
 for(UINTN t=0;t<100;t++){s=mmio32(p,opbase+4,&resumed);if(EFI_ERROR(s)){result=s;break;}reads++;if(!(resumed&STS_HCH)){running=TRUE;break;}uefi_call_wrapper(BS->Stall,1,1000);}
 Print(u"AFTER RESUME USBCMD=%08x USBSTS=%08x HCH=%u\r\n",cmd,resumed,resumed&STS_HCH?1:0);
 if(EFI_ERROR(result)||!running){Print(u"RESUME HANDSHAKE FAIL\r\n");result=EFI_TIMEOUT;}
 else Print(u"CONTROLLED HALT/RESUME: PASS\r\n");
 if(hs)FreePool(hs);done(reads,result);return result;
}