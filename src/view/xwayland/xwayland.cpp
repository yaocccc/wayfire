#include "wayfire/debug.hpp"
#include <wayfire/util/log.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/render-manager.hpp>
#include "wayfire/core.hpp"
#include "wayfire/output.hpp"
#include "wayfire/workspace-manager.hpp"
#include "wayfire/decorator.hpp"
#include "wayfire/output-layout.hpp"
#include "wayfire/signal-definitions.hpp"
#include "../core/core-impl.hpp"
#include "../core/seat/cursor.hpp"
#include "../core/seat/input-manager.hpp"
#include "../view-impl.hpp"
#include "xwayland-desktop-surface.hpp"
#include "xwayland-helpers.hpp"
#include "xwayland-toplevel.hpp"
#include <wayfire/toplevel-helpers.hpp>

#if WF_HAS_XWAYLAND

namespace wf
{
namespace xw
{

/**
 * A surface_interface_t implementation for Xwayland-based surfaces.
 * Based on the default wlr_surface surface_interface_t implementation,
 * only adds a few optimizations where possible.
 */
class xwayland_surface_t : public wlr_surface_base_t
{
    wf::wl_listener_wrapper on_destroy;
    wlr_xwayland_surface *xw;

  public:
    xwayland_surface_t(wlr_xwayland_surface *xw)
        : wlr_surface_base_t(xw->surface), xw(xw)
    {
        on_destroy.set_callback([=] (void*)
        {
            on_destroy.disconnect();
            this->xw = NULL;
        });
        on_destroy.connect(&xw->events.destroy);
    }

    wf::region_t get_opaque_region() final
    {
        // Special optimizations are possible for Xwayland clients which report
        // that they have opaque regions but Xwayland does not forward this
        // information via wl_surface.
        if (xw && !xw->has_alpha)
        {
            pixman_region32_union_rect(
                &xw->surface->opaque_region, &xw->surface->opaque_region,
                0, 0, xw->surface->current.width, xw->surface->current.height);
        }

        return wlr_surface_base_t::get_opaque_region();
    }
};

/* Translates geometry from X client configure requests to wayfire
 * coordinate system. The X coordinate system treats all outputs
 * as one big desktop, whereas wayfire treats the current workspace
 * of an output as 0,0 and everything else relative to that. This
 * means that we must take care when placing xwayland clients that
 * request a configure after initial mapping, while not on the
 * current workspace.
 *
 * @param xwayland_view The xwayland view whose geometry should be translated.
 * @param ws_offset     The view's workspace minus the current workspace.
 * @param geometry      The configure geometry as requested by the client.
 *
 * @return Geometry with a position that is within the view's workarea.
 * The workarea is the workspace where the view was initially mapped.
 * Newly mapped views are placed on the current workspace.
 */
wf::geometry_t translate_geometry_to_output(
    wf::wlr_view_t *xwayland_view,
    wf::point_t ws_offset,
    wf::geometry_t g)
{
    auto outputs = wf::get_core().output_layout->get_outputs();
    auto og   = xwayland_view->get_output()->get_layout_geometry();
    auto from = wf::get_core().output_layout->get_output_at(
        g.x + g.width / 2 + og.x, g.y + g.height / 2 + og.y);
    if (!from)
    {
        return g;
    }

    auto lg = from->get_layout_geometry();
    g.x += (og.x - lg.x) + ws_offset.x * og.width;
    g.y += (og.y - lg.y) + ws_offset.y * og.height;
    if (!xwayland_view->is_mapped())
    {
        g.x *= (float)og.width / lg.width;
        g.y *= (float)og.height / lg.height;
    }

    return g;
}

/**
 * Wayfire positions views relative to their output, but Xwayland
 * windows have a global positioning. So, we need to make sure that we
 * always transform between output-local coordinates and global
 * coordinates. Additionally, when clients send a configure request
 * after they have already been mapped, we keep the view on the
 * workspace where its center point was from last configure, in
 * case the current workspace is not where the view lives.
 *
 * @param view The view the configure request is about.
 * @param workarea The workarea available for the view.
 * @param configure_geometry The geometry as requested by the client.
 *
 * @return The geometry the view should be configured as, in Wayfire
 * coordinates.
 */
wf::geometry_t configure_request(wf::wlr_view_t *view,
    wf::geometry_t workarea,
    wf::geometry_t configure_geometry)
{
    if (!view->get_output())
    {
        return configure_geometry;
    }

    auto og = view->get_output()->get_layout_geometry();
    configure_geometry.x -= og.x;
    configure_geometry.y -= og.y;

    auto parent = wf::find_toplevel_parent({view});
    auto vg = parent->get_untransformed_bounding_box();

    // View workspace relative to current workspace
    wf::point_t view_ws = {0, 0};
    if (parent->is_mapped())
    {
        view_ws = {
            (int)std::floor((vg.x + vg.width / 2.0) / og.width),
            (int)std::floor((vg.y + vg.height / 2.0) / og.height),
        };

        workarea.x += og.width * view_ws.x;
        workarea.y += og.height * view_ws.y;
    }

    configure_geometry = translate_geometry_to_output(
        view, view_ws, configure_geometry);
    configure_geometry = wf::clamp(configure_geometry, workarea);

    return configure_geometry;
}

/**
 * A view_interface_t implementation for toplevel Xwayland surfaces.
 * Most of the work is delegated to the generic wlr_view_t implementation
 * and to the xwayland_toplevel_t component, as well as the
 * generic xwayland view controller for each view.
 */
class xwayland_toplevel_view_t : public wlr_view_t
{
    wlr_xwayland_surface *xw;
    wf::wl_listener_wrapper on_set_parent;

    static wf::output_t *determine_initial_output(wlr_xwayland_surface *xw)
    {
        wf::point_t midpoint = {xw->x + xw->width / 2, xw->y + xw->height / 2};
        // Fullscreen clients can request where they want to be fullscreened.
        if (xw->fullscreen)
        {
            auto natural_output =
                wf::get_core().output_layout->get_output_at(midpoint.x, midpoint.y);
            return natural_output ?: wf::get_core().get_active_output();
        }

        return wf::get_core().get_active_output();
    }

  public:
    xwayland_toplevel_view_t(
        wf::surface_sptr_t main_surface,
        wf::dsurface_sptr_t dsurface,
        wlr_xwayland_surface *xw) : xw(xw)
    {
        this->set_main_surface(main_surface);
        this->set_desktop_surface(dsurface);

        auto toplevel = std::make_shared<wf::xwayland_toplevel_t>(xw,
            determine_initial_output(xw));
        this->set_toplevel(toplevel);
        this->setup_toplevel_tracking();

        on_set_parent.set_callback([&] (void*)
        {
            auto parent = xw->parent ?
                static_cast<wf::view_interface_t*>(xw->parent->data)->self() : nullptr;

            // Make sure the parent is mapped, and that we are not a toplevel view
            if (parent && (!parent->is_mapped() ||
                    xwayland_surface_has_type(xw, _NET_WM_WINDOW_TYPE_NORMAL)))
            {
                parent = nullptr;
            }

            set_toplevel_parent(parent);
        });

        on_set_parent.connect(&xw->events.set_parent);

        xw->data = dynamic_cast<wf::view_interface_t*>(this);
        // set initial parent
        on_set_parent.emit(nullptr);
    }

    void destroy() final
    {
        wlr_view_t::destroy();
        on_set_parent.disconnect();
    }

    void map() override
    {
        if (xw->maximized_horz && xw->maximized_vert)
        {
            if ((xw->width > 0) && (xw->height > 0))
            {
                /* Save geometry which the window has put itself in */
                wf::geometry_t save_geometry = {
                    xw->x, xw->y, xw->width, xw->height
                };

                /* Make sure geometry is properly visible on the view output */
                save_geometry = wf::clamp(save_geometry,
                    get_output()->workspace->get_workarea());

                auto tsg = view_impl->toplevel->get_data_safe<toplevel_saved_geometry_t>();
                // FIXME: is this the best way to do this?
                tsg->last_windowed_geometry = save_geometry;
            }

            toplevel_emit_tile_request(topl(), wf::TILED_EDGES_ALL);
        }

        if (xw->fullscreen)
        {
            toplevel_emit_fullscreen_request(topl(), NULL, true);
        }

        if (!topl()->current().tiled_edges && !topl()->current().fullscreen)
        {
            auto client_wants = configure_request(this, get_output()->workspace->get_workarea(),
                {xw->x, xw->y, xw->width, xw->height});
            topl()->set_geometry(client_wants);
        }

        wf::wlr_view_t::map();
    }
};

/**
 * A view implementation for override-redirect and similar Xwayland surfaces.
 * They are characterized by the fact that they are not toplevel windows.
 * As such, the client (and the view implementation logic) is in full control of
 * their geometry. They also do not have an associated toplevel.
 */
class xwayland_or_view_t : public wf::wlr_view_t
{
    wlr_xwayland_surface *xw;

    // We listen for configure events on the Xwayland surface and adjust our
    // geometry accordingly.
    wf::wl_listener_wrapper on_configure;
    wf::wl_listener_wrapper on_set_geometry;

    wf::signal_connection_t my_output_changed = [&] (wf::signal_data_t *)
    {
        my_output_geometry_changed.disconnect();
        workspace_changed.disconnect();

        if (get_output())
        {
            get_output()->connect_signal("output-configuration-changed", &my_output_geometry_changed);
            get_output()->connect_signal("workspace-changed", &workspace_changed);
        }

        reconfigure(xw->width, xw->height);
    };

    wf::signal_connection_t my_output_geometry_changed = [&] (wf::signal_data_t*)
    {
        reconfigure(xw->width, xw->height);
    };

    wf::signal_connection_t workspace_changed = [&] (wf::signal_data_t *data)
    {
        // OR views do not have an associated toplevel.
        // Because of this, they cannot be moved by workspace manager when the
        // current workspace changes. Instead, we listen for workspace changed
        // and adjust our internal position.
        auto ev = static_cast<wf::workspace_changed_signal*>(data);
        auto delta = ev->old_viewport - ev->new_viewport;
        if (get_output())
        {
            auto dim = get_output()->get_screen_size();
            delta.x *= dim.width;
            delta.y *= dim.height;

            origin.x += delta.x;
            origin.y += delta.y;
            update_bbox();
            reconfigure(xw->width, xw->height);
        }
    };

    void reconfigure(int width, int height)
    {
        if (!xw)
        {
            return;
        }

        if (width <= 0 || height <= 0)
        {
            LOGE("Compositor bug! Xwayland surface configured with ",
                width, "x", height);
            return;
        }

        wf::point_t output_offset = {0, 0};
        if (get_output())
        {
            output_offset = wf::origin(get_output()->get_layout_geometry());
        }

        wlr_xwayland_surface_configure(xw,
            this->origin.x - output_offset.x,
            this->origin.y - output_offset.y,
            xw->width, xw->height);
    }

  public:
    xwayland_or_view_t(
        wf::surface_sptr_t main_surface,
        wf::dsurface_sptr_t dsurface,
        wlr_xwayland_surface *xw) : xw(xw)
    {
        this->set_main_surface(main_surface);
        this->set_desktop_surface(dsurface);

        this->connect_signal("set-output", &my_output_changed);
        my_output_changed.emit(NULL);

        on_configure.set_callback([&] (void *data)
        {
            auto ev = static_cast<wlr_xwayland_surface_configure_event*>(data);

            if (!get_output() || !is_mapped())
            {
                wlr_xwayland_surface_configure(xw,
                    ev->x, ev->y, ev->width, ev->height);
                return;
            }

            auto geometry = configure_request(this,
                get_output()->workspace->get_workarea(),
                {ev->x, ev->y, ev->width, ev->height});

            origin = wf::origin(geometry);
            update_bbox();
            reconfigure(geometry.width, geometry.height);
        });
        on_configure.connect(&xw->events.request_configure);

        on_set_geometry.set_callback([&] (void*)
        {
            /* Xwayland O-R views manage their position on their own. So we need to
             * update their position on each commit, if the position changed. */
            auto offset = (get_output() ?
                wf::origin(get_output()->get_layout_geometry()) : wf::point_t{0, 0});
            auto global_pos = this->origin + offset;
            auto client_pos = wf::point_t{xw->x, xw->y};

            if (global_pos != client_pos)
            {
                origin = client_pos + -offset;
                update_bbox();
            }
        });

        on_set_geometry.connect(&xw->events.set_geometry);
    }

    void map() final
    {
        /* move to the output where our center is
         * FIXME: this is a bad idea, because a dropdown menu might get sent to
         * an incorrect output. However, no matter how we calculate the real
         * output, we just can't be 100% compatible because in X all windows are
         * positioned in a global coordinate space */
        auto wo = wf::get_core().output_layout->get_output_at(
            xw->x + xw->surface->current.width / 2,
            xw->y + xw->surface->current.height / 2);

        if (!wo)
        {
            /* if surface center is outside of anything, try to check the output
             * where the pointer is */
            auto gc = wf::get_core().get_cursor_position();
            wo = wf::get_core().output_layout->get_output_at(gc.x, gc.y);
        }

        if (!wo)
        {
            wo = wf::get_core().get_active_output();
        }

        assert(wo);

        auto real_output_geometry = wo->get_layout_geometry();
        this->origin = {
            xw->x - real_output_geometry.x,
            xw->y - real_output_geometry.y
        };

        if (wo != get_output())
        {
            if (get_output())
            {
                get_output()->workspace->remove_view(self());
            }

            set_output(wo);
        }

        update_bbox();
        get_output()->workspace->add_view(self(), wf::LAYER_UNMANAGED);
        wf::wlr_view_t::map();

        if (view_impl->desktop_surface->get_keyboard_focus().accepts_focus())
        {
            get_output()->focus_view(self(), true);
        }
    }

    void destroy() final
    {
        this->xw = nullptr;
        on_configure.disconnect();
        on_set_geometry.disconnect();
        wf::wlr_view_t::destroy();
    }
};

// Xwayland DnD view
static wayfire_view dnd_view;

/**
 * A view implementation for Drag'n'Drop Xwayland surfaces.
 * They are characterized by not being on any workspace or output, instead,
 * they currently use a special rendering path in render-manager.
 *
 * Xwayland DnD views do not have to do almost anything, but they need to make
 * sure to damage all outputs they are visible on when moving.
 */
class xwayland_dnd_view_t : public wf::wlr_view_t
{
    wlr_xwayland_surface *xw;

    // We listen for configure events on the Xwayland surface and adjust our
    // geometry accordingly.
    wf::wl_listener_wrapper on_configure;
    wf::wl_listener_wrapper on_set_geometry;

    wf::geometry_t last_global_bbox = {0, 0, 0, 0};

    // Apply damage from last and new bounding box
    void do_damage()
    {
        if (!xw)
        {
            return;
        }

        wf::geometry_t bbox {
            xw->x,
            xw->y,
            xw->width,
            xw->height
        };

        for (auto& output : wf::get_core().output_layout->get_outputs())
        {
            auto local_bbox = bbox + -wf::origin(output->get_layout_geometry());
            output->render->damage(local_bbox);
            local_bbox = last_global_bbox +
                -wf::origin(output->get_layout_geometry());
            output->render->damage(local_bbox);
        }

        last_global_bbox = bbox;
    }

  public:
    xwayland_dnd_view_t(
        wf::surface_sptr_t main_surface,
        wf::dsurface_sptr_t dsurface,
        wlr_xwayland_surface *xw) : xw(xw)
    {
        this->set_main_surface(main_surface);
        this->set_desktop_surface(dsurface);


        on_configure.set_callback([&] (void *data)
        {
            auto ev = static_cast<wlr_xwayland_surface_configure_event*>(data);
            wlr_xwayland_surface_configure(xw,
                ev->x, ev->y, ev->width, ev->height);
            do_damage();
        });
        on_configure.connect(&xw->events.request_configure);

        on_set_geometry.set_callback([&] (void*)
        {
            do_damage();
        });
        on_set_geometry.connect(&xw->events.set_geometry);
    }

    void map() final
    {
        dynamic_cast<wf::wlr_surface_base_t*>(get_main_surface().get())->map();
        do_damage();

        // Do nothing else, wlr_view_t::map() contains code for regular views,
        // not DnD icons, so no call to parent map()
    }

    void unmap() final
    {
        // We literally do not do anything here, but we still need to override
        // the wlr_view_t unmap() function as it contains some helper code used
        // by the other views, but is not necessary here.
    }

    void destroy() final
    {
        this->xw = nullptr;
        on_configure.disconnect();
        on_set_geometry.disconnect();

        LOGD("Destroying a Xwayland drag icon");
        if (dnd_view.get() == this)
        {
            dnd_view = nullptr;
        }

        wf::wlr_view_t::destroy();
    }
};

/**
 * A per-xwayland-surface object.
 *
 * The view controller has several purposes:
 * - Manage the view implementation for the Xwayland surface.
 *   Different Xwayland surfaces require different view implementations, for
 *   example unmanaged, toplevel, etc.
 *   The controller tracks the current view type and if the Xwayland surface
 *   changes its type, it destroys the old view and creates a new view with
 *   the correct implementation.
 *
 * - Track mapped/unmapped state and propagate this to the view implementation.
 */
class xwayland_view_controller_t
{
    wf::surface_sptr_t main_surface;
    wf::dsurface_sptr_t dsurface;
    wlr_xwayland_surface *xw;

    wf::wl_listener_wrapper on_map;
    wf::wl_listener_wrapper on_unmap;
    wf::wl_listener_wrapper on_or_changed;
    wf::wl_listener_wrapper on_set_window_type;
    wf::wl_listener_wrapper on_destroy;
    wf::wl_listener_wrapper on_set_parent;

    // FIXME: plugins might destroy the view, we need to listen for this case.
    // There are currently no plugins which will do this, but it needs to be
    // fixed sometime.
    window_type_t current_type;
    wf::wlr_view_t *current_view = NULL;
    bool is_mapped = false;

  public:
    xwayland_view_controller_t(wlr_xwayland_surface *xw)
    {
        this->main_surface = std::make_shared<xw::xwayland_surface_t>(xw);
        this->dsurface = std::make_shared<wf::xwayland_desktop_surface_t>(xw);

        on_map.set_callback([&] (void*)
        {
            this->is_mapped = true;
            if (current_view)
            {
                current_view->map();
            }
        });
        on_unmap.set_callback([&] (void*)
        {
            this->is_mapped = false;
            if (current_view)
            {
                current_view->unmap();
            }
        });

        on_destroy.set_callback([&] (void*) { destroy(); });
        on_or_changed.set_callback([&] (void*)
        {
            recreate_view_if_necessary();
        });
        on_set_window_type.set_callback([&] (void*)
        {
            recreate_view_if_necessary();
        });

        on_map.connect(&xw->events.map);
        on_unmap.connect(&xw->events.unmap);
        on_destroy.connect(&xw->events.destroy);
        on_or_changed.connect(&xw->events.set_override_redirect);
        on_set_window_type.connect(&xw->events.set_window_type);

        recreate_view_if_necessary();
    }

    void recreate_view_if_necessary()
    {
        auto actual_type = xw::get_window_type(xw);
        if (current_view && (actual_type == current_type))
        {
            // We have a view and the type hasn't changed => Nothing to do.
            return;
        }

        // Step 1: destroy old view
        if (current_view)
        {
            if (is_mapped)
            {
                current_view->unmap();
            }

            current_view->destroy();
            current_view->unref();
            // At this point, current_view might have been freed already
            current_view = NULL;
        }

        std::unique_ptr<wf::wlr_view_t> view;
        switch (actual_type)
        {
          case window_type_t::DND:
            view = std::make_unique<xw::xwayland_dnd_view_t>(main_surface, dsurface, xw);
            dnd_view = {view};
            break;
          case window_type_t::OR:
            view = std::make_unique<xw::xwayland_or_view_t>(main_surface, dsurface, xw);
            break;
          case window_type_t::DIALOG:
            // fallthrough
          case window_type_t::TOPLEVEL:
            view = std::make_unique<xw::xwayland_toplevel_view_t>(main_surface, dsurface, xw);
            break;
        }

        this->current_view = view.get();
        this->current_type = actual_type;
        wf::get_core().add_view(std::move(view));
        if (is_mapped)
        {
            current_view->map();
        }
    }

    void destroy()
    {
        this->xw = nullptr;

        on_map.disconnect();
        on_unmap.disconnect();
        on_destroy.disconnect();
        on_or_changed.disconnect();
        on_set_window_type.disconnect();

        // Do not leak the controller, as there is nothing to destroy it.
        // The last view remaining (if any) will be freed by core when its
        // refcount drops to 0.
        delete this;
    }
};
}
}

static wlr_xwayland *xwayland_handle = nullptr;
#endif

void wf::init_xwayland()
{
#if WF_HAS_XWAYLAND
    static wf::wl_listener_wrapper on_created;
    static wf::wl_listener_wrapper on_ready;

    static signal_connection_t on_shutdown{[&] (void*)
        {
            wlr_xwayland_destroy(xwayland_handle);
        }
    };

    on_created.set_callback([] (void *data)
    {
        auto xsurf = (wlr_xwayland_surface*)data;
        // The created object will delete itself once the xwayland surface dies
        new wf::xw::xwayland_view_controller_t(xsurf);
    });

    on_ready.set_callback([&] (void *data)
    {
        if (!wf::xw::load_atoms(xwayland_handle->display_name))
        {
            LOGE("Failed to load Xwayland atoms.");
        } else
        {
            LOGD("Successfully loaded Xwayland atoms.");
        }

        wlr_xwayland_set_seat(xwayland_handle,
            wf::get_core().get_current_seat());
        xwayland_update_default_cursor();
    });

    xwayland_handle = wlr_xwayland_create(wf::get_core().display,
        wf::get_core_impl().compositor, false);

    if (xwayland_handle)
    {
        on_created.connect(&xwayland_handle->events.new_surface);
        on_ready.connect(&xwayland_handle->events.ready);
        wf::get_core().connect_signal("shutdown", &on_shutdown);
    }

#endif
}

void wf::xwayland_update_default_cursor()
{
#if WF_HAS_XWAYLAND
    if (!xwayland_handle)
    {
        return;
    }

    auto xc     = wf::get_core_impl().seat->cursor->xcursor;
    auto cursor = wlr_xcursor_manager_get_xcursor(xc, "left_ptr", 1);
    if (cursor && (cursor->image_count > 0))
    {
        auto image = cursor->images[0];
        wlr_xwayland_set_cursor(xwayland_handle, image->buffer,
            image->width * 4, image->width, image->height,
            image->hotspot_x, image->hotspot_y);
    }

#endif
}

void wf::xwayland_bring_to_front(wlr_surface *surface)
{
#if WF_HAS_XWAYLAND
    if (wlr_surface_is_xwayland_surface(surface))
    {
        auto xw = wlr_xwayland_surface_from_wlr_surface(surface);
        wlr_xwayland_surface_restack(xw, NULL, XCB_STACK_MODE_ABOVE);
    }

#endif
}

std::string wf::xwayland_get_display()
{
#if WF_HAS_XWAYLAND

    return xwayland_handle ? nonull(xwayland_handle->display_name) : "";
#else

    return "";
#endif
}

wayfire_view wf::get_xwayland_drag_icon()
{
#if WF_HAS_XWAYLAND
    if (xw::dnd_view && xw::dnd_view->is_mapped())
    {
        return xw::dnd_view.get();
    }

#endif

    return nullptr;
}
