#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#define CMD_RUN   0x00000001U
#define CMD_INTE  0x00000004U
#define STS_HCH   0x00000001U
#define STS_CNR   0x00000800U
#define CAPLENGTH_MASK 0x000000FFU
#define HCS1_MAXSLOTS_MASK 0x000000FFU
#define CONFIG_MAXSLOTS_MASK 0x000000FFU
#define CRCR_RCS 0x00000001ULL
#define CRCR_ADDR_MASK 0xFFFFFFFFFFFFFFC0ULL
#define ERST_ADDR_MASK 0xFFFFFFFFFFFFFFC0ULL
#define ERDP_ADDR_MASK 0xFFFFFFFFFFFFFFF0ULL
#define ERST_SEGMENT_TRBS 16U

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
    Print(u"\r\nV27 COMPLETE / %u MMIO READS / %u CONTROL WRITES / CONTROLLED ERST DMA READ ONLY\r\n",reads,writes);
    Print(u"RESULT: %r\r\nEXIT 5 SEC...\r\n",s);
    uefi_call_wrapper(BS->Stall,1,5000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_HANDLE *hs=NULL;
    EFI_PCI_IO_PROTOCOL *p=NULL;
    EFI_STATUS s,result=EFI_SUCCESS;
    UINTN n=0,i,t;
    UINT32 cls=0,id=0,bar0=0,bar1=0,cap0=0,hcs1=0;
    UINT32 opbase,rtsoff,runtime0,cmd=0,status=0,config_read=0;
    UINT32 reads=0,writes=0,max_slots;
    UINT32 iman=0,erstsz_read=0;
    UINT64 bar=0,dcbaap=0,crcr=0,erstba=0,erdp=0;
    UINT64 dcbaap_read=0,crcr_read=0,erstba_read=0,erdp_read=0;
    EFI_PHYSICAL_ADDRESS dcbaa_page=0,cr_page=0,event_page=0,erst_page=0;
    UINT64 *erst_entry;
    BOOLEAN halted=FALSE;

    InitializeLib(image,st);
    Print(u"TOSHIBA xHCI V27 / EVENT RING INITIALIZATION WHILE HALTED\r\n");
    Print(u"CONTROLLED FIRST ERST DMA READ / NO RUN / NO COMMAND FETCH\r\n");

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

    if(EFI_ERROR(mmio32(p,0x04,&hcs1))){done(reads,writes,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}
    reads++;
    max_slots=hcs1&HCS1_MAXSLOTS_MASK;

    if(EFI_ERROR(mmio32(p,0x18,&rtsoff))){done(reads,writes,EFI_DEVICE_ERROR);return EFI_DEVICE_ERROR;}
    reads++;
    runtime0=rtsoff;

    Print(u"PCI ID=%04x:%04x BAR=%016lx OPBASE=%02x RTSOFF=%08x\r\n",
          id&65535,id>>16,bar,opbase,rtsoff);
    Print(u"INITIAL USBCMD=%08x USBSTS=%08x HCH=%u MAXSLOTS=%u\r\n",
          cmd,status,status&STS_HCH?1:0,max_slots);

    if(max_slots==0){
        Print(u"PRECONDITION FAIL: MAXSLOTS=0\r\n");
        result=EFI_DEVICE_ERROR;
        goto out;
    }

    if(status&STS_CNR){
        Print(u"WAITING: CONTROLLER NOT READY AFTER RESET\r\n");
        for(t=0;t<10000;t++){
            uefi_call_wrapper(BS->Stall,1,1000);
            if(EFI_ERROR(mmio32(p,opbase+4,&status))){result=EFI_DEVICE_ERROR;goto out;}
            reads++;
            if(!(status&STS_CNR)) break;
        }
        if(status&STS_CNR){
            Print(u"CNR TIMEOUT USBSTS=%08x\r\n",status);
            result=EFI_TIMEOUT;
            goto out;
        }
    }

    if(!(status&STS_HCH)){
        UINT32 halted_cmd=cmd&~(CMD_RUN|CMD_INTE);
        Print(u"HALTING: CLEAR RUN/INTERRUPT ENABLE BITS BEFORE INIT\r\n");
        s=mmio_write32(p,opbase,halted_cmd);
        if(EFI_ERROR(s)){result=s;goto out;}
        writes++;
        for(t=0;t<1000;t++){
            uefi_call_wrapper(BS->Stall,1,1000);
            if(EFI_ERROR(mmio32(p,opbase+4,&status))){result=EFI_DEVICE_ERROR;goto out;}
            reads++;
            if(status&STS_HCH){halted=TRUE;break;}
        }
        if(!halted){
            Print(u"HALT TIMEOUT USBSTS=%08x\r\n",status);
            result=EFI_TIMEOUT;
            goto out;
        }
        Print(u"HALT COMPLETE USBSTS=%08x HCH=1\r\n",status);
    } else {
        halted=TRUE;
        Print(u"ALREADY HALTED: CONTINUING\r\n");
    }

    if(!halted){result=EFI_DEVICE_ERROR;goto out;}

    /* Allocate four isolated, zeroed pages. No ring is populated with commands. */
    s=uefi_call_wrapper(BS->AllocatePages,4,AllocateAnyPages,EfiBootServicesData,1,&dcbaa_page);
    if(EFI_ERROR(s)){result=s;goto out;}
    s=uefi_call_wrapper(BS->AllocatePages,4,AllocateAnyPages,EfiBootServicesData,1,&cr_page);
    if(EFI_ERROR(s)){result=s;goto free_dcbaa;}
    s=uefi_call_wrapper(BS->AllocatePages,4,AllocateAnyPages,EfiBootServicesData,1,&event_page);
    if(EFI_ERROR(s)){result=s;goto free_cr;}
    s=uefi_call_wrapper(BS->AllocatePages,4,AllocateAnyPages,EfiBootServicesData,1,&erst_page);
    if(EFI_ERROR(s)){result=s;goto free_event;}

    uefi_call_wrapper(BS->SetMem,3,(void*)(UINTN)dcbaa_page,4096,0);
    uefi_call_wrapper(BS->SetMem,3,(void*)(UINTN)cr_page,4096,0);
    uefi_call_wrapper(BS->SetMem,3,(void*)(UINTN)event_page,4096,0);
    uefi_call_wrapper(BS->SetMem,3,(void*)(UINTN)erst_page,4096,0);

    dcbaap=(UINT64)dcbaa_page;
    crcr=(UINT64)cr_page;
    erstba=(UINT64)erst_page;
    erdp=(UINT64)event_page;

    if((dcbaap&0x3fULL)||(crcr&0x3fULL)||(erstba&0x3fULL)||(erdp&0xfULL)){
        Print(u"ALIGNMENT FAIL DCBAAP=%016lx CRCR=%016lx ERSTBA=%016lx ERDP=%016lx\r\n",
              dcbaap,crcr,erstba,erdp);
        result=EFI_BAD_BUFFER_SIZE;
        goto free_erst;
    }

    /* V27 completes the pre-RUN operational data structures in spec order. */
    Print(u"V27: PROGRAM CONFIG + DCBAAP + CRCR WHILE HALTED\r\n");

    s=mmio_write32(p,opbase+0x38,1);
    if(EFI_ERROR(s)){result=s;goto free_erst;}
    writes++;
    if(EFI_ERROR(mmio32(p,opbase+0x38,&config_read))){result=EFI_DEVICE_ERROR;goto free_erst;}
    reads++;
    if((config_read&CONFIG_MAXSLOTS_MASK)!=1){
        Print(u"CONFIG READBACK FAIL=%08x\r\n",config_read);
        result=EFI_DEVICE_ERROR;
        goto free_erst;
    }

    s=mmio_write64(p,opbase+0x30,dcbaap);
    if(EFI_ERROR(s)){result=s;goto free_erst;}
    writes++;
    if(EFI_ERROR(mmio64(p,opbase+0x30,&dcbaap_read))){result=EFI_DEVICE_ERROR;goto free_erst;}
    reads++;
    if((dcbaap_read&0xffffffffffffffc0ULL)!=(dcbaap&0xffffffffffffffc0ULL)){
        Print(u"DCBAAP READBACK FAIL=%016lx EXPECTED=%016lx\r\n",dcbaap_read,dcbaap);
        result=EFI_DEVICE_ERROR;
        goto free_erst;
    }

    s=mmio_write64(p,opbase+0x18,crcr|CRCR_RCS);
    if(EFI_ERROR(s)){result=s;goto free_erst;}
    writes++;
    if(EFI_ERROR(mmio64(p,opbase+0x18,&crcr_read))){result=EFI_DEVICE_ERROR;goto free_erst;}
    reads++;
    if((crcr_read&CRCR_ADDR_MASK)!=crcr){
        Print(u"CRCR READBACK FAIL=%016lx EXPECTED=%016lx\r\n",crcr_read,crcr);
        result=EFI_DEVICE_ERROR;
        goto free_erst;
    }

    Print(u"CONFIG/DCBAAP/CRCR: PASS / RUN=0\r\n");

    /*
     * Primary interrupter, one 16-TRB event-ring segment.
     * Writing ERSTBA is the deliberate first DMA-capable step: the xHC
     * fetches ERST[0], then waits for Run/Stop=1. No command or transfer
     * ring is executed and interrupts remain disabled.
     */
    erst_entry=(UINT64*)(UINTN)erst_page;
    erst_entry[0]=erdp&ERDP_ADDR_MASK;
    ((UINT32*)erst_entry)[2]=ERST_SEGMENT_TRBS;
    ((UINT32*)erst_entry)[3]=0;

    Print(u"EVENT RING: ERST ENTRY BASE=%016lx SIZE=%u TRBS\r\n",erdp,ERST_SEGMENT_TRBS);
    Print(u"PROGRAMMING PRIMARY INTERRUPTER WHILE HALTED\r\n");

    s=mmio_write32(p,runtime0+0x20+0x08,1); /* ERSTSZ */
    if(EFI_ERROR(s)){result=s;goto free_erst;}
    writes++;

    s=mmio_write64(p,runtime0+0x20+0x18,erdp&ERDP_ADDR_MASK); /* ERDP */
    if(EFI_ERROR(s)){result=s;goto free_erst;}
    writes++;

    s=mmio_write64(p,runtime0+0x20+0x10,erstba&ERST_ADDR_MASK); /* ERSTBA */
    if(EFI_ERROR(s)){result=s;goto free_erst;}
    writes++;

    if(EFI_ERROR(mmio32(p,runtime0+0x20,&iman))){result=EFI_DEVICE_ERROR;goto free_erst;}
    reads++;
    if(EFI_ERROR(mmio32(p,runtime0+0x20+0x08,&erstsz_read))){result=EFI_DEVICE_ERROR;goto free_erst;}
    reads++;
    if(EFI_ERROR(mmio64(p,runtime0+0x20+0x10,&erstba_read))){result=EFI_DEVICE_ERROR;goto free_erst;}
    reads++;
    if(EFI_ERROR(mmio64(p,runtime0+0x20+0x18,&erdp_read))){result=EFI_DEVICE_ERROR;goto free_erst;}
    reads++;

    Print(u"IMAN=%08x ERSTSZ=%08x ERSTBA=%016lx ERDP=%016lx\r\n",
          iman,erstsz_read,erstba_read,erdp_read);

    if((iman&0x2U)!=0){
        Print(u"POSTCONDITION FAIL: INTERRUPTER ENABLED\r\n");
        result=EFI_DEVICE_ERROR;
        goto free_erst;
    }
    if((erstsz_read&0xffffU)!=1){
        Print(u"POSTCONDITION FAIL: ERSTSZ != 1\r\n");
        result=EFI_DEVICE_ERROR;
        goto free_erst;
    }
    if((erstba_read&ERST_ADDR_MASK)!=(erstba&ERST_ADDR_MASK)){
        Print(u"POSTCONDITION FAIL: ERSTBA MISMATCH\r\n");
        result=EFI_DEVICE_ERROR;
        goto free_erst;
    }
    if((erdp_read&ERDP_ADDR_MASK)!=(erdp&ERDP_ADDR_MASK)){
        Print(u"POSTCONDITION FAIL: ERDP MISMATCH\r\n");
        result=EFI_DEVICE_ERROR;
        goto free_erst;
    }

    if(EFI_ERROR(mmio32(p,opbase,&cmd))){result=EFI_DEVICE_ERROR;goto free_erst;}
    reads++;
    if(EFI_ERROR(mmio32(p,opbase+4,&status))){result=EFI_DEVICE_ERROR;goto free_erst;}
    reads++;

    Print(u"POST-EVENT-RING USBCMD=%08x USBSTS=%08x HCH=%u CNR=%u\r\n",
          cmd,status,status&STS_HCH?1:0,status&STS_CNR?1:0);

    if(cmd&CMD_RUN){
        Print(u"POSTCONDITION FAIL: RUN BIT SET\r\n");
        result=EFI_DEVICE_ERROR;
    } else if(!(status&STS_HCH)){
        Print(u"POSTCONDITION FAIL: CONTROLLER LEFT HALTED STATE\r\n");
        result=EFI_DEVICE_ERROR;
    } else if(status&STS_CNR){
        Print(u"POSTCONDITION FAIL: CONTROLLER NOT READY\r\n");
        result=EFI_TIMEOUT;
    } else {
        Print(u"V27 EVENT RING INITIALIZATION: PASS\r\n");
        Print(u"EXPECTED DMA: ERST[0] READ ONLY / NO COMMAND OR TRANSFER EXECUTION\r\n");
        Print(u"NO RUN / NO DOORBELL / INTERRUPTS DISABLED\r\n");
    }

free_erst:
    uefi_call_wrapper(BS->FreePages,2,erst_page,1);
free_event:
    uefi_call_wrapper(BS->FreePages,2,event_page,1);
free_cr:
    uefi_call_wrapper(BS->FreePages,2,cr_page,1);
free_dcbaa:
    uefi_call_wrapper(BS->FreePages,2,dcbaa_page,1);
out:
    if(hs)FreePool(hs);
    done(reads,writes,result);
    return result;
}