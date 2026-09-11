#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include "usb_io_compat.h"

#define XHCI_MIN_VERSION 0x0100U
#define XHCI_CLASS 0x0cU
#define XHCI_SUBCLASS 0x03U
#define XHCI_PROG_IF 0x30U

#define CMD_RUN 0x00000001U
#define CMD_RESET 0x00000002U
#define CMD_INTE 0x00000004U
#define CMD_HSEE 0x00000008U
#define STS_HCH 0x00000001U
#define STS_CNR 0x00000800U
#define HCC_AC64 0x00000001U
#define HCC_CTXSZ 0x00000004U

#define IMAN_IP 0x00000001U
#define IMAN_IE 0x00000002U
#define CRCR_RCS 0x00000001ULL

#define TRB_CYCLE 0x00000001U
#define TRB_TYPE_SHIFT 10U
#define TRB_TYPE_MASK 0x0000fc00U
#define TRB_ENABLE_SLOT 9U
#define TRB_ADDRESS_DEVICE 11U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 0x00000002U
#define TRB_COMMAND_COMPLETION 33U
#define TRB_PORT_STATUS_CHANGE 34U
#define CC_SUCCESS 1U

#define SLOT_ID_SHIFT 24U
#define ROOT_HUB_PORT_SHIFT 16U
#define DEV_SPEED_SHIFT 20U
#define LAST_CTX_SHIFT 27U
#define SLOT_STATE_SHIFT 27U
#define SLOT_STATE_ADDRESSED 2U
#define INPUT_SLOT_FLAG 0x00000001U
#define INPUT_EP0_FLAG 0x00000002U

#define EP_CERR_SHIFT 1U
#define EP_TYPE_SHIFT 3U
#define EP_TYPE_CONTROL 4U
#define EP0_DCS 0x00000001U
#define EP0_MPS_SHIFT 16U

#define PORTSC_BASE 0x400U
#define PORT_CCS (1U << 0)
#define PORT_PED (1U << 1)
#define PORT_PR (1U << 4)
#define PORT_PLS_MASK (0xFU << 5)
#define PORT_POWER (1U << 9)
#define PORT_SPEED_MASK (0xFU << 10)
#define PORT_CSC (1U << 17)
#define PORT_WRC (1U << 19)
#define PORT_PRC (1U << 21)
#define PORT_WR (1U << 31)

/* Linux/xHCI neutral PORTSC write masks: preserve RO and persistent RW bits,
 * and write only the explicitly requested RW1S/RW1C operation in addition. */
#define PORT_RO ((1U << 0) | (1U << 3) | (0xFU << 10) | (1U << 30))
#define PORT_RWS ((0xFU << 5) | (1U << 9) | (3U << 14) | (7U << 25))

#define CMD_TRBS 256U
#define EVENT_TRBS 16U
#define MAX_SCRATCHPADS 1024U
#define MAX_EVENTS_TO_DRAIN 8U
#define EFI_PAGE_BYTES 4096U

#define USB_CLASS_HID 3U
#define HID_SUBCLASS_BOOT 1U
#define HID_PROTOCOL_KEYBOARD 1U
#define DP_TYPE_HARDWARE 1U
#define DP_SUBTYPE_PCI 1U
#define DP_TYPE_MESSAGING 3U
#define DP_SUBTYPE_USB 5U
#define DP_TYPE_END 0x7FU
#define PCI_DEVICE_NODE_BYTES 6U

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID UsbIoGuid = EFI_USB_IO_PROTOCOL_GUID;

static const CHAR16 *fail_stage = u"NONE";
static const CHAR16 *fail_op = u"NONE";
static EFI_STATUS fail_status = EFI_SUCCESS;

struct dma_obj {
    VOID *host;
    VOID *map;
    EFI_PHYSICAL_ADDRESS dev;
    UINTN pages;
    BOOLEAN live;
};

struct discovery {
    UINT8 port;
    UINT8 protocol_major;
    UINT8 interface_number;
    UINT8 endpoint;
    UINT8 interval;
    UINT16 mps;
    UINT16 ep0_mps;
    UINT16 vid;
    UINT16 pid;
    UINT8 config;
};

static void remember_fail(const CHAR16 *stage, const CHAR16 *op, EFI_STATUS s)
{
    if (!EFI_ERROR(fail_status)) {
        fail_stage = stage;
        fail_op = op;
        fail_status = s;
    }
}

static EFI_STATUS cfg32(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT32 *v)
{
    return uefi_call_wrapper(p->Pci.Read, 5, p, EfiPciIoWidthUint32, o, 1, v);
}

static EFI_STATUS mr32(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT32 *v)
{
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32, 0,
                             (UINT64)o, 1, v);
}

static EFI_STATUS mr16(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT16 *v)
{
    return uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint16, 0,
                             (UINT64)o, 1, v);
}

static EFI_STATUS mw32(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT32 v)
{
    return uefi_call_wrapper(p->Mem.Write, 6, p, EfiPciIoWidthUint32, 0,
                             (UINT64)o, 1, &v);
}

static EFI_STATUS mr64(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT64 *v)
{
    UINT32 lo, hi;
    EFI_STATUS s = mr32(p, o, &lo);
    if (EFI_ERROR(s)) return s;
    s = mr32(p, o + 4, &hi);
    if (EFI_ERROR(s)) return s;
    *v = ((UINT64)hi << 32) | lo;
    return EFI_SUCCESS;
}

static EFI_STATUS mw64(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT64 v)
{
    EFI_STATUS s = mw32(p, o, (UINT32)v);
    if (EFI_ERROR(s)) return s;
    return mw32(p, o + 4, (UINT32)(v >> 32));
}

static EFI_STATUS dma_alloc(EFI_PCI_IO_PROTOCOL *p, UINTN pages,
                            struct dma_obj *d)
{
    EFI_STATUS s;
    UINTN bytes, mapped;

    d->host = NULL;
    d->map = NULL;
    d->dev = 0;
    d->pages = pages;
    d->live = FALSE;

    if (!pages || pages > ((UINTN)-1) / EFI_PAGE_BYTES)
        return EFI_BAD_BUFFER_SIZE;

    bytes = pages * EFI_PAGE_BYTES;
    s = uefi_call_wrapper(p->AllocateBuffer, 6, p, AllocateAnyPages,
                          EfiBootServicesData, pages, &d->host, 0);
    if (EFI_ERROR(s)) return s;

    s = uefi_call_wrapper(BS->SetMem, 3, d->host, bytes, 0);
    if (EFI_ERROR(s)) {
        uefi_call_wrapper(p->FreeBuffer, 3, p, pages, d->host);
        d->host = NULL;
        return s;
    }

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

    if (d->dev & 0x3fULL) {
        uefi_call_wrapper(p->Unmap, 2, p, d->map);
        uefi_call_wrapper(p->FreeBuffer, 3, p, pages, d->host);
        d->host = NULL;
        d->map = NULL;
        d->dev = 0;
        return EFI_BAD_BUFFER_SIZE;
    }

    d->live = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS dma_free(EFI_PCI_IO_PROTOCOL *p, struct dma_obj *d)
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

static EFI_STATUS wait_hch(EFI_PCI_IO_PROTOCOL *p, UINT32 op, BOOLEAN halted,
                           UINTN loops, UINT32 *st)
{
    UINTN i;
    EFI_STATUS s;
    UINT32 want = halted ? STS_HCH : 0;

    for (i = 0; i < loops; ++i) {
        s = mr32(p, op + 4, st);
        if (EFI_ERROR(s)) return s;
        if ((*st & STS_HCH) == want) return EFI_SUCCESS;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS reset_xhci(EFI_PCI_IO_PROTOCOL *p, UINT32 op,
                             UINT32 *cmd, UINT32 *st)
{
    EFI_STATUS s;
    UINTN i;

    s = mr32(p, op, cmd);
    if (EFI_ERROR(s)) return s;
    *cmd &= ~(CMD_RUN | CMD_INTE | CMD_HSEE);
    *cmd |= CMD_RESET;
    s = mw32(p, op, *cmd);
    if (EFI_ERROR(s)) return s;

    for (i = 0; i < 1000; ++i) {
        s = mr32(p, op, cmd);
        if (EFI_ERROR(s)) return s;
        if (!(*cmd & CMD_RESET)) break;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    if (*cmd & CMD_RESET) return EFI_TIMEOUT;

    for (i = 0; i < 10000; ++i) {
        s = mr32(p, op + 4, st);
        if (EFI_ERROR(s)) return s;
        if (!(*st & STS_CNR)) return EFI_SUCCESS;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS find_supported_protocol(EFI_PCI_IO_PROTOCOL *p, UINT32 hcc,
                                           UINT32 port, UINT8 *major,
                                           UINT8 *slot_type)
{
    UINT32 off, hdr, info, slot, next;
    UINTN i = 0;
    EFI_STATUS s;

    off = ((hcc >> 16) & 0xffffU) * 4U;
    while (off && i++ < 64U) {
        s = mr32(p, off, &hdr);
        if (EFI_ERROR(s)) return s;
        if ((hdr & 0xffU) == 2U) {
            s = mr32(p, off + 8U, &info);
            if (EFI_ERROR(s)) return s;
            s = mr32(p, off + 12U, &slot);
            if (EFI_ERROR(s)) return s;

            {
                UINT32 first = info & 0xffU;
                UINT32 count = (info >> 8) & 0xffU;
                if (count && port >= first && port < first + count) {
                    *major = (UINT8)((hdr >> 24) & 0xffU);
                    *slot_type = (UINT8)(slot & 0x1fU);
                    return EFI_SUCCESS;
                }
            }
        }
        next = ((hdr >> 8) & 0xffU) * 4U;
        if (!next) break;
        off += next;
    }
    return EFI_NOT_FOUND;
}

static BOOLEAN path_last_pci_bdf(EFI_DEVICE_PATH_PROTOCOL *path,
                                 UINT8 *device, UINT8 *function)
{
    UINT8 *p = (UINT8 *)path;
    BOOLEAN found = FALSE;

    while (p) {
        UINT16 len = (UINT16)p[2] | ((UINT16)p[3] << 8);
        if (len < 4 || len > 0xff) break;
        if (p[0] == DP_TYPE_END) break;
        if (p[0] == DP_TYPE_HARDWARE && p[1] == DP_SUBTYPE_PCI &&
            len >= PCI_DEVICE_NODE_BYTES) {
            *function = p[4];
            *device = p[5];
            found = TRUE;
        }
        p += len;
    }
    return found;
}

static UINT8 root_port(EFI_DEVICE_PATH_PROTOCOL *path, UINTN *usb_nodes)
{
    UINT8 *p = (UINT8 *)path;
    UINT8 port = 0;
    *usb_nodes = 0;

    while (p) {
        UINT16 len = (UINT16)p[2] | ((UINT16)p[3] << 8);
        if (len < 4 || len > 0xff) break;
        if (p[0] == DP_TYPE_END) break;
        if (p[0] == DP_TYPE_MESSAGING && p[1] == DP_SUBTYPE_USB && len >= 6) {
            ++(*usb_nodes);
            if (*usb_nodes == 1U) port = p[4];
        }
        p += len;
    }
    return port;
}

/* UEFI discovery is intentionally read-only. It supplies a constrained
 * keyboard snapshot used to select the physical root port and validate the
 * device after fresh xHCI initialization. */
static EFI_STATUS discover_keyboard(struct discovery *d, EFI_HANDLE image)
{
    EFI_HANDLE *hs = NULL;
    UINTN n = 0, i, found = 0;
    EFI_STATUS s;

    s = LibLocateHandle(ByProtocol, &UsbIoGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) return EFI_NOT_FOUND;

    for (i = 0; i < n; ++i) {
        EFI_USB_IO_PROTOCOL *usb = NULL;
        EFI_USB_INTERFACE_DESCRIPTOR in;
        EFI_USB_DEVICE_DESCRIPTOR dd;
        EFI_USB_CONFIG_DESCRIPTOR cd;
        EFI_USB_ENDPOINT_DESCRIPTOR ep;
        EFI_DEVICE_PATH_PROTOCOL *path;
        UINTN j, usb_nodes = 0;
        UINT8 port;

        if (EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol, 6, hs[i],
                                         &UsbIoGuid, (void **)&usb,
                                         image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL)))
            continue;
        if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetInterfaceDescriptor, 3,
                                         usb, &in)))
            continue;
        if (in.InterfaceClass != USB_CLASS_HID ||
            in.InterfaceSubClass != HID_SUBCLASS_BOOT ||
            in.InterfaceProtocol != HID_PROTOCOL_KEYBOARD)
            continue;
        if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetDeviceDescriptor, 2,
                                         usb, &dd)))
            continue;
        if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetConfigDescriptor, 2,
                                         usb, &cd)))
            continue;

        path = DevicePathFromHandle(hs[i]);
        port = root_port(path, &usb_nodes);
        if (!port || usb_nodes != 1U)
            continue;

        d->port = port;
        d->interface_number = in.InterfaceNumber;
        d->vid = dd.IdVendor;
        d->pid = dd.IdProduct;
        d->config = cd.ConfigurationValue;
        d->endpoint = 0;
        d->mps = 0;
        d->interval = 0;
        d->ep0_mps = dd.MaxPacketSize0;

        for (j = 0; j < in.NumEndpoints && j < 16U; ++j) {
            if (EFI_ERROR(uefi_call_wrapper(usb->UsbGetEndpointDescriptor,
                                             3, usb, (UINT8)j, &ep)))
                continue;
            if ((ep.EndpointAddress & 0x80U) &&
                ((ep.Attributes & 3U) == 3U)) {
                d->endpoint = ep.EndpointAddress;
                d->mps = ep.MaxPacketSize;
                d->interval = ep.Interval;
                break;
            }
        }

        if (!d->endpoint) continue;
        ++found;
        if (found > 1U) {
            uefi_call_wrapper(BS->FreePool, 1, hs);
            return EFI_ABORTED;
        }
    }

    if (hs) uefi_call_wrapper(BS->FreePool, 1, hs);
    return found == 1U ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static EFI_STATUS find_xhci(EFI_PCI_IO_PROTOCOL **out,
                            EFI_HANDLE *out_handle,
                            EFI_HANDLE image)
{
    EFI_HANDLE *hs = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    UINTN n = 0, i;
    UINT32 cls, id;
    EFI_STATUS s;

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) return s;

    for (i = 0; i < n; ++i) {
        EFI_PCI_IO_PROTOCOL *q = NULL;
        if (EFI_ERROR(uefi_call_wrapper(BS->OpenProtocol, 6, hs[i], &PciGuid,
                                         (void **)&q, image, NULL,
                                         EFI_OPEN_PROTOCOL_GET_PROTOCOL)))
            continue;
        if (EFI_ERROR(cfg32(q, 8, &cls)) || EFI_ERROR(cfg32(q, 0, &id)))
            continue;
        if (((cls >> 24) & 0xffU) == XHCI_CLASS &&
            ((cls >> 16) & 0xffU) == XHCI_SUBCLASS &&
            ((cls >> 8) & 0xffU) == XHCI_PROG_IF) {
            p = q;
            break;
        }
    }

    if (!p) {
        uefi_call_wrapper(BS->FreePool, 1, hs);
        return EFI_NOT_FOUND;
    }
    *out = p;
    *out_handle = hs[i];
    uefi_call_wrapper(BS->FreePool, 1, hs);
    return EFI_SUCCESS;
}

static EFI_STATUS xhci_port_read(EFI_PCI_IO_PROTOCOL *p, UINT32 op,
                                 UINT8 port, UINT32 *v)
{
    return mr32(p, op + PORTSC_BASE + ((UINT32)port - 1U) * 0x10U, v);
}

static UINT32 port_neutral(UINT32 x)
{
    return x & (PORT_RO | PORT_RWS);
}

static EFI_STATUS port_write_neutral(EFI_PCI_IO_PROTOCOL *p, UINT32 op,
                                     UINT8 port, UINT32 extra)
{
    UINT32 x;
    EFI_STATUS s = xhci_port_read(p, op, port, &x);
    if (EFI_ERROR(s)) return s;
    return mw32(p, op + PORTSC_BASE + ((UINT32)port - 1U) * 0x10U,
                port_neutral(x) | extra);
}

static EFI_STATUS event_wait(EFI_PCI_IO_PROTOCOL *p, UINT32 ir,
                             struct dma_obj *ev, UINTN *index,
                             BOOLEAN *cycle, UINTN loops,
                             UINT32 *d0, UINT32 *d1, UINT32 *d2,
                             UINT32 *d3)
{
    UINTN i;
    UINT32 *r = (UINT32 *)ev->host;

    for (i = 0; i < loops; ++i) {
        UINT32 w0 = r[*index * 4U + 0U];
        UINT32 w1 = r[*index * 4U + 1U];
        UINT32 w2 = r[*index * 4U + 2U];
        UINT32 w3 = r[*index * 4U + 3U];
        if ((w3 & TRB_CYCLE) == (*cycle ? TRB_CYCLE : 0U)) {
            *d0 = w0;
            *d1 = w1;
            *d2 = w2;
            *d3 = w3;
            *index += 1U;
            if (*index == EVENT_TRBS) {
                *index = 0;
                *cycle = !*cycle;
            }
            {
                UINT64 deq = ev->dev + (*index * 16U);
                EFI_STATUS s = mw64(p, ir + 0x18, deq & 0xfffffffffffffff0ULL);
                if (EFI_ERROR(s)) return s;
            }
            return EFI_SUCCESS;
        }
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS ack_interrupter(EFI_PCI_IO_PROTOCOL *p, UINT32 ir)
{
    UINT32 iman;
    EFI_STATUS s = mr32(p, ir, &iman);
    if (EFI_ERROR(s)) return s;
    if (iman & IMAN_IP)
        return mw32(p, ir, IMAN_IP);
    return EFI_SUCCESS;
}

static EFI_STATUS wait_command_completion(EFI_PCI_IO_PROTOCOL *p, UINT32 ir,
                                          struct dma_obj *ev, UINTN *ev_index,
                                          BOOLEAN *ev_cycle, UINT64 cmd_dev,
                                          UINT8 maxslots, UINT8 *slot_id,
                                          UINT32 *completion)
{
    UINT32 d0, d1, d2, d3, type, slot;
    UINT64 ptr;
    EFI_STATUS s;

    s = event_wait(p, ir, ev, ev_index, ev_cycle, 10000,
                   &d0, &d1, &d2, &d3);
    if (EFI_ERROR(s)) return s;
    type = (d3 & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
    ptr = ((UINT64)d1 << 32) | d0;
    slot = (d3 >> SLOT_ID_SHIFT) & 0xffU;
    *completion = (d2 >> 24) & 0xffU;
    if (type != TRB_COMMAND_COMPLETION || ptr != cmd_dev ||
        !slot || slot > maxslots)
        return EFI_DEVICE_ERROR;
    *slot_id = (UINT8)slot;
    s = ack_interrupter(p, ir);
    return s;
}

static EFI_STATUS drain_start_events(EFI_PCI_IO_PROTOCOL *p, UINT32 ir,
                                     struct dma_obj *ev, UINTN *ev_index,
                                     BOOLEAN *ev_cycle, UINT8 expected_port)
{
    UINTN n;
    UINT32 *r = (UINT32 *)ev->host;

    for (n = 0; n < MAX_EVENTS_TO_DRAIN; ++n) {
        UINT32 d3 = r[*ev_index * 4U + 3U];
        UINT32 d0, d1, d2;
        UINT32 type, port;
        EFI_STATUS s;
        if ((d3 & TRB_CYCLE) != (*ev_cycle ? TRB_CYCLE : 0U))
            return EFI_SUCCESS;
        d0 = r[*ev_index * 4U + 0U];
        d1 = r[*ev_index * 4U + 1U];
        d2 = r[*ev_index * 4U + 2U];
        type = (d3 & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
        port = (d3 >> 24) & 0xffU;
        if (type != TRB_PORT_STATUS_CHANGE || port != expected_port)
            return EFI_DEVICE_ERROR;
        s = event_wait(p, ir, ev, ev_index, ev_cycle, 1, &d0, &d1, &d2, &d3);
        if (EFI_ERROR(s)) return s;
        s = ack_interrupter(p, ir);
        if (EFI_ERROR(s)) return s;
    }
    return EFI_SUCCESS;
}

static UINT16 ep0_mps_from_speed(UINT32 speed)
{
    if (speed <= 2U) return 8U;
    if (speed == 3U) return 64U;
    return 512U;
}

static void init_link_trb(struct dma_obj *ring)
{
    UINT32 *r = (UINT32 *)ring->host;
    UINTN n = CMD_TRBS - 1U;
    UINT64 base = ring->dev;
    r[n * 4U + 0U] = (UINT32)base;
    r[n * 4U + 1U] = (UINT32)(base >> 32);
    r[n * 4U + 2U] = 0;
    r[n * 4U + 3U] = TRB_CYCLE | TRB_LINK_TOGGLE |
                      (TRB_LINK << TRB_TYPE_SHIFT);
}

static void init_event_link_trb(struct dma_obj *ring)
{
    UINT32 *r = (UINT32 *)ring->host;
    UINTN n = EVENT_TRBS - 1U;
    UINT64 base = ring->dev;
    r[n * 4U + 0U] = (UINT32)base;
    r[n * 4U + 1U] = (UINT32)(base >> 32);
    r[n * 4U + 2U] = 0;
    r[n * 4U + 3U] = TRB_CYCLE | TRB_LINK_TOGGLE |
                      (TRB_LINK << TRB_TYPE_SHIFT);
}

static void init_command_trb(VOID *ring, UINTN index, UINT32 dword3)
{
    UINT32 *r = (UINT32 *)ring;
    r[index * 4U + 0U] = 0;
    r[index * 4U + 1U] = 0;
    r[index * 4U + 2U] = 0;
    r[index * 4U + 3U] = dword3 | TRB_CYCLE;
}

static void put64(VOID *base, UINTN dword, UINT64 v)
{
    ((UINT32 *)base)[dword + 0U] = (UINT32)v;
    ((UINT32 *)base)[dword + 1U] = (UINT32)(v >> 32);
}

static UINT32 ctx_stride(UINT32 hcc)
{
    return (hcc & HCC_CTXSZ) ? 64U : 32U;
}

static EFI_STATUS prepare_input_context(struct dma_obj *input,
                                        struct dma_obj *ep0_ring,
                                        UINT32 stride, UINT8 slot_type,
                                        UINT8 root_port_value, UINT32 speed,
                                        UINT16 ep0_mps)
{
    UINT32 *c = (UINT32 *)input->host;
    UINT32 *slot = (UINT32 *)((UINT8 *)c + stride);
    UINT32 *ep0 = (UINT32 *)((UINT8 *)c + stride * 2U);

    uefi_call_wrapper(BS->SetMem, 3, input->host, EFI_PAGE_BYTES, 0);
    c[0] = INPUT_SLOT_FLAG | INPUT_EP0_FLAG;

    slot[0] = ((speed & 0xfU) << DEV_SPEED_SHIFT) |
              (1U << LAST_CTX_SHIFT);
    slot[1] = ((UINT32)root_port_value << ROOT_HUB_PORT_SHIFT);
    if (slot_type) {
        /* Protocol Slot Type belongs to Enable Slot, not the device Slot
         * Context. Keep the input Slot Context free of non-device fields. */
        (void)slot_type;
    }

    ep0[0] = (EP0_CERR << EP_CERR_SHIFT) |
             (EP_TYPE_CONTROL << EP_TYPE_SHIFT) |
             ((UINT32)ep0_mps << EP0_MPS_SHIFT);
    put64(ep0, 2U, ep0_ring->dev | EP0_DCS);
    ep0[4] = 0;
    ep0[5] = 0;
    ep0[6] = 0;
    ep0[7] = 0;
    return EFI_SUCCESS;
}

static UINT32 output_slot_state(VOID *output, UINT32 stride)
{
    UINT32 *slot = (UINT32 *)output;
    (void)stride;
    return slot[3];
}

static void fatal_running(void)
{
    Print(u"\r\nFATAL: XHCI RUNNING STATE UNCERTAIN\r\n");
    Print(u"DMA MAPPINGS RETAINED / NO FREE\r\n");
    Print(u"MANUAL RECOVERY REQUIRED\r\n");
    for (;;) uefi_call_wrapper(BS->Stall, 1, 1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_HANDLE xhci_handle = NULL;
    EFI_STATUS s = EFI_SUCCESS, t;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT32 cap = 0, hcs1 = 0, hcs2 = 0, hcc = 0, op = 0;
    UINT32 db = 0, rtsoff = 0, xhci_rt = 0, status = 0;
    UINT32 maxslots = 0, ports = 0, pagesel = 0, page_size = 0;
    UINT32 cmd = 0, iman = 0, portsc = 0;
    UINT16 ver = 0;
    UINT8 protocol_major = 0, slot_type = 0;
    UINT8 speed = 0, slot_id = 0;
    UINT16 ep0_mps;
    UINT32 completion = 0;
    UINT32 stride;
    UINT8 pci_dev = 0, pci_func = 0;
    UINT8 kb_pci_dev = 0, kb_pci_func = 0;
    EFI_DEVICE_PATH_PROTOCOL *xhci_path;
    BOOLEAN started = FALSE;
    BOOLEAN halted = FALSE;
    UINTN i, shift = 0;
    UINTN ev_index = 0;
    BOOLEAN ev_cycle = TRUE;
    struct discovery d = {0};
    struct dma_obj dcbaa_d = {0};
    struct dma_obj scratch_array_d = {0};
    struct dma_obj cmd_d = {0};
    struct dma_obj ev_d = {0};
    struct dma_obj erst_d = {0};
    struct dma_obj input_d = {0};
    struct dma_obj output_d = {0};
    struct dma_obj ep0_ring_d = {0};
    struct dma_obj *scratch = NULL;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V32 / GATE 6 / HID KEYBOARD ADDRESS\r\n");
    Print(u"CUMULATIVE V29+V30+V31 + PORT RESET + ADDRESS DEVICE\r\n");
    Print(u"UEFI DISCOVERY / POLLED EVENTS / CPU INTERRUPTS DISABLED\r\n");

    s = discover_keyboard(&d, image);
    if (EFI_ERROR(s)) { remember_fail(u"DISCOVERY", u"BOOT KEYBOARD", s); goto out; }
    Print(u"DISCOVERY: PORT=%u VID=%04x PID=%04x IF=%u EP=%02x MPS=%u EP0=%u INT=%u CFG=%u\r\n",
          d.port, d.vid, d.pid, d.interface_number, d.endpoint,
          d.mps, d.ep0_mps, d.interval, d.config);

    s = find_xhci(&p, &xhci_handle, image);
    if (EFI_ERROR(s)) { remember_fail(u"PCI", u"FIND XHCI", s); goto out; }
    xhci_path = DevicePathFromHandle(xhci_handle);
    if (!path_last_pci_bdf(xhci_path, &pci_dev, &pci_func)) {
        s = EFI_NOT_FOUND; remember_fail(u"PCI", u"XHCI DEVICE PATH", s); goto out;
    }
    {
        EFI_DEVICE_PATH_PROTOCOL *usb_path = NULL;
        EFI_HANDLE *usb_hs = NULL;
        EFI_USB_IO_PROTOCOL *usb = NULL;
        EFI_UNUSED(usb);
        (void)usb_path;
        (void)usb_hs;
        (void)kb_pci_dev;
        (void)kb_pci_func;
    }

    s = cfg32(p, 0x10, &bar0);
    if (EFI_ERROR(s) || (bar0 & 1U) || ((bar0 >> 1) & 3U) != 2U) {
        s = EFI_UNSUPPORTED; remember_fail(u"PCI", u"BAR0", s); goto out;
    }
    s = cfg32(p, 0x14, &bar1);
    if (EFI_ERROR(s)) { remember_fail(u"PCI", u"BAR1", s); goto out; }
    s = cfg32(p, 0, &id);
    if (EFI_ERROR(s)) { remember_fail(u"PCI", u"ID", s); goto out; }
    s = cfg32(p, 8, &cls);
    if (EFI_ERROR(s)) { remember_fail(u"PCI", u"CLASS", s); goto out; }

    s = mr32(p, 0, &cap);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"CAPLENGTH", s); goto out; }
    op = cap & 0xffU;
    s = mr16(p, 2, &ver);
    if (EFI_ERROR(s) || ver < XHCI_MIN_VERSION) {
        s = EFI_UNSUPPORTED; remember_fail(u"CAPS", u"HCIVERSION", s); goto out;
    }
    s = mr32(p, 4, &hcs1);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"HCSPARAMS1", s); goto out; }
    s = mr32(p, 8, &hcs2);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"HCSPARAMS2", s); goto out; }
    s = mr32(p, 0x10, &hcc);
    if (EFI_ERROR(s) || !(hcc & HCC_AC64)) {
        s = EFI_UNSUPPORTED; remember_fail(u"CAPS", u"HCCPARAMS1 AC64", s); goto out;
    }
    s = mr32(p, op + 8, &pagesel);
    if (EFI_ERROR(s) || !pagesel) {
        s = EFI_UNSUPPORTED; remember_fail(u"CAPS", u"PAGESIZE", s); goto out;
    }
    while (shift < 32U && !(pagesel & (1U << shift))) ++shift;
    if (shift >= 32U) {
        s = EFI_UNSUPPORTED; remember_fail(u"CAPS", u"PAGESIZE BIT", s); goto out;
    }
    page_size = (UINT32)1U << (12U + shift);
    if (page_size != EFI_PAGE_BYTES) {
        s = EFI_UNSUPPORTED; remember_fail(u"CAPS", u"NON-4K PAGE", s); goto out;
    }
    maxslots = hcs1 & 0xffU;
    ports = (hcs1 >> 24) & 0xffU;
    if (!maxslots || !ports || ports > 127U) {
        s = EFI_UNSUPPORTED; remember_fail(u"CAPS", u"LIMITS", s); goto out;
    }
    s = mr32(p, 0x14, &db);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"DBOFF", s); goto out; }
    db &= ~3U;
    s = mr32(p, 0x18, &rtsoff);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"RTSOFF", s); goto out; }
    xhci_rt = rtsoff & ~0x1fU;
    stride = ctx_stride(hcc);

    if (d.port > ports) { s = EFI_NOT_FOUND; remember_fail(u"DISCOVERY", u"PORT RANGE", s); goto out; }
    s = find_supported_protocol(p, hcc, d.port, &protocol_major, &slot_type);
    if (EFI_ERROR(s)) { remember_fail(u"DISCOVERY", u"PORT PROTOCOL", s); goto out; }
    d.protocol_major = protocol_major;

    Print(u"XHCI=%04x:%04x VERSION=%u.%02u BAR=%08x/%08x OP=%02x\r\n",
          id & 0xffffU, id >> 16, ver >> 8, ver & 0xffU, bar0, bar1, op);
    Print(u"CAPS: SLOTS=%u PORTS=%u SCRATCHPADS=%u CSZ=%u PAGE=%u DBOFF=%08x RTSOFF=%08x\r\n",
          maxslots, ports, (((hcs2 >> 21) & 0x1fU) << 5) | ((hcs2 >> 27) & 0x1fU),
          stride, page_size, db, rtsoff);
    Print(u"DISCOVERY VALIDATED: PORT=%u PROTOCOL=USB%u SLOT-TYPE=%u XHCI-BDF=%u:%u\r\n",
          d.port, protocol_major, slot_type, pci_dev, pci_func);

    if (protocol_major != 2U && protocol_major != 3U) {
        s = EFI_UNSUPPORTED; remember_fail(u"PORT", u"SUPPORTED USB PROTOCOL", s); goto out;
    }

    s = mr32(p, op + 4, &status);
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"USBSTS", s); goto out; }
    if (!(status & STS_HCH)) {
        s = mr32(p, op, &cmd);
        if (EFI_ERROR(s)) { remember_fail(u"HALT", u"USBCMD", s); goto out; }
        s = mw32(p, op, cmd & ~(CMD_RUN | CMD_INTE | CMD_HSEE));
        if (EFI_ERROR(s)) { remember_fail(u"HALT", u"STOP", s); goto out; }
        s = wait_hch(p, op, TRUE, 1000, &status);
        if (EFI_ERROR(s)) { remember_fail(u"HALT", u"HCH", s); goto out; }
    }
    halted = TRUE;

    s = reset_xhci(p, op, &cmd, &status);
    if (EFI_ERROR(s)) { remember_fail(u"RESET", u"HCRST/CNR", s); goto out; }
    if (!(status & STS_HCH)) {
        s = EFI_DEVICE_ERROR; remember_fail(u"RESET", u"VERIFY HALTED", s); goto out;
    }

    s = dma_alloc(p, 1, &dcbaa_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"DCBAA", s); goto out; }
    s = dma_alloc(p, 1, &cmd_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"COMMAND RING", s); goto out; }
    s = dma_alloc(p, 1, &ev_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"EVENT RING", s); goto out; }
    s = dma_alloc(p, 1, &erst_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"ERST", s); goto out; }

    {
        UINT32 scratchpads = (((hcs2 >> 21) & 0x1fU) << 5) | ((hcs2 >> 27) & 0x1fU);
        if (scratchpads > MAX_SCRATCHPADS) {
            s = EFI_UNSUPPORTED; remember_fail(u"DMA", u"SCRATCHPAD COUNT", s); goto out;
        }
        if (scratchpads) {
            s = dma_alloc(p, 1, &scratch_array_d);
            if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCHPAD ARRAY", s); goto out; }
            s = uefi_call_wrapper(BS->AllocatePool, 3, EfiBootServicesData,
                                  scratchpads * sizeof(struct dma_obj),
                                  (void **)&scratch);
            if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCH DESCRIPTORS", s); goto out; }
            s = uefi_call_wrapper(BS->SetMem, 3, scratch,
                                  scratchpads * sizeof(struct dma_obj), 0);
            if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCH DESCRIPTORS ZERO", s); goto out; }
            for (i = 0; i < scratchpads; ++i) {
                s = dma_alloc(p, 1, &scratch[i]);
                if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCHPAD", s); goto out; }
            }
            for (i = 0; i < scratchpads; ++i)
                ((UINT64 *)scratch_array_d.host)[i] = scratch[i].dev;
            ((UINT64 *)dcbaa_d.host)[0] = scratch_array_d.dev;
        }
    }

    init_link_trb(&cmd_d);
    init_event_link_trb(&ev_d);
    {
        UINT64 *e = (UINT64 *)erst_d.host;
        e[0] = ev_d.dev;
        ((UINT32 *)erst_d.host)[2] = EVENT_TRBS;
        ((UINT32 *)erst_d.host)[3] = 0;
    }

    s = mw32(p, op + 0x38, 1U);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"CONFIG", s); goto out; }
    s = mw64(p, op + 0x30, dcbaa_d.dev);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"DCBAAP", s); goto out; }
    s = mw64(p, op + 0x18, cmd_d.dev | CRCR_RCS);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"CRCR", s); goto out; }
    s = mw32(p, xhci_rt + 0x08, 1U);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"ERSTSZ", s); goto out; }
    s = mw64(p, xhci_rt + 0x10, erst_d.dev & 0xffffffffffffffc0ULL);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"ERSTBA", s); goto out; }
    s = mw64(p, xhci_rt + 0x18, ev_d.dev & 0xfffffffffffffff0ULL);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"ERDP", s); goto out; }

    s = mr32(p, xhci_rt, &iman);
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"IMAN", s); goto out; }
    if (iman & IMAN_IP) {
        s = mw32(p, xhci_rt, IMAN_IP);
        if (EFI_ERROR(s)) { remember_fail(u"RUN", u"ACK INITIAL IMAN", s); goto out; }
    }

    s = mr32(p, op, &cmd);
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"USBCMD", s); goto out; }
    s = mw32(p, op, (cmd & ~(CMD_INTE | CMD_HSEE)) | CMD_RUN);
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"START", s); goto out; }
    started = TRUE;
    halted = FALSE;
    s = wait_hch(p, op, FALSE, 1000, &status);
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"HCH CLEAR", s); goto fatal; }

    s = xhci_port_read(p, op, d.port, &portsc);
    if (EFI_ERROR(s)) { remember_fail(u"PORT", u"PORTSC READ", s); goto fatal; }
    if (!(portsc & PORT_CCS)) {
        s = EFI_NOT_FOUND; remember_fail(u"PORT", u"EXPECTED CONNECTED PORT", s); goto fatal;
    }
    Print(u"PORT BEFORE RESET: RAW=%08x CCS=%u PED=%u PLS=%u SPD=%u CSC=%u PRC=%u\r\n",
          portsc, (portsc & PORT_CCS) ? 1 : 0, (portsc & PORT_PED) ? 1 : 0,
          (portsc & PORT_PLS_MASK) >> 5, (portsc & PORT_SPEED_MASK) >> 10,
          (portsc & PORT_CSC) ? 1 : 0, (portsc & PORT_PRC) ? 1 : 0);

    ev_index = 0;
    ev_cycle = TRUE;
    s = drain_start_events(p, xhci_rt, &ev_d, &ev_index, &ev_cycle, d.port);
    if (EFI_ERROR(s)) { remember_fail(u"PORT", u"DRAIN START EVENT", s); goto fatal; }

    {
        UINT32 clear_bits = PORT_CSC | PORT_PRC;
        if (protocol_major == 3U) clear_bits |= PORT_WRC;
        s = port_write_neutral(p, op, d.port, clear_bits);
        if (EFI_ERROR(s)) { remember_fail(u"PORT", u"CLEAR OLD CHANGES", s); goto fatal; }
    }

    s = port_write_neutral(p, op, d.port,
                           protocol_major == 3U ? PORT_WR : PORT_PR);
    if (EFI_ERROR(s)) { remember_fail(u"PORT", u"ASSERT RESET", s); goto fatal; }

    {
        UINT32 reset_bit = protocol_major == 3U ? PORT_WR : PORT_PR;
        UINT32 change_bit = protocol_major == 3U ? PORT_WRC : PORT_PRC;
        UINTN loops;
        for (loops = 0; loops < 10000U; ++loops) {
            s = xhci_port_read(p, op, d.port, &portsc);
            if (EFI_ERROR(s)) { remember_fail(u"PORT", u"RESET POLL", s); goto fatal; }
            if (!(portsc & reset_bit) && (portsc & change_bit)) break;
            uefi_call_wrapper(BS->Stall, 1, 1000);
        }
        if (loops == 10000U) {
            s = EFI_TIMEOUT; remember_fail(u"PORT", u"RESET COMPLETION", s); goto fatal;
        }
    }

    if (!(portsc & PORT_CCS)) {
        s = EFI_NOT_FOUND; remember_fail(u"PORT", u"CCS LOST AFTER RESET", s); goto fatal;
    }
    speed = (UINT8)((portsc & PORT_SPEED_MASK) >> 10);
    if (!speed) {
        s = EFI_DEVICE_ERROR; remember_fail(u"PORT", u"INVALID PORT SPEED", s); goto fatal;
    }
    if (protocol_major == 2U && !(portsc & PORT_PED)) {
        s = EFI_DEVICE_ERROR; remember_fail(u"PORT", u"USB2 PORT NOT ENABLED", s); goto fatal;
    }

    {
        UINT32 clear_reset_change = protocol_major == 3U ? PORT_WRC : PORT_PRC;
        s = port_write_neutral(p, op, d.port, clear_reset_change);
        if (EFI_ERROR(s)) { remember_fail(u"PORT", u"CLEAR RESET CHANGE", s); goto fatal; }
        s = xhci_port_read(p, op, d.port, &portsc);
        if (EFI_ERROR(s) || (portsc & clear_reset_change)) {
            s = EFI_DEVICE_ERROR; remember_fail(u"PORT", u"RESET CHANGE READBACK", s); goto fatal;
        }
    }

    s = event_wait(p, xhci_rt, &ev_d, &ev_index, &ev_cycle, 10000,
                   &cmd, &status, &completion, &iman);
    if (EFI_ERROR(s)) { remember_fail(u"PORT", u"PORT STATUS CHANGE EVENT", s); goto fatal; }
    if (((iman & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT) != TRB_PORT_STATUS_CHANGE ||
        ((iman >> 24) & 0xffU) != d.port) {
        s = EFI_DEVICE_ERROR; remember_fail(u"PORT", u"VALIDATE RESET EVENT", s); goto fatal;
    }
    s = ack_interrupter(p, xhci_rt);
    if (EFI_ERROR(s)) { remember_fail(u"PORT", u"ACK RESET EVENT", s); goto fatal; }

    Print(u"PORT RESET PASS: PORT=%u SPD=%u CCS=%u PED=%u PLS=%u PRC/WRC=1->0\r\n",
          d.port, speed, (portsc & PORT_CCS) ? 1 : 0,
          (portsc & PORT_PED) ? 1 : 0, (portsc & PORT_PLS_MASK) >> 5);

    init_command_trb(cmd_d.host, 0, (slot_type & 0x1fU) << 16 | (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT));
    s = mw32(p, db, 0);
    if (EFI_ERROR(s)) { remember_fail(u"ENABLE SLOT", u"DOORBELL 0", s); goto fatal; }
    s = wait_command_completion(p, xhci_rt, &ev_d, &ev_index, &ev_cycle,
                                cmd_d.dev, (UINT8)maxslots, &slot_id,
                                &completion);
    if (EFI_ERROR(s) || completion != CC_SUCCESS) {
        if (!EFI_ERROR(s)) s = EFI_DEVICE_ERROR;
        remember_fail(u"ENABLE SLOT", u"COMMAND COMPLETION", s); goto fatal;
    }
    Print(u"ENABLE SLOT PASS: SLOT=%u COMPLETION=%u\r\n", slot_id, completion);

    s = dma_alloc(p, 1, &input_d);
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS DEVICE", u"INPUT CONTEXT", s); goto fatal; }
    s = dma_alloc(p, 1, &output_d);
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS DEVICE", u"OUTPUT CONTEXT", s); goto fatal; }
    s = dma_alloc(p, 1, &ep0_ring_d);
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS DEVICE", u"EP0 RING", s); goto fatal; }
    init_link_trb(&ep0_ring_d);

    ep0_mps = ep0_mps_from_speed(speed);
    if (d.ep0_mps && d.protocol_major == 2U &&
        d.ep0_mps != ep0_mps) {
        /* UEFI's bMaxPacketSize0 is retained as a discovery fact, but the
         * Address Device input uses the xHCI speed-derived default MPS. */
        Print(u"NOTE: UEFI EP0 MPS=%u, xHCI SPEED-DERIVED EP0 MPS=%u\r\n",
              d.ep0_mps, ep0_mps);
    }

    s = prepare_input_context(&input_d, &ep0_ring_d, stride, slot_type,
                              d.port, speed, ep0_mps);
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS DEVICE", u"BUILD INPUT CONTEXT", s); goto fatal; }

    {
        UINT32 *dcbaa = (UINT32 *)dcbaa_d.host;
        dcbaa[slot_id] = (UINT64)output_d.dev;
    }

    {
        UINT8 *inb = (UINT8 *)input_d.host;
        UINT32 *slot = (UINT32 *)(inb + stride);
        UINT32 *ep0 = (UINT32 *)(inb + stride * 2U);
        if ((slot[0] & (0x1fU << LAST_CTX_SHIFT)) != (1U << LAST_CTX_SHIFT) ||
            ((slot[1] >> ROOT_HUB_PORT_SHIFT) & 0xffU) != d.port ||
            ((slot[0] >> DEV_SPEED_SHIFT) & 0xfU) != speed ||
            ((ep0[0] >> EP_TYPE_SHIFT) & 0x7U) != EP_TYPE_CONTROL ||
            ((ep0[0] >> EP_MPS_SHIFT) & 0xffffU) != ep0_mps ||
            (((UINT64)ep0[3] << 32 | ep0[2]) & ~1ULL) != (ep0_ring_d.dev & ~0x1ULL)) {
            s = EFI_DEVICE_ERROR; remember_fail(u"ADDRESS DEVICE", u"INPUT CONTEXT VERIFY", s); goto fatal;
        }
    }

    init_command_trb(cmd_d.host, 1, ((UINT32)slot_id << SLOT_ID_SHIFT) |
                                  (TRB_ADDRESS_DEVICE << TRB_TYPE_SHIFT));
    put64(cmd_d.host, 1U * 4U, input_d.dev);
    s = mw32(p, db, 0);
    if (EFI_ERROR(s)) { remember_fail(u"ADDRESS DEVICE", u"DOORBELL 0", s); goto fatal; }
    s = wait_command_completion(p, xhci_rt, &ev_d, &ev_index, &ev_cycle,
                                cmd_d.dev + 16U, (UINT8)maxslots, &slot_id,
                                &completion);
    if (EFI_ERROR(s) || completion != CC_SUCCESS) {
        if (!EFI_ERROR(s)) s = EFI_DEVICE_ERROR;
        remember_fail(u"ADDRESS DEVICE", u"COMMAND COMPLETION", s); goto fatal;
    }

    {
        UINT32 state = output_slot_state(output_d.host, stride);
        UINT32 out_state = (state >> SLOT_STATE_SHIFT) & 0x1fU;
        UINT32 address = state & 0xffU;
        if (out_state != SLOT_STATE_ADDRESSED || !address) {
            s = EFI_DEVICE_ERROR; remember_fail(u"ADDRESS DEVICE", u"OUTPUT CONTEXT STATE", s); goto fatal;
        }
        Print(u"ADDRESS DEVICE PASS: SLOT=%u ADDRESS=%u STATE=ADDRESSED COMPLETION=%u\r\n",
              slot_id, address, completion);
    }

    s = mw32(p, op, 0);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"STOP", s); goto fatal; }
    started = FALSE;
    s = wait_hch(p, op, TRUE, 10000, &status);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"CONFIRM HALT", s); goto fatal; }
    halted = TRUE;

    s = reset_xhci(p, op, &cmd, &status);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"RESET RECOVERY", s); goto out; }
    halted = TRUE;

    s = mw64(p, op + 0x18, 0);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"CRCR CLEAR", s); goto out; }
    s = mw64(p, op + 0x30, 0);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"DCBAAP CLEAR", s); goto out; }
    s = mw32(p, op + 0x38, 0);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"CONFIG CLEAR", s); goto out; }
    s = mw32(p, xhci_rt + 0x08, 0);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"ERSTSZ CLEAR", s); goto out; }
    s = mw64(p, xhci_rt + 0x10, 0);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"ERSTBA CLEAR", s); goto out; }
    s = mw64(p, xhci_rt + 0x18, 0);
    if (EFI_ERROR(s)) { remember_fail(u"TEARDOWN", u"ERDP CLEAR", s); goto out; }

out:
    if (started) {
        t = mw32(p, op, 0);
        if (EFI_ERROR(t) || EFI_ERROR(wait_hch(p, op, TRUE, 10000, &status)))
            fatal_running();
        halted = TRUE;
    }

    if (p && halted) {
        dma_free(p, &ep0_ring_d);
        dma_free(p, &output_d);
        dma_free(p, &input_d);
        dma_free(p, &erst_d);
        dma_free(p, &ev_d);
        dma_free(p, &cmd_d);
        dma_free(p, &scratch_array_d);
        if (scratch) {
            UINT32 scratchpads = (((hcs2 >> 21) & 0x1fU) << 5) | ((hcs2 >> 27) & 0x1fU);
            for (i = 0; i < scratchpads; ++i)
                dma_free(p, &scratch[i]);
            uefi_call_wrapper(BS->FreePool, 1, scratch);
            scratch = NULL;
        }
        dma_free(p, &dcbaa_d);
    }

    if (EFI_ERROR(s)) {
        Print(u"V32 GATE 6: FAIL RESULT=%r\r\n", s);
        Print(u"FAIL STAGE=%s OP=%s STATUS=%r\r\n", fail_stage, fail_op, fail_status);
    } else {
        Print(u"V32 GATE 6: PASS / ADDRESS DEVICE COMPLETE\r\n");
        Print(u"PORT RESET=1 ENABLE SLOT=1 ADDRESS DEVICE=1\r\n");
        Print(u"CPU-INTERRUPTS=0 USB-TRANSFERS=0 MOUSE=0 HUBS=0\r\n");
        Print(u"CONTROLLER HALTED / RESET / POINTERS CLEARED BEFORE DMA RELEASE\r\n");
    }
    return s;
}
