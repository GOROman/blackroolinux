/*
 * serial.c — SIO1 UART driver for Blackroo bootloader
 *
 * Low-level PS1 serial port (SIO1) communication.
 * Handles hardware init, byte I/O, and 32-bit word transfer.
 * The command protocol lives in shell.c.
 *
 * Reference: psx-spx Serial Interfaces (SIO1)
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdint.h>
#include "serial.h"

/* PS1 SIO1 hardware registers */
#define SIO1_DATA  (*(volatile uint8_t  *)0x1F801050)
#define SIO1_STAT  (*(volatile uint16_t *)0x1F801054)
#define SIO1_MODE  (*(volatile uint16_t *)0x1F801058)
#define SIO1_CTRL  (*(volatile uint16_t *)0x1F80105A)
#define SIO1_BAUD  (*(volatile uint16_t *)0x1F80105E)

#define SIO_STAT_TXRDY  (1 << 0)
#define SIO_STAT_RXRDY  (1 << 1)

void serial_init(uint32_t baud) {
    volatile int i;

    /* Reset SIO1 */
    SIO1_CTRL = 0x40;
    for (i = 0; i < 1000; i++);

    /* Drain any stale RX data */
    while (SIO1_STAT & SIO_STAT_RXRDY)
        (void)SIO1_DATA;

    /*
     * 8 data bits, no parity, 1 stop bit, x16 baud multiplier
     *   Bits 0-1: 10 = MUL16
     *   Bits 2-3: 11 = 8 data bits
     *   Bit 4:    0  = no parity
     *   Bits 6-7: 01 = 1 stop bit
     * Source: psx-spx.consoledev.net/serialinterfacessio/
     */
    SIO1_MODE = 0x004E;

    /*
     * Baud divisor: sysclock / (baud * 16)
     *   33868800 / (115200 * 16) = 18.4 → 0x12
     *   33868800 / (518400 * 16) = 4.08 → 0x04
     */
    if (baud == SERIAL_BAUD_FAST)
        SIO1_BAUD = 0x04;
    else
        SIO1_BAUD = 0x12;

    /*
     * Enable TX + RX, assert DTR + RTS
     *   Bit 0: TXEN    Bit 1: DTR
     *   Bit 2: RXEN    Bit 5: RTS
     * RTS must be asserted so the FTDI adapter drives CTS on
     * the PS1 side. Without CTS, SIO1 hardware blocks all TX.
     * Source: psx-spx.consoledev.net/serialinterfacessio/
     */
    SIO1_CTRL = 0x27;

    for (i = 0; i < 1000; i++);
}

void serial_putchar(char c) {
    while (!(SIO1_STAT & SIO_STAT_TXRDY));
    SIO1_DATA = c;
}

void serial_puts(const char *s) {
    while (*s) serial_putchar(*s++);
}

int serial_getchar(void) {
    int timeout = 0;
    while (!(SIO1_STAT & SIO_STAT_RXRDY)) {
        if (++timeout > 10000000) return -1;
    }
    return SIO1_DATA;
}

int serial_getchar_timeout(int timeout_loops) {
    int timeout = 0;
    while (!(SIO1_STAT & SIO_STAT_RXRDY)) {
        if (++timeout > timeout_loops) return -1;
    }
    return SIO1_DATA;
}

void serial_send_u32(uint32_t val) {
    serial_putchar((val      ) & 0xFF);
    serial_putchar((val >>  8) & 0xFF);
    serial_putchar((val >> 16) & 0xFF);
    serial_putchar((val >> 24) & 0xFF);
}

int serial_recv_u32(uint32_t *val) {
    int b0 = serial_getchar();
    if (b0 < 0) return 0;
    int b1 = serial_getchar();
    if (b1 < 0) return 0;
    int b2 = serial_getchar();
    if (b2 < 0) return 0;
    int b3 = serial_getchar();
    if (b3 < 0) return 0;

    *val = (uint32_t)b0        | ((uint32_t)b1 << 8) |
           ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
    return 1;
}
