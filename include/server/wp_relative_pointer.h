#ifndef WAYWALL_SERVER_WP_RELATIVE_POINTER_H
#define WAYWALL_SERVER_WP_RELATIVE_POINTER_H

#include <wayland-server-core.h>

struct config;

struct server_relative_pointer {
    struct wl_global *global;
    struct wl_list objects; // wl_resource (zwp_relative_pointer_v1) link

    struct {
        double sens;
    } config;

    struct server *server;
    struct server_view *input_focus;

    double acc_x, acc_y;
    double acc_x_unaccel, acc_y_unaccel;

    struct zwp_relative_pointer_manager_v1 *remote;
    struct zwp_relative_pointer_v1 *remote_pointer;

    struct wl_listener on_input_focus;
    struct wl_listener on_pointer;

    struct wl_listener on_display_destroy;
};

struct server_relative_pointer *server_relative_pointer_create(struct server *server,
                                                               struct config *cfg);
void server_relative_pointer_set_config(struct server_relative_pointer *relative_pointer,
                                        struct config *cfg);

#endif
