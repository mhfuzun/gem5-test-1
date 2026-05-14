/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#ifndef __CPU_PRED_MY_TAGE_HH__
#define __CPU_PRED_MY_TAGE_HH__

#include <memory>
#include <vector>

#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/myTagePred/include/tage.h"
#include "params/MyTAGE.hh"

namespace gem5
{

namespace branch_prediction
{

/**
 * Minimal skeleton predictor to integrate a custom TAGE backend
 * with gem5's BPredUnit life-cycle.
 *
 * You can fill table/hash/history logic in this class and/or in the
 * myTagePred backend files.
 */
class MyTAGE : public BPredUnit
{
  public:
    explicit MyTAGE(const MyTAGEParams &params);

    void resetStats() override;

    bool lookup(ThreadID tid, Addr pc, void * &bp_history) override;
    void updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                         Addr target, const StaticInstPtr &inst,
                         void * &bp_history) override;
    void update(ThreadID tid, Addr pc, bool taken, void * &bp_history,
                bool squashed, const StaticInstPtr &inst,
                Addr target) override;
    void squash(ThreadID tid, void * &bp_history) override;
    void branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
                           void * &bp_history) override;

  private:
    struct MyTAGEHistory
    {
        bool predTaken = false;
        bool isConditional = true;
        ::tage::TAGE_State tageState;
    };

    static tage_cfg_t buildCfg(const MyTAGEParams &params);
    statistics_t aggregateBackendStats() const;

    struct MyTAGEStats : public statistics::Group
    {
        MyTAGEStats(MyTAGE &parent, unsigned comp_count);

        void regStats() override;
        void preDumpStats() override;

        MyTAGE &myTage;
        const unsigned compCount;

        statistics::Scalar totalBranches;
        statistics::Scalar takenBranches;
        statistics::Scalar missPredictions;
        statistics::Scalar correctPredictions;

        statistics::Scalar bimodalSelections;
        statistics::Scalar providerSelections;
        statistics::Scalar altSelections;
        statistics::Scalar totalAllocations;

        statistics::Scalar bimodalCorrectWhenSelected;
        statistics::Scalar providerCorrectWhenSelected;
        statistics::Scalar altCorrectWhenSelected;

        statistics::Scalar noAllocationBecauseOfHit;
        statistics::Scalar noAllocationBecauseOfNoFree;
        statistics::Scalar noAllocationBecauseOfProviderHigh;
        statistics::Scalar noAllocationButProviderZero;

        statistics::Scalar speculativeHistoryUpdates;
        statistics::Scalar squashHistoryRestores;
        statistics::Scalar mispredictHistoryRepairs;
        statistics::Scalar commitTableUpdates;
        statistics::Scalar placeholdersCreated;

        statistics::Vector providerUseByTable;
        statistics::Vector providerHitByTable;
        statistics::Vector altUseByTable;
        statistics::Vector altHitByTable;
        statistics::Vector allocationByTable;

        statistics::Formula accuracy;
        statistics::Formula mispredictionRate;
        statistics::Formula takenRate;
        statistics::Formula providerSelectionRate;
        statistics::Formula altSelectionRate;
        statistics::Formula bimodalSelectionRate;
        statistics::Formula providerAccuracyWhenSelected;
        statistics::Formula altAccuracyWhenSelected;
        statistics::Formula bimodalAccuracyWhenSelected;
        statistics::Formula allocationsPerMisprediction;
    };

  private:
    const unsigned compCount;
    std::vector<std::unique_ptr<::tage>> backendByThread;
    MyTAGEStats myStats;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_MY_TAGE_HH__
