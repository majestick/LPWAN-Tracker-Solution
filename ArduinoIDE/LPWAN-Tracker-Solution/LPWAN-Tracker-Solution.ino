/**
 * @file app.cpp
 * @author Bernd Giesecke (bernd.giesecke@rakwireless.com)
 * @brief Application specific functions. Mandatory to have init_app(), 
 *        app_event_handler(), ble_data_handler(), lora_data_handler()
 *        and lora_tx_finished()
 * @version 0.2
 * @date 2021-12-18
 * 
 * @copyright Copyright (c) 2021
 * 
 */

#include "app.h"

/** Set the device name, max length is 10 characters */
char g_ble_dev_name[10] = "RAK-GNSS";

/** Timer since last position message was sent */
time_t last_pos_send = 0;
/** Timer for delayed sending to keep duty cycle */
SoftwareTimer delayed_sending;
/** Required for give semaphore from ISR */
BaseType_t g_higher_priority_task_woken = pdTRUE;

/** Battery level uinion */
batt_s batt_level;

/** Flag if delayed sending is already activated */
bool delayed_active = false;

/** Minimum delay between sending new locations, set to 45 seconds */
time_t min_delay = 45000;

// Forward declaration
void send_delayed(TimerHandle_t unused);
void at_settings(void);

/** Send Fail counter **/
uint8_t send_fail = 0;

/** Flag for low battery protection */
bool low_batt_protection = false;

/** Initialization result */
bool init_result = true;

/** GPS precision */
bool g_gps_prec_6 = true;

/** Packet buffer */
uint8_t packet_buff[TRACKER_DATA_PREC_LEN + ENVIRONMENT_DATA_LEN];

/** Packet size */
uint8_t packet_len;

/** Flag if environment sensor was found */
bool has_env_sensor = false;

/**
 * @brief Application specific setup functions
 * 
 */
void setup_app(void)
{
	// Enable BLE
	g_enable_ble = true;
}

/**
 * @brief Application specific initializations
 * 
 * @return true Initialization success
 * @return false Initialization failure
 */
bool init_app(void)
{
	bool gnss_ok = true;
	bool acc_ok = true;

	MYLOG("APP", "init_app");

	api_set_version(SW_VERSION_1, SW_VERSION_2, SW_VERSION_3);

	// Initialize Serial for debug output
	Serial.begin(115200);

	time_t serial_timeout = millis();
	// On nRF52840 the USB serial is not available immediately
	while (!Serial)
	{
		if ((millis() - serial_timeout) < 5000)
		{
			delay(100);
			digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
		}
		else
		{
			break;
		}
	}

	AT_PRINTF("============================\n");
	AT_PRINTF("LPWAN Tracker Solution\n");
	AT_PRINTF("Built with RAK's WisBlock\n");
	AT_PRINTF("SW Version %d.%d.%d\n", g_sw_ver_1, g_sw_ver_2, g_sw_ver_3);
	AT_PRINTF("LoRa(R) is a registered trademark or service mark of Semtech Corporation or its affiliates.\nLoRaWAN(R) is a licensed mark.\n");
	AT_PRINTF("============================\n");
	at_settings();

	pinMode(WB_IO2, OUTPUT);
	digitalWrite(WB_IO2, HIGH);

	// Get precision settings
	read_gps_settings();

	// Start the I2C bus
	Wire.begin();
	Wire.setClock(400000);

	// Initialize GNSS module
	gnss_ok = init_gnss();

	// If P2P mode GNSS task needs to be started here
	if (!g_lorawan_settings.lorawan_enable)
	{
		// Prepare GNSS task
		// Create the GNSS event semaphore
		g_gnss_sem = xSemaphoreCreateBinary();
		// Initialize semaphore
		xSemaphoreGive(g_gnss_sem);
		// Take semaphore
		xSemaphoreTake(g_gnss_sem, 10);
		if (!xTaskCreate(gnss_task, "LORA", 4096, NULL, TASK_PRIO_LOW, &gnss_task_handle))
		{
			MYLOG("APP", "Failed to start GNSS task");
		}
		last_pos_send = millis();
		g_lpwan_has_joined = true;
	}

	// Initialize ACC sensor
	acc_ok = init_acc();

	// Initialize Environment sensor
	has_env_sensor = init_bme();

	if (g_lorawan_settings.send_repeat_time != 0)
	{
		// Set delay for sending to 1/2 of scheduled sending
		min_delay = g_lorawan_settings.send_repeat_time / 2;
	}
	else
	{
		// Send repeat time is 0, set delay to 30 seconds
		min_delay = 30000;
	}

	// Set delayed sending to 1/2 of programmed send interval or 30 seconds
	delayed_sending.begin(min_delay, send_delayed, NULL, false);

	AT_PRINTF("GNSS Precision:\n");
	if (g_gps_prec_6)
	{
		AT_PRINTF("   6 decimal digit precision\n");
	}
	else
	{
		AT_PRINTF("   4 decimal digit precision\n");
	}
	AT_PRINTF("============================\n");

	if (gnss_ok)
	{
		AT_PRINTF("+EVT:GNSS OK\n");
	}
	else
	{
		AT_PRINTF("+EVT:GNSS FAIL\n");
		init_result = false;
	}
	if (acc_ok)
	{
		AT_PRINTF("+EVT:ACC OK\n");
	}
	else
	{
		AT_PRINTF("+EVT:ACC FAIL\n");
		init_result = false;
	}
	if (has_env_sensor)
	{
		AT_PRINTF("+EVT:ENV OK\n");
	}
	else
	{
		AT_PRINTF("+EVT:ENV FAIL\n");
	}

	return init_result;
}

/**
 * @brief Application specific event handler
 *        Requires as minimum the handling of STATUS event
 *        Here you handle as well your application specific events
 */
void app_event_handler(void)
{
	// Timer triggered event
	if ((g_task_event_type & STATUS) == STATUS)
	{
		g_task_event_type &= N_STATUS;
		MYLOG("APP", "Timer wakeup");

		// Initialization failed, report error over AT interface */
		if (!init_result)
		{
			AT_PRINTF("+EVT:HW_FAILURE\n");
		}

		// If BLE is enabled, restart Advertising
		if (g_enable_ble)
		{
			restart_advertising(15);
		}

		if (!low_batt_protection)
		{
			if (init_result)
			{
				if (has_env_sensor)
				{
					// Wake up the temperature sensor and start measurements
					start_bme();
				}
			}
			if (gnss_option != NO_GNSS_INIT)
			{
				// Start the GNSS location tracking
				xSemaphoreGive(g_gnss_sem);
			}
		}

		// Get battery level
		batt_level.batt16 = read_batt() / 10;
		g_env_data.batt_1 = batt_level.batt8[1];
		g_env_data.batt_2 = batt_level.batt8[0];

		// Protection against battery drain
		if (batt_level.batt16 < 290)
		{
			// Battery is very low, change send time to 1 hour to protect battery
			low_batt_protection = true;						   // Set low_batt_protection active
			g_task_wakeup_timer.setPeriod(1 * 60 * 60 * 1000); // Set send time to one hour
			g_task_wakeup_timer.reset();
			MYLOG("APP", "Battery protection activated");
		}
		else if ((batt_level.batt16 > 410) && low_batt_protection)
		{
			// Battery is higher than 4V, change send time back to original setting
			low_batt_protection = false;
			g_task_wakeup_timer.setPeriod(g_lorawan_settings.send_repeat_time);
			g_task_wakeup_timer.reset();
			MYLOG("APP", "Battery protection deactivated");
		}

		if (low_batt_protection || (gnss_option == NO_GNSS_INIT))
		{
			if (g_lorawan_settings.lorawan_enable)
			{
				// Send only the battery level over LoRaWAN
				lmh_error_status result = send_lora_packet((uint8_t *)&g_env_data, 4);
				switch (result)
				{
				case LMH_SUCCESS:
					MYLOG("APP", "Packet enqueued");
					break;
				case LMH_BUSY:
					AT_PRINTF("+EVT:BUSY\n");
					MYLOG("APP", "LoRa transceiver is busy");
					break;
				case LMH_ERROR:
					AT_PRINTF("+EVT:SIZE_ERROR\n");
					MYLOG("APP", "Packet error, too big to send with current DR");
					break;
				}
			}
			else
			{
				// Send only the battery level over LoRa
				if (send_p2p_packet((uint8_t *)&g_env_data, 4))
				{
					MYLOG("APP", "Packet enqueued");
				}
				else
				{
					MYLOG("APP", "Packet enqueued");
				}
			}
		}
	}

	// ACC trigger event
	if ((g_task_event_type & ACC_TRIGGER) == ACC_TRIGGER && g_lpwan_has_joined)
	{
		g_task_event_type &= N_ACC_TRIGGER;
		MYLOG("APP", "ACC triggered");
		clear_acc_int();

		// Check time since last send
		bool send_now = true;
		if (g_lorawan_settings.send_repeat_time != 0)
		{
			if ((millis() - last_pos_send) < min_delay)
			{
				send_now = false;
				if (!delayed_active)
				{
					delayed_sending.stop();
					MYLOG("APP", "Expired time %d", (int)(millis() - last_pos_send));
					MYLOG("APP", "Max delay time %d", (int)min_delay);
					time_t wait_time = abs(min_delay - (millis() - last_pos_send) >= 0) ? (min_delay - (millis() - last_pos_send)) : min_delay;
					MYLOG("APP", "Wait time %ld", (long)wait_time);

					MYLOG("APP", "Only %lds since last position message, send delayed in %lds", (long)((millis() - last_pos_send) / 1000), (long)(wait_time / 1000));
					delayed_sending.setPeriod(wait_time);
					delayed_sending.start();
					delayed_active = true;
				}
			}
		}
		if (send_now)
		{
			// Remember last send time
			last_pos_send = millis();

			// Trigger a GNSS reading and packet sending
			g_task_event_type |= STATUS;
		}

		// Reset the standard timer
		if (g_lorawan_settings.send_repeat_time != 0)
		{
			g_task_wakeup_timer.reset();
		}
	}

	// GNSS location search finished
	if ((g_task_event_type & GNSS_FIN) == GNSS_FIN)
	{
		g_task_event_type &= N_GNSS_FIN;

		// Get Environment data
		read_bme();

		// Remember last time sending
		last_pos_send = millis();
		// Just in case
		delayed_active = false;

		memset(packet_buff, 0, (TRACKER_DATA_PREC_LEN + ENVIRONMENT_DATA_LEN));
		if (last_read_ok)
		{
			if (g_gps_prec_6)
			{
				memcpy((void *)&packet_buff[0], (void *)&g_tracker_data_l, TRACKER_DATA_PREC_LEN);
				if (has_env_sensor)
				{
					memcpy((void *)&packet_buff[TRACKER_DATA_PREC_LEN], (void *)&g_env_data, ENVIRONMENT_DATA_LEN);
					packet_len = TRACKER_DATA_PREC_LEN + ENVIRONMENT_DATA_LEN;
				}
				else
				{
					memcpy((void *)&packet_buff[TRACKER_DATA_PREC_LEN], (void *)&g_env_data, 4);
					packet_len = TRACKER_DATA_PREC_LEN + 4;
				}
			}
			else
			{
				memcpy((void *)&packet_buff[0], (void *)&g_tracker_data_s, TRACKER_DATA_SHORT_LEN);
				if (has_env_sensor)
				{
					memcpy((void *)&packet_buff[TRACKER_DATA_SHORT_LEN], (void *)&g_env_data, ENVIRONMENT_DATA_LEN);
					packet_len = TRACKER_DATA_SHORT_LEN + ENVIRONMENT_DATA_LEN;
				}
				else
				{
					memcpy((void *)&packet_buff[TRACKER_DATA_SHORT_LEN], (void *)&g_env_data, 4);
					packet_len = TRACKER_DATA_SHORT_LEN + 4;
				}
			}
		}
		else
		{
			if (has_env_sensor)
			{
				memcpy((void *)&packet_buff[0], (void *)&g_env_data, ENVIRONMENT_DATA_LEN);
				packet_len = ENVIRONMENT_DATA_LEN;
			}
			else
			{
				memcpy((void *)&packet_buff[0], (void *)&g_env_data, 4);
				packet_len = 4;
			}
		}

#if MY_DEBUG == 1
		for (int idx = 0; idx < packet_len; idx++)
		{
			Serial.printf("%02X", packet_buff[idx]);
		}
		Serial.println("");
		Serial.printf("Packetsize %d\n", packet_len);
#endif

		if (g_lorawan_settings.lorawan_enable)
		{
			// Check payload size
			if (g_lorawan_settings.lora_region == 8)
			{
				if (g_lorawan_settings.data_rate == 0)
				{
					AT_PRINTF("+EVT:DR_ERROR\n");
					return;
				}
			}

			// Send packet over LoRaWAN
			lmh_error_status result;
			result = send_lora_packet(packet_buff, packet_len);
			switch (result)
			{
			case LMH_SUCCESS:
				MYLOG("APP", "Packet enqueued");
				break;
			case LMH_BUSY:
				AT_PRINTF("+EVT:BUSY\n");
				MYLOG("APP", "LoRa transceiver is busy");
				break;
			case LMH_ERROR:
				result = send_lora_packet(packet_buff, packet_len);
				switch (result)
				{
				case LMH_SUCCESS:
					MYLOG("APP", "Packet enqueued");
					break;
				case LMH_BUSY:
					AT_PRINTF("+EVT:BUSY\n");
					MYLOG("APP", "LoRa transceiver is busy");
					break;
				case LMH_ERROR:
					AT_PRINTF("+EVT:SIZE_ERROR RETRY\n");
					result = send_lora_packet(packet_buff, packet_len);
					AT_PRINTF("+EVT:SIZE_ERROR\n");
					MYLOG("APP", "Packet error, too big to send with current DR");
				}
				break;
			}
		}
		else
		{
			// Send packet over LoRa
			if (send_p2p_packet(packet_buff, packet_len))
			{
				MYLOG("APP", "Packet enqueued");
			}
			else
			{
				AT_PRINTF("+EVT:SIZE_ERROR\n");
				MYLOG("APP", "Packet too big");
			}
		}
	}
}

/**
 * @brief Handle BLE UART data
 * 
 */
void ble_data_handler(void)
{
	if (g_enable_ble)
	{
		// BLE UART data handling
		if ((g_task_event_type & BLE_DATA) == BLE_DATA)
		{
			MYLOG("AT", "RECEIVED BLE");
			/** BLE UART data arrived */
			g_task_event_type &= N_BLE_DATA;

			while (g_ble_uart.available() > 0)
			{
				at_serial_input(uint8_t(g_ble_uart.read()));
				delay(5);
			}
			at_serial_input(uint8_t('\n'));
		}
	}
}

/**
 * @brief Handle received LoRa Data
 * 
 */
void lora_data_handler(void)
{
	// LoRa Join finished handling
	if ((g_task_event_type & LORA_JOIN_FIN) == LORA_JOIN_FIN)
	{
		g_task_event_type &= N_LORA_JOIN_FIN;
		if (g_join_result)
		{
			MYLOG("APP", "Successfully joined network");
			AT_PRINTF("+EVT:JOINED\n");

			// Prepare GNSS task
			// Create the GNSS event semaphore
			g_gnss_sem = xSemaphoreCreateBinary();
			// Initialize semaphore
			xSemaphoreGive(g_gnss_sem);
			// Take semaphore
			xSemaphoreTake(g_gnss_sem, 10);
			if (!xTaskCreate(gnss_task, "LORA", 4096, NULL, TASK_PRIO_LOW, &gnss_task_handle))
			{
				MYLOG("APP", "Failed to start GNSS task");
			}
			last_pos_send = millis();
		}
		else
		{
			MYLOG("APP", "Join network failed");
			AT_PRINTF("+EVT:JOIN FAILED\n");
			/// \todo here join could be restarted.
			lmh_join();
		}
	}

	// LoRa TX finished handling
	if ((g_task_event_type & LORA_TX_FIN) == LORA_TX_FIN)
	{
		g_task_event_type &= N_LORA_TX_FIN;

		MYLOG("APP", "LPWAN TX cycle %s", g_rx_fin_result ? "finished ACK" : "failed NAK");

		if ((g_lorawan_settings.confirmed_msg_enabled) && (g_lorawan_settings.lorawan_enable))
		{
			AT_PRINTF("+EVT:SEND CONFIRMED %s\n", g_rx_fin_result ? "SUCCESS" : "FAIL");
		}
		else
		{
			AT_PRINTF("+EVT:SEND OK\n");
		}

		if (!g_rx_fin_result)
		{
			// Increase fail send counter
			send_fail++;

			if (send_fail == 10)
			{
				// Too many failed sendings, reset node and try to rejoin
				delay(100);
				sd_nvic_SystemReset();
			}
		}
	}

	// LoRa data handling
	if ((g_task_event_type & LORA_DATA) == LORA_DATA)
	{
		/**************************************************************/
		/**************************************************************/
		/// \todo LoRa data arrived
		/// \todo parse them here
		/**************************************************************/
		/**************************************************************/
		g_task_event_type &= N_LORA_DATA;
		MYLOG("APP", "Received package over LoRa");

		if (g_lorawan_settings.lorawan_enable)
		{
			AT_PRINTF("+EVT:RX_1, RSSI %d, SNR %d\n", g_last_rssi, g_last_snr);
			AT_PRINTF("+EVT:%d:", g_last_fport);
			for (int idx = 0; idx < g_rx_data_len; idx++)
			{
				AT_PRINTF("%02X", g_rx_lora_data[idx]);
			}
			AT_PRINTF("\n");
		}
		else
		{
			AT_PRINTF("+EVT:RXP2P, RSSI %d, SNR %d\n", g_last_rssi, g_last_snr);
			AT_PRINTF("+EVT:");
			for (int idx = 0; idx < g_rx_data_len; idx++)
			{
				AT_PRINTF("%02X", g_rx_lora_data[idx]);
			}
			AT_PRINTF("\n");
		}

		char log_buff[g_rx_data_len * 3] = {0};
		uint8_t log_idx = 0;
		for (int idx = 0; idx < g_rx_data_len; idx++)
		{
			sprintf(&log_buff[log_idx], "%02X ", g_rx_lora_data[idx]);
			log_idx += 3;
		}

		MYLOG("APP", "%s", log_buff);
	}
}

/**
 * @brief Timer function used to avoid sending packages too often.
 * 			Delays the next package by 10 seconds
 * 
 * @param unused 
 * 			Timer handle, not used
 */
void send_delayed(TimerHandle_t unused)
{
	g_task_event_type |= STATUS;
	xSemaphoreGiveFromISR(g_task_sem, &g_higher_priority_task_woken);
}
