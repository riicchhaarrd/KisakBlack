#include "r_draw_bsp.h"
#include "rb_tess.h"
#include "r_vertexstream2.h"
#include "r_state.h"
#include "rb_shade.h"
#include "r_dvars.h"
#include "r_shade.h"
#include "r_buffers.h"   // R_AllocStaticIndexBuffer/R_FinishStaticIndexBuffer (static world IB)
#include <string.h>
#include <stdio.h>
#if defined(__EMSCRIPTEN__)
#include <vector>
#include <map>
#include <set>
// GL bridge (gl_d3d9_draw.cpp): draw N single-stream world surfaces as ONE multi-draw from the bound
// static IB, each with its own baseVertex. See GLDevice::KB_DrawWorldMulti.
extern "C" void KB_DrawWorldMultiC(void *dev, const int *counts, const void *const *offsets,
                                   const int *baseVerts, int n);
void R_LmArraySetLayer(unsigned lmapIndex);   // ?lmarray: defined in r_state.cpp (build + set page)
// Lit-world multi-draw merge (?nolitmerge escape) — defined after the worldmerge2 helpers below.
extern "C" int KB_WorldBaseVertexOK();        // gl_d3d9_draw.cpp: multi-draw or base-vertex ext live
static bool R_LitMergeEnabled();
static bool R_DrawTrianglesLitMulti(GfxTrianglesDrawStream *drawStream, GfxCmdBufPrimState *primState);
// [perf/lit] counters (printed once a second in glcontext_sdl.cpp)
unsigned long g_kbLitSurfs = 0;      // surfaces accumulated into multi-draw entries
unsigned long g_kbLitDraws = 0;      // entries submitted (after index-contiguity extension)
unsigned long g_kbLitFlushes = 0;    // KB_DrawWorldMultiC submissions (~ lightmap/probe texture runs)
unsigned long g_kbLitS2Fallback = 0; // stream2 surfaces drawn per-surface inside the merged walk
unsigned long g_kbLitBailStock = 0;  // lit calls that took the stock walker (any cause below)
// Bail-cause split: which dispatch condition sent the call to the stock walker
unsigned long g_kbLitBailPrepass = 0, g_kbLitBailGate = 0, g_kbLitBailIb = 0;
// Layered fold (delta-run grouping): surface i needs stream0 fetch at 44*fv_i + 44*idx and
// stream1 at vLD_i + stride1*idx. One shared bind pair (O0, O1) with per-draw baseVertex bv_i
// satisfies both iff delta_i := vLD_i - stride1*fv_i == O1 - stride1*O0/44 — the run key is the
// single scalar delta (H14 measured delta==0 nowhere, H15 measured ALL deltas negative).
// Negative deltas fold too: k = ceil(-delta/stride1), O0 = 44k, O1 = delta + stride1*k (both
// >= 0 by construction), bv_i = fv_i - k (>= 0 within a run, proven from vLD_i >= 0).
unsigned long g_kbLitDeltaRuns = 0;  // bind-pair rebinds from delta changes (run boundaries)
unsigned long g_kbLitNegDelta = 0;   // surfaces folded through a negative-delta run (info)
#endif

int g_layerDataStride[18] = { 0, 0, 0, 8, 12, 16, 20, 24, 24, 28, 32, 32, 36, 40, 0, 0, 16, 0 };
int g_stream2Stride[18] = { 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0 };


void __cdecl R_SetStreamsForBspSurface(GfxCmdBufPrimState *state, const srfTriangles_t *tris)
{
    int stream2ByteOffset; // [esp+0h] [ebp-34h]
    IDirect3DVertexBuffer9 *v3; // [esp+4h] [ebp-30h]
    int vertexLayerData; // [esp+8h] [ebp-2Ch]
    IDirect3DVertexBuffer9 *layerVb; // [esp+Ch] [ebp-28h]
    int v6; // [esp+10h] [ebp-24h]
    IDirect3DVertexBuffer9 *worldVb; // [esp+14h] [ebp-20h]
    int v8; // [esp+18h] [ebp-1Ch]
    IDirect3DVertexBuffer9 *v9; // [esp+1Ch] [ebp-18h]
    int vertexOffset; // [esp+20h] [ebp-14h]
    IDirect3DVertexBuffer9 *vb; // [esp+24h] [ebp-10h]
    unsigned int layerDataStride; // [esp+28h] [ebp-Ch]
    unsigned int stream2Stride; // [esp+2Ch] [ebp-8h]
    unsigned int stream2Stridea; // [esp+2Ch] [ebp-8h]

    layerDataStride = g_layerDataStride[state->vertDeclType];
    if ( layerDataStride )
    {
        if ( tris->stream2ByteOffset < 0 )
        {
            R_SetDoubleStreamSource(
                state,
                g_worldDraw->vd.worldVb,
                44 * tris->firstVertex,
                0x2Cu,
                g_worldDraw->vld.layerVb,
                tris->vertexLayerData,
                layerDataStride);
        }
        else
        {
            stream2Stridea = g_stream2Stride[state->vertDeclType];
            stream2ByteOffset = tris->stream2ByteOffset;
            v3 = *R_VertexStream2_GetVB();
            vertexLayerData = tris->vertexLayerData;
            layerVb = g_worldDraw->vld.layerVb;
            v6 = 44 * tris->firstVertex;
            worldVb = g_worldDraw->vd.worldVb;
            if ( state->streams[0].vb != worldVb || state->streams[0].offset != v6 || state->streams[0].stride != 44 )
                R_ChangeStreamSource(state, 0, worldVb, v6, 0x2Cu);
            if ( state->streams[1].vb != layerVb
                || state->streams[1].offset != vertexLayerData
                || state->streams[1].stride != layerDataStride )
            {
                R_ChangeStreamSource(state, 1u, layerVb, vertexLayerData, layerDataStride);
            }
            if ( state->streams[2].vb != v3
                || state->streams[2].offset != stream2ByteOffset
                || state->streams[2].stride != stream2Stridea )
            {
                R_ChangeStreamSource(state, 2u, v3, stream2ByteOffset, stream2Stridea);
            }
        }
    }
    else if ( tris->stream2ByteOffset < 0 )
    {
        R_SetStreamSource(state, g_worldDraw->vd.worldVb, 44 * tris->firstVertex, 0x2Cu);
    }
    else
    {
        stream2Stride = g_stream2Stride[state->vertDeclType];
        v8 = tris->stream2ByteOffset;
        v9 = *R_VertexStream2_GetVB();
        vertexOffset = 44 * tris->firstVertex;
        vb = g_worldDraw->vd.worldVb;
        if ( state->streams[0].vb != vb || state->streams[0].offset != vertexOffset || state->streams[0].stride != 44 )
            R_ChangeStreamSource(state, 0, vb, vertexOffset, 0x2Cu);
        if ( state->streams[1].vb || state->streams[1].offset || state->streams[1].stride )
            R_ChangeStreamSource(state, 1u, 0, 0, 0);
        if ( state->streams[2].vb != v9 || state->streams[2].offset != v8 || state->streams[2].stride != stream2Stride )
            R_ChangeStreamSource(state, 2u, v9, v8, stream2Stride);
    }
}

void __cdecl R_DrawBspDrawSurfsLit(
                const unsigned int *primDrawSurfPos,
                GfxCmdBufContext context,
                GfxCmdBufContext prepassContext)
{
    GfxTrianglesDrawStream drawStream; // [esp+4h] [ebp-28h] BYREF
    const MaterialPass *pass; // [esp+24h] [ebp-8h]
    unsigned int customSamplerFlags; // [esp+28h] [ebp-4h]

    pass = context.state->pass;
    customSamplerFlags = pass->customSamplerFlags;
    if ( (customSamplerFlags & 1) != 0 )
        R_SetSamplerState(context.state, 0xFu, 0x72u);
    if ( (customSamplerFlags & 2) != 0 )
        R_SetSamplerState(context.state, 0xCu, 0x62u);
    if ( (customSamplerFlags & 4) != 0 )
        R_SetSamplerState(context.state, 0xDu, 0x62u);
    if ( (customSamplerFlags & 8) != 0 )
        R_SetSamplerState(context.state, 0xEu, 0x62u);
    drawStream.reflectionProbeTexture = context.state->samplerTexture[15];
    drawStream.lightmapPrimaryTexture = context.state->samplerTexture[12];
    drawStream.lightmapSecondaryTexture = context.state->samplerTexture[13];
    drawStream.lightmapSecondaryTextureB = context.state->samplerTexture[14];
    drawStream.whiteTexture = &rgp.whiteImage->texture;
    drawStream.primDrawSurfPos = primDrawSurfPos;
    drawStream.customSamplerFlags = pass->customSamplerFlags;
    drawStream.hasSunDirChanged = context.source->input.data->prim.hasSunDirChanged;
    if ( prepassContext.state )
        R_DrawTrianglesLit(&drawStream, &context.state->prim, &prepassContext.state->prim);
    else
        R_DrawTrianglesLit(&drawStream, &context.state->prim, 0);
    context.state->samplerTexture[15] = drawStream.reflectionProbeTexture;
    context.state->samplerTexture[12] = drawStream.lightmapPrimaryTexture;
    context.state->samplerTexture[13] = drawStream.lightmapSecondaryTexture;
    context.state->samplerTexture[14] = drawStream.lightmapSecondaryTextureB;
}

void __cdecl R_DrawTrianglesLit(
                GfxTrianglesDrawStream *drawStream,
                GfxCmdBufPrimState *primState,
                GfxCmdBufPrimState *prepassPrimState)
{
#if defined(__EMSCRIPTEN__)
    // Lit-world multi-draw merge: the same stream0@0 + static-IB + per-surface-baseVertex fold
    // ?worldmerge2 applies to the unlit walker. Layered (terrain) decls join the fold with
    // stream1 = layerVb @ 0 (their layer data is parallel to the world verts; per-surface
    // checked, non-parallel surfaces draw per-surface). Prepass and pre-static-IB keep stock.
    if ( R_LitMergeEnabled() )
    {
        if ( prepassPrimState )
            ++g_kbLitBailPrepass;
        else if ( !KB_WorldBaseVertexOK() )
            ++g_kbLitBailGate;
        else if ( R_DrawTrianglesLitMulti(drawStream, primState) )
            return;
        else
            ++g_kbLitBailIb;
    }
    ++g_kbLitBailStock;
#endif
    GfxTexture *whiteTexture; // [esp+0h] [ebp-ACh]
    GfxImage *blackImage; // [esp+8h] [ebp-A4h]
    const GfxSurface *bspSurf; // [esp+34h] [ebp-78h]
    int baseIndex; // [esp+38h] [ebp-74h]
    unsigned int surfIndex; // [esp+3Ch] [ebp-70h]
    const srfTriangles_t *prevTris; // [esp+48h] [ebp-64h]
    unsigned int lightmapSecondaryFlag; // [esp+4Ch] [ebp-60h]
    GfxImage *newLightmapSecondaryTextureB; // [esp+50h] [ebp-5Ch]
    unsigned int reflectionProbeFlag; // [esp+54h] [ebp-58h]
    const GfxTexture *lightmapPrimaryTexture; // [esp+58h] [ebp-54h]
    const unsigned __int16 *list; // [esp+5Ch] [ebp-50h] BYREF
    int triCount; // [esp+60h] [ebp-4Ch]
    unsigned int reflectionProbeIndex; // [esp+64h] [ebp-48h]
    const GfxTexture *reflectionProbeTexture; // [esp+68h] [ebp-44h]
    const GfxTexture *newLightmapPrimaryTexture; // [esp+6Ch] [ebp-40h]
    GfxTexture *reflectionProbeTextures; // [esp+70h] [ebp-3Ch]
    GfxTexture *conflictTex; // [esp+74h] [ebp-38h]
    unsigned int index; // [esp+78h] [ebp-34h]
    unsigned int lightmapIndex; // [esp+7Ch] [ebp-30h]
    const GfxTexture *lightmapSecondaryTexture; // [esp+80h] [ebp-2Ch]
    const GfxTexture *newLightmapSecondaryTexture; // [esp+84h] [ebp-28h]
    const GfxTexture *lightmapSecondaryTextureB; // [esp+88h] [ebp-24h]
    IDirect3DDevice9 *device; // [esp+8Ch] [ebp-20h]
    unsigned int lightmapPrimaryFlag; // [esp+90h] [ebp-1Ch]
    const GfxImage *overrideImage; // [esp+94h] [ebp-18h]
    unsigned int count; // [esp+98h] [ebp-14h] BYREF
    int baseVertex; // [esp+9Ch] [ebp-10h]
    const GfxTexture *newReflectionProbeTexture; // [esp+A0h] [ebp-Ch]
    int hasSunDirChanged; // [esp+A4h] [ebp-8h]
    bool override; // [esp+ABh] [ebp-1h]

    reflectionProbeIndex = 255;
    lightmapIndex = 31;
    prevTris = 0;
    triCount = 0;
    baseVertex = -1;
    baseIndex = 0;
    reflectionProbeTexture = drawStream->reflectionProbeTexture;
    lightmapPrimaryTexture = drawStream->lightmapPrimaryTexture;
    lightmapSecondaryTexture = drawStream->lightmapSecondaryTexture;
    lightmapSecondaryTextureB = drawStream->lightmapSecondaryTextureB;
    reflectionProbeFlag = drawStream->customSamplerFlags & 1;
    lightmapPrimaryFlag = drawStream->customSamplerFlags & 2;
    lightmapSecondaryFlag = drawStream->customSamplerFlags & 4;
    reflectionProbeTextures = g_worldDraw->reflectionProbeTextures;
    hasSunDirChanged = drawStream->hasSunDirChanged;
    overrideImage = 0;
    override = r_lightMap->current.integer != 1;
    if ( override )
        overrideImage = R_OverrideGrayscaleImage(r_lightMap);
    conflictTex = 0;
    if ( r_lightConflicts->current.enabled )
    {
        if ( (rg.frontEndFrameCount & 4) != 0 )
            blackImage = rgp.blackImage;
        else
            blackImage = rgp.whiteImage;
        conflictTex = &blackImage->texture;
    }
    device = primState->device;
    while ( R_ReadBspDrawSurfs(&drawStream->primDrawSurfPos, &list, &count) )
    {
        for ( index = 0; index < count; ++index )
        {
            surfIndex = list[index];
            if ( surfIndex >= rgp.world->surfaceCount
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                            539,
                            0,
                            "surfIndex doesn't index rgp.world->surfaceCount\n\t%i not in [0, %i)",
                            surfIndex,
                            rgp.world->surfaceCount) )
            {
                __debugbreak();
            }
            bspSurf = &rgp.world->dpvs.surfaces[surfIndex];
            _mm_prefetch((const char *)&rgp.world->dpvs.surfaces[list[index + 1]].lightmapIndex, 1);
            if ( r_lightConflicts->current.color[0]
                 | (reflectionProbeIndex - bspSurf->reflectionProbeIndex)
                 | (lightmapIndex - bspSurf->lightmapIndex) )
            {
                if ( prevTris )
                    R_DrawBspTris(primState, prevTris, triCount);
                prevTris = &bspSurf->tris;
                triCount = 0;
                baseIndex = bspSurf->tris.baseIndex;
                if ( baseVertex != bspSurf->tris.firstVertex )
                {
                    baseVertex = bspSurf->tris.firstVertex;
                    R_SetStreamsForBspSurface(primState, &bspSurf->tris);
                }
                reflectionProbeIndex = bspSurf->reflectionProbeIndex;
                lightmapIndex = bspSurf->lightmapIndex;
#if defined(__EMSCRIPTEN__)
                R_LmArraySetLayer(lightmapIndex);   // ?lmarray: set the lightmap page (array bound to s12)
#endif
                if ( reflectionProbeFlag )
                {
                    if ( reflectionProbeIndex >= g_worldDraw->reflectionProbeCount
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                                    582,
                                    0,
                                    "reflectionProbeIndex doesn't index g_worldDraw->reflectionProbeCount\n\t%i not in [0, %i)",
                                    reflectionProbeIndex,
                                    g_worldDraw->reflectionProbeCount) )
                    {
                        __debugbreak();
                    }
                    newReflectionProbeTexture = &reflectionProbeTextures[reflectionProbeIndex];
                    if ( reflectionProbeTexture != newReflectionProbeTexture )
                    {
                        reflectionProbeTexture = newReflectionProbeTexture;
                        R_HW_SetSamplerTexture(device, 0xFu, newReflectionProbeTexture);
                    }
                }
                if ( lightmapIndex == 31 )
                {
                    if ( lightmapPrimaryFlag
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                                    673,
                                    0,
                                    "%s\n\t(bspSurf->material->info.name) = %s",
                                    "(!lightmapPrimaryFlag)",
                                    bspSurf->material->info.name) )
                    {
                        __debugbreak();
                    }
                    if ( lightmapSecondaryFlag
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                                    675,
                                    0,
                                    "%s\n\t(bspSurf->material->info.name) = %s",
                                    "(!lightmapSecondaryFlag)",
                                    bspSurf->material->info.name) )
                    {
                        __debugbreak();
                    }
                }
                else
                {
                    if ( lightmapIndex >= g_worldDraw->lightmapCount
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                                    593,
                                    0,
                                    "lightmapIndex doesn't index g_worldDraw->lightmapCount\n\t%i not in [0, %i)",
                                    lightmapIndex,
                                    g_worldDraw->lightmapCount) )
                    {
                        __debugbreak();
                    }
                    if ( lightmapPrimaryFlag )
                    {
                        if ( override )
                        {
                            newLightmapPrimaryTexture = &overrideImage->texture;
                        }
                        else
                        {
                            if ( hasSunDirChanged )
                                whiteTexture = drawStream->whiteTexture;
                            else
                                whiteTexture = &g_worldDraw->lightmapPrimaryTextures[lightmapIndex];
                            newLightmapPrimaryTexture = whiteTexture;
                        }
                        if ( conflictTex && (bspSurf->flags & 2) != 0 )
                            newLightmapPrimaryTexture = conflictTex;
                        if ( lightmapPrimaryTexture != newLightmapPrimaryTexture )
                        {
                            lightmapPrimaryTexture = newLightmapPrimaryTexture;
                            R_HW_SetSamplerTexture(device, 0xCu, newLightmapPrimaryTexture);
                        }
                    }
                    if ( lightmapSecondaryFlag )
                    {
                        if ( override )
                        {
                            R_OverrideGrayscaleImage(r_lightMap);
                            newLightmapSecondaryTexture = &rgp.r5g6b5Image->texture;
                            overrideImage = rgp.g16r16Image;
                            newLightmapSecondaryTextureB = rgp.g16r16Image;
                        }
                        else
                        {
                            newLightmapSecondaryTexture = &g_worldDraw->lightmapSecondaryTextures[lightmapIndex];
                            newLightmapSecondaryTextureB = (GfxImage *)&g_worldDraw->lightmapSecondaryTexturesB[lightmapIndex];
                        }
                        if ( conflictTex && (bspSurf->flags & 2) != 0 )
                        {
                            newLightmapSecondaryTexture = conflictTex;
                            newLightmapSecondaryTextureB = (GfxImage *)conflictTex;
                        }
                        if ( lightmapSecondaryTexture != newLightmapSecondaryTexture )
                        {
                            lightmapSecondaryTexture = newLightmapSecondaryTexture;
                            R_HW_SetSamplerTexture(device, 0xDu, newLightmapSecondaryTexture);
                        }
                        if ( lightmapSecondaryTextureB != (const GfxTexture *)newLightmapSecondaryTextureB )
                        {
                            lightmapSecondaryTextureB = &newLightmapSecondaryTextureB->texture;
                            R_HW_SetSamplerTexture(device, 0xEu, &newLightmapSecondaryTextureB->texture);
                        }
                    }
                }
            }
            else if ( baseVertex != bspSurf->tris.firstVertex || baseIndex + 3 * triCount != bspSurf->tris.baseIndex )
            {
                if ( prevTris )
                    R_DrawBspTris(primState, prevTris, triCount);
                prevTris = &bspSurf->tris;
                triCount = 0;
                baseIndex = bspSurf->tris.baseIndex;
                if ( baseVertex != bspSurf->tris.firstVertex )
                {
                    baseVertex = bspSurf->tris.firstVertex;
                    R_SetStreamsForBspSurface(primState, &bspSurf->tris);
                }
            }
            triCount += bspSurf->tris.triCount;
            if ( prepassPrimState )
            {
                if ( !Assert_MyHandler(
                                "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                                733,
                                0,
                                "%s",
                                "!prepassPrimState") )
                    __debugbreak();
            }
        }
    }
    if ( prevTris )
        R_DrawBspTris(primState, prevTris, triCount);
    drawStream->reflectionProbeTexture = reflectionProbeTexture;
    drawStream->lightmapPrimaryTexture = lightmapPrimaryTexture;
    drawStream->lightmapSecondaryTexture = lightmapSecondaryTexture;
    drawStream->lightmapSecondaryTextureB = lightmapSecondaryTextureB;
}

#if defined(__EMSCRIPTEN__)
// Static world index buffer. g_worldDraw->indices never changes after load, yet R_DrawBspTris copied
// a CONTIGUOUS slice of it into the dynamic IB on EVERY world draw (~1880/frame) — a per-draw
// lock+memcpy+GPU-upload that kbprof showed to be ~half of the "tess invoke" hotspot. Upload it once
// to a static IB and draw with baseIndex = tris->baseIndex (exactly what the model/particle paths
// already do via R_ChangeIndices). The callers only merge surfaces sharing firstVertex + a contiguous
// index range, and the indices are straight-copied (never rebased), so direct draws are identical.
static IDirect3DIndexBuffer9 *g_worldStaticIb = 0;
static const unsigned short *g_worldStaticIbSrc = 0;   // which indices[] array is currently uploaded
static int g_worldStaticIbCount = 0;

static IDirect3DIndexBuffer9 *R_GetWorldStaticIb()
{
    if ( g_worldStaticIb
        && g_worldStaticIbSrc == g_worldDraw->indices
        && g_worldStaticIbCount == g_worldDraw->indexCount )
        return g_worldStaticIb;                        // already uploaded for this world
    if ( g_worldStaticIb ) { g_worldStaticIb->Release(); g_worldStaticIb = 0; }  // world changed -> rebuild
    g_worldStaticIbSrc = 0;
    g_worldStaticIbCount = 0;
    int count = g_worldDraw->indexCount;
    if ( count <= 0 || !g_worldDraw->indices )
        return 0;                                      // fall back to the dynamic path
    void *data = R_AllocStaticIndexBuffer(&g_worldStaticIb, count * 2);
    if ( !data ) { g_worldStaticIb = 0; fprintf(stderr, "[worldStaticIb] ALLOC FAILED (%d indices) -> dynamic fallback\n", count); return 0; }
    memcpy(data, g_worldDraw->indices, (size_t)count * 2);
    R_FinishStaticIndexBuffer(g_worldStaticIb);
    g_worldStaticIbSrc = g_worldDraw->indices;
    g_worldStaticIbCount = count;
    fprintf(stderr, "[worldStaticIb] built: %d indices (%d KB) — world draws now skip per-draw R_SetIndexData\n",
            count, (count * 2) / 1024);
    return g_worldStaticIb;
}
#endif

void __cdecl R_DrawBspTris(GfxCmdBufPrimState *state, const srfTriangles_t *tris, unsigned int triCount)
{
    GfxDrawPrimArgs args; // [esp+0h] [ebp-Ch] BYREF

    args.vertexCount = tris->vertexCount;
    args.triCount = triCount;
#if defined(__EMSCRIPTEN__)
    IDirect3DIndexBuffer9 *wib = R_GetWorldStaticIb();
    if ( wib )
    {
        if ( state->indexBuffer != wib )
            R_ChangeIndices(state, wib);
        args.baseIndex = tris->baseIndex;              // natural offset into the static world IB
    }
    else
    {
        args.baseIndex = R_SetIndexData(state, (unsigned __int8 *)&g_worldDraw->indices[tris->baseIndex], triCount);
    }
#else
    args.baseIndex = R_SetIndexData(state, (unsigned __int8 *)&g_worldDraw->indices[tris->baseIndex], triCount);
#endif
    R_DrawIndexedPrimitive(state, &args);
    if ( state->primStats )
    {
        state->frameStats.geoIndexCount += 3 * triCount;
        state->primStats->dynamicIndexCount += 3 * triCount;
    }
}

int __cdecl R_ReadBspDrawSurfs(
                const unsigned int **primDrawSurfPos,
                const unsigned __int16 **list,
                unsigned int *count)
{
    *count = *(*primDrawSurfPos)++;
    if ( !*count )
        return 0;
    *list = (const unsigned __int16 *)*primDrawSurfPos;
    *primDrawSurfPos += (*count + 1) >> 1;
    return 1;
}

void __cdecl R_DrawBspDrawSurfsLitPreTess(const unsigned int *primDrawSurfPos, GfxCmdBufContext context)
{
    unsigned int baseIndex; // [esp+0h] [ebp-28h] BYREF
    unsigned int surfIndex; // [esp+4h] [ebp-24h]
    GfxReadCmdBuf cmdBuf; // [esp+8h] [ebp-20h] BYREF
    const srfTriangles_t *tris; // [esp+Ch] [ebp-1Ch]
    const GfxBspPreTessDrawSurf *list; // [esp+10h] [ebp-18h] BYREF
    unsigned int reflectionProbeIndex; // [esp+14h] [ebp-14h]
    const GfxSurface *bspSurf; // [esp+18h] [ebp-10h]
    unsigned int index; // [esp+1Ch] [ebp-Ch]
    unsigned int lightmapIndex; // [esp+20h] [ebp-8h]
    unsigned int count; // [esp+24h] [ebp-4h] BYREF

    cmdBuf.primDrawSurfPos = primDrawSurfPos;
    while ( R_ReadBspPreTessDrawSurfs(&cmdBuf, &list, &count, &baseIndex) )
    {
        reflectionProbeIndex = 255;
        lightmapIndex = 31;
        for ( index = 0; index < count; ++index )
        {
            surfIndex = list[index].baseSurfIndex;
            if ( surfIndex >= rgp.world->surfaceCount
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                            928,
                            0,
                            "surfIndex doesn't index rgp.world->surfaceCount\n\t%i not in [0, %i)",
                            surfIndex,
                            rgp.world->surfaceCount) )
            {
                __debugbreak();
            }
            bspSurf = &rgp.world->dpvs.surfaces[surfIndex];
            tris = &bspSurf->tris;
            if ( reflectionProbeIndex != bspSurf->reflectionProbeIndex || lightmapIndex != bspSurf->lightmapIndex )
            {
                reflectionProbeIndex = bspSurf->reflectionProbeIndex;
                lightmapIndex = bspSurf->lightmapIndex;
                R_SetReflectionProbe(context, reflectionProbeIndex);
                R_SetLightmap(context, lightmapIndex);
                R_SetupPassPerObjectArgs(context);
                R_SetupPassPerPrimArgs(context);
            }
            R_DrawPreTessTris(&context.state->prim, tris, baseIndex, list[index].totalTriCount);
            baseIndex += 3 * list[index].totalTriCount;
        }
    }
}

void __cdecl R_DrawPreTessTris(
                GfxCmdBufPrimState *state,
                const srfTriangles_t *tris,
                unsigned int baseIndex,
                unsigned int triCount)
{
    GfxDrawPrimArgs args; // [esp+0h] [ebp-Ch] BYREF

    R_SetStreamsForBspSurface(state, tris);
    args.vertexCount = tris->vertexCount;
    args.triCount = triCount;
    args.baseIndex = baseIndex;
    R_DrawIndexedPrimitive(state, &args);
    state->frameStats.geoIndexCount += 3 * triCount;
    if ( !state->primStats
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp", 393, 0, "%s", "state->primStats") )
    {
        __debugbreak();
    }
    state->primStats->dynamicIndexCount += 3 * triCount;
}

int __cdecl R_ReadBspPreTessDrawSurfs(
                GfxReadCmdBuf *cmdBuf,
                const GfxBspPreTessDrawSurf **list,
                unsigned int *count,
                unsigned int *baseIndex)
{
    *count = R_ReadPrimDrawSurfInt(cmdBuf);
    if ( !*count )
        return 0;
    *baseIndex = R_ReadPrimDrawSurfInt(cmdBuf);
    *list = (const GfxBspPreTessDrawSurf *)R_ReadPrimDrawSurfData(cmdBuf, *count);
    return 1;
}

void __cdecl R_DrawBspDrawSurfsPreTess(const unsigned int *primDrawSurfPos, GfxCmdBufContext context)
{
    unsigned int baseIndex; // [esp+0h] [ebp-2Ch] BYREF
    unsigned int surfIndex; // [esp+4h] [ebp-28h]
    GfxReadCmdBuf cmdBuf; // [esp+8h] [ebp-24h] BYREF
    const srfTriangles_t *tris; // [esp+Ch] [ebp-20h]
    const srfTriangles_t *prevTris; // [esp+10h] [ebp-1Ch]
    const GfxBspPreTessDrawSurf *list; // [esp+14h] [ebp-18h] BYREF
    unsigned int triCount; // [esp+18h] [ebp-14h]
    const GfxSurface *bspSurf; // [esp+1Ch] [ebp-10h]
    unsigned int index; // [esp+20h] [ebp-Ch]
    unsigned int count; // [esp+24h] [ebp-8h] BYREF
    int baseVertex; // [esp+28h] [ebp-4h]

    R_SetupPassPerObjectArgs(context);
    R_SetupPassPerPrimArgs(context);
    cmdBuf.primDrawSurfPos = primDrawSurfPos;
    while ( R_ReadBspPreTessDrawSurfs(&cmdBuf, &list, &count, &baseIndex) )
    {
        prevTris = 0;
        triCount = 0;
        baseVertex = -1;
        for ( index = 0; index < count; ++index )
        {
            surfIndex = list[index].baseSurfIndex;
            if ( surfIndex >= rgp.world->surfaceCount
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                            981,
                            0,
                            "surfIndex doesn't index rgp.world->surfaceCount\n\t%i not in [0, %i)",
                            surfIndex,
                            rgp.world->surfaceCount) )
            {
                __debugbreak();
            }
            bspSurf = &rgp.world->dpvs.surfaces[surfIndex];
            tris = &bspSurf->tris;
            if ( baseVertex != bspSurf->tris.firstVertex )
            {
                if ( triCount )
                {
                    R_DrawPreTessTris(&context.state->prim, prevTris, baseIndex, triCount);
                    baseIndex += 3 * triCount;
                    triCount = 0;
                }
                prevTris = tris;
                baseVertex = tris->firstVertex;
            }
            triCount += list[index].totalTriCount;
        }
        R_DrawPreTessTris(&context.state->prim, prevTris, baseIndex, triCount);
    }
}

void __cdecl R_DrawBspDrawSurfs(const unsigned int *primDrawSurfPos, GfxCmdBufState *state)
{
    GfxTrianglesDrawStream drawStream; // [esp+0h] [ebp-20h] BYREF

    drawStream.primDrawSurfPos = primDrawSurfPos;
    R_DrawTriangles(&drawStream, &state->prim);
}

#if defined(__EMSCRIPTEN__)
// ?worldmerge (opt-in, default OFF): collapse engine-side world draw calls. The regular world path
// emits one R_DrawIndexedPrimitive per index-CONTIGUOUS run; surfaces drawn in sort order with the
// same firstVertex but gaps in the index buffer each become a separate draw (~4.8 draws/material
// batch, ~1980 engine draws/frame — kbprof "draw submit"). This path gathers ALL surfaces sharing a
// firstVertex into the dynamic IB (consecutive R_SetIndexData calls append contiguously — exactly
// what happened per-surface before the static-IB change, so the buffer is already sized for it) and
// issues ONE draw per firstVertex run. Trades A's static-IB no-copy for fewer draw calls; ?worldmerge
// lets us measure which wins. Indices stay relative to the shared firstVertex (no rebase, no uint16
// overflow). Default off so the shipped build is unaffected until verified.
int g_kbWorldMerge = -1;
static bool R_WorldMergeEnabled()
{
    if ( g_kbWorldMerge < 0 ) { const char *e = getenv("KB_WORLDMERGE"); g_kbWorldMerge = (e && *e == '1') ? 1 : 0; }
    return g_kbWorldMerge != 0;
}
static inline void R_EmitMergedRun(GfxCmdBufPrimState *state, int dynBaseIndex, int triCount)
{
    if ( triCount <= 0 ) return;
    GfxDrawPrimArgs args;
    args.vertexCount = 0;            // GL ignores NumVertices; the gathered index range defines the draw
    args.triCount = triCount;
    args.baseIndex = dynBaseIndex;
    R_DrawIndexedPrimitive(state, &args);
    if ( state->primStats )
    {
        state->frameStats.geoIndexCount += 3 * triCount;
        state->primStats->dynamicIndexCount += 3 * triCount;
    }
}
static void R_DrawTrianglesMerged(GfxTrianglesDrawStream *drawStream, GfxCmdBufPrimState *state)
{
    const unsigned __int16 *list;     // BYREF
    unsigned int count;               // BYREF
    int baseVertex = -1;
    int runDynBase = -1;              // first dynamic-IB baseIndex of the current firstVertex run
    int runTriCount = 0;
    while ( R_ReadBspDrawSurfs(&drawStream->primDrawSurfPos, &list, &count) )
    {
        for ( unsigned int index = 0; index < count; ++index )
        {
            const GfxSurface *bspSurf = &rgp.world->dpvs.surfaces[list[index]];
            const srfTriangles_t *tris = &bspSurf->tris;
            if ( baseVertex != tris->firstVertex )
            {
                R_EmitMergedRun(state, runDynBase, runTriCount);   // flush previous firstVertex run
                runDynBase = -1;
                runTriCount = 0;
                baseVertex = tris->firstVertex;
                R_SetStreamsForBspSurface(state, tris);            // rebind vertex stream for the new base
            }
            int dynBase = R_SetIndexData(state, (unsigned __int8 *)&g_worldDraw->indices[tris->baseIndex], tris->triCount);
            if ( runDynBase < 0 )
                runDynBase = dynBase;
            else if ( dynBase != runDynBase + 3 * runTriCount )
            {
                // dynamic IB wrapped (DISCARD) mid-run -> emit what we have and start a fresh run
                R_EmitMergedRun(state, runDynBase, runTriCount);
                runDynBase = dynBase;
                runTriCount = 0;
            }
            runTriCount += tris->triCount;
        }
    }
    R_EmitMergedRun(state, runDynBase, runTriCount);
}

// ?worldmerge2 (opt-in): the DEEP merge. Unlike ?worldmerge (which still breaks per firstVertex), this
// binds worldVb at offset 0 ONCE and uses the static world IB (no gather), accumulating EVERY
// single-stream surface as (count, byte-offset, baseVertex=firstVertex) and issuing ONE
// glMultiDrawElementsBaseVertex for the whole stream. Collapses the ~1980 engine draws/frame to a
// handful. Surfaces needing the secondary vertex stream (stream2ByteOffset >= 0) can't share the
// stream0-only binding -> flushed and drawn the normal way, then accumulation resumes.
int g_kbWorldMerge2 = -1;
static bool R_WorldMerge2Enabled()
{
    // DEFAULT ON (validated: world renders correctly + higher fps). ?noworldmerge disables it and
    // falls back to the per-surface path (which itself can be forced to the simple merge via
    // ?worldmerge). Disable is the escape hatch if a map ever shows wrong geometry.
    if ( g_kbWorldMerge2 < 0 ) { const char *e = getenv("KB_NOWORLDMERGE"); g_kbWorldMerge2 = (e && *e == '1') ? 0 : 1; }
    return g_kbWorldMerge2 != 0;
}
static void R_DrawTrianglesMergedMulti(GfxTrianglesDrawStream *drawStream, GfxCmdBufPrimState *state)
{
    IDirect3DIndexBuffer9 *wib = R_GetWorldStaticIb();
    // Layered (terrain) decls bind multiple per-surface streams -> can't fold to one stream0 binding.
    // R_DrawTriangles is the non-layered world path, but guard anyway; fall back to the gather-merge.
    if ( !wib || g_layerDataStride[state->vertDeclType] != 0 )
    {
        R_DrawTrianglesMerged(drawStream, state);
        return;
    }
    R_SetStreamSource(state, g_worldDraw->vd.worldVb, 0, 0x2Cu);   // stream0 @ offset 0; baseVertex carries firstVertex
    if ( state->indexBuffer != wib )
        R_ChangeIndices(state, wib);

    static std::vector<int> counts;
    static std::vector<const void *> offsets;
    static std::vector<int> baseVerts;
    counts.clear(); offsets.clear(); baseVerts.clear();

    const unsigned __int16 *list;   // BYREF
    unsigned int count;             // BYREF
    while ( R_ReadBspDrawSurfs(&drawStream->primDrawSurfPos, &list, &count) )
    {
        for ( unsigned int index = 0; index < count; ++index )
        {
            const GfxSurface *bspSurf = &rgp.world->dpvs.surfaces[list[index]];
            const srfTriangles_t *tris = &bspSurf->tris;
            if ( tris->stream2ByteOffset >= 0 )
            {
                // needs the secondary stream: flush the accumulated multidraw, draw this one normally,
                // then restore stream0@0 + static IB and continue accumulating.
                if ( !counts.empty() )
                {
                    KB_DrawWorldMultiC(state->device, counts.data(), offsets.data(), baseVerts.data(), (int)counts.size());
                    counts.clear(); offsets.clear(); baseVerts.clear();
                }
                R_SetStreamsForBspSurface(state, tris);
                int db = R_SetIndexData(state, (unsigned __int8 *)&g_worldDraw->indices[tris->baseIndex], tris->triCount);
                R_EmitMergedRun(state, db, tris->triCount);
                R_SetStreamSource(state, g_worldDraw->vd.worldVb, 0, 0x2Cu);
                if ( state->indexBuffer != wib )
                    R_ChangeIndices(state, wib);
                continue;
            }
            counts.push_back(3 * (int)tris->triCount);
            offsets.push_back((const void *)(size_t)((size_t)tris->baseIndex * 2));   // uint16 IB byte offset
            baseVerts.push_back(tris->firstVertex);
        }
    }
    if ( !counts.empty() )
        KB_DrawWorldMultiC(state->device, counts.data(), offsets.data(), baseVerts.data(), (int)counts.size());
}

// Lit-world multi-draw merge (default ON, ?nolitmerge escape): R_DrawTrianglesLit is the LAST
// per-surface world walker — every firstVertex change rebinds streams (the [perf/fc] vtx class)
// and every index gap is its own engine draw. Apply the validated ?worldmerge2 fold: stream0 =
// worldVb @ offset 0 bound ONCE, the static world IB, per-surface baseVertex = firstVertex, and
// ONE KB_DrawWorldMultiC per (lightmap, reflection-probe) texture run — those sampler binds are
// the only state changes in the walk that a draw can't carry. stream2 surfaces can't share the
// stream0-only binding -> drawn per-surface, then accumulation resumes. The texture-change body
// MUST MIRROR R_DrawTrianglesLit above (incl. the overrideImage reassignment quirk).
int g_kbLitMerge = -1;
static bool R_LitMergeEnabled()
{
    if ( g_kbLitMerge < 0 ) { const char *e = getenv("KB_NOLITMERGE"); g_kbLitMerge = (e && *e == '1') ? 0 : 1; }
    return g_kbLitMerge != 0;
}
// [matarray] one-shot world-material bucketing report — sizes the texture-array material
// consolidation (memory + capture) before building it. A bucket = materials sharing
// (techniqueSet, textureCount, per-stage {nameHash, semantic, dims, mips}); one set of
// per-stage GL_TEXTURE_2D_ARRAYs serves a bucket, layer count = its material count.
// baseSize = the image's loaded bytes incl. mips, so est-MB ≈ the EXTRA GPU memory the
// arrays would add (originals must stay for non-bucketed techniques).
static const void *g_kbMatReportWorld = 0;
static void R_MatArrayReport()
{
    if ( !rgp.world || g_kbMatReportWorld == (const void *)rgp.world )
        return;
    g_kbMatReportWorld = rgp.world;
    std::set<const Material *> seen;
    std::map<std::vector<unsigned>, std::vector<const Material *>> buckets;
    for ( unsigned int i = 0; i < rgp.world->surfaceCount; ++i )
    {
        const Material *m = rgp.world->dpvs.surfaces[i].material;
        if ( !m || !seen.insert(m).second )
            continue;
        std::vector<unsigned> key;
        key.push_back((unsigned)(uintptr_t)m->techniqueSet);
        key.push_back(m->textureCount);
        bool ok = true;
        for ( unsigned int t = 0; t < m->textureCount; ++t )
        {
            const MaterialTextureDef *td = &m->textureTable[t];
            if ( td->semantic == 11 || !td->u.image ) { ok = false; break; }   // 11 = water
            key.push_back(td->nameHash);
            key.push_back(td->semantic);
            key.push_back(((unsigned)td->u.image->width << 16) | td->u.image->height);
            key.push_back(td->u.image->levelCount);
        }
        if ( ok )
            buckets[key].push_back(m);
    }
    size_t arrMats = 0, multiBuckets = 0, topLayers[5] = {0};
    unsigned long long arrBytes = 0;
    for ( auto &b : buckets )
    {
        size_t layers = b.second.size();
        if ( layers < 2 )
            continue;
        ++multiBuckets; arrMats += layers;
        unsigned long long perLayer = 0;
        const Material *m0 = b.second[0];
        for ( unsigned int t = 0; t < m0->textureCount; ++t )
            if ( m0->textureTable[t].u.image )
                perLayer += m0->textureTable[t].u.image->baseSize;
        arrBytes += perLayer * layers;
        for ( int k = 0; k < 5; ++k )
            if ( layers > topLayers[k] )
            {
                for ( int j = 4; j > k; --j ) topLayers[j] = topLayers[j - 1];
                topLayers[k] = layers;
                break;
            }
    }
    fprintf(stderr, "[matarray] world mats=%u buckets>=2: %u covering %u mats, est extra MB=%llu, top layers: %u %u %u %u %u\n",
            (unsigned)seen.size(), (unsigned)multiBuckets, (unsigned)arrMats,
            arrBytes >> 20, (unsigned)topLayers[0], (unsigned)topLayers[1],
            (unsigned)topLayers[2], (unsigned)topLayers[3], (unsigned)topLayers[4]);
}

static bool R_DrawTrianglesLitMulti(GfxTrianglesDrawStream *drawStream, GfxCmdBufPrimState *primState)
{
    IDirect3DIndexBuffer9 *wib = R_GetWorldStaticIb();
    if ( !wib )
        return false;                       // static-IB alloc failed -> stock walker (stream untouched)
    R_MatArrayReport();                     // one-shot sizing report at first lit draw
    unsigned int layerStride = g_layerDataStride[primState->vertDeclType];

    unsigned int reflectionProbeIndex = 255;
    unsigned int lightmapIndex = 31;
    const GfxTexture *reflectionProbeTexture = drawStream->reflectionProbeTexture;
    const GfxTexture *lightmapPrimaryTexture = drawStream->lightmapPrimaryTexture;
    const GfxTexture *lightmapSecondaryTexture = drawStream->lightmapSecondaryTexture;
    const GfxTexture *lightmapSecondaryTextureB = drawStream->lightmapSecondaryTextureB;
    unsigned int reflectionProbeFlag = drawStream->customSamplerFlags & 1;
    unsigned int lightmapPrimaryFlag = drawStream->customSamplerFlags & 2;
    unsigned int lightmapSecondaryFlag = drawStream->customSamplerFlags & 4;
    GfxTexture *reflectionProbeTextures = g_worldDraw->reflectionProbeTextures;
    int hasSunDirChanged = drawStream->hasSunDirChanged;
    const GfxImage *overrideImage = 0;
    bool override = r_lightMap->current.integer != 1;
    if ( override )
        overrideImage = R_OverrideGrayscaleImage(r_lightMap);
    GfxTexture *conflictTex = 0;
    if ( r_lightConflicts->current.enabled )
        conflictTex = &(((rg.frontEndFrameCount & 4) != 0) ? rgp.blackImage : rgp.whiteImage)->texture;
    IDirect3DDevice9 *device = primState->device;

    // Fold binding: stream0 = worldVb @ 0, per-draw baseVertex carries firstVertex. Layered
    // decls additionally bind stream1 = layerVb at the current delta-run's offset (set per
    // surface below); under baseVertex=fv a bind at delta = vertexLayerData - layerStride*fv
    // fetches exactly the surface's rows — surfaces sharing a delta share one bind.
    if ( layerStride )
        R_SetDoubleStreamSource(primState, g_worldDraw->vd.worldVb, 0, 0x2Cu,
                                g_worldDraw->vld.layerVb, 0, layerStride);
    else
        R_SetStreamSource(primState, g_worldDraw->vd.worldVb, 0, 0x2Cu);
    if ( primState->indexBuffer != wib )
        R_ChangeIndices(primState, wib);
    int curLayerDelta = 0;        // current run's delta (vLD - stride1*fv) — selects the bind pair
    unsigned curLayerK = 0;       // current run's k: stream0 @ 44k, stream1 @ delta+stride1*k, bv = fv-k
    bool foldBound = true;        // fold bind pair + IB intact (false after a fallback)

    static std::vector<int> counts;
    static std::vector<const void *> offsets;
    static std::vector<int> baseVerts;
    counts.clear(); offsets.clear(); baseVerts.clear();

    // Restore the fold binding lazily — only when a submit actually needs it (a fallback run
    // of N surfaces costs 1 restore instead of N). Pending entries always belong to
    // curLayerDelta: a delta change flushes before rebinding stream1.
    auto kbBindFoldPair = [&]() {
        if ( layerStride )
            R_SetDoubleStreamSource(primState, g_worldDraw->vd.worldVb, 44u * curLayerK, 0x2Cu,
                                    g_worldDraw->vld.layerVb,
                                    (unsigned)(curLayerDelta + (int)(layerStride * curLayerK)), layerStride);
        else
            R_SetStreamSource(primState, g_worldDraw->vd.worldVb, 0, 0x2Cu);
    };
    auto kbRestoreFold = [&]() {
        if ( foldBound )
            return;
        kbBindFoldPair();
        if ( primState->indexBuffer != wib )
            R_ChangeIndices(primState, wib);
        foldBound = true;
    };
    auto kbFlushAccum = [&]() {
        if ( counts.empty() )
            return;
        kbRestoreFold();
        ++g_kbLitFlushes; g_kbLitDraws += (unsigned long)counts.size();
        KB_DrawWorldMultiC(device, counts.data(), offsets.data(), baseVerts.data(), (int)counts.size());
        counts.clear(); offsets.clear(); baseVerts.clear();
    };

    const unsigned __int16 *list;   // BYREF
    unsigned int count;             // BYREF
    while ( R_ReadBspDrawSurfs(&drawStream->primDrawSurfPos, &list, &count) )
    {
        for ( unsigned int index = 0; index < count; ++index )
        {
            unsigned int surfIndex = list[index];
            if ( surfIndex >= rgp.world->surfaceCount
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                            539,
                            0,
                            "surfIndex doesn't index rgp.world->surfaceCount\n\t%i not in [0, %i)",
                            surfIndex,
                            rgp.world->surfaceCount) )
            {
                __debugbreak();
            }
            const GfxSurface *bspSurf = &rgp.world->dpvs.surfaces[surfIndex];
            _mm_prefetch((const char *)&rgp.world->dpvs.surfaces[list[index + 1]].lightmapIndex, 1);
            if ( r_lightConflicts->current.color[0]
                 | (reflectionProbeIndex - bspSurf->reflectionProbeIndex)
                 | (lightmapIndex - bspSurf->lightmapIndex) )
            {
                kbFlushAccum();   // texture state changes: submit the accumulated run first
                reflectionProbeIndex = bspSurf->reflectionProbeIndex;
                lightmapIndex = bspSurf->lightmapIndex;
                R_LmArraySetLayer(lightmapIndex);   // ?lmarray: set the lightmap page (array bound to s12)
                if ( reflectionProbeFlag )
                {
                    if ( reflectionProbeIndex >= g_worldDraw->reflectionProbeCount
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                                    582,
                                    0,
                                    "reflectionProbeIndex doesn't index g_worldDraw->reflectionProbeCount\n\t%i not in [0, %i)",
                                    reflectionProbeIndex,
                                    g_worldDraw->reflectionProbeCount) )
                    {
                        __debugbreak();
                    }
                    const GfxTexture *newReflectionProbeTexture = &reflectionProbeTextures[reflectionProbeIndex];
                    if ( reflectionProbeTexture != newReflectionProbeTexture )
                    {
                        reflectionProbeTexture = newReflectionProbeTexture;
                        R_HW_SetSamplerTexture(device, 0xFu, newReflectionProbeTexture);
                    }
                }
                if ( lightmapIndex == 31 )
                {
                    if ( lightmapPrimaryFlag
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                                    673,
                                    0,
                                    "%s\n\t(bspSurf->material->info.name) = %s",
                                    "(!lightmapPrimaryFlag)",
                                    bspSurf->material->info.name) )
                    {
                        __debugbreak();
                    }
                    if ( lightmapSecondaryFlag
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                                    675,
                                    0,
                                    "%s\n\t(bspSurf->material->info.name) = %s",
                                    "(!lightmapSecondaryFlag)",
                                    bspSurf->material->info.name) )
                    {
                        __debugbreak();
                    }
                }
                else
                {
                    if ( lightmapIndex >= g_worldDraw->lightmapCount
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_bsp.cpp",
                                    593,
                                    0,
                                    "lightmapIndex doesn't index g_worldDraw->lightmapCount\n\t%i not in [0, %i)",
                                    lightmapIndex,
                                    g_worldDraw->lightmapCount) )
                    {
                        __debugbreak();
                    }
                    if ( lightmapPrimaryFlag )
                    {
                        const GfxTexture *newLightmapPrimaryTexture;
                        if ( override )
                        {
                            newLightmapPrimaryTexture = &overrideImage->texture;
                        }
                        else if ( hasSunDirChanged )
                        {
                            newLightmapPrimaryTexture = drawStream->whiteTexture;
                        }
                        else
                        {
                            newLightmapPrimaryTexture = &g_worldDraw->lightmapPrimaryTextures[lightmapIndex];
                        }
                        if ( conflictTex && (bspSurf->flags & 2) != 0 )
                            newLightmapPrimaryTexture = conflictTex;
                        if ( lightmapPrimaryTexture != newLightmapPrimaryTexture )
                        {
                            lightmapPrimaryTexture = newLightmapPrimaryTexture;
                            R_HW_SetSamplerTexture(device, 0xCu, newLightmapPrimaryTexture);
                        }
                    }
                    if ( lightmapSecondaryFlag )
                    {
                        const GfxTexture *newLightmapSecondaryTexture;
                        GfxImage *newLightmapSecondaryTextureB;
                        if ( override )
                        {
                            R_OverrideGrayscaleImage(r_lightMap);
                            newLightmapSecondaryTexture = &rgp.r5g6b5Image->texture;
                            overrideImage = rgp.g16r16Image;
                            newLightmapSecondaryTextureB = rgp.g16r16Image;
                        }
                        else
                        {
                            newLightmapSecondaryTexture = &g_worldDraw->lightmapSecondaryTextures[lightmapIndex];
                            newLightmapSecondaryTextureB = (GfxImage *)&g_worldDraw->lightmapSecondaryTexturesB[lightmapIndex];
                        }
                        if ( conflictTex && (bspSurf->flags & 2) != 0 )
                        {
                            newLightmapSecondaryTexture = conflictTex;
                            newLightmapSecondaryTextureB = (GfxImage *)conflictTex;
                        }
                        if ( lightmapSecondaryTexture != newLightmapSecondaryTexture )
                        {
                            lightmapSecondaryTexture = newLightmapSecondaryTexture;
                            R_HW_SetSamplerTexture(device, 0xDu, newLightmapSecondaryTexture);
                        }
                        if ( lightmapSecondaryTextureB != (const GfxTexture *)newLightmapSecondaryTextureB )
                        {
                            lightmapSecondaryTextureB = &newLightmapSecondaryTextureB->texture;
                            R_HW_SetSamplerTexture(device, 0xEu, &newLightmapSecondaryTextureB->texture);
                        }
                    }
                }
            }
            const srfTriangles_t *tris = &bspSurf->tris;
            // Layered: this surface's run key is delta = vertexLayerData - layerStride*fv (any
            // sign — see the counter comment for the (O0, O1, k) construction). Only stream2
            // surfaces (per-surface dynamic-stream offset) still draw per-surface.
            int layerDelta = 0;
            if ( layerStride )
                layerDelta = tris->vertexLayerData - (int)(layerStride * (unsigned)tris->firstVertex);
            if ( tris->stream2ByteOffset >= 0 )
            {
                // submit what's pending, draw this one the stock way; the fold binding is
                // restored lazily at the next submit (consecutive fallbacks pay once).
                ++g_kbLitS2Fallback;
                kbFlushAccum();
                R_SetStreamsForBspSurface(primState, tris);
                int db = R_SetIndexData(primState, (unsigned __int8 *)&g_worldDraw->indices[tris->baseIndex], tris->triCount);
                R_EmitMergedRun(primState, db, tris->triCount);
                foldBound = false;
                continue;
            }
            if ( layerStride && layerDelta != curLayerDelta )
            {
                kbFlushAccum();             // pending entries belong to the previous delta run
                ++g_kbLitDeltaRuns;
                curLayerDelta = layerDelta;
                curLayerK = (layerDelta < 0)
                    ? (unsigned)(((unsigned)-layerDelta + layerStride - 1) / layerStride)
                    : 0u;
                if ( foldBound )            // else the lazy restore binds the new pair itself
                    kbBindFoldPair();
            }
            if ( layerDelta < 0 )
                ++g_kbLitNegDelta;
            ++g_kbLitSurfs;
            if ( !counts.empty() && baseVerts.back() == (int)tris->firstVertex - (int)curLayerK
                && (size_t)offsets.back() + (size_t)counts.back() * 2 == (size_t)tris->baseIndex * 2 )
            {
                counts.back() += 3 * (int)tris->triCount;   // index-contiguous: extend (stock merged these too)
            }
            else
            {
                counts.push_back(3 * (int)tris->triCount);
                offsets.push_back((const void *)(size_t)((size_t)tris->baseIndex * 2));   // uint16 IB byte offset
                baseVerts.push_back((int)tris->firstVertex - (int)curLayerK);
            }
        }
    }
    kbFlushAccum();
    drawStream->reflectionProbeTexture = reflectionProbeTexture;
    drawStream->lightmapPrimaryTexture = lightmapPrimaryTexture;
    drawStream->lightmapSecondaryTexture = lightmapSecondaryTexture;
    drawStream->lightmapSecondaryTextureB = lightmapSecondaryTextureB;
    return true;
}
#endif

void __cdecl R_DrawTriangles(GfxTrianglesDrawStream *drawStream, GfxCmdBufPrimState *state)
{
    int baseIndex; // [esp+0h] [ebp-28h]
    const GfxSurface *tris; // [esp+8h] [ebp-20h]
    const srfTriangles_t *prevTris; // [esp+Ch] [ebp-1Ch]
    const unsigned __int16 *list; // [esp+10h] [ebp-18h] BYREF
    int triCount; // [esp+14h] [ebp-14h]
    const GfxSurface *bspSurf; // [esp+18h] [ebp-10h]
    unsigned int index; // [esp+1Ch] [ebp-Ch]
    unsigned int count; // [esp+20h] [ebp-8h] BYREF
    int baseVertex; // [esp+24h] [ebp-4h]

#if defined(__EMSCRIPTEN__)
    // KB_WorldBaseVertexOK: without the multi-draw/base-vertex exts (headless Chromium) the
    // core glDrawElementsBaseVertex stub DROPS the base -> corrupted world; take stock instead.
    if ( R_WorldMerge2Enabled() && KB_WorldBaseVertexOK() ) { R_DrawTrianglesMergedMulti(drawStream, state); return; }
    if ( R_WorldMergeEnabled() )  { R_DrawTrianglesMerged(drawStream, state); return; }
#endif

    prevTris = 0;
    triCount = 0;
    baseVertex = -1;
    baseIndex = 0;
    while ( R_ReadBspDrawSurfs(&drawStream->primDrawSurfPos, &list, &count) )
    {
        for ( index = 0; index < count; ++index )
        {
            bspSurf = &rgp.world->dpvs.surfaces[list[index]];
            tris = bspSurf;
            if ( baseVertex != bspSurf->tris.firstVertex || baseIndex + 3 * triCount != bspSurf->tris.baseIndex )
            {
                if ( prevTris )
                    R_DrawBspTris(state, prevTris, triCount);
                prevTris = &tris->tris;
                triCount = 0;
                baseIndex = tris->tris.baseIndex;
                if ( baseVertex != tris->tris.firstVertex )
                {
                    baseVertex = tris->tris.firstVertex;
                    R_SetStreamsForBspSurface(state, &tris->tris);
                }
            }
            triCount += tris->tris.triCount;
        }
    }
    if ( prevTris )
        R_DrawBspTris(state, prevTris, triCount);
}

