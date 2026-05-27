#include <stdlib.h>
#include <stdio.h>

void __cxa_call_terminate(void) {
    fprintf(stderr, "__cxa_call_terminate called\n");
    abort();
}
