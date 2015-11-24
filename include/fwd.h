/**
 * @file fwd.h
 * @brief
 *
 * @date 24 nov. 2015
 * @author ncarrier
 * @copyright Copyright (C) 2015 Parrot S.A.
 */

#ifndef INCLUDE_FWD_H_
#define INCLUDE_FWD_H_
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* values must stay consecutive */
enum fwd_message {
	/* commands, i.e. from client to server */
	FWD_MESSAGE_FIRST,
	FWD_COMMAND_FIRST = FWD_MESSAGE_FIRST,

	FWD_COMMAND_COMMANDS = FWD_COMMAND_FIRST,
	FWD_COMMAND_CONFIG_KEYS,
	FWD_COMMAND_DROP,
	FWD_COMMAND_FOLDERS,
	FWD_COMMAND_GET_CONFIG,
	FWD_COMMAND_GET_PROPERTY,
	FWD_COMMAND_HELP,
	FWD_COMMAND_KILL,
	FWD_COMMAND_LIST,
	FWD_COMMAND_PING,
	FWD_COMMAND_PREPARE,
	FWD_COMMAND_PROPERTIES,
	FWD_COMMAND_QUIT,
	FWD_COMMAND_REMOUNT,
	FWD_COMMAND_SET_PROPERTY,
	FWD_COMMAND_SHOW,
	FWD_COMMAND_START,
	FWD_COMMAND_VERSION,

	FWD_COMMAND_LAST = FWD_COMMAND_VERSION,

	/* answers, i.e. from server to client */
	FWD_ANSWER_FIRST,

	/* acks */
	FWD_ANSWER_COMMANDS = FWD_ANSWER_FIRST,
	FWD_ANSWER_CONFIG_KEYS,
	FWD_ANSWER_ERROR,
	FWD_ANSWER_FOLDERS,
	FWD_ANSWER_GET_CONFIG,
	FWD_ANSWER_GET_PROPERTY,
	FWD_ANSWER_HELP,
	FWD_ANSWER_LIST,
	FWD_ANSWER_PONG,
	FWD_ANSWER_PROPERTIES,
	FWD_ANSWER_PROPERTY_SET,
	FWD_ANSWER_REMOUNTED,
	FWD_ANSWER_SHOW,
	FWD_ANSWER_VERSION,

	/* notifications */
	FWD_ANSWER_BYEBYE,
	FWD_ANSWER_DEAD,
	FWD_ANSWER_DROPPED,
	FWD_ANSWER_PREPARED,
	FWD_ANSWER_PREPARE_PROGRESS,
	FWD_ANSWER_STARTED,

	FWD_ANSWER_LAST = FWD_ANSWER_STARTED,
	FWD_MESSAGE_LAST = FWD_ANSWER_LAST,
	FWD_MESSAGE_INVALID = FWD_MESSAGE_LAST + 1,
};

const char *fwd_message_str(enum fwd_message message);

enum fwd_message fwd_message_from_str(const char *str);

bool fwd_message_is_invalid(enum fwd_message message);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_FWD_H_ */
