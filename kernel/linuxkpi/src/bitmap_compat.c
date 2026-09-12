/* Out-of-line bitmap helpers consumed by imported code (xarray uses
 * __bitmap_clear() through the inline API in <linux/bitmap.h>).
 *
 * Upstream keeps these in lib/bitmap.c, whose other parts (bitmap_parse,
 * bitmap_alloc through the mm/device headers) need infrastructure AvoryOS
 * does not have yet.  The bit-array primitives below are self-contained and
 * semantically identical, so they live here until the full bitmap.c import
 * becomes possible.
 */

#include <linux/bitmap.h>
#include <linux/types.h>

void __bitmap_set(unsigned long *map, unsigned int start, int len) {
  unsigned long *p = map + BIT_WORD(start);
  const unsigned int size = start + len;
  int bits_to_set = BITS_PER_LONG - (start % BITS_PER_LONG);
  unsigned long mask_to_set = BITMAP_FIRST_WORD_MASK(start);

  while (len - bits_to_set >= 0) {
    *p |= mask_to_set;
    len -= bits_to_set;
    bits_to_set = BITS_PER_LONG;
    mask_to_set = ~0UL;
    p++;
  }
  if (len) {
    mask_to_set &= BITMAP_LAST_WORD_MASK(size);
    *p |= mask_to_set;
  }
}

void __bitmap_clear(unsigned long *map, unsigned int start, int len) {
  unsigned long *p = map + BIT_WORD(start);
  const unsigned int size = start + len;
  int bits_to_clear = BITS_PER_LONG - (start % BITS_PER_LONG);
  unsigned long mask_to_clear = BITMAP_FIRST_WORD_MASK(start);

  while (len - bits_to_clear >= 0) {
    *p &= ~mask_to_clear;
    len -= bits_to_clear;
    bits_to_clear = BITS_PER_LONG;
    mask_to_clear = ~0UL;
    p++;
  }
  if (len) {
    mask_to_clear &= BITMAP_LAST_WORD_MASK(size);
    *p &= ~mask_to_clear;
  }
}
