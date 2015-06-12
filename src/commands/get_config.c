/**
 * @file get_config.c
 * @brief
 *
 * @date June 12, 2015
 * @author nicolas.carrier@parrot.com
 * @copyright Copyright (C) 2015 Parrot S.A.
 */
#include <errno.h>
#include <string.h>

#include <ut_string.h>

#include <libpomp.h>

#define ULOG_TAG firmwared_command_get_config
#include <ulog.h>
ULOG_DECLARE_TAG(firmwared_command_get_config);

#include "config.h"
#include "firmwared.h"
#include "commands.h"

#define COMMAND_NAME "GET_CONFIG"

static int get_config_command_handler(struct firmwared *f,
		struct pomp_conn *conn, const struct pomp_msg *msg)
{
	int ret;
	char __attribute__((cleanup(ut_string_free))) *cmd = NULL;
	char __attribute__((cleanup(ut_string_free))) *config_key = NULL;
	enum config_key key;

	ret = pomp_msg_read(msg, "%ms%ms", &cmd, &config_key);
	if (ret < 0) {
		cmd = config_key = NULL;
		ULOGE("pomp_msg_read: %s", strerror(-ret));
		return ret;
	}
	key = config_key_from_string(config_key);
	if (key == (enum config_key)-1)
		return -ESRCH;

	return firmwared_answer(conn, msg, "%s%s%s", COMMAND_NAME, config_key,
			config_get(key));
}

static const struct command get_config_command = {
		.name = COMMAND_NAME,
		.help = "Retrieves the value of the CONFIG_KEY configuration "
				"key.",
		.long_help = "The CONFIG_KEY is case insensitive. Use the "
				"CONFIG_KEYS command to list the available "
				"config keys to query.",
		.synopsis = "CONFIG_KEY",
		.handler = get_config_command_handler,
};

static __attribute__((constructor)) void get_config_init(void)
{
	int ret;

	ULOGD("%s", __func__);

	ret = command_register(&get_config_command);
	if (ret < 0)
		ULOGE("command_register: %s", strerror(-ret));
}

static __attribute__((destructor)) void get_config_cleanup(void)
{
	int ret;

	ULOGD("%s", __func__);

	ret = command_unregister(COMMAND_NAME);
	if (ret < 0)
		ULOGE("command_register: %s", strerror(-ret));
}