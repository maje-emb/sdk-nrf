/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <hal/nrf_egu.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>

#include <nrfx_dppi.h>
/* Optional GPIO pin debugging via GPIOTE/DPPI on nRF54 (H- and L-series) */
#if defined(CONFIG_SOC_SERIES_NRF54HX) || defined(CONFIG_SOC_SERIES_NRF54LX)
#include <nrfx_gpiote.h>
#include <helpers/nrfx_gppi.h>
#include <hal/nrf_gpio.h>
#endif

#include <zephyr/logging/log.h>

#include "esb_peripherals.h"
#include "esb_ppi_api.h"

LOG_MODULE_DECLARE(esb, CONFIG_ESB_LOG_LEVEL);

static uint8_t radio_address_timer_stop;
static uint8_t timer_compare0_radio_disable;
static uint8_t timer_compare1_radio_txen;
static uint8_t disabled_phy_end_egu;
static uint8_t egu_timer_start;
static uint8_t egu_ramp_up;
static uint8_t radio_end_timer_start;

static nrf_dppi_channel_group_t ramp_up_dppi_group;

#if defined(CONFIG_SOC_SERIES_NRF54HX) || defined(CONFIG_SOC_SERIES_NRF54LX)
#define ESB_GPIO_DEBUG_PIN NRF_GPIO_PIN_MAP(1, 10)
#endif

#if defined(CONFIG_SOC_SERIES_NRF54LX)
static const nrfx_dppi_t esb_dppi_radio_domain = NRFX_DPPI_INSTANCE(10);
static const nrfx_dppi_t esb_dppi_gpio_domain  = NRFX_DPPI_INSTANCE(20);
static const nrfx_gpiote_t esb_gpiote          = NRFX_GPIOTE_INSTANCE(20);
#elif defined(CONFIG_SOC_SERIES_NRF54HX)
static const nrfx_dppi_t esb_dppi_radio_domain = NRFX_DPPI_INSTANCE(020);
static const nrfx_dppi_t esb_dppi_gpio_domain  = NRFX_DPPI_INSTANCE(020);
static const nrfx_gpiote_t esb_gpiote          = NRFX_GPIOTE_INSTANCE(0);
#else
#error "No DPPI domain defined"
#endif

static uint8_t esb_dbg_gppi_bridge_ready;
static uint8_t esb_dbg_gppi_bridge_end;
static uint8_t esb_dbg_dppi_radio_ready;
static uint8_t esb_dbg_dppi_radio_end;
static uint8_t esb_dbg_dppi_gpio_ready;
static uint8_t esb_dbg_dppi_gpio_end;
static uint8_t esb_dbg_gpiote_chan_ready_end;
static bool esb_dbg_gpio_active;

static int esb_debug_gpio_setup(void)
{
    nrfx_err_t err;

    if (!(IS_ENABLED(CONFIG_SOC_SERIES_NRF54HX) || IS_ENABLED(CONFIG_SOC_SERIES_NRF54LX))) {
        return 0;
    }

    if (!nrfx_gpiote_init_check(&esb_gpiote)) {
        err = nrfx_gpiote_init(&esb_gpiote, 0);
        if (err != NRFX_SUCCESS) {
            LOG_ERR("GPIOTE init failed: %d", err);
            return -ENODEV;
        }
    }

    err = nrfx_gpiote_channel_alloc(&esb_gpiote, &esb_dbg_gpiote_chan_ready_end);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("GPIOTE channel alloc failed: %d", err);
        return -ENODEV;
    }

    const nrfx_gpiote_output_config_t out_cfg = NRFX_GPIOTE_DEFAULT_OUTPUT_CONFIG;
    const nrfx_gpiote_task_config_t task_cfg = {
        .task_ch = esb_dbg_gpiote_chan_ready_end,
        .polarity = NRF_GPIOTE_POLARITY_TOGGLE,
        .init_val = NRF_GPIOTE_INITIAL_VALUE_LOW,
    };

    err = nrfx_gpiote_output_configure(&esb_gpiote, ESB_GPIO_DEBUG_PIN, &out_cfg, &task_cfg);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("GPIOTE output configure failed: %d", err);
        return -ENODEV;
    }

#if defined(CONFIG_SOC_SERIES_NRF54LX)
    if (nrfx_gppi_channel_alloc(&esb_dbg_gppi_bridge_ready) != NRFX_SUCCESS ||
        nrfx_gppi_channel_alloc(&esb_dbg_gppi_bridge_end)   != NRFX_SUCCESS) {
        LOG_ERR("GPPI bridge channel alloc failed");
        return -ENODEV;
    }
#endif

    err = nrfx_dppi_channel_alloc(&esb_dppi_radio_domain, &esb_dbg_dppi_radio_ready);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("DPPI radio READY alloc failed: %d", err);
        return -ENODEV;
    }
    err = nrfx_dppi_channel_alloc(&esb_dppi_radio_domain, &esb_dbg_dppi_radio_end);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("DPPI radio END alloc failed: %d", err);
        return -ENODEV;
    }

    err = nrfx_dppi_channel_alloc(&esb_dppi_gpio_domain, &esb_dbg_dppi_gpio_ready);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("DPPI gpio READY alloc failed: %d", err);
        return -ENODEV;
    }
    err = nrfx_dppi_channel_alloc(&esb_dppi_gpio_domain, &esb_dbg_dppi_gpio_end);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("DPPI gpio END alloc failed: %d", err);
        return -ENODEV;
    }

#if defined(CONFIG_SOC_SERIES_NRF54LX)
    err = nrfx_gppi_edge_connection_setup(esb_dbg_gppi_bridge_ready,
                                          &esb_dppi_radio_domain, esb_dbg_dppi_radio_ready,
                                          &esb_dppi_gpio_domain,  esb_dbg_dppi_gpio_ready);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("GPPI bridge READY setup failed: %d", err);
        return -ENODEV;
    }
    err = nrfx_gppi_edge_connection_setup(esb_dbg_gppi_bridge_end,
                                          &esb_dppi_radio_domain, esb_dbg_dppi_radio_end,
                                          &esb_dppi_gpio_domain,  esb_dbg_dppi_gpio_end);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("GPPI bridge END setup failed: %d", err);
        return -ENODEV;
    }
#endif

    nrf_radio_publish_set(NRF_RADIO, NRF_RADIO_EVENT_READY, esb_dbg_dppi_radio_ready);
    nrf_radio_publish_set(NRF_RADIO, ESB_RADIO_EVENT_END,   esb_dbg_dppi_radio_end);

    nrf_gpiote_subscribe_set(esb_gpiote.p_reg,
                             nrfx_gpiote_set_task_address_get(&esb_gpiote, ESB_GPIO_DEBUG_PIN),
                             esb_dbg_dppi_gpio_ready);
    nrf_gpiote_subscribe_set(esb_gpiote.p_reg,
                             nrfx_gpiote_clr_task_address_get(&esb_gpiote, ESB_GPIO_DEBUG_PIN),
                             esb_dbg_dppi_gpio_end);

#if defined(CONFIG_SOC_SERIES_NRF54LX)
    nrfx_gppi_channels_enable(NRFX_BIT(esb_dbg_gppi_bridge_ready) |
                              NRFX_BIT(esb_dbg_gppi_bridge_end));
#endif
    (void)nrfx_dppi_channel_enable(&esb_dppi_radio_domain, esb_dbg_dppi_radio_ready);
    (void)nrfx_dppi_channel_enable(&esb_dppi_radio_domain, esb_dbg_dppi_radio_end);
    (void)nrfx_dppi_channel_enable(&esb_dppi_gpio_domain,  esb_dbg_dppi_gpio_ready);
    (void)nrfx_dppi_channel_enable(&esb_dppi_gpio_domain,  esb_dbg_dppi_gpio_end);

    nrfx_gpiote_out_task_enable(&esb_gpiote, ESB_GPIO_DEBUG_PIN);

    esb_dbg_gpio_active = true;
    return 0;
}

#endif /* CONFIG_SOC_SERIES_NRF54HX || CONFIG_SOC_SERIES_NRF54LX */

void esb_ppi_for_txrx_set(bool rx, bool timer_start)
{
	uint32_t channels_mask;

	nrf_egu_event_clear(ESB_EGU, ESB_EGU_EVENT);
	nrf_egu_event_clear(ESB_EGU, ESB_EGU_DPPI_EVENT);

	nrf_egu_publish_set(ESB_EGU, ESB_EGU_EVENT, egu_timer_start);
	nrf_egu_publish_set(ESB_EGU, ESB_EGU_DPPI_EVENT, egu_ramp_up);

	nrf_dppi_channels_include_in_group(ESB_DPPIC, BIT(egu_ramp_up), ramp_up_dppi_group);

	nrf_egu_subscribe_set(ESB_EGU, ESB_EGU_DPPI_TASK, egu_timer_start);
	nrf_radio_subscribe_set(NRF_RADIO, rx ? NRF_RADIO_TASK_RXEN : NRF_RADIO_TASK_TXEN,
				egu_ramp_up);
	nrf_dppi_subscribe_set(ESB_DPPIC,
				nrf_dppi_group_disable_task_get((uint8_t)ramp_up_dppi_group),
				egu_ramp_up);

	nrf_egu_subscribe_set(ESB_EGU, ESB_EGU_TASK, disabled_phy_end_egu);

	if (timer_start) {
		nrf_timer_subscribe_set(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_TASK_START,
					egu_timer_start);
	}

	channels_mask = (BIT(egu_timer_start) |
			 BIT(egu_ramp_up));

	nrf_dppi_channels_enable(ESB_DPPIC, channels_mask);
}

void esb_ppi_for_txrx_clear(bool rx, bool timer_start)
{
	uint32_t channels_mask;

	channels_mask = (BIT(egu_timer_start) |
			 BIT(egu_ramp_up));

	nrf_dppi_channels_disable(ESB_DPPIC, channels_mask);

	nrf_egu_publish_clear(ESB_EGU, ESB_EGU_EVENT);
	nrf_egu_publish_clear(ESB_EGU, ESB_EGU_DPPI_EVENT);

	nrf_egu_subscribe_clear(ESB_EGU, ESB_EGU_DPPI_TASK);
	nrf_radio_subscribe_clear(NRF_RADIO, rx ? NRF_RADIO_TASK_RXEN : NRF_RADIO_TASK_TXEN);
	nrf_dppi_subscribe_clear(ESB_DPPIC,
				 nrf_dppi_group_disable_task_get((uint8_t)ramp_up_dppi_group));
	nrf_egu_subscribe_clear(ESB_EGU, ESB_EGU_TASK);

	nrf_dppi_channels_remove_from_group(ESB_DPPIC, BIT(egu_ramp_up), ramp_up_dppi_group);

	if (timer_start) {
		nrf_timer_subscribe_clear(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_TASK_START);
	}
}

void esb_ppi_for_fem_set(void)
{
	nrf_egu_publish_set(ESB_EGU, ESB_EGU_EVENT, egu_timer_start);
	nrf_timer_subscribe_set(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_TASK_START,
				egu_timer_start);

	nrf_dppi_channels_enable(ESB_DPPIC, BIT(egu_timer_start));
}

void esb_ppi_for_fem_clear(void)
{
	nrf_dppi_channels_disable(ESB_DPPIC, BIT(egu_timer_start));

	nrf_egu_publish_clear(ESB_EGU, ESB_EGU_EVENT);
	nrf_timer_subscribe_clear(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_TASK_START);
}

void esb_ppi_for_retransmission_set(void)
{
	nrf_egu_event_clear(ESB_EGU, ESB_EGU_EVENT);

	nrf_timer_publish_set(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE1,
			      timer_compare1_radio_txen);

	nrf_radio_subscribe_set(NRF_RADIO, NRF_RADIO_TASK_TXEN, timer_compare1_radio_txen);
	nrf_egu_subscribe_set(ESB_EGU, ESB_EGU_TASK, disabled_phy_end_egu);

	if (IS_ENABLED(CONFIG_ESB_FAST_SWITCHING)) {
		nrf_radio_subscribe_set(NRF_RADIO, NRF_RADIO_TASK_RXEN, disabled_phy_end_egu);
	}

	nrf_dppi_channels_enable(ESB_DPPIC, BIT(timer_compare1_radio_txen));
}

void esb_ppi_for_retransmission_clear(void)
{
	nrf_dppi_channels_disable(ESB_DPPIC, BIT(timer_compare1_radio_txen));

	nrf_timer_publish_clear(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE1);

	nrf_radio_subscribe_clear(NRF_RADIO, NRF_RADIO_TASK_TXEN);
	nrf_egu_subscribe_clear(ESB_EGU, ESB_EGU_TASK);

	if (IS_ENABLED(CONFIG_ESB_FAST_SWITCHING)) {
		nrf_radio_subscribe_clear(NRF_RADIO, NRF_RADIO_TASK_RXEN);
	}
}

void esb_ppi_for_wait_for_ack_set(void)
{
	uint32_t channels_mask;

	nrf_radio_publish_set(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS, radio_address_timer_stop);
	nrf_timer_publish_set(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE0,
			      timer_compare0_radio_disable);

	nrf_timer_subscribe_set(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_TASK_STOP,
				radio_address_timer_stop);

	nrf_radio_subscribe_set(NRF_RADIO, NRF_RADIO_TASK_DISABLE, timer_compare0_radio_disable);

	channels_mask = (BIT(radio_address_timer_stop) |
			 BIT(timer_compare0_radio_disable));

	nrf_dppi_channels_enable(ESB_DPPIC, channels_mask);
}

void esb_ppi_for_wait_for_ack_clear(void)
{
	uint32_t channels_mask;

	channels_mask = (BIT(radio_address_timer_stop) |
			 BIT(timer_compare0_radio_disable));

	nrf_dppi_channels_disable(ESB_DPPIC, channels_mask);

	nrf_radio_publish_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_timer_publish_clear(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE0);

	nrf_timer_subscribe_clear(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_TASK_STOP);

	nrf_radio_subscribe_clear(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
}

void esb_ppi_for_wait_for_rx_set(void)
{
	uint32_t channels_mask;

	nrf_radio_publish_set(NRF_RADIO, ESB_RADIO_EVENT_END, radio_end_timer_start);
	nrf_timer_subscribe_set(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_TASK_START,
				radio_end_timer_start);

	channels_mask = (BIT(radio_end_timer_start));

	nrf_dppi_channels_enable(ESB_DPPIC, channels_mask);
}

void esb_ppi_for_wait_for_rx_clear(void)
{
	uint32_t channels_mask;

	channels_mask = (BIT(radio_end_timer_start));

	nrf_dppi_channels_disable(ESB_DPPIC, channels_mask);

	nrf_radio_publish_clear(NRF_RADIO, ESB_RADIO_EVENT_END);
	nrf_timer_subscribe_clear(ESB_NRF_TIMER_INSTANCE, NRF_TIMER_TASK_START);
}

uint32_t esb_ppi_radio_disabled_get(void)
{
	return disabled_phy_end_egu;
}

int esb_ppi_init(void)
{
	nrfx_err_t err;

#if defined(ESB_DPPI_FIXED)

	radio_address_timer_stop = ESB_DPPI_FIRST_FIXED_CHANNEL + 0;
	timer_compare0_radio_disable = ESB_DPPI_FIRST_FIXED_CHANNEL + 1;
	timer_compare1_radio_txen = ESB_DPPI_FIRST_FIXED_CHANNEL + 2;
	disabled_phy_end_egu = ESB_DPPI_FIRST_FIXED_CHANNEL + 3;
	egu_timer_start = ESB_DPPI_FIRST_FIXED_CHANNEL + 4;
	egu_ramp_up = ESB_DPPI_FIRST_FIXED_CHANNEL + 5;
	radio_end_timer_start = ESB_DPPI_FIRST_FIXED_CHANNEL + 6;
	ramp_up_dppi_group = ESB_DPPI_FIRST_FIXED_GROUP + 0;

	ARG_UNUSED(err);

#else

	nrfx_dppi_t dppi = NRFX_DPPI_INSTANCE(ESB_DPPIC_INSTANCE_NO);

	err = nrfx_dppi_channel_alloc(&dppi, &radio_address_timer_stop);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_alloc(&dppi, &timer_compare0_radio_disable);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_alloc(&dppi, &timer_compare1_radio_txen);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_alloc(&dppi, &disabled_phy_end_egu);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_alloc(&dppi, &egu_timer_start);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_alloc(&dppi, &egu_ramp_up);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	if (IS_ENABLED(CONFIG_ESB_NEVER_DISABLE_TX)) {
		err = nrfx_dppi_channel_alloc(&dppi, &radio_end_timer_start);
		if (err != NRFX_SUCCESS) {
			goto error;
		}
	}

	err = nrfx_dppi_group_alloc(&dppi, &ramp_up_dppi_group);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gppi_group_alloc failed with: %d\n", err);
		return -ENODEV;
	}

#endif /* defined(ESB_DPPI_FIXED) */

	nrf_radio_publish_set(NRF_RADIO, NRF_RADIO_EVENT_DISABLED, disabled_phy_end_egu);
	if (IS_ENABLED(CONFIG_ESB_FAST_SWITCHING)) {
		nrf_radio_publish_set(NRF_RADIO, NRF_RADIO_EVENT_PHYEND, disabled_phy_end_egu);
	}
	nrf_dppi_channels_enable(ESB_DPPIC, BIT(disabled_phy_end_egu));


#if defined(CONFIG_SOC_SERIES_NRF54HX) || defined(CONFIG_SOC_SERIES_NRF54LX)
	esb_debug_gpio_setup();
#endif

	return 0;

#if !defined(ESB_DPPI_FIXED)
error:
	LOG_ERR("gppi_channel_alloc failed with: %d\n", err);
	return -ENODEV;
#endif /* !defined(ESB_DPPI_FIXED) */
}

void esb_ppi_disable_all(void)
{
	uint32_t channels_mask = (BIT(egu_ramp_up) |
				  BIT(disabled_phy_end_egu) |
				  BIT(egu_timer_start) |
				  BIT(radio_address_timer_stop) |
				  BIT(timer_compare0_radio_disable) |
				  BIT(radio_end_timer_start) |
				  (IS_ENABLED(CONFIG_ESB_NEVER_DISABLE_TX) ?
					BIT(timer_compare1_radio_txen) : 0));

	nrf_dppi_channels_disable(ESB_DPPIC, channels_mask);

#if defined(CONFIG_SOC_SERIES_NRF54HX) || defined(CONFIG_SOC_SERIES_NRF54LX)
	/* Also disable debug GPIO DPPI channels if active. */
	if (esb_dbg_gpio_active) {
		(void)nrfx_dppi_channel_disable(&esb_dppi_radio_domain, esb_dbg_dppi_radio_ready);
		(void)nrfx_dppi_channel_disable(&esb_dppi_radio_domain, esb_dbg_dppi_radio_end);
		(void)nrfx_dppi_channel_disable(&esb_dppi_gpio_domain,  esb_dbg_dppi_gpio_ready);
		(void)nrfx_dppi_channel_disable(&esb_dppi_gpio_domain,  esb_dbg_dppi_gpio_end);
#if defined(CONFIG_SOC_SERIES_NRF54LX)
		nrfx_gppi_channels_disable(NRFX_BIT(esb_dbg_gppi_bridge_ready) |
					   NRFX_BIT(esb_dbg_gppi_bridge_end));
#endif
	}
#endif
}

void esb_ppi_deinit(void)
{
	nrfx_err_t err;

	nrf_dppi_channels_disable(ESB_DPPIC, BIT(disabled_phy_end_egu));
	nrf_radio_publish_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	if (IS_ENABLED(CONFIG_ESB_FAST_SWITCHING)) {
		nrf_radio_publish_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
	}

#if defined(ESB_DPPI_FIXED)

	ARG_UNUSED(err);

#else

	nrfx_dppi_t dppi = NRFX_DPPI_INSTANCE(ESB_DPPIC_INSTANCE_NO);

	err = nrfx_dppi_channel_free(&dppi, radio_address_timer_stop);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_free(&dppi, timer_compare0_radio_disable);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_free(&dppi, timer_compare1_radio_txen);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_free(&dppi, disabled_phy_end_egu);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_free(&dppi, egu_timer_start);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	err = nrfx_dppi_channel_free(&dppi, egu_ramp_up);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

	if (IS_ENABLED(CONFIG_ESB_NEVER_DISABLE_TX)) {
		err = nrfx_dppi_channel_free(&dppi, radio_end_timer_start);
		if (err != NRFX_SUCCESS) {
			goto error;
		}
	}

	err = nrfx_dppi_group_free(&dppi, ramp_up_dppi_group);
	if (err != NRFX_SUCCESS) {
		goto error;
	}

#endif /* defined(ESB_DPPI_FIXED) */

	return;

#if !defined(ESB_DPPI_FIXED)
/* Should not happen. */
error:
	__ASSERT(false, "Failed to free DPPI resources");
#endif
}
