#include <efi.h>
#include <efilib.h>
#include <stdarg.h>

static UINTN page_ports = 0;

static void page_wait(const CHAR16 *label)
{
    EFI_INPUT_KEY key;
    EFI_STATUS s;

    Print(u"\r\n%s\r\n", label);
    Print(u"PRESS ANY KEY TO CONTINUE...\r\n");
    for (;;) {
        s = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        if (!EFI_ERROR(s)) break;
        uefi_call_wrapper(BS->Stall, 1, 10000);
    }
}

static UINTN paged_Print(const CHAR16 *fmt, ...)
{
    va_list args;
    UINTN r;

    if (!StrCmp(fmt, u"TOSHIBA xHCI V32.3 / PORT STATE DIAGNOSTIC\r\n")) {
        r = Print(u"TOSHIBA xHCI V32.4 / PORT STATE DIAGNOSTIC\r\n");
        return r;
    }

    va_start(args, fmt);
    r = VPrint(fmt, args);
    va_end(args);

    if (!StrCmp(fmt, u"CAPS: SLOTS=%u PORTS=%u PAGESIZE=4096 AC64=1\r\n")) {
        page_wait(u"PAGE 1 COMPLETE: CONTROLLER IDENTIFICATION + CAPS");
    } else if (!StrCmp(fmt, u"P%u%s %08x CCS=%u PED=%u PR=%u PLS=%u SPD=%u CSC=%u PRC=%u PWR=%u\r\n")) {
        ++page_ports;
        if (page_ports == 10)
            page_wait(u"PAGE 2 COMPLETE: PORTS 1-10");
    }

    return r;
}

#define Print paged_Print
#define efi_main xhci_v32_3_main
#include "xhci_mmio_v32.3.c"
#undef efi_main
#undef Print

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    return xhci_v32_3_main(image, st);
}
