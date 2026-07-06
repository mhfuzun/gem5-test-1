/*
 * Custom speculative return-address stack model.
 */

#include "cpu/pred/my_ras.hh"

#include <algorithm>
#include <cassert>

#include "base/logging.hh"
#include "debug/RAS.hh"

namespace gem5
{

namespace branch_prediction
{

void
MyRAS::CyclicPtr::reset(unsigned _depth, int64_t _value)
{
    depth = _depth == 0 ? 1 : _depth;
    value = _value;
}

unsigned
MyRAS::CyclicPtr::get() const
{
    auto idx = value % static_cast<int64_t>(depth);
    if (idx < 0) {
        idx += static_cast<int64_t>(depth);
    }
    return static_cast<unsigned>(idx);
}

MyRAS::CyclicPtr
MyRAS::CyclicPtr::prev() const
{
    CyclicPtr ptr = *this;
    ptr.dec();
    return ptr;
}

MyRAS::CyclicPtr
MyRAS::CyclicPtr::minus(unsigned amount) const
{
    CyclicPtr ptr = *this;
    for (unsigned i = 0; i < amount; ++i) {
        ptr.dec();
    }
    return ptr;
}

bool
MyRAS::CyclicPtr::sameSlot(const CyclicPtr &other) const
{
    return get() == other.get();
}

bool
MyRAS::CyclicPtr::sameTurn(const CyclicPtr &other) const
{
    return value == other.value;
}

MyRAS::MyRAS(const Params &p)
    : ReturnAddrStack(p),
      rasEntries(p.numEntries),
      branchEntries(p.branchEntries),
      stateTableCapacity(p.branchEntries + 1),
      numThreads(p.numThreads),
      overflowRepair(p.overflowRepair),
      resetOnUnrecoverable(p.resetOnUnrecoverable),
      myStats(this)
{
    if (rasEntries == 0) {
        fatal("MyRAS requires numEntries > 0");
    }

    stacks.reserve(numThreads);
    for (unsigned i = 0; i < numThreads; ++i) {
        stacks.push_back(makeThreadRAS());
    }
}

MyRAS::StateEntry
MyRAS::makeEmptyEntry() const
{
    StateEntry entry;
    entry.idx.reset(rasEntries, 0);
    entry.wptr.reset(rasEntries, 0);
    entry.rptr.reset(rasEntries, -1);
    return entry;
}

MyRAS::ThreadRAS
MyRAS::makeThreadRAS() const
{
    ThreadRAS stack;
    stack.addrTable.resize(rasEntries);
    stack.stateTable.resize(stateTableCapacity);
    stack.addrTableWritePtr.reset(rasEntries, 0);
    reset(stack);
    return stack;
}

void
MyRAS::reset(ThreadRAS &stack) const
{
    for (auto &entry : stack.addrTable) {
        entry.reset();
    }

    stack.addrTableWritePtr.reset(rasEntries, 0);
    stack.stateTableReadPtr = 0;
    stack.stateTableResolvedPtr = 0;
    stack.stateTableWritePtr = nextStatePtr(0);
    stack.backUpPtr = 0;
    stack.activeStateCount = 1;
    stack.overflowDebt = 0;

    for (auto &entry : stack.stateTable) {
        entry = makeEmptyEntry();
    }

    auto &base = stack.stateTable[0];
    base.valid = true;
    base.idx.reset(rasEntries, 0);
    base.wptr.reset(rasEntries, 0);
    base.rptr.reset(rasEntries, -1);
    base.depth = 0;
    base.writeDepth = 0;
    base.backup = 0;
    base.backupValid = false;

    updateFlags(stack);
}

void
MyRAS::reset()
{
    DPRINTF(RAS, "MyRAS Reset.\n");
    for (auto &stack : stacks) {
        reset(stack);
    }
}

void
MyRAS::cleanStart(ThreadRAS &stack)
{
    myStats.cleanStarts++;
    reset(stack);
}

void
MyRAS::makeHistory(ThreadRAS &stack, void * &ras_history)
{
    if (ras_history == nullptr) {
        auto *history = new MyRASHistory;
        history->recoverId = snapshot(stack, -1);
        history->snapshotValid = history->recoverId >= 0;
        if (!history->snapshotValid && resetOnUnrecoverable) {
            cleanStart(stack);
        }
        ras_history = static_cast<void *>(history);
    }
}

int
MyRAS::snapshot(ThreadRAS &stack, int bid)
{
    if (stack.stateFull) {
        myStats.snapshotOverflows++;
        return -1;
    }

    const unsigned recover_id = currentStateId(stack);
    const unsigned new_id = stack.stateTableWritePtr;
    const auto &prev = stack.stateTable[recover_id];
    auto &next = stack.stateTable[new_id];

    stack.stateTable[recover_id].bid = bid;
    stack.stateTable[recover_id].resolved = false;

    next = makeEmptyEntry();
    next.valid = true;
    next.idx = stack.addrTableWritePtr;
    next.wptr = stack.addrTableWritePtr;
    next.depth = prev.depth;
    next.writeDepth = 0;

    if (prev.writeDepth == 0) {
        next.rptr = prev.rptr;
        next.backup = prev.backup;
        next.backupValid = prev.backupValid;
    } else {
        next.rptr = stack.addrTableWritePtr.prev();
        next.backup = recover_id;
        next.backupValid = true;
    }

    stack.stateTableWritePtr = nextStatePtr(stack.stateTableWritePtr);
    ++stack.activeStateCount;
    syncGlobalBackup(stack);
    updateFlags(stack);
    myStats.snapshots++;
    return static_cast<int>(recover_id);
}

bool
MyRAS::recover(ThreadRAS &stack, int rid)
{
    if (rid < 0 || !stateIsLive(stack, static_cast<unsigned>(rid))) {
        myStats.recoverFailures++;
        return false;
    }

    const auto id = static_cast<unsigned>(rid);
    invalidateYoungerThan(stack, id);
    stack.stateTableWritePtr = nextStatePtr(id);
    stack.activeStateCount = stateDistance(stack.stateTableReadPtr, id) + 1;

    auto &state = stack.stateTable[id];
    state.bid = -1;
    state.resolved = false;
    stack.addrTableWritePtr = state.wptr;
    syncGlobalBackup(stack);

    myStats.recoveries++;
    eachCycle(stack);
    updateFlags(stack);
    return true;
}

bool
MyRAS::resolve(ThreadRAS &stack, int rid)
{
    if (rid < 0 || !stateIsLive(stack, static_cast<unsigned>(rid))) {
        myStats.resolveFailures++;
        return false;
    }

    const auto id = static_cast<unsigned>(rid);
    if (id == currentStateId(stack)) {
        return false;
    }

    stack.stateTable[id].resolved = true;
    stack.stateTableResolvedPtr = id;
    myStats.resolves++;
    eachCycle(stack);
    updateFlags(stack);
    return true;
}

void
MyRAS::eachCycle(ThreadRAS &stack)
{
    while (stack.activeStateCount > 1 &&
           stack.stateTable[stack.stateTableReadPtr].resolved &&
           !stateIsReferencedAsBackup(stack, stack.stateTableReadPtr)) {
        stack.stateTable[stack.stateTableReadPtr] = makeEmptyEntry();
        stack.stateTableReadPtr = nextStatePtr(stack.stateTableReadPtr);
        --stack.activeStateCount;
    }

    syncGlobalBackup(stack);
    updateFlags(stack);
}

bool
MyRAS::pushAddr(ThreadRAS &stack, const PCStateBase &pc)
{
    if (stack.rasFull) {
        myStats.pushOverflows++;
        if (overflowRepair) {
            ++stack.overflowDebt;
            myStats.overflowDebtIncrements++;
        }
        return false;
    }

    set(stack.addrTable[stack.addrTableWritePtr.get()], pc);
    stack.addrTableWritePtr.inc();

    auto &state = activeState(stack);
    state.wptr = stack.addrTableWritePtr;
    ++state.depth;
    ++state.writeDepth;

    myStats.pushes++;
    updateFlags(stack);
    return true;
}

const PCStateBase *
MyRAS::popAddr(ThreadRAS &stack, MyRASHistory *history)
{
    if (stack.rasEmpty) {
        if (overflowRepair && stack.overflowDebt > 0) {
            --stack.overflowDebt;
            history->overflowDebtConsumed = true;
            history->providedTarget = false;
            history->rasEntry.reset();
            myStats.popUnderflowRepairs++;
            myStats.overflowDebtConsumes++;
            myStats.targetNotProvidedPops++;
            if (stack.overflowDebt == 0) {
                myStats.overflowDebtClears++;
            }
            return nullptr;
        }

        myStats.popUnderflows++;
        myStats.targetNotProvidedPops++;
        history->rasEntry.reset();
        return nullptr;
    }

    auto &state = activeState(stack);
    CyclicPtr target(rasEntries);

    if (state.writeDepth == 0) {
        target = state.rptr;
        advanceReadChain(stack, state.rptr, state.backup, state.backupValid);
    } else {
        target = stack.addrTableWritePtr.prev();
        stack.addrTableWritePtr.dec();
        state.wptr = stack.addrTableWritePtr;
        --state.writeDepth;
    }

    set(history->rasEntry, stack.addrTable[target.get()].get());
    history->providedTarget = history->rasEntry != nullptr;
    if (history->providedTarget) {
        myStats.targetProvidedPops++;
    } else {
        myStats.targetNotProvidedPops++;
    }
    --state.depth;

    myStats.pops++;
    eachCycle(stack);
    updateFlags(stack);
    return history->rasEntry.get();
}

void
MyRAS::push(ThreadID tid, const PCStateBase &pc, void * &ras_history)
{
    auto &stack = stacks[tid];
    myStats.pushAttempts++;
    makeHistory(stack, ras_history);
    auto *history = static_cast<MyRASHistory *>(ras_history);
    history->wasCall = true;

    if (!history->snapshotValid) {
        history->suppressed = true;
        myStats.suppressedPushes++;
        DPRINTF(RAS, "%s: MyRAS suppress push without snapshot, tid:%i\n",
                __func__, tid);
        return;
    }

    history->pushed = pushAddr(stack, pc);

    DPRINTF(RAS, "%s: MyRAS push %#x, tid:%i\n", __func__, pc.instAddr(),
            tid);
}

const PCStateBase *
MyRAS::pop(ThreadID tid, void * &ras_history)
{
    auto &stack = stacks[tid];
    myStats.popAttempts++;
    makeHistory(stack, ras_history);
    auto *history = static_cast<MyRASHistory *>(ras_history);
    history->wasReturn = true;

    if (!history->snapshotValid) {
        history->suppressed = true;
        myStats.suppressedPops++;
        myStats.targetNotProvidedPops++;
        DPRINTF(RAS, "%s: MyRAS suppress pop without snapshot, tid:%i\n",
                __func__, tid);
        return nullptr;
    }

    history->poped = true;
    const auto *entry = popAddr(stack, history);
    DPRINTF(RAS, "%s: MyRAS pop %#x, tid:%i\n", __func__,
            entry ? entry->instAddr() : 0, tid);
    return entry;
}

void
MyRAS::squash(ThreadID tid, void * &ras_history)
{
    if (ras_history == nullptr) {
        return;
    }

    myStats.squashes++;
    auto *history = static_cast<MyRASHistory *>(ras_history);
    if (history->snapshotValid) {
        const bool recovered = recover(stacks[tid], history->recoverId);
        if (!recovered && resetOnUnrecoverable) {
            cleanStart(stacks[tid]);
        }
    } else {
        myStats.squashesWithoutSnapshot++;
    }
    delete history;
    ras_history = nullptr;
}

void
MyRAS::commit(ThreadID tid, bool misp, const BranchType brType,
              void * &ras_history)
{
    if (!(brType == BranchType::Return ||
          brType == BranchType::CallDirect ||
          brType == BranchType::CallIndirect)) {
        assert(ras_history == nullptr);
        return;
    }

    if (ras_history == nullptr) {
        return;
    }

    auto *history = static_cast<MyRASHistory *>(ras_history);
    if (history->poped) {
        myStats.used++;
        if (history->providedTarget) {
            myStats.committedProvidedReturns++;
        } else {
            myStats.committedNoTargetReturns++;
        }
        if (misp) {
            myStats.incorrect++;
        } else {
            myStats.correct++;
        }
    }

    if (history->snapshotValid) {
        const bool live = stateIsLive(stacks[tid],
            static_cast<unsigned>(history->recoverId));
        const bool current = live &&
            static_cast<unsigned>(history->recoverId) ==
            currentStateId(stacks[tid]);
        const bool resolved = resolve(stacks[tid], history->recoverId);
        if (!resolved && !current && resetOnUnrecoverable) {
            cleanStart(stacks[tid]);
        }
    }
    delete history;
    ras_history = nullptr;
}

unsigned
MyRAS::nextStatePtr(unsigned ptr) const
{
    return (ptr + 1) % stateTableCapacity;
}

unsigned
MyRAS::prevStatePtr(unsigned ptr) const
{
    return ptr == 0 ? stateTableCapacity - 1 : ptr - 1;
}

unsigned
MyRAS::currentStateId(const ThreadRAS &stack) const
{
    return prevStatePtr(stack.stateTableWritePtr);
}

unsigned
MyRAS::stateDistance(unsigned from, unsigned to) const
{
    if (to >= from) {
        return to - from;
    }
    return (stateTableCapacity - from) + to;
}

MyRAS::StateEntry &
MyRAS::activeState(ThreadRAS &stack)
{
    return stack.stateTable[currentStateId(stack)];
}

const MyRAS::StateEntry &
MyRAS::activeState(const ThreadRAS &stack) const
{
    return stack.stateTable[currentStateId(stack)];
}

bool
MyRAS::stateIsLive(const ThreadRAS &stack, unsigned id) const
{
    if (id >= stateTableCapacity || !stack.stateTable[id].valid) {
        return false;
    }

    unsigned ptr = stack.stateTableReadPtr;
    for (unsigned i = 0; i < stack.activeStateCount; ++i) {
        if (ptr == id) {
            return true;
        }
        ptr = nextStatePtr(ptr);
    }
    return false;
}

bool
MyRAS::stateIsReferencedAsBackup(const ThreadRAS &stack, unsigned id) const
{
    unsigned ptr = stack.stateTableReadPtr;
    for (unsigned i = 0; i < stack.activeStateCount; ++i) {
        if (ptr != currentStateId(stack) && stack.stateTable[ptr].resolved) {
            ptr = nextStatePtr(ptr);
            continue;
        }

        unsigned backup = stack.stateTable[ptr].backup;
        bool backup_valid = stack.stateTable[ptr].backupValid;

        for (unsigned hops = 0; backup_valid && hops < stack.activeStateCount;
             ++hops) {
            if (backup == id) {
                return true;
            }
            if (!stateIsLive(stack, backup)) {
                break;
            }
            backup_valid = stack.stateTable[backup].backupValid;
            backup = stack.stateTable[backup].backup;
        }

        ptr = nextStatePtr(ptr);
    }

    return false;
}

MyRAS::CyclicPtr
MyRAS::backupBoundaryPtr(const StateEntry &state) const
{
    if (state.writeDepth == 0) {
        return state.idx;
    }
    return state.wptr.minus(state.writeDepth);
}

void
MyRAS::syncGlobalBackup(ThreadRAS &stack) const
{
    const auto &state = activeState(stack);
    stack.backUpPtr = state.backupValid ? state.backup : currentStateId(stack);
}

void
MyRAS::updateFlags(ThreadRAS &stack) const
{
    const auto &state = activeState(stack);
    stack.rasEmpty = state.depth == 0;
    stack.rasFull = state.depth >= rasEntries || writeHitsAnyLiveState(stack);
    stack.stateFull = stack.activeStateCount >= stateTableCapacity;
}

void
MyRAS::invalidateYoungerThan(ThreadRAS &stack, unsigned id)
{
    unsigned ptr = nextStatePtr(id);
    while (ptr != stack.stateTableWritePtr) {
        stack.stateTable[ptr] = makeEmptyEntry();
        ptr = nextStatePtr(ptr);
    }
}

void
MyRAS::advanceReadChain(const ThreadRAS &stack, CyclicPtr &rptr,
                        unsigned &backup, bool &backupValid) const
{
    if (backupValid && stateIsLive(stack, backup)) {
        const auto &backup_state = stack.stateTable[backup];
        if (rptr.sameTurn(backupBoundaryPtr(backup_state))) {
            rptr = backup_state.rptr;
            backup = backup_state.backup;
            backupValid = backup_state.backupValid;
            return;
        }
    }

    rptr.dec();
}

bool
MyRAS::writeHitsLiveEntries(const ThreadRAS &stack,
                            const StateEntry &state) const
{
    for (unsigned i = 0; i < state.writeDepth; ++i) {
        const auto ptr = state.wptr.minus(i + 1);
        if (ptr.sameSlot(stack.addrTableWritePtr)) {
            return true;
        }
    }

    auto rptr = state.rptr;
    auto backup = state.backup;
    auto backup_valid = state.backupValid;
    const auto read_depth = state.depth - state.writeDepth;

    for (unsigned i = 0; i < read_depth; ++i) {
        if (rptr.sameSlot(stack.addrTableWritePtr)) {
            return true;
        }
        advanceReadChain(stack, rptr, backup, backup_valid);
    }

    return false;
}

bool
MyRAS::writeHitsAnyLiveState(const ThreadRAS &stack) const
{
    unsigned ptr = stack.stateTableReadPtr;
    for (unsigned i = 0; i < stack.activeStateCount; ++i) {
        if (writeHitsLiveEntries(stack, stack.stateTable[ptr])) {
            return true;
        }
        ptr = nextStatePtr(ptr);
    }
    return false;
}

MyRAS::MyRASStats::MyRASStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(pushes, statistics::units::Count::get(),
               "Number of MyRAS pushes"),
      ADD_STAT(pushOverflows, statistics::units::Count::get(),
               "Number of MyRAS push overflows"),
      ADD_STAT(pops, statistics::units::Count::get(),
               "Number of MyRAS pops"),
      ADD_STAT(popUnderflows, statistics::units::Count::get(),
               "Number of MyRAS pop underflows"),
      ADD_STAT(snapshots, statistics::units::Count::get(),
               "Number of MyRAS snapshots"),
      ADD_STAT(snapshotOverflows, statistics::units::Count::get(),
               "Number of MyRAS snapshot overflows"),
      ADD_STAT(recoveries, statistics::units::Count::get(),
               "Number of MyRAS recoveries"),
      ADD_STAT(resolves, statistics::units::Count::get(),
               "Number of MyRAS resolves"),
      ADD_STAT(squashes, statistics::units::Count::get(),
               "Number of MyRAS squashes"),
      ADD_STAT(used, statistics::units::Count::get(),
               "Number of times MyRAS is the provider"),
      ADD_STAT(correct, statistics::units::Count::get(),
               "Number of correct MyRAS return predictions"),
      ADD_STAT(incorrect, statistics::units::Count::get(),
               "Number of incorrect MyRAS return predictions"),
      ADD_STAT(pushAttempts, statistics::units::Count::get(),
               "Number of MyRAS push attempts before suppression/filtering"),
      ADD_STAT(popAttempts, statistics::units::Count::get(),
               "Number of MyRAS pop attempts before suppression/filtering"),
      ADD_STAT(suppressedPushes, statistics::units::Count::get(),
               "Number of MyRAS pushes suppressed because snapshot failed"),
      ADD_STAT(suppressedPops, statistics::units::Count::get(),
               "Number of MyRAS pops suppressed because snapshot failed"),
      ADD_STAT(targetProvidedPops, statistics::units::Count::get(),
               "Number of MyRAS pops that returned a target"),
      ADD_STAT(targetNotProvidedPops, statistics::units::Count::get(),
               "Number of MyRAS pops that did not return a target"),
      ADD_STAT(popUnderflowRepairs, statistics::units::Count::get(),
               "Number of empty pops consumed by overflow repair debt"),
      ADD_STAT(overflowDebtIncrements, statistics::units::Count::get(),
               "Number of missed-push debt increments from push overflow"),
      ADD_STAT(overflowDebtConsumes, statistics::units::Count::get(),
               "Number of missed-push debt entries consumed by empty pops"),
      ADD_STAT(overflowDebtClears, statistics::units::Count::get(),
               "Number of times overflow repair debt returned to zero"),
      ADD_STAT(recoverFailures, statistics::units::Count::get(),
               "Number of failed MyRAS recover operations"),
      ADD_STAT(resolveFailures, statistics::units::Count::get(),
               "Number of failed MyRAS resolve operations"),
      ADD_STAT(cleanStarts, statistics::units::Count::get(),
               "Number of MyRAS clean-start resets after unrecoverable state"),
      ADD_STAT(squashesWithoutSnapshot, statistics::units::Count::get(),
               "Number of squashes for histories whose snapshot failed"),
      ADD_STAT(committedProvidedReturns, statistics::units::Count::get(),
               "Number of committed return histories that had a MyRAS target"),
      ADD_STAT(committedNoTargetReturns, statistics::units::Count::get(),
               "Number of committed return histories without a MyRAS target")
{
}

} // namespace branch_prediction
} // namespace gem5
