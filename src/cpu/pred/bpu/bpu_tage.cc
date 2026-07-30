#include <algorithm>
#include <array>

#include "bpu_tage.hh"

bpu_tage::bpu_tage()
    : cfg(make_default_cfg()),
      predictor(std::make_unique<tage>(cfg))
{
}

tage_cfg_t
bpu_tage::make_default_cfg()
{
    static constexpr std::array<int, 8> histories =
        {5, 8, 13, 21, 34, 55, 89, 128};
    static constexpr std::array<int, 8> tags =
        {8, 8, 9, 9, 10, 10, 11, 12};

    tage_cfg_t cfg{};
    cfg.bimodal_depth = 2048;
    cfg.ghistory_length = 128;
    cfg.pchistory_length = 16;
    cfg.comp_count = static_cast<int>(histories.size());
    cfg.pc_hash_start_for_idx = 2;
    cfg.pc_hash_width_for_idx = 10;
    cfg.pc_hash_start_for_tag = 10;
    cfg.pc_hash_width_for_tag = 12;
    cfg.allocate_randomplacement = true;
    cfg.allocate_longerThanProvider = true;
    cfg.periodicreset = true;
    cfg.periodicreset_branchperiod = 1 << 18;
    cfg.random_type = LFSR;
    cfg.lfsr_width = 16;
    cfg.lfsr_seed = 0xace1;
    cfg.lfsr_misprediction_update = true;

    cfg.table_cfg.resize(histories.size());
    for (std::size_t i = 0; i < histories.size(); ++i) {
        table_cfg_t table{};
        table.depth = 1024;
        table.usefull_width = 2;
        table.ctr_width = 3;
        table.tag_width = tags[i];
        table.history_width = histories[i];
        table.pchistory_start = 0;
        table.pchistory_width = std::min(16, histories[i]);
        cfg.table_cfg[i] = table;
    }

    return cfg;
}

void
bpu_tage::reset()
{
    predictor = std::make_unique<tage>(cfg);
    checkpoints.clear();
    next_checkpoint_id = 0;
}

tage_response_t
bpu_tage::predict(const std::vector<tage_lookup_slot_t>& slots)
{
    tage_response_t response;
    response.valid = true;
    response.slots = slots;
    response.slot_valid.resize(slots.size(), false);
    response.predictions.resize(slots.size(), false);
    response.checkpoint_ids.resize(slots.size(), -1);

    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].valid) {
            continue;
        }

        checkpoint_t checkpoint;
        checkpoint.id = next_checkpoint_id++;
        checkpoint.pc = slots[i].pc;
        checkpoint.prediction =
            predictor->getPrediction(static_cast<addr_t>(slots[i].pc),
                                     checkpoint.state);
        predictor->updateHist(static_cast<addr_t>(slots[i].pc),
                              checkpoint.prediction, checkpoint.state);

        response.slot_valid[i] = true;
        response.predictions[i] = checkpoint.prediction;
        response.checkpoint_ids[i] = checkpoint.id;
        checkpoints.push_back(checkpoint);
        trim_checkpoints();
    }

    return response;
}

std::vector<int>
bpu_tage::keep_path(const tage_response_t& response, int stop_offset_2b,
                    bool has_stop)
{
    std::vector<int> kept_ids;
    int first_squashed_checkpoint = -1;

    for (std::size_t i = 0; i < response.slots.size(); ++i) {
        const bool valid = i < response.slot_valid.size() &&
            response.slot_valid[i] &&
            i < response.checkpoint_ids.size() &&
            response.checkpoint_ids[i] >= 0;
        if (!valid) {
            continue;
        }

        const bool after_stop =
            has_stop && response.slots[i].sign.offset > stop_offset_2b;
        if (after_stop) {
            if (first_squashed_checkpoint < 0) {
                first_squashed_checkpoint = response.checkpoint_ids[i];
            }
        } else {
            kept_ids.push_back(response.checkpoint_ids[i]);
        }
    }

    if (first_squashed_checkpoint >= 0) {
        squash_checkpoint(first_squashed_checkpoint, true);
    }

    return kept_ids;
}

void
bpu_tage::discard_response(const tage_response_t& response)
{
    int first_checkpoint = -1;
    for (int checkpoint_id : response.checkpoint_ids) {
        if (checkpoint_id >= 0 &&
            (first_checkpoint < 0 || checkpoint_id < first_checkpoint)) {
            first_checkpoint = checkpoint_id;
        }
    }

    if (first_checkpoint >= 0) {
        squash_checkpoint(first_checkpoint, true);
    }
}

void
bpu_tage::clear_speculation()
{
    if (!checkpoints.empty()) {
        predictor->resetState(checkpoints.front().state);
    }
    checkpoints.clear();
}

void
bpu_tage::squash_checkpoint(int checkpoint_id, bool include_self)
{
    if (checkpoint_id < 0) {
        return;
    }

    const auto should_squash =
        [checkpoint_id, include_self](const checkpoint_t& checkpoint) {
            return include_self ?
                checkpoint.id >= checkpoint_id :
                checkpoint.id > checkpoint_id;
        };

    auto first = std::find_if(checkpoints.begin(), checkpoints.end(),
                              should_squash);
    if (first == checkpoints.end()) {
        return;
    }

    predictor->resetState(first->state);
    checkpoints.erase(first, checkpoints.end());
}

bpu_tage::checkpoint_t*
bpu_tage::find_checkpoint(bpu_addr_t pc)
{
    for (checkpoint_t& checkpoint : checkpoints) {
        if (checkpoint.pc == pc) {
            return &checkpoint;
        }
    }

    return nullptr;
}

bpu_tage::checkpoint_t*
bpu_tage::find_checkpoint_id(int checkpoint_id)
{
    for (checkpoint_t& checkpoint : checkpoints) {
        if (checkpoint.id == checkpoint_id) {
            return &checkpoint;
        }
    }

    return nullptr;
}

void
bpu_tage::erase_checkpoint_id(int checkpoint_id)
{
    checkpoints.erase(
        std::remove_if(checkpoints.begin(), checkpoints.end(),
            [checkpoint_id](const checkpoint_t& checkpoint) {
                return checkpoint.id == checkpoint_id;
            }),
        checkpoints.end());
}

void
bpu_tage::trim_checkpoints()
{
    while (checkpoints.size() > max_checkpoint_count) {
        checkpoints.pop_front();
    }
}

std::size_t
bpu_tage::checkpoint_count() const
{
    return checkpoints.size();
}

bool
bpu_tage::commit(bpu_addr_t pc, bool taken)
{
    checkpoint_t* checkpoint = find_checkpoint(pc);
    if (checkpoint == nullptr) {
        tage::TAGE_State state;
        predictor->getPrediction(static_cast<addr_t>(pc), state);
        predictor->update(static_cast<addr_t>(pc), taken, state);
        if (checkpoints.empty()) {
            predictor->updateHist(static_cast<addr_t>(pc), taken, state);
        }
        return false;
    }

    const int checkpoint_id = checkpoint->id;
    const bool prediction = checkpoint->prediction;
    const tage::TAGE_State state = checkpoint->state;

    if (prediction != taken) {
        predictor->resetState(state);
        predictor->updateHist(static_cast<addr_t>(pc), taken, state);
        checkpoints.erase(
            std::remove_if(checkpoints.begin(), checkpoints.end(),
                [checkpoint_id](const checkpoint_t& checkpoint) {
                    return checkpoint.id > checkpoint_id;
                }),
            checkpoints.end());
    }

    predictor->update(static_cast<addr_t>(pc), taken, state);
    erase_checkpoint_id(checkpoint_id);
    return true;
}

bool
bpu_tage::commit_checkpoint(int checkpoint_id, bpu_addr_t pc, bool taken)
{
    if (checkpoint_id < 0) {
        return commit(pc, taken);
    }

    checkpoint_t* checkpoint = find_checkpoint_id(checkpoint_id);
    if (checkpoint == nullptr) {
        return commit(pc, taken);
    }

    const bool prediction = checkpoint->prediction;
    const tage::TAGE_State state = checkpoint->state;

    if (prediction != taken) {
        predictor->resetState(state);
        predictor->updateHist(static_cast<addr_t>(pc), taken, state);
        checkpoints.erase(
            std::remove_if(checkpoints.begin(), checkpoints.end(),
                [checkpoint_id](const checkpoint_t& checkpoint) {
                    return checkpoint.id > checkpoint_id;
                }),
            checkpoints.end());
    }

    predictor->update(static_cast<addr_t>(pc), taken, state);
    erase_checkpoint_id(checkpoint_id);
    return true;
}
