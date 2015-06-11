/**
 * @file firmwared.h
 * @brief
 *
 * @date Apr 17, 2015
 * @author nicolas.carrier@parrot.com
 * @copyright Copyright (C) 2015 Parrot S.A.
 */
#ifndef FIRMWARED_H_
#define FIRMWARED_H_

#include <stdbool.h>

#include <libpomp.h>

#include <io_mon.h>
#include <io_src.h>
#include <io_src_sig.h>

#ifndef FIRMWARED_GROUP
#define FIRMWARED_GROUP "firmwared"
#endif /* FIRMWARED_GROUP */

struct firmwared {
	struct io_mon mon;
	struct io_src pomp_src;
	struct pomp_ctx *pomp;
	struct io_src_sig sig_src;
	bool loop;
};

int firmwared_init(struct firmwared *f);
void firmwared_run(struct firmwared *f);
void firmwared_stop(struct firmwared *f);
int firmwared_notify(struct firmwared *f, uint32_t msgid, const char *fmt, ...);
#define firmwared_answer(c, m, f, ...) pomp_conn_send(c, pomp_msg_get_id(m), \
		f, __VA_ARGS__)
#define firmwared_get_mon(f) ((f) == NULL ? NULL : &((f)->mon))

void firmwared_clean(struct firmwared *f);

#endif /* FIRMWARED_H_ */
