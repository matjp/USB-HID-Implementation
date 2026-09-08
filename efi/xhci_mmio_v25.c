#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define CMD_RUN   0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE  0x00000004U
#define STS_HCH   0x00000001U
#define STS_CNR   0x00000800U

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Pci.Read,5,p,EfiPciIoWidthUint32,off,1,v);
}
static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint32,0,(UINT64)off,1,v);
}
static EFI_STATUS mmio_write32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 v) {
    return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint32,0,(UINT64)off,1,&v);
}
static void done(UINT32 reads, EFI_STATUS s) {
    Print(u"\r\nV25 COMPLETE / %u MMIO READS / 3 CONTROL WRITES / NO DMA\r\n",reads);
    Print(u"RESULT: %r\r\nEXIT 5 SEC...\r\n",s);
    uefi_call_wrapper(BS->Stall,1,5000000);
}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs=NULL;
    EFI_PCI_IO_PROTOCOL *p=NULL;
    EFI_STATUS s,result=EFI_SUCCESS;
    UINTN n=0,i;
    UINT32 cls=0,id=0,bar0=0,bar1=0,cap0=0;
    UINT32 opbase,cmd=0,status=0,after=0,after_status=0,reads=0;
    UINT64 bar,dcbaap=0,dcbaap_read=0;
    EFI_PHYSICAL_ADDRESS page=0;

    InitializeLib(image,st);
    Print(u"TOSHIBA xHCI V25 / HALT THEN HOST CONTROLLER RESET\r\n");
    Print(u"AGGRESSIVE RESET TEST / NO DMA\r\n");

    s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);
    if(EFI_ERROR(s)){done(reads,s);return s;}

    for(i=0;i<n;i++){
        EFI_PCI_IO_PROTOCOL *q=NULL;
        if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,
            (void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
        if(EFI_ERROR(cfg32(q,8,&cls))) continue;
        if(((cls>>24)&255)==0x0c && ((cls>>16)&255)==0x03 &&
           ((cls>>8)&255)==0x30){p=q;break;}
    }
    if(!p){done(reads,EFI_NOT_FOUND);return EFI_NOT_FOUND;}

    cfg32(p,0,&id);
    cfg32(p,0x10,&bar0);
    cfg32(p,0x14,&bar1);
    bar=((UINT64)bar1<<32)|((UINT64)bar0&~15ULL);

    if(EFI_ERROR(mmio32(p,0,&cap0))){done(reads,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}
    reads++;
    opbase=cap0&255;

    if(EFI_ERROR(mmio32(p,opbase,&cmd))){done(reads,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}
    reads++;
    if(EFI_ERROR(mmio32(p,opbase+4,&status))){done(reads,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}
    reads++;

    Print(u"PCI ID=%04x:%04x BAR=%016lx OPBASE=%02x\r\n",
          id&65535,id>>16,bar,opbase);
    Print(u"INITIAL USBCMD=%08x USBSTS=%08x HCH=%u\r\n",
          cmd,status,status&STS_HCH?1:0);

    /*
     * V25 deliberately performs the missing transition from V23:
     * if the controller is running, halt it first. Do not resume it.
     * This gives us exactly two MMIO control writes: HALT, then HCRST.
     */
    if(!(status&STS_HCH)){
        Print(u"HALTING: CLEAR RUN/INTERRUPT ENABLE BITS BEFORE RESET\r\n");
        s=mmio_write32(p,opbase,cmd&~(CMD_RUN|CMD_INTE));
        if(EFI_ERROR(s)){
            Print(u"HALT WRITE FAIL %r\r\n",s);
            done(reads,s);
            return s;
        }

        BOOLEAN halted=FALSE;
        for(UINTN t=0;t<1000;t++){
            uefi_call_wrapper(BS->Stall,1,1000);
            s=mmio32(p,opbase+4,&status);
            if(EFI_ERROR(s)){result=s;break;}
            reads++;
            if(status&STS_HCH){halted=TRUE;break;}
        }
        if(EFI_ERROR(result)||!halted){
            Print(u"HALT HANDSHAKE FAIL USBSTS=%08x\r\n",status);
            result=EFI_TIMEOUT;
            done(reads,result);
            return result;
        }
        Print(u"HALT COMPLETE USBSTS=%08x HCH=1\r\n",status);
    } else {
        Print(u"ALREADY HALTED: PROCEEDING TO RESET\r\n");
    }

    Print(u"RESETTING: SET HOST CONTROLLER RESET BIT\r\n");
    s=mmio_write32(p,opbase,CMD_RESET);
    if(EFI_ERROR(s)){
        Print(u"RESET WRITE FAIL %r\r\n",s);
        done(reads,s);
        return s;
    }

    BOOLEAN reset_complete=FALSE;
    for(UINTN t=0;t<10000;t++){
        uefi_call_wrapper(BS->Stall,1,1000);
        s=mmio32(p,opbase,&after);
        if(EFI_ERROR(s)){result=s;break;}
        reads++;
        if(!(after&CMD_RESET)){reset_complete=TRUE;break;}
    }
    if(EFI_ERROR(result)||!reset_complete){
        Print(u"RESET HANDSHAKE FAIL USBCMD=%08x\r\n",after);
        result=EFI_TIMEOUT;
        done(reads,result);
        return result;
    }

    s=mmio32(p,opbase+4,&after_status);
    if(EFI_ERROR(s)){
        done(reads,s);
        return s;
    }
    reads++;

    Print(u"RESET COMPLETE USBCMD=%08x USBSTS=%08x HCH=%u CNR=%u\r\n",
          after,after_status,after_status&STS_HCH?1:0,
          after_status&STS_CNR?1:0);

    if(after&CMD_RUN){
        Print(u"POSTCONDITION FAIL: RUN BIT STILL SET\r\n");
        result=EFI_DEVICE_ERROR;
    } else if(after_status&STS_CNR){
        Print(u"POSTCONDITION FAIL: CONTROLLER NOT READY\r\n");
        result=EFI_TIMEOUT;
    } else {
        Print(u"POST-RESET STATE VERIFIED: HALTED AND READY\r\n");
        Print(u"V25: PROGRAM DCBAAP WHILE HALTED / NO DMA\r\n");

        s=uefi_call_wrapper(BS->AllocatePages,4,AllocateAnyPages,
                            EfiBootServicesData,1,&page);
        if(EFI_ERROR(s)){
            Print(u"DCBAA ALLOC FAIL %r\r\n",s);
            result=s;
        } else {
            uefi_call_wrapper(BS->SetMem,3,(void*)(UINTN)page,4096,0);
            dcbaap=(UINT64)page;

            if(dcbaap & 0x3fULL){
                Print(u"DCBAA ALIGN FAIL %016lx\r\n",dcbaap);
                result=EFI_BAD_BUFFER_SIZE;
            } else {
                Print(u"DCBAAP WRITE %016lx\r\n",dcbaap);
                s=uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint64,0,
                                    (UINT64)(opbase+0x30),1,&dcbaap);
                if(EFI_ERROR(s)){
                    Print(u"DCBAAP WRITE FAIL %r\r\n",s);
                    result=s;
                } else {
                    s=uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint64,0,
                                         (UINT64)(opbase+0x30),1,&dcbaap_read);
                    reads++;
                    if(EFI_ERROR(s)){
                        result=s;
                    } else {
                        reads++;
                        Print(u"DCBAAP READBACK %016lx\r\n",dcbaap_read);
                        if(dcbaap_read!=dcbaap){
                            Print(u"DCBAAP READBACK FAIL\r\n");
                            result=EFI_DEVICE_ERROR;
                        } else {
                            Print(u"DCBAAP PROGRAM: PASS / RUN=0 / NO DMA\r\n");
                        }
                    }
                }
            }
            uefi_call_wrapper(BS->FreePages,2,page,1);
        }
    }

    if(hs)FreePool(hs);
    done(reads,result);
    return result;
}
