/**
 * Copyright (c) 2010-2015 William Light <wrl@illest.net>
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

#define _DEFAULT_SOURCE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <uv.h>
#include <monome.h>

#include <serialosc/serialosc.h>

static void
print_version(void)
{
	printf("serialosc %s (%s)\n", VERSION, GIT_COMMIT);
}

int
main(int argc, char **argv)
{
	monome_t *device;

	if (argc < 2)
		return EXIT_FAILURE;

	if (argv[1][0] == '-') {
		switch (argv[1][1]) {
		case 'v':
			print_version();
			return EXIT_SUCCESS;
		}
	}

	argv[0][strlen(argv[0]) - 1] = ' ';

	if (!(device = monome_open(argv[1])))
		return EXIT_FAILURE;

#ifndef WIN32
	setenv("AVAHI_COMPAT_NOWARN", "shut up", 1);
#endif

	sosc_zeroconf_init();
	sosc_server_run(device);
	monome_close(device);

	return EXIT_SUCCESS;
}
