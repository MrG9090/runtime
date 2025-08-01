// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//
// Implementation of the NativeAOT Platform Abstraction Layer (PAL) library when Unix is the platform.
//

#include <stdio.h>
#include <errno.h>
#include <cwchar>
#include <sal.h>
#include "config.h"
#include <pthread.h>
#include "gcenv.h"
#include "gcenv.ee.h"
#include "gcconfig.h"
#include "holder.h"
#include "UnixSignals.h"
#include "NativeContext.h"
#include "HardwareExceptions.h"
#include "PalCreateDump.h"
#include "cgroupcpu.h"
#include "threadstore.h"
#include "thread.h"
#include "threadstore.inl"

#define _T(s) s
#include "RhConfig.h"

#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <cstdarg>
#include <signal.h>
#include <minipal/thread.h>

#ifdef TARGET_LINUX
#include <sys/syscall.h>
#include <link.h>
#include <elf.h>
#endif

#if HAVE_PTHREAD_GETTHREADID_NP
#include <pthread_np.h>
#endif

#if HAVE_LWP_SELF
#include <lwp.h>
#endif

#if HAVE_CLOCK_GETTIME_NSEC_NP
#include <time.h>
#endif

#ifdef TARGET_APPLE
#include <mach/mach.h>
#endif

#ifdef TARGET_HAIKU
#include <OS.h>
#endif

using std::nullptr_t;

#define INVALID_HANDLE_VALUE    ((HANDLE)(intptr_t)-1)

#define PAGE_NOACCESS           0x01
#define PAGE_READWRITE          0x04
#define PAGE_EXECUTE_READ       0x20
#define PAGE_EXECUTE_READWRITE  0x40

#define WAIT_OBJECT_0           0
#define WAIT_TIMEOUT            258
#define WAIT_FAILED             0xFFFFFFFF

static const int tccSecondsToMilliSeconds = 1000;
static const int tccSecondsToMicroSeconds = 1000000;
static const int tccSecondsToNanoSeconds = 1000000000;
static const int tccMilliSecondsToMicroSeconds = 1000;
static const int tccMilliSecondsToNanoSeconds = 1000000;
static const int tccMicroSecondsToNanoSeconds = 1000;

void RhFailFast()
{
    // Causes creation of a crash dump if enabled
    PalCreateCrashDumpIfEnabled();

    // Aborts the process
    abort();
}

#if TARGET_LINUX

struct PalGetPDBInfoPhdrCallbackData
{
    void* Base;
    void* BuildID;
    uint32_t BuildIDLength;
};

static int PalGetPDBInfoPhdrCallback(struct dl_phdr_info *info, size_t size, void* pData)
{
    struct PalGetPDBInfoPhdrCallbackData* pCallbackData = (struct PalGetPDBInfoPhdrCallbackData*)pData;

    // Find the module of interest
    void* loadAddress = NULL;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; i++)
    {
        if (info->dlpi_phdr[i].p_type == PT_LOAD)
        {
            loadAddress = (void*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            if (loadAddress == pCallbackData->Base)
                break;
        }
    }

    if (loadAddress != pCallbackData->Base)
    {
        return 0;
    }

    // Got the module of interest. Now iterate program headers and try to find the GNU build ID note
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; i++)
    {
        // Must be a note section. We don't check the name because while there's a convention for the name,
        // the convention is not mandatory.
        if (info->dlpi_phdr[i].p_type != PT_NOTE)
            continue;

        // Got a note section, iterate over the contents and find the GNU build id one
        ElfW(Nhdr) *note = (ElfW(Nhdr)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
        ElfW(Addr) align = info->dlpi_phdr[i].p_align;
        ElfW(Addr) size = info->dlpi_phdr[i].p_memsz;
        ElfW(Addr) start = (ElfW(Addr))note;

        while ((ElfW(Addr)) (note + 1) - start < size)
        {
            if (note->n_namesz == 4
                && note->n_type == NT_GNU_BUILD_ID
                && memcmp(note + 1, "GNU", 4) == 0)
            {
                // Got the note, fill out the callback data and return.
                pCallbackData->BuildID = (uint8_t*)note + sizeof(ElfW(Nhdr)) + ALIGN_UP(note->n_namesz, align);
                pCallbackData->BuildIDLength = note->n_descsz;
                return 1;
            }

            // Skip over the note. Size of the note is determined by the header and payload (aligned)
            size_t offset = sizeof(ElfW(Nhdr))
                + ALIGN_UP(note->n_namesz, align)
                + ALIGN_UP(note->n_descsz, align);
            note = (ElfW(Nhdr)*)((uint8_t*)note + offset);
        }
    }

    return 0;
}
#endif

void PalGetPDBInfo(HANDLE hOsHandle, GUID * pGuidSignature, _Out_ uint32_t * pdwAge, _Out_writes_z_(cchPath) WCHAR * wszPath, int32_t cchPath, _Out_ uint32_t * pcbBuildId, _Out_ void ** ppBuildId)
{
    memset(pGuidSignature, 0, sizeof(*pGuidSignature));
    *pdwAge = 0;
    *ppBuildId = NULL;
    *pcbBuildId = 0;
    if (cchPath <= 0)
        return;
    wszPath[0] = L'\0';

#if TARGET_LINUX
    struct PalGetPDBInfoPhdrCallbackData data;
    data.Base = hOsHandle;

    if (!dl_iterate_phdr(&PalGetPDBInfoPhdrCallback, &data))
    {
        return;
    }

    *pcbBuildId = data.BuildIDLength;
    *ppBuildId = data.BuildID;
#endif
}

static void UnmaskActivationSignal()
{
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, INJECT_ACTIVATION_SIGNAL);

    int sigmaskRet = pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL);
    _ASSERTE(sigmaskRet == 0);
}

static void TimeSpecAdd(timespec* time, uint32_t milliseconds)
{
    uint64_t nsec = time->tv_nsec + (uint64_t)milliseconds * tccMilliSecondsToNanoSeconds;
    if (nsec >= tccSecondsToNanoSeconds)
    {
        time->tv_sec += nsec / tccSecondsToNanoSeconds;
        nsec %= tccSecondsToNanoSeconds;
    }

    time->tv_nsec = nsec;
}

// Convert nanoseconds to the timespec structure
// Parameters:
//  nanoseconds - time in nanoseconds to convert
//  t           - the target timespec structure
static void NanosecondsToTimeSpec(uint64_t nanoseconds, timespec* t)
{
    t->tv_sec = nanoseconds / tccSecondsToNanoSeconds;
    t->tv_nsec = nanoseconds % tccSecondsToNanoSeconds;
}

void ReleaseCondAttr(pthread_condattr_t* condAttr)
{
    int st = pthread_condattr_destroy(condAttr);
    ASSERT_MSG(st == 0, "Failed to destroy pthread_condattr_t object");
}

class PthreadCondAttrHolder : public Wrapper<pthread_condattr_t*, DoNothing, ReleaseCondAttr, nullptr>
{
public:
    PthreadCondAttrHolder(pthread_condattr_t* attrs)
    : Wrapper<pthread_condattr_t*, DoNothing, ReleaseCondAttr, nullptr>(attrs)
    {
    }
};

class UnixEvent
{
    pthread_cond_t m_condition;
    pthread_mutex_t m_mutex;
    bool m_manualReset;
    bool m_state;
    bool m_isValid;

public:

    UnixEvent(bool manualReset, bool initialState)
    : m_manualReset(manualReset),
      m_state(initialState),
      m_isValid(false)
    {
    }

    bool Initialize()
    {
        pthread_condattr_t attrs;
        int st = pthread_condattr_init(&attrs);
        if (st != 0)
        {
            ASSERT_UNCONDITIONALLY("Failed to initialize UnixEvent condition attribute");
            return false;
        }

        PthreadCondAttrHolder attrsHolder(&attrs);

#if HAVE_PTHREAD_CONDATTR_SETCLOCK && !HAVE_CLOCK_GETTIME_NSEC_NP
        // Ensure that the pthread_cond_timedwait will use CLOCK_MONOTONIC
        st = pthread_condattr_setclock(&attrs, CLOCK_MONOTONIC);
        if (st != 0)
        {
            ASSERT_UNCONDITIONALLY("Failed to set UnixEvent condition variable wait clock");
            return false;
        }
#endif // HAVE_PTHREAD_CONDATTR_SETCLOCK && !HAVE_CLOCK_GETTIME_NSEC_NP

        st = pthread_mutex_init(&m_mutex, NULL);
        if (st != 0)
        {
            ASSERT_UNCONDITIONALLY("Failed to initialize UnixEvent mutex");
            return false;
        }

        st = pthread_cond_init(&m_condition, &attrs);
        if (st != 0)
        {
            ASSERT_UNCONDITIONALLY("Failed to initialize UnixEvent condition variable");

            st = pthread_mutex_destroy(&m_mutex);
            ASSERT_MSG(st == 0, "Failed to destroy UnixEvent mutex");
            return false;
        }

        m_isValid = true;

        return true;
    }

    bool Destroy()
    {
        bool success = true;

        if (m_isValid)
        {
            int st = pthread_mutex_destroy(&m_mutex);
            ASSERT_MSG(st == 0, "Failed to destroy UnixEvent mutex");
            success = success && (st == 0);

            st = pthread_cond_destroy(&m_condition);
            ASSERT_MSG(st == 0, "Failed to destroy UnixEvent condition variable");
            success = success && (st == 0);
        }

        return success;
    }

    uint32_t Wait(uint32_t milliseconds)
    {
        timespec endTime;
#if HAVE_CLOCK_GETTIME_NSEC_NP
        uint64_t endNanoseconds;
        if (milliseconds != INFINITE)
        {
            uint64_t nanoseconds = (uint64_t)milliseconds * tccMilliSecondsToNanoSeconds;
            NanosecondsToTimeSpec(nanoseconds, &endTime);
            endNanoseconds = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + nanoseconds;
        }
#elif HAVE_PTHREAD_CONDATTR_SETCLOCK
        if (milliseconds != INFINITE)
        {
            clock_gettime(CLOCK_MONOTONIC, &endTime);
            TimeSpecAdd(&endTime, milliseconds);
        }
#else
#error "Don't know how to perform timed wait on this platform"
#endif

        int st = 0;

        pthread_mutex_lock(&m_mutex);
        while (!m_state)
        {
            if (milliseconds == INFINITE)
            {
                st = pthread_cond_wait(&m_condition, &m_mutex);
            }
            else
            {
#if HAVE_CLOCK_GETTIME_NSEC_NP
                // Since OSX doesn't support CLOCK_MONOTONIC, we use relative variant of the
                // timed wait and we need to handle spurious wakeups properly.
                st = pthread_cond_timedwait_relative_np(&m_condition, &m_mutex, &endTime);
                if ((st == 0) && !m_state)
                {
                    uint64_t currentNanoseconds = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                    if (currentNanoseconds < endNanoseconds)
                    {
                        // The wake up was spurious, recalculate the relative endTime
                        uint64_t remainingNanoseconds = (endNanoseconds - currentNanoseconds);
                        NanosecondsToTimeSpec(remainingNanoseconds, &endTime);
                    }
                    else
                    {
                        // Although the timed wait didn't report a timeout, time calculated from the
                        // mach time shows we have already reached the end time. It can happen if
                        // the wait was spuriously woken up right before the timeout.
                        st = ETIMEDOUT;
                    }
                }
#else // HAVE_CLOCK_GETTIME_NSEC_NP
                st = pthread_cond_timedwait(&m_condition, &m_mutex, &endTime);
#endif // HAVE_CLOCK_GETTIME_NSEC_NP
            }

            if (st != 0)
            {
                // wait failed or timed out
                break;
            }
        }

        if ((st == 0) && !m_manualReset)
        {
            // Clear the state for auto-reset events so that only one waiter gets released
            m_state = false;
        }

        pthread_mutex_unlock(&m_mutex);

        uint32_t waitStatus;

        if (st == 0)
        {
            waitStatus = WAIT_OBJECT_0;
        }
        else if (st == ETIMEDOUT)
        {
            waitStatus = WAIT_TIMEOUT;
        }
        else
        {
            waitStatus = WAIT_FAILED;
        }

        return waitStatus;
    }

    void Set()
    {
        pthread_mutex_lock(&m_mutex);
        m_state = true;
        // Unblock all threads waiting for the condition variable
        pthread_cond_broadcast(&m_condition);
        pthread_mutex_unlock(&m_mutex);
    }

    void Reset()
    {
        pthread_mutex_lock(&m_mutex);
        m_state = false;
        pthread_mutex_unlock(&m_mutex);
    }
};

// This functions configures behavior of the signals that are not
// related to hardware exception handling.
void ConfigureSignals()
{
    // The default action for SIGPIPE is process termination.
    // Since SIGPIPE can be signaled when trying to write on a socket for which
    // the connection has been dropped, we need to tell the system we want
    // to ignore this signal.
    // Instead of terminating the process, the system call which would had
    // issued a SIGPIPE will, instead, report an error and set errno to EPIPE.
    signal(SIGPIPE, SIG_IGN);
}

void InitializeCurrentProcessCpuCount()
{
    uint32_t count;

    // If the configuration value has been set, it takes precedence. Otherwise, take into account
    // process affinity and CPU quota limit.

    const unsigned int MAX_PROCESSOR_COUNT = 0xffff;
    uint64_t configValue;

    if (g_pRhConfig->ReadConfigValue("PROCESSOR_COUNT", &configValue, true /* decimal */) &&
        0 < configValue && configValue <= MAX_PROCESSOR_COUNT)
    {
        count = configValue;
    }
    else
    {
#if HAVE_SCHED_GETAFFINITY

        cpu_set_t cpuSet;
        int st = sched_getaffinity(getpid(), sizeof(cpu_set_t), &cpuSet);
        if (st != 0)
        {
            _ASSERTE(!"sched_getaffinity failed");
        }

        count = CPU_COUNT(&cpuSet);
#else // HAVE_SCHED_GETAFFINITY
        count = GCToOSInterface::GetTotalProcessorCount();
#endif // HAVE_SCHED_GETAFFINITY

        uint32_t cpuLimit;
        if (GetCpuLimit(&cpuLimit) && cpuLimit < count)
            count = cpuLimit;
    }

    _ASSERTE(count > 0);
    g_RhNumberOfProcessors = count;
}

static uint32_t g_RhPageSize;

void InitializeOsPageSize()
{
    g_RhPageSize = (uint32_t)sysconf(_SC_PAGE_SIZE);

#if defined(HOST_AMD64)
    ASSERT(g_RhPageSize == 0x1000);
#elif defined(HOST_APPLE)
    ASSERT(g_RhPageSize == 0x4000);
#endif
}

uint32_t PalGetOsPageSize()
{
    return g_RhPageSize;
}

#if defined(TARGET_LINUX) || defined(TARGET_ANDROID)
static pthread_key_t key;
#endif

#ifdef FEATURE_HIJACK
bool InitializeSignalHandling();
#endif

// The NativeAOT PAL must be initialized before any of its exports can be called. Returns true for a successful
// initialization and false on failure.
bool PalInit()
{
#ifndef USE_PORTABLE_HELPERS
    if (!InitializeHardwareExceptionHandling())
    {
        return false;
    }
#endif // !USE_PORTABLE_HELPERS

    ConfigureSignals();

    if (!PalCreateDumpInitialize())
    {
        return false;
    }

    GCConfig::Initialize();

    if (!GCToOSInterface::Initialize())
    {
        return false;
    }

    InitializeCpuCGroup();

    InitializeCurrentProcessCpuCount();

    InitializeOsPageSize();

#ifdef FEATURE_HIJACK
    if (!InitializeSignalHandling())
    {
        return false;
    }
#endif

#if defined(TARGET_LINUX) || defined(TARGET_ANDROID)
    if (pthread_key_create(&key, RuntimeThreadShutdown) != 0)
    {
        return false;
    }
#endif

    return true;
}

#if !defined(TARGET_LINUX) && !defined(TARGET_ANDROID)
struct TlsDestructionMonitor
{
    void* m_thread = nullptr;

    void SetThread(void* thread)
    {
        m_thread = thread;
    }

    ~TlsDestructionMonitor()
    {
        if (m_thread != nullptr)
        {
            RuntimeThreadShutdown(m_thread);
        }
    }
};

// This thread local object is used to detect thread shutdown. Its destructor
// is called when a thread is being shut down.
thread_local TlsDestructionMonitor tls_destructionMonitor;
#endif

// This thread local variable is used for delegate marshalling
PLATFORM_THREAD_LOCAL intptr_t tls_thunkData;

#ifdef FEATURE_EMULATED_TLS
EXTERN_C intptr_t* RhpGetThunkData()
{
    return &tls_thunkData;
}
#endif //FEATURE_EMULATED_TLS

FCIMPL0(intptr_t, RhGetCurrentThunkContext)
{
    return tls_thunkData;
}
FCIMPLEND

// Register the thread with OS to be notified when thread is about to be destroyed
// It fails fast if a different thread was already registered.
// Parameters:
//  thread        - thread to attach
void PalAttachThread(void* thread)
{
#if defined(TARGET_LINUX) || defined(TARGET_ANDROID)
    if (pthread_setspecific(key, thread) != 0)
    {
        _ASSERTE(!"pthread_setspecific failed");
        RhFailFast();
    }
#else
    tls_destructionMonitor.SetThread(thread);
#endif

    UnmaskActivationSignal();
}

#if !defined(USE_PORTABLE_HELPERS) && !defined(FEATURE_RX_THUNKS)

UInt32_BOOL PalAllocateThunksFromTemplate(HANDLE hTemplateModule, uint32_t templateRva, size_t templateSize, void** newThunksOut)
{
#ifdef TARGET_APPLE
    vm_address_t addr, taddr;
    vm_prot_t prot, max_prot;
    kern_return_t ret;

    // Allocate two contiguous ranges of memory: the first range will contain the stubs
    // and the second range will contain their data.
    do
    {
        ret = vm_allocate(mach_task_self(), &addr, templateSize * 2, VM_FLAGS_ANYWHERE);
    } while (ret == KERN_ABORTED);

    if (ret != KERN_SUCCESS)
    {
        return UInt32_FALSE;
    }

    do
    {
        ret = vm_remap(
            mach_task_self(), &addr, templateSize, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
            mach_task_self(), ((vm_address_t)hTemplateModule + templateRva), FALSE, &prot, &max_prot, VM_INHERIT_SHARE);
    } while (ret == KERN_ABORTED);

    if (ret != KERN_SUCCESS)
    {
        do
        {
            ret = vm_deallocate(mach_task_self(), addr, templateSize * 2);
        } while (ret == KERN_ABORTED);

        return UInt32_FALSE;
    }

    *newThunksOut = (void*)addr;

    return UInt32_TRUE;
#else
    PORTABILITY_ASSERT("UNIXTODO: Implement this function");
#endif
}

UInt32_BOOL PalFreeThunksFromTemplate(void *pBaseAddress, size_t templateSize)
{
#ifdef TARGET_APPLE
    kern_return_t ret;

    do
    {
        ret = vm_deallocate(mach_task_self(), (vm_address_t)pBaseAddress, templateSize * 2);
    } while (ret == KERN_ABORTED);

    return ret == KERN_SUCCESS ? UInt32_TRUE : UInt32_FALSE;
#else
    PORTABILITY_ASSERT("UNIXTODO: Implement this function");
#endif
}
#endif // !USE_PORTABLE_HELPERS && !FEATURE_RX_THUNKS

UInt32_BOOL PalMarkThunksAsValidCallTargets(
    void *virtualAddress,
    int thunkSize,
    int thunksPerBlock,
    int thunkBlockSize,
    int thunkBlocksPerMapping)
{
    int ret = mprotect(
        (void*)((uintptr_t)virtualAddress + (thunkBlocksPerMapping * OS_PAGE_SIZE)),
        thunkBlocksPerMapping * OS_PAGE_SIZE,
        PROT_READ | PROT_WRITE);
    return ret == 0 ? UInt32_TRUE : UInt32_FALSE;
}

void PalSleep(uint32_t milliseconds)
{
#if HAVE_CLOCK_NANOSLEEP
    timespec endTime;
    clock_gettime(CLOCK_MONOTONIC, &endTime);
    TimeSpecAdd(&endTime, milliseconds);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &endTime, NULL) == EINTR)
    {
    }
#else // HAVE_CLOCK_NANOSLEEP
    timespec requested;
    requested.tv_sec = milliseconds / tccSecondsToMilliSeconds;
    requested.tv_nsec = (milliseconds - requested.tv_sec * tccSecondsToMilliSeconds) * tccMilliSecondsToNanoSeconds;

    timespec remaining;
    while (nanosleep(&requested, &remaining) == EINTR)
    {
        requested = remaining;
    }
#endif // HAVE_CLOCK_NANOSLEEP
}

UInt32_BOOL __stdcall PalSwitchToThread()
{
    // sched_yield yields to another thread in the current process.
    sched_yield();

    // The return value of sched_yield indicates the success of the call and does not tell whether a context switch happened.
    // On Linux sched_yield is documented as never failing.
    // Since we do not know if there was a context switch, we will just return `false`.
    return false;
}

UInt32_BOOL PalAreShadowStacksEnabled()
{
    return false;
}

UInt32_BOOL PalCloseHandle(HANDLE handle)
{
    if ((handle == NULL) || (handle == INVALID_HANDLE_VALUE))
    {
        return UInt32_FALSE;
    }

    UnixEvent* event = (UnixEvent*)handle;
    bool success = event->Destroy();
    delete event;

    return success ? UInt32_TRUE : UInt32_FALSE;
}

HANDLE PalCreateEventW(_In_opt_ LPSECURITY_ATTRIBUTES pEventAttributes, UInt32_BOOL manualReset, UInt32_BOOL initialState, _In_opt_z_ const WCHAR* pName)
{
    UnixEvent* event = new (nothrow) UnixEvent(manualReset, initialState);
    if (event == NULL)
    {
        return INVALID_HANDLE_VALUE;
    }
    if (!event->Initialize())
    {
        delete event;
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)event;
}

typedef uint32_t(__stdcall *BackgroundCallback)(_In_opt_ void* pCallbackContext);

bool PalStartBackgroundWork(_In_ BackgroundCallback callback, _In_opt_ void* pCallbackContext, UInt32_BOOL highPriority)
{
#ifdef HOST_WASM
    // No threads, so we can't start one
    ASSERT(false);
#endif // HOST_WASM
    pthread_attr_t attrs;

    int st = pthread_attr_init(&attrs);
    ASSERT(st == 0);
    
    size_t stacksize = GetDefaultStackSizeSetting();
    if (stacksize != 0)
    {
        st = pthread_attr_setstacksize(&attrs, stacksize);
        ASSERT(st == 0);
    }

    static const int NormalPriority = 0;
    static const int HighestPriority = -20;

    // TODO: Figure out which scheduler to use, the default one doesn't seem to
    // support per thread priorities.
#if 0
    sched_param params;
    memset(&params, 0, sizeof(params));

    params.sched_priority = highPriority ? HighestPriority : NormalPriority;

    // Set the priority of the thread
    st = pthread_attr_setschedparam(&attrs, &params);
    ASSERT(st == 0);
#endif
    // Create the thread as detached, that means not joinable
    st = pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);
    ASSERT(st == 0);

    pthread_t threadId;
    st = pthread_create(&threadId, &attrs, (void *(*)(void*))callback, pCallbackContext);

    int st2 = pthread_attr_destroy(&attrs);
    ASSERT(st2 == 0);

    return st == 0;
}

bool PalSetCurrentThreadName(const char* name)
{
    // Ignore requests to set the main thread name because
    // it causes the value returned by Process.ProcessName to change.
    if ((pid_t)PalGetCurrentOSThreadId() != getpid())
    {
        int setNameResult = minipal_set_thread_name(pthread_self(), name);
        (void)setNameResult; // used
        assert(setNameResult == 0);
    }
    return true;
}

bool PalStartBackgroundGCThread(_In_ BackgroundCallback callback, _In_opt_ void* pCallbackContext)
{
    return PalStartBackgroundWork(callback, pCallbackContext, UInt32_FALSE);
}

bool PalStartFinalizerThread(_In_ BackgroundCallback callback, _In_opt_ void* pCallbackContext)
{
    return PalStartBackgroundWork(callback, pCallbackContext, UInt32_TRUE);
}

bool PalStartEventPipeHelperThread(_In_ BackgroundCallback callback, _In_opt_ void* pCallbackContext)
{
    return PalStartBackgroundWork(callback, pCallbackContext, UInt32_FALSE);
}

HANDLE PalGetModuleHandleFromPointer(_In_ void* pointer)
{
    HANDLE moduleHandle = NULL;

    // Emscripten's implementation of dladdr corrupts memory,
    // but always returns 0 for the module handle, so just skip the call
#if !defined(HOST_WASM)
    Dl_info info;
    int st = dladdr(pointer, &info);
    if (st != 0)
    {
        moduleHandle = info.dli_fbase;
    }
#endif //!defined(HOST_WASM)

    return moduleHandle;
}

void PalPrintFatalError(const char* message)
{
    // Write the message using lowest-level OS API available. This is used to print the stack overflow
    // message, so there is not much that can be done here.
    // write() has __attribute__((warn_unused_result)) in glibc, for which gcc 11+ issue `-Wunused-result` even with `(void)write(..)`,
    // so we use additional NOT(!) operator to force unused-result suppression.
    (void)!write(STDERR_FILENO, message, strlen(message));
}

char* PalCopyTCharAsChar(const TCHAR* toCopy)
{
    NewArrayHolder<char> copy {new (nothrow) char[strlen(toCopy) + 1]};
    strcpy(copy, toCopy);
    return copy.Extract();
}

HANDLE PalLoadLibrary(const char* moduleName)
{
    return dlopen(moduleName, RTLD_LAZY);
}

void* PalGetProcAddress(HANDLE module, const char* functionName)
{
    return dlsym(module, functionName);
}

static int W32toUnixAccessControl(uint32_t flProtect)
{
    int prot = 0;

    switch (flProtect & 0xff)
    {
    case PAGE_NOACCESS:
        prot = PROT_NONE;
        break;
    case PAGE_READWRITE:
        prot = PROT_READ | PROT_WRITE;
        break;
    case PAGE_EXECUTE_READ:
        prot = PROT_READ | PROT_EXEC;
        break;
    case PAGE_EXECUTE_READWRITE:
        prot = PROT_READ | PROT_WRITE | PROT_EXEC;
        break;
    case PAGE_READONLY:
        prot = PROT_READ;
        break;
    default:
        ASSERT(false);
        break;
    }
    return prot;
}

_Ret_maybenull_ _Post_writable_byte_size_(size) void* PalVirtualAlloc(size_t size, uint32_t protect)
{
    int unixProtect = W32toUnixAccessControl(protect);

    int flags = MAP_ANON | MAP_PRIVATE;

#if defined(HOST_APPLE) && defined(HOST_ARM64)
    if (unixProtect & PROT_EXEC)
    {
        flags |= MAP_JIT;
    }
#endif
    void* pMappedMemory = mmap(NULL, size, unixProtect, flags, -1, 0);
    if (pMappedMemory == MAP_FAILED)
        return NULL;
    return pMappedMemory;
}

void PalVirtualFree(_In_ void* pAddress, size_t size)
{
    munmap(pAddress, size);
}

UInt32_BOOL PalVirtualProtect(_In_ void* pAddress, size_t size, uint32_t protect)
{
    int unixProtect = W32toUnixAccessControl(protect);

    // mprotect expects the address to be page-aligned
    uint8_t* pPageStart = ALIGN_DOWN((uint8_t*)pAddress, OS_PAGE_SIZE);
    size_t memSize = ALIGN_UP((uint8_t*)pAddress + size, OS_PAGE_SIZE) - pPageStart;

    return mprotect(pPageStart, memSize, unixProtect) == 0;
}

#if (defined(HOST_MACCATALYST) || defined(HOST_IOS) || defined(HOST_TVOS)) && defined(HOST_ARM64)
extern "C" void sys_icache_invalidate(const void* start, size_t len);
#endif

void PalFlushInstructionCache(_In_ void* pAddress, size_t size)
{
#if defined(__linux__) && defined(HOST_ARM)
    // On Linux/arm (at least on 3.10) we found that there is a problem with __do_cache_op (arch/arm/kernel/traps.c)
    // implementing cacheflush syscall. cacheflush flushes only the first page in range [pAddress, pAddress + size)
    // and leaves other pages in undefined state which causes random tests failures (often due to SIGSEGV) with no particular pattern.
    //
    // As a workaround, we call __builtin___clear_cache on each page separately.

    uint8_t* begin = (uint8_t*)pAddress;
    uint8_t* end = begin + size;

    while (begin < end)
    {
        uint8_t* endOrNextPageBegin = ALIGN_UP(begin + 1, OS_PAGE_SIZE);
        if (endOrNextPageBegin > end)
            endOrNextPageBegin = end;

        __builtin___clear_cache((char *)begin, (char *)endOrNextPageBegin);
        begin = endOrNextPageBegin;
    }
#elif (defined(HOST_MACCATALYST) || defined(HOST_IOS) || defined(HOST_TVOS)) && defined(HOST_ARM64)
    sys_icache_invalidate (pAddress, size);
#else
    __builtin___clear_cache((char *)pAddress, (char *)pAddress + size);
#endif
}

uint32_t PalGetCurrentProcessId()
{
    return getpid();
}

UInt32_BOOL PalSetEvent(HANDLE event)
{
    UnixEvent* unixEvent = (UnixEvent*)event;
    unixEvent->Set();
    return UInt32_TRUE;
}

UInt32_BOOL PalResetEvent(HANDLE event)
{
    UnixEvent* unixEvent = (UnixEvent*)event;
    unixEvent->Reset();
    return UInt32_TRUE;
}

uint32_t PalGetEnvironmentVariable(const char * name, char * buffer, uint32_t size)
{
    const char* value = getenv(name);
    if (value == NULL)
    {
        return 0;
    }

    size_t valueLen = strlen(value);
    if (valueLen < size)
    {
        strcpy(buffer, value);
        return valueLen;
    }

    // return required size including the null character or 0 if the size doesn't fit into uint32_t
    return (valueLen < UINT32_MAX) ? (valueLen + 1) : 0;
}

uint16_t PalCaptureStackBackTrace(uint32_t arg1, uint32_t arg2, void* arg3, uint32_t* arg4)
{
    // UNIXTODO: Implement this function
    return 0;
}

#ifdef FEATURE_HIJACK
static struct sigaction g_previousActivationHandler;

static void ActivationHandler(int code, siginfo_t* siginfo, void* context)
{
    // Only accept activations from the current process
    if (siginfo->si_pid == getpid()
#ifdef HOST_APPLE
        // On Apple platforms si_pid is sometimes 0. It was confirmed by Apple to be expected, as the si_pid is tracked at the process level. So when multiple
        // signals are in flight in the same process at the same time, it may be overwritten / zeroed.
        || siginfo->si_pid == 0
#endif
        )
    {
        // Make sure that errno is not modified
        int savedErrNo = errno;
        Thread::HijackCallback((NATIVE_CONTEXT*)context, NULL);
        errno = savedErrNo;
    }

    Thread* pThread = ThreadStore::GetCurrentThreadIfAvailable();
    if (pThread)
    {
        pThread->SetActivationPending(false);
    }

    // Call the original handler when it is not ignored or default (terminate).
    if (g_previousActivationHandler.sa_flags & SA_SIGINFO)
    {
        _ASSERTE(g_previousActivationHandler.sa_sigaction != NULL);
        g_previousActivationHandler.sa_sigaction(code, siginfo, context);
    }
    else
    {
        if (g_previousActivationHandler.sa_handler != SIG_IGN &&
            g_previousActivationHandler.sa_handler != SIG_DFL)
        {
            _ASSERTE(g_previousActivationHandler.sa_handler != NULL);
            g_previousActivationHandler.sa_handler(code);
        }
    }
}

bool InitializeSignalHandling()
{
#ifdef __APPLE__
    void *libSystem = dlopen("/usr/lib/libSystem.dylib", RTLD_LAZY);
    if (libSystem != NULL)
    {
        int (*dispatch_allow_send_signals_ptr)(int) = (int (*)(int))dlsym(libSystem, "dispatch_allow_send_signals");
        if (dispatch_allow_send_signals_ptr != NULL)
        {
            int status = dispatch_allow_send_signals_ptr(INJECT_ACTIVATION_SIGNAL);
            _ASSERTE(status == 0);
        }
    }

    // TODO: Once our CI tools can get upgraded to xcode >= 15.3, replace the code above by this:
    // if (__builtin_available(macOS 14.4, iOS 17.4, tvOS 17.4, *))
    // {
    //    // Allow sending the activation signal to dispatch queue threads
    //    int status = dispatch_allow_send_signals(INJECT_ACTIVATION_SIGNAL);
    //    _ASSERTE(status == 0);
    // }
#endif // __APPLE__

    return AddSignalHandler(INJECT_ACTIVATION_SIGNAL, ActivationHandler, &g_previousActivationHandler);
}

HijackFunc* PalGetHijackTarget(HijackFunc* defaultHijackTarget)
{
    return defaultHijackTarget;
}

void PalHijack(Thread* pThreadToHijack)
{
    pThreadToHijack->SetActivationPending(true);

    int status = pthread_kill(pThreadToHijack->GetOSThreadHandle(), INJECT_ACTIVATION_SIGNAL);

    // We can get EAGAIN when printing stack overflow stack trace and when other threads hit
    // stack overflow too. Those are held in the sigsegv_handler with blocked signals until
    // the process exits.
    // ESRCH may happen on some OSes when the thread is exiting.
    if ((status == EAGAIN)
     || (status == ESRCH)
#ifdef __APPLE__
        // On Apple, pthread_kill is not allowed to be sent to dispatch queue threads on macOS older than 14.4 or iOS/tvOS older than 17.4
     || (status == ENOTSUP)
#endif
       )
    {
        pThreadToHijack->SetActivationPending(false);
        return;
    }

    if (status != 0)
    {
        // Causes creation of a crash dump if enabled
        PalCreateCrashDumpIfEnabled();

        // Failure to send the signal is fatal. There are only two cases when sending
        // the signal can fail. First, if the signal ID is invalid and second,
        // if the thread doesn't exist anymore.
        abort();
    }
}
#endif // FEATURE_HIJACK

uint32_t PalWaitForSingleObjectEx(HANDLE handle, uint32_t milliseconds, UInt32_BOOL alertable)
{
    UnixEvent* unixEvent = (UnixEvent*)handle;
    return unixEvent->Wait(milliseconds);
}

uint32_t PalCompatibleWaitAny(UInt32_BOOL alertable, uint32_t timeout, uint32_t handleCount, HANDLE* pHandles, UInt32_BOOL allowReentrantWait)
{
    // Only a single handle wait for event is supported
    ASSERT(handleCount == 1);

    return PalWaitForSingleObjectEx(pHandles[0], timeout, alertable);
}

HANDLE PalCreateLowMemoryResourceNotification()
{
    return NULL;
}

#if !__has_builtin(_mm_pause)
extern "C" void _mm_pause()
// Defined for implementing PalYieldProcessor in Pal.h
{
#if defined(HOST_AMD64) || defined(HOST_X86)
  __asm__ volatile ("pause");
#endif
}
#endif

int32_t _stricmp(const char *string1, const char *string2)
{
    return strcasecmp(string1, string2);
}

uint32_t g_RhNumberOfProcessors;

int32_t PalGetProcessCpuCount()
{
    ASSERT(g_RhNumberOfProcessors > 0);
    return g_RhNumberOfProcessors;
}

// Retrieves the entire range of memory dedicated to the calling thread's stack.  This does
// not get the current dynamic bounds of the stack, which can be significantly smaller than
// the maximum bounds.
bool PalGetMaximumStackBounds(_Out_ void** ppStackLowOut, _Out_ void** ppStackHighOut)
{
    void* pStackHighOut = NULL;
    void* pStackLowOut = NULL;

#ifdef __APPLE__
    // This is a Mac specific method
    pStackHighOut = pthread_get_stackaddr_np(pthread_self());
    pStackLowOut = ((uint8_t *)pStackHighOut - pthread_get_stacksize_np(pthread_self()));
#else // __APPLE__
    pthread_attr_t attr;
    size_t stackSize;
    int status;

    pthread_t thread = pthread_self();

    status = pthread_attr_init(&attr);
    ASSERT_MSG(status == 0, "pthread_attr_init call failed");

#if HAVE_PTHREAD_ATTR_GET_NP
    status = pthread_attr_get_np(thread, &attr);
#elif HAVE_PTHREAD_GETATTR_NP
    status = pthread_getattr_np(thread, &attr);
#else
#error Dont know how to get thread attributes on this platform!
#endif
    ASSERT_MSG(status == 0, "pthread_getattr_np call failed");

    status = pthread_attr_getstack(&attr, &pStackLowOut, &stackSize);
    ASSERT_MSG(status == 0, "pthread_attr_getstack call failed");

    status = pthread_attr_destroy(&attr);
    ASSERT_MSG(status == 0, "pthread_attr_destroy call failed");

    pStackHighOut = (uint8_t*)pStackLowOut + stackSize;
#endif // __APPLE__

    *ppStackLowOut = pStackLowOut;
    *ppStackHighOut = pStackHighOut;

    return true;
}

// retrieves the full path to the specified module, if moduleBase is NULL retreieves the full path to the
// executable module of the current process.
//
// Return value:  number of characters in name string
//
int32_t PalGetModuleFileName(_Out_ const TCHAR** pModuleNameOut, HANDLE moduleBase)
{
#if defined(HOST_WASM)
    // Emscripten's implementation of dladdr corrupts memory and doesn't have the real name, so make up a name instead
    const TCHAR* wasmModuleName = "WebAssemblyModule";
    *pModuleNameOut = wasmModuleName;
    return strlen(wasmModuleName);
#else // HOST_WASM
    Dl_info dl;
    if (dladdr(moduleBase, &dl) == 0)
    {
        *pModuleNameOut = NULL;
        return 0;
    }

    *pModuleNameOut = dl.dli_fname;
    return strlen(dl.dli_fname);
#endif // defined(HOST_WASM)
}

void PalFlushProcessWriteBuffers()
{
    GCToOSInterface::FlushProcessWriteBuffers();
}

static const int64_t SECS_BETWEEN_1601_AND_1970_EPOCHS = 11644473600LL;
static const int64_t SECS_TO_100NS = 10000000; /* 10^7 */

void PalGetSystemTimeAsFileTime(FILETIME *lpSystemTimeAsFileTime)
{
    struct timeval time = { 0 };
    gettimeofday(&time, NULL);

    int64_t result = ((int64_t)time.tv_sec + SECS_BETWEEN_1601_AND_1970_EPOCHS) * SECS_TO_100NS +
        (time.tv_usec * 10);

    lpSystemTimeAsFileTime->dwLowDateTime = (uint32_t)result;
    lpSystemTimeAsFileTime->dwHighDateTime = (uint32_t)(result >> 32);
}

uint64_t PalGetCurrentOSThreadId()
{
    return (uint64_t)minipal_get_current_thread_id();
}
