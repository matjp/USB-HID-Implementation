#include <efi.h>
#include <efilib.h>
#include <stdarg.h>

#define PAGER_DEFAULT_ROWS 25U
#define PAGER_RESERVED_ROWS 1U
#define PAGER_MAX_BUFFER 2048U

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
        page_lines = 0;
    } else if (page_lines >= page_usable_rows) {
        page_wait();
    }

    return r;
}

#define Print paged_Print
#define XHCI_V32_NO_FINAL_DELAY 1
#define efi_main xhci_v32_3_main
#include "xhci_mmio_v32.3.c"
#undef efi_main
#undef XHCI_V32_NO_FINAL_DELAY
#undef Print

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    pager_init();
    return xhci_v32_3_main(image, st);
}
