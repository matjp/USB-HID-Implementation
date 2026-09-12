/* Version 32 — cumulative Gate 6 implementation */

#define efi_main v31_unused_efi_main
#include "xhci_mmio_v31.c"
#undef efi_main

#include "usb_io_compat.h"

#define V32_TRB_ADDRESS_DEVICE      11U
#define V32_TRB_PORT_STATUS_CHANGE  34U
#define V32_TRB_COMMAND_COMPLETION  33U
#define V32_PORTSC_CCS              0x00000001U
#define V32_PORTSC_PED              0x00000002U
#define V32_PORTSC_PR               0x00000010U
#define V32_PORTSC_PLS_MASK         0x000001e0U
#define V32_PORTSC_PRC              0x00200000U
#define V32_PORTSC_SPEED_MASK       0x00003c00U
#define V32_PORTSC_SPEED_SHIFT      10U
#define V32_PORTSC_PIC_MASK         0x0000c000U
#define V32_PORTSC_LWS              0x00010000U
#define V32_PORTSC_PP               0x00000200U
#define V32_PORTSC_WCE              0x02000000U
#define V32_PORTSC_WDE              0x04000000U
#define V32_PORTSC_WOE              0x08000000U
#define V32_PORTSC_RW_MASK          (V32_PORTSC_PED | V32_PORTSC_PR | \
                                     V32_PORTSC_PLS_MASK | V32_PORTSC_PP | \
                                     V32_PORTSC_PIC_MASK | V32_PORTSC_LWS | \
                                     V32_PORTSC_WCE | V32_PORTSC_WDE | V32_PORTSC_WOE)
#define V32_USB_SPEED_FULL          1U
#define V32_USB_SPEED_LOW           2U
#define V32_HANDOFF_MAGIC           0x48494458U
#define V32_HANDOFF_VERSION         2U
#define V32_MAX_ENDPOINTS           8U
#define V32_MAX_PATH_BYTES          512U

typedef struct {
    UINT8  endpoint_address;
    UINT8  attributes;
    UINT16 max_packet_size;
    UINT8  interval;
} V32_ENDPOINT;

typedef struct {
    UINT16 size;
    UINT16 reserved;
    UINT8  data[V32_MAX_PATH_BYTES];
} V32_DEVICE_PATH;

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
    V32_ENDPOINT endpoints[V32_MAX_ENDPOINTS];
    V32_DEVICE_PATH device_path;
} V32_HID_DEVICE;

typedef V32_HID_DEVICE V32_KEYBOARD;

typedef struct {
    UINT32 magic;
    UINT16 version;
    UINT16 size;
    UINT16 device_count;
    UINT16 pci_segment;
    UINT8  pci_bus;
    UINT8  pci_device;
    UINT8  pci_function;
    UINT8  reserved0;
    UINT16 pci_vendor;
    UINT16 pci_device_id;
    V32_DEVICE_PATH controller_path;
    V32_HID_DEVICE keyboard;
    V32_HID_DEVICE mouse;
} V32_HANDOFF;

static EFI_GUID V32_UsbIoGuid = EFI_USB_IO_PROTOCOL_GUID;

static void v32_fail(const CHAR16 *stage, const CHAR16 *op, EFI_STATUS s)
{
    fail(stage, op, s);
}

static UINT64 v32_disable_cpu_interrupts(void)
{
    UINT64 flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void v32_restore_cpu_interrupts(UINT64 flags)
{
    if (flags & (1ULL << 9))
        __asm__ __volatile__("sti" ::: "memory");
}

static BOOLEAN v32_is_hid(const EFI_USB_INTERFACE_DESCRIPTOR *d, UINT8 *kind)
{
    if (d->InterfaceClass != 0x03U || d->InterfaceSubClass != 0x01U)
        return FALSE;
    if (d->InterfaceProtocol == 0x01U) {
        *kind = 1U;
        return TRUE;
    }
    if (d->InterfaceProtocol == 0x02U) {
        *kind = 2U;
        return TRUE;
    }
    return FALSE;
}

static UINT8 v32_root_port(EFI_DEVICE_PATH_PROTOCOL *path)
{
    UINT8 *p = (UINT8 *)path;
    UINT8 root = 0xffU;
    while (p) {
        EFI_DEVICE_PATH_PROTOCOL *h = (EFI_DEVICE_PATH_PROTOCOL *)p;
        UINT16 len = (UINT16)h->Length[0] | ((UINT16)h->Length[1] << 8);
        if (len < sizeof(EFI_DEVICE_PATH_PROTOCOL) || h->Type == 0x7fU)
            break;
        if (h->Type == 0x03U && h->SubType == 0x05U && len >= 6U)
            root = p[4];
        p += len;
    }
    return root;
}

static EFI_STATUS v32_path_size(EFI_DEVICE_PATH_PROTOCOL *path, UINTN *size)
{
    UINT8 *p = (UINT8 *)path;
    UINTN total = 0;
    if (!path || !size) return EFI_INVALID_PARAMETER;
    while (total <= V32_MAX_PATH_BYTES - sizeof(EFI_DEVICE_PATH_PROTOCOL)) {
        EFI_DEVICE_PATH_PROTOCOL *h = (EFI_DEVICE_PATH_PROTOCOL *)p;
        UINT16 len = (UINT16)h->Length[0] | ((UINT16)h->Length[1] << 8);
        if (len < sizeof(EFI_DEVICE_PATH_PROTOCOL)) return EFI_DEVICE_ERROR;
        if (total + len > V32_MAX_PATH_BYTES) return EFI_BAD_BUFFER_SIZE;
        total += len;
        if (h->Type == 0x7fU) {
            *size = total;
            return EFI_SUCCESS;
        }
        p += len;
    }
    return EFI_BAD_BUFFER_SIZE;
}

static EFI_STATUS v32_copy_path(V32_DEVICE_PATH *dst, EFI_DEVICE_PATH_PROTOCOL *path)
{
    UINTN size = 0;
    EFI_STATUS s;
    if (!dst || !path) return EFI_INVALID_PARAMETER;
    s = v32_path_size(path, &size);
    if (EFI_ERROR(s)) return s;
    uefi_call_wrapper(BS->SetMem, 3, dst, sizeof(*dst), 0);
    CopyMem(dst->data, path, size);
    dst->size = (UINT16)size;
    return EFI_SUCCESS;
}

static BOOLEAN v32_path_equals(const V32_DEVICE_PATH *expected,
                               EFI_DEVICE_PATH_PROTOCOL *actual)
{
    UINTN size = 0;
    if (!expected || !expected->size || expected->size > V32_MAX_PATH_BYTES || !actual)
        return FALSE;
    if (EFI_ERROR(v32_path_size(actual, &size)) || size != expected->size)
        return FALSE;
    return CompareMem(expected->data, actual, size) == 0;
}

static BOOLEAN v32_pci_path_prefix(EFI_DEVICE_PATH_PROTOCOL *pci_path,
                                   EFI_DEVICE_PATH_PROTOCOL *usb_path,
                                   UINTN *matched)
{
    UINT8 *p = (UINT8 *)pci_path;
    UINT8 *q = (UINT8 *)usb_path;
    UINTN total = 0;
    if (!p || !q) return FALSE;
    for (;;) {
        EFI_DEVICE_PATH_PROTOCOL *a = (EFI_DEVICE_PATH_PROTOCOL *)p;
        EFI_DEVICE_PATH_PROTOCOL *b = (EFI_DEVICE_PATH_PROTOCOL *)q;
        UINT16 alen = (UINT16)a->Length[0] | ((UINT16)a->Length[1] << 8);
        UINT16 blen = (UINT16)b->Length[0] | ((UINT16)b->Length[1] << 8);
        if (alen < sizeof(EFI_DEVICE_PATH_PROTOCOL) ||
            blen < sizeof(EFI_DEVICE_PATH_PROTOCOL))
            return FALSE;
        if (a->Type == 0x7fU) {
            if (matched) *matched = total;
            return TRUE;
        }
        if (b->Type == 0x7fU || alen != blen || CompareMem(a, b, alen) != 0)
            return FALSE;
        total += alen;
        p += alen;
        q += blen;
    }
}

/* UEFI discovery producer: identify the PCI controller whose complete device
 * path is the longest prefix of the selected USB interface path. */
static EFI_STATUS v32_find_controller(EFI_DEVICE_PATH_PROTOCOL *usb_path,
                                       EFI_HANDLE *controller)
{
    EFI_HANDLE *pci_handles = NULL;
    UINTN pci_count = 0, i, best_len = 0;
    EFI_HANDLE best = NULL;
    EFI_STATUS s;

    if (!usb_path || !controller) return EFI_INVALID_PARAMETER;
    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &pci_count, &pci_handles);
    if (EFI_ERROR(s)) return s;

    for (i = 0; i < pci_count; ++i) {
        EFI_DEVICE_PATH_PROTOCOL *pci_path = NULL;
        UINTN matched = 0;
        s = uefi_call_wrapper(BS->OpenProtocol, 6, pci_handles[i],
                              &DevicePathProtocol, (VOID **)&pci_path,
                              NULL, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s) || !pci_path) continue;
        if (v32_pci_path_prefix(pci_path, usb_path, &matched) && matched > best_len) {
            best_len = matched;
            best = pci_handles[i];
        }
    }
    FreePool(pci_handles);
    if (!best) return EFI_NOT_FOUND;
    *controller = best;
    return EFI_SUCCESS;
}

static EFI_STATUS v32_validate_controller_path(EFI_HANDLE controller,
                                               const V32_DEVICE_PATH *expected)
{
    EFI_DEVICE_PATH_PROTOCOL *actual = NULL;
    EFI_STATUS s;
    s = uefi_call_wrapper(BS->OpenProtocol, 6, controller,
                          &DevicePathProtocol, (VOID **)&actual,
                          NULL, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(s)) return s;
    return v32_path_equals(expected, actual) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static EFI_STATUS v32_pci_match(EFI_HANDLE controller,
                                UINT16 segment, UINT8 bus,
                                UINT8 device, UINT8 function,
                                UINT16 vendor, UINT16 device_id,
                                EFI_PCI_IO_PROTOCOL **out)
{
    EFI_PCI_IO_PROTOCOL *p = NULL;
    UINTN sg = 0, b = 0, d = 0, f = 0;
    UINT32 id = 0, cls = 0;
    EFI_STATUS s;
    s = uefi_call_wrapper(BS->OpenProtocol, 6, controller, &PciGuid,
                          (VOID **)&p, NULL, NULL,
                          EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(s)) return s;
    s = uefi_call_wrapper(p->GetLocation, 5, p, &sg, &b, &d, &f);
    if (EFI_ERROR(s) || sg != segment || b != bus || d != device || f != function)
        return EFI_DEVICE_ERROR;
    s = cfg32(p, 0x00, &id);
    if (EFI_ERROR(s) || (UINT16)(id & 0xffffU) != vendor ||
        (UINT16)(id >> 16) != device_id)
        return EFI_DEVICE_ERROR;
    s = cfg32(p, 0x08, &cls);
    if (EFI_ERROR(s) || ((cls >> 24) & 0xffU) != 0x0cU ||
        ((cls >> 16) & 0xffU) != 0x03U ||
        ((cls >> 8) & 0xffU) != 0x30U)
        return EFI_UNSUPPORTED;
    *out = p;
    return EFI_SUCCESS;
}

static EFI_STATUS v32_fill_hid(EFI_USB_IO_PROTOCOL *usb,
                               EFI_USB_INTERFACE_DESCRIPTOR *iface,
                               EFI_DEVICE_PATH_PROTOCOL *path,
                               UINT8 kind, V32_HID_DEVICE *d)
{
    EFI_USB_DEVICE_DESCRIPTOR dd;
    EFI_USB_CONFIG_DESCRIPTOR cd;
    EFI_USB_ENDPOINT_DESCRIPTOR ep;
    UINTN e, eps;
    BOOLEAN got_in = FALSE;
    EFI_STATUS s;

    uefi_call_wrapper(BS->SetMem, 3, d, sizeof(*d), 0);
    d->kind = kind;
    d->root_port = v32_root_port(path);
    if (d->root_port == 0xffU) return EFI_NOT_FOUND;
    d->interface_number = iface->InterfaceNumber;
    d->interface_protocol = iface->InterfaceProtocol;
    d->endpoint_count = iface->NumEndpoints;
    if (d->endpoint_count > V32_MAX_ENDPOINTS)
        d->endpoint_count = V32_MAX_ENDPOINTS;
    s = v32_copy_path(&d->device_path, path);
    if (EFI_ERROR(s)) return s;

    if (!EFI_ERROR(uefi_call_wrapper(usb->UsbGetDeviceDescriptor, 3, usb, &dd))) {
        d->vendor_id = dd.IdVendor;
        d->product_id = dd.IdProduct;
        d->bcd_usb = dd.BcdUSB;
        d->ep0_mps_descriptor = dd.MaxPacketSize0;
    }
    if (!EFI_ERROR(uefi_call_wrapper(usb->UsbGetConfigDescriptor, 3, usb, &cd)))
        d->configuration_value = cd.ConfigurationValue;

    eps = d->endpoint_count;
    for (e = 0; e < eps; ++e) {
        if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetEndpointDescriptor, 4,
                                         usb, (UINT8)e, &ep)))
            continue;
        d->endpoints[e].endpoint_address = ep.EndpointAddress;
        d->endpoints[e].attributes = ep.Attributes;
        d->endpoints[e].max_packet_size = ep.MaxPacketSize;
        d->endpoints[e].interval = ep.Interval;
        if (!got_in && ep.DescriptorType == 0x05U && ep.Length >= 7U &&
            (ep.EndpointAddress & 0x80U) &&
            (ep.Attributes & 0x03U) == 0x03U &&
            ep.MaxPacketSize != 0U && ep.Interval != 0U) {
            d->interrupt_in_endpoint = ep.EndpointAddress;
            d->interrupt_max_packet_size = ep.MaxPacketSize;
            d->interval = ep.Interval;
            got_in = TRUE;
        }
    }
    return got_in ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static EFI_STATUS v32_produce_handoff(EFI_HANDLE image, V32_HANDOFF *h,
                                      EFI_HANDLE *controller)
{
    EFI_HANDLE *usb_handles = NULL;
    UINTN count = 0, i, keyboards = 0, mice = 0;
    EFI_STATUS s = LibLocateHandle(ByProtocol, &V32_UsbIoGuid, NULL,
                                   &count, &usb_handles);
    if (EFI_ERROR(s)) return EFI_NOT_FOUND;

    for (i = 0; i < count; ++i) {
        EFI_USB_IO_PROTOCOL *usb = NULL;
        EFI_USB_INTERFACE_DESCRIPTOR iface;
        EFI_DEVICE_PATH_PROTOCOL *path = NULL;
        EFI_HANDLE ch = NULL;
        UINT8 kind;

        if (EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol, 6, usb_handles[i],
                                         &V32_UsbIoGuid, (VOID **)&usb,
                                         image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL)))
            continue;
        if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetInterfaceDescriptor, 3,
                                         usb, &iface)) || !v32_is_hid(&iface, &kind))
            continue;

        s = uefi_call_wrapper(BS->OpenProtocol, 6, usb_handles[i],
                              &DevicePathProtocol, (VOID **)&path,
                              image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s) || !path) {
            v32_fail(u"DISCOVERY", u"DEVICE PATH", s);
            FreePool(usb_handles);
            return EFI_NOT_FOUND;
        }

        s = v32_find_controller(path, &ch);
        if (EFI_ERROR(s) || !ch) {
            v32_fail(u"DISCOVERY", u"PCI CONTROLLER", s);
            FreePool(usb_handles);
            return s;
        }

        if (!*controller) {
            EFI_PCI_IO_PROTOCOL *pci = NULL;
            EFI_DEVICE_PATH_PROTOCOL *cpath = NULL;
            UINTN sg = 0, b = 0, d = 0, f = 0;
            UINT32 id = 0, cls = 0;
            s = uefi_call_wrapper(BS->OpenProtocol, 6, ch, &PciGuid,
                                  (VOID **)&pci, image, NULL,
                                  EFI_OPEN_PROTOCOL_GET_PROTOCOL);
            if (EFI_ERROR(s)) { FreePool(usb_handles); return s; }
            s = uefi_call_wrapper(pci->GetLocation, 5, pci, &sg, &b, &d, &f);
            if (EFI_ERROR(s) || sg > 0xffffU || b > 0xffU || d > 0x1fU || f > 7U) {
                s = EFI_DEVICE_ERROR;
                v32_fail(u"DISCOVERY", u"PCI LOCATION", s);
                FreePool(usb_handles);
                return s;
            }
            s = cfg32(pci, 0x00, &id);
            if (EFI_ERROR(s)) { FreePool(usb_handles); return s; }
            s = cfg32(pci, 0x08, &cls);
            if (EFI_ERROR(s) || ((cls >> 24) & 0xffU) != 0x0cU ||
                ((cls >> 16) & 0xffU) != 0x03U ||
                ((cls >> 8) & 0xffU) != 0x30U) {
                s = EFI_UNSUPPORTED;
                v32_fail(u"DISCOVERY", u"XHCI CLASS", s);
                FreePool(usb_handles);
                return s;
            }
            s = uefi_call_wrapper(BS->OpenProtocol, 6, ch,
                                  &DevicePathProtocol, (VOID **)&cpath,
                                  image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
            if (EFI_ERROR(s) || !cpath) {
                v32_fail(u"DISCOVERY", u"CONTROLLER PATH", s);
                FreePool(usb_handles);
                return EFI_NOT_FOUND;
            }
            s = v32_copy_path(&h->controller_path, cpath);
            if (EFI_ERROR(s)) {
                v32_fail(u"DISCOVERY", u"CONTROLLER PATH COPY", s);
                FreePool(usb_handles);
                return s;
            }
            h->pci_segment = (UINT16)sg;
            h->pci_bus = (UINT8)b;
            h->pci_device = (UINT8)d;
            h->pci_function = (UINT8)f;
            h->pci_vendor = (UINT16)(id & 0xffffU);
            h->pci_device_id = (UINT16)(id >> 16);
            *controller = ch;
        } else if (ch != *controller) {
            s = EFI_UNSUPPORTED;
            v32_fail(u"DISCOVERY", u"MULTIPLE XHCI CONTROLLERS", s);
            FreePool(usb_handles);
            return s;
        }

        if (kind == 1U) {
            ++keyboards;
            if (keyboards != 1U) {
                s = EFI_ALREADY_STARTED;
                v32_fail(u"DISCOVERY", u"MULTIPLE KEYBOARDS", s);
                FreePool(usb_handles);
                return s;
            }
            s = v32_fill_hid(usb, &iface, path, kind, &h->keyboard);
        } else {
            ++mice;
            if (mice != 1U) {
                s = EFI_ALREADY_STARTED;
                v32_fail(u"DISCOVERY", u"MULTIPLE MICE", s);
                FreePool(usb_handles);
                return s;
            }
            s = v32_fill_hid(usb, &iface, path, kind, &h->mouse);
        }
        if (EFI_ERROR(s)) {
            v32_fail(u"DISCOVERY", kind == 1U ? u"KEYBOARD ENDPOINT" : u"MOUSE ENDPOINT", s);
            FreePool(usb_handles);
            return s;
        }
    }

    h->device_count = (UINT16)(keyboards + mice);
    if (keyboards != 1U || h->device_count == 0U || h->device_count > 2U) {
        s = keyboards ? EFI_DEVICE_ERROR : EFI_NOT_FOUND;
        v32_fail(u"DISCOVERY", u"SELECTED HID SET", s);
        FreePool(usb_handles);
        return s;
    }
    FreePool(usb_handles);
    return EFI_SUCCESS;
}

static EFI_STATUS v32_supported_protocol(EFI_PCI_IO_PROTOCOL *p, UINT32 hcc,
                                         UINT8 port, UINT8 *major,
                                         UINT8 *minor, UINT8 *slot_type,
                                         UINT32 *reads)
{
    UINT32 off = ((hcc >> 16) & 0xffffU) * 4U;
    UINT32 hdr, ports, slot, next;
    UINT8 po, pc;
    UINTN n = 0;
    EFI_STATUS s;
    while (off && n++ < 256U) {
        s = mr32(p, off, &hdr); ++*reads;
        if (EFI_ERROR(s)) return s;
        if ((hdr & 0xffU) == 2U) {
            s = mr32(p, off + 8U, &ports); ++*reads;
            if (EFI_ERROR(s)) return s;
            po = (UINT8)(ports & 0xffU);
            pc = (UINT8)((ports >> 8) & 0xffU);
            if (pc && port >= po && (UINT8)(port - po) < pc) {
                s = mr32(p, off + 12U, &slot); ++*reads;
                if (EFI_ERROR(s)) return s;
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

static EFI_STATUS v32_portsc_write(EFI_PCI_IO_PROTOCOL *p, UINT32 off,
                                   UINT32 old, UINT32 set_bits,
                                   UINT32 clear_bits, UINT32 w1c,
                                   UINT32 *writes)
{
    UINT32 v = old & V32_PORTSC_RW_MASK;
    v &= ~clear_bits;
    v |= set_bits;
    v |= w1c;
    EFI_STATUS s = mw32(p, off, v);
    ++*writes;
    return s;
}

static EFI_STATUS v32_wait_event(EFI_PCI_IO_PROTOCOL *p, UINT32 erdp_off,
                                 struct dma_obj *event_ring, UINT8 type,
                                 UINT8 port, UINT64 trb_ptr, UINT8 slot_expected,
                                 UINT8 *slot_result, UINTN *event_index,
                                 UINT8 *cycle, UINT32 *reads, UINT32 *writes)
{
    UINTN loops;
    for (loops = 0; loops < 20000U; ++loops) {
        volatile UINT32 *e = (volatile UINT32 *)((UINT8 *)event_ring->host +
                                                  (*event_index) * 16U);
        UINT32 dw0 = e[0], dw1 = e[1], dw2 = e[2], dw3 = e[3];
        if ((dw3 & TRB_CYCLE) == *cycle) {
            UINT32 event_type = (dw3 & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
            UINT8 event_port = (UINT8)(dw0 >> 24);
            UINT8 event_slot = (UINT8)(dw3 >> 24);
            UINT32 cc = (dw2 >> 24) & 0xffU;
            UINT64 ptr = ((UINT64)dw1 << 32) | dw0;
            BOOLEAN match = event_type == type;
            ++*reads;
            if (type == V32_TRB_PORT_STATUS_CHANGE)
                match = match && event_port == port;
            else
                match = match && cc == CC_SUCCESS && ptr == trb_ptr &&
                        (slot_expected == 0U || event_slot == slot_expected);
            *event_index = *event_index + 1U;
            if (*event_index == EVENT_TRBS) {
                *event_index = 0;
                *cycle ^= 1U;
            }
            {
                UINT64 next = event_ring->dev + (*event_index) * 16U;
                EFI_STATUS s = mw64(p, erdp_off, next & ERDP_ADDR_MASK);
                *writes += 2U;
                if (EFI_ERROR(s)) return s;
            }
            if (match) {
                if (slot_result) *slot_result = event_slot;
                return EFI_SUCCESS;
            }
        }
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static BOOLEAN v32_dma_ok(const struct dma_obj *d, BOOLEAN ac64)
{
    return ac64 || d->dev <= 0xffffffffULL;
}

static EFI_STATUS v32_attrs(EFI_PCI_IO_PROTOCOL *p, UINT64 *changed)
{
    UINT64 supported = 0, attrs = 0;
    UINT64 need = EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
    EFI_STATUS s;
    *changed = 0;
    s = uefi_call_wrapper(p->Attributes, 4, p,
                           EfiPciIoAttributeOperationSupported, 0, &supported);
    if (EFI_ERROR(s) || (supported & need) != need) return EFI_UNSUPPORTED;
    s = uefi_call_wrapper(p->Attributes, 4, p,
                           EfiPciIoAttributeOperationGet, 0, &attrs);
    if (EFI_ERROR(s)) return s;
    if ((attrs & need) != need) {
        UINT64 enable = need & ~attrs;
        s = uefi_call_wrapper(p->Attributes, 4, p,
                               EfiPciIoAttributeOperationEnable, enable, NULL);
        if (EFI_ERROR(s)) return s;
        *changed = enable;
    }
    return EFI_SUCCESS;
}

static void v32_fatal_running(UINT32 *reads, UINT32 *writes)
{
    Print(u"\r\nFATAL: XHCI RUNNING STATE UNCERTAIN\r\n");
    Print(u"FAIL STAGE=%s OP=%s STATUS=%r\r\n", fail_stage, fail_op, fail_status);
    Print(u"MMIO READS=%u WRITES=%u\r\n", *reads, *writes);
    for (;;) uefi_call_wrapper(BS->Stall,1,1000000);
}

static void v32_press_key(EFI_SYSTEM_TABLE *st)
{
    EFI_INPUT_KEY key;
    Print(u"PRESS A KEY TO CONTINUE\r\n");
    for (;;) {
        if (st && st->ConIn &&
            !EFI_ERROR(uefi_call_wrapper(st->ConIn->ReadKeyStroke, 2,
                                          st->ConIn, &key))) return;
        uefi_call_wrapper(BS->Stall, 1, 10000);
    }
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS s = EFI_SUCCESS, ts;
    EFI_HANDLE controller = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    V32_HANDOFF h, b;
    UINT64 attrs_changed = 0;
    UINT32 bar0 = 0, bar1 = 0, cap = 0, hcs1 = 0, hcs2 = 0, hcc = 0;
    UINT32 op = 0, db = 0, rt = 0, ir = 0, cmd = 0, status = 0, iman = 0;
    UINT32 page_reg = 0, max_slots = 0, scratchpads = 0;
    UINT8 proto_major = 0, proto_minor = 0, slot_type = 0, speed = 0, slot_id = 0;
    UINT32 reads = 0, writes = 0;
    UINTN shift = 0, xpage = 0, scratch_pages = 0;
    UINTN event_index = 0;
    UINT8 event_cycle = 1U;
    BOOLEAN ac64 = FALSE, halted = FALSE, running = FALSE;
    BOOLEAN disconnected = FALSE, attrs_changed_here = FALSE, refs_cleared = FALSE;
    UINT64 saved_rflags = 0;
    BOOLEAN cpu_interrupts_disabled = FALSE;
    struct dma_obj dcbaa_d = {0}, spa_d = {0}, cr_d = {0}, ev_d = {0}, erst_d = {0};
    struct dma_obj input_d = {0}, output_d = {0}, ep0_d = {0};
    struct dma_obj *scratch = NULL;
    UINT64 *dcbaa = NULL, *spa = NULL, *erst = NULL;
    UINT32 *cr = NULL, *input = NULL, *ep0 = NULL;
    UINT32 portsc_off;
    UINT8 xhci_port = 0U;

    InitializeLib(image, st);
    uefi_call_wrapper(BS->SetMem, 3, &h, sizeof(h), 0);
    h.magic = V32_HANDOFF_MAGIC;
    h.version = V32_HANDOFF_VERSION;
    h.size = sizeof(h);

    Print(u"TOSHIBA xHCI V32 / GATE 6 / PORT RESET + ADDRESS DEVICE\r\n");
    Print(u"UEFI KEYBOARD + MOUSE -> QUIESCE -> FRESH xHCI / LS+FS ONLY\r\n");

    /* Phase A: UEFI discovery producer. Everything below consumes only the
       completed handoff and controller handle; it performs no USB discovery. */
    s = v32_produce_handoff(image, &h, &controller);
    if (EFI_ERROR(s)) goto out;
    b = h;
    Print(u"UEFI KEYBOARD: VID=%04x PID=%04x PORT=%u IF=%u EP=%02x EP0DESC=%u\r\n",
          b.keyboard.vendor_id, b.keyboard.product_id, b.keyboard.root_port,
          b.keyboard.interface_number, b.keyboard.interrupt_in_endpoint,
          b.keyboard.ep0_mps_descriptor);
    if (b.mouse.kind == 2U)
        Print(u"UEFI MOUSE: VID=%04x PID=%04x PORT=%u IF=%u EP=%02x\r\n",
              b.mouse.vendor_id, b.mouse.product_id, b.mouse.root_port,
              b.mouse.interface_number, b.mouse.interrupt_in_endpoint);
    Print(u"HANDOFF: MAGIC=%08x VERSION=%u SIZE=%u DEVICES=%u CONTROLLER-PATH=%u\r\n",
          b.magic, b.version, b.size, b.device_count, b.controller_path.size);
    Print(u"CONTROLLER: %04x:%02x:%02x.%x %04x:%04x\r\n",
          b.pci_segment, b.pci_bus, b.pci_device, b.pci_function,
          b.pci_vendor, b.pci_device_id);
    if (b.magic != V32_HANDOFF_MAGIC || b.version != V32_HANDOFF_VERSION ||
        b.size != sizeof(b) || b.device_count < 1U || b.device_count > 2U ||
        b.keyboard.kind != 1U ||
        (b.mouse.kind != 0U && b.mouse.kind != 2U) ||
        !b.controller_path.size || !b.keyboard.device_path.size) {
        s = EFI_INVALID_PARAMETER;
        v32_fail(u"HANDOFF", u"VALIDATION", s);
        goto out;
    }

    s = uefi_call_wrapper(BS->DisconnectController, 3, controller, NULL, NULL);
    if (EFI_ERROR(s)) { v32_fail(u"QUIESCE", u"DISCONNECT CONTROLLER", s); goto out; }
    disconnected = TRUE;
    Print(u"UEFI USB STACK QUIESCED / DISCONNECT=PASS\r\n");
    saved_rflags = v32_disable_cpu_interrupts();
    cpu_interrupts_disabled = TRUE;

    s = v32_validate_controller_path(controller, &b.controller_path);
    if (EFI_ERROR(s)) { v32_fail(u"BIND", u"CONTROLLER PATH", s); goto out; }

    s = v32_pci_match(controller, b.pci_segment, b.pci_bus,
                      b.pci_device, b.pci_function,
                      b.pci_vendor, b.pci_device_id, &p);
    if (EFI_ERROR(s)) { v32_fail(u"BIND", u"HANDOFF CONTROLLER", s); goto out; }
    s = v32_attrs(p, &attrs_changed);
    if (EFI_ERROR(s)) { v32_fail(u"PCI ATTR", u"MEMORY+BUS MASTER", s); goto out; }
    attrs_changed_here = attrs_changed != 0U;

    s = cfg32(p, 0x10U, &bar0);
    if (EFI_ERROR(s)) { v32_fail(u"PCI", u"BAR0", s); goto out; }
    if (bar0 & 1U) { s = EFI_UNSUPPORTED; v32_fail(u"PCI", u"BAR I/O", s); goto out; }
    if (((bar0 >> 1) & 3U) == 2U) {
        s = cfg32(p, 0x14U, &bar1);
        if (EFI_ERROR(s)) { v32_fail(u"PCI", u"BAR1", s); goto out; }
    } else if (((bar0 >> 1) & 3U) != 0U) {
        s = EFI_UNSUPPORTED; v32_fail(u"PCI", u"BAR TYPE", s); goto out;
    }
    Print(u"BAR=%016lx WIDTH=%s\r\n",
          (((bar0 >> 1) & 3U) == 2U) ? (((UINT64)bar1 << 32) | (bar0 & ~0xfU)) : (UINT64)(bar0 & ~0xfU),
          (((bar0 >> 1) & 3U) == 2U) ? u"64" : u"32");

    s = mr32(p, 0, &cap); ++reads; if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"CAPLENGTH+VERSION", s); goto out; }
    op = cap & 0xffU;
    {
        UINT16 ver = (UINT16)(cap >> 16);
        if (ver < XHCI_MIN_VERSION) {
            s = EFI_UNSUPPORTED;
            v32_fail(u"CAPS", u"VERSION", s);
            goto out;
        }
        Print(u"xHCI VERSION=%u.%02u\r\n", ver >> 8, ver & 0xffU);
    }
    s = mr32(p, 4, &hcs1); ++reads; if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"HCSPARAMS1", s); goto out; }
    s = mr32(p, 8, &hcs2); ++reads; if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"HCSPARAMS2", s); goto out; }
    s = mr32(p, 0x10U, &hcc); ++reads; if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"HCCPARAMS1", s); goto out; }
    s = mr32(p, 0x14U, &db); ++reads; if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"DBOFF", s); goto out; }
    s = mr32(p, 0x18U, &rt); ++reads; if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"RTSOFF", s); goto out; }
    s = mr32(p, op + 8U, &page_reg); ++reads; if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"PAGESIZE", s); goto out; }
    if (!page_reg) { s = EFI_UNSUPPORTED; v32_fail(u"CAPS", u"PAGESIZE ZERO", s); goto out; }
    db &= ~3U; rt &= ~0x1fU; ir = rt + 0x20U;
    max_slots = hcs1 & 0xffU;
    scratchpads = (((hcs2 >> 27) & 0x1fU) << 5) | ((hcs2 >> 21) & 0x1fU);
    ac64 = (hcc & 1U) != 0U;
    while (shift < 32U && !(page_reg & (1U << shift))) ++shift;
    if (shift >= 32U) { s = EFI_UNSUPPORTED; v32_fail(u"CAPS", u"PAGE BIT", s); goto out; }
    xpage = (UINTN)1U << (12U + shift);
    if (xpage > 0x100000U) { s = EFI_UNSUPPORTED; v32_fail(u"CAPS", u"PAGE SIZE", s); goto out; }
    scratch_pages = xpage / 4096U;
    if (!max_slots || scratchpads > MAX_SCRATCHPADS) { s = EFI_UNSUPPORTED; v32_fail(u"CAPS", u"LIMITS", s); goto out; }
    Print(u"CAPS: SLOTS=%u SCRATCHPADS=%u PAGE=%u AC64=%u CTXSZ=%u\r\n",
          max_slots, scratchpads, (UINT32)xpage, ac64 ? 1U : 0U,
          ((hcc >> 2) & 1U) ? 64U : 32U);

    s = mr32(p, op + 4U, &status); ++reads; if (EFI_ERROR(s)) { v32_fail(u"HALT", u"USBSTS", s); goto out; }
    s = mr32(p, op, &cmd); ++reads; if (EFI_ERROR(s)) { v32_fail(u"HALT", u"USBCMD", s); goto out; }
    if (!(status & STS_HCH)) {
        s = mw32(p, op, cmd & ~(CMD_RUN | CMD_INTE | CMD_HSEE)); ++writes;
        if (EFI_ERROR(s)) { v32_fail(u"HALT", u"STOP", s); goto out; }
        s = wait_hch(p, op, TRUE, 10000U, &status, &reads);
        if (EFI_ERROR(s)) { v32_fail(u"HALT", u"HCH", s); goto out; }
    }
    halted = TRUE;
    s = reset_xhci(p, op, &cmd, &status, &reads, &writes); if (EFI_ERROR(s)) goto out;
    if (!(status & STS_HCH)) { s = EFI_DEVICE_ERROR; goto out; }

    s = dma_alloc(p, 1, &dcbaa_d); if (EFI_ERROR(s)) goto out;
    s = dma_alloc(p, 1, &spa_d); if (EFI_ERROR(s)) goto out;
    s = dma_alloc(p, 1, &cr_d); if (EFI_ERROR(s)) goto out;
    s = dma_alloc(p, 1, &ev_d); if (EFI_ERROR(s)) goto out;
    s = dma_alloc(p, 1, &erst_d); if (EFI_ERROR(s)) goto out;
    if (scratchpads) {
        scratch = (struct dma_obj *)AllocateZeroPool(sizeof(struct dma_obj) * scratchpads);
        if (!scratch) { s = EFI_OUT_OF_RESOURCES; goto out; }
        for (UINT32 i = 0; i < scratchpads; ++i) {
            s = dma_alloc(p, scratch_pages, &scratch[i]);
            if (EFI_ERROR(s)) goto out;
        }
    }
    if (!v32_dma_ok(&dcbaa_d, ac64) || !v32_dma_ok(&spa_d, ac64) ||
        !v32_dma_ok(&cr_d, ac64) || !v32_dma_ok(&ev_d, ac64) ||
        !v32_dma_ok(&erst_d, ac64)) {
        s = EFI_UNSUPPORTED; v32_fail(u"DMA", u"ADDRESSING", s); goto out;
    }

    dcbaa = (UINT64 *)dcbaa_d.host;
    spa = (UINT64 *)spa_d.host;
    cr = (UINT32 *)cr_d.host;
    erst = (UINT64 *)erst_d.host;
    dcbaa[0] = scratchpads ? spa_d.dev : 0;
    for (UINT32 i = 0; i < scratchpads; ++i) spa[i] = scratch[i].dev;
    cr[0] = 0; cr[1] = 0; cr[2] = 0;
    cr[3] = TRB_CYCLE | (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT) |
            ((UINT32)slot_type << 16);
    cr[4] = 0; cr[5] = 0; cr[6] = 0; cr[7] = 0;
    cr[(CMD_TRBS - 1U) * 4U + 0U] = (UINT32)cr_d.dev;
    cr[(CMD_TRBS - 1U) * 4U + 1U] = (UINT32)(cr_d.dev >> 32);
    cr[(CMD_TRBS - 1U) * 4U + 2U] = 0;
    cr[(CMD_TRBS - 1U) * 4U + 3U] = TRB_CYCLE | TRB_LINK_TOGGLE |
                                     (TRB_LINK << TRB_TYPE_SHIFT);
    {
        UINT32 *erst32 = (UINT32 *)erst;
        erst32[0] = (UINT32)ev_d.dev;
        erst32[1] = (UINT32)(ev_d.dev >> 32);
        erst32[2] = EVENT_TRBS;
        erst32[3] = 0U;
    }

    s = mw64(p, op + 0x30U, dcbaa_d.dev); writes += 2; if (EFI_ERROR(s)) goto out;
    s = mw32(p, op + 0x38U, 1U); ++writes; if (EFI_ERROR(s)) goto out;
    s = mw64(p, op + 0x18U, cr_d.dev | CRCR_RCS); writes += 2; if (EFI_ERROR(s)) goto out;
    s = mw32(p, ir + 8U, 1U); ++writes; if (EFI_ERROR(s)) goto out;
    s = mw64(p, ir + 0x10U, erst_d.dev & ERST_ADDR_MASK); writes += 2; if (EFI_ERROR(s)) goto out;
    s = mw64(p, ir + 0x18U, ev_d.dev & ERDP_ADDR_MASK); writes += 2; if (EFI_ERROR(s)) goto out;
    s = mr32(p, ir, &iman); ++reads; if (EFI_ERROR(s)) goto out;
    s = mw32(p, ir, iman & ~IMAN_IE); ++writes; if (EFI_ERROR(s)) goto out;
    s = mr32(p, op, &cmd); ++reads; if (EFI_ERROR(s)) goto out;
    s = mw32(p, op, (cmd & ~(CMD_INTE | CMD_HSEE)) | CMD_RUN); ++writes;
    if (EFI_ERROR(s)) goto out;
    halted = FALSE; running = TRUE;
    s = wait_hch(p, op, FALSE, 10000U, &status, &reads); if (EFI_ERROR(s)) goto out;

    if (b.keyboard.root_port == 0xffU) { s = EFI_UNSUPPORTED; v32_fail(u"PORT", u"ROOT PORT NUMBER", s); goto out; }
    xhci_port = (UINT8)(b.keyboard.root_port + 1U);
    s = v32_supported_protocol(p, hcc, xhci_port, &proto_major, &proto_minor, &slot_type, &reads); if (EFI_ERROR(s)) { v32_fail(u"PORT", u"SUPPORTED PROTOCOL", s); goto out; }
    portsc_off = op + 0x400U + ((UINT32)xhci_port - 1U) * 0x10U;
    s = mr32(p, portsc_off, &status); ++reads; if (EFI_ERROR(s)) goto out;
    if (!(status & V32_PORTSC_CCS)) { s = EFI_NOT_FOUND; v32_fail(u"PORT", u"NOT CONNECTED", s); goto out; }
    speed = (UINT8)((status & V32_PORTSC_SPEED_MASK) >> V32_PORTSC_SPEED_SHIFT);
    if (speed != V32_USB_SPEED_LOW && speed != V32_USB_SPEED_FULL) { s = EFI_UNSUPPORTED; v32_fail(u"PORT", u"LS/FS ONLY", s); goto out; }
    if (proto_major != 2U) { s = EFI_UNSUPPORTED; v32_fail(u"PORT", u"USB2 PROTOCOL", s); goto out; }
    Print(u"PORT: UEFI=%u XHCI=%u CONNECTED SPEED=%u PROTOCOL=%u.%u SLOT-TYPE=%u\r\n",
          b.keyboard.root_port, xhci_port, speed, proto_major, proto_minor, slot_type);

    if (status & V32_PORTSC_PRC) {
        s = v32_portsc_write(p, portsc_off, status, 0, V32_PORTSC_PR,
                             V32_PORTSC_PRC, &writes);
        if (EFI_ERROR(s)) goto out;
        s = mr32(p, portsc_off, &status); ++reads;
        if (EFI_ERROR(s) || (status & V32_PORTSC_PRC)) {
            s = EFI_DEVICE_ERROR; v32_fail(u"PORT RESET", u"STALE PRC", s); goto out;
        }
    }
    s = v32_portsc_write(p, portsc_off, status, V32_PORTSC_PR, 0, 0, &writes);
    if (EFI_ERROR(s)) goto out;
    Print(u"PORT RESET: ASSERTED / WAITING\r\n");
    s = v32_wait_event(p, ir + 0x18U, &ev_d, V32_TRB_PORT_STATUS_CHANGE,
                       xhci_port, 0, 0, NULL,
                       &event_index, &event_cycle, &reads, &writes);
    if (EFI_ERROR(s)) { v32_fail(u"PORT RESET", u"PORT STATUS CHANGE", s); goto out; }
    s = mr32(p, portsc_off, &status); ++reads; if (EFI_ERROR(s)) goto out;
    speed = (UINT8)((status & V32_PORTSC_SPEED_MASK) >> V32_PORTSC_SPEED_SHIFT);
    if (!(status & V32_PORTSC_CCS) || !(status & V32_PORTSC_PED) ||
        (status & V32_PORTSC_PR) || (status & V32_PORTSC_PLS_MASK) ||
        !(status & V32_PORTSC_PRC) ||
        (speed != V32_USB_SPEED_LOW && speed != V32_USB_SPEED_FULL)) {
        s = EFI_DEVICE_ERROR; v32_fail(u"PORT RESET", u"POST RESET STATE", s); goto out;
    }
    Print(u"PORT RESET: PRC=1 PED=1 U0=1 LIVE-SPEED=%u PASS\r\n", speed);

    s = mw32(p, db, 0U); ++writes;
    if (EFI_ERROR(s)) goto out;
    Print(u"ENABLE SLOT: COMMAND DOORBELL=0 PTR=%016lx SLOT-TYPE=%u\r\n", cr_d.dev, slot_type);
    s = v32_wait_event(p, ir + 0x18U, &ev_d, V32_TRB_COMMAND_COMPLETION,
                       0, cr_d.dev, 0, &slot_id,
                       &event_index, &event_cycle, &reads, &writes);
    if (EFI_ERROR(s)) { v32_fail(u"ENABLE SLOT", u"COMPLETION", s); goto out; }
    Print(u"ENABLE SLOT: COMPLETION SUCCESS SLOT=%u PASS\r\n", slot_id);

    s = dma_alloc(p, 1, &input_d); if (EFI_ERROR(s)) goto out;
    s = dma_alloc(p, 1, &output_d); if (EFI_ERROR(s)) goto out;
    s = dma_alloc(p, 1, &ep0_d); if (EFI_ERROR(s)) goto out;
    if (!v32_dma_ok(&input_d, ac64) || !v32_dma_ok(&output_d, ac64) ||
        !v32_dma_ok(&ep0_d, ac64)) {
        s = EFI_UNSUPPORTED; v32_fail(u"ADDRESS", u"DMA ADDRESSING", s); goto out;
    }
    {
        UINTN stride = ((hcc >> 2) & 1U) ? 64U : 32U;
        UINT32 *input_ctx = (UINT32 *)input_d.host;
        UINT32 *slot = (UINT32 *)((UINT8 *)input_d.host + stride);
        UINT32 *epctx = (UINT32 *)((UINT8 *)input_d.host + stride * 2U);
        uefi_call_wrapper(BS->SetMem, 3, input_d.host, 4096U, 0);
        uefi_call_wrapper(BS->SetMem, 3, output_d.host, 4096U, 0);
        uefi_call_wrapper(BS->SetMem, 3, ep0_d.host, 4096U, 0);
        input_ctx[0] = 0U;
        input_ctx[1] = (1U << 0) | (1U << 1);
        slot[0] = (1U << 27) | ((UINT32)speed << 20);
        slot[1] = ((UINT32)xhci_port << 16);
        epctx[0] = 0U;
        epctx[1] = (3U << 1) | (4U << 3) | (8U << 16);
        epctx[2] = (UINT32)ep0_d.dev | 1U;
        epctx[3] = (UINT32)(ep0_d.dev >> 32);
        epctx[4] = 8U;
        ep0 = (UINT32 *)ep0_d.host;
        ep0[(4096U / 4U) - 4U] = (UINT32)ep0_d.dev;
        ep0[(4096U / 4U) - 3U] = (UINT32)(ep0_d.dev >> 32);
        ep0[(4096U / 4U) - 2U] = 0U;
        ep0[(4096U / 4U) - 1U] = TRB_CYCLE | TRB_LINK_TOGGLE |
                                  (TRB_LINK << TRB_TYPE_SHIFT);
        dcbaa[slot_id] = output_d.dev;
    }

    cr[4] = (UINT32)input_d.dev;
    cr[5] = (UINT32)(input_d.dev >> 32);
    cr[6] = 0U;
    cr[7] = TRB_CYCLE | (V32_TRB_ADDRESS_DEVICE << TRB_TYPE_SHIFT) |
            ((UINT32)slot_id << 24);
    s = mw32(p, db, 0U);
    ++writes;
    if (EFI_ERROR(s)) goto out;
    Print(u"ADDRESS DEVICE: COMMAND DOORBELL=0 INPUT=%016lx EP0-MPS=8 AVG-TRB=8\r\n",
          input_d.dev);
    s = v32_wait_event(p, ir + 0x18U, &ev_d, V32_TRB_COMMAND_COMPLETION,
                       0, cr_d.dev + 16U, slot_id, NULL,
                       &event_index, &event_cycle, &reads, &writes);
    if (EFI_ERROR(s)) { v32_fail(u"ADDRESS", u"COMPLETION", s); goto out; }
    {
        UINTN stride = ((hcc >> 2) & 1U) ? 64U : 32U;
        UINT32 *out_slot = (UINT32 *)output_d.host;
        UINT32 d3 = out_slot[3];
        UINT32 state = (d3 >> 27) & 0x1fU;
        UINT32 addr = d3 & 0xffU;
        if (state != 2U || addr == 0U) {
            s = EFI_DEVICE_ERROR; v32_fail(u"ADDRESS", u"ADDRESSED STATE", s); goto out;
        }
        Print(u"ADDRESS DEVICE: SUCCESS STATE=ADDRESSED(%u) USB-ADDR=%u PASS\r\n", state, addr);
        (void)stride;
    }

    s = mr32(p, op, &cmd); ++reads; if (EFI_ERROR(s)) goto out;
    s = mw32(p, op, cmd & ~(CMD_RUN | CMD_INTE | CMD_HSEE)); ++writes;
    if (EFI_ERROR(s)) goto out;
    running = FALSE;
    s = wait_hch(p, op, TRUE, 10000U, &status, &reads);
    if (EFI_ERROR(s)) { v32_fail(u"HALT", u"CONFIRM HCH", s); v32_fatal_running(&reads, &writes); }
    halted = TRUE;
    s = reset_xhci(p, op, &cmd, &status, &reads, &writes); if (EFI_ERROR(s)) goto out;
    if (!(status & STS_HCH)) { s = EFI_DEVICE_ERROR; goto out; }
    s = mw64(p, op + 0x18U, 0); writes += 2; if (EFI_ERROR(s)) goto out;
    s = mw64(p, op + 0x30U, 0); writes += 2; if (EFI_ERROR(s)) goto out;
    s = mw32(p, op + 0x38U, 0); ++writes; if (EFI_ERROR(s)) goto out;
    s = mw32(p, ir + 8U, 0); ++writes; if (EFI_ERROR(s)) goto out;
    s = mw64(p, ir + 0x10U, 0); writes += 2; if (EFI_ERROR(s)) goto out;
    s = mw64(p, ir + 0x18U, 0); writes += 2; if (EFI_ERROR(s)) goto out;
    refs_cleared = TRUE;

out:
    if (EFI_ERROR(s) && !halted && (running || disconnected)) v32_fatal_running(&reads, &writes);
    if (halted && p && !refs_cleared) {
        ts = mw64(p, op + 0x18U, 0); writes += 2; if (EFI_ERROR(ts)) v32_fatal_running(&reads, &writes);
        ts = mw64(p, op + 0x30U, 0); writes += 2; if (EFI_ERROR(ts)) v32_fatal_running(&reads, &writes);
        ts = mw32(p, op + 0x38U, 0); ++writes; if (EFI_ERROR(ts)) v32_fatal_running(&reads, &writes);
        ts = mw32(p, ir + 8U, 0); ++writes; if (EFI_ERROR(ts)) v32_fatal_running(&reads, &writes);
        ts = mw64(p, ir + 0x10U, 0); writes += 2; if (EFI_ERROR(ts)) v32_fatal_running(&reads, &writes);
        ts = mw64(p, ir + 0x18U, 0); writes += 2; if (EFI_ERROR(ts)) v32_fatal_running(&reads, &writes);
        refs_cleared = TRUE;
    }
    if (halted && p) {
        if (scratch) {
            for (UINT32 i = 0; i < scratchpads; ++i) dma_free(p, &scratch[i]);
            FreePool(scratch);
        }
        dma_free(p, &ep0_d); dma_free(p, &output_d); dma_free(p, &input_d);
        dma_free(p, &erst_d); dma_free(p, &ev_d); dma_free(p, &cr_d);
        dma_free(p, &spa_d); dma_free(p, &dcbaa_d);
    }
    if (p && disconnected && attrs_changed_here) {
        ts = uefi_call_wrapper(p->Attributes, 4, p,
                               EfiPciIoAttributeOperationDisable,
                               attrs_changed, NULL);
        if (EFI_ERROR(ts) && !EFI_ERROR(s)) s = ts;
    }
    if (cpu_interrupts_disabled) {
        v32_restore_cpu_interrupts(saved_rflags);
        cpu_interrupts_disabled = FALSE;
    }
    if (EFI_ERROR(s)) {
        Print(u"\r\nV32 GATE 6: FAIL RESULT=%r\r\n", s);
        Print(u"FAIL STAGE=%s OP=%s STATUS=%r\r\n", fail_stage, fail_op, fail_status);
    } else {
        Print(u"\r\nV32 GATE 6: PASS\r\n");
        Print(u"UEFI DISCOVERY=1 QUIESCE=1 PORT RESET=1 ENABLE SLOT=1 ADDRESS DEVICE=1\r\n");
        Print(u"COMMANDS=2 COMMAND-DOORBELLS=2 CPU-INTERRUPT-DELIVERY=DISABLED / LS+FS ONLY\r\n");
        Print(u"INITIAL EP0 MPS=8 / POST-ADDRESS SLOT STATE=ADDRESSED\r\n");
        Print(u"RESET RECOVERY + POINTER CLEAR + DMA RELEASE=PASS\r\n");
    }
    Print(u"MMIO READS=%u WRITES=%u RESULT=%r\r\n", reads, writes, s);
    v32_press_key(st);
    return s;
}
