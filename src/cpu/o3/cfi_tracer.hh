#pragma once

#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/bpu/bpu_structs.hh"

using gem5::InstSeqNum;
using gem5::ThreadID;

class cfi_tracer
{
    public:
        struct cfi_point_t
        {
            bpu_sign_t sign;
            bpu_addr_t target = 0;
            int next_cfi_span_2b = 0;
            InstSeqNum seq_num = 0;
        };

        struct cfi_tracer_entry_t
        {
            bool valid = false;
            ThreadID tid = 0;
            InstSeqNum first_seq = 0;
            InstSeqNum last_seq = 0;
            bpu_addr_t fetch_block_addr = 0;
            std::vector<cfi_point_t> cfi_points;
        };

        struct check_result_t
        {
            bool mismatch = false;
            bool needs_redirect = false;
            bool wait_for_backend = false;
            bpu_addr_t redirect_target = 0;
            bpu_sign_t sign;
        };

        explicit cfi_tracer(int depth = 64, int bank_count = 1);

        /**
         * cfi_tracer içine ekleme yapar.
         */
        // Decode/FQ tarafı bir fetch block'u predecode ettikten sonra bu
        // fonksiyonu çağırır. Entry, gem5 squash/commit akışıyla
        // eşlenebilmesi için thread id ve seqNum aralığını taşır; BTB update
        // gerektiğinde ise aynı entry içindeki CFI noktaları btb_entry_t
        // formatına çevrilebilir.
        void add_fetch_block(ThreadID tid, bpu_addr_t fetch_block_addr,
                             InstSeqNum first_seq, InstSeqNum last_seq,
                             const std::vector<cfi_point_t>& cfi_points);

        /**
         * eğer iki değer uyuşmaz ise bpu, ftq flush edilir.
         * önce tage tekrar çağrılır, ve bpu history mevzusu SHQ sayesinde
         * düzeltilir.
         * SHQ içine kayıt bpu2 anında btb yanıtı ile yapılır.
         * eğer taken nokta jal ise jal değerine atlanır.
         * eğer branch ise branc değerine atlanır.
         * jalr ise backend beklenir.
         */
        // Bu kontrol sadece decode/predecode aşamasında görülen gerçek CFI
        // vektörü ile BPU'nun FTQ'ye yazdığı sign vektörünü karşılaştırır.
        // Direct JAL/branch hatalarında frontend redirect üretilebilir; JALR
        // için register değeri backend'de çözüleceğinden sonuç
        // wait_for_backend olarak döner. SHQ gelince predictor-history onarımı
        // bu mismatch sonucundaki seqNum/CFI bilgisine bağlanacak.
        check_result_t check(ThreadID tid, bpu_addr_t fetch_block_addr,
                             const std::vector<bpu_sign_t>& bpu_sign_vector)
            const;

        const cfi_tracer_entry_t* lookup(ThreadID tid,
                                         bpu_addr_t fetch_block_addr) const;
        btb_entry_t make_btb_entry(ThreadID tid,
                                   bpu_addr_t fetch_block_addr) const;
        std::vector<btb_commit_update_t> make_btb_commit_updates(
            ThreadID tid, bpu_addr_t fetch_block_addr) const;
        void squash_after(ThreadID tid, InstSeqNum seq_num);
        void commit_until(ThreadID tid, InstSeqNum done_seq);
        void clear();


    private:
        int depth;
        int bank_count;
        std::vector<cfi_tracer_entry_t> cfi_tracer_entries;

        int get_bank_2b_count() const;
        int get_bank_slot(const bpu_sign_t& sign) const;
        bpu_addr_t get_bank_pc(bpu_addr_t fetch_block_addr,
                               int bank_slot) const;
        bpu_sign_t make_btb_sign(const bpu_sign_t& fetch_block_sign) const;
};
