#include <windows.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    printf("========================================\n");
    printf("  Hello from Windows / Wine on AvoryOS! \n");
    printf("========================================\n");
    printf("Arguments received: %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }
    return 0;
}
