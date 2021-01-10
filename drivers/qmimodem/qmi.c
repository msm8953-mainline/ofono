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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <linux/qrtr.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <glib.h>

#include <ofono/log.h>

#include "qmi.h"
#include "ctl.h"

typedef void (*qmi_message_func_t)(uint16_t message, uint16_t length,
					const void *buffer, void *user_data);

struct discovery {
	qmi_destroy_func_t destroy;
};
struct discover_data;

struct qmi_version {
	uint8_t type;
	uint16_t major;
	uint16_t minor;
	uint16_t node;
	uint16_t port;
	const char *name;
};

struct qmi_device {
	int ref_count;
	int fd;
	GIOChannel *io;
	GSocket *socket;
	GSource *source;
	unsigned int next_cid;
	unsigned int node_id;
	bool close_on_unref;
	guint read_watch;
	guint write_watch;
	GQueue *req_queue;
	GQueue *control_queue;
	GQueue *service_queue;
	GQueue *discovery_queue;
	uint8_t next_control_tid;
	uint16_t next_service_tid;
	qmi_debug_func_t debug_func;
	void *debug_data;
	uint16_t control_major;
	uint16_t control_minor;
	char *version_str;
	struct qmi_version *version_list;
	uint8_t version_count;
	GHashTable *service_list;
	unsigned int release_users;
	qmi_shutdown_func_t shutdown_func;
	void *shutdown_user_data;
	qmi_destroy_func_t shutdown_destroy;
	guint shutdown_source;
	bool shutting_down : 1;
	bool destroyed : 1;
};

struct qmi_service {
	int ref_count;
	struct qmi_device *device;
	int port;
	uint8_t type;
	uint16_t major;
	uint16_t minor;
	uint8_t client_id;
	uint16_t next_notify_id;
	GList *notify_list;
};

struct qmi_param {
	void *data;
	uint16_t length;
};

struct qmi_result {
	uint16_t message;
	uint16_t result;
	uint16_t error;
	const void *data;
	uint16_t length;
};

struct qmi_request {
	uint16_t tid;
	uint8_t client;
	void *buf;
	size_t len;
	qmi_message_func_t callback;
	void *user_data;
};

struct qmi_notify {
	uint16_t id;
	uint16_t message;
	qmi_result_func_t callback;
	void *user_data;
	qmi_destroy_func_t destroy;
};

struct qmi_mux_hdr {
	uint8_t  frame;		/* Always 0x01 */
	uint16_t length;	/* Packet size without frame byte */
	uint8_t  flags;		/* Either 0x00 or 0x80 */
	uint8_t  service;	/* Service type (0x00 for control) */
	uint8_t  client;	/* Client identifier (0x00 for control) */
} __attribute__ ((packed));
#define QMI_MUX_HDR_SIZE 6

struct qmi_control_hdr {
	uint8_t  type;		/* Bit 1 = response, Bit 2 = indication */
	uint8_t  transaction;	/* Transaction identifier */
} __attribute__ ((packed));
#define QMI_CONTROL_HDR_SIZE 2

struct qmi_service_hdr {
	uint8_t  type;		/* Bit 2 = response, Bit 3 = indication */
	uint16_t transaction;	/* Transaction identifier */
} __attribute__ ((packed));
#define QMI_SERVICE_HDR_SIZE 3

struct qmi_message_hdr {
	uint16_t message;	/* Message identifier */
	uint16_t length;	/* Message size without header */
	uint8_t data[0];
} __attribute__ ((packed));
#define QMI_MESSAGE_HDR_SIZE 4

struct qmi_tlv_hdr {
	uint8_t type;
	uint16_t length;
	uint8_t value[0];
} __attribute__ ((packed));
#define QMI_TLV_HDR_SIZE 3

struct qrtr_service_create_data {
	struct qmi_service *service;
	qmi_create_func_t func;
	void *user_data;
};

void qmi_free(void *ptr)
{
	free(ptr);
}

static struct qmi_request *__request_alloc(uint8_t service,
				uint8_t client, uint16_t message,
				const void *data,
				uint16_t length, qmi_message_func_t func,
				void *user_data)
{
	struct qmi_request *req;
	struct qmi_mux_hdr *hdr;
	struct qmi_message_hdr *msg;
	uint16_t headroom;

	req = g_new0(struct qmi_request, 1);

	if (service == QMI_SERVICE_CONTROL)
		headroom = QMI_CONTROL_HDR_SIZE;
	else
		headroom = QMI_SERVICE_HDR_SIZE;

	req->len = QMI_MUX_HDR_SIZE + headroom + QMI_MESSAGE_HDR_SIZE + length;

	req->buf = g_malloc(req->len);

	req->client = client;

	hdr = req->buf;

	hdr->frame = 0x01;
	hdr->length = GUINT16_TO_LE(req->len - 1);
	hdr->flags = 0x00;
	hdr->service = service;
	hdr->client = client;

	msg = req->buf + QMI_MUX_HDR_SIZE + headroom;

	msg->message = GUINT16_TO_LE(message);
	msg->length = GUINT16_TO_LE(length);

	if (data && length > 0)
		memcpy(req->buf + QMI_MUX_HDR_SIZE + headroom +
					QMI_MESSAGE_HDR_SIZE, data, length);

	req->callback = func;
	req->user_data = user_data;

	return req;
}

static void __request_free(gpointer data, gpointer user_data)
{
	struct qmi_request *req = data;

	g_free(req->buf);
	g_free(req);
}

static gint __request_compare(gconstpointer a, gconstpointer b)
{
	const struct qmi_request *req = a;
	uint16_t tid = GPOINTER_TO_UINT(b);

	return req->tid - tid;
}

static void __discovery_free(gpointer data, gpointer user_data)
{
	struct discovery *d = data;
	qmi_destroy_func_t destroy = d->destroy;

	destroy(d);
}

static void __notify_free(gpointer data, gpointer user_data)
{
	struct qmi_notify *notify = data;

	if (notify->destroy)
		notify->destroy(notify->user_data);

	g_free(notify);
}

static gint __notify_compare(gconstpointer a, gconstpointer b)
{
	const struct qmi_notify *notify = a;
	uint16_t id = GPOINTER_TO_UINT(b);

	return notify->id - id;
}

static gboolean __service_compare_shared(gpointer key, gpointer value,
							gpointer user_data)
{
	struct qmi_service *service = value;
	uint8_t type = GPOINTER_TO_UINT(user_data);

	if (service->type == type)
		return TRUE;

	return FALSE;
}

static void __hexdump(const char dir, const unsigned char *buf, size_t len,
				qmi_debug_func_t function, void *user_data)
{
	static const char hexdigits[] = "0123456789abcdef";
	char str[68];
	size_t i;

	if (!function || !len)
		return;

	str[0] = dir;

	for (i = 0; i < len; i++) {
		str[((i % 16) * 3) + 1] = ' ';
		str[((i % 16) * 3) + 2] = hexdigits[buf[i] >> 4];
		str[((i % 16) * 3) + 3] = hexdigits[buf[i] & 0xf];
		str[(i % 16) + 51] = isprint(buf[i]) ? buf[i] : '.';

		if ((i + 1) % 16 == 0) {
			str[49] = ' ';
			str[50] = ' ';
			str[67] = '\0';
			function(str, user_data);
			str[0] = ' ';
		}
	}

	if (i % 16 > 0) {
		size_t j;
		for (j = (i % 16); j < 16; j++) {
			str[(j * 3) + 1] = ' ';
			str[(j * 3) + 2] = ' ';
			str[(j * 3) + 3] = ' ';
			str[j + 51] = ' ';
		}
		str[49] = ' ';
		str[50] = ' ';
		str[67] = '\0';
		function(str, user_data);
	}
}

static const char *__service_type_to_string(uint8_t type)
{
	switch (type) {
	case QMI_SERVICE_CONTROL:
		return "CTL";
	case QMI_SERVICE_WDS:
		return "WDS";
	case QMI_SERVICE_DMS:
		return "DMS";
	case QMI_SERVICE_NAS:
		return "NAS";
	case QMI_SERVICE_QOS:
		return "QOS";
	case QMI_SERVICE_WMS:
		return "WMS";
	case QMI_SERVICE_PDS:
		return "PDS";
	case QMI_SERVICE_AUTH:
		return "AUTH";
	case QMI_SERVICE_AT:
		return "AT";
	case QMI_SERVICE_VOICE:
		return "VOICE";
	case QMI_SERVICE_CAT:
		return "CAT";
	case QMI_SERVICE_UIM:
		return "UIM";
	case QMI_SERVICE_PBM:
		return "PBM";
	case QMI_SERVICE_QCHAT:
		return "QCHAT";
	case QMI_SERVICE_RMTFS:
		return "RMTFS";
	case QMI_SERVICE_TEST:
		return "TEST";
	case QMI_SERVICE_LOC:
		return "LOC";
	case QMI_SERVICE_SAR:
		return "SAR";
	case QMI_SERVICE_CSD:
		return "CSD";
	case QMI_SERVICE_EFS:
		return "EFS";
	case QMI_SERVICE_TS:
		return "TS";
	case QMI_SERVICE_TMD:
		return "TMD";
	case QMI_SERVICE_WDA:
		return "WDA";
	case QMI_SERVICE_CSVT:
		return "CSVT";
	case QMI_SERVICE_COEX:
		return "COEX";
	case QMI_SERVICE_PDC:
		return "PDC";
	case QMI_SERVICE_RFRPE:
		return "RFRPE";
	case QMI_SERVICE_DSD:
		return "DSD";
	case QMI_SERVICE_SSCTL:
		return "SSCTL";
	case QMI_SERVICE_CAT_OLD:
		return "CAT";
	case QMI_SERVICE_RMS:
		return "RMS";
	case QMI_SERVICE_OMA:
		return "OMA";
	}

	return NULL;
}

static const struct {
	uint16_t err;
	const char *str;
} __error_table[] = {
	{ 0x0000, "NONE"			},
	{ 0x0001, "MALFORMED_MSG"		},
	{ 0x0002, "NO_MEMORY"			},
	{ 0x0003, "INTERNAL"			},
	{ 0x0004, "ABORTED"			},
	{ 0x0005, "CLIENT_IDS_EXHAUSTED"	},
	{ 0x0006, "UNABORTABLE_TRANSACTION"	},
	{ 0x0007, "INVALID_CLIENT_ID"		},
	{ 0x0008, "NO_THRESHOLDS"		},
	{ 0x0009, "INVALID_HANDLE"		},
	{ 0x000a, "INVALID_PROFILE"		},
	{ 0x000b, "INVALID_PINID"		},
	{ 0x000c, "INCORRECT_PIN"		},
	{ 0x000d, "NO_NETWORK_FOUND"		},
	{ 0x000e, "CALL_FAILED"			},
	{ 0x000f, "OUT_OF_CALL"			},
	{ 0x0010, "NOT_PROVISIONED"		},
	{ 0x0011, "MISSING_ARG"			},
	{ 0x0013, "ARG_TOO_LONG"		},
	{ 0x0016, "INVALID_TX_ID"		},
	{ 0x0017, "DEVICE_IN_USE"		},
	{ 0x0018, "OP_NETWORK_UNSUPPORTED"	},
	{ 0x0019, "OP_DEVICE_UNSUPPORTED"	},
	{ 0x001a, "NO_EFFECT"			},
	{ 0x001b, "NO_FREE_PROFILE"		},
	{ 0x001c, "INVALID_PDP_TYPE"		},
	{ 0x001d, "INVALID_TECH_PREF"		},
	{ 0x001e, "INVALID_PROFILE_TYPE"	},
	{ 0x001f, "INVALID_SERVICE_TYPE"	},
	{ 0x0020, "INVALID_REGISTER_ACTION"	},
	{ 0x0021, "INVALID_PS_ATTACH_ACTION"	},
	{ 0x0022, "AUTHENTICATION_FAILED"	},
	{ 0x0023, "PIN_BLOCKED"			},
	{ 0x0024, "PIN_PERM_BLOCKED"		},
	{ 0x0025, "UIM_NOT_INITIALIZED"		},
	{ 0x0026, "MAX_QOS_REQUESTS_IN_USE"	},
	{ 0x0027, "INCORRECT_FLOW_FILTER"	},
	{ 0x0028, "NETWORK_QOS_UNAWARE"		},
	{ 0x0029, "INVALID_QOS_ID/INVALID_ID"	},
	{ 0x002a, "REQUESTED_NUM_UNSUPPORTED"	},
	{ 0x002b, "INTERFACE_NOT_FOUND"		},
	{ 0x002c, "FLOW_SUSPENDED"		},
	{ 0x002d, "INVALID_DATA_FORMAT"		},
	{ 0x002e, "GENERAL"			},
	{ 0x002f, "UNKNOWN"			},
	{ 0x0030, "INVALID_ARG"			},
	{ 0x0031, "INVALID_INDEX"		},
	{ 0x0032, "NO_ENTRY"			},
	{ 0x0033, "DEVICE_STORAGE_FULL"		},
	{ 0x0034, "DEVICE_NOT_READY"		},
	{ 0x0035, "NETWORK_NOT_READY"		},
	{ 0x0036, "CAUSE_CODE"			},
	{ 0x0037, "MESSAGE_NOT_SENT"		},
	{ 0x0038, "MESSAGE_DELIVERY_FAILURE"	},
	{ 0x0039, "INVALID_MESSAGE_ID"		},
	{ 0x003a, "ENCODING"			},
	{ 0x003b, "AUTHENTICATION_LOCK"		},
	{ 0x003c, "INVALID_TRANSACTION"		},
	{ 0x0041, "SESSION_INACTIVE"		},
	{ 0x0042, "SESSION_INVALID"		},
	{ 0x0043, "SESSION_OWNERSHIP"		},
	{ 0x0044, "INSUFFICIENT_RESOURCES"	},
	{ 0x0045, "DISABLED"			},
	{ 0x0046, "INVALID_OPERATION"		},
	{ 0x0047, "INVALID_QMI_CMD"		},
	{ 0x0048, "TPDU_TYPE"			},
	{ 0x0049, "SMSC_ADDR"			},
	{ 0x004a, "INFO_UNAVAILABLE"		},
	{ 0x004b, "SEGMENT_TOO_LONG"		},
	{ 0x004c, "SEGEMENT_ORDER"		},
	{ 0x004d, "BUNDLING_NOT_SUPPORTED"	},
	{ 0x004f, "POLICY_MISMATCH"		},
	{ 0x0050, "SIM_FILE_NOT_FOUND"		},
	{ 0x0051, "EXTENDED_INTERNAL"		},
	{ 0x0052, "ACCESS_DENIED"		},
	{ 0x0053, "HARDWARE_RESTRICTED"		},
	{ 0x0054, "ACK_NOT_SENT"		},
	{ 0x0055, "INJECT_TIMEOUT"		},
	{ }
};

static const char *__error_to_string(uint16_t error)
{
	int i;

	for (i = 0; __error_table[i].str; i++) {
		if (__error_table[i].err == error)
			return __error_table[i].str;
	}

	return NULL;
}

int qmi_error_to_ofono_cme(int qmi_error)
{
	switch (qmi_error) {
	case 0x0019:
		return 4; /* Not Supported */
	case 0x0052:
		return 32; /* Access Denied */
	default:
		return -1;
	}
}

static void __debug_msg(const char dir, const void *buf, size_t len,
				qmi_debug_func_t function, void *user_data)
{
	const struct qmi_mux_hdr *hdr;
	const struct qmi_message_hdr *msg;
	const char *service;
	const void *ptr;
	uint16_t offset;
	char strbuf[72 + 16], *str;
	bool pending_print = false;

	if (!function || !len)
		return;

	hdr = buf;

	str = strbuf;
	service = __service_type_to_string(hdr->service);
	if (service)
		str += sprintf(str, "%c   %s", dir, service);
	else
		str += sprintf(str, "%c   %d", dir, hdr->service);

	if (hdr->service == QMI_SERVICE_CONTROL) {
		const struct qmi_control_hdr *ctl;
		const char *type;

		ctl = buf + QMI_MUX_HDR_SIZE;
		msg = buf + QMI_MUX_HDR_SIZE + QMI_CONTROL_HDR_SIZE;
		ptr = buf + QMI_MUX_HDR_SIZE + QMI_CONTROL_HDR_SIZE +
							QMI_MESSAGE_HDR_SIZE;

		switch (ctl->type) {
		case 0x00:
			type = "_req";
			break;
		case 0x01:
			type = "_resp";
			break;
		case 0x02:
			type = "_ind";
			break;
		default:
			type = "";
			break;
		}

		str += sprintf(str, "%s msg=%d len=%d", type,
					GUINT16_FROM_LE(msg->message),
					GUINT16_FROM_LE(msg->length));

		str += sprintf(str, " [client=%d,type=%d,tid=%d,len=%d]",
					hdr->client, ctl->type,
					ctl->transaction,
					GUINT16_FROM_LE(hdr->length));
	} else {
		const struct qmi_service_hdr *srv;
		const char *type;

		srv = buf + QMI_MUX_HDR_SIZE;
		msg = buf + QMI_MUX_HDR_SIZE + QMI_SERVICE_HDR_SIZE;
		ptr = buf + QMI_MUX_HDR_SIZE + QMI_SERVICE_HDR_SIZE +
							QMI_MESSAGE_HDR_SIZE;

		switch (srv->type) {
		case 0x00:
			type = "_req";
			break;
		case 0x02:
			type = "_resp";
			break;
		case 0x04:
			type = "_ind";
			break;
		default:
			type = "";
			break;
		}

		str += sprintf(str, "%s msg=%d len=%d", type,
					GUINT16_FROM_LE(msg->message),
					GUINT16_FROM_LE(msg->length));

		str += sprintf(str, " [client=%d,type=%d,tid=%d,len=%d]",
					hdr->client, srv->type,
					GUINT16_FROM_LE(srv->transaction),
					GUINT16_FROM_LE(hdr->length));
	}

	function(strbuf, user_data);

	if (!msg->length)
		return;

	str = strbuf;
	str += sprintf(str, "      ");
	offset = 0;

	while (offset + QMI_TLV_HDR_SIZE < GUINT16_FROM_LE(msg->length)) {
		const struct qmi_tlv_hdr *tlv = ptr + offset;
		uint16_t tlv_length = GUINT16_FROM_LE(tlv->length);

		if (tlv->type == 0x02 && tlv_length == QMI_RESULT_CODE_SIZE) {
			const struct qmi_result_code *result = ptr + offset +
							QMI_TLV_HDR_SIZE;
			uint16_t error = GUINT16_FROM_LE(result->error);
			const char *error_str;

			error_str = __error_to_string(error);
			if (error_str)
				str += sprintf(str, " {type=%d,error=%s}",
							tlv->type, error_str);
			else
				str += sprintf(str, " {type=%d,error=%d}",
							tlv->type, error);
		} else {
			str += sprintf(str, " {type=%d,len=%d}", tlv->type,
								tlv_length);
		}

		if (str - strbuf > 60) {
			function(strbuf, user_data);

			str = strbuf;
			str += sprintf(str, "      ");

			pending_print = false;
		} else
			pending_print = true;

		offset += QMI_TLV_HDR_SIZE + tlv_length;
	}

	if (pending_print)
		function(strbuf, user_data);
}

static void __debug_device(struct qmi_device *device,
					const char *format, ...)
{
	char strbuf[72 + 16];
	va_list ap;

	if (!device->debug_func)
		return;

	va_start(ap, format);
	vsnprintf(strbuf, sizeof(strbuf), format, ap);
	va_end(ap);

	device->debug_func(strbuf, device->debug_data);
}

static gboolean can_write_data(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct qmi_device *device = user_data;
	struct qmi_mux_hdr *hdr;
	struct qmi_request *req;
	ssize_t bytes_written;

	req = g_queue_pop_head(device->req_queue);
	if (!req)
		return FALSE;

	bytes_written = write(device->fd, req->buf, req->len);
	if (bytes_written < 0)
		return FALSE;

	__hexdump('>', req->buf, bytes_written,
				device->debug_func, device->debug_data);

	__debug_msg(' ', req->buf, bytes_written,
				device->debug_func, device->debug_data);

	hdr = req->buf;

	if (hdr->service == QMI_SERVICE_CONTROL)
		g_queue_push_tail(device->control_queue, req);
	else
		g_queue_push_tail(device->service_queue, req);

	g_free(req->buf);
	req->buf = NULL;

	if (g_queue_get_length(device->req_queue) > 0)
		return TRUE;

	return FALSE;
}

static void write_watch_destroy(gpointer user_data)
{
	struct qmi_device *device = user_data;

	device->write_watch = 0;
}

static void wakeup_writer(struct qmi_device *device)
{
	if (device->write_watch > 0)
		return;

	device->write_watch = g_io_add_watch_full(device->io, G_PRIORITY_HIGH,
				G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				can_write_data, device, write_watch_destroy);
}

#define CALC_SZ(count) ((count + 31) / 32)
static gboolean qrtr_handle_ctrl_packet(struct qmi_device *device,
				     char *buf, int len)
{
	struct qrtr_ctrl_pkt* ctrl_pkt = (struct qrtr_ctrl_pkt*) buf;
	struct qmi_version *version;
	int32_t type;
	int32_t new_count;
	const char *name;
	int i;
	DBG ("");

	if (len < sizeof(*ctrl_pkt))
		return TRUE;

	type = GUINT32_FROM_LE (ctrl_pkt->cmd);

	if (GUINT32_FROM_LE(ctrl_pkt->server.node) != device->node_id)
		return TRUE;

	switch (type) {
	case QRTR_TYPE_NEW_SERVER:
		new_count = device->version_count + 1;
		if (CALC_SZ(new_count) > CALC_SZ(device->version_count)) {
			device->version_list = g_realloc(device->version_list,
					CALC_SZ(new_count) * 32 * sizeof(device->version_list[0]));

		}
		version = &device->version_list[new_count - 1];
		version->type = GUINT32_FROM_LE (ctrl_pkt->server.service);
		version->node = GUINT32_FROM_LE (ctrl_pkt->server.node);
		version->port = GUINT32_FROM_LE (ctrl_pkt->server.port);
		version->major = GUINT32_FROM_LE (ctrl_pkt->server.instance) & 0xff;
		version->minor = GUINT32_FROM_LE (ctrl_pkt->server.instance) >> 8;

		name = __service_type_to_string(version->type);

		__debug_device(device, "found service [%d (%s) %d.%d]",
				version->type, name ?: "unknown",
				version->major, version->minor);
		version->name = name;
		break;
	case QRTR_TYPE_DEL_SERVER:
		new_count = device->version_count - 1;
		if (new_count < 0)
			return TRUE;

		for (i = 0; i < new_count; i++) {
			if (device->version_list[i].node != GUINT32_FROM_LE (ctrl_pkt->server.node) ||
			    device->version_list[i].port != GUINT32_FROM_LE (ctrl_pkt->server.port))
				continue;
			for (; i < new_count; i++)
				device->version_list[i] = device->version_list[i+1];
			break;
		}
		break;
	default:
		return TRUE;
	}

	device->version_count = new_count;
	return TRUE;
}

bool qrtr_send_packet(GSocket *socket,
		unsigned int node,
		unsigned int port,
		char *buf, size_t buf_len)
{
	struct sockaddr_qrtr addr;
	socklen_t len = sizeof (addr);
	int ret, fd;
	DBG ("node=%u port=%u len=%lu", node, port, buf_len);

	fd = g_socket_get_fd (socket);

	if (port == QRTR_PORT_CTRL) {
		ret = getsockname (fd, (struct sockaddr *)&addr, &len);
		if (ret < 0)
			return false;
	} else {
		addr.sq_node = node;
	}
	addr.sq_family = AF_QIPCRTR;
	addr.sq_port = port;

	ret = sendto (fd, buf, buf_len, 0, (struct sockaddr *) &addr, sizeof (addr));
	if (ret < 0)
		return false;

	return true;
}

bool qrtr_send_lookup(GSocket *socket)
{
	struct qrtr_ctrl_pkt ctrl_pkt = { 0 };
	DBG ("");

	ctrl_pkt.cmd = GUINT32_TO_LE (QRTR_TYPE_NEW_LOOKUP);

	return qrtr_send_packet(socket, 0, QRTR_PORT_CTRL,
			(char*) &ctrl_pkt, sizeof (ctrl_pkt));
}

static void qrtr_request_submit(struct qmi_device *device,
				struct qmi_request *req)
{
	struct qmi_mux_hdr *hdr = req->buf;
	GHashTableIter iter;
	gpointer key, value;
	int port = -1;
	DBG ("");

	g_hash_table_iter_init (&iter, device->service_list);
	while (g_hash_table_iter_next (&iter, &key, &value))
	{
		struct qmi_service *svc = value;
		if (svc->type != hdr->service)
			continue;

		port = svc->port;
		break;
	}

	if (port < 0)
		return;

	g_assert(req->len > QMI_MUX_HDR_SIZE);

	if(!qrtr_send_packet(device->socket,
			device->node_id,
			port,
			(char*)req->buf + QMI_MUX_HDR_SIZE,
			req->len - QMI_MUX_HDR_SIZE))
		DBG("Failed to send request");

	__hexdump('>', req->buf, req->len,
				device->debug_func, device->debug_data);

	__debug_msg(' ', req->buf, req->len,
				device->debug_func, device->debug_data);

	g_queue_push_tail(device->service_queue, req);
}

GSocket* qrtr_socket_create(GSourceFunc input_callback,
				void *userdata, GSource **source)
{
	GSocket* gsocket;
	int fd;
	DBG ("");

	if (!source)
		return NULL;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);

	gsocket = g_socket_new_from_fd (fd, NULL);
	if (!gsocket) {
		close (fd);
		return NULL;
	}

	g_socket_set_timeout (gsocket, 0);

	*source = g_socket_create_source (gsocket, G_IO_IN, NULL);
	g_source_set_callback (*source,
                           input_callback,
                           userdata,
                           NULL);

	g_source_attach (*source, g_main_context_get_thread_default ());

	return gsocket;
}

static gboolean qrtr_service_create_callback(gpointer callback_data)
{
	struct qrtr_service_create_data *data =
		(struct qrtr_service_create_data*) callback_data;
	DBG ("");

	data->func(data->service, data->user_data);
	qmi_service_unref(data->service);
	g_free(data);
	return FALSE;
}

static bool qrtr_service_create(struct qmi_device *device,
				uint8_t type, qmi_create_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct qmi_version *svc_version = NULL;
	struct qmi_service *service = NULL;
	struct qrtr_service_create_data *data = NULL;
	unsigned int hash_id;
	int i;
	DBG ("%d", type);

	if (!device->version_list)
		return false;

	__debug_device(device, "service create [type=%d]", type);

	for (i = 0; i < device->version_count; i++)
		if (device->version_list[i].type == type) {
			svc_version = &device->version_list[i];
			break;
		}

	if (!svc_version)
		return false;

	service = g_try_new0(struct qmi_service, 1);
	if (!service)
		return false;

	service->ref_count = 1;
	service->device = device;
	service->type = type;
	service->major = svc_version->major;
	service->minor = svc_version->minor;
	service->port = svc_version->port;
	service->client_id = device->next_cid++;

	data = g_try_new0(struct qrtr_service_create_data, 1);
	if (!data) {
		g_free(service);
		return false;
	}

	data->service = service;
	data->func = func;
	data->user_data = user_data;

	__debug_device(device, "service created [client=%d,type=%d,port=%u]",
					service->client_id, service->type,
					service->port);

	hash_id = service->type | (service->client_id << 8);

	g_hash_table_replace(device->service_list,
				GUINT_TO_POINTER(hash_id), service);


	g_timeout_add_seconds(0, qrtr_service_create_callback, data);

	return true;
}

static uint16_t __request_submit(struct qmi_device *device,
				struct qmi_request *req)
{
	struct qmi_mux_hdr *mux;

	mux = req->buf;

	if (mux->service == QMI_SERVICE_CONTROL) {
		struct qmi_control_hdr *hdr;

		g_assert (!device->socket);

		hdr = req->buf + QMI_MUX_HDR_SIZE;
		hdr->type = 0x00;
		hdr->transaction = device->next_control_tid++;
		if (device->next_control_tid == 0)
			device->next_control_tid = 1;
		req->tid = hdr->transaction;
	} else {
		struct qmi_service_hdr *hdr;
		hdr = req->buf + QMI_MUX_HDR_SIZE;
		hdr->type = 0x00;
		hdr->transaction = device->next_service_tid++;
		if (device->next_service_tid < 256)
			device->next_service_tid = 256;
		req->tid = hdr->transaction;
	}

	if (device->socket) {
		qrtr_request_submit(device, req);
		return req->tid;
	}

	g_queue_push_tail(device->req_queue, req);

	wakeup_writer(device);

	return req->tid;
}

static void service_notify(gpointer key, gpointer value, gpointer user_data)
{
	struct qmi_service *service = value;
	struct qmi_result *result = user_data;
	GList *list;

	for (list = g_list_first(service->notify_list); list;
						list = g_list_next(list)) {
		struct qmi_notify *notify = list->data;

		if (notify->message == result->message)
			notify->callback(result, notify->user_data);
	}
}

static void handle_indication(struct qmi_device *device,
			uint8_t service_type, uint8_t client_id,
			uint16_t message, uint16_t length, const void *data)
{
	struct qmi_service *service;
	struct qmi_result result;
	unsigned int hash_id;

	if (service_type == QMI_SERVICE_CONTROL)
		return;

	result.result = 0;
	result.error = 0;
	result.message = message;
	result.data = data;
	result.length = length;

	if (client_id == 0xff) {
		g_hash_table_foreach(device->service_list,
						service_notify, &result);
		return;
	}

	hash_id = service_type | (client_id << 8);

	service = g_hash_table_lookup(device->service_list,
					GUINT_TO_POINTER(hash_id));
	if (!service)
		return;

	service_notify(NULL, service, &result);
}

static void handle_packet(struct qmi_device *device,
				const struct qmi_mux_hdr *hdr, const void *buf)
{
	struct qmi_request *req;
	uint16_t message, length;
	const void *data;

	if (hdr->service == QMI_SERVICE_CONTROL) {
		const struct qmi_control_hdr *control = buf;
		const struct qmi_message_hdr *msg;
		unsigned int tid;
		GList *list;

		/* Ignore control messages with client identifier */
		if (hdr->client != 0x00)
			return;

		msg = buf + QMI_CONTROL_HDR_SIZE;

		message = GUINT16_FROM_LE(msg->message);
		length = GUINT16_FROM_LE(msg->length);

		data = buf + QMI_CONTROL_HDR_SIZE + QMI_MESSAGE_HDR_SIZE;

		tid = control->transaction;

		if (control->type == 0x02 && control->transaction == 0x00) {
			handle_indication(device, hdr->service, hdr->client,
							message, length, data);
			return;
		}

		list = g_queue_find_custom(device->control_queue,
				GUINT_TO_POINTER(tid), __request_compare);
		if (!list)
			return;

		req = list->data;

		g_queue_delete_link(device->control_queue, list);
	} else {
		const struct qmi_service_hdr *service = buf;
		const struct qmi_message_hdr *msg;
		unsigned int tid;
		GList *list;

		msg = buf + QMI_SERVICE_HDR_SIZE;

		message = GUINT16_FROM_LE(msg->message);
		length = GUINT16_FROM_LE(msg->length);

		data = buf + QMI_SERVICE_HDR_SIZE + QMI_MESSAGE_HDR_SIZE;

		tid = GUINT16_FROM_LE(service->transaction);

		if (service->type == 0x04) {
			handle_indication(device, hdr->service, hdr->client,
							message, length, data);
			return;
		}

		list = g_queue_find_custom(device->service_queue,
				GUINT_TO_POINTER(tid), __request_compare);
		if (!list)
			return;

		req = list->data;

		g_queue_delete_link(device->service_queue, list);
	}

	if (req->callback)
		req->callback(message, length, data, req->user_data);

	__request_free(req, NULL);
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct qmi_device *device = user_data;
	struct qmi_mux_hdr *hdr;
	unsigned char buf[2048];
	ssize_t bytes_read;
	uint16_t offset;

	if (cond & G_IO_NVAL)
		return FALSE;

	bytes_read = read(device->fd, buf, sizeof(buf));
	if (bytes_read < 0)
		return TRUE;

	__hexdump('<', buf, bytes_read,
				device->debug_func, device->debug_data);

	offset = 0;

	while (offset < bytes_read) {
		uint16_t len;

		/* Check if QMI mux header fits into packet */
		if (bytes_read - offset < QMI_MUX_HDR_SIZE)
			break;

		hdr = (void *) (buf + offset);

		/* Check for fixed frame and flags value */
		if (hdr->frame != 0x01 || hdr->flags != 0x80)
			break;

		len = GUINT16_FROM_LE(hdr->length) + 1;

		/* Check that packet size matches frame size */
		if (bytes_read - offset < len)
			break;

		__debug_msg(' ', buf + offset, len,
				device->debug_func, device->debug_data);

		handle_packet(device, hdr, buf + offset + QMI_MUX_HDR_SIZE);

		offset += len;
	}

	return TRUE;
}

static void read_watch_destroy(gpointer user_data)
{
	struct qmi_device *device = user_data;

	device->read_watch = 0;
}

static void __qmi_device_discovery_started(struct qmi_device *device,
						struct discovery *d)
{
	g_queue_push_tail(device->discovery_queue, d);
}

static void __qmi_device_discovery_complete(struct qmi_device *device,
						struct discovery *d)
{
	if (g_queue_remove(device->discovery_queue, d) != TRUE)
		return;

	__discovery_free(d, NULL);
}

static void service_destroy(gpointer data)
{
	struct qmi_service *service = data;

	if (!service->device)
		return;

	service->device = NULL;
}

struct qmi_device *qmi_device_new(int fd)
{
	struct qmi_device *device;
	long flags;

	device = g_try_new0(struct qmi_device, 1);
	if (!device)
		return NULL;

	__debug_device(device, "device %p new", device);

	device->ref_count = 1;

	device->fd = fd;
	device->close_on_unref = false;
	device->socket = NULL;

	flags = fcntl(device->fd, F_GETFL, NULL);
	if (flags < 0) {
		g_free(device);
		return NULL;
	}

	if (!(flags & O_NONBLOCK)) {
		if (fcntl(device->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
			g_free(device);
			return NULL;
		}
	}

	device->io = g_io_channel_unix_new(device->fd);

	g_io_channel_set_encoding(device->io, NULL, NULL);
	g_io_channel_set_buffered(device->io, FALSE);

	device->read_watch = g_io_add_watch_full(device->io, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, device, read_watch_destroy);

	g_io_channel_unref(device->io);

	device->req_queue = g_queue_new();
	device->control_queue = g_queue_new();
	device->service_queue = g_queue_new();
	device->discovery_queue = g_queue_new();

	device->service_list = g_hash_table_new_full(g_direct_hash,
					g_direct_equal, NULL, service_destroy);

	device->next_control_tid = 1;
	device->next_service_tid = 256;

	return device;
}

static gboolean qrtr_receive(GSocket *socket,
				     GIOCondition cond,
				     struct qmi_device *device)
{
	struct qmi_mux_hdr *hdr;
	g_autoptr(GSocketAddress) gaddr = NULL;
	ssize_t bytes_recv;
	struct sockaddr_qrtr addr;
	char buf[2048];
	GHashTableIter iter;
	gpointer key, value;
	bool found = false;

	DBG ("");
	bytes_recv = g_socket_receive_from (socket, &gaddr,
					    buf + QMI_MUX_HDR_SIZE,
					    sizeof(buf) - QMI_MUX_HDR_SIZE, NULL, NULL);

	if (bytes_recv < 0)
		return FALSE;

	if (!g_socket_address_to_native (gaddr, &addr, sizeof(addr), NULL)) {
		DBG ("Parse QRTR address failed");
		return TRUE;
	}

	g_assert(addr.sq_family == AF_QIPCRTR);

	DBG ("port %d node %d", addr.sq_port, addr.sq_node);

	if (addr.sq_port == QRTR_PORT_CTRL) {
		qrtr_handle_ctrl_packet(device, buf + QMI_MUX_HDR_SIZE, bytes_recv);
		return TRUE;
	}

	if (bytes_recv < QMI_MUX_HDR_SIZE)
		return TRUE;


	hdr = (struct qmi_mux_hdr*) buf;
	hdr->frame = 0x01;
	hdr->length = GUINT16_TO_LE(bytes_recv - 1);
	hdr->flags = 0x80;

	__hexdump('<', (guchar *) buf, bytes_recv,
	          device->debug_func, device->debug_data);

	g_hash_table_iter_init (&iter, device->service_list);
	while (g_hash_table_iter_next (&iter, &key, &value))
	{
		struct qmi_service *svc = value;
		if (svc->port != addr.sq_port)
			continue;

		hdr->service = (uint8_t) svc->type;
		hdr->client = svc->client_id;
		found = true;
		break;
	}

	if (!found)
		return true;

	handle_packet(device, hdr, buf + QMI_MUX_HDR_SIZE);
	return true;
}

struct qmi_device *qmi_device_new_qrtr(int node)
{
	struct qmi_device *device;
	DBG ("");

	device = g_try_new0(struct qmi_device, 1);
	if (!device)
		return NULL;

	__debug_device(device, "device %p new (QRTR)", device);

	device->ref_count = 1;
	device->fd = -1;
	device->close_on_unref = false;
	device->node_id = node;
	device->next_cid = 1;

	device->socket = qrtr_socket_create ((GSourceFunc) qrtr_receive,
			device, &device->source);
	if (!device->socket) {
		DBG("Error creating qipcrtr socket");
		g_free(device);
		return NULL;
	}

	device->req_queue = g_queue_new();
	device->control_queue = g_queue_new();
	device->service_queue = g_queue_new();
	device->discovery_queue = g_queue_new();

	device->service_list = g_hash_table_new_full(g_direct_hash,
					g_direct_equal, NULL, service_destroy);

	device->next_control_tid = 1;
	device->next_service_tid = 256;

	return device;
}

struct qmi_device *qmi_device_ref(struct qmi_device *device)
{
	if (!device)
		return NULL;

	__sync_fetch_and_add(&device->ref_count, 1);

	return device;
}

void qmi_device_unref(struct qmi_device *device)
{
	if (!device)
		return;

	if (__sync_sub_and_fetch(&device->ref_count, 1))
		return;

	__debug_device(device, "device %p free", device);

	g_queue_foreach(device->control_queue, __request_free, NULL);
	g_queue_free(device->control_queue);

	g_queue_foreach(device->service_queue, __request_free, NULL);
	g_queue_free(device->service_queue);

	g_queue_foreach(device->req_queue, __request_free, NULL);
	g_queue_free(device->req_queue);

	g_queue_foreach(device->discovery_queue, __discovery_free, NULL);
	g_queue_free(device->discovery_queue);

	if (device->write_watch > 0)
		g_source_remove(device->write_watch);

	if (device->read_watch > 0)
		g_source_remove(device->read_watch);

	if (device->close_on_unref)
		close(device->fd);

	if (device->shutdown_source)
		g_source_remove(device->shutdown_source);

	g_hash_table_destroy(device->service_list);

	g_free(device->version_str);
	g_free(device->version_list);

	if (device->shutting_down)
		device->destroyed = true;
	else
		g_free(device);
}

void qmi_device_set_debug(struct qmi_device *device,
				qmi_debug_func_t func, void *user_data)
{
	if (device == NULL)
		return;

	device->debug_func = func;
	device->debug_data = user_data;
}

void qmi_device_set_close_on_unref(struct qmi_device *device, bool do_close)
{
	if (!device)
		return;

	device->close_on_unref = do_close;
}

void qmi_result_print_tlvs(struct qmi_result *result)
{
	const void *ptr = result->data;
	uint16_t len = result->length;

	while (len > QMI_TLV_HDR_SIZE) {
		const struct qmi_tlv_hdr *tlv = ptr;
		uint16_t tlv_length = GUINT16_FROM_LE(tlv->length);

		DBG("tlv: 0x%02x len 0x%04x", tlv->type, tlv->length);

		ptr += QMI_TLV_HDR_SIZE + tlv_length;
		len -= QMI_TLV_HDR_SIZE + tlv_length;
	}
}


static const void *tlv_get(const void *data, uint16_t size,
					uint8_t type, uint16_t *length)
{
	const void *ptr = data;
	uint16_t len = size;

	while (len > QMI_TLV_HDR_SIZE) {
		const struct qmi_tlv_hdr *tlv = ptr;
		uint16_t tlv_length = GUINT16_FROM_LE(tlv->length);

		if (tlv->type == type) {
			if (length)
				*length = tlv_length;

			return ptr + QMI_TLV_HDR_SIZE;
		}

		ptr += QMI_TLV_HDR_SIZE + tlv_length;
		len -= QMI_TLV_HDR_SIZE + tlv_length;
	}

	return NULL;
}

bool qmi_device_get_service_version(struct qmi_device *device, uint8_t type,
					uint16_t *major, uint16_t *minor)
{
	struct qmi_version *info;
	int i;

	for (i = 0, info = device->version_list;
			i < device->version_count;
			i++, info++) {
		if (info->type == type) {
			*major = info->major;
			*minor = info->minor;
			return true;
		}
	}

	return false;
}

bool qmi_device_has_service(struct qmi_device *device, uint8_t type)
{
	struct qmi_version *info;
	int i;

	for (i = 0, info = device->version_list;
			i < device->version_count;
			i++, info++) {
		if (info->type == type) {
			return true;
		}
	}

	return false;
}

struct discover_data {
	struct discovery super;
	struct qmi_device *device;
	qmi_discover_func_t func;
	void *user_data;
	qmi_destroy_func_t destroy;
	uint8_t tid;
	guint timeout;
};

static void discover_data_free(gpointer user_data)
{
	struct discover_data *data = user_data;

	if (data->timeout) {
		g_source_remove(data->timeout);
		data->timeout = 0;
	}

	if (data->destroy)
		data->destroy(data->user_data);

	g_free(data);
}

static void discover_callback(uint16_t message, uint16_t length,
					const void *buffer, void *user_data)
{
	struct discover_data *data = user_data;
	struct qmi_device *device = data->device;
	const struct qmi_result_code *result_code;
	const struct qmi_service_list *service_list;
	const void *ptr;
	uint16_t len;
	struct qmi_version *list;
	uint8_t count;
	unsigned int i;

	count = 0;
	list = NULL;

	result_code = tlv_get(buffer, length, 0x02, &len);
	if (!result_code)
		goto done;

	if (len != QMI_RESULT_CODE_SIZE)
		goto done;

	service_list = tlv_get(buffer, length, 0x01, &len);
	if (!service_list)
		goto done;

	if (len < QMI_SERVICE_LIST_SIZE)
		goto done;

	list = g_try_malloc(sizeof(struct qmi_version) * service_list->count);
	if (!list)
		goto done;

	for (i = 0; i < service_list->count; i++) {
		uint16_t major =
			GUINT16_FROM_LE(service_list->services[i].major);
		uint16_t minor =
			GUINT16_FROM_LE(service_list->services[i].minor);
		uint8_t type = service_list->services[i].type;
		const char *name = __service_type_to_string(type);

		if (name)
			__debug_device(device, "found service [%s %d.%d]",
					name, major, minor);
		else
			__debug_device(device, "found service [%d %d.%d]",
					type, major, minor);

		if (type == QMI_SERVICE_CONTROL) {
			device->control_major = major;
			device->control_minor = minor;
			continue;
		}

		list[count].type = type;
		list[count].major = major;
		list[count].minor = minor;
		list[count].name = name;

		count++;
	}

	ptr = tlv_get(buffer, length, 0x10, &len);
	if (!ptr)
		goto done;

	device->version_str = strndup(ptr + 1, *((uint8_t *) ptr));

done:
	device->version_list = list;
	device->version_count = count;

	if (data->func)
		data->func(data->user_data);

	__qmi_device_discovery_complete(data->device, &data->super);
}

static gboolean discover_reply(gpointer user_data)
{
	struct discover_data *data = user_data;
	struct qmi_device *device = data->device;
	unsigned int tid = data->tid;
	GList *list;
	struct qmi_request *req = NULL;

	data->timeout = 0;

	/* remove request from queues */
	if (tid != 0) {
		list = g_queue_find_custom(device->req_queue,
				GUINT_TO_POINTER(tid), __request_compare);

		if (list) {
			req = list->data;
			g_queue_delete_link(device->req_queue, list);
		} else {
			list = g_queue_find_custom(device->control_queue,
				GUINT_TO_POINTER(tid), __request_compare);

			if (list) {
				req = list->data;
				g_queue_delete_link(device->control_queue,
								list);
			}
		}
	}

	if (data->func)
		data->func(data->user_data);

	__qmi_device_discovery_complete(data->device, &data->super);

	if (req)
		__request_free(req, NULL);

	return FALSE;
}

bool qmi_device_discover(struct qmi_device *device, qmi_discover_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct discover_data *data;
	struct qmi_request *req;
	uint8_t tid;

	if (!device)
		return false;

	__debug_device(device, "device %p discover", device);

	data = g_try_new0(struct discover_data, 1);
	if (!data)
		return false;

	data->super.destroy = discover_data_free;
	data->device = device;
	data->func = func;
	data->user_data = user_data;
	data->destroy = destroy;

	if (device->version_list) {
		data->timeout = g_timeout_add_seconds(0, discover_reply, data);
		__qmi_device_discovery_started(device, &data->super);
		return true;
	}

	if (device->socket) {
		if (!qrtr_send_lookup(device->socket))
		{
			g_free(data);
			return false;
		}
		goto done;
	}

	req = __request_alloc(QMI_SERVICE_CONTROL, 0x00,
			QMI_CTL_GET_VERSION_INFO,
			NULL, 0, discover_callback, data);

	tid = __request_submit(device, req);

	data->tid = tid;
done:

	data->timeout = g_timeout_add_seconds(5, discover_reply, data);
	__qmi_device_discovery_started(device, &data->super);

	return true;
}

static void release_client(struct qmi_device *device,
				uint8_t type, uint8_t client_id,
				qmi_message_func_t func, void *user_data)
{
	unsigned char release_req[] = { 0x01, 0x02, 0x00, type, client_id };
	struct qmi_request *req;

	if (device->socket)
		return;

	req = __request_alloc(QMI_SERVICE_CONTROL, 0x00,
			QMI_CTL_RELEASE_CLIENT_ID,
			release_req, sizeof(release_req),
			func, user_data);

	__request_submit(device, req);
}

static void shutdown_destroy(gpointer user_data)
{
	struct qmi_device *device = user_data;

	if (device->shutdown_destroy)
		device->shutdown_destroy(device->shutdown_user_data);

	device->shutdown_source = 0;

	if (device->destroyed)
		g_free(device);
}

static gboolean shutdown_callback(gpointer user_data)
{
	struct qmi_device *device = user_data;

	if (device->release_users > 0)
		return TRUE;

	device->shutting_down = true;

	if (device->shutdown_func)
		device->shutdown_func(device->shutdown_user_data);

	device->shutting_down = true;

	return FALSE;
}

bool qmi_device_shutdown(struct qmi_device *device, qmi_shutdown_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	if (!device)
		return false;

	if (device->shutdown_source > 0)
		return false;

	__debug_device(device, "device %p shutdown", device);

	device->shutdown_source = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
						0, shutdown_callback, device,
						shutdown_destroy);
	if (device->shutdown_source == 0)
		return false;

	device->shutdown_func = func;
	device->shutdown_user_data = user_data;
	device->shutdown_destroy = destroy;

	return true;
}

struct sync_data {
	qmi_sync_func_t func;
	void *user_data;
};

static void qmi_device_sync_callback(uint16_t message, uint16_t length,
				     const void *buffer, void *user_data)
{
	struct sync_data *data = user_data;

	if (data->func)
		data->func(data->user_data);

	g_free(data);
}

/* sync will release all previous clients */
bool qmi_device_sync(struct qmi_device *device,
		     qmi_sync_func_t func, void *user_data)
{
	struct qmi_request *req;
	struct sync_data *func_data;

	if (!device)
		return false;

	__debug_device(device, "Sending sync to reset QMI");

	func_data = g_new0(struct sync_data, 1);
	func_data->func = func;
	func_data->user_data = user_data;

	req = __request_alloc(QMI_SERVICE_CONTROL, 0x00,
			QMI_CTL_SYNC,
			NULL, 0,
			qmi_device_sync_callback, func_data);

	__request_submit(device, req);

	return true;
}

/* if the device support the QMI call SYNC over the CTL interface */
bool qmi_device_is_sync_supported(struct qmi_device *device)
{
	if (device == NULL)
		return false;

	if (device->socket)
		return false;

	return (device->control_major > 1 ||
		(device->control_major == 1 && device->control_minor >= 5));
}

static bool get_device_file_name(struct qmi_device *device,
					char *file_name, int size)
{
	pid_t pid;
	char temp[100];
	ssize_t result;

	if (size <= 0)
		return false;

	pid = getpid();

	snprintf(temp, 100, "/proc/%d/fd/%d", (int) pid, device->fd);
	temp[99] = 0;

	result = readlink(temp, file_name, size - 1);

	if (result == -1 || result >= size - 1) {
		DBG("Error %d in readlink", errno);
		return false;
	}

	file_name[result] = 0;

	return true;
}

static char *get_first_dir_in_directory(char *dir_path)
{
	DIR *dir;
	struct dirent *dir_entry;
	char *dir_name = NULL;

	dir = opendir(dir_path);

	if (!dir)
		return NULL;

	dir_entry = readdir(dir);

	while ((dir_entry != NULL)) {
		if (dir_entry->d_type == DT_DIR &&
				strcmp(dir_entry->d_name, ".") != 0 &&
				strcmp(dir_entry->d_name, "..") != 0) {
			dir_name = g_strdup(dir_entry->d_name);
			break;
		}

		dir_entry = readdir(dir);
	}

	closedir(dir);
	return dir_name;
}

static char *get_device_interface(struct qmi_device *device)
{
	char * const driver_names[] = { "usbmisc", "usb" };
	unsigned int i;
	char file_path[PATH_MAX];
	char *file_name;
	char *interface = NULL;

	if (!get_device_file_name(device, file_path, sizeof(file_path)))
		return NULL;

	file_name = basename(file_path);

	for (i = 0; i < G_N_ELEMENTS(driver_names) && !interface; i++) {
		gchar *sysfs_path;

		sysfs_path = g_strdup_printf("/sys/class/%s/%s/device/net/",
						driver_names[i], file_name);
		interface = get_first_dir_in_directory(sysfs_path);
		g_free(sysfs_path);
	}

	return interface;
}

enum qmi_device_expected_data_format qmi_device_get_expected_data_format(
						struct qmi_device *device)
{
	char *sysfs_path = NULL;
	char *interface = NULL;
	int fd = -1;
	char value;
	enum qmi_device_expected_data_format expected =
					QMI_DEVICE_EXPECTED_DATA_FORMAT_UNKNOWN;

	if (!device)
		goto done;

	interface = get_device_interface(device);

	if (!interface) {
		DBG("Error while getting interface name");
		goto done;
	}

	/* Build sysfs file path and open it */
	sysfs_path = g_strdup_printf("/sys/class/net/%s/qmi/raw_ip", interface);

	fd = open(sysfs_path, O_RDONLY);
	if (fd < 0) {
		/* maybe not supported by kernel */
		DBG("Error %d in open(%s)", errno, sysfs_path);
		goto done;
	}

	if (read(fd, &value, 1) != 1) {
		DBG("Error %d in read(%s)", errno, sysfs_path);
		goto done;
	}

	if (value == 'Y')
		expected = QMI_DEVICE_EXPECTED_DATA_FORMAT_RAW_IP;
	else if (value == 'N')
		expected = QMI_DEVICE_EXPECTED_DATA_FORMAT_802_3;
	else
		DBG("Unexpected sysfs file contents");

done:
	if (fd >= 0)
		close(fd);

	if (sysfs_path)
		g_free(sysfs_path);

	if (interface)
		g_free(interface);

	return expected;
}

bool qmi_device_set_expected_data_format(struct qmi_device *device,
			enum qmi_device_expected_data_format format)
{
	bool res = false;
	char *sysfs_path = NULL;
	char *interface = NULL;
	int fd = -1;
	char value;

	if (!device)
		goto done;

	switch (format) {
	case QMI_DEVICE_EXPECTED_DATA_FORMAT_802_3:
		value = 'N';
		break;
	case QMI_DEVICE_EXPECTED_DATA_FORMAT_RAW_IP:
		value = 'Y';
		break;
	default:
		DBG("Unhandled format: %d", (int) format);
		goto done;
	}

	interface = get_device_interface(device);

	if (!interface) {
		DBG("Error while getting interface name");
		goto done;
	}

	/* Build sysfs file path and open it */
	sysfs_path = g_strdup_printf("/sys/class/net/%s/qmi/raw_ip", interface);

	fd = open(sysfs_path, O_WRONLY);
	if (fd < 0) {
		/* maybe not supported by kernel */
		DBG("Error %d in open(%s)", errno, sysfs_path);
		goto done;
	}

	if (write(fd, &value, 1) != 1) {
		DBG("Error %d in write(%s)", errno, sysfs_path);
		goto done;
	}

	res = true;

done:
	if (fd >= 0)
		close(fd);

	if (sysfs_path)
		g_free(sysfs_path);

	if (interface)
		g_free(interface);

	return res;
}

struct qmi_param *qmi_param_new(void)
{
	struct qmi_param *param;

	param = g_try_new0(struct qmi_param, 1);
	if (!param)
		return NULL;

	return param;
}

void qmi_param_free(struct qmi_param *param)
{
	if (!param)
		return;

	g_free(param->data);
	g_free(param);
}

bool qmi_param_append(struct qmi_param *param, uint8_t type,
					uint16_t length, const void *data)
{
	struct qmi_tlv_hdr *tlv;
	void *ptr;

	if (!param || !type)
		return false;

	if (!length)
		return true;

	if (!data)
		return false;

	if (param->data)
		ptr = g_try_realloc(param->data,
				param->length + QMI_TLV_HDR_SIZE + length);
	else
		ptr = g_try_malloc(QMI_TLV_HDR_SIZE + length);

	if (!ptr)
		return false;

	tlv = ptr + param->length;

	tlv->type = type;
	tlv->length = GUINT16_TO_LE(length);
	memcpy(tlv->value, data, length);

	param->data = ptr;
	param->length += QMI_TLV_HDR_SIZE + length;

	return true;
}

bool qmi_param_append_uint8(struct qmi_param *param, uint8_t type,
							uint8_t value)
{
	unsigned char buf[1] = { value };

	return qmi_param_append(param, type, sizeof(buf), buf);
}

bool qmi_param_append_uint16(struct qmi_param *param, uint8_t type,
							uint16_t value)
{
	unsigned char buf[2] = { value & 0xff, (value & 0xff00) >> 8 };

	return qmi_param_append(param, type, sizeof(buf), buf);
}

bool qmi_param_append_uint32(struct qmi_param *param, uint8_t type,
							uint32_t value)
{
	unsigned char buf[4] = { value & 0xff, (value & 0xff00) >> 8,
					(value & 0xff0000) >> 16,
					(value & 0xff000000) >> 24 };

	return qmi_param_append(param, type, sizeof(buf), buf);
}

struct qmi_param *qmi_param_new_uint8(uint8_t type, uint8_t value)
{
	struct qmi_param *param;

	param = qmi_param_new();
	if (!param)
		return NULL;

	if (!qmi_param_append_uint8(param, type, value)) {
		qmi_param_free(param);
		return NULL;
	}

	return param;
}

struct qmi_param *qmi_param_new_uint16(uint8_t type, uint16_t value)
{
	struct qmi_param *param;

	param = qmi_param_new();
	if (!param)
		return NULL;

	if (!qmi_param_append_uint16(param, type, value)) {
		qmi_param_free(param);
		return NULL;
	}

	return param;
}

struct qmi_param *qmi_param_new_uint32(uint8_t type, uint32_t value)
{
	struct qmi_param *param;

	param = qmi_param_new();
	if (!param)
		return NULL;

	if (!qmi_param_append_uint32(param, type, value)) {
		qmi_param_free(param);
		return NULL;
	}

	return param;
}

bool qmi_result_set_error(struct qmi_result *result, uint16_t *error)
{
	if (!result) {
		if (error)
			*error = 0xffff;
		return true;
	}

	if (result->result == 0x0000)
		return false;

	if (error)
		*error = result->error;

	return true;
}

const char *qmi_result_get_error(struct qmi_result *result)
{
	if (!result)
		return NULL;

	if (result->result == 0x0000)
		return NULL;

	return __error_to_string(result->error);
}

const void *qmi_result_get(struct qmi_result *result, uint8_t type,
							uint16_t *length)
{
	if (!result || !type)
		return NULL;

	return tlv_get(result->data, result->length, type, length);
}

char *qmi_result_get_string(struct qmi_result *result, uint8_t type)
{
	const void *ptr;
	uint16_t len;

	if (!result || !type)
		return NULL;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return NULL;

	return strndup(ptr, len);
}

bool qmi_result_get_uint8(struct qmi_result *result, uint8_t type,
							uint8_t *value)
{
	const unsigned char *ptr;
	uint16_t len;

	if (!result || !type)
		return false;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return false;

	if (value)
		*value = *ptr;

	return true;
}

bool qmi_result_get_int16(struct qmi_result *result, uint8_t type,
							int16_t *value)
{
	const unsigned char *ptr;
	uint16_t len, tmp;

	if (!result || !type)
		return false;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return false;

	memcpy(&tmp, ptr, 2);

	if (value)
		*value = GINT16_FROM_LE(tmp);

	return true;
}

bool qmi_result_get_uint16(struct qmi_result *result, uint8_t type,
							uint16_t *value)
{
	const unsigned char *ptr;
	uint16_t len, tmp;

	if (!result || !type)
		return false;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return false;

	memcpy(&tmp, ptr, 2);

	if (value)
		*value = GUINT16_FROM_LE(tmp);

	return true;
}

bool qmi_result_get_uint32(struct qmi_result *result, uint8_t type,
							uint32_t *value)
{
	const unsigned char *ptr;
	uint16_t len;
	uint32_t tmp;

	if (!result || !type)
		return false;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return false;

	memcpy(&tmp, ptr, 4);

	if (value)
		*value = GUINT32_FROM_LE(tmp);

	return true;
}

bool qmi_result_get_uint64(struct qmi_result *result, uint8_t type,
							uint64_t *value)
{
	const unsigned char *ptr;
	uint16_t len;
	uint64_t tmp;

	if (!result || !type)
		return false;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return false;

	memcpy(&tmp, ptr, 8);

	if (value)
		*value = GUINT64_FROM_LE(tmp);

	return true;
}

struct service_create_data {
	struct discovery super;
	struct qmi_device *device;
	uint8_t type;
	uint16_t major;
	uint16_t minor;
	qmi_create_func_t func;
	void *user_data;
	qmi_destroy_func_t destroy;
	guint timeout;
};

static void service_create_data_free(gpointer user_data)
{
	struct service_create_data *data = user_data;

	if (data->timeout) {
		g_source_remove(data->timeout);
		data->timeout = 0;
	}

	if (data->destroy)
		data->destroy(data->user_data);

	g_free(data);
}

static gboolean service_create_reply(gpointer user_data)
{
	struct service_create_data *data = user_data;

	data->timeout = 0;
	data->func(NULL, data->user_data);

	__qmi_device_discovery_complete(data->device, &data->super);

	return FALSE;
}

static void service_create_callback(uint16_t message, uint16_t length,
					const void *buffer, void *user_data)
{
	struct service_create_data *data = user_data;
	struct qmi_device *device = data->device;
	struct qmi_service *service = NULL;
	const struct qmi_result_code *result_code;
	const struct qmi_client_id *client_id;
	uint16_t len;
	unsigned int hash_id;

	result_code = tlv_get(buffer, length, 0x02, &len);
	if (!result_code)
		goto done;

	if (len != QMI_RESULT_CODE_SIZE)
		goto done;

	client_id = tlv_get(buffer, length, 0x01, &len);
	if (!client_id)
		goto done;

	if (len != QMI_CLIENT_ID_SIZE)
		goto done;

	if (client_id->service != data->type)
		goto done;

	service = g_try_new0(struct qmi_service, 1);
	if (!service)
		goto done;

	service->ref_count = 1;
	service->device = data->device;

	service->type = data->type;
	service->major = data->major;
	service->minor = data->minor;

	service->client_id = client_id->client;

	__debug_device(device, "service created [client=%d,type=%d]",
					service->client_id, service->type);

	hash_id = service->type | (service->client_id << 8);

	g_hash_table_replace(device->service_list,
				GUINT_TO_POINTER(hash_id), service);

done:
	data->func(service, data->user_data);
	qmi_service_unref(service);

	__qmi_device_discovery_complete(data->device, &data->super);
}

static bool service_create(struct qmi_device *device,
				uint8_t type, qmi_create_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct service_create_data *data;
	unsigned char client_req[] = { 0x01, 0x01, 0x00, type };
	struct qmi_request *req;
	int i;

	if (device->socket)
		return qrtr_service_create(device, type, func, user_data, destroy);

	data = g_try_new0(struct service_create_data, 1);
	if (!data)
		return false;

	if (!device->version_list)
		return false;

	data->super.destroy = service_create_data_free;
	data->device = device;
	data->type = type;
	data->func = func;
	data->user_data = user_data;
	data->destroy = destroy;

	__debug_device(device, "service create [type=%d]", type);

	for (i = 0; i < device->version_count; i++) {
		if (device->version_list[i].type == data->type) {
			data->major = device->version_list[i].major;
			data->minor = device->version_list[i].minor;
			break;
		}
	}

	req = __request_alloc(QMI_SERVICE_CONTROL, 0x00,
			QMI_CTL_GET_CLIENT_ID,
			client_req, sizeof(client_req),
			service_create_callback, data);

	__request_submit(device, req);

	data->timeout = g_timeout_add_seconds(8, service_create_reply, data);
	__qmi_device_discovery_started(device, &data->super);

	return true;
}

struct service_create_shared_data {
	struct discovery super;
	struct qmi_service *service;
	struct qmi_device *device;
	qmi_create_func_t func;
	void *user_data;
	qmi_destroy_func_t destroy;
	guint timeout;
};

static void service_create_shared_data_free(gpointer user_data)
{
	struct service_create_shared_data *data = user_data;

	if (data->timeout) {
		g_source_remove(data->timeout);
		data->timeout = 0;
	}

	qmi_service_unref(data->service);

	if (data->destroy)
		data->destroy(data->user_data);

	g_free(data);
}

static gboolean service_create_shared_reply(gpointer user_data)
{
	struct service_create_shared_data *data = user_data;

	data->timeout = 0;
	data->func(data->service, data->user_data);

	__qmi_device_discovery_complete(data->device, &data->super);

	return FALSE;
}

bool qmi_service_create_shared(struct qmi_device *device,
				uint8_t type, qmi_create_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct qmi_service *service;
	unsigned int type_val = type;

	if (!device || !func)
		return false;

	if (type == QMI_SERVICE_CONTROL)
		return false;

	service = g_hash_table_find(device->service_list,
			__service_compare_shared, GUINT_TO_POINTER(type_val));
	if (service) {
		struct service_create_shared_data *data;

		data = g_try_new0(struct service_create_shared_data, 1);
		if (!data)
			return false;

		data->super.destroy = service_create_shared_data_free;
		data->service = qmi_service_ref(service);
		data->device = device;
		data->func = func;
		data->user_data = user_data;
		data->destroy = destroy;

		data->timeout = g_timeout_add(0,
					service_create_shared_reply, data);
		__qmi_device_discovery_started(device, &data->super);

		return 0;
	}

	return service_create(device, type, func, user_data, destroy);
}

bool qmi_service_create(struct qmi_device *device,
				uint8_t type, qmi_create_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	return qmi_service_create_shared(device, type, func,
						user_data, destroy);
}

static void service_release_callback(uint16_t message, uint16_t length,
					const void *buffer, void *user_data)
{
	struct qmi_service *service = user_data;

	if (service->device)
		service->device->release_users--;

	g_free(service);
}

struct qmi_service *qmi_service_ref(struct qmi_service *service)
{
	if (!service)
		return NULL;

	__sync_fetch_and_add(&service->ref_count, 1);

	return service;
}

void qmi_service_unref(struct qmi_service *service)
{
	unsigned int hash_id;

	if (!service)
                return;

	if (__sync_sub_and_fetch(&service->ref_count, 1))
		return;

	if (!service->device) {
		g_free(service);
		return;
	}

	qmi_service_cancel_all(service);
	qmi_service_unregister_all(service);

	hash_id = service->type | (service->client_id << 8);

	g_hash_table_steal(service->device->service_list,
					GUINT_TO_POINTER(hash_id));

	service->device->release_users++;

	release_client(service->device, service->type, service->client_id,
					service_release_callback, service);
}

const char *qmi_service_get_identifier(struct qmi_service *service)
{
	if (!service)
		return NULL;

	return __service_type_to_string(service->type);
}

bool qmi_service_get_version(struct qmi_service *service,
					uint16_t *major, uint16_t *minor)
{
	if (!service)
		return false;

	if (major)
		*major = service->major;

	if (minor)
		*minor = service->minor;

	return true;
}

struct service_send_data {
	qmi_result_func_t func;
	void *user_data;
	qmi_destroy_func_t destroy;
};

static void service_send_free(struct service_send_data *data)
{
	if (data->destroy)
		data->destroy(data->user_data);

	g_free(data);
}

static void service_send_callback(uint16_t message, uint16_t length,
					const void *buffer, void *user_data)
{
	struct service_send_data *data = user_data;
	const struct qmi_result_code *result_code;
	uint16_t len;
	struct qmi_result result;

	result.message = message;
	result.data = buffer;
	result.length = length;

	result_code = tlv_get(buffer, length, 0x02, &len);
	if (!result_code)
		goto done;

	if (len != QMI_RESULT_CODE_SIZE)
		goto done;

	result.result = GUINT16_FROM_LE(result_code->result);
	result.error = GUINT16_FROM_LE(result_code->error);

done:
	if (data->func)
		data->func(&result, data->user_data);

	service_send_free(data);
}

uint16_t qmi_service_send(struct qmi_service *service,
				uint16_t message, struct qmi_param *param,
				qmi_result_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct qmi_device *device;
	struct service_send_data *data;
	struct qmi_request *req;
	uint16_t tid;

	if (!service)
		return 0;

	if (!service->client_id)
		return 0;

	device = service->device;
	if (!device)
		return 0;

	data = g_try_new0(struct service_send_data, 1);
	if (!data)
		return 0;

	data->func = func;
	data->user_data = user_data;
	data->destroy = destroy;

	req = __request_alloc(service->type, service->client_id,
				message,
				param ? param->data : NULL,
				param ? param->length : 0,
				service_send_callback, data);

	qmi_param_free(param);

	tid = __request_submit(device, req);

	return tid;
}

bool qmi_service_cancel(struct qmi_service *service, uint16_t id)
{
	unsigned int tid = id;
	struct qmi_device *device;
	struct qmi_request *req;
	GList *list;

	if (!service || !tid)
		return false;

	if (!service->client_id)
		return false;

	device = service->device;
	if (!device)
		return false;

	list = g_queue_find_custom(device->req_queue,
				GUINT_TO_POINTER(tid), __request_compare);
	if (list) {
		req = list->data;

		g_queue_delete_link(device->req_queue, list);
	} else {
		list = g_queue_find_custom(device->service_queue,
				GUINT_TO_POINTER(tid), __request_compare);
		if (!list)
			return false;

		req = list->data;

		g_queue_delete_link(device->service_queue, list);
	}

	service_send_free(req->user_data);

	__request_free(req, NULL);

	return true;
}

static GQueue *remove_client(GQueue *queue, uint8_t client)
{
	GQueue *new_queue;
	GList *list;

	new_queue = g_queue_new();

	while (1) {
		struct qmi_request *req;

		list = g_queue_pop_head_link(queue);
		if (!list)
			break;

		req = list->data;

		if (!req->client || req->client != client) {
			g_queue_push_tail_link(new_queue, list);
			continue;
		}

		service_send_free(req->user_data);

		__request_free(req, NULL);
	}

	g_queue_free(queue);

	return new_queue;
}

bool qmi_service_cancel_all(struct qmi_service *service)
{
	struct qmi_device *device;

	if (!service)
		return false;

	if (!service->client_id)
		return false;

	device = service->device;
	if (!device)
		return false;

	device->req_queue = remove_client(device->req_queue,
						service->client_id);

	device->service_queue = remove_client(device->service_queue,
							service->client_id);

	return true;
}

uint16_t qmi_service_register(struct qmi_service *service,
				uint16_t message, qmi_result_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct qmi_notify *notify;

	if (!service || !func)
		return 0;

	notify = g_try_new0(struct qmi_notify, 1);
	if (!notify)
		return 0;

	if (service->next_notify_id < 1)
		service->next_notify_id = 1;

	notify->id = service->next_notify_id++;
	notify->message = message;
	notify->callback = func;
	notify->user_data = user_data;
	notify->destroy = destroy;

	service->notify_list = g_list_append(service->notify_list, notify);

	return notify->id;
}

bool qmi_service_unregister(struct qmi_service *service, uint16_t id)
{
	unsigned int nid = id;
	struct qmi_notify *notify;
	GList *list;

	if (!service || !id)
		return false;

	list = g_list_find_custom(service->notify_list,
				GUINT_TO_POINTER(nid), __notify_compare);
	if (!list)
		return false;

	notify = list->data;

	service->notify_list = g_list_delete_link(service->notify_list, list);

	__notify_free(notify, NULL);

	return true;
}

bool qmi_service_unregister_all(struct qmi_service *service)
{
	if (!service)
		return false;

	g_list_foreach(service->notify_list, __notify_free, NULL);
	g_list_free(service->notify_list);

	service->notify_list = NULL;

	return true;
}
