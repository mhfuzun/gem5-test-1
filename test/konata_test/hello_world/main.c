// hello.c
#include <stdio.h>

#include "gem5/m5ops.h" // header dosyan m5_* fonksiyonları için

int main(void)
{
    m5_debug_break();

    int a = 3;
    int b = 5;

    m5_debug_break();

    return 0;
}
