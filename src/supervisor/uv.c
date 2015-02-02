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

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#include <uv.h>
#include <wwrl/vector_stdlib.h>

#include "serialosc.h"
#include "ipc.h"
#include "osc.h"

#define SELF_FROM(p, member) struct sosc_supervisor *self = container_of(p,	\
		struct sosc_supervisor, member)

#define DEV_FROM(p, member) struct sosc_device_subprocess *dev = \
	container_of(p, struct sosc_device_subprocess, member);

/*************************************************************************
 * datastructures and utilities
 *************************************************************************/

/* we set the `data` member (which is a void*) of the libuv
 * sosc_subprocess.proc structure to either of these values to indicate
 * what kind of subprocess it is. this is so that we can send responses to
 * a /serialosc/list by doing a uv_walk() on our run loop. */
int detector_type = 0;
int device_type = 0;

struct sosc_subprocess {
	uv_process_t proc;
	int pipe_fd;
	uv_poll_t poll;
};

struct sosc_notification_endpoint {
	char host[128];
	char port[6];
};

struct sosc_supervisor {
	uv_loop_t *loop;

	struct sosc_subprocess detector;

	struct {
		lo_server *server;
		uv_poll_t poll;
	} osc;

	VECTOR(sosc_notifications, struct sosc_notification_endpoint)
		notifications;

	int notifications_were_sent;
	uv_check_t drain_notifications;
};

struct sosc_device_subprocess {
	struct sosc_subprocess subprocess;
	struct sosc_supervisor *supervisor;

	int ready;
	int port;

	char *serial;
	char *friendly;
};

static int
launch_subprocess(struct sosc_supervisor *self, struct sosc_subprocess *proc,
		uv_exit_cb exit_cb, char *arg)
{
	struct uv_process_options_s options;
	char path_buf[1024];
	int pipefds[2], err;
	size_t len;

	len = sizeof(path_buf);
	err = uv_exepath(path_buf, &len);
	if (err < 0) {
		fprintf(stderr, "launch_subprocess() failed in uv_exepath(): %s\n",
				uv_strerror(err));

		goto err_exepath;
	}

	err = pipe(pipefds);
	if (err < 0) {
		perror("launch_subprocess() failed in pipe()");
		goto err_pipe;
	}

	options = (struct uv_process_options_s) {
		.exit_cb = exit_cb,

		.file    = path_buf,
		.args    = (char *[]) {path_buf, arg, NULL},
		.flags   = UV_PROCESS_WINDOWS_HIDE,

		.stdio_count = 2,
		.stdio = (struct uv_stdio_container_s []) {
			[STDIN_FILENO] = {
				.flags = UV_IGNORE
			},

			[STDOUT_FILENO] = {
				.flags = UV_INHERIT_FD,
				.data.fd = pipefds[1]
			}
		},
	};

	err = uv_spawn(self->loop, &proc->proc, &options);
	close(pipefds[1]);

	if (err < 0) {
		fprintf(stderr, "launch_subprocess() failed in uv_spawn(): %s\n",
				uv_strerror(err));
		goto err_spawn;
	}

	proc->pipe_fd = pipefds[0];
	uv_poll_init(self->loop, &proc->poll, proc->pipe_fd);
	return 0;

err_spawn:
	close(pipefds[0]);
err_pipe:
err_exepath:
	return err;
}

/*************************************************************************
 * osc
 *************************************************************************/

static int
portstr(char *dest, int src)
{
	return snprintf(dest, 6, "%d", src);
}

struct walk_cb_args {
	struct sosc_supervisor *self;
	lo_address *dst;
};

static void
list_devices_walk_cb(uv_handle_t *handle, void *_args)
{
	struct walk_cb_args *args = _args;
	struct sosc_supervisor *self = args->self;
	lo_address *dst = args->dst;
	struct sosc_device_subprocess *dev;

	if (handle->data != &device_type)
		return;

	dev = container_of(handle, struct sosc_device_subprocess, subprocess.proc);

	lo_send_from(dst, self->osc.server, LO_TT_IMMEDIATE, "/serialosc/device",
			"ssi", dev->serial, dev->friendly, dev->port);
}

OSC_HANDLER_FUNC(list_devices)
{
	struct sosc_supervisor *self = user_data;
	struct walk_cb_args args;
	char port[6];

	portstr(port, argv[1]->i);

	args.self = self;
	args.dst  = lo_address_new(&argv[0]->s, port);

	if (!args.dst)
		return 1;

	uv_walk(self->loop, list_devices_walk_cb, &args);

	lo_address_free(args.dst);
	return 0;
}

OSC_HANDLER_FUNC(add_notification_endpoint)
{
	struct sosc_supervisor *self = user_data;
	struct sosc_notification_endpoint n;

	portstr(n.port, argv[1]->i);
	sosc_strlcpy(n.host, &argv[0]->s, sizeof(n.host));

	VECTOR_PUSH_BACK(&self->notifications, n);
	return 0;
}

static void
osc_poll_cb(uv_poll_t *handle, int status, int events)
{
	SELF_FROM(handle, osc.poll);
	lo_server_recv_noblock(self->osc.server, 0);
}

static int
init_osc_server(struct sosc_supervisor *self)
{
	if (!(self->osc.server = lo_server_new(SOSC_SUPERVISOR_OSC_PORT, NULL)))
		return -1;

	lo_server_add_method(self->osc.server,
			"/serialosc/list", "si", list_devices, self);
	lo_server_add_method(self->osc.server,
			"/serialosc/notify", "si", add_notification_endpoint, self);

	uv_poll_init(self->loop, &self->osc.poll,
			lo_server_get_socket_fd(self->osc.server));

	return 0;
}

static int
osc_notify(struct sosc_supervisor *self, struct sosc_device_subprocess *dev,
		sosc_ipc_type_t type)
{
	struct sosc_notification_endpoint *n;
	const char *path;
	lo_address dst;
	int i;

	switch (type) {
	case SOSC_DEVICE_CONNECTION:
		path = "/serialosc/add";
		break;

	case SOSC_DEVICE_DISCONNECTION:
		path = "/serialosc/remove";
		break;

	default:
		return -1;
	}

	for (i = 0; i < self->notifications.size; i++) {
		n = &self->notifications.data[i];

		if (!(dst = lo_address_new(n->host, n->port))) {
			fprintf(stderr, "notify(): couldn't allocate lo_address\n");
			continue;
		}

		lo_send_from(dst, self->osc.server, LO_TT_IMMEDIATE, path, "ssi",
		             dev->serial, dev->friendly, dev->port);

		lo_address_free(dst);
	}

	self->notifications_were_sent = 1;
	return 0;
}

/*************************************************************************
 * device lifecycle
 *************************************************************************/

static void device_exit_cb(uv_process_t *, int64_t, int);

static int
device_init(struct sosc_supervisor *self, struct sosc_device_subprocess *dev,
		char *devnode)
{
	if (launch_subprocess(self, &dev->subprocess, device_exit_cb, devnode))
		return -1;

	dev->supervisor = self;
	dev->subprocess.proc.data = &device_type;
	return 0;
}

static void
device_fini(struct sosc_device_subprocess *dev)
{
	s_free(dev->serial);
	s_free(dev->friendly);
}

static void
device_poll_close_cb(uv_handle_t *handle)
{
	DEV_FROM(handle, subprocess.poll);

	close(dev->subprocess.pipe_fd);
	device_fini(dev);
	free(dev);
}

static void
device_exit_cb(uv_process_t *process, int64_t exit_status, int term_signal)
{
	DEV_FROM(process, subprocess.proc);

	osc_notify(dev->supervisor, dev, SOSC_DEVICE_DISCONNECTION);

	uv_close((void *) &dev->subprocess.proc, NULL);
	uv_close((void *) &dev->subprocess.poll, device_poll_close_cb);
}

/*************************************************************************
 * device communication
 *************************************************************************/

static int
handle_device_msg(struct sosc_supervisor *self,
		struct sosc_device_subprocess *dev, struct sosc_ipc_msg *msg)
{
	switch (msg->type) {
	case SOSC_DEVICE_CONNECTION:
		return -1;

	case SOSC_OSC_PORT_CHANGE:
		dev->port = msg->port_change.port;
		return 0;

	case SOSC_DEVICE_INFO:
		dev->serial   = msg->device_info.serial;
		dev->friendly = msg->device_info.friendly;
		return 0;

	case SOSC_DEVICE_READY:
		dev->ready = 1;
		osc_notify(self, dev, SOSC_DEVICE_CONNECTION);
		return 0;

	case SOSC_DEVICE_DISCONNECTION:
		/* XXX: may not actually need this, can we just use exit_cb()? */
		return 0;
	}

	return 0;
}

/*************************************************************************
 * detector communication
 *************************************************************************/

static void device_pipe_cb(uv_poll_t *, int status, int events);

static int
handle_connection(struct sosc_supervisor *self, struct sosc_ipc_msg *msg)
{
	struct sosc_device_subprocess *dev;

	if (!(dev = calloc(1, sizeof(*dev))))
		goto err_calloc;

	if (device_init(self, dev, msg->connection.devnode))
		goto err_init;

	uv_poll_start(&dev->subprocess.poll, UV_READABLE, device_pipe_cb);
	return 0;

err_init:
	free(dev);
err_calloc:
	return -1;
}

static int
handle_detector_msg(struct sosc_supervisor *self, struct sosc_ipc_msg *msg)
{
	switch (msg->type) {
	case SOSC_DEVICE_CONNECTION:
		return handle_connection(self, msg);

	case SOSC_OSC_PORT_CHANGE:
	case SOSC_DEVICE_INFO:
	case SOSC_DEVICE_READY:
	case SOSC_DEVICE_DISCONNECTION:
		return -1;
	}

	return 0;
}

/*************************************************************************
 * subprocess callbacks
 *************************************************************************/

static void
device_pipe_cb(uv_poll_t *handle, int status, int events)
{
	DEV_FROM(handle, subprocess.poll);
	struct sosc_ipc_msg msg;

	if (sosc_ipc_msg_read(dev->subprocess.pipe_fd, &msg) > 0)
		handle_device_msg(dev->supervisor, dev, &msg);
}

static void
detector_pipe_cb(uv_poll_t *handle, int status, int events)
{
	SELF_FROM(handle, detector.poll);
	struct sosc_ipc_msg msg;

	if (sosc_ipc_msg_read(self->detector.pipe_fd, &msg) > 0)
		handle_detector_msg(self, &msg);
}

/*************************************************************************
 * entry point
 *************************************************************************/

static void
drain_notifications_cb(uv_check_t *handle)
{
	SELF_FROM(handle, drain_notifications);

	if (self->notifications_were_sent) {
		VECTOR_CLEAR(&self->notifications);
		self->notifications_were_sent = 0;
	}
}

int
sosc_supervisor_run(char *progname)
{
	struct sosc_supervisor self = {NULL};

	sosc_config_create_directory();

	self.loop = uv_default_loop();
	VECTOR_INIT(&self.notifications, 32);

	if (init_osc_server(&self))
		goto err_osc_server;

	if (launch_subprocess(&self, &self.detector, NULL, "-d"))
		goto err_detector;

	self.detector.proc.data = &detector_type;

	uv_set_process_title("serialosc [supervisor]");

	uv_poll_start(&self.detector.poll, UV_READABLE, detector_pipe_cb);
	uv_poll_start(&self.osc.poll, UV_READABLE, osc_poll_cb);

	self.notifications_were_sent = 0;
	uv_check_init(self.loop, &self.drain_notifications);
	uv_check_start(&self.drain_notifications, drain_notifications_cb);

	uv_run(self.loop, UV_RUN_DEFAULT);

	VECTOR_FREE(&self.notifications);
	uv_loop_close(self.loop);

	return 0;

err_detector:
	lo_server_free(self.osc.server);
err_osc_server:
	uv_loop_close(self.loop);
	return -1;
}