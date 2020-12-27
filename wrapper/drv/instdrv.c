#include <stdio.h>
#include <windows.h>
#include "fxlib.h"

BOOL StopDriver(void)
{
    SC_HANDLE hSCManager = NULL;
    SC_HANDLE hService = NULL;
    SERVICE_STATUS ServiceStatus;
    BOOL bRet = FALSE;

    hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    
    if (hSCManager) {
	hService = OpenService(hSCManager, "MAPMEM", SERVICE_ALL_ACCESS);
	CloseServiceHandle(hSCManager);

	if (hService) {
	    bRet = ControlService(hService, SERVICE_CONTROL_STOP, &ServiceStatus);
	    CloseServiceHandle(hService);
	}
    }

    return bRet;
}

BOOL RemoveDriver(void)
{
    SC_HANDLE hSCManager;
    SC_HANDLE hService;
    BOOL bRet = FALSE;

    StopDriver();

    hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (hSCManager) {
	hService = OpenService(hSCManager, "MAPMEM", SERVICE_ALL_ACCESS);
	CloseServiceHandle(hSCManager);

	if (hService) {
	    bRet = DeleteService(hService);
	    CloseServiceHandle(hService);
	}
    }

    return bRet;
}

BOOL InstallDriver(void)
{
    SC_HANDLE hSCManager = NULL;
    SC_HANDLE hService = NULL;

    hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (hSCManager) {
	hService = CreateService(hSCManager,
		"MAPMEM", "MAPMEM",
		SERVICE_ALL_ACCESS,
		SERVICE_KERNEL_DRIVER,
		SERVICE_AUTO_START,
		SERVICE_ERROR_NORMAL,
		"\%SystemRoot\%\\system32\\drivers\\fxptl.sys",
		NULL, NULL, NULL, NULL, NULL);
	
	CloseServiceHandle(hSCManager);
    }

    if (hService == NULL)
	return FALSE;
    CloseServiceHandle(hService);
    return TRUE;
}

BOOL StartDriver(void)
{
    SC_HANDLE hSCManager;
    SC_HANDLE hService;
    BOOL bRet = FALSE;

    hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (hSCManager) {
	hService = OpenService(hSCManager, "MAPMEM", SERVICE_ALL_ACCESS);
	CloseServiceHandle(hSCManager);

	if (hService) {
	    bRet = StartService(hService, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
	    CloseServiceHandle(hService);
	}
    }

    return bRet;
}

static volatile unsigned long *ptm;
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    DRVFUNC drv;
    OSVERSIONINFO osInfo = { .dwOSVersionInfoSize = sizeof(OSVERSIONINFO) };
    unsigned long linear, length = 0x1000;

    GetVersionEx(&osInfo);
    if (osInfo.dwPlatformId == VER_PLATFORM_WIN32_NT)
        kmdDrvInit(&drv);
    else {
        printf("Platform Id != WIN32_NT\n");
        return 1;
    }

    RemoveDriver();

    if (!InstallDriver())
	return 1;
    printf("Driver installed\n");
    if (!StartDriver())
	return 1;
    printf("Driver started\n");

    if (!drv.Init())
	return 1;
    printf("fxLibInit OK\n");

    if(!drv.MapLinear(0, 0xfbdff000, &linear, &length))
	return 1;
    printf("fxMapLinear OK, %08lx\n", linear);

    ptm = (unsigned long *)linear;
    ptm[0xfbcU >> 2] = (0xa0UL << 12) | 0x243;


    drv.UnmapLinear(linear, length);
    drv.Fini();

    return 0;
}

