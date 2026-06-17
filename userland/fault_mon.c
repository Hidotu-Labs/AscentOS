#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

struct user_fault_record {
    uint32_t tid;
    uint32_t sig;
    uint64_t rip;
    uint64_t rsp;
    uint64_t cr2;
    uint64_t err_code;
    char comm[16];
    uint8_t regs[176]; // Simplified registers size (struct registers is large)
};

int main() {
    printf("--- AscentOS Fault Monitor ---\n");
    int fd = open("/dev/faults", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/faults");
        return 1;
    }

    printf("[INFO] Monitoring /dev/faults... Press Ctrl+C to stop.\n");

    struct user_fault_record rec;
    while (1) {
        int n = read(fd, &rec, sizeof(rec));
        if (n == sizeof(rec)) {
            printf("\n[!] DETECTED FAULT:\n");
            printf("    Process: %s (TID: %u)\n", rec.comm, rec.tid);
            printf("    Signal:  %u (SIGSEGV?)\n", rec.sig);
            printf("    RIP:     0x%lx\n", rec.rip);
            printf("    CR2:     0x%lx\n", rec.cr2);
            printf("    Err:     0x%lx\n", rec.err_code);
        } else {
            usleep(100000); // 100ms
        }
    }

    return 0;
}
