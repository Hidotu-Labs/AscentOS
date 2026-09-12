/* Phase 1 — imported Linux library smoke tests.
 *
 * This file is compiled with the real Linux headers (see the dedicated
 * pattern rule in kernel/GNUmakefile), so it exercises the exact upstream
 * types and signatures rather than hand-copied declarations.
 */

#include <linux/bitmap.h>
#include <linux/gcd.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/math.h>

#include <linuxkpi/log.h>

struct sort_elem {
  struct list_head list;
  int value;
};

static int sort_elem_cmp(void *priv, const struct list_head *a,
                         const struct list_head *b) {
  const struct sort_elem *ea = container_of(a, struct sort_elem, list);
  const struct sort_elem *eb = container_of(b, struct sort_elem, list);

  (void)priv;
  if (ea->value < eb->value)
    return -1;
  if (ea->value > eb->value)
    return 1;
  return 0;
}

static bool test_gcd(void) {
  return gcd(1071, 462) == 21 && gcd(17, 5) == 1 && gcd(0, 9) == 9 &&
         gcd(42, 42) == 42;
}

static bool test_int_sqrt(void) {
  return int_sqrt(0) == 0 && int_sqrt(1) == 1 && int_sqrt(144) == 12 &&
         int_sqrt(145) == 12 && int_sqrt(1000000) == 1000;
}

static bool test_find_bits(void) {
  unsigned long map[2] = {0x10UL, 0};    /* bit 4 set */
  unsigned long map2[2] = {0, 1UL << 1}; /* bit 65 set */
  unsigned long none[2] = {0, 0};

  return _find_first_bit(map, 128) == 4 &&
         _find_next_bit(map, 128, 5) == 128 &&
         _find_next_zero_bit(map, 128, 4) == 5 &&
         _find_first_bit(map2, 128) == 65 &&
         _find_last_bit(map2, 128) == 65 &&
         _find_first_bit(none, 128) == 128;
}

static bool test_list_sort(void) {
  static const int values[8] = {5, 1, 4, 2, 8, 6, 3, 7};
  struct sort_elem elems[8];
  LIST_HEAD(head);

  for (int i = 0; i < 8; i++) {
    elems[i].value = values[i];
    list_add_tail(&elems[i].list, &head);
  }

  list_sort(NULL, &head, sort_elem_cmp);

  int expected = 1;
  struct sort_elem *e;
  list_for_each_entry(e, &head, list) {
    if (e->value != expected)
      return false;
    expected++;
  }
  return expected == 9;
}

void linuxkpi_test_phase1_libs(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"gcd", test_gcd},
      {"int_sqrt", test_int_sqrt},
      {"find_bit", test_find_bits},
      {"list_sort", test_list_sort},
  };

  klog_puts("[LINUXKPI] Phase 1 library import self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s (upstream) correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s (upstream) wrong result\n", tests[i].name);
  }
}
