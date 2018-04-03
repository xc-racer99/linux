// SPDX-License-Identifier: GPL-2.0+
//
// On-the-wire packet headers used by
// modems speaking Samsung IPC v4.x
//
#ifndef UAPI_LINUX_SAMSUNG_IPC_H
#define UAPI_LINUX_SAMSUNG_IPC_H
struct fmt_header {
	/* length of message including header */
	u16 len;
	/* control bits? */
	u8 control;
} __packed;

struct raw_header {
	/* length of message including header */
	u32 len;
	/* channel ID & 0x1F */
	u8 channel;
	/* always zero for TX */
	u8 control;
} __packed;

struct rfs_header {
	/* length of header */
	u32 len;
	/* RFS command */
	u8 cmd;
	/* channel ID */
	u8 id;
};

#endif
