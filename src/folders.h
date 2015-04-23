/**
 * @file folders.h
 * @brief
 *
 * @date Apr 20, 2015
 * @author nicolas.carrier@parrot.com
 * @copyright Copyright (C) 2015 Parrot S.A.
 */
#ifndef FOLDERS_H_
#define FOLDERS_H_
#include <stdbool.h>

#include <rs_dll.h>

#include "config.h"

#define FOLDERS_CONSTRUCTOR_PRIORITY (CONFIG_CONSTRUCTOR_PRIORITY + 1)

/*
 * all the fields of the struct folder_entity are handled by the folders
 * module
 */
struct folder_entity {
	struct rs_node node;
	struct folder *folder;
	char *name;
};

struct folder;

struct folder_entity_ops {
	const char *(*sha1)(struct folder_entity *entity);
	bool (*can_drop)(struct folder_entity *entity);
	int (*drop)(struct folder_entity *entity, bool only_unregister);
	char *(*get_info)(const struct folder_entity *entity);
};

struct folder {
	struct rs_dll entities;
	char *name;
	struct folder_entity_ops ops;
};

int folder_register(const struct folder *folder);
struct folder *folder_find(const char *folder_name);
struct folder_entity *folder_next(const struct folder *folder,
		struct folder_entity *entity);
unsigned folder_get_count(const char *folder);
int folder_drop(const char *folder, struct folder_entity *entity);
int folder_store(const char *folder, struct folder_entity *entity);

char *folder_get_info(const char *folder, const char *entity_identifier);
struct folder_entity *folder_find_entity(const char *folder,
		const char *entity_identifier);
const char *folders_list(void);
const char *folder_entity_get_sha1(struct folder_entity *entity);
const char *folder_entity_get_name(const struct folder_entity *entity);
int folder_unregister(const char *folder);

#endif /* FOLDERS_H_ */
