#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include <stdarg.h>
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

#define CRCR_RCS 0x00000001ULL
#define TRB_CYCLE 0x00000001U
#define TRB_TYPE_SHIFT 10U
#define TRB_LINK 6U
#define TRB_LINK_TOGGLE 0x00000002U
#define EVENT_TRBS 16U
#define CMD_TRBS 256U

#define PORTSC_BASE 0x400U
#define PORT_CCS (1U << 0)
#define PORT_PED (1U << 1)
#define PORT_PR (1U << 4)
#define PORT_PLS_MASK (0xFU << 5)
#define PORT_POWER (1U << 9)
#define PORT_SPEED_MASK (0xFU << 10)
#define PORT_PRC (1U << 21)
#define PORT_CSC (1U << 17)

#define USB_CLASS_HID 3U
#define HID_SUBCLASS_BOOT 1U
#define HID_PROTOCOL_KEYBOARD 1U
#define DP_TYPE_MESSAGING 3U
#define DP_SUBTYPE_USB 5U
#define DP_TYPE_END 0x7FU

#define PAGER_DEFAULT_ROWS 25U
#define PAGER_RESERVED_ROWS 1U

static UINTN page_rows = PAGER_DEFAULT_ROWS;
static UINTN page_usable_rows = PAGER_DEFAULT_ROWS - PAGER_RESERVED_ROWS;
static UINTN page_lines = 0;

static void pager_clear(void)
{
    if (ST->ConOut && ST->ConOut->ClearScreen)
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
}

static void pager_init(void)
{
    UINTN columns = 0;
    UINTN rows = 0;
    EFI_STATUS s;

    if (ST->ConOut && ST->ConOut->QueryMode && ST->ConOut->Mode) {
        s = uefi_call_wrapper(ST->ConOut->QueryMode, 4,
                              ST->ConOut,
                              ST->ConOut->Mode->Mode,
                              &columns, &rows);
        if (!EFI_ERROR(s) && rows >= 2)
            page_rows = rows;
    }

    page_usable_rows = (page_rows > PAGER_RESERVED_ROWS)
                     ? page_rows - PAGER_RESERVED_ROWS
                     : 1;
    page_lines = 0;
    pager_clear();
}

static void pager_wait(void)
{
    EFI_INPUT_KEY key;
    EFI_STATUS s;

    Print(u"PRESS A KEY\r\n");
    for (;;) {
        s = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        if (!EFI_ERROR(s)) break;
        uefi_call_wrapper(BS->Stall, 1, 10000);
    }
    pager_clear();
    page_lines = 0;
}

static UINTN count_lines(const CHAR16 *text)
{
    UINTN lines = 0;
    const CHAR16 *p;

    for (p = text; *p; ++p)
        if (*p == u'\n') ++lines;
    return lines;
}

static UINTN paged_Print(const CHAR16 *fmt, ...)
{
    va_list args;
    UINTN lines;
    UINTN r;

    lines = count_lines(fmt);
    if (lines && page_lines && page_lines + lines > page_usable_rows)
        pager_wait();

    va_start(args, fmt);
    r = VPrint(fmt, args);
    va_end(args);

    if (ST->ConOut && ST->ConOut->Mode && ST->ConOut->Mode->CursorRow >= 0)
        page_lines = (UINTN)ST->ConOut->Mode->CursorRow;
    else
        page_lines += lines;

    if (page_lines >= page_usable_rows)
        pager_wait();

    return r;
}

static void pager_finish(void)
{
    if (page_lines)
        pager_wait();
    else
        pager_wait();
}

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID UsbIoGuid = EFI_USB_IO_PROTOCOL_GUID;

struct dma_obj {
    VOID *host;
    VOID *map;
    EFI_PHYSICAL_ADDRESS dev;
    UINTN pages;
    BOOLEAN live;
};

struct discovery {
    UINT8 port;
    UINT8 endpoint;
    UINT16 mps;
    UINT16 vid;
    UINT16 pid;
    UINT8 interval;
};

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

static EFI_STATUS reset_xhci(EFI_PCI_IO_PROTOCOL *p, UINT32 op)
{
    EFI_STATUS s;
    UINT32 cmd, st;
    UINTN i;

    s = mr32(p, op, &cmd);
    if (EFI_ERROR(s)) return s;
    cmd &= ~(CMD_RUN | CMD_INTE | CMD_HSEE);
    cmd |= CMD_RESET;
    s = mw32(p, op, cmd);
    if (EFI_ERROR(s)) return s;

    for (i = 0; i < 1000; ++i) {
        s = mr32(p, op, &cmd);
        if (EFI_ERROR(s)) return s;
        if (!(cmd & CMD_RESET)) break;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    if (cmd & CMD_RESET) return EFI_TIMEOUT;

    for (i = 0; i < 10000; ++i) {
        s = mr32(p, op + 4, &st);
        if (EFI_ERROR(s)) return s;
        if (!(st & STS_CNR)) return EFI_SUCCESS;
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return EFI_TIMEOUT;
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
        EFI_DEVICE_PATH_PROTOCOL *path;
        EFI_USB_ENDPOINT_DESCRIPTOR ep;
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

        path = DevicePathFromHandle(hs[i]);
        port = root_port(path);
        if (!port) continue;

        d->port = port;
        d->vid = dd.IdVendor;
        d->pid = dd.IdProduct;
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
    return found == 1 ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static void init_ring_link(struct dma_obj *d)
{
    UINT32 *r = (UINT32 *)d->host;
    UINTN n = (EVENT_TRBS - 1U) * 4U;
    UINT64 base = d->dev;
    r[n + 0] = (UINT32)base;
    r[n + 1] = (UINT32)(base >> 32);
    r[n + 2] = 0;
    r[n + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_LINK_TOGGLE | TRB_CYCLE;
}

static void init_cmd_ring(struct dma_obj *d)
{
    UINT32 *r = (UINT32 *)d->host;
    UINTN n = (CMD_TRBS - 1U) * 4U;
    UINT64 base = d->dev;
    r[n + 0] = (UINT32)base;
    r[n + 1] = (UINT32)(base >> 32);
    r[n + 2] = 0;
    r[n + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_LINK_TOGGLE | TRB_CYCLE;
}

static UINT32 port_speed(UINT32 x)
{
    return (x & PORT_SPEED_MASK) >> 10;
}

static UINT32 port_pls(UINT32 x)
{
    return (x & PORT_PLS_MASK) >> 5;
}

static void print_port(UINT32 port, UINT32 x, BOOLEAN selected)
{
    Print(u"P%u%s %08x CCS=%u PED=%u PR=%u PLS=%u SPD=%u CSC=%u PRC=%u PWR=%u\r\n",
          port, selected ? u"*" : u" ", x,
          (x & PORT_CCS) ? 1 : 0,
          (x & PORT_PED) ? 1 : 0,
          (x & PORT_PR) ? 1 : 0,
          port_pls(x), port_speed(x),
          (x & PORT_CSC) ? 1 : 0,
          (x & PORT_PRC) ? 1 : 0,
          (x & PORT_POWER) ? 1 : 0);
}

#define Print paged_Print

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_HANDLE *hs = NULL;
    EFI_PCI_IO_PROTOCOL *p = NULL;
    EFI_STATUS s = EFI_SUCCESS;
    UINTN n = 0, i;
    UINT32 id = 0, cls = 0, bar0 = 0, bar1 = 0;
    UINT32 cap = 0, hcs1 = 0, hcc = 0, op = 0;
    UINT32 status = 0, maxslots = 0, pagesize = 0;
    UINT32 rtsoff = 0, xhci_rt = 0;
    UINT32 ports = 0, portsc = 0;
    UINT16 ver = 0;
    BOOLEAN started = FALSE;
    struct discovery d = {0};
    struct dma_obj dcbaa_d = {0}, cr_d = {0}, ev_d = {0}, erst_d = {0};
    UINT64 *dcbaa = NULL;
    UINT32 *erst_entry = NULL;

    InitializeLib(image, st);
    pager_init();
    Print(u"TOSHIBA xHCI V32.4 / PAGED PORT STATE DIAGNOSTIC\r\n");
    Print(u"V32.2 PRECONDITION FAILURE: UEFI KEYBOARD PRESENT, xHCI CCS=0\r\n");
    Print(u"NO PORT RESET / NO ENABLE SLOT / NO ADDRESS DEVICE\r\n");

    s = discover_keyboard(&d, image);
    if (EFI_ERROR(s)) {
        Print(u"FAIL STAGE=DISCOVERY OP=KEYBOARD STATUS=%r\r\n", s);
        goto out;
    }
    Print(u"DISCOVERY: PORT=%u VID=%04x PID=%04x EP=%02x MPS=%u INT=%u\r\n",
          d.port, d.vid, d.pid, d.endpoint, d.mps, d.interval);

    s = LibLocateHandle(ByProtocol, &PciGuid, NULL, &n, &hs);
    if (EFI_ERROR(s)) goto out;
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
    if (!p) { s = EFI_NOT_FOUND; goto out; }

    s = cfg32(p, 0x10, &bar0);
    if (EFI_ERROR(s) || (bar0 & 1U) || ((bar0 >> 1) & 3U) != 2U) {
        s = EFI_UNSUPPORTED;
        goto out;
    }
    s = cfg32(p, 0x14, &bar1);
    if (EFI_ERROR(s)) goto out;

    s = mr32(p, 0, &cap);
    if (EFI_ERROR(s)) goto out;
    op = cap & 0xffU;
    s = mr16(p, 2, &ver);
    if (EFI_ERROR(s) || ver < XHCI_MIN_VERSION) { s = EFI_UNSUPPORTED; goto out; }
    s = mr32(p, 4, &hcs1);
    if (EFI_ERROR(s)) goto out;
    s = mr32(p, 0x10, &hcc);
    if (EFI_ERROR(s) || !(hcc & HCC_AC64)) { s = EFI_UNSUPPORTED; goto out; }
    s = mr32(p, 0x18, &rtsoff);
    if (EFI_ERROR(s)) goto out;
    xhci_rt = rtsoff & ~0x1fU;
    maxslots = hcs1 & 0xffU;
    ports = (hcs1 >> 24) & 0xffU;
    if (!maxslots || !ports || ports > 32U) { s = EFI_UNSUPPORTED; goto out; }

    s = mr32(p, op + 8, &pagesize);
    if (EFI_ERROR(s) || !(pagesize & 1U)) { s = EFI_UNSUPPORTED; goto out; }
    if ((1U << 12) != 4096U) { s = EFI_UNSUPPORTED; goto out; }

    Print(u"xHCI VERSION=%u.%02u PCI=%04x:%04x BAR=%08x/%08x OPBASE=%02x RTSOFF=%08x\r\n",
          ver >> 8, ver & 0xffU, id & 0xffffU, id >> 16, bar0, bar1, op, rtsoff);
    Print(u"CAPS: SLOTS=%u PORTS=%u PAGESIZE=4096 AC64=1\r\n",
          maxslots, ports);

    s = reset_xhci(p, op);
    if (EFI_ERROR(s)) goto out;
    s = wait_hch(p, op, TRUE, 1000, &status);
    if (EFI_ERROR(s)) goto out;

    s = dma_alloc(p, 1, &dcbaa_d);
    if (EFI_ERROR(s)) goto out;
    dcbaa = (UINT64 *)dcbaa_d.host;
    dcbaa[0] = 0;

    s = dma_alloc(p, 1, &cr_d);
    if (EFI_ERROR(s)) goto out;
    init_cmd_ring(&cr_d);

    s = dma_alloc(p, 1, &ev_d);
    if (EFI_ERROR(s)) goto out;
    init_ring_link(&ev_d);

    s = dma_alloc(p, 1, &erst_d);
    if (EFI_ERROR(s)) goto out;
    erst_entry = (UINT32 *)erst_d.host;
    erst_entry[0] = (UINT32)ev_d.dev;
    erst_entry[1] = (UINT32)(ev_d.dev >> 32);
    erst_entry[2] = EVENT_TRBS;
    erst_entry[3] = 0;

    s = mw32(p, op + 0x38, 1U);
    if (EFI_ERROR(s)) goto out;
    s = mw64(p, op + 0x30, dcbaa_d.dev);
    if (EFI_ERROR(s)) goto out;
    s = mw64(p, op + 0x18, cr_d.dev | CRCR_RCS);
    if (EFI_ERROR(s)) goto out;

    /* Primary interrupter: derive the runtime register base from RTSOFF. */
    s = mw32(p, xhci_rt + 0x08, 1U);
    if (EFI_ERROR(s)) goto out;
    s = mw64(p, xhci_rt + 0x18, ev_d.dev | 8ULL);
    if (EFI_ERROR(s)) goto out;
    s = mw64(p, xhci_rt + 0x10, erst_d.dev);
    if (EFI_ERROR(s)) goto out;

    s = mr32(p, op + 4, &status);
    if (EFI_ERROR(s) || !(status & STS_HCH)) { s = EFI_DEVICE_ERROR; goto out; }

    s = mw32(p, op, CMD_RUN);
    if (EFI_ERROR(s)) goto out;
    started = TRUE;
    s = wait_hch(p, op, FALSE, 1000, &status);
    if (EFI_ERROR(s)) goto fatal;

    Print(u"PORTS AFTER XHCI RESET + START:\r\n");
    for (i = 1; i <= ports; ++i) {
        s = mr32(p, op + PORTSC_BASE + (i - 1U) * 0x10U, &portsc);
        if (EFI_ERROR(s)) goto fatal;
        print_port((UINT32)i, portsc, (UINT8)i == d.port);
    }

    s = mr32(p, op + PORTSC_BASE + (d.port - 1U) * 0x10U, &portsc);
    if (EFI_ERROR(s)) goto fatal;
    Print(u"SELECTED: PORT=%u CCS=%u PED=%u PR=%u PLS=%u SPD=%u CSC=%u PRC=%u RAW=%08x\r\n",
          d.port, (portsc & PORT_CCS) ? 1 : 0, (portsc & PORT_PED) ? 1 : 0,
          (portsc & PORT_PR) ? 1 : 0, port_pls(portsc), port_speed(portsc),
          (portsc & PORT_CSC) ? 1 : 0, (portsc & PORT_PRC) ? 1 : 0, portsc);
    Print(u"DIAGNOSTIC COMPLETE: NO PORTSC WRITES AFTER START\r\n");

    /* Stop cleanly before releasing DMA mappings. */
    s = mw32(p, op, 0);
    if (EFI_ERROR(s)) goto fatal;
    started = FALSE;
    s = wait_hch(p, op, TRUE, 1000, &status);
    if (EFI_ERROR(s)) goto out;

out:
    if (hs) uefi_call_wrapper(BS->FreePool, 1, hs);
    if (started) {
fatal:
        Print(u"\r\nFATAL: XHCI RUNNING STATE UNCERTAIN\r\n");
        Print(u"DMA MAPPINGS RETAINED / NO FREE\r\nMANUAL RECOVERY REQUIRED\r\n");
        for (;;) uefi_call_wrapper(BS->Stall, 1, 1000000);
    }
    dma_free(p, &erst_d);
    dma_free(p, &ev_d);
    dma_free(p, &cr_d);
    dma_free(p, &dcbaa_d);
    if (EFI_ERROR(s))
        Print(u"FAIL STATUS=%r\r\n", s);
    else
        pager_finish();
    return s;
}
