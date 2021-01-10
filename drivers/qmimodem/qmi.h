/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef __OFONO_QMI_QMI_H
#define __OFONO_QMI_QMI_H

#include <stdbool.h>
#include <stdint.h>
#include <gio/gio.h>

#define QMI_SERVICE_CONTROL	0	/* Control service */
#define QMI_SERVICE_WDS		1	/* Wireless data service */
#define QMI_SERVICE_DMS		2	/* Device management service */
#define QMI_SERVICE_NAS		3	/* Network access service */
#define QMI_SERVICE_QOS		4	/* Quality of service, error service */
#define QMI_SERVICE_WMS		5	/* Wireless messaging service */
#define QMI_SERVICE_PDS		6	/* Position determination service */
#define QMI_SERVICE_AUTH	7	/* Authentication service */
#define QMI_SERVICE_AT		8	/* AT command processor service */
#define QMI_SERVICE_VOICE	9	/* Voice service */
#define QMI_SERVICE_CAT		10	/* Card application toolkit service */
#define QMI_SERVICE_UIM		11	/* UIM service */
#define QMI_SERVICE_PBM		12	/* Phonebook service */
#define QMI_SERVICE_QCHAT	13
#define QMI_SERVICE_RMTFS	14	/* Remote file system service */
#define QMI_SERVICE_TEST	15
#define QMI_SERVICE_LOC		16	/* Location service */
#define QMI_SERVICE_SAR		17	/* Specific absorption rate service */
#define QMI_SERVICE_CSD		20	/* Core sound driver service */
#define QMI_SERVICE_EFS		21	/* Embedded file system service */
#define QMI_SERVICE_TS		23	/* Thermal sensors service */
#define QMI_SERVICE_TMD		24	/* Thermal mitigation device service */
#define QMI_SERVICE_WDA		26	/* Wireless data administrative service */
#define QMI_SERVICE_CSVT	29
#define QMI_SERVICE_COEX	34
#define QMI_SERVICE_PDC		36	/* Persistent device configuration service */
#define QMI_SERVICE_RFRPE	41
#define QMI_SERVICE_DSD		42
#define QMI_SERVICE_SSCTL	43
#define QMI_SERVICE_DPM		47	/* Data Port Mapper */
#define QMI_SERVICE_CAT_OLD	224	/* Card application toolkit service */
#define QMI_SERVICE_RMS		225	/* Remote management service */
#define QMI_SERVICE_OMA		226	/* OMA device management service */

enum qmi_device_expected_data_format {
	QMI_DEVICE_EXPECTED_DATA_FORMAT_UNKNOWN,
	QMI_DEVICE_EXPECTED_DATA_FORMAT_802_3,
	QMI_DEVICE_EXPECTED_DATA_FORMAT_RAW_IP,
};

void qmi_free(void *ptr);

typedef void (*qmi_destroy_func_t)(void *user_data);


struct qmi_device;

typedef void (*qmi_debug_func_t)(const char *str, void *user_data);
typedef void (*qmi_sync_func_t)(void *user_data);
typedef void (*qmi_shutdown_func_t)(void *user_data);
typedef void (*qmi_discover_func_t)(void *user_data);

struct qmi_device *qmi_device_new(int fd);
struct qmi_device *qmi_device_new_qrtr(int node);
bool qrtr_send_packet(GSocket *socket,
			unsigned int node,
			unsigned int port,
			char *buf, size_t len);
bool qrtr_send_lookup(GSocket *socket);
GSocket* qrtr_socket_create(GSourceFunc input_callback,
				void *userdata, GSource **source);

struct qmi_device *qmi_device_ref(struct qmi_device *device);
void qmi_device_unref(struct qmi_device *device);

void qmi_device_set_debug(struct qmi_device *device,
				qmi_debug_func_t func, void *user_data);

void qmi_device_set_close_on_unref(struct qmi_device *device, bool do_close);

bool qmi_device_discover(struct qmi_device *device, qmi_discover_func_t func,
				void *user_data, qmi_destroy_func_t destroy);
bool qmi_device_shutdown(struct qmi_device *device, qmi_shutdown_func_t func,
				void *user_data, qmi_destroy_func_t destroy);

bool qmi_device_has_service(struct qmi_device *device, uint8_t type);
bool qmi_device_get_service_version(struct qmi_device *device, uint8_t type,
					uint16_t *major, uint16_t *minor);

bool qmi_device_sync(struct qmi_device *device,
		     qmi_sync_func_t func, void *user_data);
bool qmi_device_is_sync_supported(struct qmi_device *device);

enum qmi_device_expected_data_format qmi_device_get_expected_data_format(
						struct qmi_device *device);
bool qmi_device_set_expected_data_format(struct qmi_device *device,
			enum qmi_device_expected_data_format format);

struct qmi_param;

struct qmi_param *qmi_param_new(void);
void qmi_param_free(struct qmi_param *param);

bool qmi_param_append(struct qmi_param *param, uint8_t type,
					uint16_t length, const void *data);
bool qmi_param_append_uint8(struct qmi_param *param, uint8_t type,
							uint8_t value);
bool qmi_param_append_uint16(struct qmi_param *param, uint8_t type,
							uint16_t value);
bool qmi_param_append_uint32(struct qmi_param *param, uint8_t type,
							uint32_t value);

struct qmi_param *qmi_param_new_uint8(uint8_t type, uint8_t value);
struct qmi_param *qmi_param_new_uint16(uint8_t type, uint16_t value);
struct qmi_param *qmi_param_new_uint32(uint8_t type, uint32_t value);


struct qmi_result;

bool qmi_result_set_error(struct qmi_result *result, uint16_t *error);
const char *qmi_result_get_error(struct qmi_result *result);

const void *qmi_result_get(struct qmi_result *result, uint8_t type,
							uint16_t *length);
char *qmi_result_get_string(struct qmi_result *result, uint8_t type);
bool qmi_result_get_uint8(struct qmi_result *result, uint8_t type,
							uint8_t *value);
bool qmi_result_get_int16(struct qmi_result *result, uint8_t type,
							int16_t *value);
bool qmi_result_get_uint16(struct qmi_result *result, uint8_t type,
							uint16_t *value);
bool qmi_result_get_uint32(struct qmi_result *result, uint8_t type,
							uint32_t *value);
bool qmi_result_get_uint64(struct qmi_result *result, uint8_t type,
							uint64_t *value);
void qmi_result_print_tlvs(struct qmi_result *result);

int qmi_error_to_ofono_cme(int qmi_error);

struct qmi_service;

typedef void (*qmi_result_func_t)(struct qmi_result *result, void *user_data);

typedef void (*qmi_create_func_t)(struct qmi_service *service, void *user_data);

bool qmi_service_create(struct qmi_device *device,
				uint8_t type, qmi_create_func_t func,
				void *user_data, qmi_destroy_func_t destroy);
bool qmi_service_create_shared(struct qmi_device *device,
				uint8_t type, qmi_create_func_t func,
				void *user_data, qmi_destroy_func_t destroy);

struct qmi_service *qmi_service_ref(struct qmi_service *service);
void qmi_service_unref(struct qmi_service *service);

const char *qmi_service_get_identifier(struct qmi_service *service);
bool qmi_service_get_version(struct qmi_service *service,
					uint16_t *major, uint16_t *minor);

uint16_t qmi_service_send(struct qmi_service *service,
				uint16_t message, struct qmi_param *param,
				qmi_result_func_t func,
				void *user_data, qmi_destroy_func_t destroy);
bool qmi_service_cancel(struct qmi_service *service, uint16_t id);
bool qmi_service_cancel_all(struct qmi_service *service);

uint16_t qmi_service_register(struct qmi_service *service,
				uint16_t message, qmi_result_func_t func,
				void *user_data, qmi_destroy_func_t destroy);
bool qmi_service_unregister(struct qmi_service *service, uint16_t id);
bool qmi_service_unregister_all(struct qmi_service *service);


/* FIXME: find a place for parse_error */
enum parse_error {
	NONE = 0,
	MISSING_MANDATORY = 1,
	INVALID_LENGTH = 2,
};

#endif /* __OFONO_QMI_QMI_H */
