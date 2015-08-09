

#include <Windows.h>
#include <stdio.h>
#include <ctype.h>

#include "ci_mod_drv\ioctl.h"


#define PRINT_ERROR(msg)    printf("[-]ERROR (%d) : %s\n", GetLastError(), (msg))

#define CI_MOD_MAJ  0
#define CI_MOD_MIN  3

static
BOOL
_callPatchDriver
(
    HANDLE  hDev,
    DWORD   ioctlCode
)
{
    DWORD patched       = 0;
    DWORD bytesRet      = 0;
    DWORD dummyInput    = 0;
    if (!DeviceIoControl(hDev, ioctlCode, &dummyInput, sizeof(dummyInput),
        &patched, sizeof(patched), &bytesRet, NULL))
    {
        PRINT_ERROR("failed to send device ioctl");
        return FALSE;
    }
    printf("[+]CI/NTOS is now %s\n", (patched == 1) ? "PATCHED" : "RESTORED");
    return TRUE;
}

VOID 
__cdecl
main
(
    ULONG argc,
    PCHAR argv[]
)
{
    HANDLE  hDev        = NULL;
    DWORD   ioctlCode   = (DWORD)IOCTL_CI_MOD_TOGGLE;
    int     input       = 0;
    int     upperInput  = 0;

    printf("[[ CI_MOD (%d.%d) by bfosterjr ]]\n", CI_MOD_MAJ, CI_MOD_MIN);

    if (INVALID_HANDLE_VALUE == (hDev = CreateFile(WIN32_DEV_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                                                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)))
    {
        PRINT_ERROR("failed to open device (is the driver loaded?)");
    }
    else
    {
        printf("[+]Please press 'T' to toggle patching, 'C' to crash, or 'Q' quit: ");
        while (EOF != (input = getchar()))
        {
            upperInput = toupper(input);
            if ('T' == upperInput || 'C' == upperInput)
            {
                ioctlCode = ('C' == upperInput) ? (DWORD)IOCTL_CI_MOD_CRASH : (DWORD) IOCTL_CI_MOD_TOGGLE;

                if (!_callPatchDriver(hDev,ioctlCode))
                {
                    break;
                }
            }
            else if ('Q' == upperInput)
            {
                printf("[+]Quitting..\n");
                break;
            }
            else if ('\n' == input)
            {
                printf("[+]Please press 'T' to toggle patching -or- 'Q' quit: ");
            }
            else
            {
                printf("[-]Invalid input... \n");
            }
        }
        CloseHandle(hDev);
    }

}