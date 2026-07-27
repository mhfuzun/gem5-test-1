#include <algorithm>

#include "bpu_ittage.hh"

bpu_ittage::bpu_ittage()
{
    reset();
}

void
bpu_ittage::reset()
{
    table.assign(set_count, std::vector<entry_t>(way_count));
    replacement.reset(way_count);
    replacement_states.assign(set_count, replacement.new_state());
    checkpoints.clear();
    ghr = 0;
    next_checkpoint_id = 0;
}

std::uint64_t
bpu_ittage::ghr_mask() const
{
    return (std::uint64_t{1} << history_bits) - 1;
}

int
bpu_ittage::index(int pc) const
{
    return index(pc, ghr);
}

int
bpu_ittage::index(int pc, std::uint64_t history) const
{
    std::uint64_t hash = static_cast<std::uint64_t>(pc >> inst_shift);
    hash ^= history;
    hash ^= (history << 5);
    hash ^= (hash >> 11);
    return static_cast<int>(hash & (set_count - 1));
}

std::uint64_t
bpu_ittage::tag(int pc) const
{
    return tag(pc, ghr);
}

std::uint64_t
bpu_ittage::tag(int pc, std::uint64_t history) const
{
    std::uint64_t hash = static_cast<std::uint64_t>(pc >> inst_shift);
    hash ^= (history << 1);
    hash ^= (history >> 3);
    return hash & ((std::uint64_t{1} << tag_bits) - 1);
}

void
bpu_ittage::update_history(bool taken)
{
    ghr = ((ghr << 1) | (taken ? 1 : 0)) & ghr_mask();
}

ittage_response_t
bpu_ittage::lookup(int pc)
{
    checkpoint_t checkpoint;
    checkpoint.id = next_checkpoint_id++;
    checkpoint.pc = pc;
    checkpoint.ghr = ghr;

    const int set = index(pc);
    const std::uint64_t lookup_tag = tag(pc);
    for (std::size_t way = 0; way < table[set].size(); ++way) {
        const entry_t& entry = table[set][way];
        if (!entry.valid || entry.tag != lookup_tag) {
            continue;
        }

        checkpoint.hit = entry.ctr >= 2;
        checkpoint.target = entry.target;
        replacement.touch(replacement_states[set], way);
        break;
    }

    checkpoints.push_back(checkpoint);

    ittage_response_t response;
    response.hit = checkpoint.hit;
    response.target = checkpoint.target;
    response.checkpoint_id = checkpoint.id;

    if (response.hit) {
        update_history(true);
    }

    return response;
}

void
bpu_ittage::clear_speculation()
{
    if (!checkpoints.empty()) {
        ghr = checkpoints.front().ghr;
    }
    checkpoints.clear();
}

void
bpu_ittage::squash_checkpoint(int checkpoint_id, bool include_self)
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

    ghr = first->ghr;
    checkpoints.erase(first, checkpoints.end());
}

bpu_ittage::checkpoint_t*
bpu_ittage::find_checkpoint(int pc)
{
    for (checkpoint_t& checkpoint : checkpoints) {
        if (checkpoint.pc == pc) {
            return &checkpoint;
        }
    }

    return nullptr;
}

void
bpu_ittage::erase_checkpoint_id(int checkpoint_id)
{
    checkpoints.erase(
        std::remove_if(checkpoints.begin(), checkpoints.end(),
            [checkpoint_id](const checkpoint_t& checkpoint) {
                return checkpoint.id == checkpoint_id;
            }),
        checkpoints.end());
}

void
bpu_ittage::record_target(int pc, int target, std::uint64_t history)
{
    const int set = index(pc, history);
    const std::uint64_t lookup_tag = tag(pc, history);

    entry_t* victim = nullptr;
    std::size_t victim_way = 0;
    for (std::size_t way = 0; way < table[set].size(); ++way) {
        entry_t& entry = table[set][way];
        if (entry.valid && entry.tag == lookup_tag) {
            victim = &entry;
            victim_way = way;
            break;
        }
    }

    if (victim == nullptr) {
        for (std::size_t way = 0; way < table[set].size(); ++way) {
            entry_t& entry = table[set][way];
            if (!entry.valid) {
                victim = &entry;
                victim_way = way;
                break;
            }
        }
    }

    if (victim == nullptr) {
        victim_way = replacement.get_lru_and_touch(replacement_states[set]);
        victim = &table[set][victim_way];
    }

    victim->valid = true;
    victim->tag = lookup_tag;
    victim->target = target;
    if (victim->ctr < 3) {
        ++victim->ctr;
    }
    replacement.touch(replacement_states[set], victim_way);
}

bool
bpu_ittage::commit(int pc, int target, bool taken)
{
    checkpoint_t* checkpoint = find_checkpoint(pc);
    bool had_checkpoint = checkpoint != nullptr;
    std::uint64_t checkpoint_ghr = ghr;
    int checkpoint_id = -1;
    bool predicted_hit = false;
    int predicted_target = 0;

    if (checkpoint != nullptr) {
        checkpoint_ghr = checkpoint->ghr;
        checkpoint_id = checkpoint->id;
        predicted_hit = checkpoint->hit;
        predicted_target = checkpoint->target;
    }

    const bool repair_history = checkpoint != nullptr &&
        (!predicted_hit || predicted_target != target || !taken);
    if (repair_history) {
        ghr = checkpoint_ghr;
        checkpoints.erase(
            std::remove_if(checkpoints.begin(), checkpoints.end(),
                [checkpoint_id](const checkpoint_t& checkpoint) {
                    return checkpoint.id > checkpoint_id;
                }),
            checkpoints.end());
    }

    if (taken && (checkpoint || checkpoints.empty())) {
        record_target(pc, target, checkpoint_ghr);
    }

    if (repair_history || (!checkpoint && checkpoints.empty())) {
        update_history(taken);
    }

    if (checkpoint_id >= 0) {
        erase_checkpoint_id(checkpoint_id);
    }

    return had_checkpoint;
}
