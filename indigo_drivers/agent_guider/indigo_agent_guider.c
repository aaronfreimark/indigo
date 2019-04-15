// Copyright (c) 2019 CloudMakers, s. r. o.
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

/** INDIGO Guider agent
 \file indigo_agent_guider.c
 */

#define DRIVER_VERSION 0x0001
#define DRIVER_NAME	"indigo_agent_guider"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <pthread.h>

#include "indigo_driver_xml.h"
#include "indigo_filter.h"
#include "indigo_ccd_driver.h"
#include "indigo_guider_utils.h"
#include "indigo_agent_guider.h"

#define DEVICE_PRIVATE_DATA										((agent_private_data *)device->private_data)
#define CLIENT_PRIVATE_DATA										((agent_private_data *)FILTER_CLIENT_CONTEXT->device->private_data)

#define AGENT_GUIDER_MODE_PROPERTY						(DEVICE_PRIVATE_DATA->agent_guider_mode_property)
#define AGENT_GUIDER_BATCH_CENTROID_ITEM    	(AGENT_GUIDER_MODE_PROPERTY->items+0)
#define AGENT_GUIDER_BATCH_DONUTS_ITEM  			(AGENT_GUIDER_MODE_PROPERTY->items+1)

#define AGENT_START_PROCESS_PROPERTY					(DEVICE_PRIVATE_DATA->agent_start_process_property)
#define AGENT_GUIDER_START_PREVIEW_ITEM  			(AGENT_START_PROCESS_PROPERTY->items+0)
#define AGENT_GUIDER_START_CALIBRATION_ITEM 	(AGENT_START_PROCESS_PROPERTY->items+1)
#define AGENT_GUIDER_START_GUIDING_ITEM 			(AGENT_START_PROCESS_PROPERTY->items+2)

#define AGENT_ABORT_PROCESS_PROPERTY					(DEVICE_PRIVATE_DATA->agent_abort_process_property)
#define AGENT_ABORT_PROCESS_ITEM      				(AGENT_ABORT_PROCESS_PROPERTY->items+0)

#define AGENT_GUIDER_STATS_PROPERTY						(DEVICE_PRIVATE_DATA->agent_stats_property)
#define AGENT_GUIDER_STATS_FRAME_ITEM      		(AGENT_GUIDER_STATS_PROPERTY->items+0)
#define AGENT_GUIDER_STATS_DRIFT_X_ITEM      	(AGENT_GUIDER_STATS_PROPERTY->items+1)
#define AGENT_GUIDER_STATS_DRIFT_Y_ITEM      	(AGENT_GUIDER_STATS_PROPERTY->items+2)
#define AGENT_GUIDER_STATS_DRIFT_RA_ITEM      (AGENT_GUIDER_STATS_PROPERTY->items+3)
#define AGENT_GUIDER_STATS_DRIFT_DEC_ITEM     (AGENT_GUIDER_STATS_PROPERTY->items+4)
#define AGENT_GUIDER_STATS_CORR_RA_ITEM      	(AGENT_GUIDER_STATS_PROPERTY->items+5)
#define AGENT_GUIDER_STATS_CORR_DEC_ITEM      (AGENT_GUIDER_STATS_PROPERTY->items+6)
#define AGENT_GUIDER_STATS_RMSE_RA_ITEM      	(AGENT_GUIDER_STATS_PROPERTY->items+7)
#define AGENT_GUIDER_STATS_RMSE_DEC_ITEM      (AGENT_GUIDER_STATS_PROPERTY->items+8)

typedef struct {
	indigo_property *agent_guider_mode_property;
	indigo_property *agent_start_process_property;
	indigo_property *agent_abort_process_property;
	indigo_property *agent_stats_property;
	long frame;
	indigo_frame_digest reference;
} agent_private_data;

// -------------------------------------------------------------------------------- INDIGO agent common code

static void preview_process(indigo_device *device) {
	indigo_property **cache = FILTER_DEVICE_CONTEXT->device_property_cache;
	indigo_property *remote_exposure_property = NULL;
	for (int j = 0; j < INDIGO_FILTER_MAX_CACHED_PROPERTIES; j++) {
		if (cache[j] && !strcmp(cache[j]->device, FILTER_DEVICE_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX]) && !strcmp(cache[j]->name, CCD_EXPOSURE_PROPERTY_NAME)) {
			remote_exposure_property = cache[j];
			break;
		}
	}
	if (remote_exposure_property == NULL) {
		AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
		indigo_send_message(device, "%s: CCD_EXPOSURE_PROPERTY not found", GUIDER_AGENT_NAME);
	} else {
		indigo_property *local_exposure_property = indigo_init_number_property(NULL, remote_exposure_property->device, remote_exposure_property->name, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, remote_exposure_property->count);
		if (local_exposure_property) {
			memcpy(local_exposure_property, remote_exposure_property, sizeof(indigo_property) + remote_exposure_property->count * sizeof(indigo_item));
			AGENT_GUIDER_MODE_PROPERTY->state = INDIGO_BUSY_STATE;
			indigo_update_property(device, AGENT_GUIDER_MODE_PROPERTY, NULL);
			while (AGENT_START_PROCESS_PROPERTY->state == INDIGO_BUSY_STATE) {
				double time = 1; // TBD
				local_exposure_property->items[0].number.value = time;
				indigo_change_property(FILTER_DEVICE_CONTEXT->client, local_exposure_property);
				for (int i = 0; remote_exposure_property->state != INDIGO_BUSY_STATE && i < 1000; i++)
					usleep(1000);
				if (remote_exposure_property->state != INDIGO_BUSY_STATE) {
					indigo_send_message(device, "%s: CCD_EXPOSURE_PROPERTY didn't become busy in 1s", GUIDER_AGENT_NAME);
					break;
				}
				while (remote_exposure_property->state == INDIGO_BUSY_STATE) {
					if (time > 1) {
						usleep(1000000);
						time -= 1;
					} else {
						usleep(10000);
						time -= 0.01;
					}
				}
			}
			AGENT_START_PROCESS_PROPERTY->state = INDIGO_OK_STATE;
			indigo_release_property(local_exposure_property);
		}
	}
	indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
}

// -------------------------------------------------------------------------------- INDIGO agent device implementation

static indigo_result agent_enumerate_properties(indigo_device *device, indigo_client *client, indigo_property *property);

static indigo_result agent_device_attach(indigo_device *device) {
	assert(device != NULL);
	assert(DEVICE_PRIVATE_DATA != NULL);
	if (indigo_filter_device_attach(device, DRIVER_VERSION, INDIGO_INTERFACE_CCD) == INDIGO_OK) {
		// -------------------------------------------------------------------------------- Device properties
		FILTER_CCD_LIST_PROPERTY->hidden = false;
		FILTER_GUIDER_LIST_PROPERTY->hidden = false;
		// -------------------------------------------------------------------------------- Process properties
		AGENT_GUIDER_MODE_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_GUIDER_MODE_PROPERTY_NAME, "Process", "Detection mode", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 2);
		if (AGENT_GUIDER_MODE_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(AGENT_GUIDER_BATCH_CENTROID_ITEM, AGENT_GUIDER_BATCH_CENTROID_ITEM_NAME, "Centroid mode", false);
		indigo_init_switch_item(AGENT_GUIDER_BATCH_DONUTS_ITEM, AGENT_GUIDER_BATCH_DONUTS_ITEM_NAME, "Donuts mode", true);
		AGENT_START_PROCESS_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_START_PROCESS_PROPERTY_NAME, "Process", "Start", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ANY_OF_MANY_RULE, 3);
		if (AGENT_START_PROCESS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(AGENT_GUIDER_START_PREVIEW_ITEM, AGENT_GUIDER_START_PREVIEW_ITEM_NAME, "Start preview", false);
		indigo_init_switch_item(AGENT_GUIDER_START_CALIBRATION_ITEM, AGENT_GUIDER_START_CALIBRATION_ITEM_NAME, "Start calibration", false);
		indigo_init_switch_item(AGENT_GUIDER_START_GUIDING_ITEM, AGENT_GUIDER_START_GUIDING_ITEM_NAME, "Start guiding", false);
		AGENT_ABORT_PROCESS_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_ABORT_PROCESS_PROPERTY_NAME, "Process", "Abort", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ANY_OF_MANY_RULE, 1);
		if (AGENT_ABORT_PROCESS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(AGENT_ABORT_PROCESS_ITEM, AGENT_ABORT_PROCESS_ITEM_NAME, "Abort", false);
		// -------------------------------------------------------------------------------- Guiding properties
		AGENT_GUIDER_STATS_PROPERTY = indigo_init_number_property(NULL, device->name, AGENT_GUIDER_STATS_PROPERTY_NAME, "Process", "Stats", INDIGO_OK_STATE, INDIGO_RO_PERM, 9);
		if (AGENT_GUIDER_STATS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_number_item(AGENT_GUIDER_STATS_FRAME_ITEM, AGENT_GUIDER_STATS_FRAME_ITEM_NAME, "Frame #", 0, 0xFFFFFFFF, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_DRIFT_X_ITEM, AGENT_GUIDER_STATS_DRIFT_X_ITEM_NAME, "Drift X", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_DRIFT_Y_ITEM, AGENT_GUIDER_STATS_DRIFT_Y_ITEM_NAME, "Drift Y", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_DRIFT_RA_ITEM, AGENT_GUIDER_STATS_DRIFT_RA_ITEM_NAME, "Drift RA", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_DRIFT_DEC_ITEM, AGENT_GUIDER_STATS_DRIFT_DEC_ITEM_NAME, "Drift Dec", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_CORR_RA_ITEM, AGENT_GUIDER_STATS_CORR_RA_ITEM_NAME, "Correction RA", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_CORR_DEC_ITEM, AGENT_GUIDER_STATS_CORR_DEC_ITEM_NAME, "Correction Dec", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_RMSE_RA_ITEM, AGENT_GUIDER_STATS_RMSE_RA_ITEM_NAME, "RMSE RA", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_RMSE_DEC_ITEM, AGENT_GUIDER_STATS_RMSE_DEC_ITEM_NAME, "RMSE Dec", -1000, 1000, 0, 0);
		// --------------------------------------------------------------------------------
		CONNECTION_PROPERTY->hidden = true;
		PROFILE_PROPERTY->hidden = true;
		CONFIG_PROPERTY->hidden = true;
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return agent_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result agent_enumerate_properties(indigo_device *device, indigo_client *client, indigo_property *property) {
	if (client != NULL && client == FILTER_DEVICE_CONTEXT->client)
		return INDIGO_OK;
	if (!FILTER_CCD_LIST_PROPERTY->items->sw.value && !FILTER_GUIDER_LIST_PROPERTY->items->sw.value) {
		if (indigo_property_match(AGENT_GUIDER_MODE_PROPERTY, property))
			indigo_define_property(device, AGENT_GUIDER_MODE_PROPERTY, NULL);
		if (indigo_property_match(AGENT_START_PROCESS_PROPERTY, property))
			indigo_define_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
		if (indigo_property_match(AGENT_ABORT_PROCESS_PROPERTY, property))
			indigo_define_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
		if (indigo_property_match(AGENT_GUIDER_STATS_PROPERTY, property))
			indigo_define_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
	}
	return indigo_filter_enumerate_properties(device, client, property);
}

static void abort_process(indigo_device *device) {
	if (AGENT_START_PROCESS_PROPERTY->state == INDIGO_BUSY_STATE) {
		indigo_property *abort_property = indigo_init_switch_property(NULL, FILTER_DEVICE_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX], CCD_ABORT_EXPOSURE_PROPERTY_NAME, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 1);
		if (abort_property) {
			indigo_init_switch_item(abort_property->items, CCD_ABORT_EXPOSURE_ITEM_NAME, "", true);
			indigo_change_property(FILTER_DEVICE_CONTEXT->client, abort_property);
			indigo_release_property(abort_property);
		}
		AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
		indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
	}
}

static indigo_result agent_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);
	if (client == FILTER_DEVICE_CONTEXT->client)
		return INDIGO_OK;
	if (indigo_property_match(AGENT_GUIDER_MODE_PROPERTY, property)) {
		indigo_property_copy_values(AGENT_GUIDER_MODE_PROPERTY, property, false);
		AGENT_GUIDER_MODE_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, AGENT_GUIDER_MODE_PROPERTY, NULL);
	} else if (indigo_property_match(AGENT_START_PROCESS_PROPERTY, property)) {
		if (*FILTER_DEVICE_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX]) {
			indigo_property_copy_values(AGENT_START_PROCESS_PROPERTY, property, false);
			if (AGENT_START_PROCESS_PROPERTY->state != INDIGO_BUSY_STATE) {
				DEVICE_PRIVATE_DATA->frame = 0;
				indigo_delete_frame_digest(&DEVICE_PRIVATE_DATA->reference);
				if (AGENT_GUIDER_START_PREVIEW_ITEM->sw.value) {
					AGENT_GUIDER_START_PREVIEW_ITEM->sw.value = false;
					AGENT_START_PROCESS_PROPERTY->state = INDIGO_BUSY_STATE;
					indigo_set_timer(device, 0, preview_process);
				} else if (AGENT_GUIDER_START_CALIBRATION_ITEM->sw.value) {
					AGENT_GUIDER_START_PREVIEW_ITEM->sw.value = false;
					AGENT_START_PROCESS_PROPERTY->state = INDIGO_BUSY_STATE;
					//indigo_set_timer(device, 0, streaming_batch);
				} else if (AGENT_GUIDER_START_GUIDING_ITEM->sw.value) {
					AGENT_GUIDER_START_GUIDING_ITEM->sw.value = false;
					AGENT_START_PROCESS_PROPERTY->state = INDIGO_BUSY_STATE;
					//indigo_set_timer(device, 0, streaming_batch);
				}
			}
			indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
		} else {
			AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
			indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, "%s: No CCD is selected", GUIDER_AGENT_NAME);
		}
	} else 	if (indigo_property_match(AGENT_ABORT_PROCESS_PROPERTY, property)) {
		if (*FILTER_DEVICE_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX]) {
			indigo_property_copy_values(AGENT_ABORT_PROCESS_PROPERTY, property, false);
			abort_process(device);
			AGENT_ABORT_PROCESS_ITEM->sw.value = false;
			AGENT_ABORT_PROCESS_PROPERTY->state = INDIGO_OK_STATE;
			indigo_update_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
		} else {
			AGENT_ABORT_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
			indigo_update_property(device, AGENT_ABORT_PROCESS_PROPERTY, "%s: No CCD is selected", GUIDER_AGENT_NAME);
		}
	}
	return indigo_filter_change_property(device, client, property);
}

static indigo_result agent_device_detach(indigo_device *device) {
	assert(device != NULL);
	indigo_release_property(AGENT_GUIDER_MODE_PROPERTY);
	indigo_release_property(AGENT_START_PROCESS_PROPERTY);
	indigo_release_property(AGENT_ABORT_PROCESS_PROPERTY);
	indigo_release_property(AGENT_GUIDER_STATS_PROPERTY);
	return indigo_filter_device_detach(device);
}

// -------------------------------------------------------------------------------- INDIGO agent client implementation

static indigo_result agent_define_property(indigo_client *client, indigo_device *device, indigo_property *property, const char *message) {
	return indigo_filter_define_property(client, device, property, message);
}

static indigo_result agent_update_property(indigo_client *client, indigo_device *device, indigo_property *property, const char *message) {
	if (!strcmp(property->device, GUIDER_AGENT_NAME) && (!strcmp(property->name, FILTER_CCD_LIST_PROPERTY_NAME) || !strcmp(property->name, FILTER_GUIDER_LIST_PROPERTY_NAME))) {
		if (FILTER_CCD_LIST_PROPERTY->items->sw.value || FILTER_GUIDER_LIST_PROPERTY->items->sw.value) {
			abort_process(device);
			indigo_delete_property(device, AGENT_GUIDER_MODE_PROPERTY, NULL);
			indigo_delete_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
			indigo_delete_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
			indigo_delete_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
		} else {
			indigo_define_property(device, AGENT_GUIDER_MODE_PROPERTY, NULL);
			indigo_define_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
			indigo_define_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
			indigo_define_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
		}
	} else if (*FILTER_CLIENT_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX] && !strcmp(property->device, FILTER_CLIENT_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX]) && !strcmp(property->name, CCD_IMAGE_PROPERTY_NAME)) {
		if (property->state == INDIGO_OK_STATE) {
			indigo_raw_header *header = (indigo_raw_header *)(property->items->blob.value);
			bool donuts = CLIENT_PRIVATE_DATA->agent_guider_mode_property->items[1].sw.value;
			if (header->signature == INDIGO_RAW_MONO8 || header->signature == INDIGO_RAW_MONO16 || header->signature == INDIGO_RAW_RGB24 || header->signature == INDIGO_RAW_RGB48) {
				if (CLIENT_PRIVATE_DATA->frame == 0) {
					indigo_result result;
					if (donuts)
						result = indigo_donuts_frame_digest(header->signature, (void*)header + sizeof(indigo_raw_header), header->width, header->height, &CLIENT_PRIVATE_DATA->reference);
					else
						result = indigo_centroid_frame_digest(header->signature, (void*)header + sizeof(indigo_raw_header), header->width, header->height, &CLIENT_PRIVATE_DATA->reference);
					if (result == INDIGO_OK) {
						CLIENT_PRIVATE_DATA->frame++;
					}
				} else {
					indigo_frame_digest digest;
					indigo_result result;
					if (donuts)
						result = indigo_donuts_frame_digest(header->signature, (void*)header + sizeof(indigo_raw_header), header->width, header->height, &digest);
					else
						result = indigo_centroid_frame_digest(header->signature, (void*)header + sizeof(indigo_raw_header), header->width, header->height, &digest);
					if (result == INDIGO_OK) {
						double drift_x, drift_y;
						result = indigo_calculate_drift(&CLIENT_PRIVATE_DATA->reference, &digest, &drift_x, &drift_y);
						if (result == INDIGO_OK) {
							CLIENT_PRIVATE_DATA->frame++;
							//INDIGO_DRIVER_LOG(DRIVER_NAME, "%g %g", corrections.x, corrections.y);
							CLIENT_PRIVATE_DATA->agent_stats_property->items[0].number.value = CLIENT_PRIVATE_DATA->frame;
							CLIENT_PRIVATE_DATA->agent_stats_property->items[1].number.value = drift_x;
							CLIENT_PRIVATE_DATA->agent_stats_property->items[2].number.value = drift_y;
							indigo_update_property(device, CLIENT_PRIVATE_DATA->agent_stats_property, NULL);
						}
					}
					indigo_delete_frame_digest(&digest);
				}
			}
		}
	}
	return indigo_filter_update_property(client, device, property, message);
}

static indigo_result agent_delete_property(indigo_client *client, indigo_device *device, indigo_property *property, const char *message) {
	return indigo_filter_delete_property(client, device, property, message);
}
// -------------------------------------------------------------------------------- Initialization

static agent_private_data *private_data = NULL;

static indigo_device *agent_device = NULL;
static indigo_client *agent_client = NULL;

indigo_result indigo_agent_guider(indigo_driver_action action, indigo_driver_info *info) {
	static indigo_device agent_device_template = INDIGO_DEVICE_INITIALIZER(
		GUIDER_AGENT_NAME,
		agent_device_attach,
		agent_enumerate_properties,
		agent_change_property,
		NULL,
		agent_device_detach
	);

	static indigo_client agent_client_template = {
		GUIDER_AGENT_NAME, false, NULL, INDIGO_OK, INDIGO_VERSION_CURRENT, NULL,
		indigo_filter_client_attach,
		agent_define_property,
		agent_update_property,
		agent_delete_property,
		NULL,
		indigo_filter_client_detach
	};

	static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;

	SET_DRIVER_INFO(info, GUIDER_AGENT_NAME, __FUNCTION__, DRIVER_VERSION, false, last_action);

	if (action == last_action)
		return INDIGO_OK;

	switch(action) {
		case INDIGO_DRIVER_INIT:
			last_action = action;
			private_data = malloc(sizeof(agent_private_data));
			assert(private_data != NULL);
			memset(private_data, 0, sizeof(agent_private_data));
			agent_device = malloc(sizeof(indigo_device));
			assert(agent_device != NULL);
			memcpy(agent_device, &agent_device_template, sizeof(indigo_device));
			agent_device->private_data = private_data;
			indigo_attach_device(agent_device);

			agent_client = malloc(sizeof(indigo_client));
			assert(agent_client != NULL);
			memcpy(agent_client, &agent_client_template, sizeof(indigo_client));
			agent_client->client_context = agent_device->device_context;
			indigo_attach_client(agent_client);
			break;

		case INDIGO_DRIVER_SHUTDOWN:
			last_action = action;
			if (agent_client != NULL) {
				indigo_detach_client(agent_client);
				free(agent_client);
				agent_client = NULL;
			}
			if (agent_device != NULL) {
				indigo_detach_device(agent_device);
				free(agent_device);
				agent_device = NULL;
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
