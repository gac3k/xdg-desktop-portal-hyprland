#include "config.h"
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <inttypes.h>

#if HAVE_LIBXKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif

#include "util/util-color.h"
#include "util/util-mem.h"
#include "util/util-memfile.h"
#include "util/util-strings.h"
#include "util/util-time.h"

#include "main.h"
#include "libei-1.0/libeis.h"

#include "wayland-client.h"
#include "../protocols/virtual-keyboard-unstable-v1-protocol.h"
#include "../protocols/wlr-virtual-pointer-unstable-v1-protocol.h"
static inline void
_printf_(1, 2)
colorprint(const char *format, ...)
{
    static uint64_t color = 0;
    run_only_once {
        color = rgb(1, 1, 1) | rgb_bg(255, 127, 0);
    }

    cprintf(color, "EIS socket server: ");
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

void handleGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    const std::string INTERFACE = interface;

    if(INTERFACE == wl_seat_interface.name) {
        m_wayland_connection.seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, version);
        //wl_seat_add_listener(m_sWaylandConnection.seat, &seat_listener, data);
    }

    else if(INTERFACE == zwlr_virtual_pointer_manager_v1_interface.name)
        m_wayland_connection.pointerMgr = (zwlr_virtual_pointer_manager_v1*)wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, version);

    else if(INTERFACE == zwp_virtual_keyboard_manager_v1_interface.name)
        m_wayland_connection.keyboardMgr = (zwp_virtual_keyboard_manager_v1*)wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, version);
}

void handleGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) {
    ; // noop
}


inline const wl_registry_listener registryListener = {
    .global        = handleGlobal,
    .global_remove = handleGlobalRemove,
};


void init() {
    int m_iPID = getpid();

    // init wayland connection
    m_wayland_connection.display = wl_display_connect(nullptr);

    if (!m_wayland_connection.display) {
        colorprint("Couldn't connect to a wayland compositor");
        exit(1);
    }

    wl_registry* registry = wl_display_get_registry(m_wayland_connection.display);
    wl_registry_add_listener(registry, &registryListener, nullptr);

    wl_display_roundtrip(m_wayland_connection.display);
}

static void seatCapabilities(void *data, struct wl_seat *wl_seat, uint32_t caps)
{
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        colorprint("Seat has pointer");
    }
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        colorprint("Seat has keyboard");
        wl_display_dispatch(m_wayland_connection.display);
        wl_display_roundtrip(m_wayland_connection.display);
    }
}


static bool stop = false;

static void sighandler(int signal) {
	stop = true;
}


static void
log_handler(struct eis *eis, enum eis_log_priority priority,
	    const char *message, struct eis_log_context *ctx)
{
	struct lut {
		const char *color;
		const char *prefix;
	} lut[] = {
		{ .color = ansi_colorcode[RED],		.prefix = "<undefined>", }, /* debug starts at 10 */
		{ .color = ansi_colorcode[HIGHLIGHT],	.prefix = "DEBUG", },
		{ .color = ansi_colorcode[GREEN],	.prefix = "INFO", },
		{ .color = ansi_colorcode[BLUE],	.prefix = "WARN", },
		{ .color = ansi_colorcode[RED],		.prefix = "ERROR", },
	};
	static time_t last_time = 0;
	const char *reset_code = ansi_colorcode[RESET];

	run_only_once {
		if (!isatty(STDOUT_FILENO)) {
			struct lut *l;
			ARRAY_FOR_EACH(lut, l)
				l->color = "";
			reset_code = "";
		}
	}

	time_t now = time(NULL);
	char timestamp[64];

	if (last_time != now) {
		struct tm *tm = localtime(&now);
		strftime(timestamp, sizeof(timestamp), "%T", tm);
	} else {
		xsnprintf(timestamp, sizeof(timestamp), "...");
	}

	size_t idx = priority/10;
	assert(idx < ARRAY_LENGTH(lut));
	fprintf(stdout, " EIS: %8s | %s%4s%s | %s\n", timestamp,
		lut[idx].color, lut[idx].prefix, reset_code, message);

	last_time = now;
}
DEFINE_UNREF_CLEANUP_FUNC(eis);
DEFINE_UNREF_CLEANUP_FUNC(eis_event);
DEFINE_UNREF_CLEANUP_FUNC(eis_keymap);
DEFINE_UNREF_CLEANUP_FUNC(eis_seat);
DEFINE_UNREF_CLEANUP_FUNC(eis_region);

static void unlink_free(char **path) {
    if (*path) {
        unlink(*path);
        free(*path);
    }
}
#define _cleanup_unlink_free_ _cleanup_(unlink_free)

#if HAVE_MEMFD_CREATE && HAVE_LIBXKBCOMMON
DEFINE_UNREF_CLEANUP_FUNC(xkb_context);
DEFINE_UNREF_CLEANUP_FUNC(xkb_keymap);
DEFINE_UNREF_CLEANUP_FUNC(xkb_state);
#endif

static void
setup_keymap(struct hyprland_eis *server, struct eis_device *device)
{
#if HAVE_MEMFD_CREATE && HAVE_LIBXKBCOMMON
    colorprint("Using server layout: %s\n", server->layout);
    _unref_(xkb_context) *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx)
        return;

    struct xkb_rule_names names = {
        .rules = "evdev",
        .model = "pc105",
        .layout = server->layout,
    };

    _unref_(xkb_keymap) *keymap = xkb_keymap_new_from_names(ctx, &names, 0);
    if (!keymap)
        return;

    const char *str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t len = strlen(str) - 1;

    struct memfile *f = memfile_new(str, len);
    if (!f)
        return;

    _unref_(eis_keymap) *k = eis_device_new_keymap(device,
                        EIS_KEYMAP_TYPE_XKB, memfile_get_fd(f),
                        memfile_get_size(f));
    eis_keymap_add(k);
    memfile_unref(f);

    _unref_(xkb_state) *state = xkb_state_new(keymap);
    if (!state)
        return;

    server->ctx = steal(&ctx);
    server->keymap = steal(&keymap);
    server->state = steal(&state);
#endif
}

static struct hyprland_eis_client *
create_eis_client(struct hyprland_eis *server, struct eis_client *client)
{
    struct hyprland_eis_client *c = new hyprland_eis_client;
    c->client = eis_client_ref(client);
    list_append(&server->clients, &c->link);
    return c;
}

static struct hyprland_eis_client *
find_eis_client(struct hyprland_eis *server, struct eis_client *client)
{
    struct hyprland_eis_client *c;
    list_for_each(c, &server->clients, link) {
        if (c->client == client)
            return c;
    }
    return nullptr;
}

static struct eis_device *
add_device(struct hyprland_eis *server, struct eis_client *client,
	   struct eis_seat *seat, enum eis_device_capability cap)
{
	static uint32_t sequence;

	struct eis_device *device = NULL;
	switch (cap) {
	case EIS_DEVICE_CAP_POINTER:
		{
		struct eis_device *ptr = eis_seat_new_device(seat);
		eis_device_configure_name(ptr, "test pointer");
		eis_device_configure_capability(ptr, EIS_DEVICE_CAP_POINTER);
		eis_device_configure_capability(ptr, EIS_DEVICE_CAP_BUTTON);
		eis_device_configure_capability(ptr, EIS_DEVICE_CAP_SCROLL);
		_unref_(eis_region) *rel_region = eis_device_new_region(ptr);
		eis_region_set_mapping_id(rel_region, "demo region");
		eis_region_set_size(rel_region, 4520, 1440);
		eis_region_set_offset(rel_region, 0, 0);
		eis_region_add(rel_region);
		colorprint("Creating pointer device %s for %s\n", eis_device_get_name(ptr),
			   eis_client_get_name(client));
		eis_device_add(ptr);
		eis_device_resume(ptr);
		if (!eis_client_is_sender(client))
			eis_device_start_emulating(ptr, ++sequence);
		device = steal(&ptr);
		break;
		}
	case EIS_DEVICE_CAP_POINTER_ABSOLUTE:
		{
		struct eis_device *abs = eis_seat_new_device(seat);
		eis_device_configure_name(abs, "test abs pointer");
		eis_device_configure_capability(abs, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
		eis_device_configure_capability(abs, EIS_DEVICE_CAP_BUTTON);
		eis_device_configure_capability(abs, EIS_DEVICE_CAP_SCROLL);
		_unref_(eis_region) *region = eis_device_new_region(abs);
		eis_region_set_mapping_id(region, "demo region");
		eis_region_set_size(region, 4520, 1440);
		eis_region_set_offset(region, 0, 0);
		eis_region_add(region);
		colorprint("Creating abs pointer device %s for %s\n", eis_device_get_name(abs),
			   eis_client_get_name(client));
		eis_device_add(abs);
		eis_device_resume(abs);
		if (!eis_client_is_sender(client))
			eis_device_start_emulating(abs, ++sequence);
		device = steal(&abs);
		break;
		}
	case EIS_DEVICE_CAP_KEYBOARD:
		{
		struct eis_device *kbd = eis_seat_new_device(seat);
		eis_device_configure_name(kbd, "test keyboard");
		eis_device_configure_capability(kbd, EIS_DEVICE_CAP_KEYBOARD);
		if (server->layout)
			setup_keymap(server, kbd);
		colorprint("Creating keyboard device %s for %s\n", eis_device_get_name(kbd),
			   eis_client_get_name(client));
		eis_device_add(kbd);
		eis_device_resume(kbd);
		if (!eis_client_is_sender(client))
			eis_device_start_emulating(kbd, ++sequence);
		device = steal(&kbd);
		break;
		}
	case EIS_DEVICE_CAP_TOUCH:
		{
		struct eis_device *touchscreen = eis_seat_new_device(seat);
		eis_device_configure_name(touchscreen, "test touchscreen");
		eis_device_configure_capability(touchscreen, EIS_DEVICE_CAP_TOUCH);
		colorprint("Creating touchscreen device %s for %s\n", eis_device_get_name(touchscreen),
			   eis_client_get_name(client));
		eis_device_add(touchscreen);
		eis_device_resume(touchscreen);
		if (!eis_client_is_sender(client))
			eis_device_start_emulating(touchscreen, ++sequence);
		device = steal(&touchscreen);
		break;
		}
	case EIS_DEVICE_CAP_BUTTON:
	case EIS_DEVICE_CAP_SCROLL:
		/* Mixed in with pointer/abs - good enough for a demo server */
		break;
	}

	return device;
}

static void
handle_key(struct hyprland_eis *server, uint32_t keycode, bool is_press)
{
    char keysym_name[64] = {0};

#if HAVE_LIBXKBCOMMON
    if (server->state) {
        uint32_t xkbkc = keycode + 8;
        xkb_state_update_key(server->state, xkbkc, is_press ? XKB_KEY_DOWN : XKB_KEY_UP);
        xkb_state_key_get_utf8(server->state, xkbkc, keysym_name, sizeof(keysym_name));
    }
#endif
    colorprint("key %u (%s) [%s]\n",
           keycode, is_press ? "press" : "release",
           keysym_name);
    // xkb_mod_mask_t depressed = xkb_state_serialize_mods(server->state, XKB_STATE_MODS_DEPRESSED);
    // xkb_mod_mask_t latched = xkb_state_serialize_mods(server->state, XKB_STATE_MODS_LATCHED);
    // xkb_mod_mask_t locked = xkb_state_serialize_mods(server->state, XKB_STATE_MODS_LOCKED);
    // xkb_layout_index_t group = xkb_state_serialize_layout(server->state, XKB_STATE_LAYOUT_EFFECTIVE);

    zwp_virtual_keyboard_v1_key(wlr_context.keyboard, std::time(nullptr), keycode - 8, is_press ? 1 : 0);
    wl_display_flush(m_wayland_connection.display);

    colorprint("key down");
}

/**
 * The simplest event handler. Connect any client and any device and just
 * printf the events as the come in. This is an incomplete implementation,
 * it just does the basics for pointers and keyboards atm.
 */
static int
eis_demo_server_printf_handle_event(struct hyprland_eis *server,
				    struct eis_event *e)
{
	switch(eis_event_get_type(e)) {
	case EIS_EVENT_CLIENT_CONNECT:
		{
		struct eis_client *client = eis_event_get_client(e);
		bool is_sender = eis_client_is_sender(client);
		colorprint("new %s client: %s\n",
			   is_sender ? "sender" : "receiver",
			   eis_client_get_name(client));

		create_eis_client(server, client);
		if (!is_sender)
			server->nreceiver_clients++;

		/* insert sophisticated authentication here */
		eis_client_connect(client);
		colorprint("accepting client, creating new seat 'default'\n");
		_unref_(eis_seat) *seat = eis_client_new_seat(client, "default");
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_TOUCH);
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_SCROLL);
		eis_seat_add(seat);
		/* Note: we don't have a ref to this seat ourselves anywhere */
		break;
		}
	case EIS_EVENT_CLIENT_DISCONNECT:
		{
		struct eis_client *client = eis_event_get_client(e);
		bool is_sender = eis_client_is_sender(client);

		if (!is_sender)
			server->nreceiver_clients--;

		colorprint("client %s disconnected\n", eis_client_get_name(client));
		eis_client_disconnect(client);
		//eis_demo_client_unref(find_eis_client(server, client));
		break;
		}
	case EIS_EVENT_SEAT_BIND:
		{
		struct eis_client *client = eis_event_get_client(e);
		struct hyprland_eis_client *newclient = find_eis_client(server, client);
		assert(newclient);

		struct eis_seat *seat = eis_event_get_seat(e);

		if (eis_event_seat_has_capability(e, EIS_DEVICE_CAP_POINTER)) {
			if (!newclient->ptr)
				newclient->ptr = add_device(server, client, seat, EIS_DEVICE_CAP_POINTER);
		} else {
			if (newclient->ptr) {
				eis_device_remove(newclient->ptr);
				newclient->ptr = eis_device_unref(newclient->ptr);
			}
		}

		if (eis_event_seat_has_capability(e, EIS_DEVICE_CAP_POINTER_ABSOLUTE)) {
			if (!newclient->abs)
				newclient->abs = add_device(server, client, seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);

		    wlr_context.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(m_wayland_connection.pointerMgr, m_wayland_connection.seat);
		} else {
			if (newclient->abs) {
				eis_device_remove(newclient->abs);
				newclient->abs = eis_device_unref(newclient->abs);
			    zwlr_virtual_pointer_manager_v1_destroy(m_wayland_connection.pointerMgr);
			}
		}

		if (eis_event_seat_has_capability(e, EIS_DEVICE_CAP_KEYBOARD)) {
			if (!newclient->kbd)
				newclient->kbd = add_device(server, client, seat, EIS_DEVICE_CAP_KEYBOARD);

		    wlr_context.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(m_wayland_connection.keyboardMgr, m_wayland_connection.seat);

		} else {
			if (newclient->kbd) {
				eis_device_remove(newclient->kbd);
				newclient->kbd = eis_device_unref(newclient->kbd);
			    zwp_virtual_keyboard_v1_destroy(wlr_context.keyboard);
			}
		}

		if (eis_event_seat_has_capability(e, EIS_DEVICE_CAP_TOUCH)) {
			if (!newclient->touchscreen)
				newclient->touchscreen = add_device(server, client, seat, EIS_DEVICE_CAP_TOUCH);
		} else {
			if (newclient->touchscreen) {
				eis_device_remove(newclient->touchscreen);
				newclient->touchscreen = eis_device_unref(newclient->touchscreen);
			}
		}

		/* Special "Feature", if all caps are unbound remove the seat.
		 * This is a demo server after all, so let's demo this. */
		if (!eis_event_seat_has_capability(e, EIS_DEVICE_CAP_POINTER) &&
		    !eis_event_seat_has_capability(e, EIS_DEVICE_CAP_POINTER_ABSOLUTE) &&
		    !eis_event_seat_has_capability(e, EIS_DEVICE_CAP_KEYBOARD))
			eis_seat_remove(seat);

		break;
		}
	case EIS_EVENT_DEVICE_CLOSED:
		{
			struct eis_client *client = eis_event_get_client(e);
			struct hyprland_eis_client *currclient = find_eis_client(server, client);
			assert(currclient);

			struct eis_device *device = eis_event_get_device(e);
			eis_device_remove(device);

			if (currclient->ptr == device)
				currclient->ptr = NULL;

			if (currclient->abs == device)
				currclient->abs = NULL;

			if (currclient->kbd == device)
				currclient->kbd = NULL;

			if (currclient->touchscreen == device)
				currclient->touchscreen = NULL;

			eis_device_unref(device);
		}
		break;
	case EIS_EVENT_DEVICE_START_EMULATING:
		{
		struct eis_device *device = eis_event_get_device(e);
		colorprint("Device %s is ready to send events\n", eis_device_get_name(device));
		}
		break;
	case EIS_EVENT_DEVICE_STOP_EMULATING:
		{
		struct eis_device *device = eis_event_get_device(e);
		colorprint("Device %s will no longer send events\n", eis_device_get_name(device));
		}
		break;
	case EIS_EVENT_POINTER_MOTION:
		{
		colorprint("motion by %.2f/%.2f\n",
		       eis_event_pointer_get_dx(e),
		       eis_event_pointer_get_dy(e));
		}
		break;
	    case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
		{
		colorprint("absmotion to %.2f/%.2f\n",
		       eis_event_pointer_get_absolute_x(e),
		       eis_event_pointer_get_absolute_y(e));

	        long int clockTime = std::time(nullptr);

	        zwlr_virtual_pointer_v1_motion_absolute(
                wlr_context.pointer,
                clockTime,
                (uint32_t) eis_event_pointer_get_absolute_x(e),
                (uint32_t) eis_event_pointer_get_absolute_y(e),
                4520,
                1440
            );
	        zwlr_virtual_pointer_v1_frame(wlr_context.pointer);
	        wl_display_flush(m_wayland_connection.display);
		}
		break;
	case EIS_EVENT_BUTTON_BUTTON:
		{
		colorprint("button %u (%s)\n",
		       eis_event_button_get_button(e),
		       eis_event_button_get_is_press(e) ? "press" : "release");
	    zwlr_virtual_pointer_v1_button(wlr_context.pointer, std::time(nullptr), eis_event_button_get_button(e), eis_event_button_get_is_press(e) ? 1 : 0);
	    zwlr_virtual_pointer_v1_frame(wlr_context.pointer);
	    wl_display_flush(m_wayland_connection.display);
		}
		break;
	case EIS_EVENT_SCROLL_DELTA:
		{
		colorprint("scroll %.2f/%.2f\n",
			eis_event_scroll_get_dx(e),
			eis_event_scroll_get_dy(e));
		}
		break;
	case EIS_EVENT_SCROLL_DISCRETE:
		{
		colorprint("scroll discrete %d/%d\n",
			eis_event_scroll_get_discrete_dx(e),
			eis_event_scroll_get_discrete_dy(e));
		}
		break;
	case EIS_EVENT_KEYBOARD_KEY:
		{
		handle_key(server,
			   eis_event_keyboard_get_key(e),
			   eis_event_keyboard_get_key_is_press(e));
		}
		break;
	case EIS_EVENT_TOUCH_DOWN:
	case EIS_EVENT_TOUCH_MOTION:
		{
		colorprint("touch %s %u %.2f/%.2f\n",
			   eis_event_get_type(e) == EIS_EVENT_TOUCH_DOWN ? "down" : "motion",
			   eis_event_touch_get_id(e),
			   eis_event_touch_get_x(e),
			   eis_event_touch_get_y(e));
		}
		break;
	case EIS_EVENT_TOUCH_UP:
		{
		colorprint("touch up %u\n", eis_event_touch_get_id(e));
		}
		break;
	case EIS_EVENT_FRAME:
		{
		colorprint("frame timestamp: %" PRIu64 "\n",
			   eis_event_get_time(e));
		}
		break;
	default:
		abort();
	}
	return 0;
}

static void
usage(FILE *fp, const char *argv0)
{
    fprintf(fp,
        "Usage: %s [--verbose] [--uinput] [--socketpath=/path/to/socket] [--interval=1000]\n"
        "\n"
        "Start an Hyprland EIS server. The server accepts all client connections\n"
        "and devices and prints any events from the client to stdout.\n"
        "\n"
        "Options:\n"
        " --socketpath	Use the given socket path. Default: $XDG_RUNTIME_DIR/eis-0\n"
        " --layout	Use the given XKB layout (requires libxkbcommon). Default: none\n"
        " --uinput	Set up each device as uinput device (this requires root)\n"
        " --interval    Interval in milliseconds between polling\n"
        " --verbose	Enable debugging output\n"
        "",
        argv0);
}

int main(int argc, char **argv)
{
	bool verbose = false;
	bool uinput = false;
	unsigned int interval = 1000;
	const char *layout = NULL;

    init();

	_cleanup_unlink_free_ char *socketpath = NULL;
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	if (xdg)
		socketpath = xaprintf("%s/eis-0", xdg);

	while (true) {
		enum {
			OPT_VERBOSE,
			OPT_LAYOUT,
			OPT_SOCKETPATH,
			OPT_UINPUT,
			OPT_INTERVAL,
		};
		static struct option long_opts[] = {
			{"socketpath",	required_argument, 0, OPT_SOCKETPATH},
			{"layout",	required_argument, 0, OPT_LAYOUT},
			{"uinput",	no_argument, 0, OPT_UINPUT},
			{"verbose",	no_argument, 0, OPT_VERBOSE},
			{"interval",    required_argument, 0, OPT_INTERVAL},
			{"help",	no_argument, 0, 'h'},
			{NULL},
		};

		int optind = 0;
		int c = getopt_long(argc, argv, "h", long_opts, &optind);
		if (c == -1)
			break;

		switch(c) {
			case 'h':
				usage(stdout, argv[0]);
				return EXIT_SUCCESS;
			case OPT_SOCKETPATH:
				free(socketpath);
				socketpath = xstrdup(optarg);
				break;
			case OPT_LAYOUT:
				layout = optarg;
				break;
			case OPT_UINPUT:
				uinput = true;
				break;
			case OPT_VERBOSE:
				verbose = true;
				break;
			case OPT_INTERVAL:
				interval = atoi(optarg);
				break;
			default:
				usage(stderr, argv[0]);
				return EXIT_FAILURE;
		}
	}

	if (socketpath == NULL) {
		fprintf(stderr, "No socketpath given and $XDG_RUNTIME_DIR is not set\n");
		return EXIT_FAILURE;
	}

	struct hyprland_eis server = {
		.layout = layout,
		.handler = {
		    .handle_event = eis_demo_server_printf_handle_event,
		}
	};

	list_init(&server.clients);

	if (uinput) {
		int rc = -ENOTSUP;
// #if HAVE_LIBEVDEV
// 		rc = eis_demo_server_setup_uinput_handler(&server);
// #endif
		if (rc != 0) {
			fprintf(stderr, "Failed to set up uinput handler: %s\n", strerror(-rc));
			return EXIT_FAILURE;
		}
	}

	_unref_(eis) *eis = eis_new(NULL);
	assert(eis);

	if (verbose) {
		eis_log_set_priority(eis, EIS_LOG_PRIORITY_DEBUG);
		eis_log_set_handler(eis, log_handler);
	}

	signal(SIGINT, sighandler);

	int rc = eis_setup_backend_socket(eis, socketpath);
	if (rc != 0) {
		fprintf(stderr, "init failed: %s\n", strerror(errno));
		return 1;
	}

	colorprint("waiting on %s\n", socketpath);

	struct pollfd fds = {
		.fd = eis_get_fd(eis),
		.events = POLLIN,
		.revents = 0,
	};

	int nevents;
	while (!stop && (nevents = poll(&fds, 1, interval)) > -1) {
		if (nevents == 0 && server.nreceiver_clients == 0)
			continue;

		uint64_t now = eis_now(eis);
		colorprint("now: %" PRIu64 "\n", now);

		eis_dispatch(eis);

		while (true) {
			_unref_(eis_event) *e = eis_get_event(eis);
			if (!e)
				break;

			int rc = server.handler.handle_event(&server, e);
			if (rc != 0)
				break;
		}
	}

	return 0;
}
