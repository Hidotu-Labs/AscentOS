#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>

void alarm_handler(int sig) {
    (void)sig;
    printf("Received SIGALRM!\n");
}

int main() {
    printf("--- Userland Test: setsid and setitimer ---\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        // Child process
        printf("[CHILD] Initial: PID=%d, PGID=%d, SID (dummy)=%d\n", getpid(), getpgrp(), getsid(0));
        
        // setsid() will fail if we are already a group leader.
        // Since fork() makes us a non-leader in the parent's group, this should work.
        pid_t new_sid = setsid();
        if (new_sid < 0) {
            perror("setsid");
            exit(1);
        }
        
        printf("[CHILD] After setsid(): PID=%d, PGID=%d, SID=%d\n", getpid(), getpgrp(), new_sid);
        
        if (new_sid != getpid()) {
            printf("[ERROR] SID should match PID after setsid()\n");
            exit(1);
        }
        if (getpgrp() != getpid()) {
            printf("[ERROR] PGID should match PID after setsid()\n");
            exit(1);
        }

        printf("[CHILD] Testing ITIMER_REAL (500ms intervals)...\n");
        signal(SIGALRM, alarm_handler);

        struct itimerval it;
        it.it_value.tv_sec = 0;
        it.it_value.tv_usec = 500000; // 500ms
        it.it_interval.tv_sec = 0;
        it.it_interval.tv_usec = 500000; // 500ms

        if (setitimer(ITIMER_REAL, &it, NULL) < 0) {
            perror("setitimer");
            exit(1);
        }

        for (int i = 0; i < 3; i++) {
            // pause() waits for a signal
            pause();
            printf("[CHILD] Alarm %d triggered at approx %d ms\n", i + 1, (i + 1) * 500);
        }

        // Disable timer
        it.it_value.tv_sec = 0;
        it.it_value.tv_usec = 0;
        setitimer(ITIMER_REAL, &it, NULL);

        printf("[CHILD] Test PASSED\n");
        exit(123); // Special exit code
    } else {
        // Parent process
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status)) {
            printf("[PARENT] Child exited with status %d\n", WEXITSTATUS(status));
            if (WEXITSTATUS(status) == 123) {
                printf("--- All Tests PASSED ---\n");
            } else {
                printf("--- Test FAILED ---\n");
            }
        } else {
            printf("[PARENT] Child terminated abnormally\n");
        }
    }

    return 0;
}
