/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2008 Simtec Electronics
 *	Ben Dooks <ben@simtec.co.uk>
 *
 * S5PV210 - System define for arch_reset() function
 */

#ifndef __PLAT_SAMSUNG_WATCHDOG_RESET_H
#define __PLAT_SAMSUNG_WATCHDOG_RESET_H

extern void samsung_wdt_reset(void);
extern void samsung_wdt_reset_of_init(void);

#endif /* __PLAT_SAMSUNG_WATCHDOG_RESET_H */
