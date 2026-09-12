/*
 * Version 32 — clean Gate 6 implementation.
 *
 * V31 is included only as the proven low-level xHCI/DMA primitive layer.
 * V32 owns the complete Gate 6 state machine and handoff contract; it does
 * not inherit V32 state from the previous implementation.
 */
#define efi_main v31_reference_entry
#include "xhci_mmio_v31.c"
#undef efi_main

#include "usb_io_compat.h"

#define V32_HANDOFF_MAGIC       0x48494458U
#define V32_HANDOFF_VERSION     2U
#define V32_MAX_PATH_BYTES      512U
#define V32_MAX_ENDPOINTS       8U

#define V32_TRB_ENABLE_SLOT     9U
#define V32_TRB_ADDRESS_DEVICE  11U
#define V32_TRB_PORT_STATUS     34U
#define V32_TRB_COMMAND_EVENT   33U

#define V32_STS_HCH             0x00000001U
#define V32_STS_CNR             0x00000800U
#define V32_CMD_RUN             0x00000001U
#define V32_CMD_RESET           0x00000002U
#define V32_CMD_INTE            0x00000004U
#define V32_CMD_HSEE            0x00000008U
#define V32_HCC_AC64            0x00000001U
#define V32_HCC_CTXSZ           0x00000004U
#define V32_IMAN_IP             0x00000001U
#define V32_PORT_CCS            0x00000001U
#define V32_PORT_PED            0x00000002U
#define V32_PORT_PR             0x00000010U
#define V32_PORT_PLS_MASK       0x000001e0U
#define V32_PORT_SPEED_MASK     0x00003c00U
#define V32_PORT_SPEED_SHIFT    10U
#define V32_PORT_PRC            0x00200000U
#define V32_PORT_RW_FIELDS      (V32_PORT_PED | V32_PORT_PR | V32_PORT_PLS_MASK | \
                                 0x00000200U | 0x0000c000U | 0x00010000U | \
                                 0x02000000U | 0x04000000U | 0x08000000U)
#define V32_PORT_RO_FIELDS      (V32_PORT_CCS | 0x00000008U | V32_PORT_SPEED_MASK | 0x40000000U)
#define V32_PORT_PLS_U0         0U
#define V32_USB_SPEED_FULL      1U
#define V32_USB_SPEED_LOW       2U
#define V32_EVENT_TRBS          16U
#define V32_COMMAND_TRBS        256U
#define V32_CC_SUCCESS          1U
#define V32_CTX_SLOT_ADD        0x00000001U
#define V32_CTX_EP0_ADD         0x00000002U
#define V32_EP_TYPE_CONTROL     4U
#define V32_SLOT_STATE_ADDRESSED 2U
#define V32_IMAN_OFF            0x20U
#define V32_ERSTSZ_OFF          0x28U
#define V32_ERSTBA_OFF          0x30U
#define V32_ERDP_OFF            0x38U
#define V32_PORTSC_BASE         0x400U
#define V32_CONFIG_OFF          0x38U
#define V32_DCBAAP_OFF          0x30U
#define V32_CRCR_OFF            0x18U
#define V32_DB0_OFF             0U

/* The handoff remains a value object. The bridge never keeps EFI_USB_IO state. */
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
    UINT8  kind;                 /* 1 keyboard, 2 mouse */
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

static EFI_GUID v32_usb_io_guid = EFI_USB_IO_PROTOCOL_GUID;

static void v32_fail(const CHAR16 *stage, const CHAR16 *op, EFI_STATUS s)
{
    fail(stage, op, s);
}

static UINT64 v32_disable_interrupts(void)
{
    UINT64 flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void v32_restore_interrupts(UINT64 flags)
{
    if (flags & (1ULL << 9))
        __asm__ __volatile__("sti" ::: "memory");
}

static UINT16 v32_path_len(const EFI_DEVICE_PATH_PROTOCOL *p)
{
    return (UINT16)p->Length[0] | ((UINT16)p->Length[1] << 8);
}

static EFI_STATUS v32_path_size(EFI_DEVICE_PATH_PROTOCOL *path, UINTN *size)
{
    UINT8 *p = (UINT8 *)path;
    UINTN total = 0;
    if (!p || !size)
        return EFI_INVALID_PARAMETER;
    while (total + sizeof(EFI_DEVICE_PATH_PROTOCOL) <= V32_MAX_PATH_BYTES) {
        EFI_DEVICE_PATH_PROTOCOL *h = (EFI_DEVICE_PATH_PROTOCOL *)p;
        UINT16 len = v32_path_len(h);
        if (len < sizeof(EFI_DEVICE_PATH_PROTOCOL) ||
            total + len > V32_MAX_PATH_BYTES)
            return EFI_DEVICE_ERROR;
        total += len;
        if (h->Type == 0x7fU) {
            *size = total;
            return EFI_SUCCESS;
        }
        p += len;
    }
    return EFI_BAD_BUFFER_SIZE;
}

static EFI_STATUS v32_copy_path(V32_DEVICE_PATH *dst, EFI_DEVICE_PATH_PROTOCOL *src)
{
    UINTN size;
    EFI_STATUS s;
    if (!dst || !src)
        return EFI_INVALID_PARAMETER;
    s = v32_path_size(src, &size);
    if (EFI_ERROR(s))
        return s;
    uefi_call_wrapper(BS->SetMem, 3, dst, sizeof(*dst), 0);
    CopyMem(dst->data, src, size);
    dst->size = (UINT16)size;
    return EFI_SUCCESS;
}

static BOOLEAN v32_path_equal(const V32_DEVICE_PATH *expected,
                              EFI_DEVICE_PATH_PROTOCOL *actual)
{
    UINTN size;
    if (!expected || !actual || !expected->size ||
        expected->size > V32_MAX_PATH_BYTES)
        return FALSE;
    if (EFI_ERROR(v32_path_size(actual, &size)) || size != expected->size)
        return FALSE;
    return CompareMem(expected->data, actual, size) == 0;
}

static UINT8 v32_root_port(EFI_DEVICE_PATH_PROTOCOL *path)
{
    UINT8 *p = (UINT8 *)path;
    UINT8 root = 0xffU;
    while (p) {
        EFI_DEVICE_PATH_PROTOCOL *h = (EFI_DEVICE_PATH_PROTOCOL *)p;
        UINT16 len = v32_path_len(h);
        if (len < sizeof(EFI_DEVICE_PATH_PROTOCOL) || h->Type == 0x7fU)
            break;
        /* Messaging / USB node: Type 0x03, SubType 0x05, Port at byte 4. */
        if (h->Type == 0x03U && h->SubType == 0x05U && len >= 6U)
            root = p[4];
        p += len;
    }
    return root;
}

static BOOLEAN v32_is_hid(const EFI_USB_INTERFACE_DESCRIPTOR *d, UINT8 *kind)
{
    if (!d || !kind || d->InterfaceClass != 0x03U ||
        d->InterfaceSubClass != 0x01U)
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

static BOOLEAN v32_pci_prefix(EFI_DEVICE_PATH_PROTOCOL *pci,
                              EFI_DEVICE_PATH_PROTOCOL *usb,
                              UINTN *matched)
{
    UINT8 *p = (UINT8 *)pci;
    UINT8 *q = (UINT8 *)usb;
    UINTN total = 0;
    if (!p || !q)
        return FALSE;
    for (;;) {
        EFI_DEVICE_PATH_PROTOCOL *a = (EFI_DEVICE_PATH_PROTOCOL *)p;
        EFI_DEVICE_PATH_PROTOCOL *b = (EFI_DEVICE_PATH_PROTOCOL *)q;
        UINT16 alen = v32_path_len(a);
        UINT16 blen = v32_path_len(b);
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

static EFI_STATUS v32_controller_for_path(EFI_DEVICE_PATH_PROTOCOL *usb_path,
                                          EFI_HANDLE *controller)
{
    EFI_HANDLE *handles = NULL;
    UINTN count = 0, i, best_len = 0;
    EFI_HANDLE best = NULL;
    EFI_STATUS s;

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &count, &handles);
    if (EFI_ERROR(s)) return s;
    for (i = 0; i < count; ++i) {
        EFI_DEVICE_PATH_PROTOCOL *pci_path = NULL;
        UINTN matched = 0;
        s = uefi_call_wrapper(BS->OpenProtocol, 6, handles[i],
                              &DevicePathProtocol, (VOID **)&pci_path,
                              NULL, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s) || !pci_path)
            continue;
        if (v32_pci_prefix(pci_path, usb_path, &matched) && matched > best_len) {
            best_len = matched;
            best = handles[i];
        }
    }
    FreePool(handles);
    if (!best) return EFI_NOT_FOUND;
    *controller = best;
    return EFI_SUCCESS;
}

static EFI_STATUS v32_fill_device(EFI_USB_IO_PROTOCOL *usb,
                                  EFI_USB_INTERFACE_DESCRIPTOR *iface,
                                  EFI_DEVICE_PATH_PROTOCOL *path,
                                  UINT8 kind, V32_HID_DEVICE *out)
{
    EFI_USB_DEVICE_DESCRIPTOR dd;
    EFI_USB_CONFIG_DESCRIPTOR cd;
    EFI_USB_ENDPOINT_DESCRIPTOR ep;
    UINTN i, n;
    BOOLEAN got_in = FALSE;
    EFI_STATUS s;

    uefi_call_wrapper(BS->SetMem, 3, out, sizeof(*out), 0);
    out->kind = kind;
    out->root_port = v32_root_port(path);
    if (out->root_port == 0xffU)
        return EFI_NOT_FOUND;
    out->interface_number = iface->InterfaceNumber;
    out->interface_protocol = iface->InterfaceProtocol;
    out->endpoint_count = iface->NumEndpoints;
    if (out->endpoint_count > V32_MAX_ENDPOINTS)
        out->endpoint_count = V32_MAX_ENDPOINTS;
    s = v32_copy_path(&out->device_path, path);
    if (EFI_ERROR(s)) return s;

    if (!EFI_ERROR(uefi_call_wrapper(usb->UsbGetDeviceDescriptor, 3, usb, &dd))) {
        out->vendor_id = dd.IdVendor;
        out->product_id = dd.IdProduct;
        out->bcd_usb = dd.BcdUSB;
        out->ep0_mps_descriptor = dd.MaxPacketSize0;
    }
    if (!EFI_ERROR(uefi_call_wrapper(usb->UsbGetConfigDescriptor, 3, usb, &cd)))
        out->configuration_value = cd.ConfigurationValue;

    n = out->endpoint_count;
    for (i = 0; i < n; ++i) {
        if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetEndpointDescriptor, 4,
                                         usb, (UINT8)i, &ep)))
            continue;
        out->endpoints[i].endpoint_address = ep.EndpointAddress;
        out->endpoints[i].attributes = ep.Attributes;
        out->endpoints[i].max_packet_size = ep.MaxPacketSize;
        out->endpoints[i].interval = ep.Interval;
        if (!got_in && ep.DescriptorType == 0x05U && ep.Length >= 7U &&
            (ep.EndpointAddress & 0x80U) &&
            (ep.Attributes & 0x03U) == 0x03U &&
            ep.MaxPacketSize != 0U && ep.Interval != 0U) {
            out->interrupt_in_endpoint = ep.EndpointAddress;
            out->interrupt_max_packet_size = ep.MaxPacketSize;
            out->interval = ep.Interval;
            got_in = TRUE;
        }
    }
    return got_in ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static EFI_STATUS v32_produce_handoff(EFI_HANDLE image, V32_HANDOFF *h,
                                      EFI_HANDLE *controller)
{
    EFI_HANDLE *handles = NULL;
    UINTN count = 0, i;
    UINTN keyboards = 0, mice = 0;
    EFI_STATUS s;
    EFI_HANDLE selected_controller = NULL;

    uefi_call_wrapper(BS->SetMem, 3, h, sizeof(*h), 0);
    h->magic = V32_HANDOFF_MAGIC;
    h->version = V32_HANDOFF_VERSION;
    h->size = (UINT16)sizeof(*h);

    s = LibLocateHandle(ByProtocol, &v32_usb_io_guid, NULL, &count, &handles);
    if (EFI_ERROR(s)) return EFI_NOT_FOUND;

    for (i = 0; i < count; ++i) {
        EFI_USB_IO_PROTOCOL *usb = NULL;
        EFI_USB_INTERFACE_DESCRIPTOR iface;
        EFI_DEVICE_PATH_PROTOCOL *path = NULL;
        EFI_HANDLE ch = NULL;
        UINT8 kind;

        s = uefi_call_wrapper(BS->OpenProtocol, 6, handles[i],
                              &v32_usb_io_guid, (VOID **)&usb,
                              image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s)) continue;
        if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetInterfaceDescriptor, 3,
                                         usb, &iface)) ||
            !v32_is_hid(&iface, &kind))
            continue;
        s = uefi_call_wrapper(BS->OpenProtocol, 6, handles[i],
                              &DevicePathProtocol, (VOID **)&path,
                              image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s) || !path) {
            FreePool(handles);
            return EFI_DEVICE_ERROR;
        }
        s = v32_controller_for_path(path, &ch);
        if (EFI_ERROR(s) || !ch) {
            FreePool(handles);
            return s;
        }

        if (kind == 1U) {
            if (++keyboards != 1U) {
                FreePool(handles);
                return EFI_DEVICE_ERROR;
            }
            s = v32_fill_device(usb, &iface, path, kind, &h->keyboard);
            if (EFI_ERROR(s)) {
                FreePool(handles);
                return s;
            }
            selected_controller = ch;
        } else {
            if (++mice != 1U) {
                FreePool(handles);
                return EFI_DEVICE_ERROR;
            }
            s = v32_fill_device(usb, &iface, path, kind, &h->mouse);
            if (EFI_ERROR(s)) {
                FreePool(handles);
                return s;
            }
            if (selected_controller && ch != selected_controller) {
                /* Gate 6 uses one controller; keep the mouse out rather than
                 * silently changing controller ownership. */
                uefi_call_wrapper(BS->SetMem, 3, &h->mouse, sizeof(h->mouse), 0);
                --mice;
            }
        }
    }
    FreePool(handles);

    if (keyboards != 1U || !selected_controller)
        return EFI_NOT_FOUND;

    h->device_count = (UINT16)(1U + (mice ? 1U : 0U));
    *controller = selected_controller;

    {
        EFI_PCI_IO_PROTOCOL *p = NULL;
        EFI_DEVICE_PATH_PROTOCOL *cp = NULL;
        UINTN seg = 0, bus = 0, dev = 0, fun = 0;
        UINT32 id = 0;

        s = uefi_call_wrapper(BS->OpenProtocol, 6, selected_controller,
                              &PciGuid, (VOID **)&p, image, NULL,
                              EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s)) return s;
        s = uefi_call_wrapper(p->GetLocation, 5, p, &seg, &bus, &dev, &fun);
        if (EFI_ERROR(s)) return s;
        s = cfg32(p, 0, &id);
        if (EFI_ERROR(s)) return s;
        s = uefi_call_wrapper(BS->OpenProtocol, 6, selected_controller,
                              &DevicePathProtocol, (VOID **)&cp,
                              image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s) || !cp) return EFI_DEVICE_ERROR;
        s = v32_copy_path(&h->controller_path, cp);
        if (EFI_ERROR(s)) return s;
        h->pci_segment = (UINT16)seg;
        h->pci_bus = (UINT8)bus;
        h->pci_device = (UINT8)dev;
        h->pci_function = (UINT8)fun;
        h->pci_vendor = (UINT16)(id & 0xffffU);
        h->pci_device_id = (UINT16)(id >> 16);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS v32_validate_handoff(const V32_HANDOFF *h)
{
    if (!h || h->magic != V32_HANDOFF_MAGIC ||
        h->version != V32_HANDOFF_VERSION || h->size != sizeof(*h) ||
        h->device_count < 1U || h->device_count > 2U ||
        h->keyboard.kind != 1U || h->keyboard.root_port == 0xffU ||
        h->keyboard.device_path.size == 0U ||
        h->controller_path.size == 0U)
        return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}

static EFI_STATUS v32_bind_controller(EFI_HANDLE controller,
                                      const V32_HANDOFF *h,
                                      EFI_PCI_IO_PROTOCOL **out)
{
    EFI_PCI_IO_PROTOCOL *p = NULL;
    UINTN seg = 0, bus = 0, dev = 0, fun = 0;
    UINT32 id = 0, cls = 0;
    EFI_STATUS s;

    s = uefi_call_wrapper(BS->OpenProtocol, 6, controller, &PciGuid,
                          (VOID **)&p, NULL, NULL,
                          EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(s)) return s;
    s = uefi_call_wrapper(p->GetLocation, 5, p, &seg, &bus, &dev, &fun);
    if (EFI_ERROR(s) || seg != h->pci_segment || bus != h->pci_bus ||
        dev != h->pci_device || fun != h->pci_function)
        return EFI_DEVICE_ERROR;
    s = cfg32(p, 0, &id);
    if (EFI_ERROR(s) || (UINT16)(id & 0xffffU) != h->pci_vendor ||
        (UINT16)(id >> 16) != h->pci_device_id)
        return EFI_DEVICE_ERROR;
    s = cfg32(p, 8, &cls);
    if (EFI_ERROR(s) || ((cls >> 24) & 0xffU) != 0x0cU ||
        ((cls >> 16) & 0xffU) != 0x03U ||
        ((cls >> 8) & 0xffU) != 0x30U)
        return EFI_UNSUPPORTED;
    *out = p;
    return EFI_SUCCESS;
}

static EFI_STATUS v32_pci_attributes(EFI_PCI_IO_PROTOCOL *p,
                                     UINT64 *original, UINT64 *enabled)
{
    EFI_STATUS s;
    UINT64 attrs = 0;
    const UINT64 need = EFI_PCI_IO_ATTRIBUTE_MEMORY |
                        EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
    s = uefi_call_wrapper(p->Attributes, 4, p,
                          EfiPciIoAttributeOperationGet, 0, &attrs);
    if (EFI_ERROR(s)) return s;
    *original = attrs;
    *enabled = 0;
    if ((attrs & need) != need) {
        UINT64 supported = 0;
        s = uefi_call_wrapper(p->Attributes, 4, p,
                              EfiPciIoAttributeOperationSupported, need,
                              &supported);
        if (EFI_ERROR(s) || (supported & need) != need)
            return EFI_UNSUPPORTED;
        s = uefi_call_wrapper(p->Attributes, 4, p,
                              EfiPciIoAttributeOperationEnable, need, NULL);
        if (EFI_ERROR(s)) return s;
        *enabled = need & ~attrs;
    }
    return EFI_SUCCESS;
}

static UINT32 v32_port_write_base(UINT32 x)
{
    /* Preserve RO/RW fields; never replay RW1C/RW1S change bits. */
    return (x & V32_PORT_RO_FIELDS) | (x & V32_PORT_RW_FIELDS);
}

static EFI_STATUS v32_protocol_slot_type(EFI_PCI_IO_PROTOCOL *p, UINT32 hcc,
                                         UINT32 port, UINT32 *slot_type,
                                         UINT32 *reads)
{
    UINT32 off = ((hcc >> 16) & 0xffffU) * 4U;
    UINTN n = 0;
    *slot_type = 0;
    while (off && n++ < 64U) {
        UINT32 d0, d2, d3, next;
        EFI_STATUS s = mr32(p, off, &d0);
        (*reads)++;
        if (EFI_ERROR(s)) return s;
        if ((d0 & 0xffU) == 2U) {
            s = mr32(p, off + 8U, &d2); (*reads)++;
            if (EFI_ERROR(s)) return s;
            s = mr32(p, off + 12U, &d3); (*reads)++;
            if (EFI_ERROR(s)) return s;
            {
                UINT32 po = d2 & 0xffU;
                UINT32 pc = (d2 >> 8) & 0xffU;
                if (pc && port >= po && port < po + pc) {
                    *slot_type = d3 & 0x1fU;
                    return EFI_SUCCESS;
                }
            }
        }
        next = ((d0 >> 8) & 0xffU) * 4U;
        if (!next) break;
        off += next;
    }
    return EFI_NOT_FOUND;
}

static void v32_clear_trb(VOID *base, UINTN index)
{
    uefi_call_wrapper(BS->SetMem, 3, (UINT8 *)base + index * 16U, 16U, 0);
}

static UINT64 v32_trb_ptr(const struct dma_obj *ring, UINTN index)
{
    return ring->dev + index * 16U;
}

static EFI_STATUS v32_poll_event(EFI_PCI_IO_PROTOCOL *p, UINT32 ir,
                                 struct dma_obj *ev, UINTN *index,
                                 UINT8 *cycle, UINT32 *type, UINT32 *dw0,
                                 UINT32 *dw2, UINT32 *dw3, UINT64 *ptr)
{
    UINTN i;
    UINT32 *r = (UINT32 *)ev->host;
    for (i = 0; i < 10000U; ++i) {
        UINTN n = *index;
        UINT32 d3 = r[n * 4U + 3U];
        if ((d3 & 1U) == *cycle) {
            UINT64 processed = v32_trb_ptr(ev, n);
            *type = (d3 >> 10) & 0x3fU;
            *dw0 = r[n * 4U + 0U];
            *dw2 = r[n * 4U + 2U];
            *dw3 = d3;
            *ptr = ((UINT64)r[n * 4U + 1U] << 32) | r[n * 4U + 0U];
            v32_clear_trb(ev->host, n);
            __sync_synchronize();
            n++;
            if (n == V32_EVENT_TRBS) {
                n = 0;
                *cycle ^= 1U;
            }
            *index = n;
            /* ERDP is the next event software expects to consume. */
            if (EFI_ERROR(mw64(p, ir + V32_ERDP_OFF,
                               processed | 0x8ULL)))
                return EFI_DEVICE_ERROR;
            {
                UINT32 iman;
                if (EFI_ERROR(mr32(p, ir + V32_IMAN_OFF, &iman)))
                    return EFI_DEVICE_ERROR;
                if (iman & V32_IMAN_IP) {
                    if (EFI_ERROR(mw32(p, ir + V32_IMAN_OFF,
                                       iman | V32_IMAN_IP)))
                        return EFI_DEVICE_ERROR;
                }
            }
            return EFI_SUCCESS;
        }
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS v32_expect_command(EFI_PCI_IO_PROTOCOL *p, UINT32 ir,
                                     struct dma_obj *ev, UINTN *ev_index,
                                     UINT8 *ev_cycle, UINT64 command_addr,
                                     UINT32 expected_slot, UINT32 *slot)
{
    UINT32 type, dw0, dw2, dw3;
    UINT64 ptr;
    EFI_STATUS s = v32_poll_event(p, ir, ev, ev_index, ev_cycle,
                                  &type, &dw0, &dw2, &dw3, &ptr);
    if (EFI_ERROR(s)) return s;
    if (type != V32_TRB_COMMAND_EVENT || ptr != command_addr ||
        ((dw2 >> 24) & 0xffU) != V32_CC_SUCCESS)
        return EFI_DEVICE_ERROR;
    if (expected_slot && ((dw3 >> 24) & 0xffU) != expected_slot)
        return EFI_DEVICE_ERROR;
    if (slot) *slot = (dw3 >> 24) & 0xffU;
    return EFI_SUCCESS;
}

static EFI_STATUS v32_start(EFI_PCI_IO_PROTOCOL *p, UINT32 op,
                            UINT32 *reads, UINT32 *writes)
{
    UINT32 cmd, sts;
    EFI_STATUS s = mr32(p, op, &cmd);
    (*reads)++;
    if (EFI_ERROR(s)) return s;
    cmd &= ~(V32_CMD_INTE | V32_CMD_HSEE);
    cmd |= V32_CMD_RUN;
    s = mw32(p, op, cmd); (*writes)++;
    if (EFI_ERROR(s)) return s;
    s = wait_hch(p, op, FALSE, 1000U, &sts, reads);
    return s;
}

static EFI_STATUS v32_halt_and_reset(EFI_PCI_IO_PROTOCOL *p, UINT32 op,
                                     UINT32 *reads, UINT32 *writes)
{
    UINT32 cmd, sts;
    EFI_STATUS s = mr32(p, op, &cmd); (*reads)++;
    if (EFI_ERROR(s)) return s;
    cmd &= ~(V32_CMD_RUN | V32_CMD_INTE | V32_CMD_HSEE);
    s = mw32(p, op, cmd); (*writes)++;
    if (EFI_ERROR(s)) return s;
    s = wait_hch(p, op, TRUE, 1000U, &sts, reads);
    if (EFI_ERROR(s)) return s;
    s = reset_xhci(p, op, &cmd, &sts, reads, writes);
    if (EFI_ERROR(s)) return s;
    return (sts & V32_STS_HCH) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    V32_HANDOFF h;
    EFI_HANDLE controller = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_STATUS s = EFI_SUCCESS;
    UINT64 saved_flags = 0, pci_original = 0, pci_enabled = 0;
    BOOLEAN interrupts_disabled = FALSE, pci_changed = FALSE;
    BOOLEAN running = FALSE, halted = FALSE, dma_live = FALSE;
    UINT32 cap = 0, ver = 0, hcs1 = 0, hcs2 = 0, hcc = 0;
    UINT32 op = 0, db = 0, rt = 0, ir = 0, pagesize = 0;
    UINT32 reads = 0, writes = 0, slot_type = 0, slot = 0;
    UINT32 portsc = 0, speed = 0;
    UINTN shift = 0, xpage = 0, scratchpads = 0, scratch_pages = 1;
    UINTN cmd_index = 0, ev_index = 0;
    UINT8 cmd_cycle = 1, ev_cycle = 1;
    UINT32 *cmd_ring = NULL;
    UINT64 *dcbaa = NULL, *scratch_array = NULL, *erst = NULL;
    UINT8 *inctx = NULL, *outctx = NULL;
    struct dma_obj dcbaa_d = {0}, spa_d = {0}, cr_d = {0}, ev_d = {0};
    struct dma_obj erst_d = {0}, inctx_d = {0}, outctx_d = {0}, ep0_d = {0};
    struct dma_obj *scratch = NULL;
    UINT64 command_addr = 0;
    UINT32 ctxsz = 32;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V32 / CLEAN GATE 6 REIMPLEMENTATION\r\n");
    Print(u"UEFI DISCOVERY -> DISCONNECT -> FRESH xHCI -> PORT RESET -> ENABLE SLOT -> ADDRESS DEVICE\r\n");
    Print(u"KEYBOARD + OPTIONAL MOUSE HANDOFF / NO BRIDGE USB DISCOVERY / NO USB TRANSFER\r\n");

    /* Phase A: UEFI is the sole USB discovery provider. */
    s = v32_produce_handoff(image, &h, &controller);
    if (EFI_ERROR(s)) { v32_fail(u"DISCOVERY", u"HANDOFF", s); goto out; }
    s = v32_validate_handoff(&h);
    if (EFI_ERROR(s)) { v32_fail(u"HANDOFF", u"VALIDATE", s); goto out; }
    Print(u"HANDOFF: DEVICES=%u KEYBOARD PORT=%u VID=%04x PID=%04x\r\n",
          h.device_count, h.keyboard.root_port, h.keyboard.vendor_id,
          h.keyboard.product_id);

    /* Validate the exact controller path before releasing UEFI ownership. */
    {
        EFI_DEVICE_PATH_PROTOCOL *cp = NULL;
        s = uefi_call_wrapper(BS->OpenProtocol, 6, controller,
                              &DevicePathProtocol, (VOID **)&cp,
                              image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(s) || !v32_path_equal(&h.controller_path, cp)) {
            if (!EFI_ERROR(s)) s = EFI_DEVICE_ERROR;
            v32_fail(u"HANDOFF", u"CONTROLLER PATH", s);
            goto out;
        }
    }

    s = uefi_call_wrapper(BS->DisconnectController, 3, controller, NULL, NULL);
    if (EFI_ERROR(s)) { v32_fail(u"HANDOFF", u"DISCONNECT CONTROLLER", s); goto out; }
    Print(u"UEFI USB STACK: DISCONNECTED\r\n");

    saved_flags = v32_disable_interrupts();
    interrupts_disabled = TRUE;

    /* Phase B: bind only to the controller handle selected above. */
    s = v32_bind_controller(controller, &h, &p);
    if (EFI_ERROR(s)) { v32_fail(u"BIND", u"PCI ID", s); goto out; }
    s = v32_pci_attributes(p, &pci_original, &pci_enabled);
    if (EFI_ERROR(s)) { v32_fail(u"BIND", u"PCI ATTRIBUTES", s); goto out; }
    pci_changed = pci_enabled != 0;

    s = cfg32(p, 0x10, &cap); /* temporary BAR read */
    if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"BAR", s); goto out; }
    if (cap & 1U) { s = EFI_UNSUPPORTED; v32_fail(u"CAPS", u"MMIO BAR", s); goto out; }
    {
        UINT32 bar_hi = 0;
        UINT32 type = (cap >> 1) & 3U;
        if (type == 2U) {
            s = cfg32(p, 0x14, &bar_hi);
            if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"BAR HIGH", s); goto out; }
        }
        Print(u"PCI: BAR=%08x/%08x TYPE=%u\r\n", cap, bar_hi, type);
    }

    s = mr32(p, 0, &cap); reads++;
    if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"CAPLENGTH", s); goto out; }
    op = cap & 0xffU;
    s = mr16(p, 2, (UINT16 *)&ver); reads++;
    if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"HCIVERSION", s); goto out; }
    if ((ver & 0xffffU) < 0x0100U) {
        s = EFI_UNSUPPORTED; v32_fail(u"CAPS", u"xHCI VERSION", s); goto out;
    }
    s = mr32(p, 4, &hcs1); reads++;
    if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"HCSPARAMS1", s); goto out; }
    s = mr32(p, 8, &hcs2); reads++;
    if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"HCSPARAMS2", s); goto out; }
    s = mr32(p, 0x10, &hcc); reads++;
    if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"HCCPARAMS1", s); goto out; }
    s = mr32(p, op + 8U, &pagesize); reads++;
    if (EFI_ERROR(s) || !pagesize) { s = EFI_UNSUPPORTED; v32_fail(u"CAPS", u"PAGESIZE", s); goto out; }
    while (shift < 32U && !(pagesize & (1U << shift))) shift++;
    if (shift >= 32U) { s = EFI_UNSUPPORTED; v32_fail(u"CAPS", u"PAGE BIT", s); goto out; }
    xpage = (UINTN)1U << (12U + shift);
    if (xpage > 0x100000U) { s = EFI_UNSUPPORTED; v32_fail(u"CAPS", u"PAGE SIZE", s); goto out; }
    scratch_pages = xpage / 4096U;
    if (!scratch_pages) scratch_pages = 1;
    if (hcc & V32_HCC_CTXSZ) ctxsz = 64;
    scratchpads = (((hcs2 >> 21) & 31U) << 5) | ((hcs2 >> 27) & 31U);
    if (!(hcc & V32_HCC_AC64)) {
        /* AC64=0 is legal, but every returned DMA address must remain below 4G. */
        Print(u"CAPS: AC64=0 / DMA MUST REMAIN BELOW 4G\r\n");
    }
    Print(u"xHCI VERSION=%u.%02u SLOTS=%u SCRATCHPADS=%u AC64=%u CONTEXT=%u PAGESIZE=%u\r\n",
          (ver >> 8) & 0xffU, ver & 0xffU, hcs1 & 0xffU, scratchpads,
          (hcc & V32_HCC_AC64) ? 1U : 0U, ctxsz, (UINT32)xpage);

    s = mr32(p, 0x14, &db); reads++;
    if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"DBOFF", s); goto out; }
    db &= ~3U;
    s = mr32(p, 0x18, &rt); reads++;
    if (EFI_ERROR(s)) { v32_fail(u"CAPS", u"RTSOFF", s); goto out; }
    rt &= ~31U;
    ir = rt + 0x20U;

    s = v32_halt_and_reset(p, op, &reads, &writes);
    if (EFI_ERROR(s)) { v32_fail(u"RESET", u"HALT RESET", s); goto out; }
    halted = TRUE;

    /* Allocate every controller-referenced object through the proven DMA path. */
    s = dma_alloc(p, 1, &dcbaa_d); if (EFI_ERROR(s)) { v32_fail(u"DMA", u"DCBAA", s); goto out; }
    scratch_pages = (scratchpads * sizeof(UINT64) + 4095U) / 4096U;
    if (!scratch_pages) scratch_pages = 1;
    s = dma_alloc(p, scratch_pages, &spa_d); if (EFI_ERROR(s)) { v32_fail(u"DMA", u"SCRATCH ARRAY", s); goto out; }
    s = dma_alloc(p, 1, &cr_d); if (EFI_ERROR(s)) { v32_fail(u"DMA", u"COMMAND RING", s); goto out; }
    s = dma_alloc(p, 1, &ev_d); if (EFI_ERROR(s)) { v32_fail(u"DMA", u"EVENT RING", s); goto out; }
    s = dma_alloc(p, 1, &erst_d); if (EFI_ERROR(s)) { v32_fail(u"DMA", u"ERST", s); goto out; }
    s = dma_alloc(p, 1, &inctx_d); if (EFI_ERROR(s)) { v32_fail(u"DMA", u"INPUT CONTEXT", s); goto out; }
    s = dma_alloc(p, 1, &outctx_d); if (EFI_ERROR(s)) { v32_fail(u"DMA", u"OUTPUT CONTEXT", s); goto out; }
    s = dma_alloc(p, 1, &ep0_d); if (EFI_ERROR(s)) { v32_fail(u"DMA", u"EP0 RING", s); goto out; }
    dma_live = TRUE;

    if ((dcbaa_d.dev & 63ULL) || (inctx_d.dev & 63ULL) ||
        (outctx_d.dev & 63ULL) || (ep0_d.dev & 15ULL) || (cr_d.dev & 63ULL) ||
        (ev_d.dev & 63ULL) || (erst_d.dev & 63ULL)) {
        s = EFI_BAD_BUFFER_SIZE; v32_fail(u"DMA", u"ALIGNMENT", s); goto out;
    }
    if (!(hcc & V32_HCC_AC64)) {
        struct dma_obj *ds[] = { &dcbaa_d, &spa_d, &cr_d, &ev_d, &erst_d,
                                 &inctx_d, &outctx_d, &ep0_d };
        UINTN k;
        for (k = 0; k < sizeof(ds)/sizeof(ds[0]); ++k) {
            if (ds[k]->dev >> 32) {
                s = EFI_UNSUPPORTED; v32_fail(u"DMA", u"AC64 ADDRESS", s); goto out;
            }
        }
    }

    if (scratchpads) {
        s = uefi_call_wrapper(BS->AllocatePool, 3, EfiBootServicesData,
                              scratchpads * sizeof(struct dma_obj),
                              (VOID **)&scratch);
        if (EFI_ERROR(s)) { v32_fail(u"DMA", u"SCRATCH DESCRIPTORS", s); goto out; }
        uefi_call_wrapper(BS->SetMem, 3, scratch,
                          scratchpads * sizeof(struct dma_obj), 0);
        for (UINTN i = 0; i < scratchpads; ++i) {
            s = dma_alloc(p, scratch_pages, &scratch[i]);
            if (EFI_ERROR(s)) { v32_fail(u"DMA", u"SCRATCHPAD", s); goto out; }
            if (scratch[i].dev & ((UINT64)xpage - 1ULL)) {
                s = EFI_BAD_BUFFER_SIZE; v32_fail(u"DMA", u"SCRATCH ALIGN", s); goto out;
            }
            if (!(hcc & V32_HCC_AC64) && (scratch[i].dev >> 32)) {
                s = EFI_UNSUPPORTED; v32_fail(u"DMA", u"SCRATCH AC64", s); goto out;
            }
        }
    }

    dcbaa = (UINT64 *)dcbaa_d.host;
    scratch_array = (UINT64 *)spa_d.host;
    cmd_ring = (UINT32 *)cr_d.host;
    erst = (UINT64 *)erst_d.host;
    inctx = (UINT8 *)inctx_d.host;
    outctx = (UINT8 *)outctx_d.host;

    if (scratchpads) {
        for (UINTN i = 0; i < scratchpads; ++i)
            scratch_array[i] = scratch[i].dev;
        dcbaa[0] = spa_d.dev;
    }

    /* Command ring: one segment, self-link at the final TRB. */
    uefi_call_wrapper(BS->SetMem, 3, cmd_ring, 4096U, 0);
    cmd_ring[1020] = (UINT32)cr_d.dev;
    cmd_ring[1021] = (UINT32)(cr_d.dev >> 32);
    cmd_ring[1022] = 0;
    cmd_ring[1023] = (V32_TRB_LINK << 10) | 0x2U | 0x1U;

    /* Event ring segment table: one 16-TRB segment. */
    uefi_call_wrapper(BS->SetMem, 3, ev_d.host, 4096U, 0);
    erst[0] = ev_d.dev;
    erst[1] = V32_EVENT_TRBS;
    erst[2] = 0;
    erst[3] = 0;
    __sync_synchronize();

    s = mw64(p, op + V32_DCBAAP_OFF, dcbaa_d.dev); writes += 2;
    if (EFI_ERROR(s)) { v32_fail(u"INIT", u"DCBAAP", s); goto out; }
    s = mw64(p, op + V32_CRCR_OFF, cr_d.dev | 1ULL); writes += 2;
    if (EFI_ERROR(s)) { v32_fail(u"INIT", u"CRCR", s); goto out; }
    s = mw32(p, op + V32_CONFIG_OFF, hcs1 & 0xffU); writes++;
    if (EFI_ERROR(s)) { v32_fail(u"INIT", u"CONFIG", s); goto out; }
    s = mw32(p, ir + V32_IMAN_OFF, 0); writes++;
    if (EFI_ERROR(s)) { v32_fail(u"INIT", u"IMAN", s); goto out; }
    s = mw32(p, ir + 0x24U, 0); writes++;
    if (EFI_ERROR(s)) { v32_fail(u"INIT", u"IMOD", s); goto out; }
    s = mw32(p, ir + V32_ERSTSZ_OFF, 1); writes++;
    if (EFI_ERROR(s)) { v32_fail(u"INIT", u"ERSTSZ", s); goto out; }
    s = mw64(p, ir + V32_ERSTBA_OFF, erst_d.dev); writes += 2;
    if (EFI_ERROR(s)) { v32_fail(u"INIT", u"ERSTBA", s); goto out; }
    s = mw64(p, ir + V32_ERDP_OFF, ev_d.dev); writes += 2;
    if (EFI_ERROR(s)) { v32_fail(u"INIT", u"ERDP", s); goto out; }

    s = v32_start(p, op, &reads, &writes);
    if (EFI_ERROR(s)) { v32_fail(u"START", u"RUN", s); goto out; }
    running = TRUE;
    halted = FALSE;

    /* Phase C: the only port considered is the UEFI-selected keyboard port. */
    {
        UINT32 port = h.keyboard.root_port;
        UINT32 slot_cap;
        UINT32 poff = op + V32_PORTSC_BASE + (port - 1U) * 0x10U;
        s = v32_protocol_slot_type(p, hcc, port, &slot_cap, &reads);
        if (EFI_ERROR(s)) { v32_fail(u"PORT", u"SUPPORTED PROTOCOL", s); goto out; }
        s = mr32(p, poff, &portsc); reads++;
        if (EFI_ERROR(s)) { v32_fail(u"PORT", u"PORTSC", s); goto out; }
        if (!(portsc & V32_PORT_CCS)) { s = EFI_NOT_FOUND; v32_fail(u"PORT", u"NOT CONNECTED", s); goto out; }
        speed = (portsc & V32_PORT_SPEED_MASK) >> V32_PORT_SPEED_SHIFT;
        if (speed != V32_USB_SPEED_LOW && speed != V32_USB_SPEED_FULL) {
            s = EFI_UNSUPPORTED; v32_fail(u"PORT", u"USB2 SPEED", s); goto out;
        }
        Print(u"PORT=%u SLOT-TYPE=%u LIVE-SPEED=%u\r\n", port, slot_cap, speed);

        /* A stale PRC must be acknowledged before the new reset. */
        if (portsc & V32_PORT_PRC) {
            s = mw32(p, poff, V32_PORT_PRC); writes++;
            if (EFI_ERROR(s)) { v32_fail(u"PORT", u"CLEAR STALE PRC", s); goto out; }
        }
        s = mw32(p, poff, v32_port_write_base(portsc) | V32_PORT_PR); writes++;
        if (EFI_ERROR(s)) { v32_fail(u"PORT", u"PORT RESET", s); goto out; }

        {
            UINT32 type, dw0, dw2, dw3;
            UINT64 ptr;
            s = v32_poll_event(p, ir, &ev_d, &ev_index, &ev_cycle,
                               &type, &dw0, &dw2, &dw3, &ptr);
            if (EFI_ERROR(s)) { v32_fail(u"PORT", u"RESET EVENT", s); goto out; }
            if (type != V32_TRB_PORT_STATUS) {
                s = EFI_DEVICE_ERROR; v32_fail(u"PORT", u"UNEXPECTED EVENT", s); goto out;
            }
            if (((dw3 >> 24) & 0xffU) != port) {
                s = EFI_DEVICE_ERROR; v32_fail(u"PORT", u"RESET PORT ID", s); goto out;
            }
        }
        s = mr32(p, poff, &portsc); reads++;
        if (EFI_ERROR(s)) { v32_fail(u"PORT", u"POST RESET PORTSC", s); goto out; }
        speed = (portsc & V32_PORT_SPEED_MASK) >> V32_PORT_SPEED_SHIFT;
        if (!(portsc & V32_PORT_CCS) || (portsc & V32_PORT_PR) ||
            !(portsc & V32_PORT_PED) ||
            ((portsc & V32_PORT_PLS_MASK) >> 5) != V32_PORT_PLS_U0 ||
            !(portsc & V32_PORT_PRC) ||
            (speed != V32_USB_SPEED_LOW && speed != V32_USB_SPEED_FULL)) {
            s = EFI_DEVICE_ERROR; v32_fail(u"PORT", u"POST RESET STATE", s); goto out;
        }
        s = mw32(p, poff, V32_PORT_PRC); writes++;
        if (EFI_ERROR(s)) { v32_fail(u"PORT", u"CLEAR RESET CHANGE", s); goto out; }

        /* Enable Slot. */
        cmd_ring[cmd_index * 4U + 0U] = 0;
        cmd_ring[cmd_index * 4U + 1U] = 0;
        cmd_ring[cmd_index * 4U + 2U] = 0;
        cmd_ring[cmd_index * 4U + 3U] = (V32_TRB_ENABLE_SLOT << 10) |
                                          ((slot_cap & 0x1fU) << 16) |
                                          cmd_cycle;
        __sync_synchronize();
        command_addr = v32_trb_ptr(&cr_d, cmd_index);
        s = mw32(p, db + V32_DB0_OFF, 0); writes++;
        if (EFI_ERROR(s)) { v32_fail(u"ENABLE SLOT", u"DOORBELL", s); goto out; }
        s = v32_expect_command(p, ir, &ev_d, &ev_index, &ev_cycle,
                                command_addr, 0, &slot);
        if (EFI_ERROR(s) || !slot) { if (!EFI_ERROR(s)) s = EFI_DEVICE_ERROR; v32_fail(u"ENABLE SLOT", u"COMPLETION", s); goto out; }
        Print(u"ENABLE SLOT: SLOT=%u PASS\r\n", slot);
        cmd_index++;

        /* Fresh Input/Output contexts and EP0 ring. */
        uefi_call_wrapper(BS->SetMem, 3, inctx, 4096U, 0);
        uefi_call_wrapper(BS->SetMem, 3, outctx, 4096U, 0);
        uefi_call_wrapper(BS->SetMem, 3, ep0_d.host, 4096U, 0);
        {
            UINT32 *ic = (UINT32 *)inctx;
            UINT32 *slotc = (UINT32 *)(inctx + ctxsz);
            UINT32 *epc = (UINT32 *)(inctx + ctxsz * 2U);
            UINT32 speed_now = (portsc & V32_PORT_SPEED_MASK) >> V32_PORT_SPEED_SHIFT;
            UINT32 ep_dw1 = (3U << 1) | (V32_EP_TYPE_CONTROL << 3) | (8U << 16);
            UINT64 ep_ring = ep0_d.dev | 1ULL;

            ic[0] = 0;
            ic[1] = V32_CTX_SLOT_ADD | V32_CTX_EP0_ADD;
            slotc[0] = (speed_now << 20) | (1U << 27);
            slotc[1] = port << 16;
            slotc[2] = 0;
            slotc[3] = 0;
            epc[0] = 0;
            epc[1] = ep_dw1;
            epc[2] = (UINT32)ep_ring;
            epc[3] = (UINT32)(ep_ring >> 32);
            epc[4] = 8U;
            epc[5] = 0;
        }
        {
            UINT32 *r = (UINT32 *)ep0_d.host;
            r[0] = 0; r[1] = 0; r[2] = 0; r[3] = 0;
        }
        dcbaa[slot] = outctx_d.dev;
        __sync_synchronize();

        /* Address Device. */
        cmd_ring[cmd_index * 4U + 0U] = (UINT32)inctx_d.dev;
        cmd_ring[cmd_index * 4U + 1U] = (UINT32)(inctx_d.dev >> 32);
        cmd_ring[cmd_index * 4U + 2U] = 0;
        cmd_ring[cmd_index * 4U + 3U] = (V32_TRB_ADDRESS_DEVICE << 10) |
                                          (slot << 24) | cmd_cycle;
        __sync_synchronize();
        command_addr = v32_trb_ptr(&cr_d, cmd_index);
        s = mw32(p, db + V32_DB0_OFF, 0); writes++;
        if (EFI_ERROR(s)) { v32_fail(u"ADDRESS DEVICE", u"DOORBELL", s); goto out; }
        s = v32_expect_command(p, ir, &ev_d, &ev_index, &ev_cycle,
                                command_addr, slot, NULL);
        if (EFI_ERROR(s)) { v32_fail(u"ADDRESS DEVICE", u"COMPLETION", s); goto out; }

        {
            UINT32 *os = (UINT32 *)outctx;
            UINT32 state = (os[3] >> 27) & 0x1fU;
            UINT32 address = os[3] & 0xffU;
            if (state != V32_SLOT_STATE_ADDRESSED || address == 0) {
                s = EFI_DEVICE_ERROR; v32_fail(u"ADDRESS DEVICE", u"OUTPUT SLOT STATE", s); goto out;
            }
        }
        Print(u"ADDRESS DEVICE: SLOT=%u STATE=ADDRESSED PASS\r\n", slot);
    }

    /* Gate 6 ends immediately after Address Device. Safe teardown follows. */
    s = v32_halt_and_reset(p, op, &reads, &writes);
    if (EFI_ERROR(s)) {
        v32_fail(u"TEARDOWN", u"HALT RESET", s);
        fatal_running();
    }
    running = FALSE;
    halted = TRUE;

    /* Remove every controller reference before releasing DMA. */
    (void)mw64(p, op + V32_CRCR_OFF, 0);
    (void)mw64(p, op + V32_DCBAAP_OFF, 0);
    (void)mw32(p, op + V32_CONFIG_OFF, 0);
    (void)mw64(p, ir + V32_ERSTBA_OFF, 0);
    (void)mw64(p, ir + V32_ERDP_OFF, 0);
    (void)mw32(p, ir + V32_ERSTSZ_OFF, 0);

out:
    if (running) {
        /* Never release controller-referenced DMA while RUN state is uncertain. */
        fatal_running();
    }
    if (p && halted) {
        (void)mw64(p, op + V32_CRCR_OFF, 0);
        (void)mw64(p, op + V32_DCBAAP_OFF, 0);
        (void)mw32(p, op + V32_CONFIG_OFF, 0);
        (void)mw64(p, ir + V32_ERSTBA_OFF, 0);
        (void)mw64(p, ir + V32_ERDP_OFF, 0);
        (void)mw32(p, ir + V32_ERSTSZ_OFF, 0);
    }

    if (dma_live && p && halted) {
        if (scratch) {
            for (UINTN i = 0; i < scratchpads; ++i)
                (void)dma_free(p, &scratch[i]);
            FreePool(scratch);
            scratch = NULL;
        }
        (void)dma_free(p, &ep0_d);
        (void)dma_free(p, &outctx_d);
        (void)dma_free(p, &inctx_d);
        (void)dma_free(p, &erst_d);
        (void)dma_free(p, &ev_d);
        (void)dma_free(p, &cr_d);
        (void)dma_free(p, &spa_d);
        (void)dma_free(p, &dcbaa_d);
        dma_live = FALSE;
    }
    if (p && pci_changed && halted)
        (void)uefi_call_wrapper(p->Attributes, 4, p,
                                 EfiPciIoAttributeOperationDisable,
                                 pci_enabled, NULL);
    if (interrupts_disabled) {
        v32_restore_interrupts(saved_flags);
        interrupts_disabled = FALSE;
    }
    Print(u"MMIO READS=%u WRITES=%u RESULT=%r\r\n", reads, writes, s);
    uefi_call_wrapper(BS->Stall, 1, 30000000);
    return s;
}
