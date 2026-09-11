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
#define TRB_ENABLE_SLOT 9U
#define TRB_ADDRESS_DEVICE 11U
#define TRB_DISABLE_SLOT 10U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 0x00000002U
#define TRB_COMMAND_COMPLETION 33U
#define TRB_PORT_STATUS_CHANGE 34U
#define CC_SUCCESS 1U

#define PORTSC_BASE 0x400U
#define PORT_CCS (1U << 0)
#define PORT_PED (1U << 1)
#define PORT_PR (1U << 4)
#define PORT_SPEED_MASK (0xFU << 10)
#define PORT_PRC (1U << 21)
#define PORT_RO ((1U << 0) | (1U << 3) | (0xFU << 10) | (1U << 30))
#define PORT_RWS ((0xFU << 5) | (1U << 9) | (3U << 14) | (7U << 25))
#define PORT_RW (1U << 16)

#define TRB_SLOT_SHIFT 24U
#define ROOT_PORT_SHIFT 16U
#define SPEED_SHIFT 20U
#define CTX_ENTRIES_SHIFT 27U
#define EP0_CERR 3U
#define EP0_CTRL_TYPE 4U

#define CMD_TRBS 256U
#define EVENT_TRBS 16U
#define MAX_SCRATCHPADS 1024U

#define USB_CLASS_HID 3U
#define HID_SUBCLASS_BOOT 1U
#define HID_PROTOCOL_KEYBOARD 1U
#define DP_TYPE_MESSAGING 3U
#define DP_SUBTYPE_USB 5U
#define DP_TYPE_END 0x7FU

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
    UINT8 interface_number;
    UINT8 endpoint;
    UINT8 interval;
    UINT16 mps;
    UINT16 vid;
    UINT16 pid;
    UINT8 config;
    BOOLEAN found;
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

    if (!pages || pages > ((UINTN)-1) / 4096U)
        return EFI_BAD_BUFFER_SIZE;

    bytes = pages * 4096U;
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

static EFI_STATUS find_slot_type_for_port(EFI_PCI_IO_PROTOCOL *p, UINT32 hcc,
                                          UINT32 port, UINT32 *slot_type)
{
    UINT32 off = ((hcc >> 16) & 0xffffU) * 4U;
    UINT32 w, d2, d3, next;
    UINTN n = 0;
    EFI_STATUS s;

    *slot_type = 0;
    while (off && n++ < 64) {
        s = mr32(p, off, &w);
        if (EFI_ERROR(s)) return s;

        if ((w & 0xffU) == 2U) {
            s = mr32(p, off + 8, &d2);
            if (EFI_ERROR(s)) return s;
            s = mr32(p, off + 12, &d3);
            if (EFI_ERROR(s)) return s;

            {
                UINT32 first = d2 & 0xffU;
                UINT32 count = (d2 >> 8) & 0xffU;
                if (count && port >= first && port < first + count) {
                    *slot_type = d3 & 0x1fU;
                    return EFI_SUCCESS;
                }
            }
        }

        next = ((w >> 8) & 0xffU) * 4U;
        if (!next) break;
        off += next;
    }
    return EFI_NOT_FOUND;
}

static UINT8 root_port(EFI_DEVICE_PATH_PROTOCOL *path)
{
    UINT8 *p = (UINT8 *)path;

    while (p) {
        UINT16 len = (UINT16)p[2] | ((UINT16)p[3] << 8);
        if (len < 4 || len > 255) break;
        if (p[0] == DP_TYPE_END) break;
        if (p[0] == DP_TYPE_MESSAGING && p[1] == DP_SUBTYPE_USB && len >= 6)
            return p[4];
        p += len;
    }
    return 0;
}

/* UEFI is the authoritative discovery provider. Exactly one Boot Keyboard
 * interface is accepted; a composite keyboard/mouse receiver is fine. */
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
        UINTN j;
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
        port = root_port(path);
        if (!port) continue;

        d->port = port;
        d->interface_number = in.InterfaceNumber;
        d->vid = dd.IdVendor;
        d->pid = dd.IdProduct;
        d->config = cd.ConfigurationValue;
        d->endpoint = 0;
        d->mps = 0;
        d->interval = 0;

        for (j = 0; j < in.NumEndpoints && j < 16; ++j) {
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
        if (found > 1) {
            uefi_call_wrapper(BS->FreePool, 1, hs);
            return EFI_ABORTED;
        }
    }

    if (hs) uefi_call_wrapper(BS->FreePool, 1, hs);
    if (found != 1) return EFI_NOT_FOUND;
    d->found = TRUE;
    return EFI_SUCCESS;
}

static void trb_put32(VOID *base, UINTN trb, UINTN dw, UINT32 value)
{
    ((UINT32 *)base)[trb * 4U + dw] = value;
}

static void trb_put64(VOID *base, UINTN trb, UINT64 value)
{
    trb_put32(base, trb, 0, (UINT32)value);
    trb_put32(base, trb, 1, (UINT32)(value >> 32));
}

static UINT64 trb_get64(VOID *base, UINTN trb)
{
    UINT32 *r = (UINT32 *)base;
    return ((UINT64)r[trb * 4U + 1] << 32) | r[trb * 4U];
}

static void clear_trb(VOID *base, UINTN trb)
{
    trb_put32(base, trb, 0, 0);
    trb_put32(base, trb, 1, 0);
    trb_put32(base, trb, 2, 0);
    trb_put32(base, trb, 3, 0);
}

static void init_command_ring(struct dma_obj *d)
{
    UINT32 *r = (UINT32 *)d->host;
    UINT64 base = d->dev;
    r[(CMD_TRBS - 1) * 4 + 0] = (UINT32)base;
    r[(CMD_TRBS - 1) * 4 + 1] = (UINT32)(base >> 32);
    r[(CMD_TRBS - 1) * 4 + 2] = 0;
    r[(CMD_TRBS - 1) * 4 + 3] = (TRB_LINK << TRB_TYPE_SHIFT) |
                                 TRB_LINK_TOGGLE | TRB_CYCLE;
}

static void init_ep0_ring(struct dma_obj *d)
{
    UINT32 *r = (UINT32 *)d->host;
    UINT64 base = d->dev;
    r[(CMD_TRBS - 1) * 4 + 0] = (UINT32)base;
    r[(CMD_TRBS - 1) * 4 + 1] = (UINT32)(base >> 32);
    r[(CMD_TRBS - 1) * 4 + 2] = 0;
    r[(CMD_TRBS - 1) * 4 + 3] = (TRB_LINK << TRB_TYPE_SHIFT) |
                                 TRB_LINK_TOGGLE | TRB_CYCLE;
}

/* Poll one event from the primary interrupter. The command TRB pointer is
 * captured from the consumed event before the event-ring index advances. */
static EFI_STATUS poll_event(EFI_PCI_IO_PROTOCOL *p, UINT32 erdp_off,
                             struct dma_obj *ev, UINTN *idx, UINT8 *cycle,
                             UINT32 *type, UINT32 *dw0, UINT32 *dw2,
                             UINT32 *dw3, UINT64 *event_cmd_ptr)
{
    UINT32 *r = (UINT32 *)ev->host;
    UINTN i;
    UINTN consumed;
    UINT64 processed_addr;
    EFI_STATUS s;

    for (i = 0; i < 10000; ++i) {
        UINT32 d3 = r[*idx * 4 + 3];
        if ((d3 & TRB_CYCLE) == *cycle) {
            consumed = *idx;
            *dw0 = r[consumed * 4 + 0];
            *dw2 = r[consumed * 4 + 2];
            *dw3 = d3;
            *type = (d3 >> TRB_TYPE_SHIFT) & 0x3fU;
            *event_cmd_ptr = trb_get64(ev->host, consumed);
            processed_addr = ev->dev + consumed * 16U;

            clear_trb(ev->host, consumed);
            __sync_synchronize();
            s = mw64(p, erdp_off, processed_addr | 8ULL);
            if (EFI_ERROR(s)) return s;

            ++*idx;
            if (*idx == EVENT_TRBS) {
                *idx = 0;
                *cycle ^= 1;
            }
            return EFI_SUCCESS;
        }
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
}

/* PORTSC fields are grouped by xHCI access type. Preserve RO fields and
 * ordinary RW fields; do not replay RW1C/RW1S change bits. */
static UINT32 port_write_base(UINT32 x)
{
    return (x & PORT_RO) | (x & PORT_RWS) | (x & PORT_RW);
}

static UINT32 ep0_mps(UINT32 speed)
{
    switch (speed) {
    case 1: /* full speed */
    case 2: /* low speed */
        return 8U;
    case 3: /* high speed */
        return 64U;
    case 4: /* super speed */
    case 5: /* super speed plus */
        return 512U;
    default:
        return 0;
    }
}

static void fatal_running(void)
{
    Print(u"\r\nFATAL: XHCI RUNNING STATE UNCERTAIN\r\n");
    Print(u"FAIL STAGE=%s OP=%s STATUS=%r\r\n", fail_stage, fail_op, fail_status);
    Print(u"DMA MAPPINGS RETAINED / NO FREE\r\nMANUAL RECOVERY REQUIRED\r\n");
    for (;;) uefi_call_wrapper(BS->Stall, 1, 1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_HANDLE *hs = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_STATUS s = EFI_SUCCESS;
    UINTN n = 0, i;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT32 cap = 0, hcs1 = 0, hcs2 = 0, hcc = 0;
    UINT32 op = 0, db = 0, rt = 0, ir = 0, cmd = 0, status = 0;
    UINT32 ps = 0, maxslots = 0, scratchpads = 0, slot_type = 0;
    UINT32 portsc = 0, speed = 0, slot = 0;
    UINT32 event_type = 0, event_dw0 = 0, event_dw2 = 0, event_dw3 = 0;
    UINT64 event_ptr = 0;
    UINT16 ver = 0;
    UINTN shift = 0, xpage = 0, scratch_pages = 0;
    UINTN ctx_size = 32, ctx_pages = 1, input_pages = 1;
    UINTN cmd_index = 0, ev_index = 0;
    UINT8 ev_cycle = 1;
    BOOLEAN halted = FALSE, started = FALSE;

    struct discovery d = {0};
    struct dma_obj dcbaa_d = {0}, spa_d = {0}, cr_d = {0}, ev_d = {0};
    struct dma_obj erst_d = {0}, devctx_d = {0}, inctx_d = {0}, ep0_d = {0};
    struct dma_obj *scratch = NULL;
    UINT64 *dcbaa = NULL, *spa = NULL, *erst = NULL;
    UINT8 *inctx = NULL;
    UINT32 *devctx = NULL;
    UINT32 *ep0 = NULL;

    InitializeLib(image, st);
    Print(u"TOSHIBA xHCI V32.2 / CUMULATIVE DISCOVERY + PORT RESET + ADDRESS DEVICE\r\n");
    Print(u"V28 DISCOVERY + V29 INIT + V30 RUN/HALT + V31 ENABLE SLOT\r\n");
    Print(u"ONE KNOWN BOOT KEYBOARD / NO CONFIGURE ENDPOINT / NO HID REPORTS\r\n");

    s = discover_keyboard(&d, image);
    if (EFI_ERROR(s)) {
        remember_fail(u"DISCOVERY", u"KEYBOARD", s);
        goto out;
    }
    Print(u"DISCOVERY: PORT=%u VID=%04x PID=%04x IF=%u EP=%02x MPS=%u INT=%u CFG=%u\r\n",
          d.port, d.vid, d.pid, d.interface_number, d.endpoint,
          d.mps, d.interval, d.config);

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) { remember_fail(u"PCI", u"LOCATE", s); goto out; }

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
    if (!p) { s = EFI_NOT_FOUND; remember_fail(u"PCI", u"FIND XHCI", s); goto out; }

    s = cfg32(p, 0x10, &bar0);
    if (EFI_ERROR(s)) { remember_fail(u"PCI", u"BAR0", s); goto out; }
    if ((bar0 & 1U) || ((bar0 >> 1) & 3U) != 2U) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"PCI", u"64-BIT BAR", s);
        goto out;
    }
    s = cfg32(p, 0x14, &bar1);
    if (EFI_ERROR(s)) { remember_fail(u"PCI", u"BAR1", s); goto out; }

    s = mr32(p, 0, &cap);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"CAPLENGTH", s); goto out; }
    op = cap & 0xffU;

    s = mr16(p, 2, &ver);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"VERSION", s); goto out; }
    if (ver < XHCI_MIN_VERSION) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"CAPS", u"VERSION", s);
        goto out;
    }

    s = mr32(p, 4, &hcs1);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"HCSPARAMS1", s); goto out; }
    s = mr32(p, 8, &hcs2);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"HCSPARAMS2", s); goto out; }
    s = mr32(p, 0x10, &hcc);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"HCCPARAMS1", s); goto out; }
    s = mr32(p, op + 4, &status);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"USBSTS", s); goto out; }

    maxslots = hcs1 & 0xffU;
    scratchpads = (((hcs2 >> 21) & 0x1fU) << 5) | ((hcs2 >> 27) & 0x1fU);
    if (!(hcc & HCC_AC64)) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"CAPS", u"AC64", s);
        goto out;
    }
    if (!maxslots || scratchpads > MAX_SCRATCHPADS) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"CAPS", u"LIMITS", s);
        goto out;
    }

    s = mr32(p, op + 8, &ps);
    if (EFI_ERROR(s) || !ps) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"CAPS", u"PAGESIZE", s);
        goto out;
    }
    while (shift < 32 && !(ps & (1U << shift))) ++shift;
    if (shift >= 32) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"CAPS", u"PAGE BIT", s);
        goto out;
    }
    xpage = (UINTN)1U << (12U + shift);
    if (xpage != 4096U) {
        s = EFI_UNSUPPORTED;
        remember_fail(u"CAPS", u"PAGE SIZE", s);
        goto out;
    }
    scratch_pages = 1;

    if (hcc & HCC_CTXSZ) ctx_size = 64;
    ctx_pages = 1;
    input_pages = 1;

    Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x BAR=%08x/%08x OPBASE=%02x\r\n",
          ver >> 8, ver & 0xffU, id & 0xffffU, id >> 16, bar0, bar1, op);
    Print(u"CAPS: SLOTS=%u SCRATCHPADS=%u PAGESIZE=%u CONTEXT=%u HCH=%u CNR=%u\r\n",
          maxslots, scratchpads, (UINT32)xpage, (UINT32)ctx_size,
          (status & STS_HCH) ? 1 : 0, (status & STS_CNR) ? 1 : 0);

    s = mr32(p, 0x14, &db);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"DBOFF", s); goto out; }
    db &= ~3U;
    s = mr32(p, 0x18, &rt);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"RTSOFF", s); goto out; }
    rt &= ~0x1fU;
    ir = rt + 0x20U;

    s = find_slot_type_for_port(p, hcc, d.port, &slot_type);
    if (EFI_ERROR(s)) { remember_fail(u"CAPS", u"SLOT TYPE", s); goto out; }
    Print(u"CAPS: DBOFF=%08x RTSOFF=%08x PORT=%u SLOT-TYPE=%u\r\n",
          db, rt, d.port, slot_type);

    s = mr32(p, op, &cmd);
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"USBCMD", s); goto out; }
    if (!(status & STS_HCH)) {
        s = mw32(p, op, cmd & ~(CMD_RUN | CMD_INTE | CMD_HSEE));
        if (EFI_ERROR(s)) { remember_fail(u"HALT", u"STOP", s); goto out; }
        s = wait_hch(p, op, TRUE, 1000, &status);
        if (EFI_ERROR(s)) { remember_fail(u"HALT", u"HCH", s); goto out; }
    }
    halted = TRUE;

    s = reset_xhci(p, op, &cmd, &status);
    if (EFI_ERROR(s)) { remember_fail(u"RESET", u"RESET/CNR", s); goto out; }
    if (!(status & STS_HCH)) {
        s = EFI_DEVICE_ERROR;
        remember_fail(u"RESET", u"VERIFY HALTED", s);
        goto out;
    }

    s = dma_alloc(p, 2, &dcbaa_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"DCBAA", s); goto out; }
    s = dma_alloc(p, 1, &spa_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCH ARRAY", s); goto out; }
    s = dma_alloc(p, 1, &cr_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"COMMAND RING", s); goto out; }
    s = dma_alloc(p, 1, &ev_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"EVENT RING", s); goto out; }
    s = dma_alloc(p, 1, &erst_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"ERST", s); goto out; }
    s = dma_alloc(p, ctx_pages, &devctx_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"DEVICE CONTEXT", s); goto out; }
    s = dma_alloc(p, input_pages, &inctx_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"INPUT CONTEXT", s); goto out; }
    s = dma_alloc(p, 1, &ep0_d);
    if (EFI_ERROR(s)) { remember_fail(u"DMA", u"EP0 RING", s); goto out; }

    if (scratchpads) {
        s = uefi_call_wrapper(BS->AllocatePool, 3, EfiBootServicesData,
                              scratchpads * sizeof(struct dma_obj),
                              (VOID **)&scratch);
        if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCH DESCRIPTORS", s); goto out; }
        uefi_call_wrapper(BS->SetMem, 3, scratch,
                          scratchpads * sizeof(struct dma_obj), 0);
        for (i = 0; i < scratchpads; ++i) {
            s = dma_alloc(p, scratch_pages, &scratch[i]);
            if (EFI_ERROR(s)) { remember_fail(u"DMA", u"SCRATCHPAD", s); goto out; }
            if (scratch[i].dev & 0xfffULL) {
                s = EFI_BAD_BUFFER_SIZE;
                remember_fail(u"DMA", u"SCRATCH ALIGN", s);
                goto out;
            }
        }
    }

    dcbaa = (UINT64 *)dcbaa_d.host;
    spa = (UINT64 *)spa_d.host;
    erst = (UINT64 *)erst_d.host;
    devctx = (UINT32 *)devctx_d.host;
    inctx = (UINT8 *)inctx_d.host;
    ep0 = (UINT32 *)ep0_d.host;

    if (scratchpads) {
        for (i = 0; i < scratchpads; ++i)
            spa[i] = scratch[i].dev;
        dcbaa[0] = spa_d.dev;
    }

    init_command_ring(&cr_d);
    init_ep0_ring(&ep0_d);

    erst[0] = ev_d.dev;
    ((UINT32 *)erst_d.host)[2] = EVENT_TRBS;
    ((UINT32 *)erst_d.host)[3] = 0;

    s = mw32(p, ir + 0x00, 0);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"IMAN", s); goto out; }
    s = mw32(p, ir + 0x08, 1);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"ERSTSZ", s); goto out; }
    s = mw64(p, ir + 0x10, erst_d.dev);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"ERSTBA", s); goto out; }
    s = mw64(p, ir + 0x18, ev_d.dev | 8ULL);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"ERDP", s); goto out; }

    dcbaa[0] = scratchpads ? spa_d.dev : 0;
    s = mw32(p, op + 0x38, maxslots & 0xffU);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"CONFIG", s); goto out; }
    s = mw64(p, op + 0x30, dcbaa_d.dev);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"DCBAAP", s); goto out; }
    s = mw64(p, op + 0x18, cr_d.dev | CRCR_RCS);
    if (EFI_ERROR(s)) { remember_fail(u"INIT", u"CRCR", s); goto out; }

    /* CRCR Command Ring Pointer is not a readable pointer field on xHCI;
     * reading it returns zero for the pointer/control fields. Therefore the
     * CRCR programming write is validated indirectly by successful command
     * execution below rather than by an invalid pointer readback comparison. */
    {
        UINT32 v;
        s = mr32(p, op + 4, &v);
        if (EFI_ERROR(s)) {
            remember_fail(u"INIT", u"USBSTS VERIFY", s);
            goto out;
        }
        if (!(v & STS_HCH) || (v & STS_CNR)) {
            s = EFI_DEVICE_ERROR;
            remember_fail(u"INIT", u"HALTED VERIFY", s);
            goto out;
        }
        s = mr32(p, op, &v);
        if (EFI_ERROR(s)) {
            remember_fail(u"INIT", u"USBCMD VERIFY", s);
            goto out;
        }
        if (v & CMD_RUN) {
            s = EFI_DEVICE_ERROR;
            remember_fail(u"INIT", u"RUN VERIFY", s);
            goto out;
        }
    }

    s = mr32(p, op, &cmd);
    if (EFI_ERROR(s)) { remember_fail(u"RUN", u"USBCMD", s); goto out; }
    cmd |= CMD_RUN;
    cmd &= ~(CMD_INTE | CMD_HSEE);
    s = mw32(p, op, cmd);
    if (EFI_ERROR(s)) {
        remember_fail(u"RUN", u"START", s);
        fatal_running();
    }
    started = TRUE;
    s = wait_hch(p, op, FALSE, 1000, &status);
    if (EFI_ERROR(s)) {
        remember_fail(u"RUN", u"HCH", s);
        fatal_running();
    }

    s = mr32(p, op + PORTSC_BASE + (d.port - 1U) * 0x10U, &portsc);
    if (EFI_ERROR(s)) {
        remember_fail(u"PORT", u"READ PORTSC", s);
        fatal_running();
    }
    if (!(portsc & PORT_CCS)) {
        s = EFI_NOT_FOUND;
        remember_fail(u"PORT", u"EXPECTED PORT DISCONNECTED", s);
        fatal_running();
    }

    {
        UINT32 poff = op + PORTSC_BASE + (d.port - 1U) * 0x10U;
        UINT32 w = port_write_base(portsc) | PORT_PR;
        s = mw32(p, poff, w);
        if (EFI_ERROR(s)) {
            remember_fail(u"PORT", u"PORT RESET", s);
            fatal_running();
        }
    }

    for (;;) {
        s = poll_event(p, ir + 0x18, &ev_d, &ev_index, &ev_cycle,
                       &event_type, &event_dw0, &event_dw2, &event_dw3,
                       &event_ptr);
        if (EFI_ERROR(s)) {
            remember_fail(u"PORT", u"RESET EVENT", s);
            fatal_running();
        }
        if (event_type == TRB_PORT_STATUS_CHANGE &&
            ((event_dw0 >> 24) & 0xffU) == d.port)
            break;
    }

    {
        UINT32 poff = op + PORTSC_BASE + (d.port - 1U) * 0x10U;
        s = mr32(p, poff, &portsc);
        if (EFI_ERROR(s)) {
            remember_fail(u"PORT", u"POST RESET PORTSC", s);
            fatal_running();
        }
        if (!(portsc & PORT_CCS) || !(portsc & PORT_PED) ||
            (portsc & PORT_PR) || !(portsc & PORT_PRC)) {
            s = EFI_DEVICE_ERROR;
            remember_fail(u"PORT", u"RESET VERIFY", s);
            fatal_running();
        }
        speed = (portsc & PORT_SPEED_MASK) >> SPEED_SHIFT;
        if (!ep0_mps(speed)) {
            s = EFI_UNSUPPORTED;
            remember_fail(u"PORT", u"SPEED", s);
            fatal_running();
        }
        s = mw32(p, poff, PORT_PRC);
        if (EFI_ERROR(s)) {
            remember_fail(u"PORT", u"CLEAR PRC", s);
            fatal_running();
        }
    }

    Print(u"PORT RESET: PORT=%u CCS=1 PED=1 PR=0 SPEED=%u PASS\r\n",
          d.port, speed);

    {
        UINT32 cmd_trb = TRB_ENABLE_SLOT << TRB_TYPE_SHIFT;
        UINT32 *r = (UINT32 *)cr_d.host;
        r[cmd_index * 4 + 0] = 0;
        r[cmd_index * 4 + 1] = 0;
        r[cmd_index * 4 + 2] = (slot_type & 0x1fU) << 16;
        r[cmd_index * 4 + 3] = cmd_trb | TRB_CYCLE;
        __sync_synchronize();
        s = mw32(p, db, 0);
        if (EFI_ERROR(s)) { remember_fail(u"ENABLE SLOT", u"DOORBELL", s); fatal_running(); }
        ++cmd_index;

        for (;;) {
            s = poll_event(p, ir + 0x18, &ev_d, &ev_index, &ev_cycle,
                           &event_type, &event_dw0, &event_dw2, &event_dw3,
                           &event_ptr);
            if (EFI_ERROR(s)) { remember_fail(u"ENABLE SLOT", u"EVENT", s); fatal_running(); }
            if (event_type != TRB_COMMAND_COMPLETION) continue;
            if (((event_dw2 >> 24) & 0xffU) != CC_SUCCESS) {
                s = EFI_DEVICE_ERROR;
                remember_fail(u"ENABLE SLOT", u"COMPLETION", s);
                fatal_running();
            }
            slot = (event_dw3 >> TRB_SLOT_SHIFT) & 0xffU;
            if (!slot || slot > maxslots) {
                s = EFI_DEVICE_ERROR;
                remember_fail(u"ENABLE SLOT", u"SLOT ID", s);
                fatal_running();
            }
            break;
        }
        Print(u"ENABLE SLOT: SLOT=%u TYPE=%u PASS\r\n", slot, slot_type);
    }

    dcbaa[slot] = devctx_d.dev;
    __sync_synchronize();

    {
        UINT32 *ic = (UINT32 *)inctx_d.host;
        UINT32 *sc = (UINT32 *)(inctx + ctx_size);
        UINT32 *ec = (UINT32 *)(inctx + ctx_size * 2U);
        UINT32 *dc = devctx;
        UINT32 mps = ep0_mps(speed);
        UINT32 ep_info2;
        UINT64 ep_deq = ep0_d.dev | 1ULL;

        ic[0] = 0x3U;
        ic[1] = 0;
        ic[2] = 0;
        ic[3] = 0;

        sc[0] = (speed << SPEED_SHIFT) | (1U << CTX_ENTRIES_SHIFT);
        sc[1] = d.port << ROOT_PORT_SHIFT;
        sc[2] = 0;
        sc[3] = 0;

        ep_info2 = (EP0_CTRL_TYPE << 3) | (EP0_CERR << 1) | (mps << 16);
        ec[0] = 0;
        ec[1] = ep_info2;
        ec[2] = (UINT32)ep_deq;
        ec[3] = (UINT32)(ep_deq >> 32);
        ec[4] = 0;
        ec[5] = 0;
        ec[6] = 0;
        ec[7] = 0;

        dc[0] = 0;
        dc[1] = 0;
        dc[2] = 0;
        dc[3] = 0;
    }

    __sync_synchronize();

    {
        UINT32 *r = (UINT32 *)cr_d.host;
        UINT64 cmd_addr = cr_d.dev + cmd_index * 16U;

        r[cmd_index * 4 + 0] = (UINT32)inctx_d.dev;
        r[cmd_index * 4 + 1] = (UINT32)(inctx_d.dev >> 32);
        r[cmd_index * 4 + 2] = 0;
        r[cmd_index * 4 + 3] = (TRB_ADDRESS_DEVICE << TRB_TYPE_SHIFT) |
                                (slot << TRB_SLOT_SHIFT) | TRB_CYCLE;
        __sync_synchronize();
        s = mw32(p, db, 0);
        if (EFI_ERROR(s)) { remember_fail(u"ADDRESS DEVICE", u"DOORBELL", s); fatal_running(); }
        started = TRUE;
        ++cmd_index;

        for (;;) {
            s = poll_event(p, ir + 0x18, &ev_d, &ev_index, &ev_cycle,
                           &event_type, &event_dw0, &event_dw2, &event_dw3,
                           &event_ptr);
            if (EFI_ERROR(s)) { remember_fail(u"ADDRESS DEVICE", u"EVENT", s); fatal_running(); }
            if (event_type != TRB_COMMAND_COMPLETION) continue;

            if (event_ptr != cmd_addr) {
                s = EFI_DEVICE_ERROR;
                remember_fail(u"ADDRESS DEVICE", u"COMMAND POINTER", s);
                fatal_running();
            }
            if (((event_dw2 >> 24) & 0xffU) != CC_SUCCESS ||
                ((event_dw3 >> TRB_SLOT_SHIFT) & 0xffU) != slot) {
                s = EFI_DEVICE_ERROR;
                remember_fail(u"ADDRESS DEVICE", u"COMPLETION", s);
                fatal_running();
            }
            break;
        }
    }

    Print(u"ADDRESS DEVICE: SLOT=%u PORT=%u SPEED=%u EP0-MPS=%u PASS\r\n",
          slot, d.port, speed, ep0_mps(speed));

    {
        UINT32 *r = (UINT32 *)cr_d.host;
        UINT64 cmd_addr = cr_d.dev + cmd_index * 16U;
        r[cmd_index * 4 + 0] = 0;
        r[cmd_index * 4 + 1] = 0;
        r[cmd_index * 4 + 2] = 0;
        r[cmd_index * 4 + 3] = (TRB_DISABLE_SLOT << TRB_TYPE_SHIFT) |
                                (slot << TRB_SLOT_SHIFT) | TRB_CYCLE;
        __sync_synchronize();
        s = mw32(p, db, 0);
        if (EFI_ERROR(s)) { remember_fail(u"DISABLE SLOT", u"DOORBELL", s); fatal_running(); }
        ++cmd_index;
        for (;;) {
            s = poll_event(p, ir + 0x18, &ev_d, &ev_index, &ev_cycle,
                           &event_type, &event_dw0, &event_dw2, &event_dw3,
                           &event_ptr);
            if (EFI_ERROR(s)) { remember_fail(u"DISABLE SLOT", u"EVENT", s); fatal_running(); }
            if (event_type != TRB_COMMAND_COMPLETION) continue;
            if (((event_dw2 >> 24) & 0xffU) != CC_SUCCESS ||
                event_ptr != cmd_addr ||
                ((event_dw3 >> TRB_SLOT_SHIFT) & 0xffU) != slot) {
                s = EFI_DEVICE_ERROR;
                remember_fail(u"DISABLE SLOT", u"COMPLETION", s);
                fatal_running();
            }
            break;
        }
    }

    s = mr32(p, op, &cmd);
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"USBCMD", s); fatal_running(); }
    cmd &= ~(CMD_RUN | CMD_INTE | CMD_HSEE);
    s = mw32(p, op, cmd);
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"STOP", s); fatal_running(); }
    s = wait_hch(p, op, TRUE, 1000, &status);
    if (EFI_ERROR(s)) { remember_fail(u"HALT", u"HCH", s); fatal_running(); }
    halted = TRUE;
    started = FALSE;

    s = reset_xhci(p, op, &cmd, &status);
    if (EFI_ERROR(s)) { remember_fail(u"RESET", u"RECOVERY", s); fatal_running(); }
    if (!(status & STS_HCH) || (status & STS_CNR)) {
        s = EFI_DEVICE_ERROR;
        remember_fail(u"RESET", u"RECOVERY VERIFY", s);
        fatal_running();
    }

    Print(u"V32.2 ADDRESS DEVICE: PASS SLOT=%u / DISABLE SLOT: PASS\r\n", slot);
    Print(u"V32.2 COMPLETE / CUMULATIVE DISCOVERY + PORT RESET + ADDRESS DEVICE\r\n");
    Print(u"NO DESCRIPTORS / NO CONFIGURE ENDPOINT / NO HID REPORTS\r\n");

out:
    if (hs) uefi_call_wrapper(BS->FreePool, 1, hs);

    if (EFI_ERROR(s) && started && !halted)
        fatal_running();

    if (halted) {
        if (p) {
            (void)mw64(p, op + 0x30, 0);
            (void)mw64(p, op + 0x18, 0);
            (void)mw64(p, ir + 0x10, 0);
            (void)mw64(p, ir + 0x18, 0);
            (void)mw32(p, ir + 0x08, 0);
            (void)mw32(p, ir + 0x00, 0);
        }

        if (scratch) {
            for (i = 0; i < scratchpads; ++i)
                (void)dma_free(p, &scratch[i]);
            uefi_call_wrapper(BS->FreePool, 1, scratch);
            scratch = NULL;
        }
        (void)dma_free(p, &ep0_d);
        (void)dma_free(p, &inctx_d);
        (void)dma_free(p, &devctx_d);
        (void)dma_free(p, &erst_d);
        (void)dma_free(p, &ev_d);
        (void)dma_free(p, &cr_d);
        (void)dma_free(p, &spa_d);
        (void)dma_free(p, &dcbaa_d);
    }

    if (EFI_ERROR(s)) {
        Print(u"FAIL STAGE=%s OP=%s STATUS=%r\r\n", fail_stage, fail_op, s);
    } else {
        Print(u"FAIL STAGE=NONE OP=NONE STATUS=Success\r\n");
        Print(u"ALL CONTROLLER POINTERS CLEARED BEFORE DMA RELEASE\r\n");
    }
    Print(u"EXIT 5 SEC...\r\n");
    uefi_call_wrapper(BS->Stall, 1, 5000000);
    return s;
}
