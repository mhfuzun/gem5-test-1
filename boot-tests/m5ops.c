#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gem5/m5ops.h" // m5_* fonksiyonlari icin

// Kullanim menusunu ekrana basan yardimci fonksiyon
void print_usage(const char *prog_name) {
  printf("Kullanim: %s <komut>\n\n", prog_name);
  printf("Gecerli Komutlar:\n");
  printf("  reset      : Istatistikleri sifirlar (m5_reset_stats)\n");
  printf("  dump       : Istatistikleri kaydeder (m5_dump_stats)\n");
  printf("  dumpreset  : Istatistikleri kaydeder ve hemen sifirlar\n");
  printf("  checkpoint : Simulasyon checkpoint'i alir (m5_checkpoint)\n");
  printf("  workbegin  : Ilgi alani (ROI - Region of Interest) baslangicini "
         "isaretler\n");
  printf("  workend    : Ilgi alani (ROI) bitisini isaretler\n");
  printf("  help       : Bu yardim menusunu gosterir\n");
}

int main(int argc, char *argv[]) {
  // Eger hicbir komut girilmediyse (sadece ./stats calistirildiysa)
  if (argc < 2) {
    print_usage(argv[0]);
    return 1; // Hata kodu ile cik
  }

  // argv[1] kullanicinin girdigi ilk komuttur.
  // strcmp ile girilen komutu kontrol ediyoruz.
  if (strcmp(argv[1], "reset") == 0) {
    printf("[GEM5] Istatistikler sifirlaniyor...\n");
    m5_reset_stats(0, 0);
  } else if (strcmp(argv[1], "dump") == 0) {
    printf("[GEM5] Istatistikler kaydediliyor (dump)...\n");
    m5_dump_stats(0, 0);
  } else if (strcmp(argv[1], "dumpreset") == 0) {
    printf("[GEM5] Istatistikler kaydedilip sifirlaniyor...\n");
    m5_dump_reset_stats(0, 0);
  } else if (strcmp(argv[1], "checkpoint") == 0) {
    printf("[GEM5] Checkpoint aliniyor...\n");
    m5_checkpoint(0, 0);
    printf("[GEM5] Checkpoint alindi...\n");
  } else if (strcmp(argv[1], "workbegin") == 0) {
    printf("[GEM5] Work begin isaretlendi...\n");
    m5_work_begin(0, 0);
  } else if (strcmp(argv[1], "workend") == 0) {
    printf("[GEM5] Work end isaretlendi...\n");
    m5_work_end(0, 0);
  } else if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
    print_usage(argv[0]);
  } else {
    // Bilinmeyen bir komut girilirse
    printf("Hata: Bilinmeyen komut '%s'\n\n", argv[1]);
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
