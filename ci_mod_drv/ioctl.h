#define IOCTL_TYPE 0x8000
//
// The IOCTL function codes from 0x800 to 0xFFF are for customer use.
//


#define IOCTL_CI_MOD_TOGGLE \
    CTL_CODE( IOCTL_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS  )

#define DRIVER_NAME       "cimoddrv"
#define NT_DEVICE_NAME      L"\\Device\\cimoddrv"
#define DOS_DEVICE_NAME     L"\\DosDevices\\cimoddrv"
#define WIN32_DEV_NAME      "\\\\.\\cimoddrv"