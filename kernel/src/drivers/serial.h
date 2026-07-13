#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <stdint.h>
#include <stddef.h>

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *data, size_t length);
void serial_flush(void);
int serial_received(void);
char serial_get_char(void);


#endif
