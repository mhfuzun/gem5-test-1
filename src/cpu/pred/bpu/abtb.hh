#pragma once

#include <vector>

#include "bpu_structs.hh"

class abtb
{
public:
    struct abtb_entry_t
    {
        bool valid = false;
        bpu_tag_t tag = 0;
        bpu_addr_t target_addr = 0;
        bpu_addr_t next_cfi_addr = 0;
        int next_cfi_span_2b = 0;
        // dominant cfi entry target
        // bank offset
        bpu_sign_t cfi_entry;
    };

    abtb(abtb_cfg cfg = {});

    bpu_tag_t getAbtbTag(bpu_addr_t addr) const;

    abtb_entry_t lookup(bpu_addr_t addr) const;
    btb_response_t predict(bpu_addr_t addr) const;
    void insert_or_update(bpu_addr_t addr, const abtb_entry_t& new_entry);
    void insert_or_update(bpu_addr_t addr, bpu_addr_t target_addr,
                          const bpu_sign_t& cfi_entry,
                          int next_cfi_span_2b = 0,
                          bpu_addr_t next_cfi_addr = 0);
    void insert_or_update(bpu_addr_t addr, const btb_response_t& response);
    void invalidate(bpu_addr_t addr);
    void clear();
    void commit(bpu_addr_t addr, bpu_addr_t target_addr,
                const bpu_sign_t& cfi_entry,
                int next_cfi_span_2b = 0,
                bpu_addr_t next_cfi_addr = 0);
    void commit();

private:
    abtb_cfg cfg;
    std::vector<std::vector<abtb_entry_t>> table;

    int getBank2BCount() const;
    int getBankIndex(bpu_addr_t addr) const;
    int getSetIndex(bpu_addr_t addr) const;
    int getFetchBlockOffset2B(bpu_addr_t addr) const;
    bpu_sign_t makeFetchBlockSign(const bpu_sign_t& bank_sign,
                                  int fetch_block_offset_2b) const;
};
