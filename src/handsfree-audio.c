/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
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

#include <errno.h>
#include <stdio.h>

#include <gdbus.h>

#include "ofono.h"

#define HFP_AUDIO_MANAGER_INTERFACE		OFONO_SERVICE ".HandsfreeAudioManager"

static DBusMessage *am_get_cards(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	return __ofono_error_not_implemented(msg);
}

static DBusMessage *am_agent_register(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	return __ofono_error_not_implemented(msg);
}

static DBusMessage *am_agent_unregister(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	return __ofono_error_not_implemented(msg);
}

static const GDBusMethodTable am_methods[] = {
	{ GDBUS_METHOD("GetCards",
			NULL, GDBUS_ARGS({"cards", "a{oa{sv}}"}),
			am_get_cards) } ,
	{ GDBUS_METHOD("Register",
			GDBUS_ARGS({"path", "o"}, {"codecs", "ay"}), NULL,
			am_agent_register) },
	{ GDBUS_METHOD("Unregister",
			GDBUS_ARGS({"path", "o"}), NULL,
			am_agent_unregister) },
	{ }
};

int __ofono_handsfree_audio_manager_init(void)
{
	if (!g_dbus_register_interface(ofono_dbus_get_connection(),
					"/", HFP_AUDIO_MANAGER_INTERFACE,
					am_methods, NULL, NULL, NULL, NULL)) {
		return -EIO;
	}

	return 0;
}

void __ofono_handsfree_audio_manager_cleanup(void)
{
	g_dbus_unregister_interface(ofono_dbus_get_connection(), "/",
						HFP_AUDIO_MANAGER_INTERFACE);
}