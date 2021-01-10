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

#include <gatchat.h>

struct ofono_modem;

enum at_util_sms_store {
	AT_UTIL_SMS_STORE_SM =	0,
	AT_UTIL_SMS_STORE_ME =	1,
	AT_UTIL_SMS_STORE_MT =	2,
	AT_UTIL_SMS_STORE_SR =	3,
	AT_UTIL_SMS_STORE_BM =	4,
};

/* 3GPP TS 27.007 Release 8 Section 5.5 */
enum at_util_charset {
	AT_UTIL_CHARSET_GSM =		0x1,
	AT_UTIL_CHARSET_HEX =		0x2,
	AT_UTIL_CHARSET_IRA =		0x4,
	AT_UTIL_CHARSET_PCCP437 =	0x8,
	AT_UTIL_CHARSET_PCDN =		0x10,
	AT_UTIL_CHARSET_UCS2 =		0x20,
	AT_UTIL_CHARSET_UTF8 =		0x40,
	AT_UTIL_CHARSET_8859_1 =	0x80,
	AT_UTIL_CHARSET_8859_2 =	0x100,
	AT_UTIL_CHARSET_8859_3 =	0x200,
	AT_UTIL_CHARSET_8859_4 =	0x400,
	AT_UTIL_CHARSET_8859_5 =	0x800,
	AT_UTIL_CHARSET_8859_6 =	0x1000,
	AT_UTIL_CHARSET_8859_C =	0x2000,
	AT_UTIL_CHARSET_8859_A =	0x4000,
	AT_UTIL_CHARSET_8859_G =	0x8000,
	AT_UTIL_CHARSET_8859_H =	0x10000,
};

typedef void (*at_util_sim_inserted_cb_t)(gboolean present, void *userdata);

void decode_at_error(struct ofono_error *error, const char *final);
gint at_util_call_compare_by_phone_number(gconstpointer a, gconstpointer b);
gint ofono_call_compare_by_id(gconstpointer a, gconstpointer b);
GSList *at_util_parse_clcc(GAtResult *result, unsigned int *mpty_ids);
gboolean at_util_parse_reg(GAtResult *result, const char *prefix,
				int *mode, int *status,
				int *lac, int *ci, int *tech,
				unsigned int vendor);
gboolean at_util_parse_reg_unsolicited(GAtResult *result, const char *prefix,
					int *status, int *lac,
					int *ci, int *tech,
					unsigned int vendor);

gboolean at_util_parse_sms_index_delivery(GAtResult *result, const char *prefix,
						enum at_util_sms_store *store,
						int *index);

gboolean at_util_parse_cscs_supported(GAtResult *result, int *supported);
gboolean at_util_parse_cscs_query(GAtResult *result,
				enum at_util_charset *charset);

gboolean at_util_parse_attr(GAtResult *result, const char *prefix,
				const char **out_attr);

struct at_util_sim_state_query *at_util_sim_state_query_new(GAtChat *chat,
						guint interval, guint num_times,
						at_util_sim_inserted_cb_t cb,
						void *userdata,
						GDestroyNotify destroy);
void at_util_sim_state_query_free(struct at_util_sim_state_query *req);

int at_util_get_ipv4_address_and_netmask(const char *addrnetmask,
						char *address, char *netmask);
int at_util_get_ipv6_address_and_netmask(const char *addrnetmask,
						char *address, char *netmask);


int at_util_gprs_auth_method_to_auth_prot(
				enum ofono_gprs_auth_method auth_method);

const char *at_util_gprs_proto_to_pdp_type(enum ofono_gprs_proto proto);

/*
 * at_util_get_cgdcont_command
 * if the apn pointer is NULL, the context will be removed: the resulting
 * string will be like: AT+CGDCONT=7
 * but if apn pointer is not NULL and the string is empty, then
 * this function will create a normal context with empty apn, like:
 * AT+CGDCONT=4,"IPV6",""
 */
char *at_util_get_cgdcont_command(guint cid, enum ofono_gprs_proto proto,
							const char *apn);

struct cb_data {
	gint ref_count;
	void *cb;
	void *data;
	void *user;
};

static inline struct cb_data *cb_data_new(void *cb, void *data)
{
	struct cb_data *ret;

	ret = g_new0(struct cb_data, 1);
	ret->ref_count = 1;
	ret->cb = cb;
	ret->data = data;

	return ret;
}

static inline struct cb_data *cb_data_ref(struct cb_data *cbd)
{
	cbd->ref_count++;
	return cbd;
}

static inline void cb_data_unref(gpointer user_data)
{
	struct cb_data *cbd = user_data;

	if (--cbd->ref_count)
		return;

	g_free(cbd);
}

static inline int at_util_convert_signal_strength(int strength)
{
	int result;

	if (strength == 99)
		result = -1;
	else
		result = (strength * 100) / 31;

	return result;
}

#define CALLBACK_WITH_FAILURE(cb, args...)		\
	do {						\
		struct ofono_error cb_e;		\
		cb_e.type = OFONO_ERROR_TYPE_FAILURE;	\
		cb_e.error = 0;				\
							\
		cb(&cb_e, ##args);			\
	} while (0)					\

#define CALLBACK_WITH_SUCCESS(f, args...)		\
	do {						\
		struct ofono_error e;			\
		e.type = OFONO_ERROR_TYPE_NO_ERROR;	\
		e.error = 0;				\
		f(&e, ##args);				\
	} while (0)

GAtChat *at_util_open_device(struct ofono_modem *modem, const char *key,
				GAtDebugFunc debug_func, char *debug_prefix,
				char *tty_option, ...);
