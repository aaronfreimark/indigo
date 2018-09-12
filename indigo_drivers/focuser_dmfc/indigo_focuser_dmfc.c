// Copyright (c) 2018 CloudMakers, s. r. o.
// All rights reserved.
//
// You can use this software under the terms of 'INDIGO Astronomy
// open-source license' (see LICENSE.md).
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS 'AS IS' AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// version history
// 2.0 by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO StarlighXpress filter wheel driver
 \file indigo_ccd_sx.c
 */

#define DRIVER_VERSION 0x0001
#define DRIVER_NAME "indigo_focuser_dmfc"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>

#include <sys/time.h>

#include "indigo_driver_xml.h"
#include "indigo_io.h"
#include "indigo_focuser_dmfc.h"

#define PRIVATE_DATA													((dmfc_private_data *)device->private_data)

#define X_FOCUSER_MOTOR_TYPE_PROPERTY					(PRIVATE_DATA->motor_type_property)
#define X_FOCUSER_MOTOR_TYPE_STEPPER_ITEM			(X_FOCUSER_MOTOR_TYPE_PROPERTY->items+0)
#define X_FOCUSER_MOTOR_TYPE_DC_ITEM					(X_FOCUSER_MOTOR_TYPE_PROPERTY->items+1)

#define X_FOCUSER_ENCODER_PROPERTY						(PRIVATE_DATA->encoder_property)
#define X_FOCUSER_ENCODER_ENABLED_ITEM				(X_FOCUSER_ENCODER_PROPERTY->items+0)
#define X_FOCUSER_ENCODER_DISABLED_ITEM				(X_FOCUSER_ENCODER_PROPERTY->items+1)

#define X_FOCUSER_BACKLASH_PROPERTY						(PRIVATE_DATA->backlash_property)
#define X_FOCUSER_BACKLASH_ITEM								(X_FOCUSER_BACKLASH_PROPERTY->items+0)

typedef struct {
	int handle;
	pthread_mutex_t port_mutex;
	indigo_timer *timer;
	indigo_property *motor_type_property;
	indigo_property *encoder_property;
	indigo_property *backlash_property;
} dmfc_private_data;

static bool dmfc_command(indigo_device *device, char *command, char *response, int max) {
	pthread_mutex_lock(&PRIVATE_DATA->port_mutex);
	indigo_write(PRIVATE_DATA->handle, command, sizeof(command));
	indigo_write(PRIVATE_DATA->handle, "\n", 1);
	if (response != NULL) {
		if (indigo_read_line(PRIVATE_DATA->handle, response, max) == 0) {
			pthread_mutex_unlock(&PRIVATE_DATA->port_mutex);
			return false;
		}
	}
	pthread_mutex_unlock(&PRIVATE_DATA->port_mutex);
	INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Command %s -> %s", command, response != NULL ? response : "NULL");
	return true;
}

// -------------------------------------------------------------------------------- INDIGO focuser device implementation

static void timer_callback(indigo_device *device) {
	char response[16];
	if (dmfc_command(device, "T", response, sizeof(response))) {
		double temp = atof(response);
		if (FOCUSER_TEMPERATURE_ITEM->number.value != temp) {
			FOCUSER_TEMPERATURE_ITEM->number.value = temp;
			FOCUSER_TEMPERATURE_PROPERTY->state = INDIGO_OK_STATE;
			indigo_update_property(device, FOCUSER_TEMPERATURE_PROPERTY, NULL);
		}
	}
	bool update = false;
	if (dmfc_command(device, "P", response, sizeof(response))) {
		int pos = atoi(response);
		if (FOCUSER_POSITION_ITEM->number.value != pos) {
			FOCUSER_POSITION_ITEM->number.value = pos;
			update = true;
		}
	}
	if (dmfc_command(device, "I", response, sizeof(response))) {
		if (*response == 0) {
			if (FOCUSER_POSITION_PROPERTY->state != INDIGO_OK_STATE) {
				FOCUSER_STEPS_PROPERTY->state = INDIGO_OK_STATE;
				FOCUSER_POSITION_PROPERTY->state = INDIGO_OK_STATE;
				update = true;
			}
		} else {
			if (FOCUSER_POSITION_PROPERTY->state != INDIGO_BUSY_STATE) {
				FOCUSER_POSITION_PROPERTY->state = INDIGO_BUSY_STATE;
				FOCUSER_STEPS_PROPERTY->state = INDIGO_BUSY_STATE;
				update = true;
			}
		}
	}
	if (update) {
		indigo_update_property(device, FOCUSER_POSITION_PROPERTY, NULL);
		indigo_update_property(device, FOCUSER_STEPS_PROPERTY, NULL);
	}
	indigo_reschedule_timer(device, 0.5, &PRIVATE_DATA->timer);
}

static indigo_result focuser_attach(indigo_device *device) {
	assert(device != NULL);
	assert(PRIVATE_DATA != NULL);
	if (indigo_focuser_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		X_FOCUSER_MOTOR_TYPE_PROPERTY = indigo_init_switch_property(NULL, device->name, "X_FOCUSER_MOTOR_TYPE", FOCUSER_MAIN_GROUP, "Motor type", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 2);
		if (X_FOCUSER_MOTOR_TYPE_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(X_FOCUSER_MOTOR_TYPE_STEPPER_ITEM, "STEPPER", "Stepper motor", false);
		indigo_init_switch_item(X_FOCUSER_MOTOR_TYPE_DC_ITEM, "DC", "DC Motor", false);
		X_FOCUSER_ENCODER_PROPERTY = indigo_init_switch_property(NULL, device->name, "X_FOCUSER_ENCODER", FOCUSER_MAIN_GROUP, "Encoder state", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 2);
		if (X_FOCUSER_ENCODER_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(X_FOCUSER_ENCODER_ENABLED_ITEM, "ENABLED", "Enabled", false);
		indigo_init_switch_item(X_FOCUSER_ENCODER_DISABLED_ITEM, "DISABLED", "Enabled", false);
		X_FOCUSER_BACKLASH_PROPERTY = indigo_init_number_property(NULL, device->name, "X_FOCUSER_BACKLASH", FOCUSER_MAIN_GROUP, "Backlash", INDIGO_OK_STATE, INDIGO_RW_PERM, 1);
		if (X_FOCUSER_BACKLASH_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_number_item(X_FOCUSER_BACKLASH_ITEM, "BACKLASH", "Backlash", 0, 9999, 100, 0);
		// -------------------------------------------------------------------------------- DEVICE_PORT, DEVICE_PORTS
		DEVICE_PORT_PROPERTY->hidden = false;
		DEVICE_PORTS_PROPERTY->hidden = false;
#ifdef INDIGO_MACOS
		for (int i = 0; i < DEVICE_PORTS_PROPERTY->count; i++) {
			if (!strncmp(DEVICE_PORTS_PROPERTY->items[i].name, "/dev/cu.usbmodem", 16)) {
				strncpy(DEVICE_PORT_ITEM->text.value, DEVICE_PORTS_PROPERTY->items[i].name, INDIGO_VALUE_SIZE);
				break;
			}
		}
#endif
#ifdef INDIGO_LINUX
		strcpy(DEVICE_PORT_ITEM->text.value, "/dev/usb_focuser");
#endif
		// -------------------------------------------------------------------------------- INFO
		INFO_PROPERTY->count = 5;
		strcpy(INFO_DEVICE_MODEL_ITEM->text.value, "DMFCv3");
		// -------------------------------------------------------------------------------- FOCUSER_ROTATION
		FOCUSER_ROTATION_PROPERTY->hidden = false;
		// -------------------------------------------------------------------------------- FOCUSER_TEMPERATURE
		FOCUSER_TEMPERATURE_PROPERTY->hidden = false;
		// -------------------------------------------------------------------------------- FOCUSER_SPEED
		FOCUSER_SPEED_ITEM->number.min = 100;
		FOCUSER_SPEED_ITEM->number.max = 1000;
		FOCUSER_SPEED_ITEM->number.step = 100;
		// -------------------------------------------------------------------------------- FOCUSER_STEPS
		FOCUSER_STEPS_ITEM->number.min = 0;
		FOCUSER_SPEED_ITEM->number.max = 100000;
		FOCUSER_SPEED_ITEM->number.step = 1;
		// -------------------------------------------------------------------------------- FOCUSER_POSITION
		FOCUSER_POSITION_ITEM->number.min = 0;
		FOCUSER_POSITION_ITEM->number.max = 100000;
		FOCUSER_POSITION_ITEM->number.step = 1;
		// --------------------------------------------------------------------------------
		pthread_mutex_init(&PRIVATE_DATA->port_mutex, NULL);
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return indigo_focuser_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result focuser_enumerate_properties(indigo_device *device, indigo_client *client, indigo_property *property) {
	if (IS_CONNECTED) {
		if (indigo_property_match(X_FOCUSER_MOTOR_TYPE_PROPERTY, property))
			indigo_define_property(device, X_FOCUSER_MOTOR_TYPE_PROPERTY, NULL);
		if (indigo_property_match(X_FOCUSER_ENCODER_PROPERTY, property))
			indigo_define_property(device, X_FOCUSER_ENCODER_PROPERTY, NULL);
		if (indigo_property_match(X_FOCUSER_BACKLASH_PROPERTY, property))
			indigo_define_property(device, X_FOCUSER_BACKLASH_PROPERTY, NULL);
	}
	return indigo_focuser_enumerate_properties(device, NULL, NULL);
}

static indigo_result focuser_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);
	char command[16], response[64];
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONNECTION
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			CONNECTION_PROPERTY->state = INDIGO_BUSY_STATE;
			indigo_update_property(device, CONNECTION_PROPERTY, NULL);
			PRIVATE_DATA->handle = indigo_open_serial_with_speed(DEVICE_PORT_ITEM->text.value, 19200);
			if (PRIVATE_DATA->handle > 0) {
				if (dmfc_command(device, "#", response, sizeof(response)) && !strcmp(response, "OK_DMFCN")) {
					INDIGO_DRIVER_LOG(DRIVER_NAME, "DMFCv3 OK");
				} else {
					INDIGO_DRIVER_ERROR(DRIVER_NAME, "DMFCv3 not detected");
					close(PRIVATE_DATA->handle);
					PRIVATE_DATA->handle = 0;
				}
//			} else {
//				CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
//				INDIGO_DRIVER_ERROR(DRIVER_NAME, "DMFCv3 failed to open %s %s (%d)", DEVICE_PORT_ITEM->text.value, strerror(errno), errno);
			}
			if (PRIVATE_DATA->handle > 0) {
				if (dmfc_command(device, "A", response, sizeof(response)) && !strncmp(response, "OK_", 3)) {
					char *token = strtok(response, ":");
					token = strtok(NULL, ":"); // status
					if (token) { // version
						strcpy(INFO_DEVICE_FW_REVISION_ITEM->text.value, token);
					}
					token = strtok(NULL, ":");
					if (token) { // motor
						indigo_set_switch(X_FOCUSER_MOTOR_TYPE_PROPERTY, *token == '1' ? X_FOCUSER_MOTOR_TYPE_STEPPER_ITEM : X_FOCUSER_MOTOR_TYPE_DC_ITEM, true);
					}
					token = strtok(NULL, ":");
					if (token) { // temperature
						FOCUSER_TEMPERATURE_ITEM->number.value = FOCUSER_TEMPERATURE_ITEM->number.target = atof(token);
					}
					token = strtok(NULL, ":");
					if (token) { // position
						FOCUSER_POSITION_ITEM->number.value = FOCUSER_POSITION_ITEM->number.target = atoi(token);
					}
					token = strtok(NULL, ":");
					if (token) { // moving status
						FOCUSER_POSITION_PROPERTY->state = FOCUSER_STEPS_PROPERTY->state = *token == '1' ? INDIGO_BUSY_STATE : INDIGO_OK_STATE;
					}
					token = strtok(NULL, ":"); // led status
					token = strtok(NULL, ":");
					if (token) { // reverse
						indigo_set_switch(FOCUSER_ROTATION_PROPERTY, *token == '1' ? FOCUSER_ROTATION_COUNTERCLOCKWISE_ITEM : FOCUSER_ROTATION_CLOCKWISE_ITEM, true);
					}
					token = strtok(NULL, ":");
					if (token) { // encoder
						indigo_set_switch(X_FOCUSER_ENCODER_PROPERTY, *token == '1' ? X_FOCUSER_ENCODER_DISABLED_ITEM: X_FOCUSER_ENCODER_ENABLED_ITEM, true);
					}
					token = strtok(NULL, ":");
					if (token) { // backlash
						X_FOCUSER_BACKLASH_ITEM->number.value = X_FOCUSER_BACKLASH_ITEM->number.target = atoi(token);
					} else {
						INDIGO_DRIVER_ERROR(DRIVER_NAME, "Failed to parse 'A' response");
						close(PRIVATE_DATA->handle);
						PRIVATE_DATA->handle = 0;
					}
				} else {
					INDIGO_DRIVER_ERROR(DRIVER_NAME, "Failed to read 'A' response");
					close(PRIVATE_DATA->handle);
					PRIVATE_DATA->handle = 0;
				}
			}
			if (PRIVATE_DATA->handle > 0) {
				indigo_define_property(device, X_FOCUSER_MOTOR_TYPE_PROPERTY, NULL);
				indigo_define_property(device, X_FOCUSER_ENCODER_PROPERTY, NULL);
				indigo_define_property(device, X_FOCUSER_BACKLASH_PROPERTY, NULL);
				dmfc_command(device, "L:2", response, sizeof(response));
				INDIGO_DRIVER_LOG(DRIVER_NAME, "Connected to %s", DEVICE_PORT_ITEM->text.value);
				PRIVATE_DATA->timer = indigo_set_timer(device, 0, timer_callback);
				CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
			} else {
				INDIGO_DRIVER_ERROR(DRIVER_NAME, "Failed to connect to %s", DEVICE_PORT_ITEM->text.value);
				CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
				indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
			}
		} else {
			if (PRIVATE_DATA->handle > 0) {
				indigo_cancel_timer(device, &PRIVATE_DATA->timer);
				dmfc_command(device, "L:1", response, sizeof(response));
				indigo_delete_property(device, X_FOCUSER_MOTOR_TYPE_PROPERTY, NULL);
				indigo_delete_property(device, X_FOCUSER_ENCODER_PROPERTY, NULL);
				indigo_delete_property(device, X_FOCUSER_BACKLASH_PROPERTY, NULL);
				INDIGO_DRIVER_LOG(DRIVER_NAME, "Disconnected");
				close(PRIVATE_DATA->handle);
				PRIVATE_DATA->handle = 0;
			}
			CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
		}
	} else if (indigo_property_match(FOCUSER_SPEED_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- FOCUSER_SPEED
		if (IS_CONNECTED) {
			indigo_property_copy_values(FOCUSER_SPEED_PROPERTY, property, false);
			snprintf(command, sizeof(command), "S:%d", (int)FOCUSER_SPEED_ITEM->number.value);
			if (dmfc_command(device, command, NULL, 0)) {
				FOCUSER_SPEED_PROPERTY->state = INDIGO_OK_STATE;
			} else {
				FOCUSER_SPEED_PROPERTY->state = INDIGO_ALERT_STATE;
			}
			indigo_update_property(device, FOCUSER_SPEED_PROPERTY, NULL);
		}
		return INDIGO_OK;
	} else if (indigo_property_match(FOCUSER_STEPS_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- FOCUSER_STEPS
		indigo_property_copy_values(FOCUSER_STEPS_PROPERTY, property, false);
		snprintf(command, sizeof(command), "G:%d", (int)FOCUSER_STEPS_ITEM->number.value * (FOCUSER_DIRECTION_MOVE_INWARD_ITEM->sw.value ? 1 : -1));
		if (dmfc_command(device, command, NULL, 0)) {
			FOCUSER_POSITION_PROPERTY->state = INDIGO_BUSY_STATE;
			FOCUSER_SPEED_PROPERTY->state = INDIGO_BUSY_STATE;
		} else {
			FOCUSER_POSITION_PROPERTY->state = INDIGO_ALERT_STATE;
			FOCUSER_SPEED_PROPERTY->state = INDIGO_ALERT_STATE;
		}
		indigo_update_property(device, FOCUSER_POSITION_PROPERTY, NULL);
		indigo_update_property(device, FOCUSER_SPEED_PROPERTY, NULL);
		return INDIGO_OK;
	} else if (indigo_property_match(FOCUSER_POSITION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- FOCUSER_POSITION
		indigo_property_copy_values(FOCUSER_POSITION_PROPERTY, property, false);
		snprintf(command, sizeof(command), "M:%d", (int)FOCUSER_POSITION_ITEM->number.value);
		if (dmfc_command(device, command, NULL, 0)) {
			FOCUSER_POSITION_PROPERTY->state = INDIGO_BUSY_STATE;
			FOCUSER_SPEED_PROPERTY->state = INDIGO_BUSY_STATE;
		} else {
			FOCUSER_POSITION_PROPERTY->state = INDIGO_ALERT_STATE;
			FOCUSER_SPEED_PROPERTY->state = INDIGO_ALERT_STATE;
		}
		indigo_update_property(device, FOCUSER_POSITION_PROPERTY, NULL);
		indigo_update_property(device, FOCUSER_SPEED_PROPERTY, NULL);
		return INDIGO_OK;
	} else if (indigo_property_match(FOCUSER_ABORT_MOTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- FOCUSER_ABORT_MOTION
		indigo_property_copy_values(FOCUSER_ABORT_MOTION_PROPERTY, property, false);
		if (FOCUSER_ABORT_MOTION_ITEM->sw.value) {
			FOCUSER_ABORT_MOTION_ITEM->sw.value = false;
			if (dmfc_command(device, "H", NULL, 0)) {
				FOCUSER_ABORT_MOTION_PROPERTY->state = INDIGO_OK_STATE;
				FOCUSER_POSITION_PROPERTY->state = INDIGO_ALERT_STATE;
				FOCUSER_SPEED_PROPERTY->state = INDIGO_ALERT_STATE;
			} else {
				FOCUSER_ABORT_MOTION_PROPERTY->state = INDIGO_ALERT_STATE;
			}
		}
		indigo_update_property(device, FOCUSER_ABORT_MOTION_PROPERTY, NULL);
		return INDIGO_OK;
		// -------------------------------------------------------------------------------- FOCUSER_ROTATION
	} else if (indigo_property_match(FOCUSER_ROTATION_PROPERTY, property)) {
		if (IS_CONNECTED) {
			indigo_property_copy_values(FOCUSER_ROTATION_PROPERTY, property, false);
			snprintf(command, sizeof(command), "N:%d", (int)FOCUSER_ROTATION_CLOCKWISE_ITEM->sw.value? 0 : 1);
			if (dmfc_command(device, command, response, sizeof(response))) {
				FOCUSER_ROTATION_PROPERTY->state = INDIGO_OK_STATE;
			} else {
				FOCUSER_ROTATION_PROPERTY->state = INDIGO_ALERT_STATE;
			}
			indigo_update_property(device, FOCUSER_ROTATION_PROPERTY, NULL);
		}
		return INDIGO_OK;
		// -------------------------------------------------------------------------------- X_FOCUSER_MOTOR_TYPE
	} else if (indigo_property_match(X_FOCUSER_MOTOR_TYPE_PROPERTY, property)) {
		indigo_property_copy_values(X_FOCUSER_MOTOR_TYPE_PROPERTY, property, false);
		snprintf(command, sizeof(command), "R:%d", (int)X_FOCUSER_MOTOR_TYPE_STEPPER_ITEM->sw.value? 1 : 0);
		if (dmfc_command(device, command, response, sizeof(response))) {
			X_FOCUSER_MOTOR_TYPE_PROPERTY->state = INDIGO_OK_STATE;
		} else {
			X_FOCUSER_MOTOR_TYPE_PROPERTY->state = INDIGO_ALERT_STATE;
		}
		indigo_update_property(device, X_FOCUSER_MOTOR_TYPE_PROPERTY, NULL);
		return INDIGO_OK;
		// -------------------------------------------------------------------------------- X_FOCUSER_ENCODER_PROPERTY
	} else if (indigo_property_match(X_FOCUSER_ENCODER_PROPERTY, property)) {
		indigo_property_copy_values(X_FOCUSER_ENCODER_PROPERTY, property, false);
		snprintf(command, sizeof(command), "E:%d", (int)X_FOCUSER_ENCODER_DISABLED_ITEM->sw.value? 1 : 0);
		if (dmfc_command(device, command, response, sizeof(response))) {
			X_FOCUSER_ENCODER_PROPERTY->state = INDIGO_OK_STATE;
		} else {
			X_FOCUSER_ENCODER_PROPERTY->state = INDIGO_ALERT_STATE;
		}
		indigo_update_property(device, X_FOCUSER_ENCODER_PROPERTY, NULL);
		return INDIGO_OK;
		// -------------------------------------------------------------------------------- X_FOCUSER_BACKLASH
	} else if (indigo_property_match(X_FOCUSER_BACKLASH_PROPERTY, property)) {
		indigo_property_copy_values(X_FOCUSER_BACKLASH_PROPERTY, property, false);
		snprintf(command, sizeof(command), "C:%d", (int)X_FOCUSER_BACKLASH_ITEM->number.value);
		if (dmfc_command(device, command, NULL, 0)) {
			X_FOCUSER_ENCODER_PROPERTY->state = INDIGO_OK_STATE;
		} else {
			X_FOCUSER_ENCODER_PROPERTY->state = INDIGO_ALERT_STATE;
		}
		indigo_update_property(device, X_FOCUSER_ENCODER_PROPERTY, NULL);
		return INDIGO_OK;
	}
	return indigo_focuser_change_property(device, client, property);
}

static indigo_result focuser_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value)
		indigo_device_disconnect(NULL, device->name);
	indigo_release_property(X_FOCUSER_MOTOR_TYPE_PROPERTY);
	indigo_release_property(X_FOCUSER_ENCODER_PROPERTY);
	indigo_release_property(X_FOCUSER_BACKLASH_PROPERTY);
	INDIGO_DEVICE_DETACH_LOG(DRIVER_NAME, device->name);
	return indigo_focuser_detach(device);
}

indigo_result indigo_focuser_dmfc(indigo_driver_action action, indigo_driver_info *info) {
	static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;
	static dmfc_private_data *private_data = NULL;
	static indigo_device *focuser = NULL;

	static indigo_device focuser_template = INDIGO_DEVICE_INITIALIZER(
		"Pegasus DMFC",
		focuser_attach,
		focuser_enumerate_properties,
		focuser_change_property,
		NULL,
		focuser_detach
	);

	SET_DRIVER_INFO(info, "PegasusAstro DMFC Focuser", __FUNCTION__, DRIVER_VERSION, last_action);

	if (action == last_action)
		return INDIGO_OK;

	switch (action) {
		case INDIGO_DRIVER_INIT:
			last_action = action;
			private_data = malloc(sizeof(dmfc_private_data));
			assert(private_data != NULL);
			memset(private_data, 0, sizeof(dmfc_private_data));
			focuser = malloc(sizeof(indigo_device));
			assert(focuser != NULL);
			memcpy(focuser, &focuser_template, sizeof(indigo_device));
			focuser->private_data = private_data;
			indigo_attach_device(focuser);
			break;

		case INDIGO_DRIVER_SHUTDOWN:
			last_action = action;
			if (focuser != NULL) {
				indigo_detach_device(focuser);
				free(focuser);
				focuser = NULL;
			}
			if (private_data != NULL) {
				free(private_data);
				private_data = NULL;
			}
			break;

		case INDIGO_DRIVER_INFO:
			break;
	}

	return INDIGO_OK;
}