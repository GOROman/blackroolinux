#ifndef __BU_H__
#define __BU_H__

#include "asm/types.h"

#define BU_BLK_SIZE     (128)
#define BU_BLK_SHIFT    (7)
#define BU_FIRST_BLOCKS (8)
/*
 * BLACKROO 2026-08-21: multitap support.
 *
 * Was 2 — the console's two direct card ports, with the tap sub-port ("floor")
 * hardcoded to 0. A multitap addresses its four sub-ports with card address
 * bytes 0x81/0x82/0x83/0x84, which bu_rd_state0() already emits as
 * "0x81 + floor"; nothing ever set floor.
 *
 * Verified on real hardware with the monitor's polled probe (sio0 0 81 / 82):
 * both answer FLAG 08, ID 5A 5D, ACK on every byte.
 *
 * Minor number now encodes both: minor = (port << 2) | floor.
 *   minors 0-3 = port 1, tap sub-ports A-D
 *   minors 4-7 = port 2, tap sub-ports A-D
 * A card plugged directly into a port is sub-port A, i.e. minors 0 and 4,
 * so the old direct-slot behaviour still works.
 */
#define BU_MINORS       (8)
#define BU_PORT(minor)  (((minor) >> 2) & 1)
#define BU_FLOOR(minor) ((minor) & 3)
#define BU_BSIZE        (1024)
#define BU_HARDSECSIZE  (512)
#define BU_RAHEAD       (2)

// bu block transfer parameters
typedef struct _bu_request_t
{
	int   floor;				   // channel number 0..f
	__u16 block;				   // block number
	int	card;					   // 0 for bu0 and 1 for bu1
	__u8  buffer[BU_BLK_SIZE];	// data buffer
   int   mode;					   // 'R' - read, 'W' - write
} bu_request_t;

// driver transfer internal data
typedef struct
{
   bu_request_t * bu_request; // request parameters
	__u8  cs;						// checksum
	int   cnt;					   // counter
	int 	state;				   // irq FSM state
	int	stop;					   // stop packet transfer
   int   hw_state;			   // hardware state
   __u8	byte;					   // last byte received from controller
} bu_t; 

// card parameters (first block data)
typedef struct
{
   __u32 id;            // card id (must be BU_ID)
   __u32 size;          // card size in 128 blocks
   __u32 serial;        // card serial number
   __u32 number;        // card number for joined cards
} bu_first_block_t;

#define BU_ID	0x1234

// device data
typedef struct {
	bu_first_block_t first_block;
   int usage;
   int timeout;
} bu_device_t;

#define BU_NONE      0
#define BU_WAIT      1
#define BU_READY     2
#define BU_TIMEOUT   3

// bu ports
#define BU_DATA		 0x1040
#define BU_STATUS	 0x1044
#define BU_CONTROL 0x104A
#define BU_REG8		 0x1048
#define BU_REGE		 0x104E
#define PIC_ACK		 0x1070
#define PIC_MASK	 0x1074


#define BU_FLOOR_SHIFT 10
#define BU_BLOCK_MASK 0x3ff
#define BU_FLOOR_MASK 0xf

#endif
