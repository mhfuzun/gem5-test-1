/*
 * Custom speculative return-address stack model.
 */

#ifndef __CPU_PRED_MY_RAS_HH__
#define __CPU_PRED_MY_RAS_HH__

#include <memory>
#include <vector>

#include "arch/generic/pcstate.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/pred/branch_type.hh"
#include "cpu/pred/ras.hh"
#include "params/MyRAS.hh"

namespace gem5
{

namespace branch_prediction
{

class MyRAS : public ReturnAddrStack
{
  public:
    typedef MyRASParams Params;

    MyRAS(const Params &p);

    void reset() override;
    void push(ThreadID tid, const PCStateBase &pc, void * &ras_history)
        override;
    const PCStateBase* pop(ThreadID tid, void * &ras_history) override;
    void squash(ThreadID tid, void * &ras_history) override;
    void commit(ThreadID tid, bool misp, const BranchType brType,
                void * &ras_history) override;

  private:
    class CyclicPtr
    {
      public:
        CyclicPtr() = default;
        CyclicPtr(unsigned depth, int64_t value = 0) { reset(depth, value); }

        void reset(unsigned depth, int64_t value = 0);
        unsigned get() const;
        void inc() { ++value; }
        void dec() { --value; }
        CyclicPtr prev() const;
        CyclicPtr minus(unsigned amount) const;
        bool sameSlot(const CyclicPtr &other) const;
        bool sameTurn(const CyclicPtr &other) const;

      private:
        unsigned depth = 1;
        int64_t value = 0;
    };

    struct StateEntry
    {
        int bid = -1;
        CyclicPtr idx;
        CyclicPtr wptr;
        CyclicPtr rptr;
        unsigned depth = 0;
        unsigned writeDepth = 0;
        bool valid = false;
        bool resolved = false;
        unsigned backup = 0;
        bool backupValid = false;
    };

    struct ThreadRAS
    {
        std::vector<std::unique_ptr<PCStateBase>> addrTable;
        std::vector<StateEntry> stateTable;
        CyclicPtr addrTableWritePtr;
        unsigned stateTableReadPtr = 0;
        unsigned stateTableWritePtr = 0;
        unsigned stateTableResolvedPtr = 0;
        unsigned backUpPtr = 0;
        unsigned activeStateCount = 0;
        unsigned overflowDebt = 0;
        bool rasFull = false;
        bool rasEmpty = true;
        bool stateFull = false;
    };

    struct MyRASHistory
    {
        bool pushed = false;
        bool poped = false;
        bool wasReturn = false;
        bool wasCall = false;
        bool snapshotValid = false;
        bool suppressed = false;
        bool providedTarget = false;
        bool overflowDebtConsumed = false;
        int recoverId = -1;
        std::unique_ptr<PCStateBase> rasEntry;
    };

    StateEntry makeEmptyEntry() const;
    ThreadRAS makeThreadRAS() const;
    void reset(ThreadRAS &stack) const;
    void cleanStart(ThreadRAS &stack);

    void makeHistory(ThreadRAS &stack, void * &ras_history);
    int snapshot(ThreadRAS &stack, int bid);
    bool recover(ThreadRAS &stack, int rid);
    bool resolve(ThreadRAS &stack, int rid);
    void eachCycle(ThreadRAS &stack);

    bool pushAddr(ThreadRAS &stack, const PCStateBase &pc);
    const PCStateBase *popAddr(ThreadRAS &stack, MyRASHistory *history);

    unsigned nextStatePtr(unsigned ptr) const;
    unsigned prevStatePtr(unsigned ptr) const;
    unsigned currentStateId(const ThreadRAS &stack) const;
    unsigned stateDistance(unsigned from, unsigned to) const;

    StateEntry &activeState(ThreadRAS &stack);
    const StateEntry &activeState(const ThreadRAS &stack) const;
    bool stateIsLive(const ThreadRAS &stack, unsigned id) const;
    bool stateIsReferencedAsBackup(const ThreadRAS &stack, unsigned id) const;
    CyclicPtr backupBoundaryPtr(const StateEntry &state) const;
    void syncGlobalBackup(ThreadRAS &stack) const;
    void updateFlags(ThreadRAS &stack) const;
    void invalidateYoungerThan(ThreadRAS &stack, unsigned id);
    void advanceReadChain(const ThreadRAS &stack, CyclicPtr &rptr,
                          unsigned &backup, bool &backupValid) const;
    bool writeHitsLiveEntries(const ThreadRAS &stack,
                              const StateEntry &state) const;
    bool writeHitsAnyLiveState(const ThreadRAS &stack) const;

    unsigned rasEntries;
    unsigned branchEntries;
    unsigned stateTableCapacity;
    unsigned numThreads;
    bool overflowRepair;
    bool resetOnUnrecoverable;
    std::vector<ThreadRAS> stacks;

    struct MyRASStats : public statistics::Group
    {
        MyRASStats(statistics::Group *parent);
        statistics::Scalar pushes;
        statistics::Scalar pushOverflows;
        statistics::Scalar pops;
        statistics::Scalar popUnderflows;
        statistics::Scalar snapshots;
        statistics::Scalar snapshotOverflows;
        statistics::Scalar recoveries;
        statistics::Scalar resolves;
        statistics::Scalar squashes;
        statistics::Scalar used;
        statistics::Scalar correct;
        statistics::Scalar incorrect;
        statistics::Scalar pushAttempts;
        statistics::Scalar popAttempts;
        statistics::Scalar suppressedPushes;
        statistics::Scalar suppressedPops;
        statistics::Scalar targetProvidedPops;
        statistics::Scalar targetNotProvidedPops;
        statistics::Scalar popUnderflowRepairs;
        statistics::Scalar overflowDebtIncrements;
        statistics::Scalar overflowDebtConsumes;
        statistics::Scalar overflowDebtClears;
        statistics::Scalar recoverFailures;
        statistics::Scalar resolveFailures;
        statistics::Scalar cleanStarts;
        statistics::Scalar squashesWithoutSnapshot;
        statistics::Scalar committedProvidedReturns;
        statistics::Scalar committedNoTargetReturns;
    } myStats;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_MY_RAS_HH__
