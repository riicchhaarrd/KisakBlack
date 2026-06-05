// linux_workercmds.cpp — worker-thread / job-queue bring-up for Linux.
//
// A faithful port of win32/win_workercmds.cpp (that file lives under the
// win32/ tree which is excluded from the Linux build). The job-queue core
// (tl/jobqueue) and the Sys_*WorkerCmd* layer (qcommon) are platform-agnostic
// and already compiled in; this is just the small glue that initializes them:
// jqInit() seeds the lock-free queues' free lists, jqStart() spawns the worker
// threads (via the pthread-backed CreateThread), and the nuge_physics module /
// IW task-manager entry points the physics and audio systems push work through.
//
// Previously R_InitWorkerThreads() was a no-op stub, so the global job queue's
// FreeListPtr stayed null and the first Sys_AddWorkerCmdInternal() (from
// SND_Update during the loading screen) crashed in jqAtomicQueue::AllocateNode.
#include <win32/win_workercmds.h>
#include <tl/jobqueue/jobqueue_all.h>
#include <universal/com_workercmds.h>
#include <universal/q_shared.h>
#include <qcommon/threads.h>

int __cdecl SetupWorkerThread(jqBatch *batch)
{
    Sys_InitThread(batch->ParamData[0] + 2);
    return 0;
}

volatile unsigned int nuge_physicsLimit = 1;
jqModule nuge_physicsModule =
{
    .Name = "nuge_physics",
    .Type = JQ_WORKER_GENERIC,
    .Code = nuge_physicsCallback,
};
jqWorkerCmd nuge_physicsWorkerCmd = { &nuge_physicsModule, sizeof(jqBatch), 0, 0, &nuge_physicsLimit, NULL, 0u };

// no workerCmd
jqModule setup_worker_threadsModule =
{
    .Name = "setup_worker_threads",
    .Type = JQ_WORKER_GENERIC,
    .Code = SetupWorkerThread,
};

int __cdecl nuge_physicsCallback(jqBatch *batch)
{
    const char *Name; // [esp+8h] [ebp-14h]
    jqBatch *realbatch; // [esp+18h] [ebp-4h]

    realbatch = (jqBatch *)jqLockData(batch);
    Name = va("nuge_physics: %s", realbatch->Module->Name);
    PROF_SCOPED_RUNTIME_NAME(Name);
    realbatch->Module->Code(realbatch);
    jqUnlockData(batch);
    return 0;
}

void __cdecl IW_task_manager_add_batch(jqBatch *ptr)
{
    if ( !ptr->Module
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\win32\\win_workercmds.cpp", 51, 0, "%s", "ptr->Module") )
    {
        __debugbreak();
    }
    Sys_AddWorkerCmdInternal(&nuge_physicsWorkerCmd, (unsigned __int8 *)ptr, 0);
}

void __cdecl IW_task_manager_flush()
{
    Sys_WaitWorkerCmdInternal(&nuge_physicsWorkerCmd);
}

void __cdecl R_InitWorkerCmds()
{
    jqInit();
#if defined(__EMSCRIPTEN__)
    // Web: run the job queue WITHOUT async worker threads (single "main" worker).
    //
    // Pinned with the fstage tracer: the in-game spawn freeze is the main thread spinning
    // forever in Sys_WaitWorkerCmdInternal -> jqFlush during scene generation
    // (R_GenerateSortedDrawSurfs, fstage=690). jqFlush loops while
    // QueuedBatchCount+ExecutingBatchCount != 0; on the pthread build a job-queue worker
    // thread that gets stuck mid-batch leaves ExecutingBatchCount pinned at 1, so the
    // flush never returns and the render backend sits idle waiting for a frame that the
    // frontend never submits.
    //
    // jqEnableWorkers(0) -> jqStart builds a single worker for the main processor and
    // spawns NO threads. All worker cmds default to the global queue (jqAddBatch with a
    // NULL queue), which is attached to that worker, so each cmd is drained INLINE on the
    // calling thread at its existing jqFlush sync point. Nothing executes on another
    // thread, so nothing can strand the flush. Serial, but correct. The render BACKEND
    // thread (R_InitRenderThread) is independent of the job queue and keeps running, so
    // device creation / the SMP frame handshake are unaffected (no startup black screen).
    jqEnableWorkers(0);
    jqSetBatchDataHeapSize(0x4000u, 0x10u);
    jqStart();
    // NOTE: the setup_worker_threads batches below init per-worker-thread contexts; with
    // no worker threads they must NOT run (running them on the main thread would corrupt
    // its thread identity), so they are skipped on web.
#else
    jqEnableWorkers(12);
    jqSetBatchDataHeapSize(0x4000u, 0x10u);
    jqStart();

    jqBatch batch; // [esp+0h] [ebp-80h] BYREF
    batch.Module = &setup_worker_threadsModule;

    batch.ParamData[0] = 0;
    jqAddBatch(&batch, jqGetWorkerQueue(4));

    batch.ParamData[0] = 1;
    jqAddBatch(&batch, jqGetWorkerQueue(8));

    jqFlush(0, 0);
#endif
    Sys_InitWorkerThreadContext();
    Sys_WorkerCmdInit();
}

void __cdecl R_InitWorkerThreads()
{
    R_InitWorkerCmds();
}
