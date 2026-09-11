#include <efi.h>
#include <efilib.h>
#include <stdarg.h>

#define PAGER_DEFAULT_ROWS 25U
#define PAGER_RESERVED_ROWS 1U
#define PAGER_MAX_BUFFER 2048U

static UINTN page_rows = PAGER_DEFAULT_ROWS;
static UINTN page_usable_rows = PAGER_DEFAULT_ROWS - PAGER_RESERVED_ROWS;
static UINTN page_lines = 0;

static UINT32 runtime_base = 0;
static BOOLEAN runtime_base_valid = FALSE;

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

    if (ST->ConOut && ST->ConOut->QueryMode) {
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

static void wait_for_key(void)
{
    EFI_INPUT_KEY key;
    EFI_STATUS s;

    for (;;) {
        s = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        if (!EFI_ERROR(s)) break;
        uefi_call_wrapper(BS->Stall, 1, 10000);
    }
}

static void page_wait(void)
{
    /* The footer occupies one physical screen line. */
    Output(u"PRESS A KEY\r\n");
    wait_for_key();
    pager_clear();
    page_lines = 0;
}

static UINTN count_lines(const CHAR16 *text)
{
    UINTN lines = 0;
    const CHAR16 *p;

    for (p = text; *p; ++p) {
        if (*p == u'\n')
            ++lines;
    }
    return lines;
}

static UINTN paged_Print(const CHAR16 *fmt, ...)
{
    CHAR16 buffer[PAGER_MAX_BUFFER];
    va_list args;
    UINTN r;
    UINTN lines;
    BOOLEAN final_prompt = FALSE;

    va_start(args, fmt);
    r = VSPrint(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (!StrCmp(fmt, u"TOSHIBA xHCI V32.3 / PORT STATE DIAGNOSTIC\r\n"))
        StrCpy(buffer, u"TOSHIBA xHCI V32.4 / PORT STATE DIAGNOSTIC\r\n");

    if (!StrCmp(fmt, u"PASS / 5 SEC...\r\n")) {
        StrCpy(buffer, u"PRESS A KEY\r\n");
        final_prompt = TRUE;
    }

    lines = count_lines(buffer);
    if (lines && page_lines && page_lines + lines > page_usable_rows)
        page_wait();

    Output(buffer);
    page_lines += lines;

    if (final_prompt) {
        wait_for_key();
        pager_clear();
        page_lines = 0;
    } else if (page_lines >= page_usable_rows) {
        page_wait();
    }

    return r;
}

static EFI_STATUS paged_mw32(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT32 v);

#define Print paged_Print
#define mw32 paged_mw32
#define XHCI_V32_NO_FINAL_DELAY 1
#define efi_main xhci_v32_3_main
#include "xhci_mmio_v32.3.c"
#undef efi_main
#undef XHCI_V32_NO_FINAL_DELAY
#undef mw32
#undef Print

static EFI_STATUS paged_mw32(EFI_PCI_IO_PROTOCOL *p, UINT32 o, UINT32 v)
{
    UINT32 actual = o;

    /* V32.3 used fixed primary-interrupter offsets. Translate those
     * accesses through the controller's RTSOFF instead. */
    if (o == 0x28U || o == 0x30U || o == 0x34U ||
        o == 0x38U || o == 0x3cU) {
        if (!runtime_base_valid) {
            UINT32 rtsoff = 0;
            EFI_STATUS s;

            s = uefi_call_wrapper(p->Mem.Read, 6, p, EfiPciIoWidthUint32,
                                  0, (UINT64)0x18U, 1, &rtsoff);
            if (EFI_ERROR(s))
                return s;
            runtime_base = rtsoff & ~0x1fU;
            runtime_base_valid = TRUE;
        }

        actual = runtime_base + (o - 0x20U);
    }

    return uefi_call_wrapper(p->Mem.Write, 6, p, EfiPciIoWidthUint32,
                             0, (UINT64)actual, 1, &v);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    pager_init();
    return xhci_v32_3_main(image, st);
}
