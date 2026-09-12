#ifndef LINUXKPI_LOG_H
#define LINUXKPI_LOG_H

#include <linux/stdarg.h>

/* Native logging bridge for Linux-API compilation units.
 *
 * Files compiled with the real Linux headers must not include the native
 * console headers: <linux/types.h> and the freestanding <stdint.h> define
 * uint64_t/int64_t differently, and mixing them is a hard compile error.
 * This header declares only what LinuxKPI glue and boot tests need from the
 * native klog, using no libc-style typedefs, so it can be included freely
 * alongside Linux headers.  Implementations live in
 * kernel/src/console/klog.c.
 */

void klog_puts(const char *s);
void klogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vklogf(const char *fmt, va_list ap);

/* Native lib/string.c; used by kthread name formatting and warning output. */
int vsnprintf(char *buf, __SIZE_TYPE__ size, const char *fmt, va_list args);

#endif /* LINUXKPI_LOG_H */
