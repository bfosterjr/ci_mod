#include <ntddk.h>
#include <ntimage.h>
#include "ioctl.h"

#ifndef WINAPI
#define WINAPI __stdcall
#endif


typedef ULONG SYSTEM_INFORMATION_CLASS;

// Class 11
typedef struct _SYSTEM_MODULE_INFORMATION_ENTRY
{
    ULONG  Unknown1;
    ULONG  Unknown2;
#ifdef _WIN64
    ULONG Unknown3;
    ULONG Unknown4;
#endif
    PVOID  Base;
    ULONG  Size;
    ULONG  Flags;
    USHORT  Index;
    USHORT  NameLength;
    USHORT  LoadCount;
    USHORT  PathLength;
    CHAR  ImageName[256];
} SYSTEM_MODULE_INFORMATION_ENTRY, *PSYSTEM_MODULE_INFORMATION_ENTRY;

typedef struct _SYSTEM_MODULE_INFORMATION
{
    ULONG Count;
    SYSTEM_MODULE_INFORMATION_ENTRY Module[1];
} SYSTEM_MODULE_INFORMATION, *PSYSTEM_MODULE_INFORMATION;


NTSTATUS WINAPI ZwQuerySystemInformation
(   
    SYSTEM_INFORMATION_CLASS SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);


static PWSTR        g_driverKey                 = NULL;
static ULONG_PTR    g_cioptionsAddr             = 0;
static ULONG        g_CiOptionsOld              = 0;
static ULONG_PTR    g_SeILSigningPolicyAddr     = 0;
static UCHAR        g_SeILSigningPolicyOld      = 0;
static BOOLEAN      g_patched                   = FALSE;



// Creating my own "spin-lock" to avoid IRQL adjustments
// and having to write high IRQL aware code..

#define LOCKED      1
#define UNLOCKED    0
static volatile LONG g_lock = UNLOCKED;

static
void
_lockAcquire()
{
    LARGE_INTEGER interval;

    //1ms
    interval.QuadPart = 10000;

    //disable APCs
    KeEnterGuardedRegion();

    while (LOCKED == InterlockedCompareExchange(&g_lock, LOCKED, UNLOCKED))
    {
        //spin..with a breather..
        (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
}

static
void
_lockRelease()
{
    InterlockedExchange(&g_lock, UNLOCKED);

    //re-enable APCs
    KeLeaveGuardedRegion();
}

static
NTSTATUS
_getModuleList
(
    PSYSTEM_MODULE_INFORMATION *ppModuleList
)
{
    NTSTATUS    status      = STATUS_INFO_LENGTH_MISMATCH;
    SIZE_T      allocSize   = 0;
    ULONG       retLen      = 0;

    while (status == STATUS_INFO_LENGTH_MISMATCH)
    {
        allocSize = retLen;

        //incase the list grew since the last call
        allocSize += 0x1000;
        status = STATUS_MEMORY_NOT_ALLOCATED;

        if (NULL != *ppModuleList)
        {
            ExFreePool(*ppModuleList);
            *ppModuleList = NULL;
        }

        if (NULL != (*ppModuleList = ExAllocatePool(NonPagedPool, allocSize)))
        {
            status = ZwQuerySystemInformation(11, *ppModuleList, (ULONG)allocSize, &retLen);
        }
    }

    return status;
}

static
ULONG_PTR
_getModBaseSize
(
    PSYSTEM_MODULE_INFORMATION  pModuleList,
    PCHAR                       moduleName,
    PULONG                      pSize
)
{
    ULONG   index   = 0;
    
    for (index = 0; index < pModuleList->Count; index++)
    {
        if (0 == _strnicmp(moduleName, pModuleList->Module[index].ImageName, strlen(moduleName)))
        {
            *pSize = pModuleList->Module[index].Size;
            return (ULONG_PTR)pModuleList->Module[index].Base;
        }
    }

    return 0;
}

static
void
_getRegDWord
(
    PWSTR   key_name, 
    PWSTR   val_name, 
    ULONG*   pvalue
)
{
    ULONG temp = 0;
    RTL_QUERY_REGISTRY_TABLE queryTable[2];
    
    RtlZeroMemory(&queryTable, sizeof(queryTable));

    queryTable[0].QueryRoutine = NULL;
    queryTable[0].Name = val_name;
    queryTable[0].EntryContext = pvalue;
    queryTable[0].DefaultType = REG_DWORD;
    queryTable[0].DefaultData = &temp;
    queryTable[0].DefaultLength = sizeof(temp);
    queryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;

    //end the table entries
    queryTable[1].QueryRoutine = NULL;
    queryTable[1].Name = NULL;

    //ignore the return value for now..
    (void)RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, key_name, &(queryTable[0]), NULL, NULL);
}



static
BOOLEAN
_copyDriverRegKey
(
    PUNICODE_STRING driverKey
)
{
    ULONG allocLen = driverKey->Length + sizeof(WCHAR);

    if (NULL == (g_driverKey = ExAllocatePool(NonPagedPool, allocLen)))
    {
        return FALSE;
    }

    RtlZeroMemory(g_driverKey, allocLen);
    RtlCopyMemory(g_driverKey, driverKey->Buffer, driverKey->Length);

    return TRUE;
}



//Just some simple validation to ensure that the offset
//given is for the 'correct' module and it hasnt' been updated

static
BOOLEAN
_imageTimeStampValidate
(
    ULONG_PTR   imageBase, 
    ULONG       timestamp
)
{
    PIMAGE_DOS_HEADER dosHdr    = (PIMAGE_DOS_HEADER)imageBase;
    PIMAGE_NT_HEADERS ntHdr     = NULL;

    //TODO: no MZ/PE header validation here..

    ntHdr = (PIMAGE_NT_HEADERS)(imageBase + dosHdr->e_lfanew);

    if (ntHdr->FileHeader.TimeDateStamp == timestamp)
    {
        return TRUE;
    }

    return FALSE;

}



static
NTSTATUS
_getPatchAddrs
(
    VOID
)
{
    NTSTATUS                    status              = STATUS_UNSUCCESSFUL;
    PSYSTEM_MODULE_INFORMATION  pModList            = NULL;
    ULONG                       ciOptionsOffset     = 0;
    ULONG                       ciDll_ts            = 0;
    ULONG                       SigningPolicyOffset = 0;
    ULONG                       ntos_ts             = 0;
    ULONG_PTR                   ntos_base           = 0;
    ULONG_PTR                   cidll_base          = 0;
    ULONG                       ntosSize            = 0;
    ULONG                       ciSize              = 0;
    ULONG_PTR                   temp                = 0;


    if (STATUS_SUCCESS != (status = _getModuleList(&pModList)))
    {
        //noting to do
    }
    else if ( 0 == (ntos_base = _getModBaseSize(pModList, "\\SystemRoot\\system32\\ntoskrnl.exe",&ntosSize)) ||
              0 == (cidll_base = _getModBaseSize(pModList, "\\SystemRoot\\system32\\ci.dll", &ciSize)) )
    {
        status = STATUS_NOT_FOUND;
    }
    else
    { 
        //if either address is resolved, consider it a success...
        //.... I have my reasons.. :)

        //patch ci!g_CiOptions
        _getRegDWord(g_driverKey, L"g_CiOptions", &ciOptionsOffset);
        _getRegDWord(g_driverKey, L"cidll_ts", &ciDll_ts);
        if (0 != ciOptionsOffset && 0 != ciDll_ts)
        {
            if (_imageTimeStampValidate(cidll_base, ciDll_ts))
            {
                temp = cidll_base + ciOptionsOffset;
                //TODO: add more validation here.. to ensure that the address 
                //      falls within the  correct section...
                if (temp > cidll_base && (cidll_base + ciSize) > temp)
                {
                    g_cioptionsAddr = temp;
                    status = STATUS_SUCCESS;
                }
            }
        }

        //patch nt!SeIlSigningPolicy
        _getRegDWord(g_driverKey, L"SeILSigningPolicy", &SigningPolicyOffset);
        _getRegDWord(g_driverKey, L"ntos_ts", &ntos_ts);
        if (0 != SigningPolicyOffset && 0 != ntos_ts)
        {
            if (_imageTimeStampValidate(ntos_base, ntos_ts))
            {
                temp = ntos_base + SigningPolicyOffset;
                //TODO: add more validation here.. to ensure that the address 
                //      falls within the correct section...
                if (temp > ntos_base && (ntos_base + ntosSize) > temp)
                {
                    g_SeILSigningPolicyAddr = temp;
                    status = STATUS_SUCCESS;
                }
            }

        }
    }

    if (NULL != pModList)
    {
        ExFreePool(pModList);
        pModList = NULL;
    }

    return status;
}


static
NTSTATUS
_patchKernelModules
(
    VOID
)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    _lockAcquire();

    __try
    {
        if (!g_patched && ( 0 != g_cioptionsAddr || 0 != g_SeILSigningPolicyAddr) )
        {

            if (0 != g_cioptionsAddr)
            {
                g_CiOptionsOld = *((PULONG)g_cioptionsAddr);
                //2 = TEST SIGNING
                *((PULONG)g_cioptionsAddr) = 2;
            }

            if (0 != g_SeILSigningPolicyAddr)
            {
                g_SeILSigningPolicyOld = *((PUCHAR)g_SeILSigningPolicyAddr);
                //0 = UNSIGNED
                *((PUCHAR)g_SeILSigningPolicyAddr) = 0;
            }

            status = STATUS_SUCCESS;
            g_patched = TRUE;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = STATUS_FAIL_FAST_EXCEPTION;
    }

    _lockRelease();
    return status;
}

static
NTSTATUS
_restoreKernelModules
(
VOID
)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    _lockAcquire();

    __try
    {

        if (g_patched)
        {
            if (0 != g_SeILSigningPolicyAddr)
            {
                *((PUCHAR)g_SeILSigningPolicyAddr) = g_SeILSigningPolicyOld;
            }

            if (0 != g_cioptionsAddr)
            {
                *((PULONG)g_cioptionsAddr) = g_CiOptionsOld;
            }
            status = STATUS_SUCCESS;
            g_patched = FALSE;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = STATUS_FAIL_FAST_EXCEPTION;
    }

    _lockRelease();
    return status;
}


static
NTSTATUS
_deviceIoctl
(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
)
{
    NTSTATUS            status          = STATUS_INVALID_DEVICE_REQUEST;
    PIO_STACK_LOCATION  irpSp           = NULL; 
    ULONG               inBufLength     = 0;
    ULONG               outBufLength    = 0;
    PCHAR               inBuf           = NULL;
    PCHAR               outBuf          = NULL;

    UNREFERENCED_PARAMETER(DeviceObject);

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    inBufLength = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    outBufLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;

    if (!inBufLength || !outBufLength )
    {
        status = STATUS_INVALID_PARAMETER;
    }
    else
    {
        switch (irpSp->Parameters.DeviceIoControl.IoControlCode)
        {
        case IOCTL_CI_MOD_TOGGLE:

            inBuf = Irp->AssociatedIrp.SystemBuffer;
            outBuf = Irp->AssociatedIrp.SystemBuffer;

            if (sizeof(ULONG) != outBufLength)
            {
                status = STATUS_INVALID_PARAMETER;
            }
            else if (g_patched)
            {
                status = _restoreKernelModules();
            }
            else
            {
                status = _patchKernelModules();
            }

            if (STATUS_SUCCESS == status)
            {
                *((PULONG)outBuf) = g_patched ? 1 : 0;
                Irp->IoStatus.Information = sizeof(ULONG);
            }
            break;
        case IOCTL_CI_MOD_CRASH:
            //just break..  :)
            __debugbreak();
            break;
        default:
            break;
        }
    }

    Irp->IoStatus.Status = status;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static
NTSTATUS
_createClose
(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PAGED_CODE();

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


static
VOID
_cleanup
(
    PDEVICE_OBJECT devObj
)
{
    UNICODE_STRING dosDevName;
    RtlInitUnicodeString(&dosDevName, DOS_DEVICE_NAME);

    (void)IoDeleteSymbolicLink(&dosDevName);

    if (NULL != g_driverKey)
    {
        ExFreePool(g_driverKey);
        g_driverKey = NULL;
    }
    if (devObj != NULL)
    {
        IoDeleteDevice(devObj);
    }
}

static
VOID 
_unload
(
    PDRIVER_OBJECT DriverObject
)
{
    _restoreKernelModules();
    _cleanup(DriverObject->DeviceObject);
    return;
}

NTSTATUS 
DriverEntry
(
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPathName
)
{
    NTSTATUS            status      = STATUS_UNSUCCESSFUL;
    RTL_OSVERSIONINFOW  verInfo;
    UNICODE_STRING      devName;
    UNICODE_STRING      dosDevName;
    PDEVICE_OBJECT      devObj      = NULL;
   
    DriverObject->DriverUnload = _unload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = _createClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = _createClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = _deviceIoctl;

    RtlInitUnicodeString(&devName, NT_DEVICE_NAME);
    RtlInitUnicodeString(&dosDevName, DOS_DEVICE_NAME);


    if (STATUS_SUCCESS != (status = RtlGetVersion(&verInfo)))
    {
        //should not fail..
    }
    //6.2 == Windows 8 / RT
    //6.3 == Windows 8.1 / RT 8.1
    else if (verInfo.dwMajorVersion != 6 || verInfo.dwMinorVersion < 2)
    {
        status = STATUS_INVALID_KERNEL_INFO_VERSION;
    }
    //copy the driver key.. 
    else if (!_copyDriverRegKey(RegistryPathName))
    {
        status = STATUS_NO_MEMORY;
    }
    //find the patch addresses..
    else if (STATUS_SUCCESS != (status = _getPatchAddrs()))
    {
        //error already captured
    }
    //ceate the device
    else if (!NT_SUCCESS(status = IoCreateDevice(DriverObject, 0, &devName,
                                        FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                        FALSE, &devObj)))
    {
        //error already captured
    }
    //give it an accessable dos name
    else if (!NT_SUCCESS(status = IoCreateSymbolicLink(&dosDevName, &devName)))
    {
        //error already captured
    }
    else
    {
        //patch them!
        status = _patchKernelModules();
    }

    if (status != STATUS_SUCCESS)
    {
        _cleanup(devObj);
    }

    return status;
}