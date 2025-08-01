// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//
// Physical promotion is an optimization where struct fields accessed as
// LCL_FLD nodes are promoted to individual primitive-typed local variables
// accessed as LCL_VAR, allowing register allocation and removing unnecessary
// memory operations.
//
// Key components:
//
// 1. Candidate Identification:
//    - Identifies struct locals that aren't already promoted and aren't address-exposed
//    - Analyzes access patterns to determine which fields are good promotion candidates
//    - Uses weighted cost models to balance performance and code size and to take PGO
//      data into account
//
// 2. Field Promotion:
//    - Creates primitive-typed replacement locals for selected fields
//    - Records which parts of the struct remains unpromoted
//
// 3. Access Transformation:
//    - Transforms local field accesses to use promoted field variables
//    - Decomposes struct stores and copies to operate on the primitive fields
//    - Handles call argument passing and returns with field lists where appropriate
//    - Tracks when values in promoted fields vs. original struct are fresher
//    - Inserts read-backs when the struct field is fresher than the promoted local
//    - Inserts write-backs when the promoted local is fresher than the struct field
//    - Ensures proper state across basic block boundaries and exception flow
//
// The transformation carefully handles OSR locals, parameters, and call arguments,
// while maintaining correct behavior for exception handling and control flow.
//

#include "jitpch.h"
#include "promotion.h"
#include "jitstd/algorithm.h"

//------------------------------------------------------------------------
// PhysicalPromotion: Promote structs based on primitive access patterns.
//
// Returns:
//    Suitable phase status.
//
PhaseStatus Compiler::PhysicalPromotion()
{
    if (!opts.OptEnabled(CLFLG_STRUCTPROMOTE))
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

    if (fgNoStructPromotion)
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

    if ((JitConfig.JitEnablePhysicalPromotion() == 0) && !compStressCompile(STRESS_PHYSICAL_PROMOTION, 25))
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

#ifdef DEBUG
    static ConfigMethodRange s_range;
    s_range.EnsureInit(JitConfig.JitEnablePhysicalPromotionRange());

    if (!s_range.Contains(info.compMethodHash()))
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }
#endif

    Promotion prom(this);
    return prom.Run();
}

// Represents an access into a struct local.
struct Access
{
    ClassLayout* Layout;
    unsigned     Offset;
    var_types    AccessType;

    // Number of times we saw the access.
    unsigned Count = 0;
    // Number of times this is stored from the result of a call. This includes
    // being passed as the retbuf. These stores cannot be decomposed and are
    // handled via readback.
    unsigned CountStoredFromCall = 0;
    // Number of times this is passed as a call arg. We insert writebacks
    // before these.
    unsigned CountCallArgs = 0;
    // Number of times this is passed as a register call arg. We may be able to
    // avoid the writeback for some overlapping replacements for these.
    unsigned CountRegCallArgs = 0;

    weight_t CountWtd               = 0;
    weight_t CountStoredFromCallWtd = 0;
    weight_t CountCallArgsWtd       = 0;
    weight_t CountRegCallArgsWtd    = 0;

#ifdef DEBUG
    // Number of times this access is the source of a store.
    unsigned CountStoreSource = 0;
    // Number of times this access is the destination of a store.
    unsigned CountStoreDestination = 0;
    unsigned CountReturns          = 0;
    // Number of times this is stored by being passed as the retbuf.
    // These stores need a readback
    unsigned CountPassedAsRetbuf = 0;

    weight_t CountStoreSourceWtd      = 0;
    weight_t CountStoreDestinationWtd = 0;
    weight_t CountReturnsWtd          = 0;
    weight_t CountPassedAsRetbufWtd   = 0;
#endif

    Access(unsigned offset, var_types accessType, ClassLayout* layout)
        : Layout(layout)
        , Offset(offset)
        , AccessType(accessType)
    {
    }

    unsigned GetAccessSize() const
    {
        return AccessType == TYP_STRUCT ? Layout->GetSize() : genTypeSize(AccessType);
    }

    bool Overlaps(unsigned otherStart, unsigned otherSize) const
    {
        unsigned end = Offset + GetAccessSize();
        if (end <= otherStart)
        {
            return false;
        }

        unsigned otherEnd = otherStart + otherSize;
        if (otherEnd <= Offset)
        {
            return false;
        }

        return true;
    }
};

enum class AccessKindFlags : uint32_t
{
    None             = 0,
    IsCallArg        = 1,
    IsRegCallArg     = 2,
    IsStoredFromCall = 4,
    IsCallRetBuf     = 8,
#ifdef DEBUG
    IsStoreSource      = 16,
    IsStoreDestination = 32,
    IsReturned         = 64,
#endif
};

inline constexpr AccessKindFlags operator~(AccessKindFlags a)
{
    return (AccessKindFlags)(~(uint32_t)a);
}

inline constexpr AccessKindFlags operator|(AccessKindFlags a, AccessKindFlags b)
{
    return (AccessKindFlags)((uint32_t)a | (uint32_t)b);
}

inline constexpr AccessKindFlags operator&(AccessKindFlags a, AccessKindFlags b)
{
    return (AccessKindFlags)((uint32_t)a & (uint32_t)b);
}

inline AccessKindFlags& operator|=(AccessKindFlags& a, AccessKindFlags b)
{
    return a = (AccessKindFlags)((uint32_t)a | (uint32_t)b);
}

inline AccessKindFlags& operator&=(AccessKindFlags& a, AccessKindFlags b)
{
    return a = (AccessKindFlags)((uint32_t)a & (uint32_t)b);
}

//------------------------------------------------------------------------
// OverlappingReplacements:
//   Find replacements that overlap the specified [offset..offset+size) interval.
//
// Parameters:
//   offset           - Starting offset of interval
//   size             - Size of interval
//   firstReplacement - [out] The first replacement that overlaps
//   endReplacement   - [out, optional] One past the last replacement that overlaps
//
// Returns:
//   True if any replacement overlaps; otherwise false.
//
bool AggregateInfo::OverlappingReplacements(unsigned      offset,
                                            unsigned      size,
                                            Replacement** firstReplacement,
                                            Replacement** endReplacement)
{
    size_t firstIndex = Promotion::BinarySearch<Replacement, &Replacement::Offset>(Replacements, offset);
    if ((ssize_t)firstIndex < 0)
    {
        firstIndex = ~firstIndex;
        if (firstIndex > 0)
        {
            Replacement& lastRepBefore = Replacements[firstIndex - 1];
            if ((lastRepBefore.Offset + genTypeSize(lastRepBefore.AccessType)) > offset)
            {
                // Overlap with last entry starting before offs.
                firstIndex--;
            }
            else if (firstIndex >= Replacements.size())
            {
                // Starts after last replacement ends.
                return false;
            }
        }

        const Replacement& first = Replacements[firstIndex];
        if (first.Offset >= (offset + size))
        {
            // First candidate starts after this ends.
            return false;
        }
    }

    assert((firstIndex < Replacements.size()) && Replacements[firstIndex].Overlaps(offset, size));
    *firstReplacement = &Replacements[firstIndex];

    if (endReplacement != nullptr)
    {
        size_t lastIndex = Promotion::BinarySearch<Replacement, &Replacement::Offset>(Replacements, offset + size);
        if ((ssize_t)lastIndex < 0)
        {
            lastIndex = ~lastIndex;
        }

        // Since we verified above that there is an overlapping replacement
        // we know that lastIndex exists and is the next one that does not
        // overlap.
        assert(lastIndex > 0);
        *endReplacement = Replacements.data() + lastIndex;
    }

    return true;
}

//------------------------------------------------------------------------
// AggregateInfoMap::AggregateInfoMap:
//   Construct a map that maps locals to AggregateInfo.
//
// Parameters:
//   allocator - The allocator
//   numLocals - Number of locals to support in the map
//
AggregateInfoMap::AggregateInfoMap(CompAllocator allocator, unsigned numLocals)
    : m_aggregates(allocator)
    , m_numLocals(numLocals)
{
    m_lclNumToAggregateIndex = new (allocator) unsigned[numLocals];
    for (unsigned i = 0; i < numLocals; i++)
    {
        m_lclNumToAggregateIndex[i] = UINT_MAX;
    }
}

//------------------------------------------------------------------------
// AggregateInfoMap::Add:
//   Add information about a physically promoted aggregate to the map.
//
// Parameters:
//   agg - The entry to add
//
void AggregateInfoMap::Add(AggregateInfo* agg)
{
    assert(agg->LclNum < m_numLocals);
    assert(m_lclNumToAggregateIndex[agg->LclNum] == UINT_MAX);

    m_lclNumToAggregateIndex[agg->LclNum] = static_cast<unsigned>(m_aggregates.size());
    m_aggregates.push_back(agg);
}

//------------------------------------------------------------------------
// AggregateInfoMap::Lookup:
//   Lookup the promotion information for a local.
//
// Parameters:
//   lclNum - The local number
//
// Returns:
//   Pointer to the aggregate information, or nullptr if the local is not
//   physically promoted.
//
AggregateInfo* AggregateInfoMap::Lookup(unsigned lclNum)
{
    assert(lclNum < m_numLocals);
    unsigned index = m_lclNumToAggregateIndex[lclNum];

    if (index == UINT_MAX)
    {
        return nullptr;
    }

    assert(m_aggregates.size() > index);
    return m_aggregates[index];
}

struct PrimitiveAccess
{
    unsigned  Count    = 0;
    weight_t  CountWtd = 0;
    unsigned  Offset;
    var_types AccessType;

    PrimitiveAccess(unsigned offset, var_types accessType)
        : Offset(offset)
        , AccessType(accessType)
    {
    }
};

// Tracks all the accesses into one particular struct local.
class LocalUses
{
    jitstd::vector<Access>          m_accesses;
    jitstd::vector<PrimitiveAccess> m_inducedAccesses;

public:
    LocalUses(Compiler* comp)
        : m_accesses(comp->getAllocator(CMK_Promotion))
        , m_inducedAccesses(comp->getAllocator(CMK_Promotion))
    {
    }

    //------------------------------------------------------------------------
    // RecordAccess:
    //   Record an access into this local with the specified offset and access type.
    //
    // Parameters:
    //   offs         - The offset being accessed
    //   accessType   - The type of the access
    //   accessLayout - The layout of the access, for accessType == TYP_STRUCT
    //   flags        - Flags classifying the access
    //   weight       - Weight of the block containing the access
    //
    void RecordAccess(
        unsigned offs, var_types accessType, ClassLayout* accessLayout, AccessKindFlags flags, weight_t weight)
    {
        Access* access = nullptr;

        size_t index = 0;
        if (m_accesses.size() > 0)
        {
            index = Promotion::BinarySearch<Access, &Access::Offset>(m_accesses, offs);
            if ((ssize_t)index >= 0)
            {
                do
                {
                    Access& candidateAccess = m_accesses[index];
                    if ((candidateAccess.AccessType == accessType) && (candidateAccess.Layout == accessLayout))
                    {
                        access = &candidateAccess;
                        break;
                    }

                    index++;
                } while (index < m_accesses.size() && m_accesses[index].Offset == offs);
            }
            else
            {
                index = ~index;
            }
        }

        if (access == nullptr)
        {
            access = &*m_accesses.insert(m_accesses.begin() + index, Access(offs, accessType, accessLayout));
        }

        access->Count++;
        access->CountWtd += weight;

        if ((flags & AccessKindFlags::IsCallArg) != AccessKindFlags::None)
        {
            access->CountCallArgs++;
            access->CountCallArgsWtd += weight;

            if ((flags & AccessKindFlags::IsRegCallArg) != AccessKindFlags::None)
            {
                access->CountRegCallArgs++;
                access->CountRegCallArgsWtd += weight;
            }
        }

        if ((flags & (AccessKindFlags::IsStoredFromCall | AccessKindFlags::IsCallRetBuf)) != AccessKindFlags::None)
        {
            access->CountStoredFromCall++;
            access->CountStoredFromCallWtd += weight;
        }

#ifdef DEBUG
        if ((flags & AccessKindFlags::IsCallRetBuf) != AccessKindFlags::None)
        {
            access->CountPassedAsRetbuf++;
            access->CountPassedAsRetbufWtd += weight;
        }

        if ((flags & AccessKindFlags::IsStoreSource) != AccessKindFlags::None)
        {
            access->CountStoreSource++;
            access->CountStoreSourceWtd += weight;
        }

        if ((flags & AccessKindFlags::IsStoreDestination) != AccessKindFlags::None)
        {
            access->CountStoreDestination++;
            access->CountStoreDestinationWtd += weight;
        }

        if ((flags & AccessKindFlags::IsReturned) != AccessKindFlags::None)
        {
            access->CountReturns++;
            access->CountReturnsWtd += weight;
        }
#endif
    }

    //------------------------------------------------------------------------
    // RecordInducedAccess:
    //   Record an induced access into this local with the specified offset and access type.
    //
    // Parameters:
    //   offs         - The offset being accessed
    //   accessType   - The type of the access
    //   weight       - Weight of the block containing the access
    //
    // Remarks:
    //   Induced accesses are accesses that are induced by physical promotion
    //   due to store decompositon. They are always of primitive type.
    //
    void RecordInducedAccess(unsigned offs, var_types accessType, weight_t weight)
    {
        PrimitiveAccess* access = nullptr;

        size_t index = 0;
        if (m_inducedAccesses.size() > 0)
        {
            index = Promotion::BinarySearch<PrimitiveAccess, &PrimitiveAccess::Offset>(m_inducedAccesses, offs);
            if ((ssize_t)index >= 0)
            {
                do
                {
                    PrimitiveAccess& candidateAccess = m_inducedAccesses[index];
                    if (candidateAccess.AccessType == accessType)
                    {
                        access = &candidateAccess;
                        break;
                    }

                    index++;
                } while (index < m_inducedAccesses.size() && m_inducedAccesses[index].Offset == offs);
            }
            else
            {
                index = ~index;
            }
        }

        if (access == nullptr)
        {
            access = &*m_inducedAccesses.insert(m_inducedAccesses.begin() + index, PrimitiveAccess(offs, accessType));
        }

        access->Count++;
        access->CountWtd += weight;
    }

    //------------------------------------------------------------------------
    // PickPromotions:
    //   Pick specific replacements to make for this struct local after a set
    //   of accesses have been recorded.
    //
    // Parameters:
    //   comp   - Compiler instance
    //   lclNum - Local num for this struct local
    //   aggregates - Map to add aggregate information into if promotion was done
    //
    // Returns:
    //   Number of promotions picked. If above 0, an entry was added to aggregates.
    //
    int PickPromotions(Compiler* comp, unsigned lclNum, AggregateInfoMap& aggregates)
    {
        if (m_accesses.size() <= 0)
        {
            return 0;
        }

        JITDUMP("Picking promotions for V%02u\n", lclNum);

        AggregateInfo* agg     = nullptr;
        int            numReps = 0;
        for (size_t i = 0; i < m_accesses.size(); i++)
        {
            const Access& access = m_accesses[i];

            if (access.AccessType == TYP_STRUCT)
            {
                continue;
            }

            if (!EvaluateReplacement(comp, lclNum, access, 0, 0))
            {
                continue;
            }

            if (agg == nullptr)
            {
                agg = new (comp, CMK_Promotion) AggregateInfo(comp->getAllocator(CMK_Promotion), lclNum);
                aggregates.Add(agg);
            }

            agg->Replacements.push_back(Replacement(access.Offset, access.AccessType));
            numReps++;

            if (agg->Replacements.size() >= PHYSICAL_PROMOTION_MAX_PROMOTIONS_PER_STRUCT)
            {
                JITDUMP("  Promoted %zu fields in V%02u; will not promote more\n", agg->Replacements.size(),
                        agg->LclNum);
                break;
            }
        }

        JITDUMP("\n");
        return numReps;
    }

    //------------------------------------------------------------------------
    // PickInducedPromotions:
    //   Pick additional promotions to make based on the fact that some
    //   accesses will be induced by store decomposition.
    //
    // Parameters:
    //   comp   - Compiler instance
    //   lclNum - Local num for this struct local
    //   aggregates - Map for aggregate information
    //
    // Returns:
    //   Number of new promotions.
    //
    int PickInducedPromotions(Compiler* comp, unsigned lclNum, AggregateInfoMap& aggregates)
    {
        if (m_inducedAccesses.size() <= 0)
        {
            return 0;
        }

        AggregateInfo* agg = aggregates.Lookup(lclNum);

        if ((agg != nullptr) && (agg->Replacements.size() >= PHYSICAL_PROMOTION_MAX_PROMOTIONS_PER_STRUCT))
        {
            return 0;
        }

        int numReps = 0;
        JITDUMP("Picking induced promotions for V%02u\n", lclNum);
        for (PrimitiveAccess& inducedAccess : m_inducedAccesses)
        {
            bool overlapsOtherInducedAccess = false;
            for (PrimitiveAccess& otherInducedAccess : m_inducedAccesses)
            {
                if (&otherInducedAccess == &inducedAccess)
                {
                    continue;
                }

                if (inducedAccess.Offset + genTypeSize(inducedAccess.AccessType) <= otherInducedAccess.Offset)
                {
                    break;
                }

                if (otherInducedAccess.Offset + genTypeSize(otherInducedAccess.AccessType) <= inducedAccess.Offset)
                {
                    continue;
                }

                overlapsOtherInducedAccess = true;
                break;
            }

            if (overlapsOtherInducedAccess)
            {
                continue;
            }

            Access* access = FindAccess(inducedAccess.Offset, inducedAccess.AccessType);

            if (access == nullptr)
            {
                Access fakeAccess(inducedAccess.Offset, inducedAccess.AccessType, nullptr);
                if (!EvaluateReplacement(comp, lclNum, fakeAccess, inducedAccess.Count, inducedAccess.CountWtd))
                {
                    continue;
                }
            }
            else
            {
                if (!EvaluateReplacement(comp, lclNum, *access, inducedAccess.Count, inducedAccess.CountWtd))
                {
                    continue;
                }
            }

            if (agg == nullptr)
            {
                agg = new (comp, CMK_Promotion) AggregateInfo(comp->getAllocator(CMK_Promotion), lclNum);
                aggregates.Add(agg);
            }

            size_t insertionIndex;
            if (agg->Replacements.size() > 0)
            {
#ifdef DEBUG
                Replacement* overlapRep;
                assert(!agg->OverlappingReplacements(inducedAccess.Offset, genTypeSize(inducedAccess.AccessType),
                                                     &overlapRep, nullptr));
#endif

                insertionIndex =
                    Promotion::BinarySearch<Replacement, &Replacement::Offset>(agg->Replacements, inducedAccess.Offset);
                assert((ssize_t)insertionIndex < 0);
                insertionIndex = ~insertionIndex;
            }
            else
            {
                insertionIndex = 0;
            }

            agg->Replacements.insert(agg->Replacements.begin() + insertionIndex,
                                     Replacement(inducedAccess.Offset, inducedAccess.AccessType));
            numReps++;

            if (agg->Replacements.size() >= PHYSICAL_PROMOTION_MAX_PROMOTIONS_PER_STRUCT)
            {
                JITDUMP("  Promoted %zu fields in V%02u; will not promote more\n", agg->Replacements.size());
                break;
            }
        }

        return numReps;
    }

    //------------------------------------------------------------------------
    // EvaluateReplacement:
    //   Evaluate legality and profitability of a single replacement candidate.
    //
    // Parameters:
    //   comp            - Compiler instance
    //   lclNum          - Local num for this struct local
    //   access          - Access information for the candidate.
    //   inducedCountWtd - Additional weighted count due to induced accesses.
    //
    // Returns:
    //   True if we should promote this access and create a replacement; otherwise false.
    //
    bool EvaluateReplacement(
        Compiler* comp, unsigned lclNum, const Access& access, unsigned inducedCount, weight_t inducedCountWtd)
    {
        // Verify that this replacement has proper GC ness compared to the
        // layout. While reinterpreting GC fields to integers can be considered
        // UB, there are scenarios where it can happen safely:
        //
        // * The user code could have guarded the access with a dynamic check
        //   that it doesn't contain a GC pointer, so that the access is actually
        //   in dead code. This happens e.g. in span functions in SPC.
        //
        // * For byrefs, reinterpreting as an integer could be ok in a
        //   restricted scope due to pinning.
        //
        // In theory we could allow these promotions in the restricted scope,
        // but currently physical promotion works on a function-wide basis.

        LclVarDsc*   lcl    = comp->lvaGetDesc(lclNum);
        ClassLayout* layout = lcl->GetLayout();
        if (layout->IntersectsGCPtr(access.Offset, genTypeSize(access.AccessType)))
        {
            if (((access.Offset % TARGET_POINTER_SIZE) != 0) ||
                (layout->GetGCPtrType(access.Offset / TARGET_POINTER_SIZE) != access.AccessType))
            {
                return false;
            }
        }
        else
        {
            if (varTypeIsGC(access.AccessType))
            {
                return false;
            }
        }

        unsigned countOverlappedCallArg        = 0;
        unsigned countOverlappedStoredFromCall = 0;

        weight_t countOverlappedCallArgWtd        = 0;
        weight_t countOverlappedStoredFromCallWtd = 0;

        bool overlap = false;
        for (const Access& otherAccess : m_accesses)
        {
            if (&otherAccess == &access)
            {
                continue;
            }

            if (!otherAccess.Overlaps(access.Offset, genTypeSize(access.AccessType)))
            {
                continue;
            }

            if (otherAccess.AccessType != TYP_STRUCT)
            {
                return false;
            }

            countOverlappedCallArg += otherAccess.CountCallArgs;
            countOverlappedStoredFromCall += otherAccess.CountStoredFromCall;

            countOverlappedCallArgWtd += otherAccess.CountCallArgsWtd;
            countOverlappedStoredFromCallWtd += otherAccess.CountStoredFromCallWtd;

            if (otherAccess.CountRegCallArgs > 0)
            {
                auto willPassFieldInRegister = [=, &access, &otherAccess]() {
                    if (access.Offset < otherAccess.Offset)
                    {
                        return false;
                    }

                    unsigned layoutOffset = access.Offset - otherAccess.Offset;
                    if ((layoutOffset % TARGET_POINTER_SIZE) != 0)
                    {
                        return false;
                    }

                    unsigned accessSize = genTypeSize(access.AccessType);
                    if (accessSize == TARGET_POINTER_SIZE)
                    {
                        return true;
                    }

                    const SegmentList& significantSegments = otherAccess.Layout->GetNonPadding(comp);
                    if (!significantSegments.Intersects(
                            SegmentList::Segment(layoutOffset + accessSize, layoutOffset + TARGET_POINTER_SIZE)))
                    {
                        return true;
                    }

                    return false;
                };
                // We may be able to decompose the call argument to require no
                // write-back.
                if (willPassFieldInRegister())
                {
                    countOverlappedCallArg -= otherAccess.CountRegCallArgs;
                    countOverlappedCallArgWtd -= otherAccess.CountRegCallArgsWtd;
                }
            }
        }

        // We cost any normal access (which is a struct load or store) without promotion at 3 cycles.
        const weight_t COST_STRUCT_ACCESS_CYCLES = 3;
        // And at 4 bytes size
        const weight_t COST_STRUCT_ACCESS_SIZE = 4;

        weight_t costWithout = 0;
        weight_t sizeWithout = 0;

        costWithout += (access.CountWtd + inducedCountWtd) * COST_STRUCT_ACCESS_CYCLES;
        sizeWithout += (access.Count + inducedCount) * COST_STRUCT_ACCESS_SIZE;

        weight_t costWith = 0;
        weight_t sizeWith = 0;

        // For promoted accesses we expect these to turn into reg-reg movs (and in many cases be fully contained in the
        // parent).
        // We cost these at 0.5 cycles.
        const weight_t COST_REG_ACCESS_CYCLES = 0.5;
        // And 2 byte size
        const weight_t COST_REG_ACCESS_SIZE = 2;

        costWith += (access.CountWtd + inducedCountWtd) * COST_REG_ACCESS_CYCLES;
        sizeWith += (access.Count + inducedCount) * COST_REG_ACCESS_SIZE;

        // Now look at the overlapping struct uses that promotion will make more expensive.

        unsigned countReadBacks    = 0;
        weight_t countReadBacksWtd = 0;

        // For OSR locals we always need one read back.
        if (lcl->lvIsOSRLocal)
        {
            countReadBacks++;
            countReadBacksWtd += comp->fgFirstBB->getBBWeight(comp);
        }
        else if (lcl->lvIsParam)
        {
            // For parameters, the backend may be able to map it directly from a register.
            if (Promotion::MapsToParameterRegister(comp, lclNum, access.Offset, access.AccessType))
            {
                // No promotion will result in a store to stack in the prolog.
                costWithout += COST_STRUCT_ACCESS_CYCLES * comp->fgFirstBB->getBBWeight(comp);
                sizeWithout += COST_STRUCT_ACCESS_SIZE;

                // Promotion we cost like the normal reg accesses above
                costWith += COST_REG_ACCESS_CYCLES * comp->fgFirstBB->getBBWeight(comp);
                sizeWith += COST_REG_ACCESS_SIZE;
            }
            else
            {
                // Otherwise we expect no prolog work to be required if we
                // don't promote, and we need a read back from the stack.
                countReadBacks++;
                countReadBacksWtd += comp->fgFirstBB->getBBWeight(comp);
            }
        }

        // If the struct is stored from a call (either due to a multireg
        // return or by being passed as the retbuffer) then we need a readback
        // after.
        //
        // In the future we could allow multireg returns without a readback by
        // a sort of forward substitution optimization in the backend.
        countReadBacksWtd += countOverlappedStoredFromCallWtd;
        countReadBacks += countOverlappedStoredFromCall;

        // A readback turns into a stack load.
        costWith += countReadBacksWtd * COST_STRUCT_ACCESS_CYCLES;
        sizeWith += countReadBacks * COST_STRUCT_ACCESS_SIZE;

        // Write backs with TYP_REFs when the base local is an implicit byref
        // involves checked write barriers, so they are very expensive. We cost that at 10 cycles.
        const weight_t COST_WRITEBARRIER_CYCLES = 10;
        const weight_t COST_WRITEBARRIER_SIZE   = 10;

        // TODO-CQ: This should be adjusted once we type implicit byrefs as TYP_I_IMPL.
        // Otherwise we cost it like a store to stack at 3 cycles.
        weight_t writeBackCost = comp->lvaIsImplicitByRefLocal(lclNum) && (access.AccessType == TYP_REF)
                                     ? COST_WRITEBARRIER_CYCLES
                                     : COST_STRUCT_ACCESS_CYCLES;
        weight_t writeBackSize = comp->lvaIsImplicitByRefLocal(lclNum) && (access.AccessType == TYP_REF)
                                     ? COST_WRITEBARRIER_SIZE
                                     : COST_STRUCT_ACCESS_SIZE;

        // We write back before an overlapping struct use passed as an arg.
        // TODO-CQ: A store-forwarding optimization in lowering could get rid
        // of these copies; however, it requires lowering to be able to prove
        // that not writing the fields into the struct local is ok.
        //
        // Note: Technically we also introduce writebacks before returns that
        // we could account for, however the returns we see during physical
        // promotion are only for structs returned in registers and in most
        // cases the writeback introduced means we can eliminate an earlier
        // "natural" writeback, balancing out the cost.
        // Thus _not_ accounting for these is a CQ improvements.
        // (Additionally, if it weren't we could teach the backend some
        // store-forwarding/forward sub to make the write backs "free".)
        weight_t countWriteBacksWtd = countOverlappedCallArgWtd;
        unsigned countWriteBacks    = countOverlappedCallArg;
        costWith += countWriteBacksWtd * writeBackCost;
        sizeWith += countWriteBacks * writeBackSize;

        // Overlapping stores are decomposable so we don't cost them as
        // being more expensive than their unpromoted counterparts (i.e. we
        // don't consider them at all). However, we should do something more
        // clever here, since:
        // * We may still end up writing the full remainder as part of the
        //   decomposed store, in which case all the field writes are just
        //   added code size/perf cost.
        // * Even if we don't, decomposing a single struct write into many
        //   field writes is not necessarily profitable (e.g. 16 byte field
        //   stores vs 1 XMM load/store).
        //
        // TODO-CQ: This ends up being a combinatorial optimization problem. We
        // need to take a more "whole-struct" view here and look at sets of
        // fields we are promoting together, evaluating all of them at once in
        // comparison with the covering struct uses. This will also allow us to
        // give a bonus to promoting remainders that may not have scalar uses
        // but will allow fully decomposing stores away.

        weight_t cycleImprovementPerInvoc = (costWithout - costWith) / comp->fgFirstBB->getBBWeight(comp);
        weight_t sizeImprovement          = sizeWithout - sizeWith;

        JITDUMP("  Evaluating access %s @ %03u\n", varTypeName(access.AccessType), access.Offset);
        JITDUMP("    Single write-back cost: " FMT_WT "\n", writeBackCost);
        JITDUMP("    Write backs: " FMT_WT "\n", countWriteBacksWtd);
        JITDUMP("    Read backs: " FMT_WT "\n", countReadBacksWtd);
        JITDUMP("    Estimated cycle improvement: " FMT_WT " cycles per invocation\n", cycleImprovementPerInvoc);
        JITDUMP("    Estimated size improvement: " FMT_WT " bytes\n", sizeImprovement);

        // We allow X bytes of code size regressions for every cycle of
        // estimated improvement. Note that generally both estimates agree on
        // whether promotion is an improvement or regression, so this is really
        // only for rare cases where we have many call arg uses in rarely
        // executed blocks.
        const weight_t ALLOWED_SIZE_REGRESSION_PER_CYCLE_IMPROVEMENT = 2;

        if ((cycleImprovementPerInvoc > 0) &&
            ((cycleImprovementPerInvoc * ALLOWED_SIZE_REGRESSION_PER_CYCLE_IMPROVEMENT) >= -sizeImprovement))
        {
            JITDUMP("  Promoting replacement (cycle improvement)\n\n");
            return true;
        }

        // Similarly, even for a cycle-wise regression, if we see a large size
        // wise improvement we may want to promote. The main case is where all
        // uses are in blocks with bbWeight=0, but we still estimate a
        // size-wise improvement.
        const weight_t ALLOWED_CYCLE_REGRESSION_PER_SIZE_IMPROVEMENT = 0.01;

        if ((sizeImprovement > 0) &&
            ((sizeImprovement * ALLOWED_CYCLE_REGRESSION_PER_SIZE_IMPROVEMENT) >= -cycleImprovementPerInvoc))
        {
            JITDUMP("  Promoting replacement (size improvement)\n\n");
            return true;
        }

#ifdef DEBUG
        if (comp->compStressCompile(Compiler::STRESS_PHYSICAL_PROMOTION_COST, 25))
        {
            JITDUMP("  Promoting replacement (stress)\n\n");
            return true;
        }
#endif

        JITDUMP("  Disqualifying replacement\n\n");
        return false;
    }

    //------------------------------------------------------------------------
    // ClearInducedAccesses:
    //   Clear the stored induced access metrics.
    //
    void ClearInducedAccesses()
    {
        m_inducedAccesses.clear();
    }

#ifdef DEBUG
    //------------------------------------------------------------------------
    // DumpAccesses:
    //   Dump the stored access metrics for a specified local.
    //
    // Parameters:
    //   lclNum - The local
    //
    void DumpAccesses(unsigned lclNum)
    {
        if (m_accesses.size() <= 0)
        {
            return;
        }

        printf("Accesses for V%02u\n", lclNum);
        for (Access& access : m_accesses)
        {
            if (access.AccessType == TYP_STRUCT)
            {
                printf("  [%03u..%03u) as %s\n", access.Offset, access.Offset + access.Layout->GetSize(),
                       access.Layout->GetClassName());
            }
            else
            {
                printf("  %s @ %03u\n", varTypeName(access.AccessType), access.Offset);
            }

            printf("    #:                             (%u, " FMT_WT ")\n", access.Count, access.CountWtd);
            printf("    # store source:                (%u, " FMT_WT ")\n", access.CountStoreSource,
                   access.CountStoreSourceWtd);
            printf("    # store destination:           (%u, " FMT_WT ")\n", access.CountStoreDestination,
                   access.CountStoreDestinationWtd);
            printf("    # as call arg:                 (%u, " FMT_WT ")\n", access.CountCallArgs,
                   access.CountCallArgsWtd);
            printf("    # as reg call arg:             (%u, " FMT_WT ")\n", access.CountRegCallArgs,
                   access.CountRegCallArgsWtd);
            printf("    # as retbuf:                   (%u, " FMT_WT ")\n", access.CountPassedAsRetbuf,
                   access.CountPassedAsRetbufWtd);
            printf("    # as returned value:           (%u, " FMT_WT ")\n\n", access.CountReturns,
                   access.CountReturnsWtd);
        }
    }

    //------------------------------------------------------------------------
    // DumpInducedAccesses:
    //   Dump induced accesses for a specified struct local.
    //
    // Parameters:
    //   lclNum - The local
    //
    void DumpInducedAccesses(unsigned lclNum)
    {
        if (m_inducedAccesses.size() <= 0)
        {
            return;
        }

        printf("Induced accesses for V%02u\n", lclNum);
        for (PrimitiveAccess& access : m_inducedAccesses)
        {
            printf("  %s @ %03u\n", varTypeName(access.AccessType), access.Offset);
            printf("    #: (%u, " FMT_WT ")\n", access.Count, access.CountWtd);
        }
    }
#endif

private:
    //------------------------------------------------------------------------
    // FindAccess:
    //   Find access metrics information for the specified offset and access type.
    //
    // Parameters:
    //   offs       - The offset
    //   accessType - Access type
    //
    // Returns:
    //   Pointer to a matching access, or nullptr if no match was found.
    //
    Access* FindAccess(unsigned offs, var_types accessType)
    {
        if (m_accesses.size() <= 0)
        {
            return nullptr;
        }

        size_t index = Promotion::BinarySearch<Access, &Access::Offset>(m_accesses, offs);
        if ((ssize_t)index < 0)
        {
            return nullptr;
        }

        do
        {
            Access& candidateAccess = m_accesses[index];
            if (candidateAccess.AccessType == accessType)
            {
                return &candidateAccess;
            }

            index++;
        } while ((index < m_accesses.size()) && (m_accesses[index].Offset == offs));

        return nullptr;
    }
};

// Struct used to save all struct stores involving physical promotion candidates.
// These stores can induce new field accesses as part of store decomposition.
struct CandidateStore
{
    GenTreeLclVarCommon* Store;
    BasicBlock*          Block;
};

// Visitor that records information about uses of struct locals.
class LocalsUseVisitor : public GenTreeVisitor<LocalsUseVisitor>
{
    Promotion*                 m_prom;
    LocalUses**                m_uses;
    BasicBlock*                m_curBB = nullptr;
    ArrayStack<CandidateStore> m_candidateStores;

public:
    enum
    {
        DoPreOrder   = true,
        ComputeStack = true,
    };

    LocalsUseVisitor(Promotion* prom)
        : GenTreeVisitor(prom->m_compiler)
        , m_prom(prom)
        , m_candidateStores(prom->m_compiler->getAllocator(CMK_Promotion))
    {
        m_uses = new (prom->m_compiler, CMK_Promotion) LocalUses* [prom->m_compiler->lvaCount] {};
    }

    //------------------------------------------------------------------------
    // SetBB:
    //   Set current BB we are visiting. Used to get BB weights for access costing.
    //
    // Parameters:
    //   bb - The current basic block.
    //
    void SetBB(BasicBlock* bb)
    {
        m_curBB = bb;
    }

    //------------------------------------------------------------------------
    // PreOrderVisit:
    //   Visit a node in preorder and add its use information to the metrics.
    //
    // Parameters:
    //   use  - The use edge
    //   user - The user
    //
    // Returns:
    //   Visitor result
    //
    fgWalkResult PreOrderVisit(GenTree** use, GenTree* user)
    {
        GenTree* tree = *use;

        if (tree->OperIsAnyLocal())
        {
            GenTreeLclVarCommon* lcl         = tree->AsLclVarCommon();
            LclVarDsc*           dsc         = m_compiler->lvaGetDesc(lcl);
            bool                 isCandidate = Promotion::IsCandidateForPhysicalPromotion(dsc);
            if (isCandidate)
            {
                var_types       accessType;
                ClassLayout*    accessLayout;
                AccessKindFlags accessFlags;

                if (lcl->OperIs(GT_LCL_ADDR))
                {
                    assert(user->OperIs(GT_CALL) && dsc->IsDefinedViaAddress() &&
                           (user->AsCall()->gtArgs.GetRetBufferArg()->GetNode() == lcl));

                    accessType   = TYP_STRUCT;
                    accessLayout = m_compiler->typGetObjLayout(user->AsCall()->gtRetClsHnd);
                    accessFlags  = AccessKindFlags::IsCallRetBuf;
                }
                else
                {
                    GenTree* effectiveUser = user;
                    if ((user != nullptr) && user->OperIs(GT_COMMA))
                    {
                        effectiveUser = Promotion::EffectiveUser(m_ancestors);
                    }

                    accessType   = lcl->TypeGet();
                    accessLayout = accessType == TYP_STRUCT ? lcl->GetLayout(m_compiler) : nullptr;
                    accessFlags  = ClassifyLocalAccess(lcl, effectiveUser);
                }

                LocalUses* uses = GetOrCreateUses(lcl->GetLclNum());
                unsigned   offs = lcl->GetLclOffs();
                uses->RecordAccess(offs, accessType, accessLayout, accessFlags, m_curBB->getBBWeight(m_compiler));
            }

            if (tree->OperIsLocalStore() && tree->TypeIs(TYP_STRUCT))
            {
                GenTree* data = tree->Data()->gtEffectiveVal();
                if (data->OperIsLocalRead() && (isCandidate || Promotion::IsCandidateForPhysicalPromotion(
                                                                   m_compiler->lvaGetDesc(data->AsLclVarCommon()))))
                {
                    m_candidateStores.Push(CandidateStore{tree->AsLclVarCommon(), m_curBB});
                }
            }
        }

        return fgWalkResult::WALK_CONTINUE;
    }

    //------------------------------------------------------------------------
    // PickPromotions:
    //   Pick promotions and create aggregate information for each promoted
    //   struct with promotions.
    //
    // Parameters:
    //   aggregates - Map for aggregates
    //
    // Returns:
    //   True if any struct was physically promoted with at least one replacement;
    //   otherwise false.
    //
    bool PickPromotions(AggregateInfoMap& aggregates)
    {
        JITDUMP("Picking promotions\n");

        int totalNumPromotions = 0;
        // We limit the total number of promotions picked based on the tracking
        // limit to avoid blowup in the superlinear liveness computation in
        // pathological cases, and also because once we stop tracking the fields there is no benefit anymore.
        //
        // This logic could be improved by the use of ref counting to pick the
        // smart fields to compute liveness for, but as of writing this there
        // is no example in the built-in SPMI collections that hits this limit.
        //
        // Note that we may go slightly over this as once we start picking
        // replacement locals for a single struct we do not stop until we get
        // to the next struct, but PHYSICAL_PROMOTION_MAX_PROMOTIONS_PER_STRUCT
        // puts a limit on the number of promotions in each struct so this is
        // fine to avoid the pathological cases.
        const int maxTotalNumPromotions = JitConfig.JitMaxLocalsToTrack();

        for (unsigned lclNum = 0; lclNum < m_compiler->lvaCount; lclNum++)
        {
            LocalUses* uses = m_uses[lclNum];
            if (uses == nullptr)
            {
                continue;
            }

#ifdef DEBUG
            if (m_compiler->verbose)
            {
                uses->DumpAccesses(lclNum);
            }
#endif

            totalNumPromotions += uses->PickPromotions(m_compiler, lclNum, aggregates);

            if (totalNumPromotions >= maxTotalNumPromotions)
            {
                JITDUMP("Promoted %d fields which is over our limit of %d; will not promote more\n", totalNumPromotions,
                        maxTotalNumPromotions);
                break;
            }
        }

        if ((m_candidateStores.Height() > 0) && (totalNumPromotions < maxTotalNumPromotions))
        {
            // Now look for induced accesses due to store decomposition.

            JITDUMP("Looking for induced accesses with %d stores between candidates\n", m_candidateStores.Height());
            // Expand the set of fields iteratively based on the current picked
            // set. We put a limit on this fixpoint computation to avoid
            // pathological cases. From measurements no methods in our own
            // collections need more than 10 iterations and 99.5% of methods
            // need fewer than 5 iterations.
            for (int iters = 0; iters < 10; iters++)
            {
                for (int i = 0; i < m_candidateStores.Height(); i++)
                {
                    const CandidateStore& candidateStore = m_candidateStores.BottomRef(i);
                    GenTreeLclVarCommon*  store          = candidateStore.Store;

                    assert(store->TypeIs(TYP_STRUCT));
                    assert(store->Data()->gtEffectiveVal()->OperIsLocalRead());

                    GenTreeLclVarCommon* src = store->Data()->gtEffectiveVal()->AsLclVarCommon();

                    LclVarDsc* dstDsc = m_compiler->lvaGetDesc(store);
                    LclVarDsc* srcDsc = m_compiler->lvaGetDesc(src);

                    assert(Promotion::IsCandidateForPhysicalPromotion(dstDsc) ||
                           Promotion::IsCandidateForPhysicalPromotion(srcDsc));

                    if (dstDsc->lvPromoted)
                    {
                        InduceAccessesFromRegularlyPromotedStruct(aggregates, src, store, candidateStore.Block);
                    }
                    else if (srcDsc->lvPromoted)
                    {
                        InduceAccessesFromRegularlyPromotedStruct(aggregates, store, src, candidateStore.Block);
                    }
                    else
                    {
                        if (Promotion::IsCandidateForPhysicalPromotion(dstDsc))
                        {
                            InduceAccessesInCandidate(aggregates, store, src, candidateStore.Block);
                        }

                        if (Promotion::IsCandidateForPhysicalPromotion(srcDsc))
                        {
                            InduceAccessesInCandidate(aggregates, src, store, candidateStore.Block);
                        }
                    }
                }

                bool again = false;
                for (unsigned lclNum = 0; lclNum < m_compiler->lvaCount; lclNum++)
                {
                    LocalUses* uses = m_uses[lclNum];
                    if (uses == nullptr)
                    {
                        continue;
                    }
#ifdef DEBUG
                    if (m_compiler->verbose)
                    {
                        uses->DumpInducedAccesses(lclNum);
                    }
#endif

                    int numInducedProms = uses->PickInducedPromotions(m_compiler, lclNum, aggregates);
                    again |= numInducedProms > 0;

                    totalNumPromotions += numInducedProms;
                    if (totalNumPromotions >= maxTotalNumPromotions)
                    {
                        JITDUMP("Promoted %d fields and our limit is %d; will not promote more\n", totalNumPromotions,
                                maxTotalNumPromotions);
                        again = false;
                        break;
                    }
                }

                if (!again)
                {
                    break;
                }

                for (unsigned lclNum = 0; lclNum < m_compiler->lvaCount; lclNum++)
                {
                    if (m_uses[lclNum] != nullptr)
                    {
                        m_uses[lclNum]->ClearInducedAccesses();
                    }
                }
            }
        }

        m_compiler->Metrics.PhysicallyPromotedFields += totalNumPromotions;

        if (totalNumPromotions <= 0)
        {
            return false;
        }

        for (AggregateInfo* agg : aggregates)
        {
            jitstd::vector<Replacement>& reps = agg->Replacements;

            assert(reps.size() > 0);
            // Create locals
            for (Replacement& rep : reps)
            {
#ifdef DEBUG
                rep.Description = m_compiler->printfAlloc("V%02u.[%03u..%03u)", agg->LclNum, rep.Offset,
                                                          rep.Offset + genTypeSize(rep.AccessType));
#endif

                rep.LclNum     = m_compiler->lvaGrabTemp(false DEBUGARG(rep.Description));
                LclVarDsc* dsc = m_compiler->lvaGetDesc(rep.LclNum);
                dsc->lvType    = rep.AccessType;

                // Are we promoting Span<>._length field?
                if ((rep.Offset == OFFSETOF__CORINFO_Span__length) && (rep.AccessType == TYP_INT) &&
                    m_compiler->lvaGetDesc(agg->LclNum)->IsSpan())
                {
                    dsc->SetIsNeverNegative(true);
                }
            }

#ifdef DEBUG
            JITDUMP("V%02u promoted with %d replacements\n", agg->LclNum, (int)reps.size());
            for (const Replacement& rep : reps)
            {
                JITDUMP("  [%03u..%03u) promoted as %s V%02u\n", rep.Offset, rep.Offset + genTypeSize(rep.AccessType),
                        varTypeName(rep.AccessType), rep.LclNum);
            }
#endif

            agg->Unpromoted = m_compiler->lvaGetDesc(agg->LclNum)->GetLayout()->GetNonPadding(m_compiler);
            for (Replacement& rep : reps)
            {
                agg->Unpromoted.Subtract(SegmentList::Segment(rep.Offset, rep.Offset + genTypeSize(rep.AccessType)));
            }

            JITDUMP("  Unpromoted remainder: ");
            DBEXEC(m_compiler->verbose, agg->Unpromoted.Dump());
            JITDUMP("\n\n");

            SegmentList::Segment unpromotedSegment;
            if (agg->Unpromoted.CoveringSegment(&unpromotedSegment))
            {
                agg->UnpromotedMin = unpromotedSegment.Start;
                agg->UnpromotedMax = unpromotedSegment.End;
                assert(unpromotedSegment.Start < unpromotedSegment.End);
            }
            else
            {
                // Aggregate is fully promoted, leave UnpromotedMin == UnpromotedMax to indicate this.
            }
        }

        return true;
    }

private:
    //------------------------------------------------------------------------
    // GetOrCreateUses:
    //   Get the uses information for a local. Create it if it does not already exist.
    //
    // Parameters:
    //   lclNum - The local
    //
    // Returns:
    //   Uses information.
    //
    LocalUses* GetOrCreateUses(unsigned lclNum)
    {
        if (m_uses[lclNum] == nullptr)
        {
            m_uses[lclNum] = new (m_compiler, CMK_Promotion) LocalUses(m_compiler);
        }

        return m_uses[lclNum];
    }

    //------------------------------------------------------------------------
    // InduceAccessesFromRegularlyPromotedStruct:
    //   Create induced accesses based on the fact that there is a store
    //   between a physical promotion candidate and regularly promoted struct.
    //
    // Parameters:
    //   aggregates   - Aggregate information with current set of replacements
    //                  for each struct local.
    //   candidateLcl - The local node for a physical promotion candidate.
    //   regPromLcl   - The local node for the regularly promoted struct that
    //                  may induce new LCL_FLD nodes in the candidate.
    //   block        - The block that the store appears in.
    //
    void InduceAccessesFromRegularlyPromotedStruct(AggregateInfoMap&    aggregates,
                                                   GenTreeLclVarCommon* candidateLcl,
                                                   GenTreeLclVarCommon* regPromLcl,
                                                   BasicBlock*          block)
    {
        unsigned regPromOffs   = regPromLcl->GetLclOffs();
        unsigned candidateOffs = candidateLcl->GetLclOffs();
        unsigned size          = regPromLcl->GetLayout(m_compiler)->GetSize();

        LclVarDsc* regPromDsc = m_compiler->lvaGetDesc(regPromLcl);
        for (unsigned fieldLcl = regPromDsc->lvFieldLclStart, i = 0; i < regPromDsc->lvFieldCnt; fieldLcl++, i++)
        {
            LclVarDsc* fieldDsc = m_compiler->lvaGetDesc(fieldLcl);
            if ((fieldDsc->lvFldOffset >= regPromOffs) &&
                (fieldDsc->lvFldOffset + genTypeSize(fieldDsc->lvType) <= (regPromOffs + size)))
            {
                InduceAccess(aggregates, candidateLcl->GetLclNum(),
                             candidateLcl->GetLclOffs() + (fieldDsc->lvFldOffset - regPromOffs), fieldDsc->lvType,
                             block);
            }
        }
    }

    //------------------------------------------------------------------------
    // InduceAccessesInCandidate:
    //   Create induced accesses based on the fact that there is a store
    //   between a candidate and another struct local (the inducer).
    //
    // Parameters:
    //   aggregates - Aggregate information with current set of replacements
    //                for each struct local.
    //   candidate  - The local node for the physical promotion candidate.
    //   inducer    - The local node that may induce new LCL_FLD nodes in the candidate.
    //   block      - The block that the store appears in.
    //
    void InduceAccessesInCandidate(AggregateInfoMap&    aggregates,
                                   GenTreeLclVarCommon* candidate,
                                   GenTreeLclVarCommon* inducer,
                                   BasicBlock*          block)
    {
        unsigned candOffs    = candidate->GetLclOffs();
        unsigned inducerOffs = inducer->GetLclOffs();
        unsigned size        = candidate->GetLayout(m_compiler)->GetSize();

        AggregateInfo* inducerAgg = aggregates.Lookup(inducer->GetLclNum());
        if (inducerAgg != nullptr)
        {
            Replacement* firstRep;
            Replacement* endRep;
            if (inducerAgg->OverlappingReplacements(inducerOffs, size, &firstRep, &endRep))
            {
                for (Replacement* rep = firstRep; rep < endRep; rep++)
                {
                    if ((rep->Offset >= inducerOffs) &&
                        (rep->Offset + genTypeSize(rep->AccessType) <= (inducerOffs + size)))
                    {
                        InduceAccess(aggregates, candidate->GetLclNum(), candOffs + (rep->Offset - inducerOffs),
                                     rep->AccessType, block);
                    }
                }
            }
        }
    }

    //------------------------------------------------------------------------
    // InduceAccess:
    //   Record an induced access in a candidate for physical promotion.
    //
    // Parameters:
    //   aggregates - Aggregate information with current set of replacements
    //                for each struct local.
    //   lclNum     - Local that has the induced access.
    //   offset     - Offset at which the induced access starts.
    //   type       - Type of the induced access.
    //   block      - The block with the induced access.
    //
    void InduceAccess(AggregateInfoMap& aggregates, unsigned lclNum, unsigned offset, var_types type, BasicBlock* block)
    {
        AggregateInfo* agg = aggregates.Lookup(lclNum);
        if (agg != nullptr)
        {
            Replacement* overlapRep;
            if (agg->OverlappingReplacements(offset, genTypeSize(type), &overlapRep, nullptr))
            {
                return;
            }
        }

        LocalUses* uses = GetOrCreateUses(lclNum);
        uses->RecordInducedAccess(offset, type, block->getBBWeight(m_compiler));
    }

    //------------------------------------------------------------------------
    // ClassifyLocalAccess:
    //   Given a local node and its user, classify information about it.
    //
    // Parameters:
    //   lcl - The local
    //   user - The user of the local.
    //
    // Returns:
    //   Flags classifying the access.
    //
    AccessKindFlags ClassifyLocalAccess(GenTreeLclVarCommon* lcl, GenTree* user)
    {
        assert(lcl->OperIsLocalRead() || lcl->OperIsLocalStore());

        AccessKindFlags flags = AccessKindFlags::None;
        if (lcl->OperIsLocalStore())
        {
            INDEBUG(flags |= AccessKindFlags::IsStoreDestination);

            if (lcl->AsLclVarCommon()->Data()->gtEffectiveVal()->IsCall())
            {
                flags |= AccessKindFlags::IsStoredFromCall;
            }
        }

        if (user == nullptr)
        {
            return flags;
        }

        if (user->IsCall())
        {
            GenTreeCall* call = user->AsCall();
            for (CallArg& arg : call->gtArgs.Args())
            {
                if (arg.GetNode()->gtEffectiveVal() != lcl)
                {
                    continue;
                }

                flags |= AccessKindFlags::IsCallArg;

                if (!call->gtArgs.IsAbiInformationDetermined())
                {
                    call->gtArgs.DetermineABIInfo(m_compiler, call);
                }

                if (!arg.AbiInfo.HasAnyStackSegment() && !arg.AbiInfo.IsPassedByReference())
                {
                    flags |= AccessKindFlags::IsRegCallArg;
                }

                break;
            }
        }

#ifdef DEBUG
        if (user->OperIsStore() && (user->Data()->gtEffectiveVal() == lcl))
        {
            flags |= AccessKindFlags::IsStoreSource;
        }

        if (user->OperIs(GT_RETURN, GT_SWIFT_ERROR_RET))
        {
            flags |= AccessKindFlags::IsReturned;
        }
#endif

        return flags;
    }
};

//------------------------------------------------------------------------
// Replacement::Overlaps:
//   Check if this replacement overlaps the specified range.
//
// Parameters:
//   otherStart - Start of the other range.
//   otherSize  - Size of the other range.
//
// Returns:
//    True if they overlap.
//
bool Replacement::Overlaps(unsigned otherStart, unsigned otherSize) const
{
    unsigned end = Offset + genTypeSize(AccessType);
    if (end <= otherStart)
    {
        return false;
    }

    unsigned otherEnd = otherStart + otherSize;
    if (otherEnd <= Offset)
    {
        return false;
    }

    return true;
}

//------------------------------------------------------------------------
// CreateWriteBack:
//   Create IR that writes a replacement local's value back to its struct local:
//
//     STORE_LCL_FLD int V00 [+4]
//       LCL_VAR int V01
//
// Parameters:
//   compiler - Compiler instance
//   structLclNum - Struct local
//   replacement  - Information about the replacement
//
// Returns:
//   IR node.
//
GenTree* Promotion::CreateWriteBack(Compiler* compiler, unsigned structLclNum, const Replacement& replacement)
{
    GenTree* value = compiler->gtNewLclVarNode(replacement.LclNum);
    GenTree* store = compiler->gtNewStoreLclFldNode(structLclNum, replacement.AccessType, replacement.Offset, value);

    if (!compiler->lvaGetDesc(structLclNum)->lvDoNotEnregister)
    {
        compiler->lvaSetVarDoNotEnregister(structLclNum DEBUGARG(DoNotEnregisterReason::LocalField));
    }
    return store;
}

//------------------------------------------------------------------------
// CreateReadBack:
//   Create IR that reads a replacement local's value back from its struct local:
//
//     STORE_LCL_VAR int V01
//       LCL_FLD int V00 [+4]
//
// Parameters:
//   compiler - Compiler instance
//   structLclNum - Struct local
//   replacement  - Information about the replacement
//
// Returns:
//   IR node.
//
GenTree* Promotion::CreateReadBack(Compiler* compiler, unsigned structLclNum, const Replacement& replacement)
{
    GenTree* value = compiler->gtNewLclFldNode(structLclNum, replacement.AccessType, replacement.Offset);
    GenTree* store = compiler->gtNewStoreLclVarNode(replacement.LclNum, value);

    if (!compiler->lvaGetDesc(structLclNum)->lvDoNotEnregister)
    {
        compiler->lvaSetVarDoNotEnregister(structLclNum DEBUGARG(DoNotEnregisterReason::LocalField));
    }
    return store;
}

//------------------------------------------------------------------------
// StartBlock:
//   Handle reaching the end of the currently started block by preparing
//   internal state for upcoming basic blocks, and inserting any necessary
//   readbacks.
//
// Parameters:
//   block - The block
//
// Returns:
//   Statement in block to start from.
//
Statement* ReplaceVisitor::StartBlock(BasicBlock* block)
{
    m_currentBlock = block;

#ifdef DEBUG
    // At the start of every block we expect all replacements to be in their
    // local home.
    for (AggregateInfo* agg : m_aggregates)
    {
        for (Replacement& rep : agg->Replacements)
        {
            assert(!rep.NeedsReadBack);
            assert(rep.NeedsWriteBack);
        }
    }

    assert(m_numPendingReadBacks == 0);
#endif

    // OSR locals and parameters may need an initial read back, which we mark
    // when we start the initial BB.
    if (block != m_compiler->fgFirstBB)
    {
        return block->firstStmt();
    }

    Statement* lastInsertedStmt = nullptr;

    for (AggregateInfo* agg : m_aggregates)
    {
        LclVarDsc* dsc = m_compiler->lvaGetDesc(agg->LclNum);
        if (!dsc->lvIsParam && !dsc->lvIsOSRLocal)
        {
            continue;
        }

        JITDUMP("Processing fields of %s V%02u in entry BB " FMT_BB "\n", dsc->lvIsParam ? "parameter" : "OSR-local",
                agg->LclNum, block->bbNum);

        for (size_t i = 0; i < agg->Replacements.size(); i++)
        {
            Replacement& rep = agg->Replacements[i];
            ClearNeedsWriteBack(rep);
            if (!m_liveness->IsReplacementLiveIn(block, agg->LclNum, (unsigned)i))
            {
                JITDUMP("  V%02u (%s) ignored because it is not live-in to entry BB\n", rep.LclNum, rep.Description);
                continue;
            }

            if (!dsc->lvIsParam ||
                !Promotion::MapsToParameterRegister(m_compiler, agg->LclNum, rep.Offset, rep.AccessType))
            {
                SetNeedsReadBack(rep);
                JITDUMP("  V%02u (%s) marked as needing read back\n", rep.LclNum, rep.Description);
                continue;
            }

            // Insert read backs of parameters mapping to registers eagerly to
            // set the backend up for recognizing these as register accesses.
            GenTree*   readBack = Promotion::CreateReadBack(m_compiler, agg->LclNum, rep);
            Statement* stmt     = m_compiler->fgNewStmtFromTree(readBack);
            JITDUMP("  V%02u (%s) is read back eagerly because it is a register parameter\n", rep.LclNum,
                    rep.Description);
            DISPSTMT(stmt);
            if (lastInsertedStmt == nullptr)
            {
                m_compiler->fgInsertStmtAtBeg(block, stmt);
            }
            else
            {
                m_compiler->fgInsertStmtAfter(block, lastInsertedStmt, stmt);
            }
            lastInsertedStmt = stmt;
        }
    }

    // Skip all the eager read-backs if any were inserted.
    return lastInsertedStmt == nullptr ? block->firstStmt() : lastInsertedStmt->GetNextStmt();
}

//------------------------------------------------------------------------
// EndBlock:
//   Handle reaching the end of the currently started block by preparing
//   internal state for upcoming basic blocks, and inserting any necessary
//   readbacks.
//
// Remarks:
//   We currently expect all fields to be most up-to-date in their field locals
//   at the beginning of every basic block. That means all replacements should
//   have Replacement::NeedsReadBack == false and Replacement::NeedsWriteBack
//   == true at the beginning of every block. This function makes it so that is
//   the case.
//
void ReplaceVisitor::EndBlock()
{
    for (AggregateInfo* agg : m_aggregates)
    {
        for (size_t i = 0; i < agg->Replacements.size(); i++)
        {
            Replacement& rep = agg->Replacements[i];
            assert(!rep.NeedsReadBack || !rep.NeedsWriteBack);
            if (rep.NeedsReadBack)
            {
                if (m_liveness->IsReplacementLiveOut(m_currentBlock, agg->LclNum, (unsigned)i))
                {
                    JITDUMP("Reading back replacement V%02u.[%03u..%03u) -> V%02u near the end of " FMT_BB ":\n",
                            agg->LclNum, rep.Offset, rep.Offset + genTypeSize(rep.AccessType), rep.LclNum,
                            m_currentBlock->bbNum);

                    GenTree*   readBack = Promotion::CreateReadBack(m_compiler, agg->LclNum, rep);
                    Statement* stmt     = m_compiler->fgNewStmtFromTree(readBack);
                    DISPSTMT(stmt);
                    m_compiler->fgInsertStmtNearEnd(m_currentBlock, stmt);
                }
                else
                {
                    // We only mark fields as requiring read-back if they are
                    // live at the point where the stack local was written, so
                    // at first glance we would not expect this case to ever
                    // happen. However, it is possible that the field is live
                    // because it has a future struct use, in which case we may
                    // not need to insert any readbacks anywhere. For example,
                    // consider:
                    //
                    //   V03 = CALL() // V03 is a struct with promoted V03.[000..008)
                    //   CALL(struct V03)    // V03.[000.008) marked as live here
                    //
                    // While V03.[000.008) gets marked for readback at the
                    // store, no readback is necessary at the location of
                    // the call argument, and it may die after that.

                    JITDUMP("Skipping reading back dead replacement V%02u.[%03u..%03u) -> V%02u near the end of " FMT_BB
                            "\n",
                            agg->LclNum, rep.Offset, rep.Offset + genTypeSize(rep.AccessType), rep.LclNum,
                            m_currentBlock->bbNum);
                }

                ClearNeedsReadBack(rep);
            }

            SetNeedsWriteBack(rep);
        }
    }

    assert(m_numPendingReadBacks == 0);
}

//------------------------------------------------------------------------
// StartStatement:
//   Handle starting replacements within a specified statement.
//
// Parameters:
//   stmt - The statement
//
void ReplaceVisitor::StartStatement(Statement* stmt)
{
    m_currentStmt       = stmt;
    m_madeChanges       = false;
    m_mayHaveForwardSub = false;

    InsertPreStatementWriteBacks();
    InsertPreStatementReadBacks();
}

//------------------------------------------------------------------------
// PostOrderVisit:
//   Visit a node in post-order and make necessary changes for promoted field
//   uses.
//
// Parameters:
//   use  - The use edge
//   user - The user
//
// Returns:
//   Visitor result.
//
Compiler::fgWalkResult ReplaceVisitor::PostOrderVisit(GenTree** use, GenTree* user)
{
    GenTree* tree = *use;

    use = InsertMidTreeReadBacks(use);

    if (tree->OperIsStore())
    {
        if (tree->TypeIs(TYP_STRUCT))
        {
            // Struct stores can be decomposed directly into accesses of the replacements.
            HandleStructStore(use, user);
        }
        else if (tree->OperIsLocalStore())
        {
            ReplaceLocal(use, user);
        }

        return fgWalkResult::WALK_CONTINUE;
    }

    if (tree->OperIs(GT_CALL))
    {
        ReadBackAfterCall((*use)->AsCall(), user);
        return fgWalkResult::WALK_CONTINUE;
    }

    if (tree->OperIs(GT_LCL_VAR, GT_LCL_FLD))
    {
        ReplaceLocal(use, user);
        return fgWalkResult::WALK_CONTINUE;
    }

    return fgWalkResult::WALK_CONTINUE;
}

//------------------------------------------------------------------------
// SetNeedsWriteBack:
//   Track that a replacement is more up-to-date in the field local than the
//   struct local.
//
// Remarks:
//   This is usually the case since we generally always keep a field's value in
//   its created primitive local.
//
void ReplaceVisitor::SetNeedsWriteBack(Replacement& rep)
{
    rep.NeedsWriteBack = true;
    assert(!rep.NeedsReadBack);
}

//------------------------------------------------------------------------
// ClearNeedsWriteBack:
//   Track that a replacement is not is more up-to-date in the field local than
//   the struct local.
//
void ReplaceVisitor::ClearNeedsWriteBack(Replacement& rep)
{
    rep.NeedsWriteBack = false;
}

//------------------------------------------------------------------------
// SetNeedsReadBack:
//   Track that a replacement is more up-to-date in the struct local than the
//   field local.
//
// Remarks:
//   This occurs after the struct local is stored in a way that cannot be
//   decomposed directly into stores to field locals; for example because
//   it is passed as a retbuf.
//
void ReplaceVisitor::SetNeedsReadBack(Replacement& rep)
{
    if (rep.NeedsReadBack)
    {
        return;
    }

    rep.NeedsReadBack = true;
    m_numPendingReadBacks++;
}

//------------------------------------------------------------------------
// ClearNeedsReadBack:
//   Track that a replacement is not more up-to-date in the struct local than
//   the field local.
//
void ReplaceVisitor::ClearNeedsReadBack(Replacement& rep)
{
    if (!rep.NeedsReadBack)
    {
        return;
    }

    assert(m_numPendingReadBacks > 0);
    rep.NeedsReadBack = false;
    m_numPendingReadBacks--;
}

//------------------------------------------------------------------------
// InsertPreStatementReadBacks:
//   Insert readbacks before starting the current statement.
//
void ReplaceVisitor::InsertPreStatementReadBacks()
{
    if (m_numPendingReadBacks <= 0)
    {
        return;
    }

    // If we have pending readbacks then insert them as new statements for any
    // local that the statement is using. We could leave this up to ReplaceLocal
    // but do it here for three reasons:
    // 1. For QMARKs we cannot actually leave it up to ReplaceLocal since the
    // local may be conditionally executed
    // 2. This allows forward-sub to kick in
    // 3. Creating embedded stores in ReplaceLocal disables local copy prop for
    //    that local (see ReplaceLocal).

    // Normally, we read back only for the uses we will see. However, with
    // implicit EH flow we may also read back all replacements mid-tree (see
    // InsertMidTreeReadBacks). So for that case we read back everything. This
    // is a correctness requirement for QMARKs, but we do it indiscriminately
    // for the same reasons as mentioned above.
    if (((m_currentStmt->GetRootNode()->gtFlags & (GTF_EXCEPT | GTF_CALL)) != 0) &&
        m_compiler->ehBlockHasExnFlowDsc(m_currentBlock))
    {
        JITDUMP(
            "Reading back pending replacements before statement with possible exception side effect inside block in try region\n");

        for (AggregateInfo* agg : m_aggregates)
        {
            for (Replacement& rep : agg->Replacements)
            {
                InsertPreStatementReadBackIfNecessary(agg->LclNum, rep);
            }
        }
    }
    else
    {
        // Otherwise just read back the locals we see uses of.
        for (GenTreeLclVarCommon* lcl : m_currentStmt->LocalsTreeList())
        {
            if (lcl->TypeIs(TYP_STRUCT))
            {
                continue;
            }

            AggregateInfo* agg = m_aggregates.Lookup(lcl->GetLclNum());
            if (agg == nullptr)
            {
                continue;
            }

            size_t index =
                Promotion::BinarySearch<Replacement, &Replacement::Offset>(agg->Replacements, lcl->GetLclOffs());
            if ((ssize_t)index < 0)
            {
                continue;
            }

            InsertPreStatementReadBackIfNecessary(agg->LclNum, agg->Replacements[index]);
        }
    }
}

//------------------------------------------------------------------------
// InsertPreStatementReadBackIfNecessary:
//   Insert a read back of the specified replacement before the current
//   statement, if the replacement needs it.
//
// Parameters:
//   aggLclNum - Struct local
//   rep       - The replacement
//
void ReplaceVisitor::InsertPreStatementReadBackIfNecessary(unsigned aggLclNum, Replacement& rep)
{
    if (!rep.NeedsReadBack)
    {
        return;
    }

    JITDUMP("Reading back replacement V%02u.[%03u..%03u) -> V%02u before [%06u]:\n", aggLclNum, rep.Offset,
            rep.Offset + genTypeSize(rep.AccessType), rep.LclNum, Compiler::dspTreeID(m_currentStmt->GetRootNode()));

    GenTree*   readBack = Promotion::CreateReadBack(m_compiler, aggLclNum, rep);
    Statement* stmt     = m_compiler->fgNewStmtFromTree(readBack);
    DISPSTMT(stmt);
    m_compiler->fgInsertStmtBefore(m_currentBlock, m_currentStmt, stmt);
    ClearNeedsReadBack(rep);
}

//------------------------------------------------------------------------
// VisitOverlappingReplacements:
//   Call a function for every replacement that overlaps a specified segment.
//
// Parameters:
//   lcl  - The local
//   offs - Start offset of the segment
//   size - Size of the segment
//   func - Callback of type bool(Replacement&). If the callback returns false, the visit aborts.
//
// Return Value:
//   false if the visitor aborted.
//
template <typename Func>
bool ReplaceVisitor::VisitOverlappingReplacements(unsigned lcl, unsigned offs, unsigned size, Func func)
{
    AggregateInfo* agg = m_aggregates.Lookup(lcl);
    if (agg == nullptr)
    {
        return true;
    }

    jitstd::vector<Replacement>& replacements = agg->Replacements;
    size_t                       index = Promotion::BinarySearch<Replacement, &Replacement::Offset>(replacements, offs);

    if ((ssize_t)index < 0)
    {
        index = ~index;
        if ((index > 0) && replacements[index - 1].Overlaps(offs, size))
        {
            index--;
        }
    }

    unsigned end = offs + size;
    while ((index < replacements.size()) && (replacements[index].Offset < end))
    {
        Replacement& rep = replacements[index];
        if (!func(rep))
        {
            return false;
        }

        index++;
    }

    return true;
}

//------------------------------------------------------------------------
// InsertPreStatementWriteBacks:
//   Write back promoted fields for the upcoming statement if it may be
//   beneficial to do so.
//
void ReplaceVisitor::InsertPreStatementWriteBacks()
{
    GenTree* rootNode = m_currentStmt->GetRootNode();
    if ((rootNode->gtFlags & GTF_CALL) == 0)
    {
        return;
    }

    class Visitor : public GenTreeVisitor<Visitor>
    {
        ReplaceVisitor* m_replacer;

    public:
        enum
        {
            DoPreOrder = true,
        };

        Visitor(Compiler* comp, ReplaceVisitor* replacer)
            : GenTreeVisitor(comp)
            , m_replacer(replacer)
        {
        }

        fgWalkResult PreOrderVisit(GenTree** use, GenTree* user)
        {
            GenTree* node = *use;
            if ((node->gtFlags & GTF_CALL) == 0)
            {
                return fgWalkResult::WALK_SKIP_SUBTREES;
            }

            if (node->IsCall())
            {
                GenTreeCall* call = node->AsCall();
                for (CallArg& arg : call->gtArgs.Args())
                {
                    GenTree* node = arg.GetNode()->gtEffectiveVal();
                    if (!node->TypeIs(TYP_STRUCT) || !node->OperIsLocalRead() ||
                        (m_replacer->m_aggregates.Lookup(node->AsLclVarCommon()->GetLclNum()) == nullptr))
                    {
                        continue;
                    }

                    if (m_replacer->CanReplaceCallArgWithFieldListOfReplacements(call, &arg, node->AsLclVarCommon()))
                    {
                        // Register arg that can be decomposed into FIELD_LIST.
                        continue;
                    }

                    GenTreeLclVarCommon* lcl = node->AsLclVarCommon();
                    m_replacer->WriteBackBeforeCurrentStatement(lcl->GetLclNum(), lcl->GetLclOffs(),
                                                                lcl->GetLayout(m_compiler)->GetSize());
                }
            }

            return fgWalkResult::WALK_CONTINUE;
        }
    };

    Visitor visitor(m_compiler, this);
    visitor.WalkTree(m_currentStmt->GetRootNodePointer(), nullptr);
}

//------------------------------------------------------------------------
// InsertMidTreeReadBacks:
//   If necessary, insert IR to read back all replacements before the specified use.
//
// Parameters:
//   use - The use
//
// Returns:
//   New use pointing to the old tree.
//
// Remarks:
//   When a struct field is most up-to-date in its struct local it is marked to
//   need a read back. We then need to decide when to insert IR to read it back
//   to its field local.
//
//   We normally do this before the first use of the field we find, or before
//   we transfer control to any successor. This method handles the case of
//   implicit control flow related to EH; when this basic block is in a
//   try-region (or filter block) and we find a tree that may throw it eagerly
//   inserts pending readbacks.
//
GenTree** ReplaceVisitor::InsertMidTreeReadBacks(GenTree** use)
{
    if ((m_numPendingReadBacks == 0) || !m_compiler->ehBlockHasExnFlowDsc(m_currentBlock))
    {
        return use;
    }

    if (((*use)->gtFlags & (GTF_EXCEPT | GTF_CALL)) == 0)
    {
        assert(!(*use)->OperMayThrow(m_compiler));
        return use;
    }

    if (!(*use)->OperMayThrow(m_compiler))
    {
        return use;
    }

    JITDUMP("Reading back pending replacements before tree with possible exception side effect inside block in try "
            "region\n");

    for (AggregateInfo* agg : m_aggregates)
    {
        for (Replacement& rep : agg->Replacements)
        {
            if (!rep.NeedsReadBack)
            {
                continue;
            }

            JITDUMP("  V%02u.[%03u..%03u) -> V%02u\n", agg->LclNum, rep.Offset,
                    rep.Offset + genTypeSize(rep.AccessType), rep.LclNum);

            ClearNeedsReadBack(rep);
            GenTree* readBack = Promotion::CreateReadBack(m_compiler, agg->LclNum, rep);
            *use =
                m_compiler->gtNewOperNode(GT_COMMA, (*use)->IsValue() ? (*use)->TypeGet() : TYP_VOID, readBack, *use);
            use           = &(*use)->AsOp()->gtOp2;
            m_madeChanges = true;
        }
    }

    assert(m_numPendingReadBacks == 0);
    return use;
}

//------------------------------------------------------------------------
// ReplaceStructLocal:
//   Try to replace a promoted struct local with uses of its fields.
//
// Parameters:
//   user  - The user
//   value - The struct local
//
// Returns:
//   True if the local was replaced and no more work needs to be done; false if
//   the use will need to be handled via write-backs.
//
// Remarks:
//   Usually this amounts to replacing the struct local by a FIELD_LIST with
//   the promoted fields, but merged returns require more complicated handling.
//
bool ReplaceVisitor::ReplaceStructLocal(GenTree* user, GenTreeLclVarCommon* value)
{
    if (user->IsCall())
    {
        return ReplaceCallArgWithFieldList(user->AsCall(), value);
    }
    else
    {
        assert(user->OperIs(GT_RETURN, GT_SWIFT_ERROR_RET));
        return ReplaceReturnedStructLocal(user->AsOp(), value);
    }
}

//------------------------------------------------------------------------
// ReplaceReturnedStructLocal:
//   Try to replace a returned promoted struct local.
//
// Parameters:
//   ret   - The return node
//   value - The struct local
//
// Returns:
//   True if the local was used and no more work needs to be done; false if the
//   use will need to be handled via write-backs.
//
// Remarks:
//   The backend supports arbitrary FIELD_LIST for returns, i.e. there is no
//   requirement that the fields map cleanly to registers. However, morph does
//   not support introducing a returned FIELD_LIST in cases where returns are
//   being merged. Due to that, and for CQ, we instead decompose a store to the
//   return local for that case.
//
bool ReplaceVisitor::ReplaceReturnedStructLocal(GenTreeOp* ret, GenTreeLclVarCommon* value)
{
    if (m_compiler->genReturnLocal != BAD_VAR_NUM)
    {
        JITDUMP("Replacing merged return by store to merged return local\n");
        // If we have merged returns then replace with a store to the return
        // local, and switch out the GT_RETURN to return that local.
        GenTree* sideEffects = nullptr;
        m_compiler->gtExtractSideEffList(ret, &sideEffects, GTF_SIDE_EFFECT, true);
        m_currentStmt->SetRootNode(sideEffects == nullptr ? m_compiler->gtNewNothingNode() : sideEffects);
        DISPSTMT(m_currentStmt);
        m_madeChanges = true;

        GenTree*   store     = m_compiler->gtNewStoreLclVarNode(m_compiler->genReturnLocal, value);
        Statement* storeStmt = m_compiler->fgNewStmtFromTree(store);
        m_compiler->fgInsertStmtAfter(m_currentBlock, m_currentStmt, storeStmt);
        DISPSTMT(storeStmt);

        ret->SetReturnValue(m_compiler->gtNewLclVarNode(m_compiler->genReturnLocal));
        Statement* retStmt = m_compiler->fgNewStmtFromTree(ret);
        m_compiler->fgInsertStmtAfter(m_currentBlock, storeStmt, retStmt);
        DISPSTMT(retStmt);

        return true;
    }

    AggregateInfo* agg    = m_aggregates.Lookup(value->GetLclNum());
    ClassLayout*   layout = value->GetLayout(m_compiler);
    assert(layout != nullptr);

    unsigned startOffset     = value->GetLclOffs();
    unsigned returnValueSize = layout->GetSize();
    if (agg->Unpromoted.Intersects(SegmentList::Segment(startOffset, startOffset + returnValueSize)))
    {
        // TODO-CQ: We could handle cases where the intersected remainder is simple
        return false;
    }

    auto checkPartialOverlap = [=](Replacement& rep) {
        bool contained =
            (rep.Offset >= startOffset) && (rep.Offset + genTypeSize(rep.AccessType) <= startOffset + returnValueSize);

        if (contained)
        {
            // Keep visiting overlapping replacements
            return true;
        }

        // Partial overlap, abort the visit and give up
        return false;
    };

    if (!VisitOverlappingReplacements(value->GetLclNum(), startOffset, returnValueSize, checkPartialOverlap))
    {
        return false;
    }

    StructDeaths      deaths    = m_liveness->GetDeathsForStructLocal(value);
    GenTreeFieldList* fieldList = m_compiler->gtNewFieldList();

    auto addField = [=](Replacement& rep) {
        GenTree* fieldValue;
        if (!rep.NeedsReadBack)
        {
            fieldValue = m_compiler->gtNewLclvNode(rep.LclNum, rep.AccessType);

            assert(deaths.IsReplacementDying(static_cast<unsigned>(&rep - agg->Replacements.data())));
            fieldValue->gtFlags |= GTF_VAR_DEATH;
            CheckForwardSubForLastUse(rep.LclNum);
        }
        else
        {
            // Replacement local is not up to date.
            fieldValue = m_compiler->gtNewLclFldNode(value->GetLclNum(), rep.AccessType, rep.Offset);

            if (!m_compiler->lvaGetDesc(value->GetLclNum())->lvDoNotEnregister)
            {
                m_compiler->lvaSetVarDoNotEnregister(value->GetLclNum() DEBUGARG(DoNotEnregisterReason::LocalField));
            }
        }

        fieldList->AddField(m_compiler, fieldValue, rep.Offset - startOffset, rep.AccessType);

        return true;
    };

    VisitOverlappingReplacements(value->GetLclNum(), startOffset, returnValueSize, addField);

    ret->SetReturnValue(fieldList);

    m_madeChanges = true;
    return true;
}

//------------------------------------------------------------------------
// ReplaceCallArgWithFieldList:
//   Handle a call that may pass a struct local with replacements as the
//   retbuf.
//
// Parameters:
//   call    - The call
//   argNode - The argument node
//
bool ReplaceVisitor::ReplaceCallArgWithFieldList(GenTreeCall* call, GenTreeLclVarCommon* argNode)
{
    CallArg* callArg = call->gtArgs.FindByNode(argNode);
    if (callArg == nullptr)
    {
        // TODO-CQ: Could be wrapped in a comma? Does this happen?
        return false;
    }

    if (!CanReplaceCallArgWithFieldListOfReplacements(call, callArg, argNode))
    {
        return false;
    }

    AggregateInfo* agg    = m_aggregates.Lookup(argNode->GetLclNum());
    ClassLayout*   layout = argNode->GetLayout(m_compiler);
    assert(layout != nullptr);
    StructDeaths      deaths    = m_liveness->GetDeathsForStructLocal(argNode);
    GenTreeFieldList* fieldList = m_compiler->gtNewFieldList();
    for (const ABIPassingSegment& seg : callArg->AbiInfo.Segments())
    {
        Replacement* rep = nullptr;
        if (agg->OverlappingReplacements(argNode->GetLclOffs() + seg.Offset, seg.Size, &rep, nullptr) &&
            !rep->NeedsReadBack)
        {
            GenTreeLclVar* fieldValue = m_compiler->gtNewLclvNode(rep->LclNum, rep->AccessType);

            if (deaths.IsReplacementDying(static_cast<unsigned>(rep - agg->Replacements.data())))
            {
                fieldValue->gtFlags |= GTF_VAR_DEATH;
                CheckForwardSubForLastUse(rep->LclNum);
            }

            fieldList->AddField(m_compiler, fieldValue, seg.Offset, rep->AccessType);
        }
        else
        {
            // Unpromoted part, or replacement local is not up to date.
            var_types type;
            if (rep != nullptr)
            {
                type = rep->AccessType;
            }
            else if (genIsValidFloatReg(seg.GetRegister()))
            {
                type = seg.GetRegisterType();
            }
            else
            {
                if ((seg.Offset % TARGET_POINTER_SIZE) == 0 && (seg.Size == TARGET_POINTER_SIZE))
                {
                    type = layout->GetGCPtrType(seg.Offset / TARGET_POINTER_SIZE);
                }
                else
                {
                    type = seg.GetRegisterType();
                }
            }

            GenTree* fieldValue =
                m_compiler->gtNewLclFldNode(argNode->GetLclNum(), type, argNode->GetLclOffs() + seg.Offset);
            fieldList->AddField(m_compiler, fieldValue, seg.Offset, type);

            if (!m_compiler->lvaGetDesc(argNode->GetLclNum())->lvDoNotEnregister)
            {
                m_compiler->lvaSetVarDoNotEnregister(argNode->GetLclNum() DEBUGARG(DoNotEnregisterReason::LocalField));
            }
        }
    }

    assert(callArg->GetEarlyNode() == argNode);
    callArg->SetEarlyNode(fieldList);

    m_madeChanges = true;
    return true;
}

//------------------------------------------------------------------------
// CanReplaceCallArgWithFieldListOfReplacements:
//   Returns true if a struct arg is replaceable by a FIELD_LIST containing
//   some replacement field.
//
// Parameters:
//   call    - The call
//   callArg - The call argument
//   lcl     - The local that is the node of the call argument
//
// Returns:
//   True if the arg can be replaced by a FIELD_LIST that contains at least one
//   replacement.
//
bool ReplaceVisitor::CanReplaceCallArgWithFieldListOfReplacements(GenTreeCall*         call,
                                                                  CallArg*             callArg,
                                                                  GenTreeLclVarCommon* lcl)
{
    // We should have computed ABI information during the costing phase.
    assert(call->gtArgs.IsAbiInformationDetermined());

    if (callArg->AbiInfo.HasAnyStackSegment() || callArg->AbiInfo.IsPassedByReference())
    {
        return false;
    }

    AggregateInfo* agg = m_aggregates.Lookup(lcl->GetLclNum());
    assert(agg != nullptr);

    bool anyReplacements = false;
    for (const ABIPassingSegment& seg : callArg->AbiInfo.Segments())
    {
        assert(seg.IsPassedInRegister());

        auto callback = [=, &anyReplacements, &seg](Replacement& rep) {
            anyReplacements = true;

            // Replacement must start at the right offset...
            if (rep.Offset != lcl->GetLclOffs() + seg.Offset)
            {
                return false;
            }

            // It must not be too long..
            unsigned repSize = genTypeSize(rep.AccessType);
            if (repSize > seg.Size)
            {
                return false;
            }

            // If it is too short, the remainder that would be passed in the
            // register should be padding. We can check that by only checking
            // whether the remainder intersects anything unpromoted, since if
            // the remainder is a different promotion we will return false when
            // the replacement is visited in this callback.
            if ((repSize < seg.Size) &&
                agg->Unpromoted.Intersects(SegmentList::Segment(rep.Offset + repSize, rep.Offset + seg.Size)))
            {
                return false;
            }

            return true;
        };

        if (!VisitOverlappingReplacements(lcl->GetLclNum(), lcl->GetLclOffs() + seg.Offset, seg.Size, callback))
        {
            return false;
        }
    }

    return anyReplacements;
}

//------------------------------------------------------------------------
// ReadBackAfterCall:
//   Handle a call that may  pass a struct local with replacements as the
//   retbuf.
//
// Parameters:
//   call - The call
//   user - The user of the call.
//
void ReplaceVisitor::ReadBackAfterCall(GenTreeCall* call, GenTree* user)
{
    if (!call->IsOptimizingRetBufAsLocal())
    {
        return;
    }

    CallArg* retBufArg = call->gtArgs.GetRetBufferArg();
    assert(retBufArg != nullptr);
    assert(retBufArg->GetNode()->OperIs(GT_LCL_ADDR));
    GenTreeLclVarCommon* retBufLcl = retBufArg->GetNode()->AsLclVarCommon();
    unsigned             size      = m_compiler->typGetObjLayout(call->gtRetClsHnd)->GetSize();

    MarkForReadBack(retBufLcl, size DEBUGARG("used as retbuf"));
}

//------------------------------------------------------------------------
// IsPromotedStructLocalDying:
//   Check if a promoted struct local is dying at its current position.
//
// Parameters:
//   lcl - The local
//
// Returns:
//   True if so.
//
// Remarks:
//   This effectively translates our precise liveness information for struct
//   uses into the liveness information that the rest of the JIT expects.
//
//   If the remainder of the struct local is dying, then we expect that this
//   entire struct local is now dying, since all field accesses are going to be
//   replaced with other locals.
//
//   There are two exceptions to the above:
//
//     1) If there is a queued readback for any of the fields, then there is
//     live state in the struct local, so it is not dying.
//
//     2) If there are further uses of the local in the same statement then we cannot
//     actually act on the last-use information we would provide here. That's because
//     uses of locals occur at the user and we do not model that here. In the real model
//     there are cases where we do not have any place to insert any IR between the two uses.
//     For example, consider:
//
//       ▌  CALL      void   Program:Foo(Program+S,Program+S)
//       ├──▌  LCL_VAR   struct<Program+S, 4> V01 loc0
//       └──▌  LCL_VAR   struct<Program+S, 4> V01 loc0
//
//     If V01 is promoted fully then both uses of V01 are last uses here; but
//     replacing the IR with
//
//       ▌  CALL      void   Program:Foo(Program+S,Program+S)
//       ├──▌  LCL_VAR   struct<Program+S, 4> V01 loc0          (last use)
//       └──▌  COMMA     struct
//          ├──▌  STORE_LCL_FLD int    V01 loc0         [+0]
//          │  └──▌  LCL_VAR   int    V02 tmp0
//          └──▌  LCL_VAR   struct<Program+S, 4> V01 loc0          (last use)
//
//     would be illegal since the created store overlaps with the first local,
//     and does not take into account that both uses occur simultaneously at
//     the position of the CALL node.
//
bool ReplaceVisitor::IsPromotedStructLocalDying(GenTreeLclVarCommon* lcl)
{
    StructDeaths deaths = m_liveness->GetDeathsForStructLocal(lcl);
    if (!deaths.IsRemainderDying())
    {
        return false;
    }

    AggregateInfo* agg = m_aggregates.Lookup(lcl->GetLclNum());
    assert(agg != nullptr);

    for (Replacement& rep : agg->Replacements)
    {
        if (rep.NeedsReadBack)
        {
            return false;
        }
    }

    for (GenTree* cur = lcl->gtNext; cur != nullptr; cur = cur->gtNext)
    {
        assert(cur->OperIsAnyLocal());
        if (cur->TypeIs(TYP_STRUCT) && (cur->AsLclVarCommon()->GetLclNum() == lcl->GetLclNum()))
        {
            return false;
        }
    }

    return true;
}

//------------------------------------------------------------------------
// ReplaceLocal:
//   Handle a local that may need to be replaced.
//
// Parameters:
//   use - The use of the local
//   user - The user of the local.
//
// Notes:
//   This usually amounts to making a replacement like
//
//       LCL_FLD int V00 [+8] -> LCL_VAR int V10.
//
//  In some cases we may have a pending read back, meaning that the
//  replacement local is out-of-date compared to the struct local.
//  In that case we also need to insert IR to read it back.
//  This happens for example if the struct local was just stored from a
//  call or via a block copy.
//
void ReplaceVisitor::ReplaceLocal(GenTree** use, GenTree* user)
{
    GenTreeLclVarCommon* lcl    = (*use)->AsLclVarCommon();
    unsigned             lclNum = lcl->GetLclNum();
    AggregateInfo*       agg    = m_aggregates.Lookup(lclNum);
    if (agg == nullptr)
    {
        return;
    }

    jitstd::vector<Replacement>& replacements = agg->Replacements;

    unsigned  offs       = lcl->GetLclOffs();
    var_types accessType = lcl->TypeGet();

    if (accessType == TYP_STRUCT)
    {
        // We only expect to see struct uses here that need replacement. Struct
        // stores need to go through decomposition.
        assert(lcl->OperIsLocalRead());

        GenTree* effectiveUser = user;
        if ((user != nullptr) && user->OperIs(GT_COMMA))
        {
            effectiveUser = Promotion::EffectiveUser(m_ancestors);
        }

        if (effectiveUser == nullptr)
        {
            return;
        }

        if (effectiveUser->OperIsStore())
        {
            // Source of store. Will be handled by decomposition when we get to
            // the store, so we should not introduce any writebacks.
            assert(effectiveUser->Data()->gtEffectiveVal() == lcl);
            return;
        }

        JITDUMP("Processing struct use [%06u] of V%02u.[%03u..%03u)\n", Compiler::dspTreeID(lcl), lclNum, offs,
                offs + lcl->GetLayout(m_compiler)->GetSize());

        assert(effectiveUser->OperIs(GT_CALL, GT_RETURN, GT_SWIFT_ERROR_RET));

        if (!ReplaceStructLocal(effectiveUser, lcl))
        {
            unsigned size = lcl->GetLayout(m_compiler)->GetSize();
            WriteBackBeforeUse(use, lclNum, lcl->GetLclOffs(), size);

            if (IsPromotedStructLocalDying(lcl))
            {
                lcl->gtFlags |= GTF_VAR_DEATH;
                CheckForwardSubForLastUse(lclNum);

                // Relying on the values in the struct local after this struct use
                // would effectively introduce another use of the struct, so
                // indicate that no replacements are up to date.
                for (Replacement& rep : replacements)
                {
                    SetNeedsWriteBack(rep);
                }
            }
        }

        return;
    }

#ifdef DEBUG
    unsigned accessSize = genTypeSize(accessType);
    for (const Replacement& rep : replacements)
    {
        assert(!rep.Overlaps(offs, accessSize) || ((rep.Offset == offs) && (rep.AccessType == accessType)));
    }

    JITDUMP("Processing primitive use [%06u] of V%02u.[%03u..%03u)\n", Compiler::dspTreeID(lcl), lclNum, offs,
            offs + accessSize);
#endif

    size_t index = Promotion::BinarySearch<Replacement, &Replacement::Offset>(replacements, offs);
    if ((ssize_t)index < 0)
    {
        // Access that we don't have a replacement for.
        return;
    }

    Replacement& rep = replacements[index];
    assert(accessType == rep.AccessType);

    bool isDef = lcl->OperIsLocalStore();

    if (isDef)
    {
        *use = m_compiler->gtNewStoreLclVarNode(rep.LclNum, lcl->Data());
    }
    else
    {
        *use = m_compiler->gtNewLclvNode(rep.LclNum, accessType);
    }

    if ((lcl->gtFlags & GTF_VAR_DEATH) != 0)
    {
        (*use)->gtFlags |= GTF_VAR_DEATH;
        CheckForwardSubForLastUse(rep.LclNum);
    }

    if (isDef)
    {
        ClearNeedsReadBack(rep);
        SetNeedsWriteBack(rep);
    }
    else if (rep.NeedsReadBack)
    {
        // This is an uncommon case -- typically all readbacks are handled in
        // InsertPreStatementReadBacks. This case is still needed to handle the
        // situation where the readback was marked previously in this tree
        // (e.g. due to a COMMA).

        JITDUMP("  ..needs a read back\n");
        *use = m_compiler->gtNewOperNode(GT_COMMA, (*use)->TypeGet(),
                                         Promotion::CreateReadBack(m_compiler, lclNum, rep), *use);
        ClearNeedsReadBack(rep);

        // TODO: Local copy prop does not take into account that the
        // uses of LCL_VAR occur at the user, which means it may introduce
        // illegally overlapping lifetimes, such as:
        //
        // └──▌  ADD       int
        //    ├──▌  LCL_VAR   int    V10 tmp6        -> copy propagated to [V35 tmp31]
        //    └──▌  COMMA     int
        //       ├──▌  STORE_LCL_VAR int    V35 tmp31
        //       │  └──▌  LCL_FLD   int    V03 loc1         [+4]
        //
        // This really ought to be handled by local copy prop, but the way it works during
        // morph makes it hard to fix there.
        //
        // This is the short term fix. Long term fixes may be:
        // 1. Fix local copy prop
        // 2. Teach LSRA to allow the above cases, simplifying IR concepts (e.g.
        //    introduce something like GT_COPY on top of LCL_VAR when they
        //    need to be "defs")
        // 3. Change the pass here to avoid creating any embedded stores by making use
        //    of gtSplitTree. We will only need to split in very edge cases since the point
        //    at which the replacement was marked as needing read back is practically always
        //    going to be in a previous statement, so this shouldn't be too bad for CQ.

        m_compiler->lvaGetDesc(rep.LclNum)->lvRedefinedInEmbeddedStatement = true;
    }

    JITDUMP("  ..replaced with V%02u\n", rep.LclNum);

    m_madeChanges = true;
}

//------------------------------------------------------------------------
// CheckForwardSubForLastUse:
//   Indicate that a local has a last use in the current statement and that
//   there thus may be a forward substitution opportunity.
//
// Parameters:
//   lclNum - The local number with a last use in this statement.
//
void ReplaceVisitor::CheckForwardSubForLastUse(unsigned lclNum)
{
    if (m_currentBlock->firstStmt() == m_currentStmt)
    {
        return;
    }

    Statement* prevStmt = m_currentStmt->GetPrevStmt();
    GenTree*   prevNode = prevStmt->GetRootNode();

    if (prevNode->OperIsLocalStore() && (prevNode->AsLclVarCommon()->GetLclNum() == lclNum))
    {
        m_mayHaveForwardSub = true;
    }
}

//------------------------------------------------------------------------
// WriteBackBeforeCurrentStatement:
//   Insert statements before the current that write back all overlapping
//   replacements.
//
// Parameters:
//   lcl  - The struct local
//   offs - The starting offset into the struct local of the overlapping range to write back to
//   size - The size of the overlapping range
//
void ReplaceVisitor::WriteBackBeforeCurrentStatement(unsigned lcl, unsigned offs, unsigned size)
{
    VisitOverlappingReplacements(lcl, offs, size, [this, lcl](Replacement& rep) {
        if (!rep.NeedsWriteBack)
        {
            return true;
        }

        GenTree*   readBack = Promotion::CreateWriteBack(m_compiler, lcl, rep);
        Statement* stmt     = m_compiler->fgNewStmtFromTree(readBack);
        JITDUMP("Writing back %s before " FMT_STMT "\n", rep.Description, m_currentStmt->GetID());
        DISPSTMT(stmt);
        m_compiler->fgInsertStmtBefore(m_currentBlock, m_currentStmt, stmt);
        ClearNeedsWriteBack(rep);
        return true;
    });
}

//------------------------------------------------------------------------
// WriteBackBeforeUse:
//   Update the use with IR that writes back all necessary overlapping
//   replacements into a struct local.
//
// Parameters:
//   use  - The use, which will be updated with a cascading comma trees of stores
//   lcl  - The struct local
//   offs - The starting offset into the struct local of the overlapping range to write back to
//   size - The size of the overlapping range
//
void ReplaceVisitor::WriteBackBeforeUse(GenTree** use, unsigned lcl, unsigned offs, unsigned size)
{
    VisitOverlappingReplacements(lcl, offs, size, [this, &use, lcl](Replacement& rep) {
        if (!rep.NeedsWriteBack)
        {
            return true;
        }

        GenTreeOp* comma = m_compiler->gtNewOperNode(GT_COMMA, (*use)->TypeGet(),
                                                     Promotion::CreateWriteBack(m_compiler, lcl, rep), *use);
        *use             = comma;
        use              = &comma->gtOp2;

        ClearNeedsWriteBack(rep);
        m_madeChanges = true;
        return true;
    });
}

//------------------------------------------------------------------------
// MarkForReadBack:
//   Mark that replacements in the specified struct local need to be read
//   back before their next use.
//
// Parameters:
//   lcl    - Local node. Its offset is the start of the range.
//   size   - The size of the range
//   reason - The reason the readback is required
//
void ReplaceVisitor::MarkForReadBack(GenTreeLclVarCommon* lcl, unsigned size DEBUGARG(const char* reason))
{
    // We currently do not handle readbacks marked within a QMARK arm, but we
    // never create this case and we expect to expand QMARKs in an earlier pass
    // in the (relative) near future.
    assert(m_compiler->fgGetTopLevelQmark(m_currentStmt->GetRootNode()) == nullptr);

    AggregateInfo* agg = m_aggregates.Lookup(lcl->GetLclNum());
    if (agg == nullptr)
    {
        return;
    }

    unsigned                     offs         = lcl->GetLclOffs();
    jitstd::vector<Replacement>& replacements = agg->Replacements;
    size_t                       index = Promotion::BinarySearch<Replacement, &Replacement::Offset>(replacements, offs);

    if ((ssize_t)index < 0)
    {
        index = ~index;
        if ((index > 0) && replacements[index - 1].Overlaps(offs, size))
        {
            index--;
        }
    }

    unsigned end = offs + size;
    if ((index >= replacements.size()) || (replacements[index].Offset >= end))
    {
        // No overlap with any field.
        return;
    }

    StructDeaths deaths = m_liveness->GetDeathsForStructLocal(lcl);
    JITDUMP("Fields of [%06u] in range [%03u..%03u) need to be read back: %s\n", Compiler::dspTreeID(lcl), offs,
            offs + size, reason);

    do
    {
        Replacement& rep = replacements[index];
        assert(rep.Overlaps(offs, size));

        if (deaths.IsReplacementDying((unsigned)index))
        {
            JITDUMP("  V%02u (%s) not marked (is dying)\n", rep.LclNum, rep.Description);
        }
        else
        {
            SetNeedsReadBack(rep);
            JITDUMP("  V%02u (%s) marked\n", rep.LclNum, rep.Description);
        }

        ClearNeedsWriteBack(rep);

        index++;
    } while ((index < replacements.size()) && (replacements[index].Offset < end));
}

//------------------------------------------------------------------------
// Promotion::Run:
//   Run the promotion phase.
//
// Returns:
//   Suitable phase status.
//
PhaseStatus Promotion::Run()
{
    if (!HaveCandidateLocals())
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

    // First collect information about uses of locals
    LocalsUseVisitor localsUse(this);
    for (BasicBlock* bb : m_compiler->Blocks())
    {
        localsUse.SetBB(bb);

        for (Statement* stmt : bb->Statements())
        {
            for (GenTreeLclVarCommon* lcl : stmt->LocalsTreeList())
            {
                if (Promotion::IsCandidateForPhysicalPromotion(m_compiler->lvaGetDesc(lcl)))
                {
                    localsUse.WalkTree(stmt->GetRootNodePointer(), nullptr);
                    break;
                }
            }
        }
    }

    // Pick promotions based on the use information we just collected.
    AggregateInfoMap aggregates(m_compiler->getAllocator(CMK_Promotion), m_compiler->lvaCount);
    if (!localsUse.PickPromotions(aggregates))
    {
        // No promotions picked.
        return PhaseStatus::MODIFIED_NOTHING;
    }

    // We should have a proper entry BB where we can put IR that will only be
    // run once into. This is a precondition of the phase.
    assert(m_compiler->fgFirstBB->bbPreds == nullptr);

    // Compute liveness for the fields and remainders.
    PromotionLiveness liveness(m_compiler, aggregates);
    liveness.Run();

    JITDUMP("Making replacements\n\n");

    // Make all replacements we decided on.
    ReplaceVisitor replacer(this, aggregates, &liveness);
    for (BasicBlock* bb : m_compiler->Blocks())
    {
        Statement* firstStmt = replacer.StartBlock(bb);

        JITDUMP("\nReplacing in ");
        DBEXEC(m_compiler->verbose, bb->dspBlockHeader());
        JITDUMP("\n");

        for (Statement* stmt : StatementList(firstStmt))
        {
            replacer.StartStatement(stmt);

            DISPSTMT(stmt);

            replacer.WalkTree(stmt->GetRootNodePointer(), nullptr);

            if (replacer.MadeChanges())
            {
                m_compiler->fgSequenceLocals(stmt);
                m_compiler->gtUpdateStmtSideEffects(stmt);
                JITDUMP("New statement:\n");
                DISPSTMT(stmt);
            }

            if (replacer.MayHaveForwardSubOpportunity())
            {
                JITDUMP("Invoking forward sub due to a potential opportunity\n");
                while ((stmt != bb->firstStmt()) && m_compiler->fgForwardSubStatement(stmt->GetPrevStmt()))
                {
                    m_compiler->fgRemoveStmt(bb, stmt->GetPrevStmt());
                }
            }
        }

        replacer.EndBlock();
    }

    // Add necessary explicit zeroing for some locals.
    Statement* prevStmt = nullptr;
    for (AggregateInfo* agg : aggregates)
    {
        LclVarDsc* dsc = m_compiler->lvaGetDesc(agg->LclNum);
        if (dsc->lvSuppressedZeroInit)
        {
            // We may have suppressed inserting an explicit zero init based on the
            // assumption that the entire local will be zero inited in the prolog.
            // Now that we are promoting some fields that assumption may be
            // invalidated for those fields, and we may need to insert explicit
            // zero inits again.
            ExplicitlyZeroInitReplacementLocals(agg->LclNum, agg->Replacements, &prevStmt);
        }
    }

    return PhaseStatus::MODIFIED_EVERYTHING;
}

//------------------------------------------------------------------------
// Promotion::HaveCandidateLocals:
//   Check if there are any locals that are candidates for physical promotion.
//
// Returns:
//   True if so.
//
bool Promotion::HaveCandidateLocals()
{
    for (unsigned lclNum = 0; lclNum < m_compiler->lvaCount; lclNum++)
    {
        if (IsCandidateForPhysicalPromotion(m_compiler->lvaGetDesc(lclNum)))
        {
            return true;
        }
    }

    return false;
}

//------------------------------------------------------------------------
// Promotion::IsCandidateForPhysicalPromotion:
//   Check if a specified local is a candidate for physical promotion.
//
// Returns:
//   True if so.
//
bool Promotion::IsCandidateForPhysicalPromotion(LclVarDsc* dsc)
{
    return dsc->TypeIs(TYP_STRUCT) && !dsc->lvPromoted && !dsc->IsAddressExposed();
}

//------------------------------------------------------------------------
// Promotion::EffectiveUser:
//   Find the effective user given an ancestor stack.
//
// Returns:
//   The user, or null if all users are commas.
//
GenTree* Promotion::EffectiveUser(Compiler::GenTreeStack& ancestors)
{
    int userIndex = 1;
    while (userIndex < ancestors.Height())
    {
        GenTree* ancestor = ancestors.Top(userIndex);
        GenTree* child    = ancestors.Top(userIndex - 1);

        if (!ancestor->OperIs(GT_COMMA) || (ancestor->gtGetOp2() != child))
        {
            return ancestor;
        }

        userIndex++;
    }

    return nullptr;
}

//------------------------------------------------------------------------
// MapsToParameterRegister:
//   Check if a specific access in the specified parameter local is
//   expected to map to a register.
//
// Parameters:
//   comp       - Compiler instance
//   lclNum     - Local being accessed into
//   offset     - Offset being accessed at
//   accessType - Type of access
//
// Returns:
//   True if the access can be efficiently done via a parameter register.
//
bool Promotion::MapsToParameterRegister(Compiler* comp, unsigned lclNum, unsigned offset, var_types accessType)
{
    assert(lclNum < comp->info.compArgsCount);

    if (comp->opts.IsOSR())
    {
        return false;
    }

    const ABIPassingInformation& abiInfo = comp->lvaGetParameterABIInfo(lclNum);
    if (abiInfo.IsPassedByReference() || abiInfo.HasAnyStackSegment())
    {
        return false;
    }

    for (const ABIPassingSegment& seg : abiInfo.Segments())
    {
        if ((offset == seg.Offset) && (genTypeSize(accessType) == seg.Size) &&
            (varTypeUsesIntReg(accessType) == genIsValidIntReg(seg.GetRegister())))
        {
            return true;
        }
    }

    return false;
}

// Promotion::ExplicitlyZeroInitReplacementLocals:
//   Insert IR to zero out replacement locals if necessary.
//
// Parameters:
//   lclNum       - The struct local
//   replacements - Replacements for the struct local
//   prevStmt     - [in, out] Previous statement to insert after
//
void Promotion::ExplicitlyZeroInitReplacementLocals(unsigned                           lclNum,
                                                    const jitstd::vector<Replacement>& replacements,
                                                    Statement**                        prevStmt)
{
    for (unsigned i = 0; i < replacements.size(); i++)
    {
        const Replacement& rep = replacements[i];

        if (!m_compiler->fgVarNeedsExplicitZeroInit(rep.LclNum, false, false))
        {
            // Other downstream code (e.g. recursive-tailcalls-to-loops opt) may
            // still need to insert further explicit zero initing.
            m_compiler->lvaGetDesc(rep.LclNum)->lvSuppressedZeroInit = true;
            continue;
        }

        GenTree* value = m_compiler->gtNewZeroConNode(rep.AccessType);
        GenTree* store = m_compiler->gtNewStoreLclVarNode(rep.LclNum, value);
        InsertInitStatement(prevStmt, store);
    }
}

//------------------------------------------------------------------------
// Promotion::InsertInitStatement:
//   Insert a new statement after the specified statement in the entry block,
//   or at the beginning of the entry block if no other statements were
//   inserted yet.
//
// Parameters:
//   prevStmt - [in, out] Previous statement to insert after
//   tree     - Tree to create statement from
//
void Promotion::InsertInitStatement(Statement** prevStmt, GenTree* tree)
{
    Statement* stmt = m_compiler->fgNewStmtFromTree(tree);
    if (*prevStmt != nullptr)
    {
        m_compiler->fgInsertStmtAfter(m_compiler->fgFirstBB, *prevStmt, stmt);
    }
    else
    {
        m_compiler->fgInsertStmtAtBeg(m_compiler->fgFirstBB, stmt);
    }

    *prevStmt = stmt;
}
