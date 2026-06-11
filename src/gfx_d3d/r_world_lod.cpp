#include "r_world_lod.h"
#include <math.h>
#include <universal/q_shared.h>
#include "r_init.h"
#include <universal/com_memory.h>
#include "r_dvars.h"

unsigned __int8 *s_lodVals;
LodChainState *s_lodState[1];

void __thiscall LodChainState::Init(const GfxWorldLodChain *lodChain, int localClientNum)
{
    int i; // [esp+10h] [ebp-4h]

    this->m_lodChain = lodChain;
    this->m_fade = 0.0f;
    this->m_curLevel = 1;
    this->m_lastLevel = 1;
    for ( i = 1; i < this->m_lodChain->lodInfoCount; ++i )
        SetFadeForSortedSurfaces(0.0, i + this->m_lodChain->firstLodInfo, localClientNum);
}

void __thiscall LodChainState::UpdateLevel(int newLevel, int localClientNum)
{
    __int64 v3; // rax
    float val; // [esp+Ch] [ebp-10h]
    unsigned int i; // [esp+18h] [ebp-4h]

    if ( newLevel != this->m_curLevel )
    {
        if ( this->m_lastLevel != this->m_curLevel )
            LodChainState::UpdateFade(1000.0, localClientNum);
        this->m_lastLevel = this->m_curLevel;
        this->m_curLevel = newLevel;
        v3 = this->m_curLevel - this->m_lastLevel;
        if ( (int)((HIDWORD(v3) ^ v3) - HIDWORD(v3)) <= 1 )
        {
            if ( this->m_curLevel <= (int)this->m_lastLevel )
            {
                if ( this->m_lastLevel == (unsigned __int8)(this->m_lodChain->lodInfoCount + 1) )
                {
                    this->m_fade = 0.0f;
                }
                else
                {
                    this->m_fade = 1.0f;
                    if ( this->m_curLevel )
                        SetFadeForSortedSurfaces(1.0, this->m_lodChain->firstLodInfo + this->m_curLevel - 1, localClientNum);
                }
            }
            else if ( this->m_curLevel == (unsigned __int8)(this->m_lodChain->lodInfoCount + 1) )
            {
                this->m_fade = 1.0f;
            }
            else
            {
                this->m_fade = 0.0f;
            }
        }
        else
        {
            for ( i = 0; i < this->m_lodChain->lodInfoCount; ++i )
            {
                if ( i == this->m_curLevel )
                    val = 1.0f;
                else
                    val = 0.0f;
                if ( i )
                    SetFadeForSortedSurfaces(val, this->m_lodChain->firstLodInfo + i - 1, localClientNum);
            }
            this->m_lastLevel = this->m_curLevel;
        }
    }
}

void __thiscall LodChainState::UpdateFade(float dt, int localClientNum)
{
    int m_lastLevel; // [esp+10h] [ebp-10h]
    int savedLastLevel; // [esp+14h] [ebp-Ch]
    unsigned int firstLodInfo; // [esp+18h] [ebp-8h]
    unsigned int lodInfoIndex; // [esp+1Ch] [ebp-4h]

    if ( this->m_curLevel != this->m_lastLevel )
    {
        if ( this->m_lastLevel && this->m_lastLevel != this->m_lodChain->lodInfoCount + 1 )
        {
            if ( this->m_curLevel && this->m_curLevel != this->m_lodChain->lodInfoCount + 1 )
            {
                if ( this->m_curLevel <= (int)this->m_lastLevel )
                {
                    m_lastLevel = this->m_lastLevel;
                    this->m_fade = this->m_fade - (float)(dt * 1.0);
                    if ( this->m_fade <= 0.0 )
                    {
                        this->m_fade = 0.0f;
                        this->m_lastLevel = this->m_curLevel;
                    }
                    SetFadeForSortedSurfaces(this->m_fade, this->m_lodChain->firstLodInfo + m_lastLevel - 1, localClientNum);
                }
                else
                {
                    savedLastLevel = this->m_lastLevel;
                    this->m_fade = (float)(dt * 1.0) + this->m_fade;
                    if ( this->m_fade >= 1.0 )
                    {
                        this->m_fade = 1.0f;
                        this->m_lastLevel = this->m_curLevel;
                    }
                    if ( this->m_fade >= 1.0 )
                        SetFadeForSortedSurfaces(0.0, this->m_lodChain->firstLodInfo + savedLastLevel - 1, localClientNum);
                    SetFadeForSortedSurfaces(this->m_fade, this->m_lodChain->firstLodInfo + this->m_curLevel - 1, localClientNum);
                }
            }
            else
            {
                firstLodInfo = this->m_lodChain->firstLodInfo;
                if ( this->m_curLevel )
                    firstLodInfo = firstLodInfo + this->m_lodChain->lodInfoCount - 1;
                this->m_fade = this->m_fade - (float)(dt * 1.0);
                if ( this->m_fade <= 0.0 )
                {
                    this->m_fade = 0.0f;
                    this->m_lastLevel = this->m_curLevel;
                }
                SetFadeForSortedSurfaces(this->m_fade, firstLodInfo, localClientNum);
            }
        }
        else
        {
            lodInfoIndex = this->m_lodChain->firstLodInfo;
            if ( this->m_lastLevel )
                lodInfoIndex = lodInfoIndex + this->m_lodChain->lodInfoCount - 1;
            this->m_fade = (float)(dt * 1.0) + this->m_fade;
            if ( this->m_fade >= 1.0 )
            {
                this->m_fade = 1.0f;
                this->m_lastLevel = this->m_curLevel;
            }
            SetFadeForSortedSurfaces(this->m_fade, lodInfoIndex, localClientNum);
        }
    }
}

void __cdecl SetFadeForSortedSurfaces(float val, int lodInfoIndex, int localClientNum)
{
    unsigned int index; // [esp+0h] [ebp-18h]
    const GfxWorldLodInfo *lodInfo; // [esp+4h] [ebp-14h]
    unsigned int offset; // [esp+Ch] [ebp-Ch]
    unsigned int *surf; // [esp+10h] [ebp-8h]
    unsigned int *last; // [esp+14h] [ebp-4h]

    lodInfo = &rgp.world->worldLodInfos[lodInfoIndex];
    if ( !rgp.world->worldLodSurfaces
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_world_lod.cpp",
                    209,
                    0,
                    "%s",
                    "rgp.world->worldLodSurfaces") )
    {
        __debugbreak();
    }
    surf = &rgp.world->worldLodSurfaces[lodInfo->firstSurf];
    last = &surf[lodInfo->surfCount];
    offset = rgp.world->surfaceCount * localClientNum;
    while ( surf != last )
    {
        index = offset + rgp.world->dpvs.sortedSurfIndex[*surf];
        if ( index >= rgp.world->surfaceCount
            && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_world_lod.cpp",
                        217,
                        0,
                        "index doesn't index rgp.world->surfaceCount * MAX_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                        index,
                        rgp.world->surfaceCount) )
        {
            __debugbreak();
        }
        s_lodVals[index] = (int)(float)(val * 255.0);
        ++surf;
    }
}

void __cdecl R_WorldLod_Init()
{
    unsigned int i; // [esp+0h] [ebp-8h]
    int clientIdx; // [esp+4h] [ebp-4h]

    s_lodVals = Hunk_Alloc(rgp.world->surfaceCount, "R_WorldLod_Init", 23);
    memset(s_lodVals, 0xFFu, rgp.world->surfaceCount);
    for ( clientIdx = 0; clientIdx < 1; ++clientIdx )
    {
        s_lodState[clientIdx] = (LodChainState *)Hunk_Alloc(12 * rgp.world->worldLodChainCount, "R_WorldLod_Init", 23);
        for (i = 0; i < rgp.world->worldLodChainCount; ++i)
        {
            //LodChainState::Init(&s_lodState[clientIdx][i], &rgp.world->worldLodChains[i], clientIdx);
            s_lodState[clientIdx][i].Init(&rgp.world->worldLodChains[i], clientIdx);
        }
    }
}

float s_lastTime = -c_fadeRate * 2.0f;;
void __cdecl R_WorldLod_FrameUpdate(float curTime, float *camPos, int localClientNum)
{
    float v3; // [esp+8h] [ebp-10h]
    float v4; // [esp+Ch] [ebp-Ch]
    unsigned int i; // [esp+10h] [ebp-8h]
    float dt; // [esp+14h] [ebp-4h]
    int savedregs; // [esp+18h] [ebp+0h] BYREF

    dt = curTime - s_lastTime;
    s_lastTime = curTime;
    if ( (float)(dt - 1.0) < 0.0 )
        v4 = dt;
    else
        v4 = 1.0f;
    if ( (float)(0.0 - dt) < 0.0 )
        v3 = v4;
    else
        v3 = 0.0f;
    for ( i = 0; i < rgp.world->worldLodChainCount; ++i )
        UpdateChain(i, camPos, v3, localClientNum);
}

// local variable allocation has failed, the output may be wrong!
void    UpdateChain(int index, const float *inputCamPos, float dt, int localClientNum)
{
    // REWRITTEN from the decompile (IDA: "local variable allocation has failed, the
    // output may be wrong!"). The original stored the camera-to-chain delta into
    // SEPARATE locals (one of them an int!) and read them back as a contiguous vec3
    // through (math::Dir3 *)&curLevel — UB stack aliasing that Clang lays out however
    // it likes, so math::Abs returned GARBAGE distance varying per frame -> the world
    // LOD level flapped -> distant world geometry faded/popped in and out every frame
    // (the long-standing mesh-pop flicker). Plain, well-defined distance math instead.
    LodChainState *state = &s_lodState[localClientNum][index];
    GfxWorldLodChain *chain = &rgp.world->worldLodChains[index];

    float dx = inputCamPos[0] - chain->origin[0];
    float dy = inputCamPos[1] - chain->origin[1];
    float dz = inputCamPos[2] - chain->origin[2];
    float camDist = sqrtf(dx * dx + dy * dy + dz * dz);

    int level = 0;
    if (!r_reflectionProbeGenerate->current.enabled && r_worldLod->current.enabled)
    {
        while (level < chain->lodInfoCount
               && camDist > rgp.world->worldLodInfos[chain->firstLodInfo + level].dist)
            ++level;
        if (camDist > chain->lastDist)
            ++level;
    }
    else
    {
        level = 1;
    }
    state->UpdateLevel(level, localClientNum);
    state->UpdateFade(dt, localClientNum);
}

double __cdecl math::Abs(const math::Dir3 *_v)
{
    return (float)sqrt((float)((float)((float)(_v->v.x * _v->v.x) + (float)(_v->v.y * _v->v.y))
                                                     + (float)(_v->v.z * _v->v.z)));
}

