// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// ---------------------------------------------------------------------------
// StressLog.h
//
// StressLog infrastructure
//
// The StressLog is a binary, memory based circular queue of logging messages.
//   It is intended to be used in retail builds during stress runs (activated
//   by registry key), to help find bugs that only turn up during stress runs.
//
// Differently from the desktop implementation the RH implementation of the
//   stress log will log all facilities, and only filter on logging level.
//
// The log has a very simple structure, and is meant to be dumped from an NTSD
//   extension (eg. strike).
//
// debug\rhsos\stresslogdump.cpp contains the dumper utility that parses this
//   log.
// ---------------------------------------------------------------------------

#ifndef StressLog_h
#define StressLog_h  1

#ifdef _MSC_VER
#define SUPPRESS_WARNING_4127   \
    __pragma(warning(push))     \
    __pragma(warning(disable:4127)) /* conditional expression is constant*/

#define POP_WARNING_STATE       \
    __pragma(warning(pop))
#else // _MSC_VER
#define SUPPRESS_WARNING_4127
#define POP_WARNING_STATE
#endif // _MSC_VER

#define WHILE_0             \
    SUPPRESS_WARNING_4127   \
    while(0)                \
    POP_WARNING_STATE       \


// let's keep STRESS_LOG defined always...
#if !defined(STRESS_LOG) && !defined(NO_STRESS_LOG)
#define STRESS_LOG
#endif

#if defined(STRESS_LOG)

//
// Logging levels and facilities
//
#define DEFINE_LOG_FACILITY(logname, value)  logname = value,

enum LogFacilitiesEnum: unsigned int {
#include "loglf.h"
    LF_ALWAYS        = 0x80000000u, // Log message irrepespective of LogFacility (if the level matches)
    LF_ALL           = 0xFFFFFFFFu, // Used only to mask bits. Never use as LOG((LF_ALL, ...))
};


#define LL_EVERYTHING  10
#define LL_INFO1000000  9       // can be expected to generate 1,000,000 logs per small but not trivial run
#define LL_INFO100000   8       // can be expected to generate 100,000 logs per small but not trivial run
#define LL_INFO10000    7       // can be expected to generate 10,000 logs per small but not trivial run
#define LL_INFO1000     6       // can be expected to generate 1,000 logs per small but not trivial run
#define LL_INFO100      5       // can be expected to generate 100 logs per small but not trivial run
#define LL_INFO10       4       // can be expected to generate 10 logs per small but not trivial run
#define LL_WARNING      3
#define LL_ERROR        2
#define LL_FATALERROR   1
#define LL_ALWAYS       0       // impossible to turn off (log level never negative)

//
//
//

#ifndef _ASSERTE
#define _ASSERTE(expr)
#endif


#ifndef DACCESS_COMPILE


//==========================================================================================
// The STRESS_LOG* macros
//
// The STRESS_LOG* macros work like printf.  In fact the use printf in their implementation
// so all printf format specifications work.  In addition the Stress log dumper knows
// about certain suffixes for the %p format specification (normally used to print a pointer)
//
//          %pM     // The pointer is a MethodInfo -- not supported yet (use %pK instead)
//          %pT     // The pointer is a type (MethodTable)
//          %pV     // The pointer is a C++ Vtable pointer
//          %pK     // The pointer is a code address (used for call stacks or method names)
//

/*  STRESS_LOG_VA was added to allow sending GC trace output to the stress log. msg must be enclosed
    in ()'s and contain a format string followed by 0 to 12 arguments. The arguments must be numbers
     or string literals. This was done because GC Trace uses dprintf which doesn't contain info on
    how many arguments are getting passed in and using va_args would require parsing the format
    string during the GC
*/
#define _Args(...) __VA_ARGS__

#define STRESS_LOG_VA(dprintfLevel,msg) do {                                                 \
            if (StressLog::StressLogOn(LF_ALWAYS|(dprintfLevel<<16)|LF_GC, LL_ALWAYS))       \
                StressLog::LogMsgOL(_Args msg);                                              \
            } WHILE_0

#define STRESS_LOG_WRITE(facility, level, msg, ...) do {                                      \
            if (StressLog::StressLogOn(facility, level))                                      \
                StressLog::LogMsgOL(facility, msg, __VA_ARGS__);                              \
            } WHILE_0

#define STRESS_LOG0(facility, level, msg) do {                                      \
            if (StressLog::StressLogOn(facility, level))                            \
                StressLog::LogMsg(facility, 0, msg);                                \
            } WHILE_0                                                               \

#define STRESS_LOG1(facility, level, msg, data1) \
    STRESS_LOG_WRITE(facility, level, msg, data1)

#define STRESS_LOG2(facility, level, msg, data1, data2) \
    STRESS_LOG_WRITE(facility, level, msg, data1, data2)

#define STRESS_LOG3(facility, level, msg, data1, data2, data3) \
    STRESS_LOG_WRITE(facility, level, msg, data1, data2, data3)

#define STRESS_LOG4(facility, level, msg, data1, data2, data3, data4) \
    STRESS_LOG_WRITE(facility, level, msg, data1, data2, data3, data4)

#define STRESS_LOG5(facility, level, msg, data1, data2, data3, data4, data5) \
    STRESS_LOG_WRITE(facility, level, msg, data1, data2, data3, data4, data5)

#define STRESS_LOG6(facility, level, msg, data1, data2, data3, data4, data5, data6) \
    STRESS_LOG_WRITE(facility, level, msg, data1, data2, data3, data4, data5, data6)

#define STRESS_LOG7(facility, level, msg, data1, data2, data3, data4, data5, data6, data7)  \
    STRESS_LOG_WRITE(facility, level, msg, data1, data2, data3, data4, data5, data6, data7)

#define STRESS_LOG_RESERVE_MEM(numChunks) do {                                                \
            if (StressLog::StressLogOn(LF_ALL, LL_ALWAYS))                         \
                {StressLog::ReserveStressLogChunks (numChunks);}                              \
            } WHILE_0

// !!! WARNING !!!
// !!! DO NOT ADD STRESS_LOG8, as the stress log infrastructure supports a maximum of 7 arguments
// !!! WARNING !!!

#define STRESS_LOG_PLUG_MOVE(plug_start, plug_end, plug_delta) do {                           \
            if (StressLog::StressLogOn(LF_GC, LL_INFO1000))                                   \
                StressLog::LogMsg(LF_GC, 3, ThreadStressLog::gcPlugMoveMsg(),                 \
                (void*)(size_t)(plug_start), (void*)(size_t)(plug_end), (void*)(size_t)(plug_delta)); \
            } WHILE_0

#define STRESS_LOG_ROOT_PROMOTE(root_addr, objPtr, methodTable) do {                          \
            if (StressLog::StressLogOn(LF_GC|LF_GCROOTS, LL_INFO1000))                        \
                StressLog::LogMsg(LF_GC|LF_GCROOTS, 3, ThreadStressLog::gcRootPromoteMsg(),   \
                    (void*)(size_t)(root_addr), (void*)(size_t)(objPtr), (void*)(size_t)(methodTable)); \
            } WHILE_0

#define STRESS_LOG_ROOT_RELOCATE(root_addr, old_value, new_value, methodTable) do {           \
            if (StressLog::StressLogOn(LF_GC|LF_GCROOTS, LL_INFO1000) && ((size_t)(old_value) != (size_t)(new_value))) \
                StressLog::LogMsg(LF_GC|LF_GCROOTS, 4, ThreadStressLog::gcRootMsg(),          \
                    (void*)(size_t)(root_addr), (void*)(size_t)(old_value),                   \
                    (void*)(size_t)(new_value), (void*)(size_t)(methodTable));                \
            } WHILE_0

#define STRESS_LOG_GC_START(gcCount, Gen, collectClasses) do {                                \
            if (StressLog::StressLogOn(LF_GCROOTS|LF_GC|LF_GCALLOC, LL_INFO10))               \
                StressLog::LogMsg(LF_GCROOTS|LF_GC|LF_GCALLOC, 3, ThreadStressLog::gcStartMsg(),        \
                    (void*)(size_t)(gcCount), (void*)(size_t)(Gen), (void*)(size_t)(collectClasses));   \
            } WHILE_0

#define STRESS_LOG_GC_END(gcCount, Gen, collectClasses) do {                                  \
            if (StressLog::StressLogOn(LF_GCROOTS|LF_GC|LF_GCALLOC, LL_INFO10))               \
                StressLog::LogMsg(LF_GCROOTS|LF_GC|LF_GCALLOC, 3, ThreadStressLog::gcEndMsg(),\
                    (void*)(size_t)(gcCount), (void*)(size_t)(Gen), (void*)(size_t)(collectClasses), 0);\
            } WHILE_0

#if defined(_DEBUG)
#define MAX_CALL_STACK_TRACE          20
#define STRESS_LOG_OOM_STACK(size) do {                                                       \
                if (StressLog::StressLogOn(LF_ALWAYS, LL_ALWAYS))                              \
                {                                                                             \
                    StressLog::LogMsgOL("OOM on alloc of size %x \n", (void*)(size_t)(size)); \
                    StressLog::LogCallStack ("OOM");                                          \
                }                                                                             \
            } WHILE_0
#define STRESS_LOG_GC_STACK do {                                                              \
                if (StressLog::StressLogOn(LF_GC |LF_GCINFO, LL_ALWAYS))                      \
                {                                                                             \
                    StressLog::LogMsgOL("GC is triggered \n");                                \
                    StressLog::LogCallStack ("GC");                                           \
                }                                                                             \
            } WHILE_0
#else //_DEBUG
#define STRESS_LOG_OOM_STACK(size)
#define STRESS_LOG_GC_STACK
#endif //_DEBUG

#endif // DACCESS_COMPILE

//
// forward declarations:
//
class CrstStatic;
class Thread;
typedef DPTR(Thread) PTR_Thread;
class StressLog;
typedef DPTR(StressLog) PTR_StressLog;
class ThreadStressLog;
typedef DPTR(ThreadStressLog) PTR_ThreadStressLog;
struct StressLogChunk;
typedef DPTR(StressLogChunk) PTR_StressLogChunk;
struct DacpStressLogEnumCBArgs;
extern "C" void PopulateDebugHeaders();


//==========================================================================================
// StressLog - per-thread circular queue of stresslog messages
//
class StressLog {
    friend void PopulateDebugHeaders();
public:
// private:
    unsigned facilitiesToLog;               // Bitvector of facilities to log (see loglf.h)
    unsigned levelToLog;                    // log level
    unsigned MaxSizePerThread;              // maximum number of bytes each thread should have before wrapping
    unsigned MaxSizeTotal;                  // maximum memory allowed for stress log
    int32_t totalChunk;                       // current number of total chunks allocated
    PTR_ThreadStressLog logs;               // the list of logs for every thread.
    int32_t deadCount;                        // count of dead threads in the log
    CrstStatic *pLock;                      // lock
    uint64_t tickFrequency;         // number of ticks per second
    uint64_t startTimeStamp;        // start time from when tick counter started
    FILETIME startTime;                     // time the application started
    size_t   moduleOffset;                  // Used to compute format strings.

#ifndef DACCESS_COMPILE
public:
    static void Initialize(unsigned facilities, unsigned level, unsigned maxBytesPerThread,
                    unsigned maxBytesTotal, HANDLE hMod);
    // Called at DllMain THREAD_DETACH to recycle thread's logs
    static void ThreadDetach(ThreadStressLog *msgs);
    static long NewChunk ()     { return PalInterlockedIncrement (&theLog.totalChunk); }
    static long ChunkDeleted () { return PalInterlockedDecrement (&theLog.totalChunk); }

    //the result is not 100% accurate. If multiple threads call this function at the same time,
    //we could allow the total size be bigger than required. But the memory won't grow forever
    //and this is not critical so we don't try to fix the race
    static bool AllowNewChunk (long numChunksInCurThread);

    //preallocate Stress log chunks for current thread. The memory we could preallocate is still
    //bounded by per thread size limit and total size limit. If chunksToReserve is 0, we will try to
    //preallocate up to per thread size limit
    static bool ReserveStressLogChunks (unsigned int chunksToReserve);

// private:
    static ThreadStressLog* CreateThreadStressLog(Thread * pThread);
    static ThreadStressLog* CreateThreadStressLogHelper(Thread * pThread);

#else // DACCESS_COMPILE
public:
    bool Initialize();

    // Can't refer to the types in sospriv.h because it drags in windows.h
    void EnumerateStressMsgs(/*STRESSMSGCALLBACK*/ void* smcb, /*ENDTHREADLOGCALLBACK*/ void* etcb,
                                        void *token);
    void EnumStressLogMemRanges(/*STRESSLOGMEMRANGECALLBACK*/ void* slmrcb, void *token);

    // Called while dumping logs after operations are completed, to ensure DAC-caches
    // allow the stress logs to be dumped again
    void ResetForRead();

    ThreadStressLog* FindLatestThreadLog() const;

    friend class ClrDataAccess;

#endif // DACCESS_COMPILE

#ifndef DACCESS_COMPILE
public:
    FORCEINLINE static bool StressLogOn(unsigned /*facility*/, unsigned level)
    {
    #if defined(DACCESS_COMPILE)
        UNREFERENCED_PARAMETER(level);
        return FALSE;
    #else
        // In NativeAOT, we have rationalized facility codes and have much
        // fewer compared to desktop, as such we'll log all facilities and
        // limit the filtering to the log level...
        return
            // (theLog.facilitiesToLog & facility)
            //  &&
            (level <= theLog.levelToLog);
    #endif
    }

    static void LogMsg(unsigned facility, int cArgs, const char* format, ... );

    // Support functions for STRESS_LOG_VA
    // We disable the warning "conversion from 'type' to 'type' of greater size" since everything will
    // end up on the stack, and LogMsg will know the size of the variable based on the format string.
    #ifdef _MSC_VER
    #pragma warning( push )
    #pragma warning( disable : 4312 )
    #endif

    template<typename T>
    static void* ConvertArgument(T arg)
    {
        C_ASSERT(sizeof(T) <= sizeof(void*));
        return (void*)(size_t)arg;
    }

    template<typename... Ts>
    static void LogMsgOL(const char* format, Ts... args)
    {
        LogMsg(LF_GC, sizeof...(args), format, ConvertArgument(args)...);
    }

    template<typename... Ts>
    static void LogMsgOL(unsigned facility, const char* format, Ts... args)
    {
        LogMsg(facility, sizeof...(args), format, ConvertArgument(args)...);
    }

    #ifdef _MSC_VER
    #pragma warning( pop )
    #endif

// We can only log the stacktrace on DEBUG builds!
#ifdef _DEBUG
    static void LogCallStack(const char *const callTag);
#endif //_DEBUG

#endif // DACCESS_COMPILE

// private: // static variables
    static StressLog theLog;    // We only have one log, and this is it
};


#if TARGET_64BIT
template<>
inline void* StressLog::ConvertArgument(double arg)
{
    return (void*)(size_t)(*((uint64_t*)&arg));
}

// COMPAT: Convert 32-bit floats to 64-bit doubles.
template<>
inline void* StressLog::ConvertArgument(float arg)
{
    return StressLog::ConvertArgument((double)arg);
}
#else
template<>
void* StressLog::ConvertArgument(double arg) = delete;

template<>
void* StressLog::ConvertArgument(float arg) = delete;

// COMPAT: Truncate 64-bit integer arguments to 32-bit
template<>
inline void* StressLog::ConvertArgument(uint64_t arg)
{
    return (void*)(size_t)arg;
}

template<>
inline void* StressLog::ConvertArgument(int64_t arg)
{
    return (void*)(size_t)arg;
}
#endif

//==========================================================================================
// Private classes
//

#if defined(_MSC_VER)
// don't warn about 0 sized array below or unnamed structures
#pragma warning(disable:4200 4201)
#endif

//==========================================================================================
// The order of fields is important.  Ensure that we minimize padding
// to fit more messages in a chunk.
struct StressMsg
{
private:
    static const size_t formatOffsetLowBits = 26;
    static const size_t formatOffsetHighBits = 13;

    // We split the format offset to ensure that we utilize every bit and that
    // the compiler does not align the format offset to a new 64-bit boundary.
    uint64_t facility: 32;                           // facility used to log the entry
    uint64_t numberOfArgs : 6;                       // number of arguments
    uint64_t formatOffsetLow: formatOffsetLowBits;   // offset of format string in modules
    uint64_t formatOffsetHigh: formatOffsetHighBits; // offset of format string in modules
    uint64_t timeStamp: 51;                          // time when msg was logged (100ns ticks since runtime start)

public:
    void*     args[0];                               // size given by numberOfArgs

    void SetFormatOffset(uint64_t offset)
    {
        formatOffsetLow = (uint32_t)(offset & ((1 << formatOffsetLowBits) - 1));
        formatOffsetHigh = offset >> formatOffsetLowBits;
    }

    uint64_t GetFormatOffset()
    {
        return (formatOffsetHigh << formatOffsetLowBits) | formatOffsetLow;
    }

    void SetNumberOfArgs(uint32_t num)
    {
        numberOfArgs = num;
    }

    uint32_t GetNumberOfArgs()
    {
        return numberOfArgs;
    }

    void SetFacility(uint32_t fac)
    {
        facility = fac;
    }

    uint32_t GetFacility()
    {
        return facility;
    }

    uint64_t GetTimeStamp()
    {
        return timeStamp;
    }

    void SetTimeStamp(uint64_t time)
    {
        timeStamp = time;
    }

    static const size_t maxArgCnt = 63;
    static const int64_t maxOffset = (int64_t)1 << (formatOffsetLowBits + formatOffsetHighBits);
    static constexpr size_t maxMsgSize = sizeof(uint64_t) * 2 + maxArgCnt * sizeof(void*);

    friend void PopulateDebugHeaders();
};

static_assert(sizeof(StressMsg) == sizeof(uint64_t) * 2, "StressMsg bitfields aren't aligned correctly");

#ifdef _WIN64
#define STRESSLOG_CHUNK_SIZE (32 * 1024)
#else //_WIN64
#define STRESSLOG_CHUNK_SIZE (16 * 1024)
#endif //_WIN64
#define GC_STRESSLOG_MULTIPLY (5)

//==========================================================================================
// StressLogChunk
//
//  A chunk of contiguous memory containing instances of StressMsg
//
struct StressLogChunk
{
    PTR_StressLogChunk prev;
    PTR_StressLogChunk next;
    char buf[STRESSLOG_CHUNK_SIZE];
    uint32_t dwSig1;
    uint32_t dwSig2;

#ifndef DACCESS_COMPILE

    StressLogChunk (PTR_StressLogChunk p = NULL, PTR_StressLogChunk n = NULL)
        :prev (p), next (n), dwSig1 (0xCFCFCFCF), dwSig2 (0xCFCFCFCF)
    {}

#endif //!DACCESS_COMPILE

    char * StartPtr ()
    {
        return buf;
    }

    char * EndPtr ()
    {
        return buf + STRESSLOG_CHUNK_SIZE;
    }

    bool IsValid () const
    {
        return dwSig1 == 0xCFCFCFCF && dwSig2 == 0xCFCFCFCF;
    }
};

//==========================================================================================
// ThreadStressLog
//
// This class implements a circular stack of variable sized elements
//    .The buffer between startPtr-endPtr is used in a circular manner
//     to store instances of the variable-sized struct StressMsg.
//     The StressMsg are always aligned to endPtr, while the space
//     left between startPtr and the last element is 0-padded.
//    .curPtr points to the most recently written log message
//    .readPtr points to the next log message to be dumped
//    .hasWrapped is TRUE while dumping the log, if we had wrapped
//     past the endPtr marker, back to startPtr
// The AdvanceRead/AdvanceWrite operations simply update the
//     readPtr / curPtr fields. thecaller is responsible for reading/writing
//     to the corresponding field
class ThreadStressLog {
    PTR_ThreadStressLog next;   // we keep a linked list of these
    uint64_t   threadId;        // the id for the thread using this buffer
    bool       isDead;          // Is this thread dead
    bool       readHasWrapped;      // set when read ptr has passed chunListTail
    bool       writeHasWrapped;     // set when write ptr has passed chunListHead
    StressMsg* curPtr;          // where packets are being put on the queue
    StressMsg* readPtr;         // where we are reading off the queue (used during dumping)
    PTR_StressLogChunk chunkListHead; //head of a list of stress log chunks
    PTR_StressLogChunk chunkListTail; //tail of a list of stress log chunks
    PTR_StressLogChunk curReadChunk;  //the stress log chunk we are currently reading
    PTR_StressLogChunk curWriteChunk; //the stress log chunk we are currently writing
    long chunkListLength;       // how many stress log chunks are in this stress log
    PTR_Thread pThread;         // thread associated with these stress logs
    StressMsg * origCurPtr;     // this holds the original curPtr before we start the dump

    friend void PopulateDebugHeaders();
    friend class StressLog;

#ifndef DACCESS_COMPILE
public:
    inline ThreadStressLog ();
    inline ~ThreadStressLog ();

    void LogMsg ( uint32_t facility, int cArgs, const char* format, ... )
    {
        va_list Args;
        va_start(Args, format);
        LogMsg (facility, cArgs, format, Args);
    }

    void LogMsg ( uint32_t facility, int cArgs, const char* format, va_list Args);

private:
    FORCEINLINE StressMsg* AdvanceWrite(int cArgs);
    inline StressMsg* AdvWritePastBoundary(int cArgs);
    FORCEINLINE bool GrowChunkList ();

#else // DACCESS_COMPILE
public:
    friend class ClrDataAccess;

    // Called while dumping.  Returns true after all messages in log were dumped
    FORCEINLINE bool CompletedDump ();

private:
    FORCEINLINE bool IsReadyForRead()       { return readPtr != NULL; }
    FORCEINLINE StressMsg* AdvanceRead(uint32_t cArgs);
    inline StressMsg* AdvReadPastBoundary();
#endif //!DACCESS_COMPILE

public:
    void Activate (Thread * pThread);

    bool IsValid () const
    {
        return chunkListHead != NULL && (!curWriteChunk || curWriteChunk->IsValid ());
    }

    #define STATIC_CONTRACT_LEAF
    #include "../../../inc/gcmsg.inl"
    #undef STATIC_CONTRACT_LEAF
};


//==========================================================================================
// Inline implementations:
//

#ifdef DACCESS_COMPILE

//------------------------------------------------------------------------------------------
// Called while dumping.  Returns true after all messages in log were dumped
FORCEINLINE bool ThreadStressLog::CompletedDump ()
{
    return readPtr->timeStamp == 0
            //if read has passed end of list but write has not passed head of list yet, we are done
            //if write has also wrapped, we are at the end if read pointer passed write pointer
            || (readHasWrapped &&
                    (!writeHasWrapped || (curReadChunk == curWriteChunk && readPtr >= curPtr)));
}

//------------------------------------------------------------------------------------------
// Called when dumping the log (by StressLog::Dump())
// Updates readPtr to point to next stress messaage to be dumped
inline StressMsg* ThreadStressLog::AdvanceRead(uint32_t cArgs) {
    // advance the marker
    readPtr = (StressMsg*)((char*)readPtr + sizeof(StressMsg) + cArgs * sizeof(void*));
    // wrap around if we need to
    if (readPtr >= (StressMsg *)curReadChunk->EndPtr ())
    {
        AdvReadPastBoundary();
    }
    return readPtr;
}

//------------------------------------------------------------------------------------------
// The factored-out slow codepath for AdvanceRead(), only called by AdvanceRead().
// Updates readPtr to and returns the first stress message >= startPtr
inline StressMsg* ThreadStressLog::AdvReadPastBoundary() {
    //if we pass boundary of tail list, we need to set has Wrapped
    if (curReadChunk == chunkListTail)
    {
        readHasWrapped = true;
        //If write has not wrapped, we know the contents from list head to
        //cur pointer is garbage, we don't need to read them
        if (!writeHasWrapped)
        {
            return readPtr;
        }
    }
    curReadChunk = curReadChunk->next;
    void** p = (void**)curReadChunk->StartPtr();
    while (*p == NULL && (size_t)(p-(void**)curReadChunk->StartPtr ()) < (StressMsg::maxMsgSize/sizeof(void*)))
    {
        ++p;
    }
    // if we failed to find a valid start of a StressMsg fallback to startPtr (since timeStamp==0)
    if (*p == NULL)
    {
        p = (void**) curReadChunk->StartPtr ();
    }
    readPtr = (StressMsg*)p;

    return readPtr;
}

#else // DACCESS_COMPILE

//------------------------------------------------------------------------------------------
// Initialize a ThreadStressLog
inline ThreadStressLog::ThreadStressLog()
{
    chunkListHead = chunkListTail = curWriteChunk = NULL;
    StressLogChunk * newChunk = new (nothrow) StressLogChunk;
    //OOM or in cantalloc region
    if (newChunk == NULL)
    {
        return;
    }
    StressLog::NewChunk ();

    newChunk->prev = newChunk;
    newChunk->next = newChunk;

    chunkListHead = chunkListTail = newChunk;

    next = NULL;
    isDead = TRUE;
    curPtr = NULL;
    readPtr = NULL;
    writeHasWrapped = FALSE;
    curReadChunk = NULL;
    curWriteChunk = NULL;
    chunkListLength = 1;
    origCurPtr = NULL;
}

inline ThreadStressLog::~ThreadStressLog ()
{
    //no thing to do if the list is empty (failed to initialize)
    if (chunkListHead == NULL)
    {
        return;
    }

    StressLogChunk * chunk = chunkListHead;

    do
    {
        StressLogChunk * tmp = chunk;
        chunk = chunk->next;
        delete tmp;
        StressLog::ChunkDeleted ();
    } while (chunk != chunkListHead);
}

//------------------------------------------------------------------------------------------
// Called when logging, checks if we can increase the number of stress log chunks associated
// with the current thread
FORCEINLINE bool ThreadStressLog::GrowChunkList ()
{
    _ASSERTE (chunkListLength >= 1);
    if (!StressLog::AllowNewChunk (chunkListLength))
    {
        return FALSE;
    }
    StressLogChunk * newChunk = new (nothrow) StressLogChunk (chunkListTail, chunkListHead);
    if (newChunk == NULL)
    {
        return FALSE;
    }
    StressLog::NewChunk ();
    chunkListLength++;
    chunkListHead->prev = newChunk;
    chunkListTail->next = newChunk;
    chunkListHead = newChunk;

    return TRUE;
}

//------------------------------------------------------------------------------------------
// Called at runtime when writing the log (by StressLog::LogMsg())
// Updates curPtr to point to the next spot in the log where we can write
// a stress message with cArgs arguments
// For convenience it returns a pointer to the empty slot where we can
// write the next stress message.
// cArgs is the number of arguments in the message to be written.
inline StressMsg* ThreadStressLog::AdvanceWrite(int cArgs) {
    // _ASSERTE(cArgs <= StressMsg::maxArgCnt);
    // advance the marker
    StressMsg* p = (StressMsg*)((char*)curPtr - sizeof(StressMsg) - cArgs*sizeof(void*));

    //past start of current chunk
    //wrap around if we need to
    if (p < (StressMsg*)curWriteChunk->StartPtr ())
    {
       curPtr = AdvWritePastBoundary(cArgs);
    }
    else
    {
        curPtr = p;
    }

    return curPtr;
}

//------------------------------------------------------------------------------------------
// This is the factored-out slow codepath for AdvanceWrite() and is only called by
// AdvanceWrite().
// Returns the stress message flushed against endPtr
// In addition it writes NULLs b/w the startPtr and curPtr
inline StressMsg* ThreadStressLog::AdvWritePastBoundary(int cArgs) {
    //zeroed out remaining buffer
    memset (curWriteChunk->StartPtr (), 0, (char *)curPtr - (char *)curWriteChunk->StartPtr ());

    //if we are already at head of the list, try to grow the list
    if (curWriteChunk == chunkListHead)
    {
        GrowChunkList ();
    }

    curWriteChunk = curWriteChunk->prev;
    if (curWriteChunk == chunkListTail)
    {
        writeHasWrapped = TRUE;
    }
    curPtr = (StressMsg*)((char*)curWriteChunk->EndPtr () - sizeof(StressMsg) - cArgs * sizeof(void*));
    return curPtr;
}

#endif // DACCESS_COMPILE

#endif // STRESS_LOG

#ifndef __GCENV_BASE_INCLUDED__
#if !defined(STRESS_LOG) || defined(DACCESS_COMPILE)
#define STRESS_LOG_VA(msg)                                              do { } WHILE_0
#define STRESS_LOG0(facility, level, msg)                               do { } WHILE_0
#define STRESS_LOG1(facility, level, msg, data1)                        do { } WHILE_0
#define STRESS_LOG2(facility, level, msg, data1, data2)                 do { } WHILE_0
#define STRESS_LOG3(facility, level, msg, data1, data2, data3)          do { } WHILE_0
#define STRESS_LOG4(facility, level, msg, data1, data2, data3, data4)   do { } WHILE_0
#define STRESS_LOG5(facility, level, msg, data1, data2, data3, data4, data5)   do { } WHILE_0
#define STRESS_LOG6(facility, level, msg, data1, data2, data3, data4, data5, data6)   do { } WHILE_0
#define STRESS_LOG7(facility, level, msg, data1, data2, data3, data4, data5, data6, data7)   do { } WHILE_0
#define STRESS_LOG_PLUG_MOVE(plug_start, plug_end, plug_delta)          do { } WHILE_0
#define STRESS_LOG_ROOT_PROMOTE(root_addr, objPtr, methodTable)         do { } WHILE_0
#define STRESS_LOG_ROOT_RELOCATE(root_addr, old_value, new_value, methodTable) do { } WHILE_0
#define STRESS_LOG_GC_START(gcCount, Gen, collectClasses)               do { } WHILE_0
#define STRESS_LOG_GC_END(gcCount, Gen, collectClasses)                 do { } WHILE_0
#define STRESS_LOG_OOM_STACK(size)          do { } WHILE_0
#define STRESS_LOG_GC_STACK                 do { } WHILE_0
#define STRESS_LOG_RESERVE_MEM(numChunks)   do { } WHILE_0
#endif // !STRESS_LOG || DACCESS_COMPILE
#endif // !__GCENV_BASE_INCLUDED__

#endif // StressLog_h
