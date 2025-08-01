// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// ===========================================================================
// File: CEELOAD.H
//

//
// CEELOAD.H defines the class use to represent the PE file
// ===========================================================================

#ifndef CEELOAD_H_
#define CEELOAD_H_

#include "common.h"
#include "vars.hpp" // for LPCUTF8
#include "hash.h"
#include "clsload.hpp"
#include "cgensys.h"
#include "corsym.h"
#include "typehandle.h"
#include "arraylist.h"
#include "peassembly.h"
#include "typehash.h"
#include "contractimpl.h"
#include "bitmask.h"
#include "instmethhash.h"
#include "eetwain.h"    // For EnumGCRefs (we should probably move that somewhere else, but can't
                        // find anything better (modulo common or vars.hpp)
#include "classloadlevel.h"
#include "precode.h"
#include "ilstubcache.h"
#include "classhash.h"

#include "corcompile.h"
#include <gcinfodecoder.h>

#include "wellknownattributes.h"

#ifdef FEATURE_READYTORUN
#include "readytoruninfo.h"
#endif

#include "ilinstrumentation.h"
#include "codeversion.h"

class MethodDesc;
class FieldDesc;
class Crst;
class RefClassWriter;
class ReflectionModule;
class EEStringData;
class MethodDescChunk;
class SigTypeContext;
class Assembly;
class AppDomain;
class SystemDomain;
class Module;
class SString;
class MethodTable;
class DynamicMethodTable;
class TieredCompilationManager;
class JITInlineTrackingMap;

#ifdef FEATURE_METADATA_UPDATER
class EnCEEClassData;
#endif // FEATURE_METADATA_UPDATER

// Hash table parameter of available classes (name -> module/class) hash
#define AVAILABLE_CLASSES_HASH_BUCKETS 1024
#define AVAILABLE_CLASSES_HASH_BUCKETS_COLLECTIBLE 128
#define PARAMTYPES_HASH_BUCKETS 23
#define PARAMMETHODS_HASH_BUCKETS 11
#define METHOD_STUBS_HASH_BUCKETS 11
#define GUID_TO_TYPE_HASH_BUCKETS 16

typedef DPTR(JITInlineTrackingMap) PTR_JITInlineTrackingMap;

//
// LookupMaps are used to implement RID maps
// It is a linked list of nodes, each handling a successive (and consecutive)
// range of RIDs.
//
// LookupMapBase is non-type safe implementation of the worker methods. LookupMap is type
// safe wrapper around it.
//

typedef DPTR(struct LookupMapBase) PTR_LookupMapBase;

struct DynamicMetadata
{
    uint32_t Size;
    BYTE Data[0];
    friend struct ::cdac_data<DynamicMetadata>;
};

template<>
struct cdac_data<DynamicMetadata>
{
    static constexpr size_t Size = offsetof(DynamicMetadata, Size);
    static constexpr size_t Data = offsetof(DynamicMetadata, Data);
};

struct LookupMapBase
{
    DPTR(LookupMapBase) pNext;

    ArrayDPTR(TADDR)    pTable;

    // Number of elements in this node (only RIDs less than this value can be present in this node)
    DWORD               dwCount;

    // Set of flags that the map supports writing on top of the data value
    TADDR               supportedFlags;

#ifdef DACCESS_COMPILE
    void EnumMemoryRegions(CLRDataEnumMemoryFlags flags,
                           bool enumThis);
    void ListEnumMemoryRegions(CLRDataEnumMemoryFlags flags);
#endif // DACCESS_COMPILE

    PTR_TADDR GetIndexPtr(DWORD index)
    {
        LIMITED_METHOD_DAC_CONTRACT;
        _ASSERTE(index < dwCount);
        return dac_cast<PTR_TADDR>(pTable) + index;
    }

    PTR_TADDR GetElementPtr(DWORD rid);
    PTR_TADDR GrowMap(ModuleBase * pModule, DWORD rid);

    // Get number of RIDs that this table can store
    DWORD GetSize();

#ifdef _DEBUG
    void DebugGetRidMapOccupancy(DWORD *pdwOccupied, DWORD *pdwSize);
#endif
};

#define NO_MAP_FLAGS ((TADDR)0)

template <typename TYPE>
struct LookupMap : LookupMapBase
{
    static TYPE GetValueAt(PTR_TADDR pValue, TADDR* pFlags, TADDR supportedFlags);

#ifndef DACCESS_COMPILE
    static void SetValueAt(PTR_TADDR pValue, TYPE value, TADDR flags);
#endif // DACCESS_COMPILE

    TYPE GetElement(DWORD rid, TADDR* pFlags);
    void SetElement(DWORD rid, TYPE value, TADDR flags);
    BOOL TrySetElement(DWORD rid, TYPE value, TADDR flags);
    void AddElement(ModuleBase * pModule, DWORD rid, TYPE value, TADDR flags);
    void EnsureElementCanBeStored(Module * pModule, DWORD rid);
    DWORD Find(TYPE value, TADDR* flags);


public:

    //
    // Retrieve the value associated with a rid
    //
    TYPE GetElement(DWORD rid)
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;

        return GetElement(rid, NULL);
    }

    TYPE GetElementAndFlags(DWORD rid, TADDR* pFlags)
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;

        _ASSERTE(pFlags != NULL);

        return GetElement(rid, pFlags);
    }

    //
    // Stores an association in a map that has been previously grown to
    // the required size. Will never throw or fail.
    //
    void SetElement(DWORD rid, TYPE value)
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;

        SetElement(rid, value, 0);
    }

    void SetElementWithFlags(DWORD rid, TYPE value, TADDR flags)
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;

        // Validate flags: that they are in the predefined range and that the range does not collide with value
        _ASSERTE((flags & supportedFlags) == flags);
        _ASSERTE((dac_cast<TADDR>(value) & supportedFlags) == 0);

        SetElement(rid, value, flags);
    }

#ifndef DACCESS_COMPILE
    void AddFlag(DWORD rid, TADDR flag)
    {
        WRAPPER_NO_CONTRACT;

        _ASSERTE((flag & supportedFlags) == flag);

        PTR_TADDR pElement = GetElementPtr(rid);
        _ASSERTE(pElement);

        if (!pElement)
        {
            return;
        }

        TADDR existingFlags;
        TYPE existingValue = GetValueAt(pElement, &existingFlags, supportedFlags);
        SetValueAt(pElement, existingValue, existingFlags | flag);
    }
#endif // DACCESS_COMPILE

    //
    // Try to store an association in a map. Will never throw or fail.
    //
    BOOL TrySetElement(DWORD rid, TYPE value)
    {
        WRAPPER_NO_CONTRACT;

        return TrySetElement(rid, value, 0);
    }

    BOOL TrySetElementWithFlags(DWORD rid, TYPE value, TADDR flags)
    {
        WRAPPER_NO_CONTRACT;

        // Validate flags: that they are in the predefined range and that the range does not collide with value
        _ASSERTE((flags & supportedFlags) == flags);
        _ASSERTE((dac_cast<TADDR>(value) & supportedFlags) == 0);

        return TrySetElement(rid, value, flags);
    }

    //
    // Stores an association in a map. Grows the map as necessary.
    //
    void AddElement(ModuleBase * pModule, DWORD rid, TYPE value)
    {
        WRAPPER_NO_CONTRACT;

        AddElement(pModule, rid, value, 0);
    }

    void AddElementWithFlags(ModuleBase * pModule, DWORD rid, TYPE value, TADDR flags)
    {
        WRAPPER_NO_CONTRACT;

        // Validate flags: that they are in the predefined range and that the range does not collide with value
        _ASSERTE((flags & supportedFlags) == flags);
        _ASSERTE((dac_cast<TADDR>(value) & supportedFlags) == 0);

        AddElement(pModule, rid, value, flags);
    }

    //
    // Find the given value in the table and return its RID
    //
    DWORD Find(TYPE value)
    {
        WRAPPER_NO_CONTRACT;

        return Find(value, NULL);
    }

    DWORD FindWithFlags(TYPE value, TADDR flags)
    {
        WRAPPER_NO_CONTRACT;

        // Validate flags: that they are in the predefined range and that the range does not collide with value
        _ASSERTE((flags & supportedFlags) == flags);
        _ASSERTE((dac_cast<TADDR>(value) & supportedFlags) == 0);

        return Find(value, &flags);
    }

    class Iterator
    {
    public:
        Iterator(LookupMap* map);

        BOOL Next();

        TYPE GetElement()
        {
            WRAPPER_NO_CONTRACT;
            SUPPORTS_DAC;

            return GetElement(NULL);
        }

        TYPE GetElementAndFlags(TADDR* pFlags)
        {
            WRAPPER_NO_CONTRACT;
            SUPPORTS_DAC;

            return GetElement(pFlags);
        }

    private:
        TYPE GetElement(TADDR* pFlags);

        LookupMap* m_map;
        DWORD m_index;
    };
};

// Place holder types for RID maps that store cross-module references

class TypeRef { };
typedef DPTR(class TypeRef) PTR_TypeRef;

class MemberRef { };
typedef DPTR(class MemberRef) PTR_MemberRef;


// flag used to mark member ref pointers to field descriptors in the member ref cache
#define IS_FIELD_MEMBER_REF ((TADDR)0x00000002)


//
// VASigCookies are allocated to encapsulate a varargs call signature.
// A reference to the cookie is embedded in the code stream.  Cookies
// are shared amongst call sites with identical signatures in the same
// module
//

typedef DPTR(struct VASigCookie) PTR_VASigCookie;
typedef DPTR(PTR_VASigCookie) PTR_PTR_VASigCookie;
struct VASigCookie
{
    // The JIT wants knows that the size of the arguments comes first
    // so please keep this field first
    unsigned        sizeOfArgs;             // size of argument list
    Volatile<PCODE> pPInvokeILStub;         // will be use if target is PInvoke (tag == 0)
    PTR_Module      pModule;
    PTR_Module      pLoaderModule;
    Signature       signature;
    Instantiation   classInst;
    Instantiation   methodInst;
};

//
// VASigCookies are allocated in VASigCookieBlocks to amortize
// allocation cost and allow proper bookkeeping.
//

struct VASigCookieBlock
{
    enum {
#ifdef _DEBUG
        kVASigCookieBlockSize = 2
#else // !_DEBUG
        kVASigCookieBlockSize = 20
#endif // !_DEBUG
    };

    VASigCookieBlock    *m_Next;
    UINT                 m_numcookies;
    VASigCookie          m_cookies[kVASigCookieBlockSize];
};


// Hashtable of absolute addresses of IL blobs for dynamics, keyed by token

 struct  DynamicILBlobEntry
{
    mdToken     m_methodToken;
    TADDR       m_il;
};

class DynamicILBlobTraits : public NoRemoveSHashTraits<DefaultSHashTraits<DynamicILBlobEntry> >
{
public:
    typedef mdToken key_t;

    static key_t GetKey(element_t e)
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;
        return e.m_methodToken;
    }
    static BOOL Equals(key_t k1, key_t k2)
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;
        return k1 == k2;
    }
    static count_t Hash(key_t k)
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;
        return (count_t)(size_t)k;
    }
    static const element_t Null()
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;
        DynamicILBlobEntry e;
        e.m_il = TADDR(0);
        e.m_methodToken = 0;
        return e;
    }
    static bool IsNull(const element_t &e)
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;
        return e.m_methodToken == 0;
    }
};

typedef SHash<DynamicILBlobTraits> DynamicILBlobTable;
typedef DPTR(DynamicILBlobTable) PTR_DynamicILBlobTable;

#ifdef FEATURE_READYTORUN
typedef DPTR(class ReadyToRunInfo)      PTR_ReadyToRunInfo;
#endif

// A ModuleBase represents the ability to reference code via tokens
// This abstraction exists to allow the R2R manifest metadata to have
// tokens which can be resolved at runtime.
class ModuleBase
{
#ifdef DACCESS_COMPILE
    friend class ClrDataAccess;
    friend class NativeImageDumper;
#endif

    friend class DataImage;

    VPTR_BASE_VTABLE_CLASS(ModuleBase)

protected:
    // Linear mapping from TypeRef token to TypeHandle *
    LookupMap<PTR_TypeRef>          m_TypeRefToMethodTableMap;
    // Mapping of AssemblyRef token to Module *
    LookupMap<PTR_Module>           m_ManifestModuleReferencesMap;

    // mapping from MemberRef token to MethodDesc*, FieldDesc*
    LookupMap<TADDR>                m_MemberRefMap;

    // For protecting additions to the heap
    CrstExplicitInit        m_LookupTableCrst;

    PTR_LoaderAllocator     m_loaderAllocator;

    // The vtable needs to match between DAC and non-DAC, but we don't want any use of IsSigInIL in the DAC
    virtual BOOL IsSigInILImpl(PCCOR_SIGNATURE signature) { return FALSE; } // ModuleBase doesn't have a PE image to examine
    // The vtable needs to match between DAC and non-DAC, but we don't want any use of LoadAssembly in the DAC
    virtual Assembly * LoadAssemblyImpl(mdAssemblyRef kAssemblyRef) = 0;

    // The vtable needs to match between DAC and non-DAC, but we don't want any use of ThrowTypeLoadException in the DAC
    virtual void DECLSPEC_NORETURN ThrowTypeLoadExceptionImpl(IMDInternalImport *pInternalImport,
                                                  mdToken token,
                                                  UINT resIDWhy)
#ifndef DACCESS_COMPILE
                                                   = 0;
#else
                                                ;
#endif

public:
    ModuleBase() = default;

    virtual LPCWSTR GetPathForErrorMessages();

    CrstBase *GetLookupTableCrst()
    {
        LIMITED_METHOD_CONTRACT;
        return &m_LookupTableCrst;
    }

    PTR_LoaderAllocator GetLoaderAllocator()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_loaderAllocator;
    }

    FORCEINLINE TADDR LookupMemberRef(mdMemberRef token, BOOL *pfIsMethod)
    {
        WRAPPER_NO_CONTRACT;

        _ASSERTE(TypeFromToken(token) == mdtMemberRef);

        TADDR flags;
        TADDR pResult = m_MemberRefMap.GetElementAndFlags(RidFromToken(token), &flags);
        *pfIsMethod = !(flags & IS_FIELD_MEMBER_REF);

        return pResult;
    }
#ifndef DACCESS_COMPILE
    void StoreMemberRef(mdMemberRef token, FieldDesc *value)
    {
        WRAPPER_NO_CONTRACT;

        _ASSERTE(TypeFromToken(token) == mdtMemberRef);
        m_MemberRefMap.AddElementWithFlags(this, RidFromToken(token), (TADDR)value, IS_FIELD_MEMBER_REF);
    }
    void StoreMemberRef(mdMemberRef token, MethodDesc *value)
    {
        WRAPPER_NO_CONTRACT;

        _ASSERTE(TypeFromToken(token) == mdtMemberRef);
        m_MemberRefMap.AddElementWithFlags(this, RidFromToken(token), (TADDR)value, 0);
    }
#endif // !DACCESS_COMPILE

    TypeHandle LookupTypeRef(mdTypeRef token);
    virtual IMDInternalImport *GetMDImport() const = 0;
    virtual bool IsFullModule() const { return false; }


    void StoreTypeRef(mdTypeRef token, TypeHandle value)
    {
        WRAPPER_NO_CONTRACT;

        _ASSERTE(TypeFromToken(token) == mdtTypeRef);

        // The TypeRef cache is strictly a lookaside cache. If we get an OOM trying to grow the table,
        // we cannot abort the load. (This will cause fatal errors during gc promotion.)
        m_TypeRefToMethodTableMap.TrySetElement(RidFromToken(token),
            dac_cast<PTR_TypeRef>(value.AsTAddr()));
    }
    virtual PTR_Module LookupModule(mdToken kFile) { return NULL; }; //wrapper over GetModuleIfLoaded, takes modulerefs as well
    virtual Module *GetModuleIfLoaded(mdFile kFile) { return NULL; };
#ifndef DACCESS_COMPILE
    virtual Module *LoadModule(mdFile kFile);
#endif
    DWORD GetAssemblyRefFlags(mdAssemblyRef tkAssemblyRef);

    Assembly *LookupAssemblyRef(mdAssemblyRef token);
    // Module/Assembly traversal
    virtual Assembly * GetAssemblyIfLoaded(
            mdAssemblyRef       kAssemblyRef,
            IMDInternalImport * pMDImportOverride = NULL,
            AssemblyBinder      *pBinderForLoadedAssembly = NULL
            )
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return NULL;
    };

    const ReadyToRun_EnclosingTypeMap *m_pEnclosingTypeMap = &ReadyToRun_EnclosingTypeMap::EmptyInstance;

#ifndef DACCESS_COMPILE
    // The vtable needs to match between DAC and non-DAC, but we don't want any use of ThrowTypeLoadException in the DAC
    void DECLSPEC_NORETURN ThrowTypeLoadException(IMDInternalImport *pInternalImport,
                                                  mdToken token,
                                                  UINT resIDWhy)
    {
        ThrowTypeLoadExceptionImpl(pInternalImport, token, resIDWhy);
    }

    // The vtable needs to match between DAC and non-DAC, but we don't want any use of IsSigInIL in the DAC
    BOOL IsSigInIL(PCCOR_SIGNATURE signature) { return IsSigInILImpl(signature); }
    Assembly * LoadAssembly(mdAssemblyRef kAssemblyRef)
    {
        WRAPPER_NO_CONTRACT;
        return LoadAssemblyImpl(kAssemblyRef);
    }

    // Resolving
    STRINGREF* ResolveStringRef(DWORD Token, void** ppPinnedString = nullptr);
private:
    // string helper
    void InitializeStringData(DWORD token, EEStringData *pstrData, CQuickBytes *pqb);
#endif

};

// A code:Module represents a DLL or EXE file loaded from the disk. A module live in a code:Assembly
//
// Some important fields are
//    * code:Module.m_pPEAssembly - this points at a code:PEAssembly that understands the layout of a PE assembly. The most
//        important part is getting at the code:Module (see file:..\inc\corhdr.h#ManagedHeader) from there
//        you can get at the Meta-data and IL)
//    * code:Module.m_pAvailableClasses - this is a table that lets you look up the types (the code:EEClass)
//        for all the types in the module
//
// See file:..\inc\corhdr.h#ManagedHeader for more on the layout of managed executable files.
class Module : public ModuleBase
{
#ifdef DACCESS_COMPILE
    friend class ClrDataAccess;
    friend class NativeImageDumper;
#endif

    friend class DataImage;

    VPTR_VTABLE_CLASS(Module, ModuleBase)

private:
    PTR_CUTF8               m_pSimpleName;  // Cached simple name for better performance and easier diagnostics
    const WCHAR*            m_path;         // Cached path for easier diagnostics
    const WCHAR*            m_fileName;     // Cached file name for easier diagnostics

    PTR_PEAssembly          m_pPEAssembly;
    PTR_VOID                m_baseAddress; // Cached base address for easier diagnostics

    enum {
        // These are the values set in m_dwTransientFlags.

        MODULE_IS_TENURED           = 0x00000001,   // Set once we know for sure the Module will not be freed until the appdomain itself exits
        // unused                   = 0x00000002,
        CLASSES_FREED               = 0x00000004,
        IS_EDIT_AND_CONTINUE        = 0x00000008,   // is EnC Enabled for this module

        IS_PROFILER_NOTIFIED        = 0x00000010,
        IS_ETW_NOTIFIED             = 0x00000020,

        IS_REFLECTION_EMIT          = 0x00000040,
        PROF_DISABLE_OPTIMIZATIONS  = 0x00000080,   // indicates if Profiler disabled JIT optimization event mask was set when loaded
        PROF_DISABLE_INLINING       = 0x00000100,   // indicates if Profiler disabled JIT Inlining event mask was set when loaded

        //
        // Note: The values below must match the ones defined in
        // cordbpriv.h for DebuggerAssemblyControlFlags when shifted
        // right DEBUGGER_INFO_SHIFT bits.
        //
        DEBUGGER_USER_OVERRIDE_PRIV = 0x00000400,
        DEBUGGER_ALLOW_JIT_OPTS_PRIV= 0x00000800,
        DEBUGGER_TRACK_JIT_INFO_PRIV= 0x00001000,
        DEBUGGER_ENC_ENABLED_PRIV   = 0x00002000,   // this is what was attempted to be set.  IS_EDIT_AND_CONTINUE is actual result.
        DEBUGGER_PDBS_COPIED        = 0x00004000,
        DEBUGGER_IGNORE_PDBS        = 0x00008000,
        DEBUGGER_INFO_MASK_PRIV     = 0x0000Fc00,
        DEBUGGER_INFO_SHIFT_PRIV    = 10,

        // Used to indicate that this module has had it's IJW fixups properly installed.
        IS_IJW_FIXED_UP             = 0x00080000,
        IS_BEING_UNLOADED           = 0x00100000,
    };

    static_assert_no_msg(DEBUGGER_USER_OVERRIDE_PRIV >> DEBUGGER_INFO_SHIFT_PRIV == DebuggerAssemblyControlFlags::DACF_USER_OVERRIDE);
    static_assert_no_msg(DEBUGGER_ALLOW_JIT_OPTS_PRIV >> DEBUGGER_INFO_SHIFT_PRIV == DebuggerAssemblyControlFlags::DACF_ALLOW_JIT_OPTS);
    static_assert_no_msg(DEBUGGER_TRACK_JIT_INFO_PRIV >> DEBUGGER_INFO_SHIFT_PRIV == DebuggerAssemblyControlFlags::DACF_OBSOLETE_TRACK_JIT_INFO);
    static_assert_no_msg(DEBUGGER_ENC_ENABLED_PRIV >> DEBUGGER_INFO_SHIFT_PRIV == DebuggerAssemblyControlFlags::DACF_ENC_ENABLED);
    static_assert_no_msg(DEBUGGER_PDBS_COPIED >> DEBUGGER_INFO_SHIFT_PRIV == DebuggerAssemblyControlFlags::DACF_PDBS_COPIED);
    static_assert_no_msg(DEBUGGER_IGNORE_PDBS >> DEBUGGER_INFO_SHIFT_PRIV == DebuggerAssemblyControlFlags::DACF_IGNORE_PDBS);

    enum {
        // These are the values set in m_dwPersistedFlags.
        // unused                   = 0x00000001,
        COMPUTED_GLOBAL_CLASS       = 0x00000002,

        // unused                   = 0x00000004,
        // unused                   = 0x00000008,

        // This flag applies to assembly, but it is stored so it can be cached in ngen image
        COMPUTED_WRAP_EXCEPTIONS    = 0x00000010,
        WRAP_EXCEPTIONS             = 0x00000020,

        // unused                   = 0x00000040,
        // unused                   = 0x00000080,

        //If attribute value has been cached before
        DEFAULT_DLL_IMPORT_SEARCH_PATHS_IS_CACHED   = 0x00000400,

        //If module has default dll import search paths attribute
        DEFAULT_DLL_IMPORT_SEARCH_PATHS_STATUS      = 0x00000800,


        //If setting has been cached
        RUNTIME_MARSHALLING_ENABLED_IS_CACHED = 0x00008000,
        //If runtime marshalling is enabled for this assembly
        RUNTIME_MARSHALLING_ENABLED = 0x00010000,
    };

    Volatile<DWORD>          m_dwTransientFlags;
    Volatile<DWORD>          m_dwPersistedFlags;

    // Linked list of VASig cookie blocks: protected by m_pStubListCrst
    VASigCookieBlock        *m_pVASigCookieBlock;

    PTR_Assembly            m_pAssembly;

    CrstExplicitInit        m_Crst;

    // Debugging symbols reader interface. This will only be
    // initialized if needed, either by the debugging subsystem or for
    // an exception.
    ISymUnmanagedReader *   m_pISymUnmanagedReader;

    // The reader lock is used to serialize all creation of symbol readers.
    // It does NOT seralize all access to the readers since we freely give
    // out references to the reader outside this class.  Instead, once a
    // reader object is created, it is entirely read-only and so thread-safe.
    CrstExplicitInit        m_ISymUnmanagedReaderCrst;

    // Storage for the in-memory symbol stream if any
    // Debugger may retrieve this from out-of-process.
    PTR_CGrowableStream     m_pIStreamSym;

    #define TYPE_DEF_MAP_ALL_FLAGS                    NO_MAP_FLAGS

    #define TYPE_REF_MAP_ALL_FLAGS                    NO_MAP_FLAGS
        // For type ref map, 0x1 cannot be used as a flag: reserved for FIXUP_POINTER_INDIRECTION bit
        // For type ref map, 0x2 cannot be used as a flag: reserved for TypeHandle to signify TypeDesc

    #define METHOD_DEF_MAP_ALL_FLAGS                  NO_MAP_FLAGS

    #define FIELD_DEF_MAP_ALL_FLAGS                   NO_MAP_FLAGS

    #define MEMBER_REF_MAP_ALL_FLAGS                  ((TADDR)0x00000003)
	// For member ref hash table, 0x1 is reserved for IsHot bit
        #define IS_FIELD_MEMBER_REF                   ((TADDR)0x00000002)      // denotes that target is a FieldDesc

    #define GENERIC_PARAM_MAP_ALL_FLAGS               NO_MAP_FLAGS

    #define MANIFEST_MODULE_MAP_ALL_FLAGS             NO_MAP_FLAGS
        // For manifest module map, 0x1 cannot be used as a flag: reserved for FIXUP_POINTER_INDIRECTION bit

    // Linear mapping from TypeDef token to MethodTable *
    // For generic types, IsGenericTypeDefinition() is true i.e. instantiation at formals
    LookupMap<PTR_MethodTable>      m_TypeDefToMethodTableMap;

    // Linear mapping from MethodDef token to MethodDesc *
    // For generic methods, IsGenericTypeDefinition() is true i.e. instantiation at formals
    LookupMap<PTR_MethodDesc>       m_MethodDefToDescMap;

    // Linear mapping from MethodDef token to ILCodeVersioningState *
    // This is used for Code Versioning logic
    LookupMap<PTR_ILCodeVersioningState>    m_ILCodeVersioningStateMap;

    // Linear mapping from FieldDef token to FieldDesc*
    LookupMap<PTR_FieldDesc>        m_FieldDefToDescMap;

    // Linear mapping from GenericParam token to TypeVarTypeDesc*
    LookupMap<PTR_TypeVarTypeDesc>  m_GenericParamToDescMap;

    // IL stub cache with fabricated MethodTable parented by this module.
    ILStubCache                *m_pILStubCache;

    ULONG m_DefaultDllImportSearchPathsAttributeValue;
public:
    LookupMap<PTR_MethodTable>::Iterator EnumerateTypeDefs()
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;

        return LookupMap<PTR_MethodTable>::Iterator(&m_TypeDefToMethodTableMap);
    }

    // Hash of available types by name
    PTR_EEClassHashTable    m_pAvailableClasses;

    // Hashtable of generic type instances
    PTR_EETypeHashTable     m_pAvailableParamTypes;

    // For protecting additions to m_pInstMethodHashTable
    CrstExplicitInit        m_InstMethodHashTableCrst;

    // Hashtable of instantiated methods and per-instantiation static methods
    PTR_InstMethodHashTable m_pInstMethodHashTable;

    // This is used by the Debugger. We need to store a dword
    // for a count of JMC functions. This is a count, not a pointer.
    // We'll pass the address of this field
    // off to the jit, which will include it in probes injected for
    // debuggable code.
    // This means we need the dword at the time a function is jitted.
    // The Debugger has its own module structure, but those aren't created
    // if a debugger isn't attached.
    // We put it here instead of in the debugger's module because:
    // 1) we need a module structure that's around even when the debugger
    // isn't attached... so we use the EE's module.
    // 2) Needs to be here for ngen
    DWORD                   m_dwDebuggerJMCProbeCount;

    bool IsFullModule() const final { return true; }
    // We can skip the JMC probes if we know that a module has no JMC stuff
    // inside. So keep a strict count of all functions inside us.
    bool HasAnyJMCFunctions();
    void IncJMCFuncCount();
    void DecJMCFuncCount();

    // Get and set the default JMC status of this module.
    bool GetJMCStatus();
    void SetJMCStatus(bool fStatus);

    // If this is a dynamic module, eagerly serialize the metadata so that it is available for DAC.
    // This is a nop for non-dynamic modules.
    void UpdateDynamicMetadataIfNeeded();

#ifdef _DEBUG
    //
    // We call these methods to seal/unseal the
    // lists: m_pAvailableClasses and m_pAvailableParamTypes
    //
    // When they are sealed ClassLoader::PublishType cannot
    // add new generic types or methods
    //
    void SealGenericTypesAndMethods();
    void UnsealGenericTypesAndMethods();
#endif

private:
    // Set the given bit on m_dwTransientFlags. Return true if we won the race to set the bit.
    BOOL SetTransientFlagInterlocked(DWORD dwFlag);

    // Cannoically-cased hashtable of the available class names for
    // case insensitive lookup.  Contains pointers into
    // m_pAvailableClasses.
    PTR_EEClassHashTable    m_pAvailableClassesCaseIns;

public:
    BOOL IsCollectible();

#ifdef FEATURE_READYTORUN
private:
    PTR_ReadyToRunInfo      m_pReadyToRunInfo;
    PTR_NativeImage         m_pNativeImage;
#endif

#if PROFILING_SUPPORTED_DATA
private:
    DWORD                   m_dwTypeCount;
    DWORD                   m_dwExportedTypeCount;
    DWORD                   m_dwCustomAttributeCount;
#endif // PROFILING_SUPPORTED_DATA

protected:
    void DoInit(AllocMemTracker *pamTracker, LPCWSTR szName);

protected:
#ifndef DACCESS_COMPILE
    virtual void Initialize(AllocMemTracker *pamTracker, LPCWSTR szName = NULL);
#endif

    void AllocateMaps();

#ifdef _DEBUG
    void DebugLogRidMapOccupancy();
#endif // _DEBUG

 public:
    static Module *Create(Assembly *pAssembly, PEAssembly *pPEAssembly, AllocMemTracker *pamTracker);

 protected:
    Module(Assembly *pAssembly, PEAssembly *file);


 public:
#ifndef DACCESS_COMPILE
    virtual void Destruct();
#endif

    PTR_PEAssembly GetPEAssembly() const { LIMITED_METHOD_DAC_CONTRACT; return m_pPEAssembly; }

    void ApplyMetaData();

    void FixupVTables();

    void FreeClassTables();

#ifdef DACCESS_COMPILE
    virtual void EnumMemoryRegions(CLRDataEnumMemoryFlags flags,
                                   bool enumThis);
#endif // DACCESS_COMPILE

    ReflectionModule *GetReflectionModule() const
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;

        _ASSERTE(IsReflectionEmit());
        return dac_cast<PTR_ReflectionModule>(this);
    }

    PTR_Assembly GetAssembly() const;

    MethodTable *GetGlobalMethodTable();
    bool         NeedsGlobalMethodTable();

    DomainAssembly *GetDomainAssembly();

    void SetDomainAssembly(DomainAssembly *pDomainAssembly);

    OBJECTREF GetExposedObject();
    OBJECTREF GetExposedObjectIfExists();

    ClassLoader *GetClassLoader();
#ifdef FEATURE_CODE_VERSIONING
    CodeVersionManager * GetCodeVersionManager();
#endif

    BOOL IsReflectionEmit() const { WRAPPER_NO_CONTRACT; SUPPORTS_DAC; return (m_dwTransientFlags & IS_REFLECTION_EMIT) != 0; }
    BOOL IsSystem() { WRAPPER_NO_CONTRACT; SUPPORTS_DAC; return m_pPEAssembly->IsSystem(); }
    // Returns true iff the debugger can see this module.
    BOOL IsVisibleToDebugger();

    virtual BOOL IsEditAndContinueCapable() const { return FALSE; }

    BOOL IsEditAndContinueEnabled()
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;
        _ASSERTE((m_dwTransientFlags & IS_EDIT_AND_CONTINUE) == 0 || IsEditAndContinueCapable());
        return (m_dwTransientFlags & IS_EDIT_AND_CONTINUE) != 0;
    }

    BOOL IsInliningDisabledByProfiler() const
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;

        return (m_dwTransientFlags & PROF_DISABLE_INLINING) != 0;
    }

    BOOL AreJITOptimizationsDisabled() const
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;

#ifdef DEBUGGING_SUPPORTED
        // check if debugger has disallowed JIT optimizations
        auto dwDebuggerBits = GetDebuggerInfoBits();
        if (!CORDebuggerAllowJITOpts(dwDebuggerBits))
        {
            return TRUE;
        }
#endif // DEBUGGING_SUPPORTED

#if defined(PROFILING_SUPPORTED) || defined(PROFILING_SUPPORTED_DATA)
        // check if profiler had disabled JIT optimizations when module was loaded
        if (m_dwTransientFlags & PROF_DISABLE_OPTIMIZATIONS)
        {
            return TRUE;
        }
#endif // defined(PROFILING_SUPPORTED) || defined(PROFILING_SUPPORTED_DATA)

        return FALSE;
    }

#ifdef FEATURE_METADATA_UPDATER
    // Holds a table of EnCEEClassData object for classes in this module that have been modified
    CUnorderedArray<EnCEEClassData*, 5> m_ClassList;
#endif // FEATURE_METADATA_UPDATER

private:
    void EnableEditAndContinue()
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;
        _ASSERTE(IsEditAndContinueCapable());
        LOG((LF_ENC, LL_INFO100, "M:EnableEditAndContinue: this:%p, %s\n", this, GetDebugName()));
        m_dwTransientFlags |= IS_EDIT_AND_CONTINUE;
    }

public:
    BOOL IsTenured()
    {
        LIMITED_METHOD_CONTRACT;
        return m_dwTransientFlags & MODULE_IS_TENURED;
    }

#ifndef DACCESS_COMPILE
    VOID SetIsTenured()
    {
        LIMITED_METHOD_CONTRACT;
        InterlockedOr((LONG*)&m_dwTransientFlags, MODULE_IS_TENURED);
    }
#endif // !DACCESS_COMPILE

#ifndef DACCESS_COMPILE
    VOID EnsureActive();
#endif

    CHECK CheckActivated();

    HRESULT GetCustomAttribute(mdToken parentToken,
                               WellKnownAttribute attribute,
                               const void  **ppData,
                               ULONG *pcbData)
    {
        if (IsReadyToRun())
        {
            if (!GetReadyToRunInfo()->MayHaveCustomAttribute(attribute, parentToken))
                return S_FALSE;
        }

        return GetMDImport()->GetCustomAttributeByName(parentToken, GetWellKnownAttributeName(attribute), ppData, pcbData);
    }

    IMDInternalImport *GetMDImport() const final
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;

#ifdef DACCESS_COMPILE
        if (IsReflectionEmit())
        {
            return DacGetMDImport(GetReflectionModule(), true);
        }
#endif // DACCESS_COMPILE
        return m_pPEAssembly->GetMDImport();
    }

#ifndef DACCESS_COMPILE
    IMetaDataEmit *GetEmitter()
    {
        WRAPPER_NO_CONTRACT;

        return m_pPEAssembly->GetEmitter();
    }

    IMetaDataImport2 *GetRWImporter()
    {
        WRAPPER_NO_CONTRACT;

        return m_pPEAssembly->GetRWImporter();
    }

    HRESULT GetReadablePublicMetaDataInterface(DWORD dwOpenFlags, REFIID riid, LPVOID * ppvInterface);
#endif // !DACCESS_COMPILE

#if defined(FEATURE_READYTORUN)
    BOOL IsInSameVersionBubble(Module *target);
#endif // FEATURE_READYTORUN


    LPCWSTR GetPathForErrorMessages() final;


#ifdef FEATURE_ISYM_READER
    // Gets an up-to-date symbol reader for this module, lazily creating it if necessary
    // The caller must call Release
    ISymUnmanagedReader *GetISymUnmanagedReader(void);
    ISymUnmanagedReader *GetISymUnmanagedReaderNoThrow(void);
#endif // FEATURE_ISYM_READER

    // Save a copy of the provided debugging symbols in the InMemorySymbolStream.
    // These are used by code:Module::GetInMemorySymbolStream and code:Module.GetISymUnmanagedReader
    // This can only be called during module creation, before anyone may have tried to create a reader.
    void SetSymbolBytes(LPCBYTE pSyms, DWORD cbSyms);

    // Does the current configuration permit reading of symbols for this module?
    // Note that this may require calling into managed code (to resolve security policy).
    BOOL IsSymbolReadingEnabled(void);

    // Get the in-memory symbol stream for this module, if any.
    // If none, this will return null.  This is used by modules loaded in-memory (eg. from a byte-array)
    // and by dynamic modules.
    PTR_CGrowableStream GetInMemorySymbolStream()
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;

        return m_pIStreamSym;
    }

#ifndef DACCESS_COMPILE
    // Set the in-memory stream for debug symbols
    // This must only be called when there is no existing stream.
    // This takes an AddRef on the supplied stream.
    void SetInMemorySymbolStream(CGrowableStream *pStream)
    {
        LIMITED_METHOD_CONTRACT;

        // Must have provided valid stream data
        CONSISTENCY_CHECK(pStream != NULL);

        // we expect set to only be called once
        CONSISTENCY_CHECK(m_pIStreamSym == NULL);

        m_pIStreamSym = pStream;
        m_pIStreamSym->AddRef();
    }

    // Release and clear the in-memory symbol stream if any
    void ClearInMemorySymbolStream()
    {
        LIMITED_METHOD_CONTRACT;
        if( m_pIStreamSym != NULL )
        {
            m_pIStreamSym->Release();
            m_pIStreamSym = NULL;
        }
    }

    // Release the symbol reader if any
    // Caller is responsible for aquiring the reader lock if this could occur
    // concurrently with other uses of the reader (i.e. not shutdown/unload time)
    void ReleaseISymUnmanagedReader(void);

#endif // DACCESS_COMPILE

    // IL stub cache
    ILStubCache* GetILStubCache();

    // Classes
    void AddClass(mdTypeDef classdef);
    void BuildClassForModule();
    PTR_EEClassHashTable GetAvailableClassHash()
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;

        return m_pAvailableClasses;
    }
#ifndef DACCESS_COMPILE
    void SetAvailableClassHash(EEClassHashTable *pAvailableClasses)
    {

        m_pAvailableClasses = pAvailableClasses;
    }
#endif // !DACCESS_COMPILE
    PTR_EEClassHashTable GetAvailableClassCaseInsHash()
    {
        LIMITED_METHOD_CONTRACT;

        return m_pAvailableClassesCaseIns;
    }
#ifndef DACCESS_COMPILE
    void SetAvailableClassCaseInsHash(EEClassHashTable *pAvailableClassesCaseIns)
    {

        m_pAvailableClassesCaseIns = pAvailableClassesCaseIns;
    }
#endif // !DACCESS_COMPILE

    // Constructed types tables
    EETypeHashTable *GetAvailableParamTypes()
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;

        return m_pAvailableParamTypes;
    }

    InstMethodHashTable *GetInstMethodHashTable()
    {
        LIMITED_METHOD_CONTRACT;

        return m_pInstMethodHashTable;
    }

    // Creates a new Method table for an array.  Used to make type handles
    // Note that if kind == SZARRAY or ARRAY, we get passed the GENERIC_ARRAY
    // needed to create the array.  That way we dont need to load classes during
    // the class load, which avoids the need for a 'being loaded' list
    MethodTable* CreateArrayMethodTable(TypeHandle elemType, CorElementType kind, unsigned rank, class AllocMemTracker *pamTracker);

    // Module/Assembly traversal
    Assembly * GetAssemblyIfLoaded(
            mdAssemblyRef       kAssemblyRef,
            IMDInternalImport * pMDImportOverride = NULL,
            AssemblyBinder      *pBinderForLoadedAssembly = NULL
            ) final;

protected:
#ifndef DACCESS_COMPILE
    void DECLSPEC_NORETURN ThrowTypeLoadExceptionImpl(IMDInternalImport *pInternalImport,
                                                  mdToken token,
                                                  UINT resIDWhy) final;
#endif

    Assembly * LoadAssemblyImpl(mdAssemblyRef kAssemblyRef) final;
public:
    PTR_Module LookupModule(mdToken kFile) final;
    Module *GetModuleIfLoaded(mdFile kFile) final;

    // RID maps
    TypeHandle LookupTypeDef(mdTypeDef token, ClassLoadLevel *pLoadLevel = NULL)
    {
        LIMITED_METHOD_DAC_CONTRACT;

        BAD_FORMAT_NOTHROW_ASSERT(TypeFromToken(token) == mdtTypeDef);

        TypeHandle th = TypeHandle(m_TypeDefToMethodTableMap.GetElement(RidFromToken(token)));

        if (pLoadLevel && !th.IsNull())
        {
            *pLoadLevel = th.GetLoadLevel();
        }

        return th;
    }

#ifndef DACCESS_COMPILE
    VOID EnsureTypeDefCanBeStored(mdTypeDef token)
    {
        WRAPPER_NO_CONTRACT; // THROWS/GC_NOTRIGGER/INJECT_FAULT()/MODE_ANY
        m_TypeDefToMethodTableMap.EnsureElementCanBeStored(this, RidFromToken(token));
    }

    void EnsuredStoreTypeDef(mdTypeDef token, TypeHandle value)
    {
        WRAPPER_NO_CONTRACT; // NOTHROW/GC_NOTRIGGER/FORBID_FAULT/MODE_ANY

        _ASSERTE(TypeFromToken(token) == mdtTypeDef);
        m_TypeDefToMethodTableMap.SetElement(RidFromToken(token), value.AsMethodTable());
    }

#endif // !DACCESS_COMPILE

#ifndef DACCESS_COMPILE
    //
    // Increase the size of the TypeRef-to-MethodTable LookupMap to make sure the specified token
    // can be stored.  Note that nothing is actually added to the LookupMap at this point.
    //
    // Arguments:
    //    token - the TypeRef metadata token we need to accommodate
    //

    void EnsureTypeRefCanBeStored(mdTypeRef token)
    {
        WRAPPER_NO_CONTRACT; // THROWS/GC_NOTRIGGER/INJECT_FAULT()/MODE_ANY

        _ASSERTE(TypeFromToken(token) == mdtTypeRef);
        m_TypeRefToMethodTableMap.EnsureElementCanBeStored(this, RidFromToken(token));
    }
#endif // !DACCESS_COMPILE

    PTR_MethodDesc LookupMethodDef(mdMethodDef token);

#ifndef DACCESS_COMPILE
    void EnsureMethodDefCanBeStored(mdMethodDef token)
    {
        WRAPPER_NO_CONTRACT; // THROWS/GC_NOTRIGGER/INJECT_FAULT()/MODE_ANY
        m_MethodDefToDescMap.EnsureElementCanBeStored(this, RidFromToken(token));
    }

    void EnsuredStoreMethodDef(mdMethodDef token, MethodDesc *value)
    {
        WRAPPER_NO_CONTRACT; // NOTHROW/GC_NOTRIGGER/FORBID_FAULT/MODE_ANY

        _ASSERTE(TypeFromToken(token) == mdtMethodDef);
        m_MethodDefToDescMap.SetElement(RidFromToken(token), value);
    }
#endif // !DACCESS_COMPILE

    PTR_ILCodeVersioningState LookupILCodeVersioningState(mdMethodDef token);

#ifndef DACCESS_COMPILE
    void EnsureILCodeVersioningStateCanBeStored(mdMethodDef token)
    {
        WRAPPER_NO_CONTRACT; // THROWS/GC_NOTRIGGER/INJECT_FAULT()/MODE_ANY
        _ASSERTE(CodeVersionManager::IsLockOwnedByCurrentThread());
        m_ILCodeVersioningStateMap.EnsureElementCanBeStored(this, RidFromToken(token));
    }

    void EnsuredStoreILCodeVersioningState(mdMethodDef token, PTR_ILCodeVersioningState value)
    {
        WRAPPER_NO_CONTRACT; // NOTHROW/GC_NOTRIGGER/FORBID_FAULT/MODE_ANY
        _ASSERTE(CodeVersionManager::IsLockOwnedByCurrentThread());
        _ASSERTE(TypeFromToken(token) == mdtMethodDef);
        m_ILCodeVersioningStateMap.SetElement(RidFromToken(token), value);
    }
#endif // !DACCESS_COMPILE

#ifndef DACCESS_COMPILE
    FieldDesc *LookupFieldDef(mdFieldDef token)
    {
        WRAPPER_NO_CONTRACT;

        _ASSERTE(TypeFromToken(token) == mdtFieldDef);
        return m_FieldDefToDescMap.GetElement(RidFromToken(token));
    }
#else // DACCESS_COMPILE
    // FieldDesc isn't defined at this point so PTR_FieldDesc can't work.
    FieldDesc *LookupFieldDef(mdFieldDef token);
#endif // DACCESS_COMPILE

#ifndef DACCESS_COMPILE
    void EnsureFieldDefCanBeStored(mdFieldDef token)
    {
        WRAPPER_NO_CONTRACT; // THROWS/GC_NOTRIGGER/INJECT_FAULT()/MODE_ANY
        m_FieldDefToDescMap.EnsureElementCanBeStored(this, RidFromToken(token));
    }

    void EnsuredStoreFieldDef(mdFieldDef token, FieldDesc *value)
    {
        WRAPPER_NO_CONTRACT; // NOTHROW/GC_NOTRIGGER/FORBID_FAULT/MODE_ANY

        _ASSERTE(TypeFromToken(token) == mdtFieldDef);
        m_FieldDefToDescMap.SetElement(RidFromToken(token), value);
    }
#endif // !DACCESS_COMPILE

    MethodDesc *LookupMemberRefAsMethod(mdMemberRef token);

    PTR_TypeVarTypeDesc LookupGenericParam(mdGenericParam token)
    {
        WRAPPER_NO_CONTRACT;

        _ASSERTE(TypeFromToken(token) == mdtGenericParam);
        return m_GenericParamToDescMap.GetElement(RidFromToken(token));
    }
#ifndef DACCESS_COMPILE
    void StoreGenericParamThrowing(mdGenericParam token, TypeVarTypeDesc *value)
    {
        WRAPPER_NO_CONTRACT;

        _ASSERTE(TypeFromToken(token) == mdtGenericParam);
        m_GenericParamToDescMap.AddElement(this, RidFromToken(token), value);
    }
#endif // !DACCESS_COMPILE

    PTR_Module LookupFile(mdFile token)
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;

        _ASSERTE(TypeFromToken(token) == mdtFile);

        // We don't support multi-module, so just check if we are looking for this module
        return token == mdFileNil ? dac_cast<PTR_Module>(this) : NULL;
    }

#ifndef DACCESS_COMPILE
    //
    // Increase the size of the AssemblyRef-to-Module LookupMap to make sure the specified token
    // can be stored.  Note that nothing is actually added to the LookupMap at this point.
    //
    // Arguments:
    //    token - the AssemblyRef metadata token we need to accommodate
    //

    void EnsureAssemblyRefCanBeStored(mdAssemblyRef token)
    {
        WRAPPER_NO_CONTRACT; // THROWS/GC_NOTRIGGER/INJECT_FAULT()/MODE_ANY

        _ASSERTE(TypeFromToken(token) == mdtAssemblyRef);
        m_ManifestModuleReferencesMap.EnsureElementCanBeStored(this, RidFromToken(token));
    }

    void ForceStoreAssemblyRef(mdAssemblyRef token, Assembly *value);
    void StoreAssemblyRef(mdAssemblyRef token, Assembly *value);

    mdAssemblyRef FindAssemblyRef(Assembly *targetAssembly);

#endif // !DACCESS_COMPILE

    DWORD GetAssemblyRefMax() {LIMITED_METHOD_CONTRACT;  return m_ManifestModuleReferencesMap.GetSize() - 1; }

    MethodDesc *FindMethodThrowing(mdToken pMethod);
    MethodDesc *FindMethod(mdToken pMethod);

#ifndef DACCESS_COMPILE
public:
    // light code gen. Keep the list of MethodTables needed for creating dynamic methods
    DynamicMethodTable* GetDynamicMethodTable();
#endif
private:
    // m_pDynamicMethodTable is used by the light code generation to allow method
    // generation on the fly. They are lazily created when/if a dynamic method is requested
    // for this specific module
    DynamicMethodTable*         m_pDynamicMethodTable;

public:

    // Debugger stuff
    BOOL NotifyDebuggerLoad(DomainAssembly * pDomainAssembly, int level, BOOL attaching);
    void NotifyDebuggerUnload();

    void SetDebuggerInfoBits(DebuggerAssemblyControlFlags newBits);

    DebuggerAssemblyControlFlags GetDebuggerInfoBits(void) const
    {
        LIMITED_METHOD_CONTRACT;
        SUPPORTS_DAC;

        return (DebuggerAssemblyControlFlags)((m_dwTransientFlags &
                                               DEBUGGER_INFO_MASK_PRIV) >>
                                              DEBUGGER_INFO_SHIFT_PRIV);
    }

    void UpdateNewlyAddedTypes();

#ifdef PROFILING_SUPPORTED
    BOOL IsProfilerNotified() {LIMITED_METHOD_CONTRACT;  return (m_dwTransientFlags & IS_PROFILER_NOTIFIED) != 0; }
    void NotifyProfilerLoadFinished(HRESULT hr);
#endif // PROFILING_SUPPORTED

    BOOL HasReadyToRunInlineTrackingMap();
    COUNT_T GetReadyToRunInliners(PTR_Module inlineeOwnerMod, mdMethodDef inlineeTkn, COUNT_T inlinersSize, MethodInModule inliners[], BOOL *incompleteData);
#if defined(PROFILING_SUPPORTED) && !defined(DACCESS_COMPILE)
    BOOL HasJitInlineTrackingMap();
    PTR_JITInlineTrackingMap GetJitInlineTrackingMap() { LIMITED_METHOD_CONTRACT; return m_pJitInlinerTrackingMap; }
    void AddInlining(MethodDesc *inliner, MethodDesc *inlinee);
#endif // defined(PROFILING_SUPPORTED) && !defined(DACCESS_COMPILE)

public:
    void NotifyEtwLoadFinished(HRESULT hr);

    // Enregisters a VASig.
    VASigCookie *GetVASigCookie(Signature vaSignature, const SigTypeContext* typeContext);
private:
    static VASigCookie *GetVASigCookieWorker(Module* pDefiningModule, Module* pLoaderModule, Signature vaSignature, const SigTypeContext* typeContext);

public:
#ifndef DACCESS_COMPILE
    BOOL Equals(Module *pModule) { WRAPPER_NO_CONTRACT; return m_pPEAssembly->Equals(pModule->m_pPEAssembly); }
    BOOL Equals(PEAssembly *pPEAssembly) { WRAPPER_NO_CONTRACT; return m_pPEAssembly->Equals(pPEAssembly); }
#endif // !DACCESS_COMPILE

    LPCUTF8 GetSimpleName()
    {
        WRAPPER_NO_CONTRACT;
        _ASSERTE(m_pSimpleName != NULL);
        return m_pSimpleName;
    }

    HRESULT GetScopeName(LPCUTF8 * pszName) { WRAPPER_NO_CONTRACT; return m_pPEAssembly->GetScopeName(pszName); }
    const SString &GetPath()
    {
        WRAPPER_NO_CONTRACT;
        // Validate the pointers are the same to ensure the lifetime of m_path is handled.
        _ASSERTE(m_path == m_pPEAssembly->GetPath().GetUnicode());
        return m_pPEAssembly->GetPath();
    }

#ifdef LOGGING
    LPCUTF8 GetDebugName() { WRAPPER_NO_CONTRACT; return m_pPEAssembly->GetDebugName(); }
#endif

    PEImageLayout * GetReadyToRunImage();
    PTR_READYTORUN_IMPORT_SECTION GetImportSections(COUNT_T *pCount);
    PTR_READYTORUN_IMPORT_SECTION GetImportSectionFromIndex(COUNT_T index);
    PTR_READYTORUN_IMPORT_SECTION GetImportSectionForRVA(RVA rva);

    // These are overridden by reflection modules
    virtual TADDR GetIL(RVA il);

    virtual PTR_VOID GetRvaField(RVA field);
    CHECK CheckRvaField(RVA field);
    CHECK CheckRvaField(RVA field, COUNT_T size);

    const void *GetInternalPInvokeTarget(RVA target)
    { WRAPPER_NO_CONTRACT; return m_pPEAssembly->GetInternalPInvokeTarget(target); }

    BOOL HasTls();
    BOOL IsRvaFieldTls(DWORD field);
    UINT32 GetFieldTlsOffset(DWORD field);
    UINT32 GetTlsIndex();

protected:
#ifndef DACCESS_COMPILE
    BOOL IsSigInILImpl(PCCOR_SIGNATURE signature) final;
#endif
public:

    mdToken GetEntryPointToken();

    BYTE *GetProfilerBase();

    BYTE* GetNativeFixupBlobData(RVA fixup);

    IMDInternalImport *GetNativeAssemblyImport(BOOL loadAllowed = TRUE);
    IMDInternalImport *GetNativeAssemblyImportIfLoaded();

    BOOL FixupNativeEntry(READYTORUN_IMPORT_SECTION * pSection, SIZE_T fixupIndex, SIZE_T *fixup, BOOL mayUsePrecompiledPInvokeMethods = TRUE);

    //this split exists to support new CLR Dump functionality in DAC.  The
    //template removes any indirections.
    BOOL FixupDelayList(TADDR pFixupList, BOOL mayUsePrecompiledPInvokeMethods = TRUE);

    template<typename Ptr, typename FixupNativeEntryCallback>
    BOOL FixupDelayListAux(TADDR pFixupList,
                           Ptr pThis, FixupNativeEntryCallback pfnCB,
                           PTR_READYTORUN_IMPORT_SECTION pImportSections, COUNT_T nImportSections,
                           PEDecoder * pNativeImage, BOOL mayUsePrecompiledPInvokeMethods = TRUE);
    void RunEagerFixups();
    void RunEagerFixupsUnlocked();

    ModuleBase *GetModuleFromIndex(DWORD ix);
    ModuleBase *GetModuleFromIndexIfLoaded(DWORD ix);

    BOOL IsReadyToRun() const
    {
        LIMITED_METHOD_DAC_CONTRACT;

#ifdef FEATURE_READYTORUN
        return m_pReadyToRunInfo != NULL;
#else
        return FALSE;
#endif
    }

#ifdef FEATURE_READYTORUN
    PTR_ReadyToRunInfo GetReadyToRunInfo() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_pReadyToRunInfo;
    }

    PTR_NativeImage GetCompositeNativeImage() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_pNativeImage;
    }
#endif

#ifdef _DEBUG
    //Similar to the ExpandAll we use for NGen, this forces jitting of all methods in a module.  This is
    //used for debug purposes though.
    void ExpandAll();
#endif

    BOOL IsIJWFixedUp() { return m_dwTransientFlags & IS_IJW_FIXED_UP; }
    void SetIsIJWFixedUp();

    BOOL IsBeingUnloaded() { return m_dwTransientFlags & IS_BEING_UNLOADED; }
    void   SetBeingUnloaded();
    void   StartUnload();

public:
#ifndef DACCESS_COMPILE
    void SetDynamicIL(mdToken token, TADDR blobAddress);
#endif // !DACCESS_COMPILE
    TADDR GetDynamicIL(mdToken token);

protected:
#ifndef DACCESS_COMPILE
    void SetDynamicRvaField(mdToken token, TADDR blobAddress);
#endif // !DACCESS_COMPILE

public:
    TADDR GetDynamicRvaField(mdToken token);

    // store and retrieve the instrumented IL offset mapping for a particular method
#if !defined(DACCESS_COMPILE)
    void SetInstrumentedILOffsetMapping(mdMethodDef token, InstrumentedILOffsetMapping mapping);
#endif // !DACCESS_COMPILE
    InstrumentedILOffsetMapping GetInstrumentedILOffsetMapping(mdMethodDef token);

public:
    const ReadyToRun_MethodIsGenericMap *m_pMethodIsGenericMap = &ReadyToRun_MethodIsGenericMap::EmptyInstance;
    const ReadyToRun_TypeGenericInfoMap *m_pTypeGenericInfoMap = &ReadyToRun_TypeGenericInfoMap::EmptyInstance;

protected:

    PTR_DomainAssembly      m_pDomainAssembly;

public:
    //-----------------------------------------------------------------------------------------
    // Returns a BOOL to indicate if we have computed whether compiler has instructed us to
    // wrap the non-CLS compliant exceptions or not.
    //-----------------------------------------------------------------------------------------
    BOOL                    IsRuntimeWrapExceptionsStatusComputed();

    //-----------------------------------------------------------------------------------------
    // If true,  any non-CLSCompliant exceptions (i.e. ones which derive from something other
    // than System.Exception) are wrapped in a RuntimeWrappedException instance.  In other
    // words, they become compliant
    //-----------------------------------------------------------------------------------------
    BOOL                    IsRuntimeWrapExceptions();
    void                    UpdateCachedIsRuntimeWrapExceptions();
    BOOL                    IsRuntimeWrapExceptionsDuringEH();

    //-----------------------------------------------------------------------------------------
    // If true, the built-in runtime-generated marshalling subsystem will be used for
    // P/Invokes, function pointer invocations, and delegates defined in this module
    //-----------------------------------------------------------------------------------------
    BOOL                    IsRuntimeMarshallingEnabled();

    BOOL                    IsRuntimeMarshallingEnabledCached()
    {
        LIMITED_METHOD_CONTRACT;
        return (m_dwPersistedFlags & RUNTIME_MARSHALLING_ENABLED_IS_CACHED);
    }

protected:
    // For reflection emit modules we set this flag when we emit the attribute, and always consider
    // the current setting of the flag to be set.
    void SetIsRuntimeWrapExceptionsCached_ForReflectionEmitModules()
    {
        LIMITED_METHOD_CONTRACT;
        m_dwPersistedFlags |= COMPUTED_WRAP_EXCEPTIONS;
    }
public:

    BOOL                    HasDefaultDllImportSearchPathsAttribute();

    BOOL IsDefaultDllImportSearchPathsAttributeCached()
    {
        LIMITED_METHOD_CONTRACT;
        return (m_dwPersistedFlags & DEFAULT_DLL_IMPORT_SEARCH_PATHS_IS_CACHED) != 0;
    }

    ULONG DefaultDllImportSearchPathsAttributeCachedValue()
    {
        LIMITED_METHOD_CONTRACT;
        return m_DefaultDllImportSearchPathsAttributeValue & 0xFFFFFFFD;
    }

    BOOL DllImportSearchAssemblyDirectory()
    {
        LIMITED_METHOD_CONTRACT;
        return (m_DefaultDllImportSearchPathsAttributeValue & 0x2) != 0;
    }

protected:

    // initialize Crst controlling the Dynamic IL hashtables
    void                    InitializeDynamicILCrst();

private:

    // This struct stores the data used by the managed debugging infrastructure.  If it turns out that
    // the debugger is increasing the size of the Module class by too much, we can consider allocating
    // this struct lazily on demand.
    struct DebuggerSpecificData
    {
        // Mutex protecting update access to the DynamicILBlobTable and TemporaryILBlobTable
        PTR_Crst                 m_pDynamicILCrst;

                                                // maps tokens for EnC/dynamics/reflection emit to their corresponding IL blobs
                                                // this map *always* overrides the Metadata RVA
        PTR_DynamicILBlobTable   m_pDynamicILBlobTable;

        // hash table storing any profiler-provided instrumented IL offset mapping
        PTR_ILOffsetMappingTable m_pILOffsetMappingTable;

        // Strict count of # of methods in this module that are JMC-enabled.
        LONG    m_cTotalJMCFuncs;

        // The default JMC status for methods in this module.
        // Individual methods can be overridden.
        bool    m_fDefaultJMCStatus;
    };

    DebuggerSpecificData  m_debuggerSpecificData;

#if defined(PROFILING_SUPPORTED) || defined(PROFILING_SUPPORTED_DATA)
    PTR_JITInlineTrackingMap m_pJitInlinerTrackingMap;
#endif // defined(PROFILING_SUPPORTED) || defined(PROFILING_SUPPORTED_DATA)

    // a.dll calls a method in b.dll and that method call a method in c.dll. When ngening
    // a.dll it is possible then method in b.dll can be inlined. When that happens a.ni.dll stores
    // an added native metadata which has information about assemblyRef to c.dll
    // Now due to facades, this scenario is very common. This led to lots of calls to
    // binder to get the module corresponding to assemblyRef in native metadata.
    // Adding a lookup map to cache assembly ptr so that AssemblySpec::LoadAssembly()
    // is not called for each fixup

    PTR_Assembly           *m_NativeMetadataAssemblyRefMap;

    LOADERHANDLE m_hExposedObject;

    // Buffer of Metadata storage for dynamic modules. May be NULL. This provides a reasonable way for
    // the debugger to get metadata of dynamic modules from out of process.
    // A dynamic module will eagerly serialize its metadata to this buffer.
    // This points at a uint32_t array.
    // The first uint32_t is the number of bytes in the saved metadata
    // Starting at the address of the second uint32_t value is the saved metadata itself
protected:
    TADDR m_pDynamicMetadata;

public:
#if !defined(DACCESS_COMPILE)
    PTR_Assembly GetNativeMetadataAssemblyRefFromCache(DWORD rid)
    {
        PTR_Assembly * NativeMetadataAssemblyRefMap = VolatileLoadWithoutBarrier(&m_NativeMetadataAssemblyRefMap);

        if (NativeMetadataAssemblyRefMap == NULL)
            return NULL;

        _ASSERTE(rid <= GetNativeMetadataAssemblyCount());
        return NativeMetadataAssemblyRefMap[rid - 1];
    }

    void SetNativeMetadataAssemblyRefInCache(DWORD rid, PTR_Assembly pAssembly);

    uint32_t GetNativeMetadataAssemblyCount();
#endif // !defined(DACCESS_COMPILE)

    friend struct ::cdac_data<Module>;
};

template<>
struct cdac_data<Module>
{
    static constexpr size_t Assembly = offsetof(Module, m_pAssembly);
    static constexpr size_t PEAssembly = offsetof(Module, m_pPEAssembly);
    static constexpr size_t Base = offsetof(Module, m_baseAddress);
    static constexpr size_t Flags = offsetof(Module, m_dwTransientFlags);
    static constexpr size_t LoaderAllocator = offsetof(Module, m_loaderAllocator);
    static constexpr size_t DynamicMetadata = offsetof(Module, m_pDynamicMetadata);
    static constexpr size_t Path = offsetof(Module, m_path);
    static constexpr size_t FileName = offsetof(Module, m_fileName);
    static constexpr size_t ReadyToRunInfo = offsetof(Module, m_pReadyToRunInfo);
    static constexpr size_t GrowableSymbolStream = offsetof(Module, m_pIStreamSym);

    // Lookup map pointers
    static constexpr size_t FieldDefToDescMap = offsetof(Module, m_FieldDefToDescMap);
    static constexpr size_t ManifestModuleReferencesMap = offsetof(Module, m_ManifestModuleReferencesMap);
    static constexpr size_t MemberRefToDescMap = offsetof(Module, m_MemberRefMap);
    static constexpr size_t MethodDefToDescMap = offsetof(Module, m_MethodDefToDescMap);
    static constexpr size_t TypeDefToMethodTableMap = offsetof(Module, m_TypeDefToMethodTableMap);
    static constexpr size_t TypeRefToMethodTableMap = offsetof(Module, m_TypeRefToMethodTableMap);
    static constexpr size_t MethodDefToILCodeVersioningStateMap = offsetof(Module, m_ILCodeVersioningStateMap);
};

//
// A ReflectionModule is a module created by reflection
//

class ReflectionModule : public Module
{
    VPTR_VTABLE_CLASS(ReflectionModule, Module)

 public:
    HCEESECTION m_sdataSection;

 protected:
    ICeeGenInternal * m_pCeeFileGen;
private:
    RefClassWriter       *m_pInMemoryWriter;


    // Simple Critical Section used for basic leaf-lock operatons.
    CrstExplicitInit        m_CrstLeafLock;

#if !defined DACCESS_COMPILE
    ReflectionModule(Assembly *pAssembly, PEAssembly *pPEAssembly);
#endif // !DACCESS_COMPILE

public:

#ifdef DACCESS_COMPILE
    // Accessor to expose m_pDynamicMetadata to debugger.
    TADDR GetDynamicMetadataBuffer() const;
#endif

#if !defined DACCESS_COMPILE
    static ReflectionModule *Create(Assembly *pAssembly, PEAssembly *pPEAssembly, AllocMemTracker *pamTracker, LPCWSTR szName);
    void Initialize(AllocMemTracker *pamTracker, LPCWSTR szName);
    void Destruct();
#endif // !DACCESS_COMPILE

    // Overrides functions to access sections
    virtual TADDR GetIL(RVA target);
    virtual PTR_VOID GetRvaField(RVA rva);

    ICeeGenInternal *GetCeeGen() {LIMITED_METHOD_CONTRACT;  return m_pCeeFileGen; }

    RefClassWriter *GetClassWriter()
    {
        LIMITED_METHOD_CONTRACT;

        return m_pInMemoryWriter;
    }

    // Eagerly serialize the metadata to a buffer that the debugger can retrieve.
    void CaptureModuleMetaDataToMemory();
};

// Module holders
FORCEINLINE void VoidModuleDestruct(Module *pModule)
{
#ifndef DACCESS_COMPILE
    if (g_fEEStarted)
        pModule->Destruct();
#endif
}

typedef Wrapper<Module*, DoNothing, VoidModuleDestruct, 0> ModuleHolder;



FORCEINLINE void VoidReflectionModuleDestruct(ReflectionModule *pModule)
{
#ifndef DACCESS_COMPILE
    pModule->Destruct();
#endif
}

typedef Wrapper<ReflectionModule*, DoNothing, VoidReflectionModuleDestruct, 0> ReflectionModuleHolder;



//----------------------------------------------------------------------
// VASigCookieEx (used to create a fake VASigCookie for unmanaged->managed
// calls to vararg functions. These fakes are distinguished from the
// real thing by having a null mdVASig.
//----------------------------------------------------------------------
struct VASigCookieEx : public VASigCookie
{
    const BYTE *m_pArgs;        // pointer to first unfixed unmanaged arg
};

// Save the command line for the current process.
void SaveManagedCommandLine(LPCWSTR pwzAssemblyPath, int argc, LPCWSTR *argv);

LPCWSTR GetCommandLineForDiagnostics();

#endif // !CEELOAD_H_
