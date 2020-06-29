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
LOG_MODULE_REGISTER(oob_main);

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
#include "aws.h"
#include "nv.h"
#include "ble_cellular_service.h"
#include "ble_aws_service.h"
#include "ble_power_service.h"
#include "laird_power.h"
#include "dis.h"
#include "bootloader.h"
#include "FrameworkIncludes.h"
#include "laird_utility_macros.h"
#include "single_peripheral.h"
#include "print_thread.h"
#include "string_util.h"
#include "app_version.h"

#if CONFIG_BL654_SENSOR
#include "bl654_sensor.h"
#endif

#if CONFIG_BLUEGRASS
#include "bluegrass.h"
#endif

#ifdef CONFIG_LWM2M
#include "lwm2m_client.h"
#include "ble_lwm2m_service.h"
#endif

/******************************************************************************/
/* Local Constant, Macro and Type Definitions                                 */
/******************************************************************************/
#define WAIT_TIME_BEFORE_RETRY_TICKS K_SECONDS(10)

#define NUMBER_OF_IMEI_DIGITS_TO_USE_IN_DEV_NAME 7

enum CREDENTIAL_TYPE { CREDENTIAL_CERT, CREDENTIAL_KEY };

enum APP_ERROR {
	APP_ERR_NOT_READY = -1,
	APP_ERR_COMMISSION_DISALLOWED = -2,
	APP_ERR_CRED_TOO_LARGE = -3,
	APP_ERR_UNKNOWN_CRED = -4,
	APP_ERR_READ_CERT = -5,
	APP_ERR_READ_KEY = -6,
};

typedef void (*app_state_function_t)(void);

/******************************************************************************/
/* Local Data Definitions                                                     */
/******************************************************************************/
K_SEM_DEFINE(lte_ready_sem, 0, 1);
K_SEM_DEFINE(rx_cert_sem, 0, 1);

static bool initShadow = true;
static bool resolveAwsServer = true;

static bool commissioned;
static bool allowCommissioning = false;
static bool appReady = false;
static bool devCertSet;
static bool devKeySet;

static app_state_function_t appState;
struct lte_status *lteInfo;

static FwkMsgReceiver_t awsMsgReceiver;

K_MSGQ_DEFINE(awsQ, FWK_QUEUE_ENTRY_SIZE, 16, FWK_QUEUE_ALIGNMENT);

static struct k_timer awsKeepAliveTimer;

/******************************************************************************/
/* Local Function Prototypes                                                  */
/******************************************************************************/
static void appStateAwsSendSensorData(void);
static void appStateAwsInitShadow(void);
static void appStateAwsConnect(void);
static void appStateAwsDisconnect(void);
static void appStateAwsResolveServer(void);
static void appStateWaitForLte(void);
static void appStateCommissionDevice(void);
static void appStateStartup(void);
static void appStateLteConnectedAws(void);

static void appSetNextState(app_state_function_t next);
static const char *getAppStateString(app_state_function_t state);

static void setAwsStatusWrapper(enum aws_status status);

static void initializeBle(const char *imei);
static void initializeAwsMsgReceiver(void);
static void awsMsgHandler(void);
static void awsSvcEvent(enum aws_svc_event event);
static int setAwsCredentials(void);
static void lteEvent(enum lte_event event);
static void softwareReset(uint32_t DelayMs);

#ifdef CONFIG_LWM2M
static void appStateInitLwm2mClient(void);
static void appStateLwm2m(void);
static void lwm2mMsgHandler(void);
#endif

static void configure_leds(void);

static void StartKeepAliveTimer(void);
static void AwsKeepAliveTimerCallbackIsr(struct k_timer *timer_id);

/******************************************************************************/
/* Global Function Definitions                                                */
/******************************************************************************/
void main(void)
{
	int rc;

#ifdef CONFIG_LWM2M
	printk("\nOOB demo - LwM2M v%s\n", APP_VERSION_STRING);
#else
	printk("\nOOB demo - AWS v%s\n", APP_VERSION_STRING);
#endif

	configure_leds();

	Framework_Initialize();

	/* Init NV storage */
	rc = nvInit();
	if (rc < 0) {
		MAIN_LOG_ERR("NV init (%d)", rc);
		goto exit;
	}

	nvReadCommissioned(&commissioned);

	/* init LTE */
	lteRegisterEventCallback(lteEvent);
	rc = lteInit();
	if (rc < 0) {
		MAIN_LOG_ERR("LTE init (%d)", rc);
		goto exit;
	}
	lteInfo = lteGetStatus();

	/* init AWS */
	rc = awsInit();
	if (rc != 0) {
		goto exit;
	}

	initializeAwsMsgReceiver();
	k_timer_init(&awsKeepAliveTimer, AwsKeepAliveTimerCallbackIsr, NULL);

	initializeBle(lteInfo->IMEI);
	single_peripheral_initialize();

#if CONFIG_BL654_SENSOR
	bl654_sensor_initialize();
#endif

#if CONFIG_BLUEGRASS
	Bluegrass_Initialize(awsMsgReceiver.pQueue);
#endif

	dis_initialize(APP_VERSION_STRING);

	/* Start up BLE portion of the demo */
	cell_svc_init();
	cell_svc_set_imei(lteInfo->IMEI);
	cell_svc_set_fw_ver(lteInfo->radio_version);
	cell_svc_set_iccid(lteInfo->ICCID);
	cell_svc_set_serial_number(lteInfo->serialNumber);

	/* Setup the power service */
	power_svc_init();
	power_init();

#ifdef CONFIG_LAIRDCONNECTIVITY_BLR
	bootloader_init();
#endif
	rc = aws_svc_init(lteInfo->IMEI);
	if (rc != 0) {
		goto exit;
	}
	aws_svc_set_event_callback(awsSvcEvent);
	if (commissioned) {
		aws_svc_set_status(NULL, AWS_STATUS_DISCONNECTED);
	} else {
		aws_svc_set_status(NULL, AWS_STATUS_NOT_PROVISIONED);
	}

#ifdef CONFIG_LWM2M
	ble_lwm2m_service_init();
#endif

	appReady = true;
	printk("\n!!!!!!!! App is ready! !!!!!!!!\n");

	appSetNextState(appStateStartup);

#if CONFIG_PRINT_THREAD_LIST
	print_thread_list();
#endif

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

#if CONFIG_LAIRD_CONNECTIVITY_DEBUG
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
static void initializeBle(const char *imei)
{
	int err;
	static char bleDevName[sizeof(CONFIG_BT_DEVICE_NAME "-") +
			       NUMBER_OF_IMEI_DIGITS_TO_USE_IN_DEV_NAME];
	int devNameEnd;
	int imeiEnd;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return;
	}

	LOG_INF("Bluetooth initialized");

	/* add digits of IMEI to dev name */
	strncpy(bleDevName, CONFIG_BT_DEVICE_NAME "-", sizeof(bleDevName) - 1);
	devNameEnd = strlen(bleDevName);
	imeiEnd = strlen(imei);
	strncat(bleDevName + devNameEnd,
		imei + imeiEnd - NUMBER_OF_IMEI_DIGITS_TO_USE_IN_DEV_NAME,
		NUMBER_OF_IMEI_DIGITS_TO_USE_IN_DEV_NAME);
	err = bt_set_name((const char *)bleDevName);
	if (err) {
		LOG_ERR("Failed to set device name (%d)", err);
	} else {
		LOG_INF("BLE device name set to [%s]", log_strdup(bleDevName));
	}
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

static void appStateAwsSendSensorData(void)
{
	/* If decommissioned then disconnect. */
	if (!commissioned || !awsConnected()) {
		appSetNextState(appStateAwsDisconnect);
		led_turn_off(GREEN_LED2);
		return;
	}

	/* Process messages until there is an error. */
	awsMsgHandler();

	if (k_msgq_num_used_get(&awsQ) != 0) {
		MAIN_LOG_WRN("%u unsent messages", k_msgq_num_used_get(&awsQ));
	}
}

static const char *getAppStateString(app_state_function_t state)
{
#ifdef CONFIG_LWM2M
	IF_RETURN_STRING(state, appStateLwm2m);
	IF_RETURN_STRING(state, appStateInitLwm2mClient);
#endif
	IF_RETURN_STRING(state, appStateAwsSendSensorData);
	IF_RETURN_STRING(state, appStateAwsConnect);
	IF_RETURN_STRING(state, appStateAwsDisconnect);
	IF_RETURN_STRING(state, appStateWaitForLte);
	IF_RETURN_STRING(state, appStateLteConnectedAws);
	IF_RETURN_STRING(state, appStateAwsResolveServer);
	IF_RETURN_STRING(state, appStateAwsInitShadow);
	IF_RETURN_STRING(state, appStateCommissionDevice);
	IF_RETURN_STRING(state, appStateStartup);
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
#ifdef CONFIG_LWM2M
	appSetNextState(appStateWaitForLte);
#else
	if (commissioned && setAwsCredentials() == 0) {
		appSetNextState(appStateWaitForLte);
	} else {
		appSetNextState(appStateCommissionDevice);
	}
#endif
}

/* This function will throw away sensor data if it can't send it.
 * Subscription failures can occur even when the return value was success.
 * An AWS disconnect callback is used to send a message to unblock this queue.
 * This allows the UI (green LED) to be updated immediately.
 */
static void awsMsgHandler(void)
{
	int rc = 0;
	FwkMsg_t *pMsg;
	bool freeMsg;

	while (rc == 0) {
		led_turn_on(GREEN_LED2);
		/* Remove sensor/gateway data from queue and send it to cloud.
		 * Block if there are not any messages.
		 * The keep alive message (RSSI) occurs every ~30 seconds.
		 */
		rc = -EINVAL;
		pMsg = NULL;
		Framework_Receive(awsMsgReceiver.pQueue, &pMsg, K_FOREVER);
		if (pMsg == NULL) {
			return;
		}
		freeMsg = true;

		/* BL654 data is sent to the gateway topic.  If Bluegrass is enabled,
		 * then sensor data (BT510) is sent to individual topics.  It also allows
		 * AWS to configure sensors.
		 */
		switch (pMsg->header.msgCode) {
		case FMC_BL654_SENSOR_EVENT: {
			BL654SensorMsg_t *pBmeMsg = (BL654SensorMsg_t *)pMsg;
			rc = awsPublishBl654SensorData(pBmeMsg->temperatureC,
						       pBmeMsg->humidityPercent,
						       pBmeMsg->pressurePa);
		} break;

		case FMC_AWS_KEEP_ALIVE: {
			/* Periodically sending the RSSI keeps AWS connection open. */
			lteInfo = lteGetStatus();
			rc = awsPublishPinnacleData(lteInfo->rssi,
						    lteInfo->sinr);
			StartKeepAliveTimer();
		} break;

		case FMC_AWS_DECOMMISSION:
		case FMC_AWS_DISCONNECTED:
			/* Message is used to unblock queue. */
			break;

		default:
#if CONFIG_BLUEGRASS
			rc = Bluegrass_MsgHandler(pMsg, &freeMsg);
#endif
			break;
		}

		if (freeMsg) {
			BufferPool_Free(pMsg);
		}

		/* A publish error will most likely result in an immediate disconnect.
		 * A disconnect due to a subscription error may be delayed.
		 *
		 * When the permissions change on a sensor topic
		 * (sensor enabled in Bluegrass) the first subscription will result in
		 * a disconnect. The second attempt will work.
		 */
		led_turn_off(GREEN_LED2);
		if (rc == 0) {
			k_sleep(K_MSEC(
				CONFIG_AWS_DATA_SEND_LED_OFF_DURATION_MILLISECONDS));
		}
	}
}

/* The shadow init is only sent once after the very first connect.*/
static void appStateAwsInitShadow(void)
{
	int rc = 0;

	if (initShadow) {
		awsGenerateGatewayTopics(lteInfo->IMEI);
		/* Fill in base shadow info and publish */
		awsSetShadowAppFirmwareVersion(APP_VERSION_STRING);
		awsSetShadowKernelVersion(KERNEL_VERSION_STRING);
		awsSetShadowIMEI(lteInfo->IMEI);
		awsSetShadowICCID(lteInfo->ICCID);
		awsSetShadowRadioFirmwareVersion(lteInfo->radio_version);
		awsSetShadowRadioSerialNumber(lteInfo->serialNumber);

		MAIN_LOG_INF("Send persistent shadow data");
		rc = awsPublishShadowPersistentData();
	}

	if (rc != 0) {
		LOG_ERR("%d", rc);
		appSetNextState(appStateAwsDisconnect);
	} else {
		initShadow = false;
		appSetNextState(appStateAwsSendSensorData);
		StartKeepAliveTimer();
#if CONFIG_BLUEGRASS
		Bluegrass_ConnectedCallback();
#endif
	}
}

void awsDisconnectCallback(void)
{
	FRAMEWORK_MSG_CREATE_AND_SEND(FWK_ID_RESERVED, FWK_ID_AWS,
				      FMC_AWS_DISCONNECTED);
}

static void appStateAwsConnect(void)
{
	if ((devCertSet != true) || (devKeySet != true)) {
		appSetNextState(appStateCommissionDevice);
		return;
	}

	if (!lteIsReady()) {
		appSetNextState(appStateWaitForLte);
		return;
	}

	setAwsStatusWrapper(AWS_STATUS_CONNECTING);

	if (awsConnect() != 0) {
		MAIN_LOG_ERR("Could not connect to AWS");
		setAwsStatusWrapper(AWS_STATUS_CONNECTION_ERR);

		/* wait some time before trying to re-connect */
		k_sleep(WAIT_TIME_BEFORE_RETRY_TICKS);
		return;
	}

	setAwsStatusWrapper(AWS_STATUS_CONNECTED);

	appSetNextState(appStateAwsInitShadow);
}

static bool areCertsSet(void)
{
	return (devCertSet && devKeySet);
}

static void appStateAwsDisconnect(void)
{
	awsDisconnect();

	setAwsStatusWrapper(AWS_STATUS_DISCONNECTED);

	FRAMEWORK_MSG_CREATE_AND_BROADCAST(FWK_ID_RESERVED,
					   FMC_AWS_DISCONNECTED);

#if CONFIG_BLUEGRASS
	Bluegrass_DisconnectedCallback();
#endif

	appSetNextState(appStateAwsConnect);
}

static void appStateAwsResolveServer(void)
{
	if (awsGetServerAddr() != 0) {
		MAIN_LOG_ERR("Could not get server address");
		/* wait some time before trying to resolve address again */
		k_sleep(WAIT_TIME_BEFORE_RETRY_TICKS);
		return;
	}
	resolveAwsServer = false;
	appSetNextState(appStateAwsConnect);
}

static void appStateWaitForLte(void)
{
	setAwsStatusWrapper(AWS_STATUS_DISCONNECTED);

	if (!lteIsReady()) {
		/* Wait for LTE ready evt */
		k_sem_take(&lte_ready_sem, K_FOREVER);
	}

#ifdef CONFIG_LWM2M
	appSetNextState(appStateInitLwm2mClient);
#else
	appSetNextState(appStateLteConnectedAws);
#endif
}

static void appStateLteConnectedAws(void)
{
	if (resolveAwsServer && areCertsSet()) {
		appSetNextState(appStateAwsResolveServer);
	} else if (areCertsSet()) {
		appSetNextState(appStateAwsConnect);
	} else {
		appSetNextState(appStateCommissionDevice);
	}
}

#ifdef CONFIG_LWM2M
static void appStateInitLwm2mClient(void)
{
	lwm2m_client_init();
	appSetNextState(appStateLwm2m);
}

static void appStateLwm2m(void)
{
	lwm2mMsgHandler();
}

static void lwm2mMsgHandler(void)
{
	int rc = 0;
	FwkMsg_t *pMsg;

	while (rc == 0) {
		/* Remove sensor/gateway data from queue and send it to cloud. */
		rc = -EINVAL;
		pMsg = NULL;
		Framework_Receive(awsMsgReceiver.pQueue, &pMsg, K_FOREVER);
		if (pMsg == NULL) {
			return;
		}

		switch (pMsg->header.msgCode) {
		case FMC_BL654_SENSOR_EVENT: {
			BL654SensorMsg_t *pBmeMsg = (BL654SensorMsg_t *)pMsg;
			rc = lwm2m_set_bl654_sensor_data(
				pBmeMsg->temperatureC, pBmeMsg->humidityPercent,
				pBmeMsg->pressurePa);
		} break;

		default:
			break;
		}
		BufferPool_Free(pMsg);

		if (rc != 0) {
			MAIN_LOG_ERR("Could not send data (%d)", rc);
		}
	}
}
#endif

static int setAwsCredentials(void)
{
	int rc;

	if (!aws_svc_client_cert_is_stored()) {
		return APP_ERR_READ_CERT;
	}

	if (!aws_svc_client_key_is_stored()) {
		return APP_ERR_READ_KEY;
	}
	devCertSet = true;
	devKeySet = true;
	rc = awsSetCredentials(aws_svc_get_client_cert(),
			       aws_svc_get_client_key());
	return rc;
}

static void appStateCommissionDevice(void)
{
	printk("\n\nWaiting to commission device\n\n");
	setAwsStatusWrapper(AWS_STATUS_NOT_PROVISIONED);
	allowCommissioning = true;

	k_sem_take(&rx_cert_sem, K_FOREVER);
	if (setAwsCredentials() == 0) {
		appSetNextState(appStateWaitForLte);
	}
}

static void decommission(void)
{
	nvStoreCommissioned(false);
	devCertSet = false;
	devKeySet = false;
	commissioned = false;
	allowCommissioning = true;
	initShadow = true;
	appSetNextState(appStateAwsDisconnect);
#if CONFIG_BLUEGRASS
	/* If the device is deleted from AWS it must be decommissioned
	 * in the BLE app before it is reprovisioned.
	 */
	FRAMEWORK_MSG_CREATE_AND_SEND(FWK_ID_RESERVED, FWK_ID_SENSOR_TASK,
				      FMC_AWS_DECOMMISSION);
#endif
	FRAMEWORK_MSG_CREATE_AND_SEND(FWK_ID_RESERVED, FWK_ID_AWS,
				      FMC_AWS_DECOMMISSION);
	printk("Device is decommissioned\n");
}

static void set_commissioned(void)
{
	nvStoreCommissioned(true);
	commissioned = true;
	allowCommissioning = false;
	setAwsStatusWrapper(AWS_STATUS_DISCONNECTED);
	k_sem_give(&rx_cert_sem);
	printk("Device is commissioned\n");
}

static void awsSvcEvent(enum aws_svc_event event)
{
	switch (event) {
	case AWS_SVC_EVENT_SETTINGS_SAVED:
		set_commissioned();
		break;
	case AWS_SVC_EVENT_SETTINGS_CLEARED:
		decommission();
		break;
	}
}

static void setAwsStatusWrapper(enum aws_status status)
{
	aws_svc_set_status(single_peripheral_get_conn(), status);
}

static void initializeAwsMsgReceiver(void)
{
	awsMsgReceiver.id = FWK_ID_AWS;
	awsMsgReceiver.pQueue = &awsQ;
	awsMsgReceiver.rxBlockTicks = K_NO_WAIT; /* unused */
	awsMsgReceiver.pMsgDispatcher = NULL; /* unused */
	Framework_RegisterReceiver(&awsMsgReceiver);
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

static void StartKeepAliveTimer(void)
{
	k_timer_start(&awsKeepAliveTimer,
		      K_SECONDS(CONFIG_AWS_KEEP_ALIVE_SECONDS), K_NO_WAIT);
}

static void AwsKeepAliveTimerCallbackIsr(struct k_timer *timer_id)
{
	UNUSED_PARAMETER(timer_id);
	FRAMEWORK_MSG_CREATE_AND_SEND(FWK_ID_AWS, FWK_ID_AWS,
				      FMC_AWS_KEEP_ALIVE);
}

/* Override weak implementation in laird_power.c */
void power_measurement_callback(uint8_t integer, uint8_t decimal)
{
	power_svc_set_voltage(integer, decimal);
}

/******************************************************************************/
/* Shell                                                                      */
/******************************************************************************/
#ifdef CONFIG_SHELL
static int shellSetCert(enum CREDENTIAL_TYPE type, uint8_t *cred)
{
	int rc;
	int certSize;
	int expSize = 0;
	char *newCred = NULL;

	if (!appReady) {
		printk("App is not ready\n");
		return APP_ERR_NOT_READY;
	}

	if (!allowCommissioning) {
		printk("Not ready for commissioning, decommission device first\n");
		return APP_ERR_COMMISSION_DISALLOWED;
	}

	certSize = strlen(cred);

	if (type == CREDENTIAL_CERT) {
		expSize = AWS_CLIENT_CERT_MAX_LENGTH;
		newCred = (char *)aws_svc_get_client_cert();
	} else if (type == CREDENTIAL_KEY) {
		expSize = AWS_CLIENT_KEY_MAX_LENGTH;
		newCred = (char *)aws_svc_get_client_key();
	} else {
		return APP_ERR_UNKNOWN_CRED;
	}

	if (certSize > expSize) {
		printk("Cert is too large (%d)\n", certSize);
		return APP_ERR_CRED_TOO_LARGE;
	}

	replace_word(cred, "\\n", "\n", newCred, expSize);
	replace_word(newCred, "\\s", " ", newCred, expSize);

	rc = aws_svc_save_clear_settings(true);
	if (rc < 0) {
		MAIN_LOG_ERR("Error storing credential (%d)", rc);
	} else if (type == CREDENTIAL_CERT) {
		printk("Stored cert:\n%s\n", newCred);
		devCertSet = true;

	} else if (type == CREDENTIAL_KEY) {
		printk("Stored key:\n%s\n", newCred);
		devKeySet = true;
	}

	if (rc >= 0 && areCertsSet()) {
		k_sem_give(&rx_cert_sem);
	}

	return rc;
}

static int shell_set_aws_device_cert(const struct shell *shell, size_t argc,
				     char **argv)
{
	ARG_UNUSED(argc);

	return shellSetCert(CREDENTIAL_CERT, argv[1]);
}

static int shell_set_aws_device_key(const struct shell *shell, size_t argc,
				    char **argv)
{
	ARG_UNUSED(argc);

	return shellSetCert(CREDENTIAL_KEY, argv[1]);
}

static int shell_decommission(const struct shell *shell, size_t argc,
			      char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!appReady) {
		printk("App is not ready\n");
		return APP_ERR_NOT_READY;
	}

	aws_svc_save_clear_settings(false);
	decommission();

	return 0;
}

#ifdef CONFIG_REBOOT
static int shell_reboot(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	power_reboot_module(REBOOT_TYPE_NORMAL);

	return 0;
}

static int shell_bootloader(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	power_reboot_module(REBOOT_TYPE_BOOTLOADER);

	return 0;
}
#endif /* CONFIG_REBOOT */
#endif /* CONFIG_SHELL */

#ifdef CONFIG_SHELL
SHELL_STATIC_SUBCMD_SET_CREATE(
	oob_cmds,
	SHELL_CMD_ARG(set_cert, NULL, "Set device cert",
		      shell_set_aws_device_cert, 2, 0),
	SHELL_CMD_ARG(set_key, NULL, "Set device key", shell_set_aws_device_key,
		      2, 0),
	SHELL_CMD(reset, NULL, "Factory reset (decommission) device",
		  shell_decommission),
#ifdef CONFIG_REBOOT
	SHELL_CMD(reboot, NULL, "Reboot module", shell_reboot),
	SHELL_CMD(bootloader, NULL, "Boot to UART bootloader",
		  shell_bootloader),
#endif
	SHELL_SUBCMD_SET_END /* Array terminated. */
);
SHELL_CMD_REGISTER(oob, &oob_cmds, "OOB Demo commands", NULL);

static int shell_send_at_cmd(const struct shell *shell, size_t argc,
			     char **argv)
{
	if ((argc == 2) && (argv[1] != NULL)) {
		int result = mdm_hl7800_send_at_cmd(argv[1]);
		if (result < 0) {
			shell_error(shell, "Command not accepted");
		}
	} else {
		shell_error(shell, "Invalid parameter");
		return -EINVAL;
	}
	return 0;
}

SHELL_CMD_REGISTER(at, NULL, "Send an AT command string to the HL7800",
		   shell_send_at_cmd);

#if CONFIG_PRINT_THREAD_LIST
static int print_thread_cmd(const struct shell *shell, size_t argc, char **argv)
{
	print_thread_list();
	return 0;
}

SHELL_CMD_REGISTER(print_threads, NULL, "Print list of threads",
		   print_thread_cmd);
#endif

#endif /* CONFIG_SHELL */
