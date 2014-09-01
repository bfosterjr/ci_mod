

#include <Windows.h>
#include <stdio.h>
#include <ctype.h>

#include "ci_mod_drv\ioctl.h"


#define PRINT_ERROR(msg)    printf("[-]ERROR (%d) : %s\n", GetLastError(), (msg))

#define CI_MOD_MAJ  0
#define CI_MOD_Min  1

VOID 
__cdecl
main
(
    ULONG argc,
    PCHAR argv[]
)
{
    HANDLE  hDev        = NULL;
    DWORD   patched     = 0;
    DWORD   dummyInput  = 0;
    DWORD   bytesRet    = 0;
    int     input       = 0;

    printf("[[ CI_MOD (%d.%d) by bfosterjr ]]\n", CI_MOD_MAJ, CI_MOD_Min);

    if (INVALID_HANDLE_VALUE == (hDev = CreateFile(WIN32_DEV_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                                                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)))
    {
        PRINT_ERROR("failed to open device (is the driver loaded?)");
    }
    else
    {
        printf("[+]Please press 'T' to toggle patching -or- 'Q' quit: ");
        while (EOF != (input = getchar()))
        {
            if ('T' == toupper(input))
            {
                if (!DeviceIoControl(hDev, (DWORD)IOCTL_CI_MOD_TOGGLE, &dummyInput, sizeof(dummyInput),
                                    &patched, sizeof(patched), &bytesRet, NULL))
                {
                    PRINT_ERROR("failed to send device ioctl");
                    break;
                }
                else
                {
                    printf("[+]CI/NTOS is now %s\n", (patched == 1) ? "PATCHED" : "RESTORED");
                }
            }
            else if ('Q' == toupper(input))
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