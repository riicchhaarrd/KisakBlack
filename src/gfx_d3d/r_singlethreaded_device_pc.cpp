#include "r_singlethreaded_device_pc.h"

#if defined(__EMSCRIPTEN__)

#include <Windows.h>
#include <qcommon/threads.h>
#include <win32/win_common.h>
#include <win32/win_net.h>
#include <universal/assertive.h>
#include <universal/profile.h>

int g_AcquisitionCount;
unsigned __int64 g_DXDeviceThread;

int __cdecl R_AcquireDXDeviceOwnership(void (__cdecl *pumpfunc)())
{
    //PROF_SCOPED("R_AcquireDXDeviceOwnership"); 4 MILLION CALLS PER SECOND. THIS LAGS.

    unsigned __int64 current_thread; // [esp+0h] [ebp-8h]

    current_thread = GetCurrentThreadId();
    if ( current_thread == g_DXDeviceThread )
        return 0;
    if ( Sys_IsMainThread() )
    {
        while ( !Sys_TryEnterCriticalSection(CRITSECT_DXDEVICE) )
        {
            PROF_SCOPED("R_AcquireDXDeviceOwnership Pump&Sleep")
#if defined(__EMSCRIPTEN__)
            extern unsigned long g_kbDevSpin; ++g_kbDevSpin;   // frontend stuck waiting for device (freeze diag)
#endif
            if ( pumpfunc )
                pumpfunc();
            NET_Sleep(1u);
        }
    }
    else
    {
        Sys_EnterCriticalSection(CRITSECT_DXDEVICE);
    }
    Sys_EnterCriticalSection(CRITSECT_DXDEVICE_GLOB);
    ++g_AcquisitionCount;
    g_DXDeviceThread = current_thread;
    Sys_LeaveCriticalSection(CRITSECT_DXDEVICE_GLOB);
    return 1;
}

int __cdecl R_ReleaseDXDeviceOwnership()
{
    PROF_SCOPED("R_ReleaseDXDeviceOwnership");

    Sys_EnterCriticalSection(CRITSECT_DXDEVICE_GLOB);
    if ( g_DXDeviceThread == GetCurrentThreadId() )
    {
        if ( g_DXDeviceThread != GetCurrentThreadId()
            && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_singlethreaded_device_pc.cpp",
                        67,
                        0,
                        "%s",
                        "g_DXDeviceThread == tlGetCurrentThreadId()") )
        {
            __debugbreak();
        }
        if ( g_AcquisitionCount != 1
            && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_singlethreaded_device_pc.cpp",
                        68,
                        0,
                        "%s",
                        "g_AcquisitionCount == 1") )
        {
            __debugbreak();
        }
        if ( !--g_AcquisitionCount )
            g_DXDeviceThread = 0;
        Sys_LeaveCriticalSection(CRITSECT_DXDEVICE);
        Sys_LeaveCriticalSection(CRITSECT_DXDEVICE_GLOB);
        return 1;
    }
    else
    {
        Sys_LeaveCriticalSection(CRITSECT_DXDEVICE_GLOB);
        return 0;
    }
}

void __cdecl R_AssertDXDeviceOwnership()
{
    if ( !Sys_IsRenderThread()
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_singlethreaded_device_pc.cpp",
                    89,
                    0,
                    "%s",
                    "Sys_IsRenderThread()") )
    {
        __debugbreak();
    }
    Sys_EnterCriticalSection(CRITSECT_DXDEVICE_GLOB);
    if ( g_DXDeviceThread != GetCurrentThreadId()
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_singlethreaded_device_pc.cpp",
                    94,
                    0,
                    "%s",
                    "g_DXDeviceThread == GetCurrentThreadId()") )
    {
        __debugbreak();
    }
    Sys_LeaveCriticalSection(CRITSECT_DXDEVICE_GLOB);
}

#endif
