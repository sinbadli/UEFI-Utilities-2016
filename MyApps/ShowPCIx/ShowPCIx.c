//
//  Copyright (c) 2017  Finnbarr P. Murphy.   All rights reserved.
//
//  Show PCI devices using PCI.IDS text-based database
//
//  License: UDK2015 license applies to code from UDK2015 source, 
//           BSD 2 cluase license applies to all other code.
//


#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/ShellCEntryLib.h>
#include <Library/ShellLib.h>
#include <Library/ShellCommandLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>

#include <Protocol/EfiShell.h>
#include <Protocol/PciEnumerationComplete.h>
#include <Protocol/PciRootBridgeIo.h>

#include <IndustryStandard/Pci.h>

#define CALC_EFI_PCI_ADDRESS(Bus, Dev, Func, Reg) \
    ((UINT64) ((((UINTN) Bus) << 24) + (((UINTN) Dev) << 16) + (((UINTN) Func) << 8) + ((UINTN) Reg)))

// all typedefs from UDK2015 sources
#pragma pack(1)
typedef struct {
   UINT16  VendorId;
   UINT16  DeviceId;
   UINT16  Command;
   UINT16  Status;
   UINT8   RevisionId;
   UINT8   ClassCode[3];
   UINT8   CacheLineSize;
   UINT8   PrimaryLatencyTimer;
   UINT8   HeaderType;
   UINT8   Bist;
} PCI_COMMON_HEADER;

typedef struct {
   UINT32  Bar[6];               // Base Address Registers
   UINT32  CardBusCISPtr;        // CardBus CIS Pointer
   UINT16  SubVendorId;          // Subsystem Vendor ID
   UINT16  SubSystemId;          // Subsystem ID
   UINT32  ROMBar;               // Expansion ROM Base Address
   UINT8   CapabilitiesPtr;      // Capabilities Pointer
   UINT8   Reserved[3];
   UINT32  Reserved1;
   UINT8   InterruptLine;        // Interrupt Line
   UINT8   InterruptPin;         // Interrupt Pin
   UINT8   MinGnt;               // Min_Gnt
   UINT8   MaxLat;               // Max_Lat
} PCI_DEVICE_HEADER;

typedef struct {
   UINT32  CardBusSocketReg;     // Cardus Socket/ExCA Base Address Register
   UINT8   CapabilitiesPtr;      // 14h in pci-cardbus bridge.
   UINT8   Reserved;
   UINT16  SecondaryStatus;      // Secondary Status
   UINT8   PciBusNumber;         // PCI Bus Number
   UINT8   CardBusBusNumber;     // CardBus Bus Number
   UINT8   SubordinateBusNumber; // Subordinate Bus Number
   UINT8   CardBusLatencyTimer;  // CardBus Latency Timer
   UINT32  MemoryBase0;          // Memory Base Register 0
   UINT32  MemoryLimit0;         // Memory Limit Register 0
   UINT32  MemoryBase1;
   UINT32  MemoryLimit1;
   UINT32  IoBase0;
   UINT32  IoLimit0;             // I/O Base Register 0
   UINT32  IoBase1;              // I/O Limit Register 0
   UINT32  IoLimit1;
   UINT8   InterruptLine;        // Interrupt Line
   UINT8   InterruptPin;         // Interrupt Pin
   UINT16  BridgeControl;        // Bridge Control
} PCI_CARDBUS_HEADER;

typedef union {
   PCI_DEVICE_HEADER   Device;
   PCI_CARDBUS_HEADER  CardBus;
} NON_COMMON_UNION;

typedef struct {
   PCI_COMMON_HEADER Common;
   NON_COMMON_UNION  NonCommon;
   UINT32            Data[48];
} PCI_CONFIG_SPACE;
#pragma pack()

#define UTILITY_VERSION L"0.8"
#define LINE_MAX  1024

#define EFI_PCI_EMUMERATION_COMPLETE_GUID \
    { 0x30cfe3e7, 0x3de1, 0x4586, {0xbe, 0x20, 0xde, 0xab, 0xa1, 0xb3, 0xb7, 0x93}}


CHAR16 *
GetDeviceDesc( CHAR16 *Line)
{
    CHAR16 *s = Line;
    static CHAR16 DeviceDesc[LINE_MAX];
    CHAR16 *d = DeviceDesc;

    s++;
    while (*s++) {
        if (*s == L' ' || *s == L'\t')
           break;
    }

    while (*s++) {
        if (*s != L' ' && *s != L'\t')
           break;
    }

    while (*s) {
        *(d++) = *(s++);
    }
    *d = 0;

    return DeviceDesc;
}


CHAR16 *
GetVendorDesc( CHAR16 *Line)
{
    CHAR16 *s = Line;
    static CHAR16 VendorDesc[LINE_MAX];
    CHAR16 *d = VendorDesc;

    while (*s++) {
        if (*s == L' ' || *s == L'\t')
            break;
    }

    while (*s++) {
        if (*s != L' ' && *s != L'\t')
            break;
    }

    while (*s) {
        *(d++) = *(s++);
    }
    *d = 0;

    return VendorDesc;
}


//
// Copyed from UDK2015 Source.
//
EFI_STATUS
PciGetNextBusRange( EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR **Descriptors,
                    UINT16 *MinBus,
                    UINT16 *MaxBus,
                    BOOLEAN *IsEnd)
{
    *IsEnd = FALSE;

    if ((*Descriptors) == NULL) {
        *MinBus = 0;
        *MaxBus = PCI_MAX_BUS;
        return EFI_SUCCESS;
    }

    while ((*Descriptors)->Desc != ACPI_END_TAG_DESCRIPTOR) {
        if ((*Descriptors)->ResType == ACPI_ADDRESS_SPACE_TYPE_BUS) {
            *MinBus = (UINT16) (*Descriptors)->AddrRangeMin;
            *MaxBus = (UINT16) (*Descriptors)->AddrRangeMax;
            (*Descriptors)++;
            return (EFI_SUCCESS);
        }

        (*Descriptors)++;
    }

    if ((*Descriptors)->Desc == ACPI_END_TAG_DESCRIPTOR) {
        *IsEnd = TRUE;
    }

    return EFI_SUCCESS;
}


//
// Copyed from UDK2015 Source. 
//
EFI_STATUS
PciGetProtocolAndResource( EFI_HANDLE Handle,
                           EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL **IoDev,
                           EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR **Descriptors)
{
    EFI_STATUS Status;

    Status = gBS->HandleProtocol( Handle,
                                  &gEfiPciRootBridgeIoProtocolGuid,
                                  (VOID**)IoDev);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    Status = (*IoDev)->Configuration (*IoDev, (VOID**)Descriptors);
    if (Status == EFI_UNSUPPORTED) {
        *Descriptors = NULL;
        return EFI_SUCCESS;
    }

    return Status;
}


VOID
LowerCaseStr( CHAR16 *Str)
{
    for (int i = 0; Str[i] != L'\0'; i++) {
        if (Str[i] >= L'A' && Str[i] <= L'Z') {
            Str[i] -= (CHAR16)(L'A' - L'a');
        }
    }
}


BOOLEAN
SearchPciData( SHELL_FILE_HANDLE FileHandle,
               CHAR16 *ReadLine,
               UINTN VendorID, 
               UINTN DeviceID)
{

    EFI_STATUS Status = EFI_SUCCESS;
    BOOLEAN Found = FALSE;
    BOOLEAN VendorFound = FALSE;
    BOOLEAN Ascii = TRUE;
    UINTN   Size = LINE_MAX;
    CHAR16  Vendor[5];
    CHAR16  Device[5];

    UnicodeSPrint(Vendor, sizeof(Vendor), L"%04x", VendorID);
    LowerCaseStr(Vendor);
    UnicodeSPrint(Device, sizeof(Device), L"%04x", DeviceID);
    LowerCaseStr(Device);

    ShellSetFilePosition(FileHandle, 0);

    // read file line by line
    for (;!ShellFileHandleEof(FileHandle); Size = 1024) {
        Status = ShellFileHandleReadLine(FileHandle, ReadLine, &Size, TRUE, &Ascii);
        if (Status == EFI_BUFFER_TOO_SMALL) {
            Status = EFI_SUCCESS;
        } else if (EFI_ERROR(Status)) {
            break;
        }

        // Skip comment and empty lines
        if (ReadLine[0] == L'#' || ReadLine[0] == L' ' || 
            ReadLine[0] == L'\n' || ReadLine[0] == L'\r') {
            continue;
        }
 
        if (StrnCmp(ReadLine, Vendor, 4) == 0) {
            Print(L"     %s", GetVendorDesc(ReadLine));
            VendorFound = TRUE;
        } else if (VendorFound && StrnCmp(&ReadLine[1], Device, 4) == 0) {
            Print(L", %s", GetDeviceDesc(ReadLine));
            Found = TRUE;
            break;
        } else if (VendorFound && (StrnCmp(ReadLine, L"\t", 1) != 0) && 
                  (StrnCmp(ReadLine, L"\t\t", 2) != 0)) {
            break;
        }
    }

    return Found;
} 


VOID
Usage( CHAR16 *Str)
{
    Print(L"Usage: %s [ -v | --verbose ]\n", Str);
    Print(L"       %s [ -h | --help | -V | --version ]\n", Str);
}


INTN
EFIAPI
ShellAppMain(UINTN Argc, CHAR16 **Argv)
{
    EFI_GUID gEfiPciEnumerationCompleteProtocolGuid = EFI_PCI_EMUMERATION_COMPLETE_GUID;  
    EFI_STATUS Status = EFI_SUCCESS;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *IoDev;
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *Descriptors;
    SHELL_FILE_HANDLE InFileHandle = (SHELL_FILE_HANDLE)NULL;
    PCI_COMMON_HEADER PciHeader;
    PCI_CONFIG_SPACE ConfigSpace;
    PCI_DEVICE_HEADER *DeviceHeader;
    CHAR16 FileName[] = L"pci.ids";
    CHAR16 *FullFileName = (CHAR16 *)NULL;
    CHAR16 *ReadLine = (CHAR16 *)NULL;
    VOID *Interface;
    EFI_HANDLE *HandleBuf;
    UINTN HandleBufSize;
    UINTN HandleCount;
    UINTN Size = LINE_MAX;
    UINT16 MinBus, MaxBus;
    UINT64 Address;
    BOOLEAN IsEnd; 
    BOOLEAN Verbose = FALSE;
  
    for (int i = 1; i < Argc; i++) {
        if (!StrCmp(Argv[i], L"--version") ||
            !StrCmp(Argv[i], L"-V")) {
            Print(L"Version: %s\n", UTILITY_VERSION);
            return Status;
        } else if (!StrCmp(Argv[i], L"--verbose") ||
            !StrCmp(Argv[i], L"-v")) {
            Verbose = TRUE;
        } else if (!StrCmp(Argv[i], L"--help") ||
            !StrCmp(Argv[i], L"-h") ||
            !StrCmp(Argv[i], L"-?")) {
            Usage(Argv[0]);
            return Status;
        } else {
            Print(L"ERROR: Unknown option.\n");
            Usage(Argv[0]);
            return Status;
        }
    }


    Status = gBS->LocateProtocol( &gEfiPciEnumerationCompleteProtocolGuid,
                                  NULL,
                                  &Interface);
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: Could not find an enumerated PCI database\n");
        return Status;
    }

    HandleBufSize = sizeof(EFI_HANDLE);
    HandleBuf = (EFI_HANDLE *) AllocateZeroPool( HandleBufSize);
    if (HandleBuf == NULL) {
        Print(L"ERROR: Out of memory resources\n");
        goto Done;
    }

    Status = gBS->LocateHandle( ByProtocol,
                                &gEfiPciRootBridgeIoProtocolGuid,
                                NULL,
                                &HandleBufSize,
                                HandleBuf);

    if (Status == EFI_BUFFER_TOO_SMALL) {
        HandleBuf = ReallocatePool (sizeof (EFI_HANDLE), HandleBufSize, HandleBuf);
        if (HandleBuf == NULL) {
            Print(L"ERROR: Out of memory resources\n");
            goto Done;
        }

        Status = gBS->LocateHandle( ByProtocol,
                                    &gEfiPciRootBridgeIoProtocolGuid,
                                    NULL,
                                    &HandleBufSize,
                                    HandleBuf);
    }

    if (EFI_ERROR (Status)) {
        Print(L"ERROR: Failed to find any PCI handles\n");
        goto Done;
    }

    if (Verbose) {
        FullFileName = ShellFindFilePath(FileName);
        if (FullFileName == NULL) {
            Print(L"ERROR: Could not find %s\n", FileName);
            Status = EFI_NOT_FOUND;
            goto Done;
        }

        // open the file
        Status = ShellOpenFileByName(FullFileName, &InFileHandle, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(Status)) {
            Print(L"ERROR: Could not open %s\n", FileName);
            goto Done;
        }

        // allocate a buffer to read lines into
        ReadLine = AllocateZeroPool(Size);
        if (ReadLine == NULL) {
            Print(L"ERROR: Could not allocate memory\n");
            Status = EFI_OUT_OF_RESOURCES;
            goto Done;
        }
    }

    HandleCount = HandleBufSize / sizeof (EFI_HANDLE);

    for (UINT16 Index = 0; Index < HandleCount; Index++) {
        Status = PciGetProtocolAndResource( HandleBuf[Index],
                                            &IoDev,
                                            &Descriptors);
        if (EFI_ERROR(Status)) {
            Print(L"ERROR: PciGetProtocolAndResource [%d]\n, Status");
            goto Done;
        }
  
        while(1) {
            Status = PciGetNextBusRange( &Descriptors, &MinBus, &MaxBus, &IsEnd);
            if (EFI_ERROR(Status)) {
                Print(L"ERROR: Retrieving PCI bus range [%d]\n", Status);
                goto Done;
            }

            if (IsEnd) {
                break;
            }

            Print(L"\n");
            Print(L"Bus    Vendor   Device  Subvendor SVDevice\n");
            Print(L"\n");

            for (UINT16 Bus = MinBus; Bus <= MaxBus; Bus++) {
                for (UINT16 Device = 0; Device <= PCI_MAX_DEVICE; Device++) {
                    for (UINT16 Func = 0; Func <= PCI_MAX_FUNC; Func++) {
                         Address = CALC_EFI_PCI_ADDRESS (Bus, Device, Func, 0);

                         Status = IoDev->Pci.Read( IoDev,
                                                   EfiPciWidthUint8,
                                                   Address,
                                                   sizeof(ConfigSpace),
                                                   &ConfigSpace);

                         DeviceHeader = (PCI_DEVICE_HEADER *) &(ConfigSpace.NonCommon.Device);

                         Status = IoDev->Pci.Read( IoDev,
                                                   EfiPciWidthUint16,
                                                   Address,
                                                   1,
                                                   &PciHeader.VendorId);

                         if (PciHeader.VendorId == 0xffff && Func == 0) {
                             break;
                         }

                         if (PciHeader.VendorId != 0xffff) {
                             Status = IoDev->Pci.Read( IoDev,
                                                       EfiPciWidthUint32,
                                                       Address,
                                                       sizeof(PciHeader)/sizeof(UINT32),
                                                       &PciHeader);

                             Print(L" %02d     %04x     %04x     %04x     %04x", 
                                   Bus, PciHeader.VendorId, PciHeader.DeviceId, 
                                   DeviceHeader->SubVendorId, DeviceHeader->SubSystemId);

                             if (Verbose) {
                                 SearchPciData( InFileHandle, 
                                                ReadLine, 
                                                PciHeader.VendorId, 
                                                PciHeader.DeviceId);
                             }

                             Print(L"\n");

                             if (Func == 0 && 
                                ((PciHeader.HeaderType & HEADER_TYPE_MULTI_FUNCTION) == 0x00)) {
                                break;
                             }
                         }
                     }
                 }
             }

            if (Descriptors == NULL) {
                break;
            }
        }
    }

    Print(L"\n");

Done:
    if (HandleBuf != NULL) {
        FreePool(HandleBuf);
    }
    if (Verbose) {
        if (ReadLine != NULL) {
            FreePool(ReadLine);
        }
        if (FullFileName != NULL) {
            FreePool(FullFileName);
        }
        if (InFileHandle != NULL) {
            ShellCloseFile(&InFileHandle);
        }
    }

    return Status;
}
