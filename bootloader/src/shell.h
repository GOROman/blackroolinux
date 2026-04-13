/*
 * shell.h — Blackroo Serial Shell protocol
 *
 * Command-driven serial interface for the Blackroo bootloader.
 * The PS1 acts as a command server over SIO1, processing file
 * uploads, memory operations, and kernel launch requests from
 * the host PC.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_SHELL_H
#define BLACKROO_SHELL_H

#include <stdint.h>

/* Build a 4-byte protocol tag from ASCII chars (little-endian) */
#define SHELL_TAG(a,b,c,d) \
    ((uint32_t)(a) | ((uint32_t)(b)<<8) | \
     ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))

/* ── Beacon ─────────────────────────────────────────────────── */
/* PS1 sends this periodically to advertise shell readiness.    */
/* Host waits for this before sending any command.              */
#define SHELL_BEACON  SHELL_TAG('B','K','>','>')

/* ── Responses (PS1 -> Host) ─────────────────────────────────  */
#define SHELL_OK      SHELL_TAG('O','K','O','K')   /* Accepted    */
#define SHELL_ERR     SHELL_TAG('F','A','I','L')   /* Error       */
#define SHELL_MORE    SHELL_TAG('M','O','R','E')   /* Next chunk  */
#define SHELL_DONE    SHELL_TAG('D','O','N','E')   /* Complete    */
#define SHELL_PONG    SHELL_TAG('P','O','N','G')   /* Ping reply  */

/* ── Commands (Host -> PS1) ──────────────────────────────────  */
#define CMD_PING      SHELL_TAG('P','I','N','G')   /* Ping        */
#define CMD_UEXE      SHELL_TAG('U','E','X','E')   /* Upload EXE  */
#define CMD_UBIN      SHELL_TAG('U','B','I','N')   /* Upload bin  */
#define CMD_EXEC      SHELL_TAG('E','X','E','C')   /* Jump addr   */
#define CMD_BOOT      SHELL_TAG('B','O','O','T')   /* Launch kern */
#define CMD_DUMP      SHELL_TAG('D','U','M','P')   /* Dump memory */
#define CMD_PEEK      SHELL_TAG('P','E','E','K')   /* Read word   */
#define CMD_POKE      SHELL_TAG('P','O','K','E')   /* Write word  */
#define CMD_FAST      SHELL_TAG('F','A','S','T')   /* 518400 baud */
#define CMD_SLOW      SHELL_TAG('S','L','O','W')   /* 115200 baud */
#define CMD_REST      SHELL_TAG('R','E','S','T')   /* Reset       */

/* Data transfer chunk size (matches CD sector size) */
#define SHELL_CHUNK   2048

/*
 * Enter the serial shell command loop.
 *
 * The PS1 broadcasts a "BK>>" beacon over SIO1 to advertise
 * readiness, then processes commands from the host.
 *
 * Supports: file upload (EXE/binary), memory read/write,
 * kernel launch, speed negotiation, and reset.
 *
 * Returns when the user presses Select on the controller.
 */
void shell_run(uint32_t baud);

#endif
