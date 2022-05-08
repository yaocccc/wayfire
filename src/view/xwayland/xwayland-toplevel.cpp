#include <wayfire/toplevel-helpers.hpp>
#include "xwayland-toplevel.hpp"
#include "xwayland-helpers.hpp"
#include <wayfire/signal-definitions.hpp>
#include "../view-impl.hpp"

wf::xwayland_toplevel_t::xwayland_toplevel_t(
    wlr_xwayland_surface *xw, wf::output_t *initial_output)
{
    this->xw = xw;
    this->_current.primary_output = initial_output;
    initial_output->connect_signal("output-configuration-changed",
        &on_output_geometry_changed);

    on_commit.set_callback([&] (void*)
    {
        commit();
    });
    on_destroy.set_callback([&] (void*)
    {
        destroy();
    });
    on_set_decorations.set_callback([&] (void*)
    {
        // TODO
    });
    on_configure.set_callback([&] (void *data)
    {
        handle_configure_request((wlr_xwayland_surface_configure_event*)data);
    });

    on_request_move.set_callback([&] (void*)
    {
        wf::toplevel_emit_move_request({this});
    });
    on_request_resize.set_callback([&] (auto data)
    {
        auto ev = static_cast<wlr_xwayland_resize_event*>(data);
        wf::toplevel_emit_resize_request({this}, ev->edges);
    });
    on_request_maximize.set_callback([&] (void*)
    {
        uint32_t desired_edges =
            (xw->maximized_horz && xw->maximized_vert) ? wf::TILED_EDGES_ALL : 0;
        wf::toplevel_emit_tile_request({this}, desired_edges);
    });
    on_request_fullscreen.set_callback([&] (void*)
    {
        wf::toplevel_emit_fullscreen_request({this},
            _current.primary_output, xw->fullscreen);
    });
    on_request_minimize.set_callback([&] (void *data)
    {
        auto ev = (wlr_xwayland_minimize_event*)data;
        wf::toplevel_emit_minimize_request({this}, ev->minimize);
    });
    on_request_activate.set_callback([&] (void*)
    {
        if (!this->_current.activated)
        {
            wf::toplevel_focus_request_signal data;
            data.toplevel = {this};
            data.self_request = true;
            emit_signal("toplevel-focus-request", &data);
            wf::get_core().emit_signal("view-focus-request", &data);
        }
    });
    on_set_hints.set_callback([&] (void*)
    {
        wf::toplevel_hints_changed_signal data;
        data.toplevel = {this};
        if (xw->hints_urgency)
        {
            data.demands_attention = true;
        }

        wf::get_core().emit_signal("view-hints-changed", &data);
        this->emit_signal("hints-changed", &data);
    });

    on_commit.connect(&xw->surface->events.commit);
    on_destroy.connect(&xw->events.destroy);
    on_set_decorations.connect(&xw->events.set_decorations);
    on_set_decorations.emit(NULL);

    on_set_hints.connect(&xw->events.set_hints);

    on_configure.connect(&xw->events.request_configure);
    on_request_move.connect(&xw->events.request_move);
    on_request_resize.connect(&xw->events.request_resize);
    on_request_activate.connect(&xw->events.request_activate);
    on_request_maximize.connect(&xw->events.request_maximize);
    on_request_minimize.connect(&xw->events.request_minimize);
    on_request_fullscreen.connect(&xw->events.request_fullscreen);

    // If the output position changes, we need to synchronize the Xwayland
    // position with the current geometry on Wayfire's side (which is relative
    // to the toplevel's primary output).
    on_output_geometry_changed.set_callback([&] (wf::signal_data_t*)
    {
        send_configure();
    });
}

void wf::xwayland_toplevel_t::destroy()
{
    this->xw = NULL;
    on_commit.disconnect();
    on_destroy.disconnect();
    on_set_hints.disconnect();
    on_set_decorations.disconnect();

    on_configure.disconnect();
    on_request_move.disconnect();
    on_request_resize.disconnect();
    on_request_activate.disconnect();
    on_request_maximize.disconnect();
    on_request_minimize.disconnect();
    on_request_fullscreen.disconnect();
}

void wf::xwayland_toplevel_t::commit()
{
    wf::dimensions_t new_size = {
        xw->surface->current.width,
        xw->surface->current.height,
    };

    if ((new_size != wf::dimensions(_current.base_geometry)))
    {
        // Base geometry changed. Adjust for gravity.
        wf::toplevel_geometry_changed_signal data;
        data.toplevel     = {this};
        data.old_geometry = _current.geometry;

        auto new_size_full = new_size;
        if (decorator)
        {
            auto margin = decorator->get_margins();
            new_size_full.width  += margin.left + margin.right;
            new_size_full.height += margin.top + margin.bottom;
        }

        _current.geometry = adjust_geometry_for_gravity(_current.geometry,
            this->resizing_edges, new_size_full);

        if (decorator)
        {
            _current.base_geometry =
                wf::shrink_by_margins(_current.geometry, decorator->get_margins());
            decorator->notify_resized(_current.geometry);
        } else
        {
            _current.base_geometry = _current.geometry;
        }

        emit_toplevel_signal(this, "geometry-changed", &data);

        /* Avoid loops where the client wants to have a certain size but the
         * compositor keeps trying to resize it */
        last_size_request = new_size;
        send_configure();
    }

    /* Clear the resize edges.
     * This is must be done here because if the user(or plugin) resizes too fast,
     * the shell client might still haven't configured the surface, and in this
     * case the next commit(here) needs to still have access to the gravity */
    if (!this->is_resizing())
    {
        this->resizing_edges = 0;
    }

    /* Avoid loops where the client wants to have a certain size but the
     * compositor keeps trying to resize it */
    last_size_request = new_size;
}

wf::toplevel_state_t& wf::xwayland_toplevel_t::current()
{
    return _current;
}

bool wf::xwayland_toplevel_t::should_be_decorated()
{
    if (!xw)
    {
        return false;
    }

    // return (wf::wlr_view_t::should_be_decorated() &&
    // !has_type(_NET_WM_WINDOW_TYPE_SPLASH));

    // uint32_t csd_flags = WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE |
    // WLR_XWAYLAND_SURFACE_DECORATIONS_NO_BORDER;
    // this->set_decoration_mode(xw->decorations & csd_flags);

    // TODO
    return true;
}

void wf::xwayland_toplevel_t::set_minimized(bool minimized)
{
    if (xw)
    {
        wlr_xwayland_surface_set_minimized(xw, minimized);
    }

    _current.minimized = minimized;
    wf::toplevel_minimized_signal data;
    data.toplevel = {this};
    data.state    = minimized;
    wf::emit_toplevel_signal(this, "minimized", &data);
}

void wf::xwayland_toplevel_t::set_tiled(uint32_t edges)
{
    if (xw)
    {
        wlr_xwayland_surface_set_maximized(xw, !!edges);
    }

    wf::toplevel_tiled_signal data;
    data.toplevel  = {this};
    data.old_edges = _current.tiled_edges;

    _current.tiled_edges = edges;
    if (decorator)
    {
        decorator->notify_tiled();
    }

    data.new_edges = edges;
    wf::emit_toplevel_signal(this, "tiled", &data);
}

void wf::xwayland_toplevel_t::set_fullscreen(bool fullscreen)
{
    if (xw)
    {
        wlr_xwayland_surface_set_fullscreen(xw, fullscreen);
    }

    _current.fullscreen = fullscreen;
    if (decorator)
    {
        decorator->notify_fullscreen();
    }

    wf::toplevel_fullscreen_signal data;
    data.toplevel = {this};
    data.state    = fullscreen;
    wf::emit_toplevel_signal(this, "fullscreen", &data);
}

void wf::xwayland_toplevel_t::set_activated(bool active)
{
    if (xw)
    {
        wlr_xwayland_surface_activate(xw, active);
    }

    this->_current.activated = active;
    if (decorator)
    {
        decorator->notify_activated(active);
    }
}

void wf::xwayland_toplevel_t::handle_configure_request(
    wlr_xwayland_surface_configure_event *ev)
{
    const auto& wo = _current.primary_output;
    const auto output_offset =
        wo ? wf::origin(wo->get_layout_geometry()) : wf::point_t{0, 0};

    if (xw->mapped)
    {
        // Default implementation does not allow Xwayland surfaces to move
        // themselves. However, maybe the view wants to just resize itself.
        if ((ev->mask & XCB_CONFIG_WINDOW_WIDTH) &&
            (ev->mask & XCB_CONFIG_WINDOW_HEIGHT))
        {
            auto base = _current.base_geometry;
            base.width  = ev->width;
            base.height = ev->height;

            auto desired = base;
            if (decorator)
            {
                desired = wf::expand_with_margins(base, decorator->get_margins());
            }

            set_geometry(desired);
        }

        return;
    }

    // If the view is not mapped yet, let it do whatever it wants
    wlr_xwayland_surface_configure(xw,
        ev->x, ev->y, ev->width, ev->height);

    if ((ev->mask & XCB_CONFIG_WINDOW_X) &&
        (ev->mask & XCB_CONFIG_WINDOW_Y))
    {
        const auto x = ev->x - output_offset.x;
        const auto y = ev->y - output_offset.y;

        _current.base_geometry.x = x;
        _current.base_geometry.y = y;
        _current.geometry.x = x;
        _current.geometry.y = y;
        _current.flags.has_position = true;
    }
}

void wf::xwayland_toplevel_t::send_configure()
{
    if ((last_size_request.width < 0) || (last_size_request.height < 0))
    {
        /* such a configure request would freeze xwayland.
         * This is most probably a bug somewhere in the compositor. */
        LOGE("Configuring a xwayland surface with width/height <0");

        return;
    }

    wf::geometry_t target = wf::construct_box(
        wf::origin(_current.base_geometry), last_size_request);
    if (_current.primary_output)
    {
        auto offset = wf::origin(_current.primary_output->get_layout_geometry());
        target = target + offset;
    }

    wlr_xwayland_surface_configure(xw,
        target.x, target.y, target.width, target.height);
}

void wf::xwayland_toplevel_t::move(int x, int y)
{
    wf::toplevel_geometry_changed_signal data;
    data.toplevel     = {this};
    data.old_geometry = _current.geometry;

    wf::point_t offset = {0, 0};
    if (decorator)
    {
        auto margins = decorator->get_margins();
        offset.x = margins.left;
        offset.y = margins.top;
    }

    this->_current.base_geometry.x = x - offset.x;
    this->_current.base_geometry.y = y - offset.y;
    this->_current.geometry.x = x;
    this->_current.geometry.y = y;
    this->_current.flags.has_position = true;

    if (xw && !moving_counter)
    {
        send_configure();
    }

    wf::emit_toplevel_signal(this, "geometry-changed", &data);
}

void wf::xwayland_toplevel_t::set_geometry(wf::geometry_t g)
{
    auto real = g;
    if (decorator)
    {
        real = wf::shrink_by_margins(g, decorator->get_margins());
    }

    auto cur_size = wf::dimensions(_current.base_geometry);
    if (should_resize_client(wf::dimensions(real), cur_size))
    {
        last_size_request = wf::dimensions(real);
    }

    _current.geometry.width  = g.width;
    _current.geometry.height = g.height;
    _current.base_geometry.width  = real.width;
    _current.base_geometry.height = real.height;

    move(g.x, g.y);
}

void wf::xwayland_toplevel_t::set_output(wf::output_t *new_output)
{
    wf::toplevel_output_changed_signal data;
    data.toplevel   = {this};
    data.old_output = _current.primary_output;

    _current.primary_output = new_output;
    wf::emit_toplevel_signal(this, "output-changed", &data);

    // We need to re-send our position, because it changed on the global scale.
    on_output_geometry_changed.disconnect();
    new_output->connect_signal("output-configuration-changed",
        &on_output_geometry_changed);
    send_configure();
}

void wf::xwayland_toplevel_t::set_moving(bool moving)
{
    this->moving_counter += moving ? 1 : -1;
    if (this->moving_counter < 0)
    {
        LOGE("in_continuous_move counter dropped below 0!");
    }

    /* We don't send updates while in continuous move, because that means
     * too much configure requests. Instead, we set it at the end */
    if (xw && !this->moving_counter)
    {
        send_configure();
    }
}

bool wf::xwayland_toplevel_t::is_moving()
{
    return this->moving_counter;
}

void wf::xwayland_toplevel_t::set_resizing(bool resizing, uint32_t edges)
{
    if (resizing)
    {
        resizing_edges = edges;
    }

    this->resizing_counter += resizing ? 1 : -1;
    if (this->resizing_counter < 0)
    {
        LOGE("in_continuous_resize counter dropped below 0!");
    }
}

bool wf::xwayland_toplevel_t::is_resizing()
{
    return this->resizing_counter;
}

void wf::xwayland_toplevel_t::request_native_size()
{
    if (!xw || !xw->size_hints)
    {
        return;
    }

    if ((xw->size_hints->base_width > 0) && (xw->size_hints->base_height > 0))
    {
        this->last_size_request = {
            xw->size_hints->base_width,
            xw->size_hints->base_height
        };
        send_configure();
    }
}

void wf::xwayland_toplevel_t::set_decoration(
    std::unique_ptr<toplevel_decorator_t> frame)
{
    // TODO
}
