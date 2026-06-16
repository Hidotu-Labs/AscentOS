#include "string.h"

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  while (n && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

size_t strlen(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  return len;
}

char *strcpy(char *dest, const char *src) {
  char *original_dest = dest;
  while ((*dest++ = *src++))
    ;
  return original_dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
  size_t i;
  for (i = 0; i < n && src[i] != '\0'; i++) {
    dest[i] = src[i];
  }
  for (; i < n; i++) {
    dest[i] = '\0';
  }
  return dest;
}

char *strrchr(const char *s, int c) {
  char *last = 0;
  do {
    if (*s == (char)c)
      last = (char *)s;
  } while (*s++);
  return last;
}

char *strcat(char *dest, const char *src) {
  char *ptr = dest;
  while (*ptr)
    ptr++;
  while ((*ptr++ = *src++))
    ;
  return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
  char *ptr = dest;
  while (*ptr)
    ptr++;
  while (n && (*ptr++ = *src++))
    n--;
  if (n == 0)
    *ptr = '\0';
  return dest;
}

void *memset(void *s, int c, size_t n) {
  unsigned char *p = s;
  uint8_t b = (uint8_t)c;

  // Fill lead-in bytes to align to 8-byte boundary
  while (n > 0 && ((uintptr_t)p & 7) != 0) {
    *p++ = b;
    n--;
  }

  if (n >= 8) {
    uint64_t val64 = (uint64_t)b | ((uint64_t)b << 8) | ((uint64_t)b << 16) |
                     ((uint64_t)b << 24);
    val64 |= (val64 << 32);

    uint64_t *p64 = (uint64_t *)p;
    while (n >= 8) {
      *p64++ = val64;
      n -= 8;
    }
    p = (unsigned char *)p64;
  }

  while (n--) {
    *p++ = b;
  }
  return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
  unsigned char *d = dest;
  const unsigned char *s = src;

  // If n is small or alignment is difficult, just byte copy
  if (n < 16 || (((uintptr_t)d & 7) != ((uintptr_t)s & 7))) {
    while (n--) {
      *d++ = *s++;
    }
    return dest;
  }

  // Align to 8-byte boundary
  while (n > 0 && ((uintptr_t)d & 7) != 0) {
    *d++ = *s++;
    n--;
  }

  uint64_t *d64 = (uint64_t *)d;
  const uint64_t *s64 = (const uint64_t *)s;
  while (n >= 8) {
    *d64++ = *s64++;
    n -= 8;
  }

  d = (unsigned char *)d64;
  s = (const unsigned char *)s64;
  while (n--) {
    *d++ = *s++;
  }

  return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *p1 = s1, *p2 = s2;
  while (n--) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

uint32_t atoui(const char *s) {
  uint32_t res = 0;
  while (*s >= '0' && *s <= '9') {
    res = res * 10 + (*s - '0');
    s++;
  }
  return res;
}

static char tolower_char(char c) {
  if (c >= 'A' && c <= 'Z')
    return c + ('a' - 'A');
  return c;
}

int strcasecmp(const char *s1, const char *s2) {
  while (*s1 && *s2) {
    char c1 = tolower_char(*s1);
    char c2 = tolower_char(*s2);
    if (c1 != c2)
      return (unsigned char)c1 - (unsigned char)c2;
    s1++;
    s2++;
  }
  return (unsigned char)*s1 - (unsigned char)*s2;
}


#define EMIT(c)                          \
    do {                                 \
        if (pos + 1 < size)              \
            buf[pos] = (char)(c);        \
        pos++;                           \
    } while (0)

/* Write `str` of `len` characters with padding into the buffer. */
static size_t emit_padded(char *buf, size_t size, size_t pos,
                          const char *str, size_t len,
                          int width, int left_align, char pad_char) {
    int w = (int)len;
    /* Pad on the left */
    if (!left_align && width > w) {
        for (int i = 0; i < width - w; i++)
            EMIT(pad_char);
    }
    /* Write the string */
    for (size_t i = 0; i < len; i++)
        EMIT(str[i]);
    /* Pad on the right */
    if (left_align && width > w) {
        for (int i = 0; i < width - w; i++)
            EMIT(' ');
    }
    return pos;
}

static int u64_to_buf(uint64_t val, unsigned int base, int uppercase,
                      char *tmp) {
    static const char lower[] = "0123456789abcdef";
    static const char upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? upper : lower;

    if (val == 0) {
        tmp[0] = '0';
        return 1;
    }

    char rev[66];
    int n = 0;
    while (val) {
        rev[n++] = digits[val % base];
        val /= base;
    }
    /* Reverse into tmp */
    for (int i = 0; i < n; i++)
        tmp[i] = rev[n - 1 - i];
    return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;

    /* Need at least 1 byte for the NUL terminator */
    if (!buf || size == 0)
        return 0;

    while (*fmt) {
        if (*fmt != '%') {
            EMIT(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* ---- Parse flags ---- */
        int left_align = 0;
        char pad_char  = ' ';

        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0' && !left_align) pad_char = '0';
            fmt++;
        }
        /* Left-align overrides zero-pad */
        if (left_align) pad_char = ' ';

        /* ---- Parse width ---- */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        /* ---- Parse precision ---- */
        int prec = -1; /* -1 means "not specified" */
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9')
                prec = prec * 10 + (*fmt++ - '0');
        }

        /* ---- Parse length modifier ---- */
        int is_ll = 0, is_l = 0, is_z = 0;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { is_ll = 1; fmt++; }
            else              { is_l  = 1; }
        } else if (*fmt == 'z') {
            is_z = 1; fmt++;
        }

        char conv = *fmt++;

        /* ---- Handle each conversion ---- */
        switch (conv) {
        case '%':
            EMIT('%');
            break;

        case 'c':
            {
                char ch = (char)va_arg(ap, int);
                char tmp[1] = { ch };
                pos = emit_padded(buf, size, pos, tmp, 1,
                                  width, left_align, ' ');
            }
            break;

        case 's':
            {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                size_t slen = strlen(s);
                if (prec >= 0 && (size_t)prec < slen)
                    slen = (size_t)prec;
                pos = emit_padded(buf, size, pos, s, slen,
                                  width, left_align, ' ');
            }
            break;

        case 'd':
        case 'i':
            {
                int64_t val;
                if      (is_ll) val = (int64_t)va_arg(ap, long long);
                else if (is_l)  val = (int64_t)va_arg(ap, long);
                else if (is_z)  val = (int64_t)va_arg(ap, int64_t);
                else            val = (int64_t)va_arg(ap, int);

                char tmp[66];
                int  neg = 0;
                int  n;
                if (val < 0) { neg = 1; val = -val; }
                n = u64_to_buf((uint64_t)val, 10, 0, tmp);

                /* Apply precision: minimum digit count */
                int digits = n;
                if (prec > digits) digits = prec;
                int total = digits + (neg ? 1 : 0);

                if (!left_align && pad_char == '0') {
                    /* sign then zeros then digits */
                    if (neg) EMIT('-');
                    for (int i = total - (neg ? 1 : 0); i < width - (neg ? 1 : 0); i++)
                        EMIT('0');
                    for (int i = digits - n; i > 0; i--) EMIT('0');
                    for (int i = 0; i < n; i++) EMIT(tmp[i]);
                } else {
                    /* build into a local array for emit_padded */
                    char full[80];
                    int  fi = 0;
                    if (neg) full[fi++] = '-';
                    for (int i = digits - n; i > 0; i--) full[fi++] = '0';
                    for (int i = 0; i < n; i++) full[fi++] = tmp[i];
                    pos = emit_padded(buf, size, pos, full, (size_t)fi,
                                      width, left_align, pad_char);
                }
            }
            break;

        case 'u':
        case 'x':
        case 'X':
        case 'o':
            {
                uint64_t val;
                if      (is_ll) val = (uint64_t)va_arg(ap, unsigned long long);
                else if (is_l)  val = (uint64_t)va_arg(ap, unsigned long);
                else if (is_z)  val = (uint64_t)va_arg(ap, size_t);
                else            val = (uint64_t)va_arg(ap, unsigned int);

                unsigned int base = (conv == 'x' || conv == 'X') ? 16
                                  : (conv == 'o')                 ?  8
                                                                   : 10;
                int uppercase = (conv == 'X');

                char tmp[66];
                int n = u64_to_buf(val, base, uppercase, tmp);

                int digits = n;
                if (prec > digits) digits = prec;

                if (!left_align && pad_char == '0') {
                    for (int i = digits; i < width; i++) EMIT('0');
                    for (int i = digits - n; i > 0; i--) EMIT('0');
                    for (int i = 0; i < n; i++) EMIT(tmp[i]);
                } else {
                    char full[80];
                    int  fi = 0;
                    for (int i = digits - n; i > 0; i--) full[fi++] = '0';
                    for (int i = 0; i < n; i++) full[fi++] = tmp[i];
                    pos = emit_padded(buf, size, pos, full, (size_t)fi,
                                      width, left_align, pad_char);
                }
            }
            break;

        case 'p':
            {
                uintptr_t val = (uintptr_t)va_arg(ap, void *);
                char tmp[66];
                int n = u64_to_buf((uint64_t)val, 16, 0, tmp);
                /* "0x" prefix + zero-padded to pointer width */
                int ptr_digits = (int)(sizeof(void *) * 2);
                int digits = n > ptr_digits ? n : ptr_digits;
                int total  = digits + 2; /* "0x" */

                char full[80];
                int fi = 0;
                full[fi++] = '0';
                full[fi++] = 'x';
                for (int i = digits - n; i > 0; i--) full[fi++] = '0';
                for (int i = 0; i < n; i++) full[fi++] = tmp[i];
                pos = emit_padded(buf, size, pos, full, (size_t)(total > fi ? fi : total),
                                  width, left_align, ' ');
            }
            break;

        default:
            /* Unknown specifier — emit literally */
            EMIT('%');
            EMIT(conv);
            break;
        }
    }

    /* NUL-terminate; pos is the logical length (may be >= size) */
    buf[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
}

#undef EMIT

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    /* Unbounded write — caller must ensure buf is large enough. */
    int ret = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}
