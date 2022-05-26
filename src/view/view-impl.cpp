#include "wayfire/core.hpp"
#include "../core/core-impl.hpp"
#include "../output/gtk-shell.hpp"
#include "view-impl.hpp"
#include "wayfire/decorator.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/workspace-manager.hpp"
#include <wayfire/util/log.hpp>
#include <wayfire/debug.hpp>

void wf::emit_view_signal(wf::view_interface_t *view,
    std::string_view signal_name, wf::signal_data_t *data)
{
    view->emit_signal(std::string(signal_name), data);
    if (view->get_output())
    {
        view->get_output()->emit_signal(
            "view-" + std::string(signal_name), data);
    }
}

wf::geometry_t wf::adjust_geometry_for_gravity(wf::geometry_t wmg,
    uint32_t edges, wf::dimensions_t new_size)
{
    if (edges & WLR_EDGE_LEFT)
    {
        wmg.x += wmg.width - new_size.width;
    }

    if (edges & WLR_EDGE_TOP)
    {
        wmg.y += wmg.height - new_size.height;
    }

    wmg.width  = new_size.width;
    wmg.height = new_size.height;
    return wmg;
}

wf::geometry_t wf::shrink_by_margins(
    wf::geometry_t g, wf::decoration_margin_t margin)
{
    g.x     += margin.left;
    g.y     += margin.top;
    g.width -= margin.left + margin.right;
    g.height -= margin.top + margin.bottom;
    return g;
}

wf::geometry_t wf::expand_with_margins(
    wf::geometry_t g, wf::decoration_margin_t margin)
{
    g.x     -= margin.left;
    g.y     -= margin.top;
    g.width += margin.left + margin.right;
    g.height += margin.top + margin.bottom;
    return g;
}

void wf::wlr_view_t::update_bbox()
{
    view_damage_raw({this}, last_bounding_box);

    wf::view_bbox_changed_signal data;
    data.old_bbox     = last_bounding_box;
    last_bounding_box = get_bounding_box();
    wf::emit_view_signal(this, "bounding-box-changed", &data);

    damage();
}

void wf::wlr_view_t::setup_toplevel_tracking()
{
    assert(view_impl->toplevel);

    on_toplevel_geometry_changed.set_callback([&] (wf::signal_data_t *data)
    {
        update_bbox();
    });
    view_impl->toplevel->connect_signal("geometry-changed",
        &on_toplevel_geometry_changed);
}

wf::point_t wf::wlr_view_t::get_origin()
{
    if (view_impl->toplevel)
    {
        return wf::origin(view_impl->toplevel->current().base_geometry);
    }

    return origin;
}

bool wf::wlr_view_t::is_mapped() const
{
    return mapped;
}

/* if the view isn't mapped, then it will be positioned properly in map() */
if (is_mapped())
{
    reposition_relative_to_parent(self());
}

static void reposition_relative_to_parent(wayfire_view view)
{
    if (!view->parent)
    {
        return;
    }

    auto parent_geometry = view->parent->get_wm_geometry();
    auto wm_geometry     = view->get_wm_geometry();
    auto scr_size = view->get_output()->get_screen_size();
    // Guess which workspace the parent is on
    wf::point_t center = {
        parent_geometry.x + parent_geometry.width / 2,
        parent_geometry.y + parent_geometry.height / 2,
    };
    wf::point_t parent_ws = {
        (int)std::floor(1.0 * center.x / scr_size.width),
        (int)std::floor(1.0 * center.y / scr_size.height),
    };

    auto workarea = view->get_output()->render->get_ws_box(
        view->get_output()->workspace->get_current_workspace() + parent_ws);
    if (view->parent->is_mapped())
    {
        auto parent_g = view->parent->get_wm_geometry();
        wm_geometry.x = parent_g.x + (parent_g.width - wm_geometry.width) / 2;
        wm_geometry.y = parent_g.y + (parent_g.height - wm_geometry.height) / 2;
    } else
    {
        /* if we have a parent which still isn't mapped, we cannot determine
         * the view's position, so we center it on the screen */
        wm_geometry.x = workarea.width / 2 - wm_geometry.width / 2;
        wm_geometry.y = workarea.height / 2 - wm_geometry.height / 2;
    }

    /* make sure view is visible afterwards */
    wm_geometry = wf::clamp(wm_geometry, workarea);
    view->move(wm_geometry.x, wm_geometry.y);
    if (wf::dimensions(wm_geometry) != wf::dimensions_t(wm_geometry))
    {
        view->resize(wm_geometry.width, wm_geometry.height);
    }
}

wf::wlr_view_t::wlr_view_t() : wf::view_interface_t(nullptr)
{}

void wf::wlr_view_t::set_position(wf::point_t point)
{
    this->origin = point;
    update_bbox();
}

void wf::wlr_view_t::map()
{
    this->mapped = true;
    // FIXME: for surfaces, mapped/unmapped state doesn't really matter,
    // this is a toplevel construct. As a result, surfaces shouldn't really
    // have a mapped state and the next line shouldn't be necessary ...
    dynamic_cast<wf::wlr_surface_base_t*>(get_main_surface().get())->map();
    if (dsurf()->get_role() == desktop_surface_t::role::TOPLEVEL)
    {
        if (!parent)
        {
            get_output()->workspace->add_view(self(), wf::LAYER_WORKSPACE);
        }

        get_output()->focus_view(self(), true);
    }

    damage();
    this->emit_map();
    /* Might trigger repositioning */
    set_toplevel_parent(this->parent);
}

void wf::wlr_view_t::unmap()
{
    damage();
    emit_view_pre_unmap(self());
    this->mapped = false;
    dynamic_cast<wf::wlr_surface_base_t*>(get_main_surface().get())->unmap();
    emit_view_unmap(self());
}

void wf::emit_view_map_signal(wayfire_view view, bool has_position)
{
    wf::view_mapped_signal data;
    data.view = view;
    data.is_positioned = has_position;
    view->get_output()->emit_signal("view-mapped", &data);
    view->emit_signal("mapped", &data);
}

void wf::emit_ping_timeout_signal(desktop_surface_t *dsurface)
{
    wf::dsurface_ping_timeout_signal data;
    data.dsurface = dsurface;
    dsurface->emit_signal("ping-timeout", &data);
}

void wf::wlr_view_t::emit_map()
{
    emit_view_map_signal(self(), false);
}

void wf::emit_view_map(wayfire_view view)
{
    emit_view_map_signal(view, false);
}

void wf::emit_view_unmap(wayfire_view view)
{
    view_unmapped_signal data;
    data.view = view;

    if (view->get_output())
    {
        view->get_output()->emit_signal("view-unmapped", &data);
        view->get_output()->emit_signal("view-disappeared", &data);
    }

    view->emit_signal("unmapped", &data);
}

void wf::emit_view_pre_unmap(wayfire_view view)
{
    view_pre_unmap_signal data;
    data.view = view;

    if (view->get_output())
    {
        view->get_output()->emit_signal("view-pre-unmapped", &data);
    }

    view->emit_signal("pre-unmapped", &data);
}

void wf::wlr_view_t::destroy()
{
    view_impl->is_alive = false;
    /* Drop the internal reference created in surface_interface_t */
    unref();
}

wf::point_t wf::wlr_view_t::get_window_offset()
{
    return {0, 0};
}

void wf::init_desktop_apis()
{
    init_xdg_shell();
    init_layer_shell();

    wf::option_wrapper_t<bool> xwayland_enabled("core/xwayland");
    if (xwayland_enabled == 1)
    {
        init_xwayland();
    }
}

wf::surface_interface_t*wf::wf_surface_from_void(void *handle)
{
    return static_cast<wf::surface_interface_t*>(handle);
}

wayfire_view wf::wl_surface_to_wayfire_view(wl_resource *resource)
{
    auto surface = (wlr_surface*)wl_resource_get_user_data(resource);

    void *handle = NULL;
    if (wlr_surface_is_xdg_surface(surface))
    {
        handle = wlr_xdg_surface_from_wlr_surface(surface)->data;
    }

    if (wlr_surface_is_layer_surface(surface))
    {
        handle = wlr_layer_surface_v1_from_wlr_surface(surface)->data;
    }

#if WF_HAS_XWAYLAND
    if (wlr_surface_is_xwayland_surface(surface))
    {
        handle = wlr_xwayland_surface_from_wlr_surface(surface)->data;
    }

#endif

    wf::view_interface_t *view = static_cast<wf::view_interface_t*>(handle);
    return view ? view->self() : nullptr;
}
