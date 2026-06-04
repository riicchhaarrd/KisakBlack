#pragma once

#if defined(__EMSCRIPTEN__)
int __cdecl R_AcquireDXDeviceOwnership(void (__cdecl *pumpfunc)());
int __cdecl R_ReleaseDXDeviceOwnership();
void __cdecl R_AssertDXDeviceOwnership();
#endif
