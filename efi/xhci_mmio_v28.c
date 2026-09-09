#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include <efiusbio.h>

#define XHCI_MIN_VERSION       0x0100U
#define XHCI_CLASS             0x0c
#define XHCI_SUBCLASS          0x03
#define XHCI_PROG_IF           0x30
#define XHCI_STS_HCH           0x00000001U
#define XHCI_STS_CNR           0x00000800U

#define USB_CLASS_HID          0x03
#define HID_SUBCLASS_BOOT      0x01
#define HID_PROTOCOL_KEYBOARD  0x01
#define HID_PROTOCOL_MOUSE     0x02

#define DP_TYPE_END            0x7f
#define DP_TYPE_MESSAGING      0x03
#define DP_SUBTYPE_USB         0x05

#define HANDOFF_MAGIC          0x48494458U
#define HANDOFF_VERSION        1U
#define HANDOFF_MAX_DEVICES    2U
#define HANDOFF_MAX_REPORT     256U
#define HANDOFF_MAX_ENDPOINTS  8U

typedef struct {
    UINT8  length;
    UINT8  descriptor_type;
    UINT8  endpoint_address;
    UINT8  attributes;
    UINT16 max_packet_size;
    UINT8  interval;
} HANDOFF_ENDPOINT;

typedef struct {
    UINT8  kind;                 /* 1 keyboard, 2 mouse */
    UINT8  root_port;            /* 0 = unavailable */
    UINT8  speed;                /* 0 = unavailable from EFI_USB_IO */
    UINT8  configuration_value;
    UINT8  interface_number;
    UINT8  interface_protocol;
    UINT8  interrupt_in_endpoint;
    UINT8  endpoint_count;
    UINT16 vendor_id;
    UINT16 product_id;
    UINT16 bcd_usb;
    UINT16 max_packet_size;
    UINT8  interval;
    UINT16 report_length;
    UINT8  report_descriptor[HANDOFF_MAX_REPORT];
    HANDOFF_ENDPOINT endpoints[HANDOFF_MAX_ENDPOINTS];
} HANDOFF_HID_DEVICE;

typedef struct {
    UINT32 magic;
    UINT16 version;
    UINT16 size;
    UINT16 xhci_version;
    UINT16 device_count;
    UINT16 pci_vendor;
    UINT16 pci_device;
    UINT64 mmio_base;
    UINT32 hcsparams1;
    UINT32 hcsparams2;
    UINT32 hcsparams3;
    UINT32 hccparams1;
    UINT32 dboff;
    UINT32 rtsoff;
    UINT32 pagesize;
    UINT8  max_slots;
    UINT8  max_interrupters;
    UINT8  max_ports;
    UINT8  dma_ac64;
    UINT8  controller_halted;
    UINT8  controller_cnr;
    UINT8  reserved[2];
    HANDOFF_HID_DEVICE devices[HANDOFF_MAX_DEVICES];
} XHCI_HANDOFF;

typedef struct {
    UINT8 type;
    UINT8 subtype;
    UINT16 length;
} DP_HEADER;

typedef struct {
    DP_HEADER header;
    UINT8 parent_port;
    UINT8 interface_number;
} USB_DP_NODE;

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID UsbIoGuid = EFI_USB_IO_PROTOCOL_GUID;

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v)
{
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}

static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v)
{
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32, 0,
                              (UINT64)off, 1, v);
}

static UINT16 mmio16(EFI_PCI_IO_PROTOCOL *p, UINT32 off, EFI_STATUS *s)
{
    UINT16 v = 0;
    *s = uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint16, 0,
                            (UINT64)off, 1, &v);
    return v;
}

static BOOLEAN classify_hid(const EFI_USB_INTERFACE_DESCRIPTOR *d, UINT8 *kind)
{
    if (d->InterfaceClass != USB_CLASS_HID ||
        d->InterfaceSubClass != HID_SUBCLASS_BOOT)
        return FALSE;

    if (d->InterfaceProtocol == HID_PROTOCOL_KEYBOARD) {
        *kind = 1;
        return TRUE;
    }
    if (d->InterfaceProtocol == HID_PROTOCOL_MOUSE) {
        *kind = 2;
        return TRUE;
    }
    return FALSE;
}

static UINT8 find_root_port(EFI_DEVICE_PATH_PROTOCOL *path)
{
    UINT8 result = 0;
    UINT8 *p = (UINT8 *)path;

    while (p != NULL) {
        DP_HEADER *h = (DP_HEADER *)p;
        UINT16 len = h->length;

        if (len < sizeof(DP_HEADER) || h->type == DP_TYPE_END)
            break;

        if (h->type == DP_TYPE_MESSAGING &&
            h->subtype == DP_SUBTYPE_USB &&
            len >= sizeof(USB_DP_NODE)) {
            USB_DP_NODE *u = (USB_DP_NODE *)p;
            result = u->parent_port;
        }
        p += len;
    }
    return result;
}

static EFI_STATUS get_report_descriptor(EFI_USB_IO_PROTOCOL *usb,
                                        UINT8 interface_number,
                                        UINT8 *dst,
                                        UINTN capacity,
                                        UINTN *actual)
{
    EFI_USB_DEVICE_REQUEST req;
    UINT8 hid_desc[9];
    UINT16 report_size;
    UINT32 usb_status = 0;
    EFI_STATUS s;

    uefi_call_wrapper(BS->SetMem, 3, &req, sizeof(req), 0);
    req.RequestType = 0x81;       /* IN | standard | interface */
    req.Request = 0x06;           /* GET_DESCRIPTOR */
    req.Value = 0x2100;            /* HID descriptor */
    req.Index = interface_number;
    req.Length = sizeof(hid_desc);

    s = uefi_call_wrapper(usb->UsbControlTransfer, 7, usb, &req,
                          EfiUsbDataIn, 1000, hid_desc, sizeof(hid_desc),
                          &usb_status);
    if (EFI_ERROR(s) || hid_desc[1] != 0x21 || hid_desc[0] < 9)
        return EFI_NOT_FOUND;

    report_size = (UINT16)hid_desc[7] | ((UINT16)hid_desc[8] << 8);
    if (report_size > capacity)
        report_size = (UINT16)capacity;

    uefi_call_wrapper(BS->SetMem, 3, &req, sizeof(req), 0);
    req.RequestType = 0x81;
    req.Request = 0x06;
    req.Value = 0x2200;           /* HID report descriptor */
    req.Index = interface_number;
    req.Length = report_size;

    s = uefi_call_wrapper(usb->UsbControlTransfer, 7, usb, &req,
                          EfiUsbDataIn, 1000, dst, report_size, &usb_status);
    if (EFI_ERROR(s))
        return s;

    *actual = report_size;
    return EFI_SUCCESS;
}

static void finish(EFI_STATUS s, UINTN usb_handles, UINTN hid_candidates,
                   UINTN devices)
{
    Print(u"\r\nV28 COMPLETE / UEFI HID HANDOFF VALIDATION\r\n");
    Print(u"USB I/O HANDLES=%u HID CANDIDATES=%u HANDOFF DEVICES=%u\r\n",
          usb_handles, hid_candidates, devices);
    Print(u"RESULT: %r\r\n", s);
    Print(u"NO DIRECT xHCI MMIO WRITES / NO SERVICE DMA / NO PORT RESET / UEFI-ONLY USB DISCOVERY\r\n");
    Print(u"EXIT 5 SEC...\r\n");
    uefi_call_wrapper(BS->Stall, 1, 5000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS s = EFI_SUCCESS, rs;
    EFI_HANDLE *pci_handles = NULL, *usb_handles = NULL;
    EFI_PCI_IO_PROTOCOL *pci = NULL;
    UINTN pci_count = 0, usb_count = 0, i;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT32 cap0 = 0, hcs1 = 0, hcs2 = 0, hcs3 = 0, hcc1 = 0, dboff = 0, rtsoff = 0, status = 0, pagesize = 0;
    UINT32 opbase;
    UINT16 version;
    XHCI_HANDOFF handoff;
    UINTN hid_candidates = 0;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V28 / EXPLICIT UEFI -> SERVICE HANDOFF\r\n");
    Print(u"KEYBOARD + MOUSE ONLY / DISCOVERY ONLY\r\n");

    uefi_call_wrapper(BS->SetMem, 3, &handoff, sizeof(handoff), 0);
    handoff.magic = HANDOFF_MAGIC;
    handoff.version = HANDOFF_VERSION;
    handoff.size = sizeof(handoff);

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &pci_count, &pci_handles);
    if (EFI_ERROR(s))
        goto done;

    for (i = 0; i < pci_count; ++i) {
        EFI_PCI_IO_PROTOCOL *q = NULL;
        if (EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol, 6, pci_handles[i],
                                         &PciGuid, (void **)&q, image, NULL,
                                         EFI_OPEN_PROTOCOL_GET_PROTOCOL)))
            continue;
        if (EFI_ERROR(cfg32(q, 0x08, &cls)) ||
            EFI_ERROR(cfg32(q, 0x00, &id)))
            continue;

        if (((cls >> 24) & 0xffU) == XHCI_CLASS &&
            ((cls >> 16) & 0xffU) == XHCI_SUBCLASS &&
            ((cls >> 8) & 0xffU) == XHCI_PROG_IF) {
            pci = q;
            break;
        }
    }

    if (!pci) {
        s = EFI_NOT_FOUND;
        goto done;
    }

    cfg32(pci, 0x10, &bar0);
    cfg32(pci, 0x14, &bar1);
    handoff.mmio_base = ((UINT64)bar1 << 32) | ((UINT64)bar0 & ~0xFULL);
    handoff.pci_vendor = (UINT16)(id & 0xffffU);
    handoff.pci_device = (UINT16)(id >> 16);

    if (EFI_ERROR(mmio32(pci, 0, &cap0))) {
        s = EFI_DEVICE_ERROR;
        goto done;
    }
    opbase = cap0 & 0xffU;

    version = mmio16(pci, 0x02, &rs);
    if (EFI_ERROR(rs)) {
        s = rs;
        goto done;
    }
    handoff.xhci_version = version;

    if (version < XHCI_MIN_VERSION) {
        Print(u"UNSUPPORTED xHCI VERSION %04x (< 0100)\r\n", version);
        s = EFI_UNSUPPORTED;
        goto done;
    }

    if (EFI_ERROR(mmio32(pci, 0x04, &hcs1)) ||
        EFI_ERROR(mmio32(pci, 0x08, &hcs2)) ||
        EFI_ERROR(mmio32(pci, 0x0c, &hcs3)) ||
        EFI_ERROR(mmio32(pci, 0x10, &hcc1)) ||
        EFI_ERROR(mmio32(pci, 0x14, &dboff)) ||
        EFI_ERROR(mmio32(pci, 0x18, &rtsoff)) ||
        EFI_ERROR(mmio32(pci, opbase + 0x08, &pagesize)) ||
        EFI_ERROR(mmio32(pci, opbase + 0x04, &status))) {
        s = EFI_DEVICE_ERROR;
        goto done;
    }

    handoff.hcsparams1 = hcs1;
    handoff.hcsparams2 = hcs2;
    handoff.hcsparams3 = hcs3;
    handoff.hccparams1 = hcc1;
    handoff.dboff = dboff & ~0x3U;
    handoff.rtsoff = rtsoff & ~0x1fU;
    handoff.pagesize = pagesize;
    handoff.max_slots = (UINT8)(hcs1 & 0xffU);
    handoff.max_interrupters = (UINT8)(((hcs1 >> 8) & 0x7ffU) + 1U);
    handoff.max_ports = (UINT8)((hcs1 >> 24) & 0xffU);
    handoff.dma_ac64 = (hcc1 & 1U) ? 1 : 0;
    handoff.controller_halted = (status & XHCI_STS_HCH) ? 1 : 0;
    handoff.controller_cnr = (status & XHCI_STS_CNR) ? 1 : 0;

    Print(u"xHCI %u.%02u PCI=%04x:%04x BAR=%016lx OPBASE=%02x\r\n",
          version >> 8, version & 0xff, handoff.pci_vendor,
          handoff.pci_device, handoff.mmio_base, opbase);
    Print(u"CAPS: SLOTS=%u INTR=%u PORTS=%u AC64=%u HCH=%u CNR=%u\r\n",
          handoff.max_slots, handoff.max_interrupters, handoff.max_ports,
          handoff.dma_ac64, handoff.controller_halted,
          handoff.controller_cnr);

    /*
     * EFI_USB_IO_PROTOCOL is the UEFI discovery source. Only HID boot
     * keyboard/mouse interfaces are copied into the handoff. Everything
     * else is intentionally invisible to the service.
     */
    s = LibLocateHandle(ByProtocol, &UsbIoGuid, NULL, &usb_count, &usb_handles);
    if (EFI_ERROR(s)) {
        s = EFI_SUCCESS;
        goto validate;
    }

    for (i = 0; i < usb_count; ++i) {
        EFI_USB_IO_PROTOCOL *usb = NULL;
        EFI_USB_DEVICE_DESCRIPTOR dd;
        EFI_USB_CONFIG_DESCRIPTOR cd;
        EFI_USB_INTERFACE_DESCRIPTOR iface;
        EFI_USB_ENDPOINT_DESCRIPTOR ep;
        EFI_DEVICE_PATH_PROTOCOL *path = NULL;
        HANDOFF_HID_DEVICE *d;
        UINT8 kind = 0;
        UINTN ep_i, report_len = 0;
        BOOLEAN got_in = FALSE;

        if (EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol, 6, usb_handles[i],
                                         &UsbIoGuid, (void **)&usb, image, NULL,
                                         EFI_OPEN_PROTOCOL_GET_PROTOCOL)))
            continue;

        if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetInterfaceDescriptor, 3,
                                         usb, &iface)))
            continue;

        if (!classify_hid(&iface, &kind))
            continue;

        ++hid_candidates;
        if (handoff.device_count >= HANDOFF_MAX_DEVICES) {
            Print(u"HANDOFF FAIL: MORE THAN %u KEYBOARD/MOUSE DEVICES\r\n", HANDOFF_MAX_DEVICES);
            s = EFI_BAD_BUFFER_SIZE;
            goto validate;
        }
        d = &handoff.devices[handoff.device_count];
        uefi_call_wrapper(BS->SetMem, 3, d, sizeof(*d), 0);
        d->kind = kind;
        d->interface_number = iface.InterfaceNumber;
        d->interface_protocol = iface.InterfaceProtocol;
        d->endpoint_count = iface.NumEndpoints > HANDOFF_MAX_ENDPOINTS ?
                             HANDOFF_MAX_ENDPOINTS : iface.NumEndpoints;

        if (!EFI_ERROR(uefi_call_wrapper(usb->UsbGetDeviceDescriptor, 3,
                                          usb, &dd))) {
            d->vendor_id = dd.IdVendor;
            d->product_id = dd.IdProduct;
            d->bcd_usb = dd.BcdUSB;
        }

        if (!EFI_ERROR(uefi_call_wrapper(usb->UsbGetConfigDescriptor, 3,
                                          usb, &cd)))
            d->configuration_value = cd.ConfigurationValue;

        if (!EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol, 6, usb_handles[i],
                                          &DevicePathProtocol, (void **)&path,
                                          image, NULL,
                                          EFI_OPEN_PROTOCOL_GET_PROTOCOL)))
            d->root_port = find_root_port(path);

        for (ep_i = 0; ep_i < d->endpoint_count; ++ep_i) {
            if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetEndpointDescriptor, 4,
                                             usb, (UINT8)ep_i, &ep)))
                continue;

            d->endpoints[ep_i].length = ep.Length;
            d->endpoints[ep_i].descriptor_type = ep.DescriptorType;
            d->endpoints[ep_i].endpoint_address = ep.EndpointAddress;
            d->endpoints[ep_i].attributes = ep.Attributes;
            d->endpoints[ep_i].max_packet_size = ep.MaxPacketSize;
            d->endpoints[ep_i].interval = ep.Interval;

            if ((ep.EndpointAddress & 0x80U) &&
                ((ep.Attributes & 0x03U) == 0x03U) && !got_in) {
                d->interrupt_in_endpoint = ep.EndpointAddress;
                d->max_packet_size = ep.MaxPacketSize;
                d->interval = ep.Interval;
                got_in = TRUE;
            }
        }

        if (!got_in) {
            Print(u"SKIP HID: no interrupt-IN endpoint\r\n");
            continue;
        }

        if (!EFI_ERROR(get_report_descriptor(usb, d->interface_number,
                                              d->report_descriptor,
                                              HANDOFF_MAX_REPORT,
                                              &report_len)))
            d->report_length = (UINT16)report_len;

        Print(u"HID[%u] %s VID=%04x PID=%04x PORT=%u IF=%u EP=%02x MPS=%u INT=%u REPORT=%u\r\n",
              handoff.device_count,
              kind == 1 ? u"KEYBOARD" : u"MOUSE",
              d->vendor_id, d->product_id, d->root_port,
              d->interface_number, d->interrupt_in_endpoint,
              d->max_packet_size, d->interval, d->report_length);

        ++handoff.device_count;
    }

validate:
    /*
     * This is a snapshot ABI, not an ownership transfer. No UEFI-created
     * xHCI rings, contexts, slot IDs, DMA buffers, or controller run state
     * are inherited by the service.
     */
    Print(u"HANDOFF MAGIC=%08x VERSION=%u SIZE=%u DEVICES=%u\r\n",
          handoff.magic, handoff.version, handoff.size,
          handoff.device_count);
    Print(u"HANDOFF CONTRACT: DISCOVERY FACTS ONLY / KEYBOARD + MOUSE ONLY\r\n");

    if (handoff.device_count > HANDOFF_MAX_DEVICES) {
        s = EFI_BAD_BUFFER_SIZE;
        goto done;
    }

    if (handoff.device_count == 0)
        Print(u"WARNING: NO BOOT-PROTOCOL KEYBOARD/MOUSE DISCOVERED\r\n");

done:
    if (usb_handles)
        FreePool(usb_handles);
    if (pci_handles)
        FreePool(pci_handles);

    finish(s, usb_count, hid_candidates, handoff.device_count);
    return s;
}
