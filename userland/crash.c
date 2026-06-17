#include <stdio.h>

int main() {
    printf("I'm about to crash...\n");
    volatile int *ptr = (int *)0xDEADBEEF;
    *ptr = 123;
    printf("I didn't crash?!?\n");
    return 0;
}
