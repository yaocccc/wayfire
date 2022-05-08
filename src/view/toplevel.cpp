#include <wayfire/toplevel-helpers.hpp>
#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>

#include "view-impl.hpp"

wayfire_view wf::toplevel_find_primary_view(wf::optr<wf::toplevel_t> toplevel)
{
    auto views = wf::get_core().find_views_with_toplevel(toplevel.get());
    if (views.empty())
    {
        return nullptr;
    }

    using rank_t = std::tuple<int, uint32_t, wayfire_view>;
    std::vector<rank_t> values;
    std::transform(views.begin(), views.end(), std::back_inserter(values),
        [&] (wayfire_view view)
    {
        return rank_t{
            view->get_output() == toplevel->current().primary_output,
            view->get_id(),
            view
        };
    });

    auto [x, y, best_candidate] = *std::max_element(values.begin(), values.end());
    return best_candidate;
}

void wf::toplevel_saved_geometry_t::store_geometry(wf::optr<wf::toplevel_t> tl)
{
    auto& ts = tl->current();
    if (!ts.is_mapped || ts.tiled_edges || ts.fullscreen ||
        tl->is_moving() || tl->is_resizing())
    {
        // Nothing to do here
        return;
    }

    this->last_windowed_geometry = ts.geometry;
    if (ts.primary_output)
    {
        this->saved_workarea =
            ts.primary_output->workspace->get_workarea();
    }
}

std::optional<wf::geometry_t> wf::toplevel_saved_geometry_t::calculate_geometry(
    const wf::geometry_t& workarea)
{
    if (!last_windowed_geometry.has_value() || !saved_workarea.has_value())
    {
        return last_windowed_geometry;
    }

    const auto& geom     = last_windowed_geometry.value();
    const auto& old_area = saved_workarea.value();
    const auto& relpos   = wf::origin(geom) + -wf::origin(old_area);

    return wf::geometry_t{
        .x     = workarea.x + relpos.x * workarea.width / old_area.width,
        .y     = workarea.y + relpos.y * workarea.height / old_area.height,
        .width = geom.width * workarea.width / old_area.width,
        .height = geom.height * workarea.height / old_area.height
    };
}

void wf::toplevel_emit_move_request(wf::optr<wf::toplevel_t> toplevel)
{
    toplevel_move_request_signal data;
    data.toplevel = toplevel;
    emit_toplevel_signal(toplevel.get(), "move-request", &data);
}

void wf::toplevel_emit_resize_request(wf::optr<wf::toplevel_t> toplevel,
    uint32_t edges)
{
    toplevel_resize_request_signal data;
    data.toplevel = toplevel;
    data.edges    = edges;
    emit_toplevel_signal(toplevel.get(), "resize-request", &data);
}

static void do_minimize(wf::optr<wf::toplevel_t> toplevel)
{
    auto views = wf::get_core().find_views_with_toplevel(toplevel.get());
    for (auto& v : views)
    {
        wf::view_disappeared_signal data;
        data.view = v;
        v->get_output()->emit_signal("view-disappeared", &data);
        v->get_output()->workspace->add_view(v, wf::LAYER_MINIMIZED);
    }
}

static void do_restore(wf::optr<wf::toplevel_t> toplevel)
{
    auto views = wf::get_core().find_views_with_toplevel(toplevel.get());
    for (auto& v : views)
    {
        v->get_output()->workspace->add_view(v, wf::LAYER_WORKSPACE);
        v->get_output()->focus_view(v, true);
    }
}

void wf::toplevel_emit_minimize_request(wf::optr<wf::toplevel_t> toplevel,
    bool minimized)
{
    if (toplevel->current().minimized == minimized)
    {
        // nothing to do anyway
        return;
    }

    toplevel_minimize_request_signal data;
    data.toplevel = toplevel;
    data.state    = minimized;
    emit_toplevel_signal(toplevel.get(), "minimize-request", &data);

    /* Some plugin (e.g animate) will take care of the request, so we need
     * to just send proper state to foreign-toplevel clients */
    if (!data.carried_out)
    {
        if (minimized)
        {
            do_minimize(toplevel);
        } else
        {
            do_restore(toplevel);
        }

        toplevel->set_minimized(minimized);
    }
}

/**
 * Put a view on the given workspace.
 */
static void move_to_workspace(wf::optr<wf::toplevel_t> toplevel,
    wf::point_t workspace)
{
    auto output = toplevel->pending().primary_output;
    auto wm_geometry = toplevel->pending().geometry;
    auto delta    = workspace - output->workspace->get_current_workspace();
    auto scr_size = output->get_screen_size();

    wm_geometry.x += scr_size.width * delta.x;
    wm_geometry.y += scr_size.height * delta.y;

    toplevel->set_geometry(wm_geometry);
}

void wf::toplevel_emit_tile_request(wf::optr<wf::toplevel_t> toplevel,
    uint32_t tiled_edges, std::optional<wf::point_t> ws)
{
    auto wo = toplevel->pending().primary_output;
    auto ps = toplevel->pending();
    if (!ps.is_mapped || ps.fullscreen || !wo)
    {
        return;
    }

    toplevel_tile_request_signal data;
    data.toplevel  = toplevel;
    data.edges     = tiled_edges;
    data.workspace = ws.value_or(wo->workspace->get_current_workspace());
    if (tiled_edges)
    {
        data.desired_size = wo->workspace->get_workarea();
    } else
    {
        auto tsg = toplevel->get_data_safe<toplevel_saved_geometry_t>();
        data.desired_size = tsg->calculate_geometry(wo->workspace->get_workarea());
    }

    ps.primary_output->emit_signal("toplevel-tile-request", &data);
    if (!data.carried_out)
    {
        if (data.desired_size.has_value())
        {
            toplevel->set_geometry(data.desired_size.value());
        } else
        {
            toplevel->request_native_size();
        }

        move_to_workspace(toplevel, data.workspace);
    }
}

void wf::toplevel_emit_fullscreen_request(wf::optr<wf::toplevel_t> toplevel,
    wf::output_t *output, bool state, std::optional<wf::point_t> ws)
{
    auto ps = toplevel->current();
    if (!ps.is_mapped || (ps.fullscreen == state))
    {
        return;
    }

    auto desired_output =
        (output ?: (ps.primary_output ?: wf::get_core().get_active_output()));

    /* TODO: what happens if the view is moved to the other output, but not
     * fullscreened? We should make sure that it stays visible there */
    if (ps.primary_output != desired_output)
    {
        auto view = toplevel_find_primary_view(toplevel);
        wf::get_core().move_view_to_output(view, desired_output, false);
    }

    toplevel_fullscreen_request_signal data;
    data.toplevel  = toplevel;
    data.state     = state;
    data.workspace =
        ws.value_or(desired_output->workspace->get_current_workspace());

    if (state)
    {
        data.desired_size = desired_output->get_relative_geometry();
    } else if (ps.tiled_edges)
    {
        data.desired_size = desired_output->workspace->get_workarea();
    } else
    {
        auto tsg = toplevel->get_data_safe<toplevel_saved_geometry_t>();
        data.desired_size =
            tsg->calculate_geometry(desired_output->workspace->get_workarea());
    }

    desired_output->emit_signal("toplevel-fullscreen-request", &data);
    if (!data.carried_out)
    {
        if (data.desired_size.has_value())
        {
            toplevel->set_geometry(data.desired_size.value());
        } else
        {
            toplevel->request_native_size();
        }

        move_to_workspace(toplevel, data.workspace);
    }
}

void wf::emit_toplevel_signal(wf::toplevel_t *toplevel,
    std::string_view signal_name, wf::signal_data_t *data)
{
    toplevel->emit_signal(std::string(signal_name), data);
    if (toplevel->current().primary_output)
    {
        toplevel->current().primary_output->emit_signal(
            "toplevel-" + std::string(signal_name), data);
    }

    wf::get_core().emit_signal("toplevel-" + std::string(signal_name), data);
}

void wf::view_interface_t::focus_request()
{
    if (get_output())
    {
        view_focus_request_signal data;
        data.view = self();
        data.self_request = false;

        emit_signal("view-focus-request", &data);
        wf::get_core().emit_signal("view-focus-request", &data);
        if (!data.carried_out)
        {
            wf::get_core().focus_output(get_output());
            get_output()->ensure_visible(self());
            get_output()->focus_view(self(), true);
        }
    }
}

bool wf::view_interface_t::should_be_decorated()
{
    return false;
}

nonstd::observer_ptr<wf::decorator_frame_t_t> wf::view_interface_t::get_decoration()
{
    return this->view_impl->frame.get();
}

void wf::view_interface_t::set_decoration(
    std::unique_ptr<wf::decorator_frame_t_t> frame)
{
    if (!frame)
    {
        damage();

        // Take wm geometry as it was with the decoration.
        const auto wm = get_wm_geometry();

        // Drop the owned frame.
        view_impl->frame = nullptr;

        // Grow the tiled view to fill its old expanded geometry that included
        // the decoration.
        if (!fullscreen && this->tiled_edges && (wm != get_wm_geometry()))
        {
            set_geometry(wm);
        }

        emit_signal("decoration-changed", nullptr);
        return;
    }

    // Take wm geometry as it was before adding the frame */
    auto wm = get_wm_geometry();

    damage();
    // Drop the old frame if any and assign the new one.
    view_impl->frame = std::move(frame);

    /* Calculate the wm geometry of the view after adding the decoration.
     *
     * If the view is neither maximized nor fullscreen, then we want to expand
     * the view geometry so that the actual view contents retain their size.
     *
     * For fullscreen and maximized views we want to "shrink" the view contents
     * so that the total wm geometry remains the same as before. */
    wf::geometry_t target_wm_geometry;
    if (!fullscreen && !this->tiled_edges)
    {
        target_wm_geometry = view_impl->frame->expand_wm_geometry(wm);
        // make sure that the view doesn't go outside of the screen or such
        auto wa = get_output()->workspace->get_workarea();
        auto visible = wf::geometry_intersection(target_wm_geometry, wa);
        if (visible != target_wm_geometry)
        {
            target_wm_geometry.x = wm.x;
            target_wm_geometry.y = wm.y;
        }
    } else if (fullscreen)
    {
        target_wm_geometry = get_output()->get_relative_geometry();
    } else if (this->tiled_edges)
    {
        target_wm_geometry = get_output()->workspace->get_workarea();
    }

    // notify the frame of the current size
    view_impl->frame->notify_view_resized(get_wm_geometry());
    // but request the target size, it will be sent to the frame on the
    // next commit
    set_geometry(target_wm_geometry);
    damage();

    emit_signal("decoration-changed", nullptr);
}
