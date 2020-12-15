/**
 * @file main.c
 * @brief Application main entry point
 *
 * Copyright (c) 2020 Laird Connectivity
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
#define LOG_LEVEL LOG_LEVEL_DBG
LOG_MODULE_REGISTER(main);

#define MAIN_LOG_ERR(...) LOG_ERR(__VA_ARGS__)
#define MAIN_LOG_WRN(...) LOG_WRN(__VA_ARGS__)
#define MAIN_LOG_INF(...) LOG_INF(__VA_ARGS__)
#define MAIN_LOG_DBG(...) LOG_DBG(__VA_ARGS__)

/******************************************************************************/
/* Includes                                                                   */
/******************************************************************************/
#include <stdio.h>
#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr.h>
#include <version.h>
#include <shell/shell.h>
#include <shell/shell_uart.h>

#include "led_configuration.h"
#include "lte.h"
#include "laird_power.h"
#include "dis.h"
#include "aws.h"
#include "FrameworkIncludes.h"
#include "laird_utility_macros.h"
#include "string_util.h"
#include "app_version.h"

#ifdef CONFIG_MCUMGR
#include "mcumgr_wrapper.h"
#endif

/******************************************************************************/
/* Local Constant, Macro and Type Definitions                                 */
/******************************************************************************/
#define WAIT_TIME_BEFORE_RETRY_TICKS K_SECONDS(10)

#define NUMBER_OF_IMEI_DIGITS_TO_USE_IN_DEV_NAME 7

enum CREDENTIAL_TYPE { CREDENTIAL_CERT, CREDENTIAL_KEY };

typedef void (*app_state_function_t)(void);

#if defined(CONFIG_SHELL) && defined(CONFIG_MODEM_HL7800)
#define APN_MSG "APN: [%s]"
#endif

/******************************************************************************/
/* Global Data Definitions                                                    */
/******************************************************************************/
#if defined(CONFIG_SHELL) && defined(CONFIG_MODEM_HL7800)
extern struct mdm_hl7800_apn *lte_apn_config;
#endif

/******************************************************************************/
/* Local Data Definitions                                                     */
/******************************************************************************/
K_SEM_DEFINE(lte_ready_sem, 0, 1);

static FwkMsgReceiver_t cloudMsgReceiver;
static bool appReady = false;

static app_state_function_t appState;
struct lte_status *lteInfo;

K_MSGQ_DEFINE(cloudQ, FWK_QUEUE_ENTRY_SIZE, CONFIG_CLOUD_QUEUE_SIZE,
	      FWK_QUEUE_ALIGNMENT);

static struct k_timer fifo_timer;

/******************************************************************************/
/* Local Function Prototypes                                                  */
/******************************************************************************/
static void initializeCloudMsgReceiver(void);
static void appStateWaitForLte(void);
static void appStateStartup(void);
static void appStateLteConnected(void);

static void appSetNextState(app_state_function_t next);
static const char *getAppStateString(app_state_function_t state);

static void lteEvent(enum lte_event event);
static void softwareReset(uint32_t DelayMs);

static void configure_leds(void);

static void cloud_fifo_monitor_isr(struct k_timer *timer_id);

/******************************************************************************/
/* Global Function Definitions                                                */
/******************************************************************************/
void main(void)
{
	int rc;

	printk("\nCar MQTT v%s\n", APP_VERSION_STRING);

	configure_leds();

	Framework_Initialize();

	lteRegisterEventCallback(lteEvent);
	rc = lteInit();
	if (rc < 0) {
		MAIN_LOG_ERR("LTE init (%d)", rc);
		goto exit;
	}
	lteInfo = lteGetStatus();

	initializeCloudMsgReceiver();

	dis_initialize(APP_VERSION_STRING);

#ifdef CONFIG_MCUMGR
	mcumgr_wrapper_register_subsystems();
#endif

	k_timer_init(&fifo_timer, cloud_fifo_monitor_isr, NULL);
	k_timer_start(&fifo_timer,
		      K_SECONDS(CONFIG_CLOUD_FIFO_CHECK_RATE_SECONDS),
		      K_SECONDS(CONFIG_CLOUD_FIFO_CHECK_RATE_SECONDS));

	appReady = true;
	printk("\n!!!!!!!! App is ready! !!!!!!!!\n");

	appSetNextState(appStateStartup);

	while (true) {
		appState();
	}
exit:
	MAIN_LOG_ERR("Exiting main thread");
	return;
}

/******************************************************************************/
/* Framework                                                                  */
/******************************************************************************/
EXTERNED void Framework_AssertionHandler(char *file, int line)
{
	static atomic_t busy = ATOMIC_INIT(0);
	/* prevent recursion (buffer alloc fail, ...) */
	if (!busy) {
		atomic_set(&busy, 1);
		LOG_ERR("\r\n!---> Framework Assertion <---! %s:%d\r\n", file,
			line);
		LOG_ERR("Thread name: %s",
			log_strdup(k_thread_name_get(k_current_get())));
	}

#ifdef CONFIG_LAIRD_CONNECTIVITY_DEBUG
	/* breakpoint location */
	volatile bool wait = true;
	while (wait)
		;
#endif

	softwareReset(CONFIG_FWK_RESET_DELAY_MS);
}

/******************************************************************************/
/* Local Function Definitions                                                 */
/******************************************************************************/

static void initializeCloudMsgReceiver(void)
{
	cloudMsgReceiver.id = FWK_ID_CLOUD;
	cloudMsgReceiver.pQueue = &cloudQ;
	cloudMsgReceiver.rxBlockTicks = K_NO_WAIT; /* unused */
	cloudMsgReceiver.pMsgDispatcher = NULL; /* unused */
	Framework_RegisterReceiver(&cloudMsgReceiver);
}

static void lteEvent(enum lte_event event)
{
	switch (event) {
	case LTE_EVT_READY:
		k_sem_give(&lte_ready_sem);
		break;
	case LTE_EVT_DISCONNECTED:
		k_sem_reset(&lte_ready_sem);
		break;
	default:
		break;
	}
}

static const char *getAppStateString(app_state_function_t state)
{
	IF_RETURN_STRING(state, appStateStartup);
	IF_RETURN_STRING(state, appStateWaitForLte);
	IF_RETURN_STRING(state, appStateLteConnected);
	return "appStateUnknown";
}

static void appSetNextState(app_state_function_t next)
{
	MAIN_LOG_DBG("%s->%s", getAppStateString(appState),
		     getAppStateString(next));
	appState = next;
}

static void appStateStartup(void)
{
	appSetNextState(appStateWaitForLte);
}

static void appStateWaitForLte(void)
{
	if (!lteIsReady()) {
		/* Wait for LTE ready evt */
		k_sem_take(&lte_ready_sem, K_FOREVER);
	}
	appSetNextState(appStateLteConnected);
}
 // For an implementation set the above "appSetNextState" function call to the needed state.
static void appStateLteConnected(void)
{
	k_sleep(K_SECONDS(1));
}

static void softwareReset(uint32_t DelayMs)
{
#ifdef CONFIG_REBOOT
	LOG_ERR("Software Reset in %d milliseconds", DelayMs);
	k_sleep(K_MSEC(DelayMs));
	power_reboot_module(REBOOT_TYPE_NORMAL);
#endif
}

static void configure_leds(void)
{
	struct led_configuration c[] = {
		{ BLUE_LED1, LED1_DEV, LED1, LED_ACTIVE_HIGH },
		{ GREEN_LED2, LED2_DEV, LED2, LED_ACTIVE_HIGH },
		{ RED_LED3, LED3_DEV, LED3, LED_ACTIVE_HIGH },
		{ GREEN_LED4, LED4_DEV, LED4, LED_ACTIVE_HIGH }
	};
	led_init(c, ARRAY_SIZE(c));
}

/******************************************************************************/
/* Interrupt Service Routines                                                 */
/******************************************************************************/
/* The cloud (MQTT) queue isn't checked in all states.  Therefore, it needs to
 * be periodically checked so that other tasks don't overfill it
 */
static void cloud_fifo_monitor_isr(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);

	uint32_t numUsed = k_msgq_num_used_get(cloudMsgReceiver.pQueue);
	if (numUsed > CONFIG_CLOUD_PURGE_THRESHOLD) {
		size_t flushed = Framework_Flush(FWK_ID_CLOUD);
		if (flushed > 0) {
			LOG_WRN("Flushed %u cloud messages", flushed);
		}
	}
}
