/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/logging/log.h>
#include <nrfx_ipc.h>

#define IPC_MEM_IDX 0

LOG_MODULE_REGISTER(net_core_monitor, CONFIG_NET_CORE_MONITOR_LOG_LEVEL);

int ncm_net_status_check(void)
{
	volatile uint32_t gpmem;
	uint16_t reset, cnt;
	static uint16_t prv_cnt;

	gpmem = nrfx_ipc_gpmem_get(IPC_MEM_IDX);
	reset = (gpmem >> 16) & BIT_MASK(16);
	cnt = gpmem & BIT_MASK(16);

	LOG_DBG("gpmem: 0x%08X reset: 0x%04x, cnt: 0x%04x", gpmem, reset, cnt);

	if (reset) {
		nrfx_ipc_gpmem_set(IPC_MEM_IDX, 0);
		return -EFAULT;
	}
	if (prv_cnt == cnt) {
		return -EBUSY;
	}

	prv_cnt = cnt;
	return 0;
}
