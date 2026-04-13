/*
 * serial.h — SIO1 UART driver for Blackroo bootloader
 *
 * Low-level serial port I/O over PS1 SIO1 (0x1F801050).
 * The protocol layer lives in shell.c; this is just the driver.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_SERIAL_H
#define BLACKROO_SERIAL_H

#include <stdint.h>

#define SERIAL_BAUD_SLOW  115200
#define SERIAL_BAUD_FAST  518400

/*
 * Quick RX-ready check for tight polling loops.
 * Reads SIO1_STAT bit 1 directly — no function call overhead.
 */
#define serial_rx_ready() \
    ((*(volatile uint16_t *)0x1F801054) & (1 << 1))

/* Initialize SIO1 at the given baud rate (8N1, x16 multiplier) */
void serial_init(uint32_t baud);

/* Blocking single-byte I/O */
void serial_putchar(char c);
int  serial_getchar(void);

/* String output */
void serial_puts(const char *s);

/* Non-blocking read with loop-count timeout. Returns -1 on timeout. */
int  serial_getchar_timeout(int timeout_loops);

/* 32-bit word I/O (little-endian) */
void serial_send_u32(uint32_t val);
int  serial_recv_u32(uint32_t *val);  /* returns 0 on timeout */

#endif
