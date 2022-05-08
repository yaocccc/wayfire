#pragma once

#include <wayfire/toplevel.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/util.hpp>

namespace wf
{
class xwayland_toplevel_t : public wf::toplevel_t
{
  private:
    toplevel_state_t _current;

    int moving_counter   = 0;
    int resizing_counter = 0;
    uint32_t resizing_edges = 0;

    /** Last request to the client */
    wf::dimensions_t last_size_request = {0, 0};
    bool should_resize_client(wf::dimensions_t request,
        wf::dimensions_t current_size);

    bool has_csd = true;
    std::unique_ptr<toplevel_decorator_t> decorator;
    wlr_xwayland_surface *xw;

    wf::wl_listener_wrapper on_commit;
    wf::wl_listener_wrapper on_destroy;
    wf::wl_listener_wrapper on_request_move;
    wf::wl_listener_wrapper on_request_resize;
    wf::wl_listener_wrapper on_request_activate;
    wf::wl_listener_wrapper on_request_minimize;
    wf::wl_listener_wrapper on_request_maximize;
    wf::wl_listener_wrapper on_request_fullscreen;
    wf::wl_listener_wrapper on_configure;

    wf::wl_listener_wrapper on_set_hints;
    wf::wl_listener_wrapper on_set_decorations;

    void handle_configure_request(wlr_xwayland_surface_configure_event *ev);
    void send_configure();
    void commit();
    void destroy();

    wf::signal_connection_t on_output_geometry_changed;

  public:
    xwayland_toplevel_t(wlr_xwayland_surface *xw,
        wf::output_t *initial_output);

    bool should_be_decorated() final;
    toplevel_state_t& current() final;

    void set_minimized(bool minimized) final;
    void set_tiled(uint32_t edges) final;
    void set_fullscreen(bool fullscreen) final;
    void set_activated(bool active) final;
    void set_geometry(wf::geometry_t g) final;
    void move(int x, int y) final;

    void set_output(wf::output_t *new_output) final;
    void set_moving(bool moving) final;
    bool is_moving() final;
    void set_resizing(bool resizing, uint32_t edges = 0) final;
    bool is_resizing() final;
    void request_native_size() final;
    void set_decoration(std::unique_ptr<toplevel_decorator_t> frame) final;
};
}
