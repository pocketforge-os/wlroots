/*
 * Protocol-level tests for the input protocols exposed by wlroots:
 * zwp_text_input_manager_v3 / zwp_text_input_v3 and
 * wp_cursor_shape_manager_v1 / wp_cursor_shape_device_v1.
 *
 * Each mode runs an in-process server (this process) and forks a client
 * child that connects over an abstract socket path. The child checks the
 * client-visible protocol behaviour (advertised versions, request
 * round-trips, protocol errors); the parent checks the server-side
 * signals. The child exit status and the parent-side checks together
 * decide the test result.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/util/log.h>
#include "cursor-shape-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		failures++; \
	} \
} while (0)

struct server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_seat *seat;
	struct wlr_text_input_manager_v3 *text_input_manager;
	struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;

	struct wlr_text_input_v3 *text_input;
	size_t text_inputs_created;
	size_t enable_events;
	size_t commit_events;
	uint32_t last_content_hint;

	size_t set_shape_events;
	uint32_t shapes_seen[8];
};

static struct server server;

static struct wl_listener text_input_listener, set_shape_listener;
#define MAX_TEXT_INPUTS 4
static struct {
	struct wl_listener enable;
	struct wl_listener commit;
} ti_listeners[MAX_TEXT_INPUTS];
static size_t ti_listener_slots;

static void handle_text_input_enable(struct wl_listener *listener, void *data) {
	struct wlr_text_input_v3 *text_input = data;
	server.enable_events++;
	server.last_content_hint = text_input->current.content_type.hint;
}

static void handle_text_input_commit(struct wl_listener *listener, void *data) {
	struct wlr_text_input_v3 *text_input = data;
	server.commit_events++;
	server.last_content_hint = text_input->current.content_type.hint;
}

static void handle_manager_text_input(struct wl_listener *listener, void *data) {
	struct wlr_text_input_v3 *text_input = data;
	size_t slot = server.text_inputs_created++;
	server.text_input = text_input;

	if (slot >= MAX_TEXT_INPUTS) {
		return;
	}
	ti_listeners[slot].enable.notify = handle_text_input_enable;
	ti_listeners[slot].commit.notify = handle_text_input_commit;
	wl_signal_add(&text_input->events.enable, &ti_listeners[slot].enable);
	wl_signal_add(&text_input->events.commit, &ti_listeners[slot].commit);
	ti_listener_slots = slot + 1;
}

static void server_remove_listeners(void) {
	for (size_t i = 0; i < ti_listener_slots; i++) {
		wl_list_remove(&ti_listeners[i].enable.link);
		wl_list_remove(&ti_listeners[i].commit.link);
	}
	if (server.text_input_manager != NULL) {
		wl_list_remove(&text_input_listener.link);
	}
	if (server.cursor_shape_manager != NULL) {
		wl_list_remove(&set_shape_listener.link);
	}
}

static void handle_request_set_shape(struct wl_listener *listener, void *data) {
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (server.set_shape_events < sizeof(server.shapes_seen) / sizeof(server.shapes_seen[0])) {
		server.shapes_seen[server.set_shape_events] = event->shape;
	}
	server.set_shape_events++;
}

struct client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct wl_pointer *pointer;

	struct zwp_text_input_manager_v3 *text_input_manager;
	uint32_t text_input_manager_version;
	struct zwp_text_input_manager_v3 *text_input_manager_v1;
	struct zwp_text_input_v3 *text_input;
	struct zwp_text_input_v3 *text_input_v1;

	struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
	uint32_t cursor_shape_manager_version;
	struct wp_cursor_shape_device_v1 *cursor_shape_device;
	struct wp_cursor_shape_device_v1 *cursor_shape_device_v1;

	uint32_t text_input_manager_name;
	uint32_t cursor_shape_manager_name;
	uint32_t seat_name;
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct client *client = data;
	if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
		client->text_input_manager_name = name;
		client->text_input_manager_version = version;
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		client->cursor_shape_manager_name = name;
		client->cursor_shape_manager_version = version;
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		client->seat_name = name;
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
		uint32_t capabilities) {
	struct client *client = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && client->pointer == NULL) {
		client->pointer = wl_seat_get_pointer(seat);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void client_connect(struct client *client, const char *socket) {
	memset(client, 0, sizeof(*client));
	client->display = wl_display_connect(socket);
	if (client->display == NULL) {
		fprintf(stderr, "client: failed to connect to %s\n", socket);
		exit(1);
	}
	client->registry = wl_display_get_registry(client->display);
	wl_registry_add_listener(client->registry, &registry_listener, client);
	wl_display_roundtrip(client->display);
	client->seat = wl_registry_bind(client->registry, client->seat_name,
		&wl_seat_interface, 5);
	wl_seat_add_listener(client->seat, &seat_listener, client);
	wl_display_roundtrip(client->display);
}

static int client_text_input(const char *socket) {
	struct client client;
	client_connect(&client, socket);

	if (client.text_input_manager_name == 0) {
		fprintf(stderr, "client: zwp_text_input_manager_v3 not advertised\n");
		return 1;
	}
	// Positive control: the manager global advertises version 2.
	// Negative control (implicit): a v1 client can still bind below.
	CHECK(client.text_input_manager_version == 2,
		"zwp_text_input_manager_v3 advertised version %"PRIu32", expected 2",
		client.text_input_manager_version);
	if (client.text_input_manager_version < 2) {
		return failures != 0;
	}

	client.text_input_manager = wl_registry_bind(client.registry,
		client.text_input_manager_name, &zwp_text_input_manager_v3_interface, 2);
	client.text_input = zwp_text_input_manager_v3_get_text_input(
		client.text_input_manager, client.seat);

	// Existing v1 flow still works: content hints (including the version 2
	// hint flags, which pass through the unvalidated hint bitmask) plus
	// enable and commit round-trips without a protocol error.
	zwp_text_input_v3_set_content_type(client.text_input,
		ZWP_TEXT_INPUT_V3_CONTENT_HINT_ON_SCREEN_INPUT_PROVIDED |
		ZWP_TEXT_INPUT_V3_CONTENT_HINT_NO_EMOJI |
		ZWP_TEXT_INPUT_V3_CONTENT_HINT_PREEDIT_SHOWN,
		ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
	zwp_text_input_v3_enable(client.text_input);
	zwp_text_input_v3_commit(client.text_input);
	wl_display_roundtrip(client.display);
	CHECK(wl_display_get_error(client.display) == 0,
		"v1 enable/commit round-trip failed: %s",
		strerror(wl_display_get_error(client.display)));

	// Version 2 flow: announce an available action and explicitly request
	// the input panel, then commit and hide the panel again. None of the
	// v2 requests may raise a protocol error on a v2-bound text input.
	struct wl_array actions;
	wl_array_init(&actions);
	uint32_t *action = wl_array_add(&actions, sizeof(*action));
	*action = ZWP_TEXT_INPUT_V3_ACTION_SUBMIT;
	zwp_text_input_v3_set_available_actions(client.text_input, &actions);
	wl_array_release(&actions);
	zwp_text_input_v3_show_input_panel(client.text_input);
	zwp_text_input_v3_commit(client.text_input);
	zwp_text_input_v3_hide_input_panel(client.text_input);
	wl_display_roundtrip(client.display);
	CHECK(wl_display_get_error(client.display) == 0,
		"v2 request round-trip failed: %s",
		strerror(wl_display_get_error(client.display)));

	// Negative control: the same v2 request on a v1-bound text input must
	// be rejected with a protocol error.
	client.text_input_manager_v1 = wl_registry_bind(client.registry,
		client.text_input_manager_name, &zwp_text_input_manager_v3_interface, 1);
	client.text_input_v1 = zwp_text_input_manager_v3_get_text_input(
		client.text_input_manager_v1, client.seat);
	zwp_text_input_v3_show_input_panel(client.text_input_v1);
	wl_display_flush(client.display);
	wl_display_roundtrip(client.display);
	CHECK(wl_display_get_error(client.display) != 0,
		"v2 request on a v1-bound text input was not rejected");

	return failures != 0;
}

static int client_cursor_shape(const char *socket) {
	struct client client;
	client_connect(&client, socket);

	if (client.cursor_shape_manager_name == 0) {
		fprintf(stderr, "client: wp_cursor_shape_manager_v1 not advertised\n");
		return 1;
	}
	// Positive control: the manager global advertises version 2.
	CHECK(client.cursor_shape_manager_version == 2,
		"wp_cursor_shape_manager_v1 advertised version %"PRIu32", expected 2",
		client.cursor_shape_manager_version);
	if (client.cursor_shape_manager_version < 2) {
		return failures != 0;
	}

	// Negative control first, on a v1 binding: the classic shapes keep
	// working for v1 clients.
	client.cursor_shape_manager = wl_registry_bind(client.registry,
		client.cursor_shape_manager_name, &wp_cursor_shape_manager_v1_interface, 1);
	client.cursor_shape_device_v1 = wp_cursor_shape_manager_v1_get_pointer(
		client.cursor_shape_manager, client.pointer);
	wp_cursor_shape_device_v1_set_shape(client.cursor_shape_device_v1, 1,
		WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	wl_display_roundtrip(client.display);
	CHECK(wl_display_get_error(client.display) == 0,
		"v1 set_shape round-trip failed: %s",
		strerror(wl_display_get_error(client.display)));

	// Version 2 binding: the new shapes must round-trip.
	struct wp_cursor_shape_manager_v1 *manager_v2 = wl_registry_bind(
		client.registry, client.cursor_shape_manager_name,
		&wp_cursor_shape_manager_v1_interface, 2);
	client.cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
		manager_v2, client.pointer);
	wp_cursor_shape_device_v1_set_shape(client.cursor_shape_device, 2,
		WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK);
	wp_cursor_shape_device_v1_set_shape(client.cursor_shape_device, 3,
		WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE);
	wl_display_roundtrip(client.display);
	CHECK(wl_display_get_error(client.display) == 0,
		"v2 set_shape round-trip failed: %s",
		strerror(wl_display_get_error(client.display)));

	// Negative control: a shape beyond the v2 range is rejected.
	wp_cursor_shape_device_v1_set_shape(client.cursor_shape_device, 4,
		WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE + 1);
	wl_display_flush(client.display);
	wl_display_roundtrip(client.display);
	CHECK(wl_display_get_error(client.display) != 0,
		"out-of-range shape was not rejected");

	return failures != 0;
}

int main(int argc, char *argv[]) {
	if (argc != 2 ||
			(strcmp(argv[1], "text-input") != 0 &&
			 strcmp(argv[1], "cursor-shape") != 0)) {
		fprintf(stderr, "usage: %s text-input|cursor-shape\n", argv[0]);
		return 2;
	}
	bool text_input_mode = strcmp(argv[1], "text-input") == 0;

	wlr_log_init(WLR_ERROR, NULL);

	server.display = wl_display_create();
	assert(server.display != NULL);
	server.loop = wl_display_get_event_loop(server.display);
	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		fprintf(stderr, "server: failed to add a socket (XDG_RUNTIME_DIR?)\n");
		return 2;
	}

	server.seat = wlr_seat_create(server.display, "test-seat");
	assert(server.seat != NULL);
	wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER);

	if (text_input_mode) {
		server.text_input_manager =
			wlr_text_input_manager_v3_create(server.display);
		assert(server.text_input_manager != NULL);
		text_input_listener.notify = handle_manager_text_input;
		wl_signal_add(&server.text_input_manager->events.text_input,
			&text_input_listener);
	} else {
		// Positive control: the v2 shape names resolve. This aborts via
		// the name-table bounds assert while the shapes are missing.
		CHECK(strcmp(wlr_cursor_shape_v1_name(
				WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK), "dnd-ask") == 0,
			"wlr_cursor_shape_v1_name(DND_ASK) is not \"dnd-ask\"");
		CHECK(strcmp(wlr_cursor_shape_v1_name(
				WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE), "all-resize") == 0,
			"wlr_cursor_shape_v1_name(ALL_RESIZE) is not \"all-resize\"");

		server.cursor_shape_manager =
			wlr_cursor_shape_manager_v1_create(server.display, 2);
		assert(server.cursor_shape_manager != NULL);
		set_shape_listener.notify = handle_request_set_shape;
		wl_signal_add(&server.cursor_shape_manager->events.request_set_shape,
			&set_shape_listener);
	}

	wl_display_flush_clients(server.display);

	pid_t pid = fork();
	if (pid == 0) {
		int rc = text_input_mode ?
			client_text_input(socket) : client_cursor_shape(socket);
		fflush(stderr);
		_exit(rc);
	}
	if (pid < 0) {
		fprintf(stderr, "server: fork failed\n");
		return 2;
	}

	int status = 0;
	for (;;) {
		pid_t done = waitpid(pid, &status, WNOHANG);
		if (done == pid) {
			break;
		}
		if (done < 0) {
			fprintf(stderr, "server: waitpid failed\n");
			return 2;
		}
		if (wl_event_loop_dispatch(server.loop, 100) < 0) {
			fprintf(stderr, "server: event loop dispatch failed\n");
			return 2;
		}
		wl_display_flush_clients(server.display);
	}

	if (text_input_mode) {
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			"client exited abnormally (status %d)", status);
		CHECK(server.text_inputs_created == 2,
			"expected 2 text inputs, got %zu", server.text_inputs_created);
		CHECK(server.enable_events == 1,
			"expected 1 enable event, got %zu", server.enable_events);
		CHECK(server.commit_events == 1,
			"expected 1 commit event, got %zu", server.commit_events);
		CHECK(server.last_content_hint ==
				(ZWP_TEXT_INPUT_V3_CONTENT_HINT_ON_SCREEN_INPUT_PROVIDED |
				 ZWP_TEXT_INPUT_V3_CONTENT_HINT_NO_EMOJI |
				 ZWP_TEXT_INPUT_V3_CONTENT_HINT_PREEDIT_SHOWN),
			"version 2 content hints did not pass through (0x%x)",
			server.last_content_hint);
	} else {
		// The client's final negative control kills the connection with
		// an expected protocol error; the client still exits cleanly.
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			"client exited abnormally (status %d)", status);
		CHECK(server.set_shape_events == 3,
			"expected 3 set_shape events, got %zu", server.set_shape_events);
		CHECK(server.set_shape_events >= 3 &&
				server.shapes_seen[0] == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT &&
				server.shapes_seen[1] == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK &&
				server.shapes_seen[2] == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE,
			"unexpected shape sequence");
	}

	server_remove_listeners();
	wl_display_destroy(server.display);
	return failures != 0;
}
