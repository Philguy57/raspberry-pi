#ifndef WINCE
#include "iothubtransportamqp.h"
#else
#include "iothubtransporthttp.h"
#endif
#include "schemalib.h"
#include "iothub_client.h"
#include "serializer.h"
#include "schemaserializer.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

#include <linux/input.h>
#include <linux/joystick.h>

#include <wiringPi.h>

#include "locking.h"

#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/platform.h"

// Combination of previous controller input reading with new IOT hub implementation
// IOT hub code is highly inspired by some practice code for a temperature sensor, 
// https://github.com/Azure-Samples/iot-hub-c-raspberrypi-getstartedkit/blob/master/samples/remote_monitoring/remote_monitoring.c

/* mapping of LEDs on the board to the GPIO pins on the raspberry pi. */
#define LED_1		4		
#define LED_2		17		
#define LED_3		22		
#define LED_4		10		
#define LED_5		9		
#define LED_6		11		
#define BUZZER		8		

/* indexes into the various buttons. */
#define A_BUTTON	0
#define B_BUTTON	1
#define X_BUTTON	2
#define Y_BUTTON	3
#define R_BUMPER	4
#define L_BUMPER	5
#define START		6
#define BACK		7

// Specific to the IOT hub, likely to change
static const char* deviceId = "raspberryPi3";
static const char* deviceKey = "7O369bU/PvaUjFXnC3wGeJWhQ2y/yGPAmUzfTKedKX8=";
static const char* hubName = "PhilPiHub";
static const char* hubSuffix = "azure-devices.net";

static const int Spi_channel = 0;
static const int Spi_clock = 1000000L;

static const int Red_led_pin = 4;
static const int Grn_led_pin = 5;

static int fd;

static int Lock_fd;

// Define the Model
BEGIN_NAMESPACE(Contoso);

DECLARE_STRUCT(SystemProperties,
ascii_char_ptr, DeviceID,
_Bool, Enabled
);

DECLARE_STRUCT(DeviceProperties,
ascii_char_ptr, DeviceID,
_Bool, HubEnabledState
);

DECLARE_MODEL(Controller,

/* Event data (buttons, right analog, left analog) */
WITH_DATA(char, Buttons),
WITH_DATA(float, RightAnalog),
WITH_DATA(float, LeftAnalog),
WITH_DATA(ascii_char_ptr, DeviceId),

/* Device Info - This is command metadata + some extra fields */
WITH_DATA(ascii_char_ptr, ObjectType),
WITH_DATA(_Bool, IsSimulatedDevice),
WITH_DATA(ascii_char_ptr, Version),
WITH_DATA(DeviceProperties, DeviceProperties),
WITH_DATA(ascii_char_ptr_no_quotes, Commands),

/* Commands implemented by the device */
WITH_ACTION(SetButtons, int, buttons),
WITH_ACTION(SetRightAnalog, int, rightAnalog)
WITH_ACTION(SetLeftAnalog, int, leftAnalog)
);

END_NAMESPACE(Contoso);

EXECUTE_COMMAND_RESULT SetButtons(Controller* controller, int buttons)
{
	controller->Buttons = buttons;
	return EXECUTE_COMMAND_SUCCESS;
}

EXECUTE_COMMAND_RESULT SetRightAnalog(Controller* controller, int rightAnalog)
{
	controller->RightAnalog = rightAnalog;
	return EXECUTE_COMMAND_SUCCESS;
}

EXECUTE_COMMAND_RESULT SetLeftAnalog(Controller* controller, int leftAnalog)
{
	controller->LeftAnalog = leftAnalog;
	return EXECUTE_COMMAND_SUCCESS;
}

static void sendMessage(IOTHUB_CLIENT_HANDLE iotHubClientHandle, const unsigned char* buffer, size_t size)
{
	IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromByteArray(buffer, size);
	if (messageHandle == NULL)
	{
		(void)printf("unable to create a new IoTHubMessage\r\n");
	}
	else
	{
		if (IoTHubClient_SendEventAsync(iotHubClientHandle, messageHandle, NULL, NULL) != IOTHUB_CLIENT_OK)
		{
			(void)printf("failed to hand over the message to IoTHubClient\r\n");
		}
		else
		{
			(void)printf("IoTHubClient accepted the message for delivery\r\n");
		}

		IoTHubMessage_Destroy(messageHandle);
	}
	free((void*)buffer);
}

/*this function "links" IoTHub to the serialization library*/
static IOTHUBMESSAGE_DISPOSITION_RESULT IoTHubMessage(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback)
{
	IOTHUBMESSAGE_DISPOSITION_RESULT result;
	const unsigned char* buffer;
	size_t size;
	if (IoTHubMessage_GetByteArray(message, &buffer, &size) != IOTHUB_MESSAGE_OK)
	{
		printf("unable to IoTHubMessage_GetByteArray\r\n");
		result = EXECUTE_COMMAND_ERROR;
	}
	else
	{
		/*buffer is not zero terminated*/
		char* temp = malloc(size + 1);
		if (temp == NULL)
		{
			printf("failed to malloc\r\n");
			result = EXECUTE_COMMAND_ERROR;
		}
		else
		{
			EXECUTE_COMMAND_RESULT executeCommandResult;

			memcpy(temp, buffer, size);
			temp[size] = '\0';
			executeCommandResult = EXECUTE_COMMAND(userContextCallback, temp);
			result =
				(executeCommandResult == EXECUTE_COMMAND_ERROR) ? IOTHUBMESSAGE_ABANDONED :
				(executeCommandResult == EXECUTE_COMMAND_SUCCESS) ? IOTHUBMESSAGE_ACCEPTED :
				IOTHUBMESSAGE_REJECTED;
			free(temp);
		}
	}
	return result;
}

static int remote_monitoring_init(void)
{
	int result;

	Lock_fd = open_lockfile(LOCKFILE);

	if (setuid(getuid()) < 0)
	{
		perror("Dropping privileges failed. (did you use sudo?)n");
		result = EXIT_FAILURE;
	}
	else
	{
		result = wiringPiSetup();
		if (result != 0)
		{
			perror("Wiring Pi setup failed.");
		}
		else
		{
			fd = open("/dev/input/js0", O_RDONLY);
			result = fd;
			if (result < 0)
			{
				printf("Could not open joystick.");
			}
		}
	}
	return result;
}

static void remote_monitoring_run(void)
{
	if (platform_init() != 0)
	{
		printf("Failed to initialize the platform.\r\n");
	}
	else
	{
		if (serializer_init(NULL) != SERIALIZER_OK)
		{
			printf("Failed on serializer_init\r\n");
		}
		else
		{
			IOTHUB_CLIENT_CONFIG config;
			IOTHUB_CLIENT_HANDLE iotHubClientHandle;

			config.deviceSasToken = NULL;
			config.deviceId = deviceId;
			config.deviceKey = deviceKey;
			config.iotHubName = hubName;
			config.iotHubSuffix = hubSuffix;
#ifndef WINCE
			config.protocol = AMQP_Protocol;
#else
			config.protocol = HTTP_Protocol;
#endif
			iotHubClientHandle = IoTHubClient_Create(&config);
			if (iotHubClientHandle == NULL)
			{
				(void)printf("Failed on IoTHubClient_CreateFromConnectionString\r\n");
			}
			else
			{
#ifdef MBED_BUILD_TIMESTAMP
				// For mbed add the certificate information
				if (IoTHubClient_SetOption(iotHubClientHandle, "TrustedCerts", certificates) != IOTHUB_CLIENT_OK)
				{
					printf("failure to set option \"TrustedCerts\"\r\n");
				}
#endif // MBED_BUILD_TIMESTAMP

				Controller* controller = CREATE_MODEL_INSTANCE(Contoso, Controller);
				if (controller == NULL)
				{
					(void)printf("Failed on CREATE_MODEL_INSTANCE\r\n");
				}
				else
				{
					STRING_HANDLE commandsMetadata;

					if (IoTHubClient_SetMessageCallback(iotHubClientHandle, IoTHubMessage, controller) != IOTHUB_CLIENT_OK)
					{
						printf("unable to IoTHubClient_SetMessageCallback\r\n");
					}
					else
					{

						/* send the device info upon startup so that the cloud app knows
						what commands are available and the fact that the device is up */
						controller->ObjectType = "DeviceInfo";
						controller->IsSimulatedDevice = false;
						controller->Version = "1.0";
						controller->DeviceProperties.HubEnabledState = true;
						controller->DeviceProperties.DeviceID = (char*)deviceId;

						commandsMetadata = STRING_new();
						if (commandsMetadata == NULL)
						{
							(void)printf("Failed on creating string for commands metadata\r\n");
						}
						else
						{
							/* Serialize the commands metadata as a JSON string before sending */
							if (SchemaSerializer_SerializeCommandMetadata(GET_MODEL_HANDLE(Contoso, Controller), commandsMetadata) != SCHEMA_SERIALIZER_OK)
							{
								(void)printf("Failed serializing commands metadata\r\n");
							}
							else
							{
								unsigned char* buffer;
								size_t bufferSize;
								controller->Commands = (char*)STRING_c_str(commandsMetadata);

								/* Here is the actual send of the Device Info */
								if (SERIALIZE(&buffer, &bufferSize, controller->ObjectType, controller->Version, controller->IsSimulatedDevice, controller->DeviceProperties, controller->Commands) != IOT_AGENT_OK)
								{
									(void)printf("Failed serializing\r\n");
								}
								else
								{
									sendMessage(iotHubClientHandle, buffer, bufferSize);
								}

							}

							STRING_delete(commandsMetadata);
						}
						// INIT to zero for all
						controller->Buttons = 0;
						controller->RightAnalog = 0;
						controller->LeftAnalog = 0;
						controller->DeviceId = (char*)deviceId;

						struct js_event js;

						while (1)
						{
							unsigned char*buffer;
							size_t bufferSize;
							float rightAnalog = 0;
							float leftAnalog = 0;
							char buttons = 0;


							if (read(fd, &js, sizeof(struct js_event)) != sizeof(struct js_event)) {
								printf("Failed on reading joystick input");
								return -1;
							}

							/* check whether what changed was an axis or a button, and store the value accordingly */
							switch (js.type & ~JS_EVENT_INIT) {
							case JS_EVENT_AXIS:
								if (js.number == 0) {
									rightAnalog = js.value;
								}
								if (js.number == 1) {
									leftAnalog = js.value;
								}
								break;
							case JS_EVENT_BUTTON:
								if (js.number == A_BUTTON) {
									buttons ^= 0x01;
								}
								if (js.number == B_BUTTON) {
									buttons ^= 0x02;
								}
								if (js.number == X_BUTTON) {
									buttons ^= 0x04;
								}
								if (js.number == Y_BUTTON) {
									buttons ^= 0x08;
								}
								if (js.number == R_BUMPER) {
									buttons ^= 0x10;
								}
								if (js.number == L_BUMPER) {
									buttons ^= 0x20;
								}
								if (js.number == START) {
									buttons ^= 0x40;
								}
								if (js.number == BACK) {
									buttons ^= 0x80;
								}
								break;
							}

							controller->Buttons = buttons;
							controller->RightAnalog = rightAnalog;
							controller->LeftAnalog = leftAnalog;

							// not confident that this totally works...

							if (SERIALIZE(&buffer, &bufferSize, controller->DeviceId, controller->Buttons, controller->RightAnalog, controller->LeftAnalog) != IOT_AGENT_OK)
							{
								(void)printf("Failed sending sensor value\r\n");
							}
							else
							{
								sendMessage(iotHubClientHandle, buffer, bufferSize);
							}


							sendMessage(iotHubClientHandle, buffer, bufferSize);

							ThreadAPI_Sleep(1000);
						}
					}
					close_lockfile(Lock_fd);
					DESTROY_MODEL_INSTANCE(controller);
				}
				IoTHubClient_Destroy(iotHubClientHandle);
			}
			serializer_deinit();
		}
		platform_deinit();
	}
}

int main(void)
{
	int result = remote_monitoring_init();
	if (result == 0)
	{
		remote_monitoring_run();
	}
	return result;
}
