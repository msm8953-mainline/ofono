/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2018 Gemalto M2M
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

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/log.h>
#include <ofono/types.h>
#include <ofono/modem.h>

#include <drivers/common/call_list.h>

#include "atutil.h"
#include "vendor.h"

static const char *cpin_prefix[] = { "+CPIN:", NULL };

struct at_util_sim_state_query {
	GAtChat *chat;
	guint cpin_poll_source;
	guint cpin_poll_count;
	guint interval;
	guint num_times;
	at_util_sim_inserted_cb_t cb;
	void *userdata;
	GDestroyNotify destroy;
};

static gboolean cpin_check(gpointer userdata);

void decode_at_error(struct ofono_error *error, const char *final)
{
	if (!strcmp(final, "OK")) {
		error->type = OFONO_ERROR_TYPE_NO_ERROR;
		error->error = 0;
	} else if (g_str_has_prefix(final, "+CMS ERROR:")) {
		error->type = OFONO_ERROR_TYPE_CMS;
		error->error = strtol(&final[11], NULL, 0);
	} else if (g_str_has_prefix(final, "+CME ERROR:")) {
		error->type = OFONO_ERROR_TYPE_CME;
		error->error = strtol(&final[11], NULL, 0);
	} else {
		error->type = OFONO_ERROR_TYPE_FAILURE;
		error->error = 0;
	}
}

gint at_util_call_compare_by_phone_number(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	const struct ofono_phone_number *pb = b;

	return memcmp(&call->phone_number, pb,
				sizeof(struct ofono_phone_number));
}

gint at_util_call_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	unsigned int id = GPOINTER_TO_UINT(b);

	if (id < call->id)
		return -1;

	if (id > call->id)
		return 1;

	return 0;
}

GSList *at_util_parse_clcc(GAtResult *result, unsigned int *ret_mpty_ids)
{
	GAtResultIter iter;
	GSList *l = NULL;
	int id, dir, status, type;
	ofono_bool_t mpty;
	struct ofono_call *call;
	unsigned int mpty_ids = 0;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CLCC:")) {
		const char *str = "";
		int number_type = 129;

		if (!g_at_result_iter_next_number(&iter, &id))
			continue;

		if (id == 0)
			continue;

		if (!g_at_result_iter_next_number(&iter, &dir))
			continue;

		if (!g_at_result_iter_next_number(&iter, &status))
			continue;

		if (status > 5)
			continue;

		if (!g_at_result_iter_next_number(&iter, &type))
			continue;

		if (!g_at_result_iter_next_number(&iter, &mpty))
			continue;

		if (g_at_result_iter_next_string(&iter, &str))
			g_at_result_iter_next_number(&iter, &number_type);

		call = g_try_new(struct ofono_call, 1);
		if (call == NULL)
			break;

		ofono_call_init(call);

		call->id = id;
		call->direction = dir;
		call->status = status;
		call->type = type;
		strncpy(call->phone_number.number, str,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
		call->phone_number.type = number_type;

		if (strlen(call->phone_number.number) > 0)
			call->clip_validity = 0;
		else
			call->clip_validity = 2;

		l = g_slist_insert_sorted(l, call, ofono_call_compare);

		if (mpty)
			mpty_ids |= 1 << id;
	}

	if (ret_mpty_ids)
		*ret_mpty_ids = mpty_ids;

	return l;
}

gboolean at_util_parse_reg_unsolicited(GAtResult *result, const char *prefix,
					int *status,
					int *lac, int *ci, int *tech,
					unsigned int vendor)
{
	GAtResultIter iter;
	int s;
	int l = -1, c = -1, t = -1;
	const char *str;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, prefix) == FALSE)
		return FALSE;

	if (g_at_result_iter_next_number(&iter, &s) == FALSE)
		return FALSE;

	/* Some firmware will report bogus lac/ci when unregistered */
	if (s != 1 && s != 5)
		goto out;

	switch (vendor) {
	case OFONO_VENDOR_GOBI:
	case OFONO_VENDOR_ZTE:
	case OFONO_VENDOR_HUAWEI:
	case OFONO_VENDOR_NOVATEL:
	case OFONO_VENDOR_SPEEDUP:
		if (g_at_result_iter_next_unquoted_string(&iter, &str) == TRUE)
			l = strtol(str, NULL, 16);
		else
			goto out;

		if (g_at_result_iter_next_unquoted_string(&iter, &str) == TRUE)
			c = strtol(str, NULL, 16);
		else
			goto out;

		break;
	default:
		if (g_at_result_iter_next_string(&iter, &str) == TRUE)
			l = strtol(str, NULL, 16);
		else
			goto out;

		if (g_at_result_iter_next_string(&iter, &str) == TRUE)
			c = strtol(str, NULL, 16);
		else
			goto out;
	}

	g_at_result_iter_next_number(&iter, &t);

out:
	if (status)
		*status = s;

	if (lac)
		*lac = l;

	if (ci)
		*ci = c;

	if (tech)
		*tech = t;

	return TRUE;
}

gboolean at_util_parse_reg(GAtResult *result, const char *prefix,
				int *mode, int *status,
				int *lac, int *ci, int *tech,
				unsigned int vendor)
{
	GAtResultIter iter;
	int m, s;
	int l = -1, c = -1, t = -1;
	const char *str;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, prefix)) {
		gboolean r;

		g_at_result_iter_next_number(&iter, &m);

		/* Sometimes we get an unsolicited CREG/CGREG here, skip it */
		switch (vendor) {
		case OFONO_VENDOR_ZTE:
		case OFONO_VENDOR_HUAWEI:
		case OFONO_VENDOR_NOVATEL:
		case OFONO_VENDOR_SPEEDUP:
			r = g_at_result_iter_next_unquoted_string(&iter, &str);

			if (r == FALSE || strlen(str) != 1)
				continue;

			s = strtol(str, NULL, 10);

			break;
		default:
			if (g_at_result_iter_next_number(&iter, &s) == FALSE)
				continue;

			break;
		}

		/* Some firmware will report bogus lac/ci when unregistered */
		if (s != 1 && s != 5)
			goto out;

		switch (vendor) {
		case OFONO_VENDOR_GOBI:
		case OFONO_VENDOR_ZTE:
		case OFONO_VENDOR_HUAWEI:
		case OFONO_VENDOR_NOVATEL:
		case OFONO_VENDOR_SPEEDUP:
			r = g_at_result_iter_next_unquoted_string(&iter, &str);

			if (r == TRUE)
				l = strtol(str, NULL, 16);
			else
				goto out;

			r = g_at_result_iter_next_unquoted_string(&iter, &str);

			if (r == TRUE)
				c = strtol(str, NULL, 16);
			else
				goto out;

			break;
		default:
			if (g_at_result_iter_next_string(&iter, &str) == TRUE)
				l = strtol(str, NULL, 16);
			else
				goto out;

			if (g_at_result_iter_next_string(&iter, &str) == TRUE)
				c = strtol(str, NULL, 16);
			else
				goto out;
		}

		g_at_result_iter_next_number(&iter, &t);

out:
		if (mode)
			*mode = m;

		if (status)
			*status = s;

		if (lac)
			*lac = l;

		if (ci)
			*ci = c;

		if (tech)
			*tech = t;

		return TRUE;
	}

	return FALSE;
}

gboolean at_util_parse_sms_index_delivery(GAtResult *result, const char *prefix,
						enum at_util_sms_store *out_st,
						int *out_index)
{
	GAtResultIter iter;
	const char *strstore;
	enum at_util_sms_store st;
	int index;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, prefix))
		return FALSE;

	if (!g_at_result_iter_next_string(&iter, &strstore))
		return FALSE;

	if (g_str_equal(strstore, "ME"))
		st = AT_UTIL_SMS_STORE_ME;
	else if (g_str_equal(strstore, "SM"))
		st = AT_UTIL_SMS_STORE_SM;
	else if (g_str_equal(strstore, "SR"))
		st = AT_UTIL_SMS_STORE_SR;
	else if (g_str_equal(strstore, "BM"))
		st = AT_UTIL_SMS_STORE_BM;
	else
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, &index))
		return FALSE;

	if (out_index)
		*out_index = index;

	if (out_st)
		*out_st = st;

	return TRUE;
}

static gboolean at_util_charset_string_to_charset(const char *str,
					enum at_util_charset *charset)
{
	if (!g_strcmp0(str, "GSM"))
		*charset = AT_UTIL_CHARSET_GSM;
	else if (!g_strcmp0(str, "HEX"))
		*charset = AT_UTIL_CHARSET_HEX;
	else if (!g_strcmp0(str, "IRA"))
		*charset = AT_UTIL_CHARSET_IRA;
	else if (!g_strcmp0(str, "PCCP437"))
		*charset = AT_UTIL_CHARSET_PCCP437;
	else if (!g_strcmp0(str, "PCDN"))
		*charset = AT_UTIL_CHARSET_PCDN;
	else if (!g_strcmp0(str, "UCS2"))
		*charset = AT_UTIL_CHARSET_UCS2;
	else if (!g_strcmp0(str, "UTF-8"))
		*charset = AT_UTIL_CHARSET_UTF8;
	else if (!g_strcmp0(str, "8859-1"))
		*charset = AT_UTIL_CHARSET_8859_1;
	else if (!g_strcmp0(str, "8859-2"))
		*charset = AT_UTIL_CHARSET_8859_2;
	else if (!g_strcmp0(str, "8859-3"))
		*charset = AT_UTIL_CHARSET_8859_3;
	else if (!g_strcmp0(str, "8859-4"))
		*charset = AT_UTIL_CHARSET_8859_4;
	else if (!g_strcmp0(str, "8859-5"))
		*charset = AT_UTIL_CHARSET_8859_5;
	else if (!g_strcmp0(str, "8859-6"))
		*charset = AT_UTIL_CHARSET_8859_6;
	else if (!g_strcmp0(str, "8859-C"))
		*charset = AT_UTIL_CHARSET_8859_C;
	else if (!g_strcmp0(str, "8859-A"))
		*charset = AT_UTIL_CHARSET_8859_A;
	else if (!g_strcmp0(str, "8859-G"))
		*charset = AT_UTIL_CHARSET_8859_G;
	else if (!g_strcmp0(str, "8859-H"))
		*charset = AT_UTIL_CHARSET_8859_H;
	else
		return FALSE;

	return TRUE;
}

gboolean at_util_parse_cscs_supported(GAtResult *result, int *supported)
{
	GAtResultIter iter;
	const char *str;
	enum at_util_charset charset;
	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		return FALSE;

	/* Some modems don't report CSCS in a proper list */
	g_at_result_iter_open_list(&iter);

	while (g_at_result_iter_next_string(&iter, &str)) {
		if (at_util_charset_string_to_charset(str, &charset))
			*supported |= charset;
	}

	g_at_result_iter_close_list(&iter);

	return TRUE;
}

gboolean at_util_parse_cscs_query(GAtResult *result,
				enum at_util_charset *charset)
{
	GAtResultIter iter;
	const char *str;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		return FALSE;

	if (g_at_result_iter_next_string(&iter, &str))
		return at_util_charset_string_to_charset(str, charset);

	return FALSE;
}

static const char *at_util_fixup_return(const char *line, const char *prefix)
{
	if (g_str_has_prefix(line, prefix) == FALSE)
		return line;

	line += strlen(prefix);

	while (line[0] == ' ')
		line++;

	return line;
}

gboolean at_util_parse_attr(GAtResult *result, const char *prefix,
				const char **out_attr)
{
	int numlines = g_at_result_num_response_lines(result);
	GAtResultIter iter;
	const char *line;
	int i;

	if (numlines == 0)
		return FALSE;

	g_at_result_iter_init(&iter, result);

	/*
	 * We have to be careful here, sometimes a stray unsolicited
	 * notification will appear as part of the response and we
	 * cannot rely on having a prefix to recognize the actual
	 * response line.  So use the last line only as the response
	 */
	for (i = 0; i < numlines; i++)
		g_at_result_iter_next(&iter, NULL);

	line = g_at_result_iter_raw_line(&iter);

	if (out_attr)
		*out_attr = at_util_fixup_return(line, prefix);

	return TRUE;
}

static void cpin_check_cb(gboolean ok, GAtResult *result, gpointer userdata)
{
	struct at_util_sim_state_query *req = userdata;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (error.type == OFONO_ERROR_TYPE_NO_ERROR)
		goto done;

	/*
	 * If we got a generic error the AT port might not be ready,
	 * try again
	 */
	if (error.type == OFONO_ERROR_TYPE_FAILURE)
		goto tryagain;

	/* If we got any other error besides CME, fail */
	if (error.type != OFONO_ERROR_TYPE_CME)
		goto done;

	switch (error.error) {
	case 10:
	case 13:
		goto done;

	case 14:
		goto tryagain;

	default:
		/* Assume SIM is present */
		ok = TRUE;
		goto done;
	}

tryagain:
	if (req->cpin_poll_count++ < req->num_times) {
		req->cpin_poll_source = g_timeout_add_seconds(req->interval,
								cpin_check,
								req);
		return;
	}

done:
	if (req->cb)
		req->cb(ok, req->userdata);
}

static gboolean cpin_check(gpointer userdata)
{
	struct at_util_sim_state_query *req = userdata;

	req->cpin_poll_source = 0;

	g_at_chat_send(req->chat, "AT+CPIN?", cpin_prefix,
			cpin_check_cb, req, NULL);

	return FALSE;
}

struct at_util_sim_state_query *at_util_sim_state_query_new(GAtChat *chat,
						guint interval, guint num_times,
						at_util_sim_inserted_cb_t cb,
						void *userdata,
						GDestroyNotify destroy)
{
	struct at_util_sim_state_query *req;

	req = g_new0(struct at_util_sim_state_query, 1);

	req->chat = chat;
	req->interval = interval;
	req->num_times = num_times;
	req->cb = cb;
	req->userdata = userdata;
	req->destroy = destroy;

	cpin_check(req);

	return req;
}

void at_util_sim_state_query_free(struct at_util_sim_state_query *req)
{
	if (req == NULL)
		return;

	if (req->cpin_poll_source > 0)
		g_source_remove(req->cpin_poll_source);

	if (req->destroy)
		req->destroy(req->userdata);

	g_free(req);
}

/*
 * CGCONTRDP returns addr + netmask in the same string in the form
 * of "a.b.c.d.m.m.m.m" for IPv4.
 * address/netmask must be able to hold
 * 255.255.255.255 + null = 16 characters
 */
int at_util_get_ipv4_address_and_netmask(const char *addrnetmask,
						char *address, char *netmask)
{
	const char *s = addrnetmask;
	const char *net = NULL;

	int ret = -EINVAL;
	int i;

	/* Count 7 dots for ipv4, less or more means error. */
	for (i = 0; i < 9; i++, s++) {
		s = strchr(s, '.');

		if (!s)
			break;

		if (i == 3) {
			/* set netmask ptr and break the string */
			net = s + 1;
		}
	}

	if (i == 7) {
		memcpy(address, addrnetmask, net - addrnetmask);
		address[net - addrnetmask - 1] = '\0';
		strcpy(netmask, net);

		ret = 0;
	}

	return ret;
}

/*
 * CGCONTRDP returns addr + netmask in the same string in the form
 * of "a1.a2.a3.a4.a5.a6.a7.a8.a9.a10.a11.a12.a13.a14.a15.a16.m1.m2.
 * m3.m4.m5.m6.m7.m8.m9.m10.m11.m12.m13.m14.m15.m16" for IPv6.
 * address/netmask must be able to hold 64 characters.
 */
int at_util_get_ipv6_address_and_netmask(const char *addrnetmask,
						char *address, char *netmask)
{
	const char *s = addrnetmask;
	const char *net = NULL;

	int ret = -EINVAL;
	int i;

	/* Count 31 dots for ipv6, less or more means error. */
	for (i = 0; i < 33; i++, s++) {
		s = strchr(s, '.');

		if (!s)
			break;

		if (i == 15) {
			/* set netmask ptr and break the string */
			net = s + 1;
		}
	}

	if (i == 31) {
		memcpy(address, addrnetmask, net - addrnetmask);
		address[net - addrnetmask - 1] = '\0';
		strcpy(netmask, net);

		ret = 0;
	}

	return ret;
}

int at_util_gprs_auth_method_to_auth_prot(
					enum ofono_gprs_auth_method auth_method)
{
	switch (auth_method) {
	case OFONO_GPRS_AUTH_METHOD_PAP:
		return 1;
	case OFONO_GPRS_AUTH_METHOD_CHAP:
		return 2;
	case OFONO_GPRS_AUTH_METHOD_NONE:
		return 0;
	}

	return 0;
}

const char *at_util_gprs_proto_to_pdp_type(enum ofono_gprs_proto proto)
{
	switch (proto) {
	case OFONO_GPRS_PROTO_IPV6:
		return "IPV6";
	case OFONO_GPRS_PROTO_IPV4V6:
		return "IPV4V6";
		break;
	case OFONO_GPRS_PROTO_IP:
		return "IP";
	}

	return NULL;
}

char *at_util_get_cgdcont_command(guint cid, enum ofono_gprs_proto proto,
								const char *apn)
{
	const char *pdp_type = at_util_gprs_proto_to_pdp_type(proto);

	if (!apn)
		return g_strdup_printf("AT+CGDCONT=%u", cid);

	return g_strdup_printf("AT+CGDCONT=%u,\"%s\",\"%s\"", cid, pdp_type,
									apn);
}

GAtChat *at_util_open_device(struct ofono_modem *modem, const char *key,
				GAtDebugFunc debug_func, char *debug_prefix,
				char *tty_option, ...)
{
	const char *device;
	va_list args;
	GIOChannel *channel;
	GAtSyntax *syntax;
	GAtChat *chat;
	GHashTable *options = NULL;

	device = ofono_modem_get_string(modem, key);
	if (device == NULL)
		return NULL;

	if (tty_option) {
		options = g_hash_table_new(g_str_hash, g_str_equal);
		if (options == NULL)
			return NULL;

		va_start(args, tty_option);
		while (tty_option) {
			gpointer value = (gpointer) va_arg(args, const char *);

			g_hash_table_insert(options, tty_option, value);
			tty_option = (gpointer) va_arg(args, const char *);
		}
		va_end(args);
	}

	channel = g_at_tty_open(device, options);

	if (options)
		g_hash_table_destroy(options);

	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, debug_func, debug_prefix);

	return chat;
}
