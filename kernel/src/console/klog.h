#ifndef CONSOLE_KLOG_H
#define CONSOLE_KLOG_H

#include <stdbool.h>
#include <stdint.h>

#define KLOG_CLR_RESET  "\x1b[0m"
#define KLOG_CLR_RED    "\x1b[31m"
#define KLOG_CLR_GREEN  "\x1b[32m"
#define KLOG_CLR_YELLOW "\x1b[33m"
#define KLOG_CLR_BLUE   "\x1b[34m"
#define KLOG_CLR_MAGENTA "\x1b[35m"
#define KLOG_CLR_CYAN   "\x1b[36m"
#define KLOG_CLR_WHITE  "\x1b[37m"

void klog_putchar(char c);
void klog_puts(const char *s);
void klog_uint64(uint64_t num);
void klog_hex64(uint64_t num);
void klog_hex32(uint32_t num);

void klog_set_screen_logging(bool enabled);

#endif
