#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

struct tinywl_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_output;
	struct wl_listener new_xdg_toplevel;
};

static void server_new_output(struct wl_listener *listener, void *data) {
	struct tinywl_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) wlr_output_state_set_mode(&state, mode);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	wlr_scene_output_create(server->scene, wlr_output);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct tinywl_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;
	wlr_scene_xdg_surface_create(&server->scene->tree, toplevel->base);
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	printf("[TinyWL] Starting minimized version...\n");
	
	/* Force Pixman renderer and software cursor to minimize hardware/DRM requirements */
	setenv("WLR_RENDERER", "pixman", 1);
	setenv("WLR_NO_HARDWARE_CURSORS", "1", 1);
	
	struct tinywl_server server = {0};
	server.wl_display = wl_display_create();
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), NULL);
	if (!server.backend) {
		fprintf(stderr, "Failed to create backend\n");
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		fprintf(stderr, "Failed to create renderer\n");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		fprintf(stderr, "Failed to create allocator\n");
		return 1;
	}

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	server.scene = wlr_scene_create();
	
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) return 1;

	if (!wlr_backend_start(server.backend)) {
		fprintf(stderr, "Failed to start backend\n");
		return 1;
	}

	printf("Running on WAYLAND_DISPLAY=%s\n", socket);
	setenv("WAYLAND_DISPLAY", socket, 1);
	wl_display_run(server.wl_display);

	wl_display_destroy(server.wl_display);
	return 0;
}
