#include <stdint.h>
#include <stdio.h>

#include "gem5/m5ops.h" // header dosyan m5_* fonksiyonları için

int main() {
    // Her şey boot olduktan sonra terminalden çalıştır
    printf("Checkpoint alinacak...\n");
    m5_checkpoint(0, 0);  // hemen checkpoint
    printf("Checkpoint alindi.\n");
    return 0;
}
