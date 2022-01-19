/**
 * @file gnss.cpp
 * @author Bernd Giesecke (bernd.giesecke@rakwireless.com)
 * @brief GNSS functions and task
 * @version 0.2
 * @date 2021-12-28
 * 
 * @copyright Copyright (c) 2020
 * 
 */
#include "app.h"

// The GNSS object
TinyGPSPlus my_rak1910_gnss; // RAK1910_GNSS
SFE_UBLOX_GNSS my_gnss;		 // RAK12500_GNSS

/** LoRa task handle */
TaskHandle_t gnss_task_handle;
/** GPS reading task */
void gnss_task(void *pvParameters);

/** Semaphore for GNSS aquisition task */
SemaphoreHandle_t g_gnss_sem;

/** GNSS polling function */
bool poll_gnss(void);

/** Location data as byte array Cayenne LPP format */
tracker_data_short_s g_tracker_data_s;

/** Location data as byte array precise format */
tracker_data_prec_s g_tracker_data_l;

/** Latitude/Longitude value converter */
latLong_s pos_union;

/** Flag if location was found */
volatile bool last_read_ok = false;

/** Flag if GNSS is serial or I2C */
bool i2c_gnss = false;

/** The GPS module to use */
uint8_t gnss_option = 0;

/** Switch between GNSS on/off (1) and GNSS power save mode (0)*/
#define GNSS_OFF 1

/**
 * @brief Initialize the GNSS
 * 
 */
bool init_gnss(void)
{
	bool gnss_found = false;

	// Power on the GNSS module
	digitalWrite(WB_IO2, HIGH);

	// Give the module some time to power up
	delay(500);

	if (gnss_option == NO_GNSS_INIT)
	{
		if (!my_gnss.begin())
		{
			MYLOG("GNSS", "UBLOX did not answer on I2C, retry on Serial1");
			i2c_gnss = false;
		}
		else
		{
			MYLOG("GNSS", "UBLOX found on I2C");
			i2c_gnss = true;
			gnss_found = true;
			my_gnss.setI2COutput(COM_TYPE_UBX); //Set the I2C port to output UBX only (turn off NMEA noise)
			gnss_option = RAK12500_GNSS;
		}

		if (!i2c_gnss)
		{
			uint8_t retry = 0;
			//Assume that the U-Blox GNSS is running at 9600 baud (the default) or at 38400 baud.
			//Loop until we're in sync and then ensure it's at 38400 baud.
			do
			{
				MYLOG("GNSS", "GNSS: trying 38400 baud");
				Serial1.begin(38400);
				while (!Serial1)
					;
				if (my_gnss.begin(Serial1) == true)
				{
					MYLOG("GNSS", "UBLOX found on Serial1 with 38400");
					my_gnss.setUART1Output(COM_TYPE_UBX); //Set the UART port to output UBX only
					gnss_found = true;

					gnss_option = RAK12500_GNSS;
					break;
				}
				delay(100);
				MYLOG("GNSS", "GNSS: trying 9600 baud");
				Serial1.begin(9600);
				while (!Serial1)
					;
				if (my_gnss.begin(Serial1) == true)
				{
					MYLOG("GNSS", "GNSS: connected at 9600 baud, switching to 38400");
					my_gnss.setSerialRate(38400);
					delay(100);
				}
				else
				{
					my_gnss.factoryReset();
					delay(2000); //Wait a bit before trying again to limit the Serial output
				}
				retry++;
				if (retry == 3)
				{
					break;
				}
			} while (1);
		}

		if (gnss_found)
		{
			my_gnss.saveConfiguration(); //Save the current settings to flash and BBR

			my_gnss.setMeasurementRate(500);
			return true;
		}

		// No RAK12500 found, assume RAK1910 is plugged in
		gnss_option = RAK1910_GNSS;
		MYLOG("GNSS", "Initialize RAK1910");
		Serial1.end();
		delay(500);
		Serial1.begin(9600);
		while (!Serial1)
			;
		return true;
	}
	else
	{
		if (gnss_option == RAK12500_GNSS)
		{
			if (i2c_gnss)
			{
				my_gnss.begin();
				my_gnss.setI2COutput(COM_TYPE_UBX); //Set the I2C port to output UBX only (turn off NMEA noise)
			}
			else
			{
				Serial1.begin(38400);
				my_gnss.begin(Serial1);
				my_gnss.setUART1Output(COM_TYPE_UBX); //Set the UART port to output UBX only
			}
			my_gnss.setMeasurementRate(500);
		}
		else
		{
			Serial1.begin(9600);
			while (!Serial1)
				;
		}
		return true;
	}
}

/**
 * @brief Check GNSS module for position
 * 
 * @return true Valid position found
 * @return false No valid position
 */
bool poll_gnss(void)
{
	MYLOG("GNSS", "poll_gnss");

	last_read_ok = false;

#if GNSS_OFF == 1
	// Startup GNSS module
	init_gnss();
#endif

	time_t time_out = millis();
	int64_t latitude = 0;
	int64_t longitude = 0;
	int32_t altitude = 0;
	int32_t accuracy = 0;

	time_t check_limit = 90000;

	if (g_lorawan_settings.send_repeat_time == 0)
	{
		check_limit = 90000;
	}
	else if (g_lorawan_settings.send_repeat_time <= 90000)
	{
		check_limit = g_lorawan_settings.send_repeat_time / 2;
	}
	else
	{
		check_limit = 90000;
	}

	MYLOG("GNSS", "GNSS timeout %ld", (long int)check_limit);

	MYLOG("GNSS", "Using %s", gnss_option == RAK12500_GNSS ? "RAK12500" : "RAK1910");

	bool has_pos = false;
	bool has_alt = false;

	while ((millis() - time_out) < check_limit)
	{
		if (gnss_option == RAK12500_GNSS)
		{
			if (my_gnss.getGnssFixOk())
			{
				byte fix_type = my_gnss.getFixType(); // Get the fix type
				char fix_type_str[32] = {0};
				if (fix_type == 0)
					sprintf(fix_type_str, "No Fix");
				else if (fix_type == 1)
					sprintf(fix_type_str, "Dead reckoning");
				else if (fix_type == 2)
					sprintf(fix_type_str, "Fix type 2D");
				else if (fix_type == 3)
					sprintf(fix_type_str, "Fix type 3D");
				else if (fix_type == 4)
					sprintf(fix_type_str, "GNSS fix");
				else if (fix_type == 5)
					sprintf(fix_type_str, "Time fix");

				// if ((fix_type >= 3) && (my_gnss.getSIV() >= 5)) /** Fix type 3D and at least 5 satellites */
				if (fix_type >= 3) /** Fix type 3D */
				{
					last_read_ok = true;
					latitude = my_gnss.getLatitude();
					longitude = my_gnss.getLongitude();
					altitude = my_gnss.getAltitude();
					accuracy = my_gnss.getHorizontalDOP();

					MYLOG("GNSS", "Fixtype: %d %s", my_gnss.getFixType(), fix_type_str);
					MYLOG("GNSS", "Lat: %.4f Lon: %.4f", latitude / 10000000.0, longitude / 10000000.0);
					MYLOG("GNSS", "Alt: %.2f", altitude / 1000.0);
					MYLOG("GNSS", "Acy: %.2f ", accuracy / 100.0);

					// Break the while()
					break;
				}
			}
			else
			{
				delay(1000);
			}
		}
		else
		{
			while (Serial1.available() > 0)
			{
				// char gnss = Serial1.read();
				// Serial.print(gnss);
				// if (my_rak1910_gnss.encode(gnss))
				if (my_rak1910_gnss.encode(Serial1.read()))
				{
					if (my_rak1910_gnss.location.isUpdated() && my_rak1910_gnss.location.isValid())
					{
						MYLOG("GNSS", "Location valid");
						has_pos = true;
						latitude = (uint64_t)(my_rak1910_gnss.location.lat() * 10000000.0);
						longitude = (uint64_t)(my_rak1910_gnss.location.lng() * 10000000.0);
					}
					else if (my_rak1910_gnss.altitude.isUpdated() && my_rak1910_gnss.altitude.isValid())
					{
						MYLOG("GNSS", "Altitude valid");
						has_alt = true;
						altitude = (uint32_t)(my_rak1910_gnss.altitude.meters() * 1000);
					}
					else if (my_rak1910_gnss.hdop.isUpdated() && my_rak1910_gnss.hdop.isValid())
					{
						accuracy = my_rak1910_gnss.hdop.hdop() * 100;
					}
				}
				// if (has_pos && has_alt)
				if (has_pos && has_alt)
				{
					MYLOG("GNSS", "Lat: %.4f Lon: %.4f", latitude / 10000000.0, longitude / 10000000.0);
					MYLOG("GNSS", "Alt: %.2f", altitude / 1000.0);
					MYLOG("GNSS", "Acy: %.2f ", accuracy / 100.0);
					last_read_ok = true;
					break;
				}
			}
			if (has_pos && has_alt)
			{
				last_read_ok = true;
				break;
			}
		}
	}

#if GNSS_OFF == 1
	// Power down the module
	digitalWrite(WB_IO2, LOW);
	delay(100);
#endif

	if (last_read_ok)
	{
		if ((latitude == 0) && (longitude == 0))
		{
			last_read_ok = false;
			return false;
		}
		// Save default Cayenne LPP precision
		pos_union.val32 = latitude / 1000; // Cayenne LPP 0.0001 ° Signed MSB
		g_tracker_data_s.lat_1 = pos_union.val8[2];
		g_tracker_data_s.lat_2 = pos_union.val8[1];
		g_tracker_data_s.lat_3 = pos_union.val8[0];

		pos_union.val32 = longitude / 1000; // Cayenne LPP 0.0001 ° Signed MSB
		g_tracker_data_s.long_1 = pos_union.val8[2];
		g_tracker_data_s.long_2 = pos_union.val8[1];
		g_tracker_data_s.long_3 = pos_union.val8[0];

		pos_union.val32 = altitude / 10; // Cayenne LPP 0.01 meter Signed MSB
		g_tracker_data_s.alt_1 = pos_union.val8[2];
		g_tracker_data_s.alt_2 = pos_union.val8[1];
		g_tracker_data_s.alt_3 = pos_union.val8[0];

		// Save extended precision, not Cayenne LPP compatible
		pos_union.val32 = latitude / 10; // Custom 0.000001 ° Signed MSB
		g_tracker_data_l.lat_1 = pos_union.val8[3];
		g_tracker_data_l.lat_2 = pos_union.val8[2];
		g_tracker_data_l.lat_3 = pos_union.val8[1];
		g_tracker_data_l.lat_4 = pos_union.val8[0];

		pos_union.val32 = longitude / 10; // Custom 0.000001 ° Signed MSB
		g_tracker_data_l.long_1 = pos_union.val8[3];
		g_tracker_data_l.long_2 = pos_union.val8[2];
		g_tracker_data_l.long_3 = pos_union.val8[1];
		g_tracker_data_l.long_4 = pos_union.val8[0];

		pos_union.val32 = altitude / 10; // Cayenne LPP 0.01 meter Signed MSB
		g_tracker_data_l.alt_1 = pos_union.val8[2];
		g_tracker_data_l.alt_2 = pos_union.val8[1];
		g_tracker_data_l.alt_3 = pos_union.val8[0];
#if GNSS_OFF == 0
		my_gnss.setMeasurementRate(10000);
		my_gnss.setNavigationFrequency(1, 10000);
		my_gnss.powerSaveMode(true, 10000);
#endif
		return true;
	}
	else
	{
		// No location found, set the data to 0
		g_tracker_data_s.lat_1 = 0;
		g_tracker_data_s.lat_2 = 0;
		g_tracker_data_s.lat_3 = 0;

		g_tracker_data_s.long_1 = 0;
		g_tracker_data_s.long_2 = 0;
		g_tracker_data_s.long_3 = 0;

		g_tracker_data_s.alt_1 = 0;
		g_tracker_data_s.alt_2 = 0;
		g_tracker_data_s.alt_3 = 0;

		g_tracker_data_l.lat_1 = 0;
		g_tracker_data_l.lat_2 = 0;
		g_tracker_data_l.lat_3 = 0;
		g_tracker_data_l.lat_4 = 0;

		g_tracker_data_l.long_1 = 0;
		g_tracker_data_l.long_2 = 0;
		g_tracker_data_l.long_3 = 0;
		g_tracker_data_l.long_4 = 0;

		g_tracker_data_l.alt_1 = 0;
		g_tracker_data_l.alt_2 = 0;
		g_tracker_data_l.alt_3 = 0;

		/// \todo Enable below to get a fake GPS position if no location fix could be obtained
		// 	Serial.println("Faking GPS");
		// 	// 14.4213730, 121.0069140, 35.000
		// 	latitude = 144213730;
		// 	longitude = 1210069140;
		// 	altitude = 35000;

		// Save default Cayenne LPP precision
		// pos_union.val32 = latitude / 1000; // Cayenne LPP 0.0001 ° Signed MSB
		// g_tracker_data_s.lat_1 = pos_union.val8[2];
		// g_tracker_data_s.lat_2 = pos_union.val8[1];
		// g_tracker_data_s.lat_3 = pos_union.val8[0];

		// pos_union.val32 = longitude / 1000; // Cayenne LPP 0.0001 ° Signed MSB
		// g_tracker_data_s.long_1 = pos_union.val8[2];
		// g_tracker_data_s.long_2 = pos_union.val8[1];
		// g_tracker_data_s.long_3 = pos_union.val8[0];

		// pos_union.val32 = altitude / 10; // Cayenne LPP 0.01 meter Signed MSB
		// g_tracker_data_s.alt_1 = pos_union.val8[2];
		// g_tracker_data_s.alt_2 = pos_union.val8[1];
		// g_tracker_data_s.alt_3 = pos_union.val8[0];

		// // Save extended precision, not Cayenne LPP compatible
		// pos_union.val32 = latitude / 10; // Custom 0.000001 ° Signed MSB
		// g_tracker_data_l.lat_1 = pos_union.val8[3];
		// g_tracker_data_l.lat_2 = pos_union.val8[2];
		// g_tracker_data_l.lat_3 = pos_union.val8[1];
		// g_tracker_data_l.lat_4 = pos_union.val8[0];

		// pos_union.val32 = longitude / 10; // Custom 0.000001 ° Signed MSB
		// g_tracker_data_l.long_1 = pos_union.val8[3];
		// g_tracker_data_l.long_2 = pos_union.val8[2];
		// g_tracker_data_l.long_3 = pos_union.val8[1];
		// g_tracker_data_l.long_4 = pos_union.val8[0];

		// pos_union.val32 = altitude / 10; // Cayenne LPP 0.01 meter Signed MSB
		// g_tracker_data_l.alt_1 = pos_union.val8[2];
		// g_tracker_data_l.alt_2 = pos_union.val8[1];
		// g_tracker_data_l.alt_3 = pos_union.val8[0];
	}

	MYLOG("GNSS", "No valid location found");
	last_read_ok = false;

#if GNSS_OFF == 0
	if (gnss_option == RAK12500_GNSS)
	{
		my_gnss.setMeasurementRate(1000);
	}
#endif
	return false;
}

void gnss_task(void *pvParameters)
{
	MYLOG("GNSS", "GNSS Task started");

#if GNSS_OFF == 1
	// Power down the module
	digitalWrite(WB_IO2, LOW);
	delay(100);
#endif

	uint8_t busy_cnt = 0;
	while (1)
	{
		if (xSemaphoreTake(g_gnss_sem, portMAX_DELAY) == pdTRUE)
		{
			MYLOG("GNSS", "GNSS Task wake up");
			AT_PRINTF("+EVT:START_LOCATION\n");
			// Get location
			bool got_location = poll_gnss();
			AT_PRINTF("+EVT:LOCATION %s\n", got_location ? "FIX" : "NOFIX");

			// if ((g_task_sem != NULL) && got_location)
			if (g_task_sem != NULL)
			{
				g_task_event_type |= GNSS_FIN;
				xSemaphoreGiveFromISR(g_task_sem, &g_higher_priority_task_woken);
			}
			MYLOG("GNSS", "GNSS Task finished");
		}
	}
}
