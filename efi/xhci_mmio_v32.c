/* Version 32 — cumulative Gate 6 implementation */

#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include "usb_io_compat.h"

#define XHCI_MIN_VERSION       0x0100U
#define XHCI_CLASS             0x0cU
#define XHCI_SUBCLASS          0x03U
#define XHCI_PROG_IF           0x30U

#define CMD_RUN                0x00000001U
#define CMD_RESET              0x00000002U
#define CMD_INTE               0x00000004U
#define CMD_HSEE               0x00000008U
#define STS_HCH                0x00000001U
#define STS_HSE                0x00000004U
#define STS_CNR                0x00000800U

#define IMAN_IP                0x00000001U
#define IMAN_IE                0x00000002U

#define CRCR_RCS              0x0000000000000001ULL
#define TRB_CYCLE              0x00000001U
#define TRB_TYPE_SHIFT        10U
#define TRB_TYPE_MASK         0x0000fc00U
#define TRB_ENABLE_SLOT       9U
#define TRB_ADDRESS_DEVICE     11U
#define TRB_LINK               6U
#define TRB_LINK_TOGGLE        0x00000002U
#define TRB_COMMAND_COMPLETION 33U
#define TRB_PORT_STATUS_CHANGE 34U
#define CC_SUCCESS             1U

#define ERST_ADDR_MASK         0xffffffffffffffc0ULL
#define ERDP_ADDR_MASK         0xfffffffffffffff0ULL
#define CMD_TRBS               256U
#define EVENT_TRBS             16U
#define MAX_SCRATCHPADS        1024U

#define PORTSC_CCS              0x00000001U
#define PORTSC_PED              0x00000002U
#define PORTSC_PR               0x00000010U
#define PORTSC_PLS_MASK         0x000001e0U
#define PORTSC_PP               0x00000200U
#define PORTSC_SPEED_MASK       0x00003c00U
#define PORTSC_SPEED_SHIFT      10U
#define PORTSC_PIC_MASK         0x0000c000U
#define PORTSC_LWS              0x00010000U
#define PORTSC_CSC              0x00020000U
#define PORTSC_PEC              0x00040000U
#define PORTSC_WRC              0x00080000U
#define PORTSC_OCC              0x00100000U
#define PORTSC_PRC              0x00200000U
#define PORTSC_PLC              0x00400000U
#define PORTSC_CEC              0x00800000U
#define PORTSC_WCE              0x02000000U
#define PORTSC_WDE              0x04000000U
#define PORTSC_WOE              0x08000000U
#define PORTSC_RW_MASK          (PORTSC_PED | PORTSC_PR | PORTSC_PLS_MASK | PORTSC_PP | PORTSC_PIC_MASK | PORTSC_LWS | PORTSC_WCE | PORTSC_WDE | PORTSC_WOE)

#define USB_SPEED_LOW           1U
#define USB_SPEED_FULL          2U
#define USB2_PLS_U0             0U

#define HID_HANDOFF_MAGIC       0x48494458U
#define HID_HANDOFF_VERSION     1U
#define HID_HANDOFF_MAX_REPORT  256U
#define HID_HANDOFF_MAX_ENDPOINTS 8U

typedef struct {
    UINT8  length;
    UINT8  descriptor_type;
    UINT8  endpoint_address;
    UINT8  attributes;
    UINT16 max_packet_size;
    UINT8  interval;
} HANDOFF_ENDPOINT;

typedef struct {
    UINT8  kind;
    UINT8  root_port;
    UINT8  speed;
    UINT8  configuration_value;
    UINT8  interface_number;
    UINT8  interface_protocol;
    UINT8  interrupt_in_endpoint;
    UINT8  endpoint_count;
    UINT16 vendor_id;
    UINT16 product_id;
    UINT16 bcd_usb;
    UINT8  ep0_mps_descriptor;
    UINT8  reserved0;
    UINT16 interrupt_max_packet_size;
    UINT8  interval;
    UINT8  reserved1;
    UINT16 report_length;
    UINT8  report_descriptor[HID_HANDOFF_MAX_REPORT];
    HANDOFF_ENDPOINT endpoints[HID_HANDOFF_MAX_ENDPOINTS];
} HANDOFF_HID_DEVICE;

typedef struct {
    UINT32 magic;
    UINT16 version;
    UINT16 size;
    UINT16 xhci_version;
    UINT16 device_count;
    UINT16 pci_segment;
    UINT8  pci_bus;
    UINT8  pci_device;
    UINT8  pci_function;
    UINT8  reserved0;
    UINT16 pci_vendor;
    UINT16 pci_device_id;
    UINT8  reserved1[2];
    HANDOFF_HID_DEVICE keyboard;
} XHCI_HANDOFF;

typedef struct {
    VOID *host;
    VOID *map;
    EFI_PHYSICAL_ADDRESS dev;
    UINTN pages;
    BOOLEAN live;
} DMA_OBJ;

typedef struct {
    EFI_HANDLE controller_handle;
    EFI_PCI_IO_PROTOCOL *pci;
    XHCI_HANDOFF handoff;
} BRIDGE_BINDING;

typedef struct {
    UINT8 pci_device;
    UINT8 pci_function;
    UINT8 root_port;
    UINT8 usb_nodes;
} PATH_INFO;

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID UsbIoGuid = EFI_USB_IO_PROTOCOL_GUID;
static const CHAR16 *fail_stage = u"NONE";
static const CHAR16 *fail_op = u"NONE";
static EFI_STATUS fail_status = EFI_SUCCESS;

static void remember_fail(const CHAR16 *stage, const CHAR16 *op, EFI_STATUS s)
{
    if (!EFI_ERROR(fail_status)) {
        fail_stage = stage;
        fail_op = op;
        fail_status = s;
    }
}

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v)
{
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, off, 1, v);
}

static EFI_STATUS mmio32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 *v)
{
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32, 0,
                              (UINT64)off, 1, v);
}

static EFI_STATUS mmio16(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT16 *v)
{
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint16, 0,
                              (UINT64)off, 1, v);
}

static EFI_STATUS mmio_write32(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT32 v)
{
    return uefi_call_wrapper(p->Mem.Write, 6, p, EfiPciIoWidthUint32, 0,
                              (UINT64)off, 1, &v);
}

static EFI_STATUS mmio_read64(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 *v)
{
    UINT32 lo, hi;
    EFI_STATUS s = mmio32(p, off, &lo);
    if (EFI_ERROR(s)) return s;
    s = mmio32(p, off + 4U, &hi);
    if (EFI_ERROR(s)) return s;
    *v = ((UINT64)hi << 32) | lo;
    return EFI_SUCCESS;
}

static EFI_STATUS mmio_write64(EFI_PCI_IO_PROTOCOL *p, UINT32 off, UINT64 v)
{
    EFI_STATUS s = mmio_write32(p, off, (UINT32)v);
    if (EFI_ERROR(s)) return s;
    return mmio_write32(p, off + 4U, (UINT32)(v >> 32));
}

static EFI_STATUS pci_location(EFI_PCI_IO_PROTOCOL *p,
                               UINTN *segment, UINTN *bus,
                               UINTN *device, UINTN *function)
{
    return uefi_call_wrapper(p->GetLocation, 5, p,
                              segment, bus, device, function);
}

static BOOLEAN classify_boot_keyboard(const EFI_USB_INTERFACE_DESCRIPTOR *d)
{
    return d->InterfaceClass == 0x03U &&
           d->InterfaceSubClass == 0x01U &&
           d->InterfaceProtocol == 0x01U;
}

static UINT8 path_root_port(EFI_DEVICE_PATH_PROTOCOL *path,
                            UINT8 *usb_nodes,
                            UINT8 *pci_device,
                            UINT8 *pci_function)
{
    UINT8 *p = (UINT8 *)path;
    UINT8 root = 0, count = 0;
    UINT8 last_dev = 0, last_fun = 0;

    while (p) {
        EFI_DEVICE_PATH_PROTOCOL *h = (EFI_DEVICE_PATH_PROTOCOL *)p;
        UINT16 len = h->Length;
        if (len < sizeof(EFI_DEVICE_PATH_PROTOCOL) || h->Type == 0x7fU)
            break;

        if (h->Type == 0x01U && h->SubType == 0x01U && len >= 6U) {
            last_fun = p[4];
            last_dev = p[5];
        }

        if (h->Type == 0x03U && h->SubType == 0x05U && len >= 6U) {
            root = p[4];
            ++count;
        }
        p += len;
    }

    *usb_nodes = count;
    *pci_device = last_dev;
    *pci_function = last_fun;
    return root;
}

static EFI_STATUS find_controller_for_usb(EFI_DEVICE_PATH_PROTOCOL *path,
                                           EFI_HANDLE *controller)
{
    EFI_DEVICE_PATH_PROTOCOL *lookup = path;
    return uefi_call_wrapper(BS->LocateDevicePath, 3, &PciGuid,
                              &lookup, controller);
}

static EFI_STATUS find_keyboard_handoff(EFI_HANDLE image,
                                        XHCI_HANDOFF *handoff,
                                        EFI_HANDLE *controller_handle)
{
    EFI_HANDLE *usb_handles = NULL;
    UINTN usb_count = 0, i;
    EFI_STATUS s;
    UINTN keyboard_count = 0;

    s = LibLocateHandle(ByProtocol, &UsbIoGuid, NULL, &usb_count, &usb_handles);
    if (EFI_ERROR(s)) {
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < usb_count; ++i) {
        EFI_USB_IO_PROTOCOL *usb = NULL;
        EFI_USB_INTERFACE_DESCRIPTOR iface;
        EFI_USB_DEVICE_DESCRIPTOR dd;
        EFI_USB_CONFIG_DESCRIPTOR cd;
        EFI_USB_ENDPOINT_DESCRIPTOR ep;
        EFI_DEVICE_PATH_PROTOCOL *path = NULL;
        EFI_HANDLE controller = NULL;
        PATH_INFO pi;
        UINTN e, endpoint_count;
        BOOLEAN got_in = FALSE;
        HANDOFF_HID_DEVICE *d;

        if (EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol, 6, usb_handles[i],
                                         &UsbIoGuid, (VOID **)&usb, image, NULL,
                                         EFI_OPEN_PROTOCOL_GET_PROTOCOL)))
            continue;
        if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetInterfaceDescriptor, 3,
                                         usb, &iface)))
            continue;
        if (!classify_boot_keyboard(&iface))
            continue;

        ++keyboard_count;
        if (keyboard_count != 1U) {
            s = EFI_ALREADY_STARTED;
            remember_fail(u"DISCOVERY", u"MULTIPLE KEYBOARDS", s);
            FreePool(usb_handles);
            return s;
        }

        if (EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol, 6, usb_handles[i],
                                         &DevicePathProtocol, (VOID **)&path,
                                         image, NULL,
                                         EFI_OPEN_PROTOCOL_GET_PROTOCOL))) {
            s = EFI_NOT_FOUND;
            remember_fail(u"DISCOVERY", u"DEVICE PATH", s);
            FreePool(usb_handles);
            return s;
        }

        pi.usb_nodes = 0;
        pi.pci_device = 0;
        pi.pci_function = 0;
        handoff->keyboard.root_port = path_root_port(path, &pi.usb_nodes,
                                                      &pi.pci_device,
                                                      &pi.pci_function);
        if (pi.usb_nodes != 1U || handoff->keyboard.root_port == 0U) {
            s = EFI_UNSUPPORTED;
            remember_fail(u"DISCOVERY", u"DIRECT ROOT USB PATH", s);
            FreePool(usb_handles);
            return s;
        }

        s = find_controller_for_usb(path, &controller);
        if (EFI_ERROR(s) || !controller) {
            remember_fail(u"DISCOVERY", u"LOCATE PCI CONTROLLER", s);
            FreePool(usb_handles);
            return EFI_NOT_FOUND;
        }

        {
            EFI_PCI_IO_PROTOCOL *pci = NULL;
            UINT32 cls = 0, id = 0;
            UINTN seg = 0, bus = 0, dev = 0, fun = 0;

            s = uefi_call_wrapper(BS->OpenProtocol, 6, controller, &PciGuid,
                                  (VOID **)&pci, image, NULL,
                                  EFI_OPEN_PROTOCOL_GET_PROTOCOL);
            if (EFI_ERROR(s)) {
                remember_fail(u"DISCOVERY", u"PCI IO", s);
                FreePool(usb_handles);
                return s;
            }
            s = pci_location(pci, &seg, &bus, &dev, &fun);
            if (EFI_ERROR(s) || seg > 0xffffU || bus > 0xffU ||
                dev > 0x1fU || fun > 7U) {
                remember_fail(u"DISCOVERY", u"PCI LOCATION",
                              EFI_DEVICE_ERROR);
                FreePool(usb_handles);
                return EFI_DEVICE_ERROR;
            }
            if (EFI_ERROR(cfg32(pci, 0x08, &cls)) ||
                EFI_ERROR(cfg32(pci, 0x00, &id)) ||
                ((cls >> 24) & 0xffU) != XHCI_CLASS ||
                ((cls >> 16) & 0xffU) != XHCI_SUBCLASS ||
                ((cls >> 8) & 0xffU) != XHCI_PROG_IF) {
                s = EFI_UNSUPPORTED;
                remember_fail(u"DISCOVERY", u"CONTROLLER IS XHCI", s);
                FreePool(usb_handles);
                return s;
            }

            handoff->pci_segment = (UINT16)seg;
            handoff->pci_bus = (UINT8)bus;
            handoff->pci_device = (UINT8)dev;
            handoff->pci_function = (UINT8)fun;
            handoff->pci_vendor = (UINT16)(id & 0xffffU);
            handoff->pci_device_id = (UINT16)(id >> 16);
        }

        d = &handoff->keyboard;
        d->kind = 1U;
        d->interface_number = iface.InterfaceNumber;
        d->interface_protocol = iface.InterfaceProtocol;
        d->endpoint_count = iface.NumEndpoints;
        if (d->endpoint_count > HID_HANDOFF_MAX_ENDPOINTS)
            d->endpoint_count = HID_HANDOFF_MAX_ENDPOINTS;

        if (!EFI_ERROR(uefi_call_wrapper(usb->UsbGetDeviceDescriptor, 3,
                                          usb, &dd))) {
            d->vendor_id = dd.IdVendor;
            d->product_id = dd.IdProduct;
            d->bcd_usb = dd.BcdUSB;
            d->ep0_mps_descriptor = dd.MaxPacketSize0;
        }
        if (!EFI_ERROR(uefi_call_wrapper(usb->UsbGetConfigDescriptor, 3,
                                          usb, &cd)))
            d->configuration_value = cd.ConfigurationValue;

        endpoint_count = d->endpoint_count;
        for (e = 0; e < endpoint_count; ++e) {
            if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetEndpointDescriptor, 4,
                                             usb, (UINT8)e, &ep)))
                continue;
            d->endpoints[e].length = ep.Length;
            d->endpoints[e].descriptor_type = ep.DescriptorType;
            d->endpoints[e].endpoint_address = ep.EndpointAddress;
            d->endpoints[e].attributes = ep.Attributes;
            d->endpoints[e].max_packet_size = ep.MaxPacketSize;
            d->endpoints[e].interval = ep.Interval;
            if (!got_in && ep.Length >= 7U && ep.DescriptorType == 0x05U &&
                (ep.EndpointAddress & 0x80U) &&
                (ep.Attributes & 0x03U) == 0x03U &&
                ep.MaxPacketSize != 0U && ep.Interval != 0U) {
                d->interrupt_in_endpoint = ep.EndpointAddress;
                d->interrupt_max_packet_size = ep.MaxPacketSize;
                d->interval = ep.Interval;
                got_in = TRUE;
            }
        }
        if (!got_in) {
            s = EFI_UNSUPPORTED;
            remember_fail(u"DISCOVERY", u"KEYBOARD INTERRUPT-IN", s);
            FreePool(usb_handles);
            return s;
        }

        handoff->device_count = 1U;
        *controller_handle = controller;
        FreePool(usb_handles);
        return EFI_SUCCESS;
    }

    FreePool(usb_handles);
    return EFI_NOT_FOUND;
}

static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL *p, UINTN pages, DMA_OBJ *d)
{
    EFI_STATUS s;
    UINTN bytes, mapped;
    d->host = NULL;
    d->map = NULL;
    d->dev = 0;
    d->pages = pages;
    d->live = FALSE;
    if (!pages || pages > ((UINTN)-1) / 4096U)
        return EFI_BAD_BUFFER_SIZE;
    bytes = pages * 4096U;
    s = uefi_call_wrapper(p->AllocateBuffer, 6, p, AllocateAnyPages,
                           EfiBootServicesData, pages, &d->host, 0);
    if (EFI_ERROR(s)) return s;
    uefi_call_wrapper(BS->SetMem, 3, d->host, bytes, 0);
    mapped = bytes;
    s = uefi_call_wrapper(p->Map, 6, p,
                           EfiPciIoOperationBusMasterCommonBuffer,
                           d->host, &mapped, &d->dev, &d->map);
    if (EFI_ERROR(s) || mapped != bytes) {
        EFI_STATUS x = EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
        if (!EFI_ERROR(s) && d->map)
            uefi_call_wrapper(p->Unmap, 2, p, d->map);
        uefi_call_wrapper(p->FreeBuffer, 3, p, pages, d->host);
        d->host = NULL;
        d->map = NULL;
        d->dev = 0;
        return x;
    }
    d->live = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL *p, DMA_OBJ *d)
{
    EFI_STATUS s = EFI_SUCCESS, t;
    if (!d->live) return EFI_SUCCESS;
    if (d->map) {
        t = uefi_call_wrapper(p->Unmap, 2, p, d->map);
        if (EFI_ERROR(t)) s = t;
    }
    if (d->host) {
        t = uefi_call_wrapper(p->FreeBuffer, 3, p, d->pages, d->host);
        if (EFI_ERROR(t) && !EFI_ERROR(s)) s = t;
    }
    d->host = NULL;
    d->map = NULL;
    d->dev = 0;
    d->live = FALSE;
    return s;
}

static EFI_STATUS wait_hch(EFI_PCI_IO_PROTOCOL *p, UINT32 op, BOOLEAN halt,
                           UINTN loops, UINT32 *status, UINT32 *reads)
{
    UINTN i;
    EFI_STATUS s;
    for (i = 0; i < loops; ++i) {
        s = mmio32(p, op + 4U, status);
        ++*reads;
        if (EFI_ERROR(s)) return s;
        if (((*status & STS_HCH) != 0U) == halt)
            return EFI_SUCCESS;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS reset_xhci(EFI_PCI_IO_PROTOCOL *p, UINT32 op,
                             UINT32 *cmd, UINT32 *status,
                             UINT32 *reads, UINT32 *writes)
{
    UINTN i;
    EFI_STATUS s;
    s = mmio32(p, op, cmd); ++*reads;
    if (EFI_ERROR(s)) return s;
    *cmd &= ~(CMD_RUN | CMD_INTE | CMD_HSEE);
    *cmd |= CMD_RESET;
    s = mmio_write32(p, op, *cmd); ++*writes;
    if (EFI_ERROR(s)) return s;
    for (i = 0; i < 1000U; ++i) {
        s = mmio32(p, op, cmd); ++*reads;
        if (EFI_ERROR(s)) return s;
        if ((*cmd & CMD_RESET) == 0U) break;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    if (*cmd & CMD_RESET) return EFI_TIMEOUT;
    for (i = 0; i < 10000U; ++i) {
        s = mmio32(p, op + 4U, status); ++*reads;
        if (EFI_ERROR(s)) return s;
        if ((*status & STS_CNR) == 0U) return EFI_SUCCESS;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS find_supported_protocol(EFI_PCI_IO_PROTOCOL *p, UINT32 hcc,
                                          UINT8 port, UINT32 *cap_off,
                                          UINT8 *major, UINT8 *minor,
                                          UINT8 *slot_type, UINT32 *reads)
{
    UINT32 off = ((hcc >> 16) & 0xffffU) * 4U;
    UINT32 hdr, ports, slot, next;
    UINT8 po, pc;
    UINTN n = 0;
    EFI_STATUS s;

    while (off && n++ < 256U) {
        s = mmio32(p, off, &hdr); ++*reads;
        if (EFI_ERROR(s)) return s;
        if ((hdr & 0xffU) == 2U) {
            s = mmio32(p, off + 8U, &ports); ++*reads;
            if (EFI_ERROR(s)) return s;
            po = (UINT8)(ports & 0xffU);
            pc = (UINT8)((ports >> 8) & 0xffU);
            if (pc != 0U && port >= po && (UINT8)(port - po) < pc) {
                s = mmio32(p, off + 12U, &slot); ++*reads;
                if (EFI_ERROR(s)) return s;
                *cap_off = off;
                *major = (UINT8)((hdr >> 24) & 0xffU);
                *minor = (UINT8)((hdr >> 16) & 0xffU);
                *slot_type = (UINT8)(slot & 0x1fU);
                return EFI_SUCCESS;
            }
        }
        next = ((hdr >> 8) & 0xffU) * 4U;
        if (!next) break;
        off += next;
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS check_dma_addr(const DMA_OBJ *d, BOOLEAN ac64)
{
    if (!ac64 && d->dev > 0xffffffffULL)
        return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}

static EFI_STATUS pci_attributes_prepare(EFI_PCI_IO_PROTOCOL *p,
                                         UINT64 *original,
                                         UINT64 *changed)
{
    EFI_STATUS s;
    UINT64 supports = 0, attrs = 0;
    UINT64 need = EFI_PCI_IO_ATTRIBUTE_MEMORY |
                  EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;

    *original = 0;
    *changed = 0;
    s = uefi_call_wrapper(p->Attributes, 4, p,
                           EfiPciIoAttributeOperationSupported, 0,
                           &supports);
    if (EFI_ERROR(s)) return s;
    if ((supports & need) != need)
        return EFI_UNSUPPORTED;
    s = uefi_call_wrapper(p->Attributes, 4, p,
                           EfiPciIoAttributeOperationGet, 0, &attrs);
    if (EFI_ERROR(s)) return s;
    *original = attrs;
    if ((attrs & need) != need) {
        UINT64 enable = need & ~attrs;
        s = uefi_call_wrapper(p->Attributes, 4, p,
                               EfiPciIoAttributeOperationEnable,
                               enable, NULL);
        if (EFI_ERROR(s)) return s;
        *changed = enable;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS portsc_write(EFI_PCI_IO_PROTOCOL *p, UINT32 off,
                               UINT32 old, UINT32 set_bits, UINT32 clear_bits,
                               UINT32 write_one_change, UINT32 *writes)
{
    UINT32 v = old & PORTSC_RW_MASK;
    v &= ~clear_bits;
    v |= set_bits;
    v |= write_one_change;
    EFI_STATUS s = mmio_write32(p, off, v);
    ++*writes;
    return s;
}

static EFI_STATUS wait_port_reset_event(EFI_PCI_IO_PROTOCOL *p, UINT32 erdp_off,
                                        DMA_OBJ *event_ring, UINT8 port,
                                        UINTN *event_index, UINT8 *cycle,
                                        UINT32 *reads, UINT32 *writes)
{
    UINTN loops;
    for (loops = 0; loops < 20000U; ++loops) {
        volatile UINT32 *e = (volatile UINT32 *)((UINT8 *)event_ring->host +
                                                   (*event_index) * 16U);
        UINT32 dw0 = e[0], dw3 = e[3];
        if ((dw3 & TRB_CYCLE) == *cycle) {
            UINT32 type = (dw3 & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
            UINT8 event_port = (UINT8)(dw0 >> 24);
            ++*reads;
            if (type == TRB_PORT_STATUS_CHANGE && event_port == port) {
                *event_index = *event_index + 1U;
                if (*event_index == EVENT_TRBS) {
                    *event_index = 0;
                    *cycle ^= 1U;
                }
                {
                    UINT64 next = event_ring->dev + (*event_index) * 16U;
                    EFI_STATUS s = mmio_write64(p, erdp_off, next & ERDP_ADDR_MASK);
                    *writes += 2U;
                    if (EFI_ERROR(s)) return s;
                }
                return EFI_SUCCESS;
            }
            *event_index = *event_index + 1U;
            if (*event_index == EVENT_TRBS) {
                *event_index = 0;
                *cycle ^= 1U;
            }
            {
                UINT64 next = event_ring->dev + (*event_index) * 16U;
                EFI_STATUS s = mmio_write64(p, erdp_off, next & ERDP_ADDR_MASK);
                *writes += 2U;
                if (EFI_ERROR(s)) return s;
            }
        }
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS wait_command_completion(EFI_PCI_IO_PROTOCOL *p,
                                           UINT32 erdp_off, DMA_OBJ *event_ring,
                                           UINTN *event_index, UINT8 *cycle,
                                           UINT64 command_trb,
                                           UINT8 expected_slot,
                                           UINT8 *out_slot,
                                           UINT32 *reads, UINT32 *writes)
{
    UINTN loops;
    for (loops = 0; loops < 20000U; ++loops) {
        volatile UINT32 *e = (volatile UINT32 *)((UINT8 *)event_ring->host +
                                                   (*event_index) * 16U);
        UINT32 dw0 = e[0], dw1 = e[1], dw2 = e[2], dw3 = e[3];
        if ((dw3 & TRB_CYCLE) == *cycle) {
            UINT32 type = (dw3 & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
            UINT32 cc = (dw2 >> 24) & 0xffU;
            UINT8 slot = (UINT8)(dw3 >> 24);
            UINT64 ptr = ((UINT64)dw1 << 32) | dw0;
            ++*reads;
            *event_index = *event_index + 1U;
            if (*event_index == EVENT_TRBS) {
                *event_index = 0;
                *cycle ^= 1U;
            }
            {
                UINT64 next = event_ring->dev + (*event_index) * 16U;
                EFI_STATUS s = mmio_write64(p, erdp_off, next & ERDP_ADDR_MASK);
                *writes += 2U;
                if (EFI_ERROR(s)) return s;
            }
            if (type == TRB_COMMAND_COMPLETION &&
                cc == CC_SUCCESS && ptr == command_trb &&
                slot != 0U && slot <= expected_slot) {
                *out_slot = slot;
                return EFI_SUCCESS;
            }
        }
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static void fatal_running(void)
{
    Print(u"\r\nFATAL: XHCI NOT CONFIRMED HALTED\r\n");
    Print(u"DMA MAPPINGS RETAINED / NO FREE / MANUAL RECOVERY REQUIRED\r\n");
    for (;;) uefi_call_wrapper(BS->Stall, 1, 1000000);
}

static void press_a_key(EFI_SYSTEM_TABLE *st)
{
    EFI_INPUT_KEY key;
    Print(u"PRESS A KEY TO CONTINUE\r\n");
    for (;;) {
        if (st && st->ConIn &&
            !EFI_ERROR(uefi_call_wrapper(st->ConIn->ReadKeyStroke, 2,
                                          st->ConIn, &key)))
            return;
        uefi_call_wrapper(BS->Stall, 1, 10000);
    }
}

static EFI_STATUS clear_controller_refs(EFI_PCI_IO_PROTOCOL *p, UINT32 op,
                                        UINT32 ir, UINT32 *writes)
{
    EFI_STATUS s;
    s = mmio_write64(p, op + 0x18U, 0); *writes += 2U;
    if (EFI_ERROR(s)) return s;
    s = mmio_write64(p, op + 0x30U, 0); *writes += 2U;
    if (EFI_ERROR(s)) return s;
    s = mmio_write32(p, op + 0x38U, 0); ++*writes;
    if (EFI_ERROR(s)) return s;
    s = mmio_write32(p, ir + 0x08U, 0); ++*writes;
    if (EFI_ERROR(s)) return s;
    s = mmio_write64(p, ir + 0x10U, 0); *writes += 2U;
    if (EFI_ERROR(s)) return s;
    s = mmio_write64(p, ir + 0x18U, 0); *writes += 2U;
    return s;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS s = EFI_SUCCESS, ts;
    EFI_HANDLE controller_handle = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    XHCI_HANDOFF handoff;
    UINT64 original_attrs = 0, changed_attrs = 0;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT32 cap = 0, hcs1 = 0, hcs2 = 0, hcc = 0;
    UINT32 op = 0, db = 0, rt = 0, ir = 0;
    UINT32 cmd = 0, status = 0, iman = 0, page_size_reg = 0;
    UINT32 max_slots = 0, scratchpads = 0;
    UINT32 protocol_cap = 0;
    UINT8 protocol_major = 0, protocol_minor = 0, slot_type = 0;
    UINT8 port_speed = 0;
    UINT32 portsc = 0;
    UINT32 reads = 0, writes = 0;
    UINTN seg = 0, bus = 0, dev = 0, fun = 0;
    UINTN shift = 0, xhci_page = 0, scratch_pages = 0;
    UINTN event_index = 0;
    UINT8 event_cycle = 1U;
    BOOLEAN ac64 = FALSE;
    BOOLEAN halted = FALSE;
    BOOLEAN running = FALSE;
    BOOLEAN disconnected = FALSE;
    BOOLEAN attrs_changed = FALSE;
    BOOLEAN submitted = FALSE;
    BOOLEAN address_submitted = FALSE;
    BOOLEAN controller_refs_cleared = FALSE;
    DMA_OBJ dcbaa_d = {0}, scratch_array_d = {0}, cmd_ring_d = {0},
             event_ring_d = {0}, erst_d = {0}, input_ctx_d = {0},
             output_ctx_d = {0}, ep0_ring_d = {0};
    DMA_OBJ *scratch = NULL;
    UINT64 *dcbaa = NULL, *scratch_array = NULL, *erst = NULL;
    UINT32 *cr = NULL, *input = NULL, *output = NULL, *ep0_ring = NULL;
    UINT8 slot_id = 0;
    UINT32 address_cmd_offset = 16U;

    InitializeLib(image, st);
    uefi_call_wrapper(BS->SetMem, 3, &handoff, sizeof(handoff), 0);
    handoff.magic = HID_HANDOFF_MAGIC;
    handoff.version = HID_HANDOFF_VERSION;
    handoff.size = sizeof(handoff);

    Print(u"TOSHIBA xHCI V32 / GATE 6 / PORT RESET + ADDRESS DEVICE\r\n");
    Print(u"UEFI KEYBOARD -> QUIESCE -> FRESH xHCI / LS+FS ONLY\r\n");

    /* Stage 1: UEFI discovery producer. No xHCI MMIO writes occur here. */
    s = find_keyboard_handoff(image, &handoff, &controller_handle);
    if (EFI_ERROR(s)) {
        remember_fail(u"DISCOVERY", u"KEYBOARD HANDOFF", s);
        goto out;
    }

    Print(u"UEFI SELECT: KEYBOARD VID=%04x PID=%04x PORT=%u IF=%u EP=%02x\r\n",
          handoff.keyboard.vendor_id, handoff.keyboard.product_id,
          handoff.keyboard.root_port, handoff.keyboard.interface_number,
          handoff.keyboard.interrupt_in_endpoint);
    Print(u"UEFI EP0 DESCRIPTOR MPS=%u / INITIAL ADDRESS MPS=8\r\n",
          handoff.keyboard.ep0_mps_descriptor);
    Print(u"UEFI CONTROLLER=%04x:%02x:%02x.%x %04x:%04x\r\n",
          handoff.pci_segment, handoff.pci_bus, handoff.pci_device,
          handoff.pci_function, handoff.pci_vendor,
          handoff.pci_device_id);
    Print(u"HANDOFF MAGIC=%08x VERSION=%u SIZE=%u DEVICES=%u\r\n",
          handoff.magic, handoff.version, handoff.size,
          handoff.device_count);

    if (handoff.magic != HID_HANDOFF_MAGIC ||
        handoff.version != HID_HANDOFF_VERSION ||
        handoff.size != sizeof(handoff) || handoff.device_count != 1U ||
        handoff.keyboard.kind != 1U || handoff.keyboard.root_port == 0U) {
        s = EFI_COMPROMISED_DATA;
        remember_fail(u"HANDOFF", u"VALIDATION", s);
        goto out;
    }

    /* Bridge-local copy: all subsequent code consumes only this snapshot. */
    {
        BRIDGE_BINDING binding;
        uefi_call_wrapper(BS->CopyMem, 3, &binding.handoff, &handoff,
                          sizeof(handoff));
        binding.controller_handle = controller_handle;
        binding.pci = NULL;
        handoff = binding.handoff;
    }

    /* Ownership handoff must complete before any active xHCI MMIO access. */
    s = uefi_call_wrapper(BS->DisconnectController, 3,
                          controller_handle, NULL, NULL);
    if (EFI_ERROR(s)) {
        remember_fail(u"QUIESCE", u"DISCONNECT CONTROLLER", s);
        goto out;
    }
    disconnected = TRUE;
    Print(u"UEFI USB STACK QUIESCED / DISCONNECT=PASS\r\n");

    s = uefi_call_wrapper(BS->OpenProtocol, 6, controller_handle, &PciGuid,
                          (VOID **)&p, image, NULL,
                          EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(s)) {
        remember_fail(u"BIND", u"PCI IO", s);
        goto out;
    }

    s = pci_location(p, &seg, &bus, &dev, &fun);
    if (EFI_ERROR(s) || seg != handoff.pci_segment ||
        bus != handoff.pci_bus || dev != handoff.pci_device ||
        fun != handoff.pci_function) {
        s = EFI_DEVICE_ERROR;
        remember_fail(u"BIND", u"CONTROLLER IDENTITY", s);
        goto out;
    }

    if (EFI_ERROR(cfg32(p, 0x00, &id)) || EFI_ERROR(cfg32(p, 0x08, &cls)) ||
        ((cls >> 24) & 0xffU) != XHCI_CLASS ||
        ((cls >> 16) & 0xffU) != XHCI_SUBCLASS ||
        ((cls >> 8) & 0xffU) != XHCI_PROG_IF ||
        (UINT16)(id & 0xffffU) != handoff.pci_vendor ||
        (UINT16)(id >> 16) != handoff.pci_device_id) {
        s = EFI_DEVICE_ERROR;
        remember_fail(u"BIND", u"XHCI CONTROLLER MATCH", s);
        goto out;
    }
    Print(u"BRIDGE BIND: HANDOFF CONTROLLER MATCH=PASS\r\n");

    s = pci_attributes_prepare(p, &original_attrs, &changed_attrs);
    if (EFI_ERROR(s)) {
        remember_fail(u"PCI ATTR", u"MEMORY+BUS MASTER", s);
        goto out;
    }
    attrs_changed = changed_attrs != 0U;
    Print(u"PCI ATTR: MEMORY+BUS MASTER READY / CHANGED=%c\r\n",
          attrs_changed ? 'Y' : 'N');

    s = cfg32(p, 0x10, &bar0);
    if (EFI_ERROR(s)) { remember_fail(u"PCI", u"BAR0", s); goto out; }
    if (bar0 & 1U) { s = EFI_UNSUPPORTED; remember_fail(u"PCI", u"BAR I/O", s); goto out; }
    if (((bar0 >> 1) & 3U) == 2U) {
        s = cfg32(p, 0x14, &bar1);
        if (EFI_ERROR(s)) { remember_fail(u"PCI", u"BAR1", s); goto out; }
    } else if (((bar0 >> 1) & 3U) != 0U) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"PCI", u"BAR TYPE", s);
        goto out;
    }

    {
        UINT64 mmio_base = (((bar0 >> 1) & 3U) == 2U) ?
            (((UINT64)bar1 << 32) | ((UINT64)bar0 & ~0xFULL)) :
            (UINT64)(bar0 & ~0xFULL);
        if (!mmio_base) {
            s = EFI_DEVICE_ERROR;
            remember_fail(u"PCI", u"BAR BASE", s);
            goto out;
        }
        Print(u"BAR=%016lx WIDTH=%s\r\n", mmio_base,
              (((bar0 >> 1) & 3U) == 2U) ? u"64" : u"32");
    }

    /* Cumulative V29/V30/V31 controller setup starts here. */
    s = mmio32(p, 0, &cap); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"CAPLENGTH", s); goto out; }
    op = cap & 0xffU;
    {
        UINT16 ver = 0;
        s = mmio16(p, 2, &ver); ++reads;
        if (EFI_ERROR(s) || ver < XHCI_MIN_VERSION) {
            if (!EFI_ERROR(s)) s = EFI_UNSUPPORTED;
            remember_fail(u"CAPS", u"HCIVERSION", s);
            goto out;
        }
        handoff.xhci_version = ver;
        Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x OPBASE=%02x\r\n",
              ver >> 8, ver & 0xffU,
              handoff.pci_vendor, handoff.pci_device_id, op);
    }
    s = mmio32(p, 4, &hcs1); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"HCSPARAMS1", s); goto out; }
    s = mmio32(p, 8, &hcs2); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"HCSPARAMS2", s); goto out; }
    s = mmio32(p, 0x10U, &hcc); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"HCCPARAMS1", s); goto out; }
    s = mmio32(p, 0x14U, &db); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"DBOFF", s); goto out; }
    s = mmio32(p, 0x18U, &rt); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"RTSOFF", s); goto out; }
    s = mmio32(p, op + 8U, &page_size_reg); ++reads;
    if (EFI_ERROR(s) || !page_size_reg) {
        if (!EFI_ERROR(s)) s = EFI_UNSUPPORTED;
        remember_fail(u"CAPS", u"PAGESIZE", s);
        goto out;
    }
    db &= ~3U;
    rt &= ~0x1fU;
    ir = rt + 0x20U;
    max_slots = hcs1 & 0xffU;
    scratchpads = (((hcs2 >> 27) & 0x1fU) << 5) | ((hcs2 >> 21) & 0x1fU);
    ac64 = (hcc & 1U) != 0U;
    while (shift < 32U && !(page_size_reg & (1U << shift))) ++shift;
    if (shift >= 32U) { s = EFI_UNSUPPORTED; remember_fail(u"CAPS", u"PAGE BIT", s); goto out; }
    xhci_page = (UINTN)1U << (12U + shift);
    if (xhci_page > 0x100000U || xhci_page < 4096U) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"CAPS", u"PAGE SIZE", s);
        goto out;
    }
    scratch_pages = xhci_page / 4096U;
    if (!max_slots || scratchpads > MAX_SCRATCHPADS) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"CAPS", u"LIMITS", s);
        goto out;
    }
    Print(u"CAPS: SLOTS=%u SCRATCHPADS=%u PAGE=%u AC64=%u CTXSZ=%u\r\n",
          max_slots, scratchpads, (UINT32)xhci_page, ac64 ? 1U : 0U,
          ((hcc >> 2) & 1U) ? 64U : 32U);

    s = mmio32(p, op + 4U, &status); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"USBSTS", s); goto out; }
    s = mmio32(p, op, &cmd); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"USBCMD", s); goto out; }
    if (!(status & STS_HCH)) {
        s = mmio_write32(p, op, cmd & ~(CMD_RUN | CMD_INTE | CMD_HSEE)); ++writes;
        if (EFI_ERROR(s)) { remember_fail(u"HALT", u"STOP", s); goto out; }
        s = wait_hch(p, op, TRUE, 10000U, &status, &reads);
        if (EFI_ERROR(s)) { remember_fail(u"HALT", u"HCH", s); goto out; }
    }
    halted = TRUE;
    s = reset_xhci(p, op, &cmd, &status, &reads, &writes);
    if (EFI_ERROR(s)) { remember_fail(u"RESET", u"RESET/CNR", s); goto out; }
    if (!(status & STS_HCH)) {
        s = EFI_DEVICE_ERROR;
        remember_fail(u"RESET", u"VERIFY HALTED", s);
        goto out;
    }

    s = dma_alloc(p, 1, &dcbaa_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"DCBAA", s); goto out; }
    s = dma_alloc(p, 1, &scratch_array_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCH ARRAY", s); goto out; }
    s = dma_alloc(p, 1, &cmd_ring_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"COMMAND RING", s); goto out; }
    s = dma_alloc(p, 1, &event_ring_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"EVENT RING", s); goto out; }
    s = dma_alloc(p, 1, &erst_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"ERST", s); goto out; }
    if (scratchpads) {
        s = uefi_call_wrapper(BS->AllocatePool, 3, EfiBootServicesData,
                              scratchpads * sizeof(DMA_OBJ), (VOID **)&scratch);
        if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCH DESCRIPTORS", s); goto out; }
        uefi_call_wrapper(BS->SetMem, 3, scratch,
                          scratchpads * sizeof(DMA_OBJ), 0);
        for (UINT32 i = 0; i < scratchpads; ++i) {
            s = dma_alloc(p, scratch_pages, &scratch[i]);
            if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCHPAD", s); goto out; }
            if (scratch[i].dev & ((UINT64)xhci_page - 1ULL)) {
                s = EFI_BAD_BUFFER_SIZE;
                remember_fail(u"DMA", u"SCRATCH ALIGN", s);
                goto out;
            }
            s = check_dma_addr(&scratch[i], ac64);
            if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCH ADDRESS", s); goto out; }
        }
    }

    if (EFI_ERROR(check_dma_addr(&dcbaa_d, ac64)) ||
        EFI_ERROR(check_dma_addr(&scratch_array_d, ac64)) ||
        EFI_ERROR(check_dma_addr(&cmd_ring_d, ac64)) ||
        EFI_ERROR(check_dma_addr(&event_ring_d, ac64)) ||
        EFI_ERROR(check_dma_addr(&erst_d, ac64))) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"DMA", u"32-BIT ADDRESSING", s);
        goto out;
    }

    dcbaa = (UINT64 *)dcbaa_d.host;
    scratch_array = (UINT64 *)scratch_array_d.host;
    cr = (UINT32 *)cmd_ring_d.host;
    erst = (UINT64 *)erst_d.host;
    dcbaa[0] = scratchpads ? scratch_array_d.dev : 0;
    for (UINT32 i = 0; i < scratchpads; ++i) scratch_array[i] = scratch[i].dev;

    cr[0] = 0;
    cr[1] = 0;
    cr[2] = 0;
    cr[3] = TRB_CYCLE | (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT);
    cr[address_cmd_offset + 0] = 0;
    cr[address_cmd_offset + 1] = 0;
    cr[address_cmd_offset + 2] = 0;
    cr[address_cmd_offset + 3] = TRB_CYCLE | (TRB_ADDRESS_DEVICE << TRB_TYPE_SHIFT);
    cr[(CMD_TRBS - 1U) * 4U + 0U] = (UINT32)cmd_ring_d.dev;
    cr[(CMD_TRBS - 1U) * 4U + 1U] = (UINT32)(cmd_ring_d.dev >> 32);
    cr[(CMD_TRBS - 1U) * 4U + 2U] = 0;
    cr[(CMD_TRBS - 1U) * 4U + 3U] = TRB_CYCLE | TRB_LINK_TOGGLE |
                                     (TRB_LINK << TRB_TYPE_SHIFT);
    erst[0] = event_ring_d.dev;
    erst[1] = 0;
    erst[2] = EVENT_TRBS;
    erst[3] = 0;

    s = mmio_write64(p, op + 0x30U, dcbaa_d.dev); writes += 2;
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"DCBAAP", s); goto out; }
    s = mmio_write32(p, op + 0x38U, 1U); ++writes;
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"CONFIG", s); goto out; }
    s = mmio_write64(p, op + 0x18U, cmd_ring_d.dev | CRCR_RCS); writes += 2;
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"CRCR", s); goto out; }
    s = mmio_write32(p, ir + 8U, 1U); ++writes;
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"ERSTSZ", s); goto out; }
    s = mmio_write64(p, ir + 0x10U, erst_d.dev & ERST_ADDR_MASK); writes += 2;
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"ERSTBA", s); goto out; }
    s = mmio_write64(p, ir + 0x18U, event_ring_d.dev & ERDP_ADDR_MASK); writes += 2;
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"ERDP", s); goto out; }
    s = mmio32(p, ir, &iman); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"IMAN", s); goto out; }
    s = mmio_write32(p, ir, iman & ~IMAN_IE); ++writes;
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"DISABLE IRQ", s); goto out; }

    s = mmio32(p, op, &cmd); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"USBCMD", s); goto out; }
    s = mmio_write32(p, op, (cmd & ~(CMD_INTE | CMD_HSEE)) | CMD_RUN); ++writes;
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"START", s); goto out; }
    halted = FALSE;
    running = TRUE;
    s = wait_hch(p, op, FALSE, 10000U, &status, &reads);
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"HCH CLEAR", s); goto out; }

    /* Gate 6 port selection and protocol matching use only the handoff port. */
    {
        UINT32 portsc_off = op + 0x400U + ((UINT32)handoff.keyboard.root_port - 1U) * 0x10U;
        s = mmio32(p, portsc_off, &portsc); ++reads;
        if (EFI_ERROR(s)) { remember_fail(u"PORT", u"PORTSC READ", s); goto out; }
        if (!(portsc & PORTSC_CCS)) {
            s = EFI_NOT_FOUND;
            remember_fail(u"PORT", u"NOT CONNECTED", s);
            goto out;
        }
        port_speed = (UINT8)((portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT);
        if (port_speed != USB_SPEED_LOW && port_speed != USB_SPEED_FULL) {
            s = EFI_UNSUPPORTED;
            remember_fail(u"PORT", u"SPEED LS/FS ONLY", s);
            goto out;
        }
        s = find_supported_protocol(p, hcc, handoff.keyboard.root_port,
                                    &protocol_cap, &protocol_major,
                                    &protocol_minor, &slot_type, &reads);
        if (EFI_ERROR(s)) { remember_fail(u"PORT", u"SUPPORTED PROTOCOL", s); goto out; }
        Print(u"PORT=%u CONNECTED SPEED=%u PROTOCOL=%u.%u SLOT-TYPE=%u\r\n",
              handoff.keyboard.root_port, port_speed,
              protocol_major, protocol_minor, slot_type);

        /* Clear stale PRC without clearing unrelated W1C status bits. */
        if (portsc & PORTSC_PRC) {
            s = portsc_write(p, portsc_off, portsc, 0, PORTSC_PR,
                             PORTSC_PRC, &writes);
            if (EFI_ERROR(s)) { remember_fail(u"PORT RESET", u"CLEAR STALE PRC", s); goto out; }
            s = mmio32(p, portsc_off, &portsc); ++reads;
            if (EFI_ERROR(s)) { remember_fail(u"PORT RESET", u"REREAD", s); goto out; }
            if (portsc & PORTSC_PRC) {
                s = EFI_DEVICE_ERROR;
                remember_fail(u"PORT RESET", u"PRC NOT CLEARED", s);
                goto out;
            }
        }

        /* Assert reset while preserving ordinary writable PORTSC state. */
        s = portsc_write(p, portsc_off, portsc, PORTSC_PR, 0, 0, &writes);
        if (EFI_ERROR(s)) { remember_fail(u"PORT RESET", u"ASSERT PR", s); goto out; }
        Print(u"PORT RESET: ASSERTED / WAITING FOR PORT STATUS CHANGE\r\n");
        s = wait_port_reset_event(p, ir + 0x18U, &event_ring_d,
                                  handoff.keyboard.root_port, &event_index,
                                  &event_cycle, &reads, &writes);
        if (EFI_ERROR(s)) { remember_fail(u"PORT RESET", u"PORT CHANGE EVENT", s); goto out; }
        s = mmio32(p, portsc_off, &portsc); ++reads;
        if (EFI_ERROR(s)) { remember_fail(u"PORT RESET", u"POST RESET PORTSC", s); goto out; }
        port_speed = (UINT8)((portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT);
        if (!(portsc & PORTSC_CCS) || !(portsc & PORTSC_PED) ||
            (portsc & PORTSC_PR) || ((portsc & PORTSC_PLS_MASK) >> 5) != USB2_PLS_U0 ||
            (portsc & PORTSC_PRC) == 0U ||
            (port_speed != USB_SPEED_LOW && port_speed != USB_SPEED_FULL)) {
            s = EFI_DEVICE_ERROR;
            remember_fail(u"PORT RESET", u"POST RESET STATE", s);
            goto out;
        }
        Print(u"PORT RESET: PRC=1 PED=1 U0=1 LIVE-SPEED=%u PASS\r\n", port_speed);
    }

    /* V31 cumulative Enable Slot command: matching protocol Slot Type. */
    s = mmio_write32(p, db, 0U); ++writes;
    if (EFI_ERROR(s)) { remember_fail(u"ENABLE SLOT", u"DOORBELL 0", s); goto out; }
    submitted = TRUE;
    Print(u"ENABLE SLOT: COMMAND PTR=%016lx SLOT-TYPE=%u\r\n",
          cmd_ring_d.dev, slot_type);
    s = wait_command_completion(p, ir + 0x18U, &event_ring_d,
                                &event_index, &event_cycle, cmd_ring_d.dev,
                                (UINT8)max_slots, &slot_id, &reads, &writes);
    if (EFI_ERROR(s)) { remember_fail(u"ENABLE SLOT", u"COMPLETION", s); goto out; }
    Print(u"ENABLE SLOT: COMPLETION SUCCESS SLOT=%u PASS\r\n", slot_id);

    /* Fresh device state for the second command. */
    s = dma_alloc(p, 1, &input_ctx_d);
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS", u"INPUT CONTEXT", s); goto out; }
    s = dma_alloc(p, 1, &output_ctx_d);
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS", u"OUTPUT CONTEXT", s); goto out; }
    s = dma_alloc(p, 1, &ep0_ring_d);
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS", u"EP0 RING", s); goto out; }
    if (EFI_ERROR(check_dma_addr(&input_ctx_d, ac64)) ||
        EFI_ERROR(check_dma_addr(&output_ctx_d, ac64)) ||
        EFI_ERROR(check_dma_addr(&ep0_ring_d, ac64))) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"ADDRESS", u"DMA ADDRESSING", s);
        goto out;
    }

    {
        UINTN stride = ((hcc >> 2) & 1U) ? 64U : 32U;
        UINT32 *icc = input;
        UINT32 *slot = (UINT32 *)((UINT8 *)input_ctx_d.host + stride);
        UINT32 *ep0 = (UINT32 *)((UINT8 *)input_ctx_d.host + stride * 2U);
        UINT32 *out_slot = (UINT32 *)output_ctx_d.host;

        input = (UINT32 *)input_ctx_d.host;
        output = (UINT32 *)output_ctx_d.host;
        ep0_ring = (UINT32 *)ep0_ring_d.host;
        icc = input;
        slot = (UINT32 *)((UINT8 *)input_ctx_d.host + stride);
        ep0 = (UINT32 *)((UINT8 *)input_ctx_d.host + stride * 2U);
        out_slot = output;
        uefi_call_wrapper(BS->SetMem, 3, input_ctx_d.host, 4096U, 0);
        uefi_call_wrapper(BS->SetMem, 3, output_ctx_d.host, 4096U, 0);
        uefi_call_wrapper(BS->SetMem, 3, ep0_ring_d.host, 4096U, 0);

        /* Input Control Context: Add Slot + EP0, no Drop Context. */
        icc[0] = 0U;
        icc[1] = (1U << 0) | (1U << 1);

        /* Input Slot Context: direct root-port device, one endpoint context. */
        slot[0] = (1U << 27) | ((UINT32)port_speed << 20);
        slot[1] = ((UINT32)handoff.keyboard.root_port << 16);
        slot[2] = 0U;
        slot[3] = 0U;

        /* Input EP0 Context: control EP, MPS=8, average TRB length=8. */
        ep0[0] = 0U;
        ep0[1] = (3U << 1) | (4U << 3) | (8U << 16);
        ep0[2] = (UINT32)ep0_ring_d.dev | 1U;
        ep0[3] = (UINT32)(ep0_ring_d.dev >> 32);
        ep0[4] = 8U;

        /* Fresh EP0 transfer ring with a self-contained Link TRB. */
        ep0_ring[(4096U / 4U) - 4U] = (UINT32)ep0_ring_d.dev;
        ep0_ring[(4096U / 4U) - 3U] = (UINT32)(ep0_ring_d.dev >> 32);
        ep0_ring[(4096U / 4U) - 2U] = 0U;
        ep0_ring[(4096U / 4U) - 1U] = TRB_CYCLE | TRB_LINK_TOGGLE |
                                       (TRB_LINK << TRB_TYPE_SHIFT);

        dcbaa[slot_id] = output_ctx_d.dev;
        (void)out_slot;
    }

    /* Address Device command occupies TRB 1 in the existing V31 command ring. */
    cr[address_cmd_offset + 0U] = (UINT32)input_ctx_d.dev;
    cr[address_cmd_offset + 1U] = (UINT32)(input_ctx_d.dev >> 32);
    cr[address_cmd_offset + 2U] = 0U;
    cr[address_cmd_offset + 3U] = TRB_CYCLE |
                                   (TRB_ADDRESS_DEVICE << TRB_TYPE_SHIFT) |
                                   ((UINT32)slot_id << 24);

    s = mmio_write32(p, db + ((UINT32)slot_id * 4U), 0U); ++writes;
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS", u"SLOT DOORBELL", s); goto out; }
    address_submitted = TRUE;
    Print(u"ADDRESS DEVICE: INPUT=%016lx EP0-MPS=8 AVG-TRB=8 DOORBELL=%u\r\n",
          input_ctx_d.dev, slot_id);

    s = wait_command_completion(p, ir + 0x18U, &event_ring_d,
                                &event_index, &event_cycle,
                                cmd_ring_d.dev + 16U, slot_id, &slot_id,
                                &reads, &writes);
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS", u"COMPLETION", s); goto out; }

    {
        UINTN stride = ((hcc >> 2) & 1U) ? 64U : 32U;
        UINT32 *out_slot = (UINT32 *)((UINT8 *)output_ctx_d.host);
        UINT32 state = (out_slot[stride / 4U * 3U] >> 27) & 0x1fU;
        UINT32 addr = out_slot[stride / 4U * 3U] & 0xffU;
        if (state != 2U || addr == 0U) {
            s = EFI_DEVICE_ERROR;
            remember_fail(u"ADDRESS", u"OUTPUT ADDRESSED STATE", s);
            goto out;
        }
        Print(u"ADDRESS DEVICE: COMPLETION SUCCESS STATE=ADDRESSED(%u) USB-ADDR=%u PASS\r\n",
              state, addr);
    }

    /* Required cumulative teardown before releasing any DMA mappings. */
    s = mmio32(p, op, &cmd); ++reads;
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"USBCMD", s); goto out; }
    s = mmio_write32(p, op, cmd & ~(CMD_RUN | CMD_INTE | CMD_HSEE)); ++writes;
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"STOP", s); goto out; }
    running = FALSE;
    s = wait_hch(p, op, TRUE, 10000U, &status, &reads);
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"CONFIRM HCH", s); fatal_running(); }
    halted = TRUE;

    s = reset_xhci(p, op, &cmd, &status, &reads, &writes);
    if (EFI_ERROR(s)) { remember_fail(u"RESET", u"RECOVERY", s); goto out; }
    if (!(status & STS_HCH)) { s = EFI_DEVICE_ERROR; remember_fail(u"RESET", u"RECOVERY HALTED", s); goto out; }

    s = clear_controller_refs(p, op, ir, &writes);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"CLEAR POINTERS", s); fatal_running(); }
    controller_refs_cleared = TRUE;

out:
    if (EFI_ERROR(s) && !halted && (submitted || address_submitted || running))
        fatal_running();

    /* If the controller was safely halted, complete pointer teardown before DMA release. */
    if (halted && p && !controller_refs_cleared) {
        EFI_STATUS cs = clear_controller_refs(p, op, ir, &writes);
        if (EFI_ERROR(cs))
            fatal_running();
        controller_refs_cleared = TRUE;
    }

    if (halted) {
        if (scratch) {
            for (UINT32 i = 0; i < scratchpads; ++i) {
                ts = dma_free(p, &scratch[i]);
                if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
            }
            uefi_call_wrapper(BS->FreePool, 1, scratch);
            scratch = NULL;
        }
        ts = dma_free(p, &ep0_ring_d); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &output_ctx_d); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &input_ctx_d); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &erst_d); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &event_ring_d); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &cmd_ring_d); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &scratch_array_d); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
        ts = dma_free(p, &dcbaa_d); if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
    }

    if (p && disconnected && attrs_changed) {
        ts = uefi_call_wrapper(p->Attributes, 4, p,
                               EfiPciIoAttributeOperationDisable,
                               changed_attrs, NULL);
        if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
    }

    if (EFI_ERROR(s)) {
        Print(u"\r\nV32 GATE 6: FAIL RESULT=%r\r\n", s);
        Print(u"FAIL STAGE=%s OP=%s STATUS=%r\r\n",
              fail_stage, fail_op, fail_status);
    } else {
        Print(u"\r\nV32 GATE 6: PASS\r\n");
        Print(u"UEFI DISCOVERY=1 QUIESCE=1 PORT RESET=1 ENABLE SLOT=1 ADDRESS DEVICE=1\r\n");
        Print(u"COMMANDS=2 DOORBELLS=2 CPU-INTERRUPTS=0 / LS+FS ONLY\r\n");
        Print(u"INITIAL EP0 MPS=8 / POST-ADDRESS SLOT STATE=ADDRESSED\r\n");
        Print(u"RESET RECOVERY + CONTROLLER POINTER CLEAR + DMA RELEASE=PASS\r\n");
    }
    Print(u"MMIO READS=%u WRITES=%u RESULT=%r\r\n", reads, writes, s);
    press_a_key(st);
    return s;
}
