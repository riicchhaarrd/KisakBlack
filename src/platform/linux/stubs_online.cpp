// ============================================================================
// stubs_online.cpp
//
// Offline single-player stubs for the Steam / online / voice-chat layer of the
// decompiled Call of Duty: Black Ops engine on Linux.
//
// This translation unit DEFINES the undefined Steam / Live / Voice / GamerProfile
// / Content symbols so the engine links and boots without Steamworks, online,
// or voice. Every stub returns a benign "offline" value: logged-out / false /
// 0 / nullptr.
//
// Each definition matches the engine-header declaration EXACTLY (return type,
// parameter types, namespace, const-ness, calling convention) so the mangled /
// unmangled symbol name resolves the corresponding undefined reference.
//
// The list of symbols this file must satisfy is /tmp/syms_online.txt.
// ============================================================================

// --- Engine headers that declare the symbols defined below --------------------
// (Included so types such as SessionData_s, profileWriteState_t, GamerSettingState,
//  netadr_t, HSteamUser, SteamAPICall_t, CCallbackBase resolve to the real types.)

#include <steam/steam_api.h>          // SteamAPI_* / SteamInternal_* / SteamGameServer_*
#include <win32/win_steam.h>          // Steam_* (ticket / client) + g_steamInitialized
#include <win32/win_voice.h>          // Voice_* + Live_GetClientNumForXuid + voice_current_scaler
#include <win32/win_gamerprofile.h>   // GamerProfile_* + profileWriteState_t + GamerSettingState
#include <win32/win_content.h>        // Content_GetAvailableContentPacks
#include <win32/win_main.h>           // g_allowMature

// ============================================================================
// Steamworks public API (extern "C", declared via S_API / S_CALLTYPE).
//
// These are the low-level Steamworks entry points the engine calls directly.
// Offline: report no user, do nothing, and hand back null interface pointers.
// ============================================================================

S_API HSteamUser S_CALLTYPE SteamAPI_GetHSteamUser()
{
    return 0;
}

S_API HSteamUser S_CALLTYPE SteamGameServer_GetHSteamUser()
{
    return 0;
}

S_API void *S_CALLTYPE SteamInternal_ContextInit( void * /*pContextInitData*/ )
{
    return nullptr;
}

S_API void *S_CALLTYPE SteamInternal_FindOrCreateUserInterface( HSteamUser /*hSteamUser*/, const char * /*pszVersion*/ )
{
    return nullptr;
}

S_API void *S_CALLTYPE SteamInternal_FindOrCreateGameServerInterface( HSteamUser /*hSteamUser*/, const char * /*pszVersion*/ )
{
    return nullptr;
}

S_API void S_CALLTYPE SteamAPI_RunCallbacks()
{
}

S_API void S_CALLTYPE SteamGameServer_RunCallbacks()
{
}

S_API void S_CALLTYPE SteamAPI_RegisterCallback( class CCallbackBase * /*pCallback*/, int /*iCallback*/ )
{
}

S_API void S_CALLTYPE SteamAPI_UnregisterCallback( class CCallbackBase * /*pCallback*/ )
{
}

S_API void S_CALLTYPE SteamAPI_RegisterCallResult( class CCallbackBase * /*pCallback*/, SteamAPICall_t /*hAPICall*/ )
{
}

S_API void S_CALLTYPE SteamAPI_UnregisterCallResult( class CCallbackBase * /*pCallback*/, SteamAPICall_t /*hAPICall*/ )
{
}

// ============================================================================
// Engine Steam wrapper (win_steam.h) — ticket / client authentication.
//
// Offline: never initialized, no tickets, no SteamID, all checks "fail closed".
// ============================================================================

bool g_steamInitialized = false;

void Steam_Init()
{
}

void Steam_Shutdown()
{
}

bool Steam_UpdateClientAuthTicket( netadr_t /*serverIpv4*/ )
{
    return false;
}

bool Steam_GetRawClientTicket( unsigned char ** /*pBuffer*/, uint32 * /*pSize*/ )
{
    return false;
}

void Steam_CancelClientTicket()
{
}

uint64_t Steam_GetClientSteamID64()
{
    return 0;
}

bool Steam_CheckClientTicket( const void * /*pAuthTicket*/, uint32 /*authTicketLen*/, uint64_t /*steamID64*/ )
{
    return false;
}

void Steam_CheckClients()
{
}

void Steam_OnClientDropped( uint64_t /*steamID64*/ )
{
}

// ============================================================================
// Live — session helpers (win_voice.h).
//
// Offline: no remote client maps to any XUID.
// ============================================================================

int Live_GetClientNumForXuid( const SessionData_s * /*session*/, unsigned __int64 /*xuid*/ )
{
    return -1;
}

// ============================================================================
// Voice chat (win_voice.h).
//
// Offline: voice fully disabled — nobody talks, no data, neutral levels.
// ============================================================================

float voice_current_scaler = 1.0f;

bool Voice_Init()
{
    return false;
}

void Voice_Shutdown()
{
}

void Voice_Playback()
{
}

bool Voice_IsClientTalking( unsigned int /*clientNum*/ )
{
    return false;
}

void Voice_IncomingVoiceData( unsigned __int8 /*talker*/, unsigned __int8 * /*data*/, int /*packetDataSize*/ )
{
}

double Voice_GetVoiceLevel()
{
    return 0.0;
}

int Voice_GetLocalVoiceData()
{
    return 0;
}

void Voice_StopClientSamples()
{
}

// ============================================================================
// Gamer profile (win_gamerprofile.h).
//
// Offline: no profile is logged in; settings unavailable; playlist neutral.
// ============================================================================

bool g_allowMature = false;  // win_main.h declares it as `extern bool g_allowMature;`

GamerSettingState *GamerProfile_GetProfileSettings( int /*controllerIndex*/ )
{
    return nullptr;
}

bool GamerProfile_IsProfileLoggedIn( int /*controllerIndex*/ )
{
    return false;
}

int GamerProfile_GetPlaylistNum( int /*controllerIndex*/ )
{
    return 0;
}

void GamerProfile_SetPlaylistNum( int /*controllerIndex*/, int /*playlistNum*/ )
{
}

void GamerProfile_UpdateDvarsFromProfile( int /*controllerIndex*/ )
{
}

void GamerProfile_UpdateProfileFromDvars( int /*controllerIndex*/, profileWriteState_t /*profileWriteState*/ )
{
}

void GamerProfile_LogInProfile( int /*controllerIndex*/ )
{
}

// ============================================================================
// Downloadable content (win_content.h).
//
// Offline: no content packs available.
// ============================================================================

int Content_GetAvailableContentPacks()
{
    return 0;
}
