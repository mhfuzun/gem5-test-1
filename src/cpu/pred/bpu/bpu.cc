#include <algorithm>

#include "bpu.hh"

bpu::bpu(btb_cfg btb_cfg, ubtb_cfg ubtb_cfg, tt_cfg tt_cfg,
         ftq_cfg ftq_cfg)
    : bpu_ubtb(ubtb_cfg),
      bpu_btb(btb_cfg),
      bpu_tt(tt_cfg),
      bpu_ftq(ftq_cfg)
{
    fetch_block_size_2b = std::max(1, btb_cfg.bank_count);
}

void
bpu::reset()
{
    base_addr = 0;
    cfi_addr = 0;
    pending_next_cfi_span_2b = 0;
    bpu_ftq.clear();
}

void
bpu::set_base_addr(int pc)
{
    base_addr = pc;
    pending_next_cfi_span_2b = 0;
    cfi_addr = compute_cfi_addr();
}

bpu_cycle_output_t
bpu::tick(const bpu_cycle_input_t& input)
{
    if (input.base_valid) {
        set_base_addr(input.base_addr);
    }

    bpu_cycle_output_t output;
    cfi_addr = compute_cfi_addr();

    tt_bank_response_t tt_response = input.tt_response;
    if (tt_response.banks.empty()) {
        tt_response = bpu_tt.lookup(cfi_addr);
    }

    const int ftq_size_before_bpu1 = bpu_ftq.size();
    output.bpu1 = run_bpu1(cfi_addr);

    output.bpu2 = run_bpu2(cfi_addr, input.tage_response,
                           tt_response, input.ras_response);
    output.redirect = run_bpu3(cfi_addr, output.bpu2,
                               input.ittage_response);

    output.ftq_pushed = bpu_ftq.size() > ftq_size_before_bpu1;
    output.ftq_updated = output.bpu2.valid || output.redirect.valid;

    if (output.redirect.valid) {
        advance_from_prediction(true, output.redirect.target,
                                output.redirect.next_cfi_span_2b);
    } else if (output.bpu2.taken) {
        output.redirect.valid = true;
        output.redirect.target = output.bpu2.target;
        output.redirect.next_cfi_span_2b = output.bpu2.next_cfi_span_2b;
        output.redirect.sign = output.bpu2.sign;
        advance_from_prediction(true, output.bpu2.target,
                                output.bpu2.next_cfi_span_2b);
    } else if (output.bpu2.valid &&
               output.bpu2.sign.type == CFI_JALR_CALL &&
               !output.bpu2.tt_hit) {
        cfi_addr = compute_cfi_addr();
    } else if (output.bpu2.valid) {
        advance_fallthrough(output.bpu2.next_cfi_span_2b);
    } else if (output.bpu1.taken) {
        advance_from_prediction(true, output.bpu1.target,
                                output.bpu1.next_cfi_span_2b);
    } else {
        advance_fallthrough(output.bpu1.next_cfi_span_2b);
    }

    return output;
}

ubtb_response_t
bpu::run_bpu1(int pc)
{
    ubtb_response_t ubtb_response = bpu_ubtb.predict(pc);
    // ubtb oku, sonucu ftq içine base adres ve span olarak yaz.
    // Bu stage hızlı yol olduğu için yalnızca uBTB'nin verdiği erken block
    // bilgisini FTQ'ye taşır. Daha güvenilir BTB/TAGE/TT/RAS sonucu BPU2'de
    // aynı FTQ entry'sini düzeltir; burada amaç fetch tarafını boş bırakmadan
    // ilerideki muhtemel CFI bloğuna doğru kaba bir yol haritası üretmek.
    // fetch_span_2b, IFU'nun base_addr'den itibaren kaç 16-bit parça çekeceği;
    // next_cfi_span_2b ise redirect target/base üzerinden bir sonraki CFI
    // fetch block'una bank-aligned yürüyüş mesafesidir.
    const int fetch_span_2b = ubtb_response.valid && ubtb_response.taken ?
        fetch_span_through_taken_2b(ubtb_response.sign) :
        pending_next_cfi_span_2b + fetch_block_size_2b;

    ftq_entry_t* entry = bpu_ftq.add_entry(base_addr, fetch_span_2b);
    if (entry != nullptr) {
        entry->target = ubtb_response.target;
        entry->next_cfi_span_2b =
            bank_align_span_2b(ubtb_response.next_cfi_span_2b);
        entry->cfi_taken_sign = ubtb_response.sign;
    }

    return ubtb_response;
}

btb_response_t
bpu::run_bpu2(int pc, tage_response_t tage_response,
              tt_bank_response_t tt_response, ras_response_t ras_response)
{
    // ...

    /**
     * btb, TAGE, RAS (ToS), TT'den yanıt al,
     * yanıtı btb yanıtı ile karşılaştır, bir farklılık varsa düzelt.
     * span değerini doğru olacak şekilde ayarla (ftq içindeki)
     * bpu redirect yap, eğer ftq isteği okunup işlem yapılmış ise
     * şuan ilk okunduktan sonraki noktadır (pop->use it şeklinde), onu da
     * denetle
     */
    /*
     * BPU2 burada bank hizalı sonuçları birleştirir. BTB lookup, fetch bloğu
     * içindeki bank sırasını koruyarak CFI kayıtlarını döndürür; TT de aynı
     * istek PC'si için bank sayısı kadar hit/target cevabı üretir. Bir JALR
     * call kaydı seçilirse BTB kaydının bulunduğu bank slotu ile aynı TT slotu
     * okunur. Böylece TT tek global target gibi davranmaz; BTB'nin banklanmış
     * CFI görüşüyle aynı fiziksel hizayı takip eder.
     *
     * Bu fonksiyon şu an yalnızca FTQ front entry'sini düzeltir. Gem5
     * entegrasyonunda IFU bu entry'yi tüketmişse redirect çıktısı üzerinden
     * fetch PC düzeltmesi yapılmalı; SHQ/CFI tracer geldiğinde aynı noktada
     * predictor metadata id'leri de bağlanacak.
     *
     * Taken bir kayıt seçildiğinde FTQ fetch_span_2b, önceki redirect'in
     * next_cfi_span_2b değeri + bu block içindeki CFI offset'i + instruction
     * uzunluğu olarak hesaplanır. Böylece IFU, taken CFI'ın kendisini de
     * fetch eder; bir sonraki base target olur ve yeni CFI probe adresi
     * target + next_cfi_span_2b*2 şeklinde bulunur.
     */

    btb_response_t btb_response =
        bpu_btb.predict(pc, tage_response, tt_response, ras_response);

    ftq_entry_t* entry = bpu_ftq.back();
    if (entry != nullptr && btb_response.valid) {
        if (btb_response.taken ||
            btb_response.sign.type == CFI_JALR_CALL ||
            btb_response.sign.type == CFI_JALR_RET) {
            entry->fetch_span_2b =
                fetch_span_through_taken_2b(btb_response.sign);
        } else if (btb_response.sign.type == CFI_BRA) {
            entry->fetch_span_2b = pending_next_cfi_span_2b +
                bank_align_span_2b(btb_response.next_cfi_span_2b);
        }

        entry->target = btb_response.target;
        entry->next_cfi_span_2b =
            bank_align_span_2b(btb_response.next_cfi_span_2b);
        entry->cfi_taken_sign = btb_response.sign;
    }

    return btb_response;
}

bpu_redirect_t
bpu::run_bpu3(int pc, const btb_response_t& bpu2_response,
              ittage_response_t ittage_response)
{
    // ...

    // burada sadece ITTAGE, target değişkliği yapar,
    // bpu1, ve bpu2 flush'lanır. yaptıkları yazma işlemleri engellenir.
    // eğer bpu1'deki target yanlış adres ile IFU'de işlenmeye başlamış ise
    // bunu yakala ve düzelt.
    // ITTAGE yanıtı BPU2'den daha geç geldiği için burada yalnızca target
    // doğruluğu ile ilgilenmek gerekir. Yön/CFI seçimi BPU2'de sabit kalır;
    // ileride ITTAGE target değiştirdiğinde genç BPU1/BPU2 yazmaları iptal
    // edilip FTQ ve fetch tarafına geç target redirect'i gönderilecek.
    (void)pc;

    bpu_redirect_t redirect;
    ftq_entry_t* entry = bpu_ftq.back();

    if (entry == nullptr ||
        bpu2_response.sign.type != CFI_JALR_CALL ||
        bpu2_response.tt_hit) {
        return redirect;
    }

    if (ittage_response.hit) {
        entry->target = ittage_response.target;
        entry->next_cfi_span_2b =
            bank_align_span_2b(ittage_response.next_cfi_span_2b);
        entry->jalr_fail = false;

        redirect.valid = true;
        redirect.target = ittage_response.target;
        redirect.next_cfi_span_2b = entry->next_cfi_span_2b;
        redirect.sign = bpu2_response.sign;
        return redirect;
    }

    bpu_ftq.set_jalr_fail(*entry, true);
    return redirect;
}

void
bpu::consume_ftq(int two_byte_count)
{
    bpu_ftq.consume(two_byte_count);
}

int
bpu::get_fetch_span() const
{
    return bpu_ftq.get_fetch_span_2b();
}

const ftq_entry_t*
bpu::get_ftq_front() const
{
    return bpu_ftq.front();
}

void
bpu::update_ubtb(int pc, const ubtb_entry_t& entry)
{
    bpu_ubtb.insert_or_update(pc, entry);
}

void
bpu::update_btb(int pc, const btb_entry_t& entry)
{
    bpu_btb.insert_or_update(pc, entry);
}

void
bpu::update_tt(int pc, int target)
{
    bpu_tt.insert_or_update(pc, target);
}

int
bpu::compute_cfi_addr() const
{
    return base_addr + pending_next_cfi_span_2b * 2;
}

int
bpu::bank_align_span_2b(int span_2b) const
{
    if (span_2b <= 0) {
        return 0;
    }

    return (span_2b / fetch_block_size_2b) * fetch_block_size_2b;
}

int
bpu::cfi_inst_size_2b(const bpu_sign_t& sign) const
{
    return sign.compressed ? 1 : 2;
}

int
bpu::fetch_span_through_taken_2b(const bpu_sign_t& sign) const
{
    return pending_next_cfi_span_2b + sign.offset + cfi_inst_size_2b(sign);
}

void
bpu::advance_from_prediction(bool taken, int target, int next_cfi_span_2b)
{
    if (taken) {
        base_addr = target;
        pending_next_cfi_span_2b = bank_align_span_2b(next_cfi_span_2b);
    }

    cfi_addr = compute_cfi_addr();
}

void
bpu::advance_fallthrough(int next_cfi_span_2b)
{
    const int aligned_span = bank_align_span_2b(next_cfi_span_2b);
    pending_next_cfi_span_2b +=
        aligned_span > 0 ? aligned_span : fetch_block_size_2b;
    cfi_addr = compute_cfi_addr();
}
