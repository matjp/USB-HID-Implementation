#ifndef V28_USB_IO_COMPAT_H
#define V28_USB_IO_COMPAT_H

#include <efi.h>

/*
 * Minimal UEFI USB I/O protocol declarations required by V28.
 *
 * GNU-EFI does not ship the EDK2-style UsbIo.h header used by newer UEFI
 * development environments, so V28 keeps only the protocol ABI it consumes.
 * The GUID, descriptor layouts, enum values, and method ordering follow the
 * UEFI USB I/O protocol definition.
 */

#define EFI_USB_IO_PROTOCOL_GUID \
    { 0x2B2F68D6, 0x0CD2, 0x44cf, \
      { 0x8E, 0x8B, 0xBB, 0xA2, 0x0B, 0x1B, 0x5B, 0x75 } }

typedef struct {
    UINT8 RequestType;
    UINT8 Request;
    UINT16 Value;
    UINT16 Index;
    UINT16 Length;
} EFI_USB_DEVICE_REQUEST;

typedef struct {
    UINT8 Length;
    UINT8 DescriptorType;
    UINT16 BcdUSB;
    UINT8 DeviceClass;
    UINT8 DeviceSubClass;
    UINT8 DeviceProtocol;
    UINT8 MaxPacketSize0;
    UINT16 IdVendor;
    UINT16 IdProduct;
    UINT16 BcdDevice;
    UINT8 StrManufacturer;
    UINT8 StrProduct;
    UINT8 StrSerialNumber;
    UINT8 NumConfigurations;
} EFI_USB_DEVICE_DESCRIPTOR;

typedef struct {
    UINT8 Length;
    UINT8 DescriptorType;
    UINT16 TotalLength;
    UINT8 NumInterfaces;
    UINT8 ConfigurationValue;
    UINT8 Configuration;
    UINT8 Attributes;
    UINT8 MaxPower;
} EFI_USB_CONFIG_DESCRIPTOR;

typedef struct {
    UINT8 Length;
    UINT8 DescriptorType;
    UINT8 InterfaceNumber;
    UINT8 AlternateSetting;
    UINT8 NumEndpoints;
    UINT8 InterfaceClass;
    UINT8 InterfaceSubClass;
    UINT8 InterfaceProtocol;
    UINT8 Interface;
} EFI_USB_INTERFACE_DESCRIPTOR;

typedef struct {
    UINT8 Length;
    UINT8 DescriptorType;
    UINT8 EndpointAddress;
    UINT8 Attributes;
    UINT16 MaxPacketSize;
    UINT8 Interval;
} EFI_USB_ENDPOINT_DESCRIPTOR;

typedef enum {
    EfiUsbDataIn,
    EfiUsbDataOut,
    EfiUsbNoData
} EFI_USB_DATA_DIRECTION;

typedef struct _EFI_USB_IO_PROTOCOL EFI_USB_IO_PROTOCOL;

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_CONTROL_TRANSFER)(
    EFI_USB_IO_PROTOCOL *This,
    EFI_USB_DEVICE_REQUEST *Request,
    EFI_USB_DATA_DIRECTION Direction,
    UINT32 Timeout,
    VOID *Data,
    UINTN DataLength,
    UINT32 *Status);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_DEVICE_DESCRIPTOR)(
    EFI_USB_IO_PROTOCOL *This,
    EFI_USB_DEVICE_DESCRIPTOR *DeviceDescriptor);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_CONFIG_DESCRIPTOR)(
    EFI_USB_IO_PROTOCOL *This,
    EFI_USB_CONFIG_DESCRIPTOR *ConfigurationDescriptor);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_INTERFACE_DESCRIPTOR)(
    EFI_USB_IO_PROTOCOL *This,
    EFI_USB_INTERFACE_DESCRIPTOR *InterfaceDescriptor);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_ENDPOINT_DESCRIPTOR)(
    EFI_USB_IO_PROTOCOL *This,
    UINT8 EndpointIndex,
    EFI_USB_ENDPOINT_DESCRIPTOR *EndpointDescriptor);

/*
 * Preserve the complete protocol prefix through the methods V28 does not
 * call. This keeps the offsets of UsbGet* identical to the UEFI-defined ABI.
 */
typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_BULK_TRANSFER)(
    EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN *, UINTN, UINT32 *);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_ASYNC_INTERRUPT_TRANSFER)(
    EFI_USB_IO_PROTOCOL *, UINT8, BOOLEAN, UINTN, UINTN, VOID *, VOID *);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_SYNC_INTERRUPT_TRANSFER)(
    EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN *, UINTN, UINT32 *);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_ISOCHRONOUS_TRANSFER)(
    EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN, UINT32 *);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_ASYNC_ISOCHRONOUS_TRANSFER)(
    EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN, VOID *, VOID *);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_STRING_DESCRIPTOR)(
    EFI_USB_IO_PROTOCOL *, UINT16, UINT8, CHAR16 **);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_SUPPORTED_LANGUAGE)(
    EFI_USB_IO_PROTOCOL *, UINT16 **, UINT16 *);

typedef EFI_STATUS
(EFIAPI *EFI_USB_IO_PORT_RESET)(
    EFI_USB_IO_PROTOCOL *);

struct _EFI_USB_IO_PROTOCOL {
    EFI_USB_IO_CONTROL_TRANSFER UsbControlTransfer;
    EFI_USB_IO_BULK_TRANSFER UsbBulkTransfer;
    EFI_USB_IO_ASYNC_INTERRUPT_TRANSFER UsbAsyncInterruptTransfer;
    EFI_USB_IO_SYNC_INTERRUPT_TRANSFER UsbSyncInterruptTransfer;
    EFI_USB_IO_ISOCHRONOUS_TRANSFER UsbIsochronousTransfer;
    EFI_USB_IO_ASYNC_ISOCHRONOUS_TRANSFER UsbAsyncIsochronousTransfer;

    EFI_USB_IO_GET_DEVICE_DESCRIPTOR UsbGetDeviceDescriptor;
    EFI_USB_IO_GET_CONFIG_DESCRIPTOR UsbGetConfigDescriptor;
    EFI_USB_IO_GET_INTERFACE_DESCRIPTOR UsbGetInterfaceDescriptor;
    EFI_USB_IO_GET_ENDPOINT_DESCRIPTOR UsbGetEndpointDescriptor;
    EFI_USB_IO_GET_STRING_DESCRIPTOR UsbGetStringDescriptor;
    EFI_USB_IO_GET_SUPPORTED_LANGUAGE UsbGetSupportedLanguages;

    EFI_USB_IO_PORT_RESET UsbPortReset;
};

#endif
