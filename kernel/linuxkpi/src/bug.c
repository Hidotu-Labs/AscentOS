/* Out-of-line BUG/WARN support that imported code expects from the Linux
 * panic code.  WARN_ONCE/WARN with a message expand to __warn_printk();
 * route it to the native klog through the bridge in linuxkpi/log.h. */

#include <linux/bug.h>
#include <linux/stdarg.h>

#include <linuxkpi/log.h>

void __warn_printk(const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  vklogf(fmt, args);
  va_end(args);
}
