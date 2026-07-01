/*
 * reboot — trigger a hardware reboot via the sys_reboot syscall.
 *
 * Usage: reboot
 *
 * Uses the Linux reboot(2) interface (syscall 169) with the mandatory
 * magic values that the kernel validates before acting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

/* Linux reboot(2) magic numbers */
#define LINUX_REBOOT_MAGIC1   0xfee1deadu
#define LINUX_REBOOT_MAGIC2   0x28121969u
#define LINUX_REBOOT_CMD_RESTART 0x01234567u

int main(void) {
    fprintf(stderr, "System is going down for reboot NOW!\n");
    fflush(stderr);

    long ret = syscall(SYS_reboot,
                       (long)LINUX_REBOOT_MAGIC1,
                       (long)LINUX_REBOOT_MAGIC2,
                       (long)LINUX_REBOOT_CMD_RESTART,
                       (long)0);

    /* Should never reach here on success */
    if (ret < 0) {
        perror("reboot");
        return 1;
    }
    return 0;
}
