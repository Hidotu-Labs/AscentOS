/*
 * shutdown — power off the system via the sys_reboot syscall.
 *
 * Usage: shutdown [-h | -P | -r | --halt | --poweroff | --reboot]
 *        (default: power off)
 *
 *   -h / --halt     : halt (cli + hlt loop, no power off)
 *   -P / --poweroff : power off (default)
 *   -r / --reboot   : reboot
 *
 * Uses the Linux reboot(2) interface (syscall 169) with the mandatory
 * magic values that the kernel validates before acting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

/* Linux reboot(2) magic numbers */
#define LINUX_REBOOT_MAGIC1        0xfee1deadu
#define LINUX_REBOOT_MAGIC2        0x28121969u

#define LINUX_REBOOT_CMD_RESTART   0x01234567u
#define LINUX_REBOOT_CMD_HALT      0xcdef0123u
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedcu

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-h|--halt] [-P|--poweroff] [-r|--reboot]\n"
            "  -h, --halt      Halt the system (no power off)\n"
            "  -P, --poweroff  Power off the system (default)\n"
            "  -r, --reboot    Reboot the system\n",
            prog);
}

int main(int argc, char *argv[]) {
    unsigned int cmd = LINUX_REBOOT_CMD_POWER_OFF;
    const char  *verb = "power off";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--halt") == 0) {
            cmd  = LINUX_REBOOT_CMD_HALT;
            verb = "halt";
        } else if (strcmp(argv[i], "-P") == 0 ||
                   strcmp(argv[i], "--poweroff") == 0) {
            cmd  = LINUX_REBOOT_CMD_POWER_OFF;
            verb = "power off";
        } else if (strcmp(argv[i], "-r") == 0 ||
                   strcmp(argv[i], "--reboot") == 0) {
            cmd  = LINUX_REBOOT_CMD_RESTART;
            verb = "reboot";
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-?") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    fprintf(stderr, "System is going down to %s NOW!\n", verb);
    fflush(stderr);

    long ret = syscall(SYS_reboot,
                       (long)LINUX_REBOOT_MAGIC1,
                       (long)LINUX_REBOOT_MAGIC2,
                       (long)cmd,
                       (long)0);

    /* Should never reach here on success */
    if (ret < 0) {
        perror("reboot syscall");
        return 1;
    }
    return 0;
}
