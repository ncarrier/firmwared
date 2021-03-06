/**
 * @file utils.h
 * @brief
 *
 * @date Apr 21, 2015
 * @author nicolas.carrier@parrot.com
 * @copyright Copyright (C) 2015 Parrot S.A.
 */
#ifndef UTILS_H_
#define UTILS_H_
#include <stddef.h>
#include <stdbool.h>

char *buffer_to_string(const unsigned char *src, size_t len, char *dst);
char *get_argz_i(const char *argz, size_t argz_len, int i);
int argz_property_geti(const char *argz, size_t argz_len, unsigned index,
		char **value);
int argz_property_seti(char **argz, size_t *argz_len, unsigned index,
		const char *value);

#endif /* UTILS_H_ */
