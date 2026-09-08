#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define CMD_RUN   0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE  0x00000004U
#define STS_HCH   0x00000001U
#define STS_CNR   0x00000800U

#define CAPLENGTH_MASK 0x000000FFU
#define HCSPARAMS1_MAXSLOTS_MASK 0x000000FFU
#define CONFIG_MAXSLOTS_MASK 0x000000FFU
#define CRCR_RCS 0x00000001ULL
#define CRCR_ADDR_MASK 0xFFFFFFFFFFFFFFC0ULL

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
static EFI_STATUS mmio64(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 *v) {
    return uefi_call_wrapper(p->Mem.Read,6,p,EfiPciIoWidthUint64,0,(UINT64)off,1,v);
}
static EFI_STATUS mmio_write64(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 v) {
    return uefi_call_wrapper(p->Mem.Write,6,p,EfiPciIoWidthUint64,0,(UINT64)off,1,&v);
}
static void done(UINT32 reads, UINT32 writes, EFI_STATUS s) {
    Print(u"\r\nV26 COMPLETE / %u MMIO READS / %u CONTROL WRITES / NO DMA\r\n",reads,writes);
    Print(u"RESULT: %r\r\nEXIT 5 SEC...\r\n",s);
    uefi_call_wrapper(BS->Stall,1,5000000);
}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs=NULL;
    EFI_PCI_IO_PROTOCOL *p=NULL;
    EFI_STATUS s,result=EFI_SUCCESS;
    UINTN n=0,i;
    UINT32 cls=0,id=0,bar0=0,bar1=0,cap0=0,hcs1=0;
    UINT32 opbase,cmd=0,status=0,after_status=0,config=0,config_read=0,reads=0,writes=0;
    UINT32 max_slots;
    UINT64 bar,crcr=0,crcr_read=0;
    EFI_PHYSICAL_ADDRESS page=0;

    InitializeLib(image,st);
    Print(u"TOSHIBA xHCI V26 / POST-RESET CONTROLLER INITIALIZATION\r\n");
    Print(u"PROGRAM CONFIG + COMMAND RING WHILE HALTED / NO DMA\r\n");

    s=LibLocateHandle(ByProtocol,&PciGuid,NULL,&n,&hs);
    if(EFI_ERROR(s)){done(reads,writes,s);return s;}

    for(i=0;i<n;i++){
        EFI_PCI_IO_PROTOCOL *q=NULL;
        if(EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol,6,hs[i],&PciGuid,
            (void**)&q,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
        if(EFI_ERROR(cfg32(q,8,&cls))) continue;
        if(((cls>>24)&255)==0x0c && ((cls>>16)&255)==0x03 &&
           ((cls>>8)&255)==0x30){p=q;break;}
    }
    if(!p){done(reads,writes,EFI_NOT_FOUND);return EFI_NOT_FOUND;}

    cfg32(p,0,&id);
    cfg32(p,0x10,&bar0);
    cfg32(p,0x14,&bar1);
    bar=((UINT64)bar1<<32)|((UINT64)bar0&~15ULL);

    if(EFI_ERROR(mmio32(p,0,&cap0))){done(reads,writes,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}
    reads++;
    opbase=cap0&CAPLENGTH_MASK;

    if(EFI_ERROR(mmio32(p,opbase,&cmd))){done(reads,writes,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}
    reads++;
    if(EFI_ERROR(mmio32(p,opbase+4,&status))){done(reads,writes,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}
    reads++;

    Print(u"PCI ID=%04x:%04x BAR=%016lx OPBASE=%02x\r\n",
          id&65535,id>>16,bar,opbase);
    Print(u"INITIAL USBCMD=%08x USBSTS=%08x HCH=%u\r\n",
          cmd,status,status&STS_HCH?1:0);

    /*
     * V23 established that this machine can leave the controller running
     * after the earlier tests. V26 therefore owns its halt transition
     * instead of treating HCH=1 as an external precondition.
     */
    if(!(status&STS_HCH)){
        UINT32 halted_cmd = cmd & ~(CMD_RUN | CMD_INTE);
        UINT32 poll_status = status;
        UINT32 poll;
        Print(u"HALTING: CLEAR RUN/INTERRUPT ENABLE BITS BEFORE INIT\\r\\n");
        if(EFI_ERROR(mmio_write32(p,opbase,halted_cmd))){
            Print(u"HALT WRITE FAIL\\r\\n");
            result=EFI_DEVICE_ERROR;
            goto out;
        }
        writes++;
        for(poll=0;poll<100;poll++){
            if(EFI_ERROR(mmio32(p,opbase+4,&poll_status))){
                result=EFI_DEVICE_ERROR;
                goto out;
            }
            reads++;
            if(poll_status&STS_HCH) break;
            uefi_call_wrapper(BS->Stall,1,1000);
        }
        if(!(poll_status&STS_HCH)){
            Print(u"HALT TIMEOUT: USBSTS=%08x\\r\\n",poll_status);
            result=EFI_TIMEOUT;
            goto out;
        }
        status=poll_status;
        cmd=halted_cmd;
        Print(u"HALT COMPLETE USBSTS=%08x HCH=1\\r\\n",status);
    } else {
        Print(u"PRECONDITION: CONTROLLER ALREADY HALTED\\r\\n");
    }

    if(EFI_ERROR(mmio32(p,0x04,&hcs1))){
        done(reads,writes,EFI_DEVICE_ERROR);
        return EFI_DEVICE_ERROR;
    }
    reads++;
    max_slots=hcs1&HCSPARAMS1_MAXSLOTS_MASK;
    Print(u"HCSPARAMS1=%08x MAX SLOTS=%u\r\n",hcs1,max_slots);
    if(max_slots==0){
        Print(u"PRECONDITION FAIL: CONTROLLER REPORTS ZERO MAX SLOTS\r\n");
        done(reads,writes,EFI_DEVICE_ERROR);
        return EFI_DEVICE_ERROR;
    }

    /*
     * V26 advances one step beyond V25: program the post-reset CONFIG
     * register while the controller is halted. One slot is enough for
     * this initialization probe and is valid on any controller reporting
     * at least one supported slot. RUN remains 0, so no DMA can begin.
     */
    config=1;
    Print(u"V26: PROGRAM CONFIG WHILE HALTED / MAXSLOTSEN=1\r\n");
    s=mmio_write32(p,opbase+0x38,config);
    if(EFI_ERROR(s)){
        Print(u"CONFIG WRITE FAIL %r\r\n",s);
        result=s;
        goto out;
    }
    writes++;

    s=mmio32(p,opbase+0x38,&config_read);
    if(EFI_ERROR(s)){
        result=s;
        goto out;
    }
    reads++;
    Print(u"CONFIG READBACK %08x\r\n",config_read);
    if((config_read&CONFIG_MAXSLOTS_MASK)!=1){
        Print(u"CONFIG READBACK FAIL: MAXSLOTSEN=%u\r\n",
              config_read&CONFIG_MAXSLOTS_MASK);
        result=EFI_DEVICE_ERROR;
        goto out;
    }
    Print(u"CONFIG PROGRAM: PASS / MAXSLOTSEN=1 / RUN=0 / NO DMA\r\n");

    /*
     * Allocate a zeroed page for the command ring and program CRCR while
     * halted. The ring is not populated and RUN is never set, so this is
     * an address-programming test only; the controller must not fetch it.
     */
    s=uefi_call_wrapper(BS->AllocatePages,4,AllocateAnyPages,
                        EfiBootServicesData,1,&page);
    if(EFI_ERROR(s)){
        Print(u"COMMAND RING ALLOC FAIL %r\r\n",s);
        result=s;
        goto out;
    }

    uefi_call_wrapper(BS->SetMem,3,(void*)(UINTN)page,4096,0);
    crcr=((UINT64)page)&CRCR_ADDR_MASK;
    if(crcr==0){
        Print(u"COMMAND RING ADDRESS INVALID\r\n");
        result=EFI_BAD_BUFFER_SIZE;
        goto free_page;
    }

    Print(u"V26: PROGRAM CRCR WHILE HALTED / NO COMMAND FETCH\r\n");
    Print(u"CRCR WRITE %016lx\r\n",crcr|CRCR_RCS);
    s=mmio_write64(p,opbase+0x18,crcr|CRCR_RCS);
    if(EFI_ERROR(s)){
        Print(u"CRCR WRITE FAIL %r\r\n",s);
        result=s;
        goto free_page;
    }
    writes++;

    s=mmio64(p,opbase+0x18,&crcr_read);
    if(EFI_ERROR(s)){
        result=s;
        goto free_page;
    }
    reads++;
    Print(u"CRCR READBACK %016lx\r\n",crcr_read);
    if((crcr_read&CRCR_ADDR_MASK)!=crcr){
        Print(u"CRCR READBACK FAIL: BASE=%016lx EXPECTED=%016lx\r\n",
              crcr_read&CRCR_ADDR_MASK,crcr);
        result=EFI_DEVICE_ERROR;
        goto free_page;
    }

    s=mmio32(p,opbase,&cmd);
    if(EFI_ERROR(s)){
        result=s;
        goto free_page;
    }
    reads++;
    s=mmio32(p,opbase+4,&after_status);
    if(EFI_ERROR(s)){
        result=s;
        goto free_page;
    }
    reads++;

    Print(u"POST-INIT USBCMD=%08x USBSTS=%08x HCH=%u CNR=%u\r\n",
          cmd,after_status,after_status&STS_HCH?1:0,
          after_status&STS_CNR?1:0);

    if(cmd&CMD_RUN){
        Print(u"POSTCONDITION FAIL: RUN BIT SET\r\n");
        result=EFI_DEVICE_ERROR;
    } else if(!(after_status&STS_HCH)){
        Print(u"POSTCONDITION FAIL: CONTROLLER LEFT HALTED STATE\r\n");
        result=EFI_DEVICE_ERROR;
    } else if(after_status&STS_CNR){
        Print(u"POSTCONDITION FAIL: CONTROLLER NOT READY\r\n");
        result=EFI_TIMEOUT;
    } else {
        Print(u"CONFIG + CRCR PROGRAMMING: PASS\r\n");
        Print(u"HALTED / NO COMMAND FETCH / NO DMA\r\n");
    }

free_page:
    uefi_call_wrapper(BS->FreePages,2,page,1);
out:
    if(hs)FreePool(hs);
    done(reads,writes,result);
    return result;
}
