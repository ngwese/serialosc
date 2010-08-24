/**
 * Copyright (c) 2010 William Light <will@illest.net>
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>

#include <lo/lo.h>
#include <monome.h>

#include "serialosc.h"
#include "osc.h"


static int sys_mode_handler(const char *path, const char *types,
                            lo_arg **argv, int argc,
                            lo_message data, void *user_data) {
	monome_t *monome = user_data;

	return monome_mode(monome, argv[0]->i);
}

static int sys_info_handler(const char *path, const char *types,
                            lo_arg **argv, int argc,
                            lo_message data, void *user_data) {
	sosc_state_t *state = user_data;

	lo_address *dst;
	char *host, port[6];

	host = &argv[0]->s;
	snprintf(port, 6, "%d", argv[1]->i);

	if( !(dst = lo_address_new(host, port)) ) {
		fprintf(stderr, "could not allocate lo_address");
		return 1;
	}

	lo_send_from(dst, state->server, LO_TT_IMMEDIATE, "/sys/info", "siis",
	             monome_get_serial(state->monome),
	             monome_get_rows(state->monome),
	             monome_get_cols(state->monome),
	             state->osc_prefix);

	lo_address_free(dst);

	return 0;
}

void osc_register_sys_methods(sosc_state_t *state) {
#define SYS_METHOD(method, types, context) \
	lo_server_add_method(state->server, "/sys/" #method, types, \
	                     sys_##method##_handler, context)

	SYS_METHOD(mode, "i", state->monome);
	SYS_METHOD(info, "si", state);

#undef SYS_METHOD
}