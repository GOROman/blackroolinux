/*
 * shell.c — Blackroo Serial Shell
 *
 * Command server running on the PS1 over SIO1. Processes
 * requests from the host PC: file upload, memory operations,
 * kernel launch, and speed negotiation.
 *
 * The PS1 sends "BK>>" beacons to advertise readiness.
 * The host sends 4-byte command tags, and the PS1 responds
 * with status tags and data. All values are little-endian.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxpad.h>
#include "shell.h"
#include "serial.h"
#include "kernel.h"
#include "settings.h"
#include "hwinfo.h"
#include "menu.h"
#include "version.h"

extern uint8_t pad_buff[2][34];
extern hw_info_t hw;
extern blackroo_settings_t cfg;

/* ── Shell state ──────────────────────────────────────────────── */

static uint32_t cur_baud;
static const char *status_text;
static const char *last_cmd_text;
static uint32_t last_entry;
static uint32_t last_load;
static uint32_t last_size;
static int has_upload;

/* ── Debug diagnostics ───────────────────────────────────────── */

#define DBG_RING_SIZE 16
static uint8_t  dbg_ring[DBG_RING_SIZE];
static int      dbg_ring_pos;
static uint32_t dbg_rx_total;        /* total bytes received     */
static uint32_t dbg_beacons_sent;    /* beacons transmitted      */
static uint32_t dbg_cmds_seen;       /* tags that reached dispatch */
static uint32_t dbg_last_tag;        /* last 4-byte tag value    */
static uint32_t dbg_poll_hits;       /* poll_rx returned a byte  */
static uint32_t dbg_poll_misses;     /* poll_rx timed out        */
static uint16_t dbg_sio_stat;       /* last SIO1_STAT snapshot  */
static uint16_t dbg_sio_ctrl;       /* last SIO1_CTRL snapshot  */

static void dbg_push(uint8_t b) {
    dbg_ring[dbg_ring_pos % DBG_RING_SIZE] = b;
    dbg_ring_pos++;
    dbg_rx_total++;
}

/* Snapshot SIO1 registers for display */
#define SIO1_STAT_REG  (*(volatile uint16_t *)0x1F801054)
#define SIO1_CTRL_REG  (*(volatile uint16_t *)0x1F80105A)
static void dbg_snap_regs(void) {
    dbg_sio_stat = SIO1_STAT_REG;
    dbg_sio_ctrl = SIO1_CTRL_REG;
}

/* ── Helpers ──────────────────────────────────────────────────── */

/*
 * Poll for first byte of a command with timeout.
 * Returns the byte, or -1 if nothing arrived.
 * Uses a tight loop for minimum latency.
 */
static int poll_rx(int iterations) {
    for (int i = 0; i < iterations; i++) {
        if (serial_rx_ready()) {
            int b = serial_getchar_timeout(1);
            if (b >= 0) {
                dbg_push((uint8_t)b);
                dbg_poll_hits++;
                dbg_snap_regs();
            }
            return b;
        }
    }
    dbg_poll_misses++;
    return -1;
}

/*
 * Read a full 4-byte command tag.
 * First byte already received, read remaining 3.
 */
static int read_tag(int first_byte, uint32_t *tag) {
    int b1 = serial_getchar();
    int b2 = serial_getchar();
    int b3 = serial_getchar();

    if (b1 >= 0) dbg_push((uint8_t)b1);
    if (b2 >= 0) dbg_push((uint8_t)b2);
    if (b3 >= 0) dbg_push((uint8_t)b3);

    if (b1 < 0 || b2 < 0 || b3 < 0) {
        last_cmd_text = "TAG TIMEOUT";
        return 0;
    }

    *tag = (uint32_t)first_byte |
           ((uint32_t)b1 << 8)  |
           ((uint32_t)b2 << 16) |
           ((uint32_t)b3 << 24);
    dbg_last_tag = *tag;
    return 1;
}

/* ── Data transfer ────────────────────────────────────────────── */

/*
 * Receive a block of data in 2KB chunks with byte-sum checksum.
 *
 * Protocol:
 *   Host sends data in SHELL_CHUNK-byte chunks.
 *   PS1 sends MORE after each chunk.
 *   After all data, PS1 sends DONE (checksum match) or ERR.
 *
 * Checksum must be provided by caller (already received from host).
 */
static void shell_draw(void);   /* defined below; used by the transfer banner */

/* 0x1F801074 — see kernel.c; masking these is what makes bulk RX reliable */
#define SHELL_INT_MASK (*(volatile uint16_t *)0x1F801074)

static int receive_data(uint8_t *dst, uint32_t size,
                        uint32_t expected_checksum) {
    uint32_t received = 0;
    uint32_t checksum = 0;
    uint16_t saved_imask;

    /*
     * Mask every interrupt for the duration of the transfer.
     *
     * serial_getchar() polls SIO1 RXRDY and cannot throttle the host — there
     * is no RTS handshake in this path — and SIO1 has only an 8-byte RX FIFO.
     * Any stall longer than ~700us at 115200 therefore loses bytes, and the
     * VSync IRQ handler (which polls the controller over SIO0) is exactly
     * that long. The host then waits forever for a MORE that never comes, and
     * the console looks frozen: this is why every large upload failed.
     *
     * With interrupts masked the poll loop is never preempted and the transfer
     * runs at line rate. The pad is not read during a transfer, so SELECT will
     * not exit mid-upload — an acceptable trade for a working loader.
     */
    saved_imask = SHELL_INT_MASK;
    SHELL_INT_MASK = 0;

    while (received < size) {
        uint32_t chunk = size - received;
        if (chunk > SHELL_CHUNK) chunk = SHELL_CHUNK;

        for (uint32_t i = 0; i < chunk; i++) {
            int b = serial_getchar();
            if (b < 0) {
                SHELL_INT_MASK = saved_imask;
                serial_send_u32(SHELL_ERR);
                return 0;
            }
            dst[received + i] = (uint8_t)b;
            checksum += (uint8_t)b;
        }

        received += chunk;
        serial_send_u32(SHELL_MORE);
    }

    SHELL_INT_MASK = saved_imask;

    if (checksum != expected_checksum) {
        serial_send_u32(SHELL_ERR);
        return 0;
    }

    serial_send_u32(SHELL_DONE);
    return 1;
}

/*
 * Send a block of data in 2KB chunks.
 * Host sends MORE after each chunk.
 * PS1 sends checksum after all data.
 */
static void send_data(const uint8_t *src, uint32_t size) {
    uint32_t sent = 0;
    uint32_t checksum = 0;

    while (sent < size) {
        uint32_t chunk = size - sent;
        if (chunk > SHELL_CHUNK) chunk = SHELL_CHUNK;

        for (uint32_t i = 0; i < chunk; i++) {
            uint8_t b = src[sent + i];
            serial_putchar(b);
            checksum += b;
        }

        sent += chunk;

        /* Wait for host to acknowledge chunk */
        uint32_t ack;
        if (!serial_recv_u32(&ack) || ack != SHELL_MORE)
            return;
    }

    /* Send checksum so host can verify */
    serial_send_u32(checksum);
}

/* ── Command handlers ─────────────────────────────────────────── */

/* UEXE: Upload PS-EXE (host pre-parses header, sends raw data) */
static void cmd_upload_exe(void) {
    uint32_t entry, load_addr, size, checksum;

    serial_send_u32(SHELL_OK);

    /* Receive transfer header */
    if (!serial_recv_u32(&entry))    goto fail;
    if (!serial_recv_u32(&load_addr)) goto fail;
    if (!serial_recv_u32(&size))     goto fail;
    if (!serial_recv_u32(&checksum)) goto fail;

    last_entry = entry;
    last_load  = load_addr;
    last_size  = size;
    status_text = "RECEIVING - do not touch";

    /*
     * Draw BEFORE acknowledging the header. The host sends no payload byte
     * until it sees this OKOK, so this is the only moment we can afford a
     * VSync: nothing is in flight. Drawing after the ack would drop bytes,
     * which is why the screen otherwise stays frozen during a transfer.
     */
    shell_draw();
    menu_swap_buffers();

    /* Acknowledge header — payload starts arriving immediately after this */
    serial_send_u32(SHELL_OK);

    /* Receive payload directly to load address */
    if (!receive_data((uint8_t *)load_addr, size, checksum))
        goto fail;

    has_upload = 1;
    status_text = "Upload complete";
    last_cmd_text = "UEXE OK";
    return;

fail:
    serial_send_u32(SHELL_ERR);
    status_text = "Upload failed";
    last_cmd_text = "UEXE FAIL";
}

/* UBIN: Upload raw binary to a specified address */
static void cmd_upload_bin(void) {
    uint32_t addr, size, checksum;

    serial_send_u32(SHELL_OK);

    if (!serial_recv_u32(&addr))     goto fail;
    if (!serial_recv_u32(&size))     goto fail;
    if (!serial_recv_u32(&checksum)) goto fail;

    serial_send_u32(SHELL_OK);

    last_load = addr;
    last_size = size;
    status_text = "Receiving BIN...";

    if (!receive_data((uint8_t *)addr, size, checksum))
        goto fail;

    status_text = "Binary loaded";
    last_cmd_text = "UBIN OK";
    return;

fail:
    serial_send_u32(SHELL_ERR);
    status_text = "Binary failed";
    last_cmd_text = "UBIN FAIL";
}

/* EXEC: Jump to address (never returns to shell) */
static void cmd_exec(void) {
    uint32_t addr;

    serial_send_u32(SHELL_OK);
    if (!serial_recv_u32(&addr)) return;

    ((void (*)(void))addr)();
}

/* BOOT: Launch uploaded kernel with cmdline */
static void cmd_boot(void) {
    uint32_t cmdline_len;
    char cmdline[256];

    if (!has_upload) {
        serial_send_u32(SHELL_ERR);
        last_cmd_text = "BOOT: no upload";
        return;
    }

    serial_send_u32(SHELL_OK);

    /* Host sends cmdline length (0 = use settings) */
    if (!serial_recv_u32(&cmdline_len)) return;

    if (cmdline_len > 0 && cmdline_len < sizeof(cmdline)) {
        /* Host provides custom cmdline */
        for (uint32_t i = 0; i < cmdline_len; i++) {
            int b = serial_getchar();
            if (b < 0) return;
            cmdline[i] = (char)b;
        }
        cmdline[cmdline_len] = '\0';
    } else {
        /* Build cmdline from saved settings */
        settings_build_cmdline(&cfg, cmdline, sizeof(cmdline));
    }

    /* Never returns */
    kernel_launch(last_entry, hw.ram_size_kb, cmdline);
}

/* DUMP: Send memory contents to host */
static void cmd_dump(void) {
    uint32_t addr, size;

    serial_send_u32(SHELL_OK);

    if (!serial_recv_u32(&addr)) return;
    if (!serial_recv_u32(&size)) return;

    send_data((const uint8_t *)addr, size);
    last_cmd_text = "DUMP OK";
}

/* PEEK: Read 32-bit word at address */
static void cmd_peek(void) {
    uint32_t addr;

    serial_send_u32(SHELL_OK);
    if (!serial_recv_u32(&addr)) return;

    uint32_t val = *(volatile uint32_t *)addr;
    serial_send_u32(val);
    last_cmd_text = "PEEK OK";
}

/* POKE: Write 32-bit word to address */
static void cmd_poke(void) {
    uint32_t addr, val;

    serial_send_u32(SHELL_OK);
    if (!serial_recv_u32(&addr)) return;
    if (!serial_recv_u32(&val))  return;

    *(volatile uint32_t *)addr = val;
    serial_send_u32(SHELL_DONE);
    last_cmd_text = "POKE OK";
}

/* FAST: Switch to 518400 baud */
static void cmd_fast(void) {
    volatile int i;
    serial_send_u32(SHELL_OK);
    /* Let the OK bytes transmit before changing baud */
    for (i = 0; i < 200000; i++);
    serial_init(SERIAL_BAUD_FAST);
    cur_baud = SERIAL_BAUD_FAST;
    last_cmd_text = "FAST (518400)";
}

/* SLOW: Switch to 115200 baud */
static void cmd_slow(void) {
    volatile int i;
    serial_send_u32(SHELL_OK);
    for (i = 0; i < 200000; i++);
    serial_init(SERIAL_BAUD_SLOW);
    cur_baud = SERIAL_BAUD_SLOW;
    last_cmd_text = "SLOW (115200)";
}

/* ── Command dispatch ─────────────────────────────────────────── */

static void shell_dispatch(uint32_t cmd) {
    dbg_cmds_seen++;
    switch (cmd) {
        case CMD_PING:
            serial_send_u32(SHELL_PONG);
            last_cmd_text = "PING";
            break;
        case CMD_UEXE: cmd_upload_exe(); break;
        case CMD_UBIN: cmd_upload_bin(); break;
        case CMD_EXEC: cmd_exec();       break;
        case CMD_BOOT: cmd_boot();       break;
        case CMD_DUMP: cmd_dump();       break;
        case CMD_PEEK: cmd_peek();       break;
        case CMD_POKE: cmd_poke();       break;
        case CMD_FAST: cmd_fast();       break;
        case CMD_SLOW: cmd_slow();       break;
        case CMD_REST:
            /* Jump to BIOS reset vector */
            __asm__ volatile(
                ".set noreorder\n"
                "li $t0, 0xBFC00000\n"
                "jr $t0\n"
                "nop\n"
                ".set reorder\n"
            );
            break;
        default:
            serial_send_u32(SHELL_ERR);
            /* Show unknown tag as hex + ASCII on screen */
            {
                static char unk_buf[32];
                uint8_t c0 = cmd & 0xFF;
                uint8_t c1 = (cmd >> 8) & 0xFF;
                uint8_t c2 = (cmd >> 16) & 0xFF;
                uint8_t c3 = (cmd >> 24) & 0xFF;
                /* Quick inline hex formatter */
                static const char hex[] = "0123456789ABCDEF";
                unk_buf[0]  = '?'; unk_buf[1] = ' ';
                unk_buf[2]  = hex[c0>>4]; unk_buf[3] = hex[c0&0xF];
                unk_buf[4]  = ' ';
                unk_buf[5]  = hex[c1>>4]; unk_buf[6] = hex[c1&0xF];
                unk_buf[7]  = ' ';
                unk_buf[8]  = hex[c2>>4]; unk_buf[9] = hex[c2&0xF];
                unk_buf[10] = ' ';
                unk_buf[11] = hex[c3>>4]; unk_buf[12] = hex[c3&0xF];
                unk_buf[13] = ' ';
                unk_buf[14] = '\'';
                unk_buf[15] = (c0>=0x20 && c0<0x7F) ? c0 : '.';
                unk_buf[16] = (c1>=0x20 && c1<0x7F) ? c1 : '.';
                unk_buf[17] = (c2>=0x20 && c2<0x7F) ? c2 : '.';
                unk_buf[18] = (c3>=0x20 && c3<0x7F) ? c3 : '.';
                unk_buf[19] = '\'';
                unk_buf[20] = '\0';
                last_cmd_text = unk_buf;
            }
            break;
    }
}

/* ── Display ──────────────────────────────────────────────────── */

static void shell_draw(void) {
    int i;

    FntPrint(0, "\n");
    FntPrint(0, "  BLACKROO SHELL v" BLACKROO_VERSION "\n");
    FntPrint(0, "  =====================\n");
    FntPrint(0, "  %s  Baud:%d\n", status_text, cur_baud);

    if (last_cmd_text)
        FntPrint(0, "  Last: %s\n", last_cmd_text);

    if (has_upload) {
        FntPrint(0, "  Kernel: 0x%08X %d B\n",
                 last_entry, last_size);
    }

    /* ── Debug panel ─────────────────────────── */
    FntPrint(0, "  ---- SIO1 DEBUG ----\n");
    FntPrint(0, "  STAT:%04X CTRL:%04X\n",
             dbg_sio_stat, dbg_sio_ctrl);
    FntPrint(0, "  RX:%d Bcn:%d Cmd:%d\n",
             dbg_rx_total, dbg_beacons_sent, dbg_cmds_seen);
    FntPrint(0, "  Hit:%d Miss:%d\n",
             dbg_poll_hits, dbg_poll_misses);

    if (dbg_last_tag) {
        uint8_t t0 = dbg_last_tag & 0xFF;
        uint8_t t1 = (dbg_last_tag >> 8) & 0xFF;
        uint8_t t2 = (dbg_last_tag >> 16) & 0xFF;
        uint8_t t3 = (dbg_last_tag >> 24) & 0xFF;
        FntPrint(0, "  Tag:%02X %02X %02X %02X '%c%c%c%c'\n",
                 t0, t1, t2, t3,
                 (t0>=0x20&&t0<0x7F)?t0:'.',
                 (t1>=0x20&&t1<0x7F)?t1:'.',
                 (t2>=0x20&&t2<0x7F)?t2:'.',
                 (t3>=0x20&&t3<0x7F)?t3:'.');
    }

    /* Show last N received bytes as hex */
    FntPrint(0, "  RX buf:");
    {
        int count = (dbg_ring_pos < DBG_RING_SIZE)
                    ? dbg_ring_pos : DBG_RING_SIZE;
        int start = (dbg_ring_pos < DBG_RING_SIZE)
                    ? 0 : (dbg_ring_pos % DBG_RING_SIZE);
        for (i = 0; i < count && i < 8; i++) {
            int idx = (start + i) % DBG_RING_SIZE;
            FntPrint(0, " %02X", dbg_ring[idx]);
        }
        FntPrint(0, "\n");
        if (count > 8) {
            FntPrint(0, "         ");
            for (i = 8; i < count; i++) {
                int idx = (start + i) % DBG_RING_SIZE;
                FntPrint(0, " %02X", dbg_ring[idx]);
            }
            FntPrint(0, "\n");
        }
    }

    FntPrint(0, "\n  [Select] Back to menu\n");

    FntFlush(-1);
}

/* ── Main shell loop ──────────────────────────────────────────── */

void shell_run(uint32_t baud) {
    cur_baud = baud;
    status_text = "Listening";
    last_cmd_text = 0;
    last_entry = 0;
    last_load = 0;
    last_size = 0;
    has_upload = 0;

    /* Reset debug counters */
    memset(dbg_ring, 0, sizeof(dbg_ring));
    dbg_ring_pos = 0;
    dbg_rx_total = 0;
    dbg_beacons_sent = 0;
    dbg_cmds_seen = 0;
    dbg_last_tag = 0;
    dbg_poll_hits = 0;
    dbg_poll_misses = 0;
    dbg_sio_stat = 0;
    dbg_sio_ctrl = 0;

    serial_init(baud);
    /* Snapshot registers right after init */
    dbg_snap_regs();
    menu_wait_release();

    /* Draw initial screen */
    shell_draw();
    menu_swap_buffers();

    while (1) {
        /*
         * Check controller for exit.
         * Pad buffer is updated by the VSync interrupt handler,
         * so it's fresh even without calling VSync ourselves.
         */
        {
            PADTYPE *pad = (PADTYPE *)pad_buff[0];
            uint16_t btn = (pad->stat == 0) ? ~pad->btn : 0;
            if (btn & PAD_SELECT) {
                menu_wait_release();
                return;
            }
        }

        /* Send beacon so the host knows we're ready */
        serial_send_u32(SHELL_BEACON);
        dbg_beacons_sent++;

        /*
         * Poll for incoming command.
         *
         * We spin here for ~500ms checking the SIO1 RX flag.
         * This keeps latency low — we catch the host's response
         * as soon as it arrives after the beacon. The tight loop
         * avoids the 16ms VSync gap that would cause byte loss.
         *
         * ~15M iterations ≈ 500ms at 33.8 MHz (4 cycles/iter).
         */
        int b0 = poll_rx(15000000);
        if (b0 < 0) {
            /* Nothing received — redraw screen and try again */
            shell_draw();
            menu_swap_buffers();
            continue;
        }

        /* Got first byte — read rest of 4-byte command tag */
        uint32_t cmd;
        if (!read_tag(b0, &cmd)) continue;

        /* Dispatch command (may block for data transfer) */
        status_text = "Processing...";
        shell_dispatch(cmd);
        status_text = "Listening";

        /* Redraw screen with updated status */
        shell_draw();
        menu_swap_buffers();
    }
}
