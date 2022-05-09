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
#include "xwayland-helpers.hpp"

#if WF_HAS_XWAYLAND

namespace wf
{
namespace xw
{
class view_base_t : public wf::wlr_view_t
{
  public:
    virtual window_type_t get_current_type() = 0;
};

/**
 * Check whether the view's classification is different than its current
 * implementation type.
 * If yes, the view is destroyed and a new view is created with the correct
 * implementation type.
 */
bool check_recreate_view(view_base_t *base, wlr_xwayland_surface *xw);

class toplevel_view_t : public view_base_t
{
    wf::wl_listener_wrapper on_set_parent;
    wf::wl_listener_wrapper on_set_hints;
    wlr_xwayland_surface *xw;

  public:
    toplevel_view_t(wlr_xwayland_surface *xww) : view_base_t()
    {
        on_set_parent.set_callback([&] (void*)
        {
            /* Menus, etc. with TRANSIENT_FOR but not dialogs */
            if (check_recreate_view(this, this->xw))
            {
                return;
            }

            auto parent = xw->parent ?
                static_cast<wf::view_interface_t*>(xw->parent->data)->self() : nullptr;

            // Make sure the parent is mapped, and that we are not a toplevel view
            if (parent &&
                (!parent->is_mapped() ||
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

    virtual void destroy() override
    {
        on_set_parent.disconnect();
        view_base_t::destroy();
    }

    void emit_map() override
    {
        /* Some X clients position themselves on map, and others let the window
         * manager determine this. We try to heuristically guess which of the
         *  bool client_self_positioned = self_positioned;
         *  emit_view_map_signal(self(), client_self_positioned);
         *  }
         *
         *  void map() override
         *  {
         *  if (xw->maximized_horz && xw->maximized_vert)
         *  {
         *   if ((xw->width > 0) && (xw->height > 0))
         *   {
         *       /* Save geometry which the window has put itself in */
        wf::geometry_t save_geometry = {
            xw->x, xw->y, xw->width, xw->height
        };

        /* Make sure geometry is properly visible on the view output */
        save_geometry = wf::clamp(save_geometry,
            get_output()->workspace->get_workarea());
        view_impl->update_windowed_geometry(self(), save_geometry);
    }

};

    tile_request(wf::TILED_EDGES_ALL);
if (xw->fullscreen)
{
    fullscreen_request(get_output(), true);
}

if (!this->tiled_edges && !xw->fullscreen)
{
    configure_request({xw->x, xw->y, xw->width, xw->height});
}

wf::wlr_view_t::map();
}

void commit() override
{
    if (!xw->has_alpha)
    {
        pixman_region32_union_rect(
            &xw->surface->opaque_region, &xw->surface->opaque_region,
            0, 0, xw->surface->current.width, xw->surface->current.height);
    }

    wf::wlr_view_t::commit();
}

xwayland_view_type_t get_current_impl_type() const override
{
    return xwayland_view_type_t::NORMAL;
}
}
}
}



class wayfire_xwayland_view_base : public wf::wlr_view_t
{
  protected:
    wf::wl_listener_wrapper on_destroy;
    wf::wl_listener_wrapper on_map;
    wf::wl_listener_wrapper on_unmap;
    wf::wl_listener_wrapper on_or_changed;
    wf::wl_listener_wrapper on_set_window_type;

    wlr_xwayland_surface *xw;

    /**
     * Get the current implementation type.
     */
    virtual wf::xw::window_type_t get_current_impl_type() const = 0;

  public:
    wayfire_xwayland_view_base(wlr_xwayland_surface *xww) :
        wlr_view_t(), xw(xww)
    {
        auto surf = std::make_shared<wf::wlr_surface_base_t>(xww->surface);
        this->set_main_surface(surf);

        on_map.set_callback([&] (void*) { map(); });
        on_unmap.set_callback([&] (void*) { unmap(); });
        on_destroy.set_callback([&] (void*) { destroy(); });
    }

    virtual void initialize() override
    {
        wf::wlr_view_t::initialize();
        on_or_changed.set_callback([&] (void*)
        {
            recreate_view();
        });
        on_set_window_type.set_callback([&] (void*)
        {
            recreate_view();
        });

        on_map.connect(&xw->events.map);
        on_unmap.connect(&xw->events.unmap);
        on_destroy.connect(&xw->events.destroy);
        on_configure.connect(&xw->events.request_configure);
        on_or_changed.connect(&xw->events.set_override_redirect);
        on_set_window_type.connect(&xw->events.set_window_type);
    }

    /**
     * Destroy the view, and create a new one with the correct type -
     * unmanaged(override-redirect), DnD or normal.
     *
     * No-op if the view already has the correct type.
     */
    virtual void recreate_view();

    virtual void destroy() override
    {
        this->xw = nullptr;
        output_geometry_changed.disconnect();

        on_map.disconnect();
        on_unmap.disconnect();
        on_destroy.disconnect();
        on_configure.disconnect();
        on_or_changed.disconnect();
        on_set_window_type.disconnect();

        wf::wlr_view_t::destroy();
    }

    /* Translates geometry from X client configure requests to wayfire
     * coordinate system. The X coordinate system treats all outputs
     * as one big desktop, whereas wayfire treats the current workspace
     * of an output as 0,0 and everything else relative to that. This
     * means that we must take care when placing xwayland clients that
     * request a configure after initial mapping, while not on the
     * current workspace.
     *
     * @param output    The view's output
     * @param ws_offset The view's workspace minus the current workspace
     * @param geometry  The configure geometry as requested by the client
     *
     * @return Geometry with a position that is within the view's workarea.
     * The workarea is the workspace where the view was initially mapped.
     * Newly mapped views are placed on the current workspace.
     */
    wf::geometry_t translate_geometry_to_output(wf::output_t *output,
        wf::point_t ws_offset,
        wf::geometry_t g)
    {
        auto outputs = wf::get_core().output_layout->get_outputs();
        auto og = output->get_layout_geometry();
        auto from = wf::get_core().output_layout->get_output_at(
            g.x + g.width / 2 + og.x, g.y + g.height / 2 + og.y);
        if (!from)
        {
            return g;
        }

        auto lg = from->get_layout_geometry();
        g.x += (og.x - lg.x) + ws_offset.x * og.width;
        g.y += (og.y - lg.y) + ws_offset.y * og.height;
        if (!this->is_mapped())
        {
            g.x *= (float)og.width / lg.width;
            g.y *= (float)og.height / lg.height;
        }

        return g;
    }

    virtual void configure_request(wf::geometry_t configure_geometry)
    {
        /* Wayfire positions views relative to their output, but Xwayland
         * windows have a global positioning. So, we need to make sure that we
         * always transform between output-local coordinates and global
         * coordinates. Additionally, when clients send a configure request
         * after they have already been mapped, we keep the view on the
         * workspace where its center point was from last configure, in
         * case the current workspace is not where the view lives */
        auto o = get_output();
        if (o)
        {
            auto view_workarea = (fullscreen ?
                o->get_relative_geometry() : o->workspace->get_workarea());
            auto og = o->get_layout_geometry();
            configure_geometry.x -= og.x;
            configure_geometry.y -= og.y;

            auto view = this->self();
            while (view->parent)
            {
                view = view->parent;
            }

            auto vg = view->get_wm_geometry();

            // View workspace relative to current workspace
            wf::point_t view_ws = {0, 0};
            if (view->is_mapped())
            {
                view_ws = {
                    (int)std::floor((vg.x + vg.width / 2.0) / og.width),
                    (int)std::floor((vg.y + vg.height / 2.0) / og.height),
                };

                view_workarea.x += og.width * view_ws.x;
                view_workarea.y += og.height * view_ws.y;
            }

            configure_geometry = translate_geometry_to_output(
                o, view_ws, configure_geometry);
            configure_geometry = wf::clamp(configure_geometry, view_workarea);
        }

        if (view_impl->frame)
        {
            configure_geometry =
                view_impl->frame->expand_wm_geometry(configure_geometry);
        }

        set_geometry(configure_geometry);
    }

    void set_geometry(wf::geometry_t geometry) override
    {
        wlr_view_t::move(geometry.x, geometry.y);
        resize(geometry.width, geometry.height);
    }

    virtual void set_output(wf::output_t *wo) override
    {
        output_geometry_changed.disconnect();
        wlr_view_t::set_output(wo);

        if (wo)
        {
            wo->connect_signal("output-configuration-changed",
                &output_geometry_changed);
        }

        /* Update the real position */
        if (is_mapped())
        {
            send_configure();
        }
    }
};

wayfire_unmanaged_xwayland_view::wayfire_unmanaged_xwayland_view(
    wlr_xwayland_surface *xww) :
    wayfire_xwayland_view_base(xww)
{
    on_configure.set_callback([&] (void *data)
    {
        auto ev = static_cast<wlr_xwayland_surface_configure_event*>(data);
        wf::point_t output_origin = {0, 0};
        if (get_output())
        {
            output_origin = {
                get_output()->get_relative_geometry().x,
                get_output()->get_relative_geometry().y
            };
        }

        if (!is_mapped())
        {
            /* If the view is not mapped yet, let it be configured as it
             * wishes. We will position it properly in ::map() */
            wlr_xwayland_surface_configure(xw,
                ev->x, ev->y, ev->width, ev->height);

            if ((ev->mask & XCB_CONFIG_WINDOW_X) &&
                (ev->mask & XCB_CONFIG_WINDOW_Y))
            {
                this->self_positioned = true;
                this->geometry.x = ev->x - output_origin.x;
                this->geometry.y = ev->y - output_origin.y;
            }

            return;
        }

        /**
         * Regular Xwayland windows are not allowed to change their position
         * after mapping, in which respect they behave just like Wayland apps.
         *
         * However, OR views or special views which do not have NORMAL type
         * should be allowed to move around the screen.
         */
        bool enable_custom_position = xw->override_redirect ||
            (xw->window_type_len > 0 &&
                xw->window_type[0] != _NET_WM_WINDOW_TYPE_NORMAL);

        if ((ev->mask & XCB_CONFIG_WINDOW_X) &&
            (ev->mask & XCB_CONFIG_WINDOW_Y) &&
            enable_custom_position)
        {
            /* override-redirect views generally have full freedom. */
            self_positioned = true;
            configure_request({ev->x, ev->y, ev->width, ev->height});

            return;
        }

        /* Use old x/y values */
        ev->x = geometry.x + output_origin.x;
        ev->y = geometry.y + output_origin.y;
        configure_request(wlr_box{ev->x, ev->y, ev->width, ev->height});
    });
    wf::signal_connection_t output_geometry_changed = [this] (wf::signal_data_t*)
    {
        if (is_mapped())
        {
            auto wm_geometry = get_wm_geometry();
            move(wm_geometry.x, wm_geometry.y);
        }
    }

        LOGE("new unmanaged xwayland surface ", xw->title, " class: ", xw->class_t,
        " instance: ", xw->instance);

    xw->data = this;
    on_set_geometry.set_callback([&] (void*)
    {
        /* Xwayland O-R views manage their position on their own. So we need to
         * update their position on each commit, if the position changed. */
        if ((global_x != xw->x) || (global_y != xw->y))
        {
            geometry.x = global_x = xw->x;
            geometry.y = global_y = xw->y;

            if (get_output())
            {
                auto real_output = get_output()->get_layout_geometry();
                geometry.x -= real_output.x;
                geometry.y -= real_output.y;
            }

            wf::wlr_view_t::move(geometry.x, geometry.y);
        }
    });

    on_set_geometry.connect(&xw->events.set_geometry);
}

void wayfire_unmanaged_xwayland_view::map()
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

    global_x = xw->x;
    global_y = xw->y;
    wf::wlr_view_t::move(xw->x - real_output_geometry.x,
        xw->y - real_output_geometry.y);

    if (wo != get_output())
    {
        if (get_output())
        {
            get_output()->workspace->remove_view(self());
        }

        set_output(wo);
    }

    damage();

    get_output()->workspace->add_view(self(), wf::LAYER_UNMANAGED);
    wf::wlr_view_t::map();

    if (dsurface->keyboard_focus_enabled)
    {
        get_output()->focus_view(self(), true);
    }
}

bool wayfire_unmanaged_xwayland_view::should_be_decorated()
{
    return (!xw->override_redirect && !this->has_client_decoration);
}

void wayfire_unmanaged_xwayland_view::destroy()
{
    on_set_geometry.disconnect();
    wayfire_xwayland_view_base::destroy();
}

// Xwayland DnD view
static wayfire_view dnd_view;

class wayfire_dnd_xwayland_view : public wayfire_unmanaged_xwayland_view
{
  protected:
    wf::wl_listener_wrapper on_set_geometry;

  public:
    using wayfire_unmanaged_xwayland_view::wayfire_unmanaged_xwayland_view;

    xwayland_view_type_t get_current_impl_type() const override
    {
        return xwayland_view_type_t::DND;
    }

    void destruct() override
    {
        LOGD("Destroying a Xwayland drag icon");
        if (dnd_view.get() == this)
        {
            dnd_view = nullptr;
        }

        wayfire_unmanaged_xwayland_view::destruct();
    }

    void deinitialize() override
    {
        wayfire_unmanaged_xwayland_view::deinitialize();
    }

    wf::geometry_t last_global_bbox = {0, 0, 0, 0};

    void damage() override
    {
        if (!get_output())
        {
            return;
        }

        auto bbox = get_bounding_box() +
            wf::origin(this->get_output()->get_layout_geometry());

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

    void map() override
    {
        LOGD("Mapping a Xwayland drag icon");
        this->set_output(wf::get_core().get_active_output());
        wayfire_xwayland_view_base::map();
        this->damage();
    }
};

void wayfire_xwayland_view_base::recreate_view()
{
    xwayland_view_type_t target_type = xwayland_view_type_t::NORMAL;
    if (this->is_dnd())
    {
        target_type = xwayland_view_type_t::DND;
    } else if (this->is_unmanaged())
    {
        target_type = xwayland_view_type_t::UNMANAGED;
    }

    if (target_type == this->get_current_impl_type())
    {
        // Nothing changed
        return;
    }

    /*
     * Copy xw and mapped status into the stack, because "this" may be destroyed
     * at some point of this function.
     */
    auto xw_surf = this->xw;
    bool was_mapped = is_mapped();

    // destroy the view (unmap + destroy)
    if (was_mapped)
    {
        unmap();
    }

    destroy();

    // Create the new view.
    // Take care! The new_view pointer is passed to core as unique_ptr
    wayfire_xwayland_view_base *new_view;
    switch (target_type)
    {
      case xwayland_view_type_t::DND:
        new_view = new wayfire_dnd_xwayland_view(xw_surf);
        ::dnd_view = new_view;
        break;

      case xwayland_view_type_t::UNMANAGED:
        new_view = new wayfire_unmanaged_xwayland_view(xw_surf);
        wf::get_core().add_view(std::unique_ptr<view_interface_t>(new_view));
        break;

      case xwayland_view_type_t::NORMAL:
        new_view = new wayfire_xwayland_view(xw_surf);
        break;
    }

    wf::get_core().add_view(std::unique_ptr<view_interface_t>(new_view));
    if (was_mapped)
    {
        new_view->map();
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
        if (xsurf->override_redirect)
        {
            wf::get_core().add_view(
                std::make_unique<wayfire_unmanaged_xwayland_view>(xsurf));
        } else
        {
            wf::get_core().add_view(
                std::make_unique<wayfire_xwayland_view>(xsurf));
        }
    });

    on_ready.set_callback([&] (void *data)
    {
        if (!wayfire_xwayland_view_base::load_atoms(xwayland_handle->display_name))
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

    auto xc = wf::get_core_impl().seat->cursor->xcursor;
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
    if (dnd_view && dnd_view->is_mapped() && dnd_view->get_output())
    {
        return dnd_view.get();
    }

#endif

    return nullptr;
}
