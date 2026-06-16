/*
 * test_invlpg_bench.c  —  invlpg-on-fresh-map elimination benchmark
 *
 * All timing is done with raw TSC (rdtsc).  The TSC frequency is calibrated
 * once at startup using a 200 ms sleep, giving sub-microsecond resolution
 * regardless of how coarse CLOCK_MONOTONIC is on this platform.
 *
 * OUTPUT COLUMNS
 *   mmap   (cyc)  — TSC cycles for mmap(2)  [VMA registration, O(1)]
 *   touch  (cyc)  — TSC cycles to touch every page (N page faults)
 *   munmap (cyc)  — TSC cycles for munmap(2) [still calls invlpg per page]
 *   cyc/pg        — touch cycles ÷ page count  ← main signal for the fix
 *
 * HOW TO USE
 *   Run once on the UNFIXED kernel  → save output
 *   Run once on the FIXED kernel    → save output
 *   Compare the cyc/pg column for the touch rows.
 *   Expected: ~5-25% lower on the fixed kernel for large workloads.
 *
 * BUILD
 *   make userland/test_invlpg_bench.elf
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

/* ── TSC helpers ─────────────────────────────────────────────────────────── */

/* lfence serialises the instruction stream before rdtsc so we don't measure
 * out-of-order instructions from the preceding block leaking into the window */
static inline uint64_t tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate: count TSC ticks in SLEEP_MS milliseconds.
 * Returns ticks-per-nanosecond as a double (typically ~3.0 on a 3 GHz CPU). */
#define CALIB_SLEEP_MS 200

static double calib_ticks_per_ns(void) {
    struct timespec req = { 0, (long)CALIB_SLEEP_MS * 1000000L };
    uint64_t t0 = tsc();
    nanosleep(&req, NULL);
    uint64_t t1 = tsc();
    return (double)(t1 - t0) / ((double)CALIB_SLEEP_MS * 1e6);
}

/* ── Statistics ──────────────────────────────────────────────────────────── */

#define MAX_TRIALS 32

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t median_u64(uint64_t *a, int n) {
    qsort(a, n, sizeof(uint64_t), cmp_u64);
    return (n & 1) ? a[n/2] : (a[n/2-1] + a[n/2]) / 2;
}

/* ── Single trial ────────────────────────────────────────────────────────── */

typedef struct { uint64_t mmap_cyc, touch_cyc, munmap_cyc; } trial_t;

static trial_t run_trial(size_t pages) {
    size_t bytes = pages * 4096;
    trial_t r;

    /* mmap */
    uint64_t t0 = tsc();
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t t1 = tsc();
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    r.mmap_cyc = t1 - t0;

    /* touch — forces one page fault per page */
    volatile char *cp = (volatile char *)p;
    t0 = tsc();
    for (size_t i = 0; i < pages; i++)
        cp[i * 4096] = (char)(i & 0xFF);
    __asm__ volatile("" ::: "memory"); /* prevent hoisting */
    t1 = tsc();
    r.touch_cyc = t1 - t0;

    /* munmap */
    t0 = tsc();
    munmap(p, bytes);
    t1 = tsc();
    r.munmap_cyc = t1 - t0;

    return r;
}

/* ── Workload table ──────────────────────────────────────────────────────── */

static const struct { const char *label; size_t pages; } wl[] = {
    { "  128 pages  (  512 KB)", 128   },
    { "  512 pages  (    2 MB)", 512   },
    { " 2048 pages  (    8 MB)", 2048  },
    { " 8192 pages  (   32 MB)", 8192  },
    { "16384 pages  (   64 MB)", 16384 },
};
#define NWL (int)(sizeof(wl)/sizeof(wl[0]))

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    const int TRIALS = 10;
    const int WARMUP = 2;

    printf("\nCalibrating TSC frequency (%d ms sleep)...\n", CALIB_SLEEP_MS);
    double tpns = calib_ticks_per_ns();
    double ghz  = tpns;                        /* ticks/ns == GHz */
    printf("TSC rate: %.4f GHz\n\n", ghz);

    printf("=== invlpg-on-fresh-map benchmark (TSC) ===\n");
    printf("Trials: %d  (+%d warmup discarded)\n\n", TRIALS, WARMUP);

    /* header */
    printf("%-26s  %12s  %14s  %12s  %10s  %10s\n",
           "Workload",
           "mmap (cyc)", "touch (Kcyc)", "munmap (cyc)",
           "cyc/page", "us/page");
    printf("%-26s  %12s  %14s  %12s  %10s  %10s\n",
           "──────────────────────────",
           "────────────", "──────────────",
           "────────────", "──────────", "──────────");

    for (int w = 0; w < NWL; w++) {
        size_t pages = wl[w].pages;
        uint64_t mmap_s[MAX_TRIALS], touch_s[MAX_TRIALS], munmap_s[MAX_TRIALS];
        int s = 0;

        for (int t = 0; t < TRIALS + WARMUP; t++) {
            trial_t r = run_trial(pages);
            if (t < WARMUP) continue;
            mmap_s[s]   = r.mmap_cyc;
            touch_s[s]  = r.touch_cyc;
            munmap_s[s] = r.munmap_cyc;
            s++;
        }

        uint64_t med_mmap   = median_u64(mmap_s,   s);
        uint64_t med_touch  = median_u64(touch_s,  s);
        uint64_t med_munmap = median_u64(munmap_s, s);
        double   cyc_per_pg = (double)med_touch / (double)pages;
        double   us_per_pg  = cyc_per_pg / (tpns * 1000.0);

        printf("%-26s  %12lu  %14.1f  %12lu  %10.1f  %10.3f\n",
               wl[w].label,
               (unsigned long)med_mmap,
               (double)med_touch / 1000.0,
               (unsigned long)med_munmap,
               cyc_per_pg,
               us_per_pg);
    }

    printf("\n");
    printf("KEY\n");
    printf("  touch (Kcyc) / cyc/page  = page-fault cost, TARGET of the fix\n");
    printf("  mmap / munmap            = should be stable across kernels\n");
    printf("\n");
    printf("EXPECTED (fixed vs unfixed kernel)\n");
    printf("  cyc/page decreases ~5-25%% on fixed kernel (no invlpg per fault)\n");
    printf("  mmap / munmap within ~10%% noise floor\n");
    printf("\n");
    printf("REVERT INSTRUCTIONS\n");
    printf("  git diff HEAD~1 -- kernel/src/mm/vmm_map.c\n");
    printf("  Rebuild without the flush_tlb=false change to get 'before' numbers\n");
    printf("\n");

    return 0;
}
