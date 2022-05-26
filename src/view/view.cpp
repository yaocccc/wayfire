#include <wayfire/util/log.hpp>
#include "../core/core-impl.hpp"
#include "view-impl.hpp"
#include "wayfire/opengl.hpp"
#include "wayfire/output.hpp"
#include "wayfire/view.hpp"
#include "wayfire/view-transform.hpp"
#include "wayfire/workspace-manager.hpp"
#include "wayfire/render-manager.hpp"
#include "../output/gtk-shell.hpp"

#include <algorithm>
#include <glm/glm.hpp>
#include "wayfire/signal-definitions.hpp"

static void unset_toplevel_parent(wayfire_view view)
{
    if (view->parent)
    {
        auto& container = view->parent->children;
        auto it = std::remove(container.begin(), container.end(), view);
        container.erase(it, container.end());
    }
}

wayfire_view wf::find_toplevel_parent(wayfire_view view)
{
    while (view->parent)
    {
        view = view->parent;
    }

    return view;
}

/**
 * Check whether the toplevel parent needs refocus.
 * This may be needed because when focusing a view, its topmost child is given
 * keyboard focus. When the parent-child relations change, it may happen that
 * the parent needs to be focused again, this time with a different keyboard
 * focus surface.
 */
static void check_refocus_parent(wayfire_view view)
{
    view = find_toplevel_parent(view);
    if (view->get_output() && (view->get_output()->get_active_view() == view))
    {
        view->get_output()->focus_view(view, false);
    }
}

void wf::view_interface_t::set_toplevel_parent(wayfire_view new_parent)
{
    auto old_parent = parent;
    if (parent != new_parent)
    {
        /* Erase from the old parent */
        unset_toplevel_parent(self());

        /* Add in the list of the new parent */
        if (new_parent)
        {
            new_parent->children.insert(new_parent->children.begin(), self());
        }

        parent = new_parent;
    }

    if (parent)
    {
        /* Make sure the view is available only as a child */
        if (this->get_output())
        {
            this->get_output()->workspace->remove_view(self());
        }

        this->set_output(parent->get_output());
        check_refocus_parent(parent);
    } else if (old_parent)
    {
        /* At this point, we are a regular view. We should try to position ourselves
         * directly above the old parent */
        if (this->get_output())
        {
            this->get_output()->workspace->add_view(
                self(), wf::LAYER_WORKSPACE);

            check_refocus_parent(old_parent);
            this->get_output()->workspace->restack_above(self(),
                find_toplevel_parent(old_parent));
        }
    }
}

std::vector<wayfire_view> wf::view_interface_t::enumerate_views(
    bool mapped_only)
{
    if (!this->is_mapped() && mapped_only)
    {
        return {};
    }

    std::vector<wayfire_view> result;
    result.reserve(view_impl->last_view_cnt);
    for (auto& v : this->children)
    {
        auto cdr = v->enumerate_views(mapped_only);
        result.insert(result.end(), cdr.begin(), cdr.end());
    }

    result.push_back(self());
    view_impl->last_view_cnt = result.size();

    return result;
}

std::string wf::view_interface_t::to_string() const
{
    return "view-" + wf::object_base_t::to_string();
}

wayfire_view wf::view_interface_t::self()
{
    return wayfire_view(this);
}

const wf::surface_sptr_t& wf::view_interface_t::get_main_surface() const
{
    return view_impl->main_surface;
}

const wf::dsurface_sptr_t& wf::view_interface_t::dsurf() const
{
    return view_impl->desktop_surface;
}

bool wf::view_interface_t::is_mapped() const
{
    if (view_impl->toplevel)
    {
        return view_impl->toplevel->current().is_mapped;
    } else
    {
        return view_impl->main_surface->is_mapped();
    }
}

/** Set the view's output. */
void wf::view_interface_t::set_output(wf::output_t *new_output)
{
    /* Make sure the view doesn't stay on the old output */
    if (get_output() && (get_output() != new_output))
    {
        /* Emit view-layer-detached first */
        get_output()->workspace->remove_view(self());

        view_detached_signal data;
        data.view = self();
        get_output()->emit_signal("view-disappeared", &data);
        get_output()->emit_signal("view-detached", &data);
    }

    _output_signal data;
    data.output = get_output();

    if (data.output != new_output)
    {
        // Notify all surfaces of the new output
        for (auto& surf : this->get_main_surface()->enumerate_surfaces())
        {
            if (data.output)
            {
                surf.surface->output().set_visible_on_output(data.output, false);
            }

            if (new_output)
            {
                surf.surface->output().set_visible_on_output(new_output, true);
            }
        }

        this->view_impl->output = new_output;
    }

    if ((new_output != data.output) && new_output)
    {
        view_attached_signal data;
        data.view = self();
        get_output()->emit_signal("view-attached", &data);
    }

    emit_signal("set-output", &data);

    for (auto& view : this->children)
    {
        view->set_output(new_output);
    }
}

wf::output_t*wf::view_interface_t::get_output()
{
    return view_impl->output;
}

wlr_box wf::view_interface_t::get_bounding_box()
{
    return transform_region(get_untransformed_bounding_box());
}

#define INVALID_COORDS(p) (std::isnan(p.x) || std::isnan(p.y))

wf::pointf_t wf::view_interface_t::global_to_local_point(const wf::pointf_t& arg,
    wf::surface_interface_t *surface)
{
    if (!is_mapped())
    {
        return arg;
    }

    /* First, untransform the coordinates to make them relative to the view's
     * internal coordinate system */
    wf::pointf_t result = arg;
    if (view_impl->transforms.size())
    {
        std::vector<wf::geometry_t> bb;
        bb.reserve(view_impl->transforms.size());
        auto box = get_untransformed_bounding_box();
        bb.push_back(box);
        view_impl->transforms.for_each([&] (auto& tr)
        {
            if (tr == view_impl->transforms.back())
            {
                return;
            }

            auto& transform = tr->transform;
            box = transform->get_bounding_box(box, box);
            bb.push_back(box);
        });

        view_impl->transforms.for_each_reverse([&] (auto& tr)
        {
            if (INVALID_COORDS(result))
            {
                return;
            }

            auto& transform = tr->transform;
            box = bb.back();
            bb.pop_back();
            result = transform->untransform_point(box, result);
        });

        if (INVALID_COORDS(result))
        {
            return result;
        }
    }

    /* Make cooordinates relative to the view */
    result.x += get_origin().x;
    result.y += get_origin().y;

    /* Go up from the surface, finding offsets */
    while (surface && surface->get_parent() != nullptr)
    {
        auto offset = surface->output().get_offset();
        result.x -= offset.x;
        result.y -= offset.y;

        surface = surface->priv->parent_surface;
    }

    return result;
}

wf::surface_interface_t*wf::view_interface_t::map_input_coordinates(
    wf::pointf_t cursor, wf::pointf_t& local)
{
    if (!is_mapped())
    {
        return nullptr;
    }

    auto view_relative_coordinates =
        global_to_local_point(cursor, nullptr);

    for (auto& child : get_main_surface()->enumerate_surfaces(true))
    {
        local.x = view_relative_coordinates.x - child.position.x;
        local.y = view_relative_coordinates.y - child.position.y;

        if (child.surface->input().accepts_input(local))
        {
            return child.surface;
        }
    }

    return nullptr;
}

void wf::view_interface_t::set_sticky(bool sticky)
{
    if (this->view_impl->sticky == sticky)
    {
        return;
    }

    damage();
    this->view_impl->sticky = sticky;
    damage();

    wf::view_set_sticky_signal data;
    data.view = self();

    this->emit_signal("set-sticky", &data);
    if (this->get_output())
    {
        this->get_output()->emit_signal("view-set-sticky", &data);
    }
}

bool wf::view_interface_t::is_visible()
{
    if (view_impl->visibility_counter <= 0)
    {
        return false;
    }

    if (is_mapped())
    {
        return true;
    }

    /* If we have an unmapped view, then there are two cases:
     *
     * 1. View has been "destroyed". In this case, the view is visible as long
     * as it has at least one reference (for ex. a plugin which shows unmap
     * animation)
     *
     * 2. View hasn't been "destroyed", just unmapped. Here we need to have at
     * least 2 references, which would mean that the view is in unmap animation.
     */
    if (view_impl->is_alive)
    {
        return view_impl->ref_cnt >= 2;
    } else
    {
        return view_impl->ref_cnt >= 1;
    }
}

void wf::view_interface_t::set_visible(bool visible)
{
    this->view_impl->visibility_counter += (visible ? 1 : -1);
    if (this->view_impl->visibility_counter > 1)
    {
        LOGE("set_visible(true) called more often than set_visible(false)!");
    }

    this->damage();
}

void wf::view_interface_t::damage()
{
    auto bbox = get_untransformed_bounding_box();
    view_impl->offscreen_buffer.cached_damage |= bbox;
    view_damage_raw(self(), transform_region(bbox));
}

wlr_box wf::view_interface_t::get_minimize_hint()
{
    return this->view_impl->minimize_hint;
}

void wf::view_interface_t::set_minimize_hint(wlr_box hint)
{
    this->view_impl->minimize_hint = hint;
}

void wf::view_interface_t::add_transformer(
    std::unique_ptr<wf::view_transformer_t> transformer)
{
    add_transformer(std::move(transformer), "");
}

void wf::view_interface_t::add_transformer(
    std::unique_ptr<wf::view_transformer_t> transformer, std::string name)
{
    damage();

    auto tr = std::make_shared<wf::view_transform_block_t>();
    tr->transform   = std::move(transformer);
    tr->plugin_name = name;

    view_impl->transforms.emplace_at(std::move(tr), [&] (auto& other)
    {
        if (other->transform->get_z_order() >= tr->transform->get_z_order())
        {
            return view_impl->transforms.INSERT_BEFORE;
        }

        return view_impl->transforms.INSERT_NONE;
    });

    damage();
}

nonstd::observer_ptr<wf::view_transformer_t> wf::view_interface_t::get_transformer(
    std::string name)
{
    nonstd::observer_ptr<wf::view_transformer_t> result{nullptr};
    view_impl->transforms.for_each([&] (auto& tr)
    {
        if (tr->plugin_name == name)
        {
            result = nonstd::make_observer(tr->transform.get());
        }
    });

    return result;
}

void wf::view_interface_t::pop_transformer(
    nonstd::observer_ptr<wf::view_transformer_t> transformer)
{
    view_impl->transforms.remove_if([&] (auto& tr)
    {
        return tr->transform.get() == transformer.get();
    });

    /* Since we can remove transformers while rendering the output, damaging it
     * won't help at this stage (damage is already calculated).
     *
     * Instead, we directly damage the whole output for the next frame */
    if (get_output())
    {
        get_output()->render->damage_whole_idle();
    }
}

void wf::view_interface_t::pop_transformer(std::string name)
{
    pop_transformer(get_transformer(name));
}

bool wf::view_interface_t::has_transformer()
{
    return view_impl->transforms.size();
}

wf::geometry_t wf::view_interface_t::get_main_geometry()
{
    if (topl())
    {
        // FIXME: Translate if the output is different.
        // Right now, this is not implemented as there are no plugins which
        // create multiple views for the same toplevel.
        return topl()->current().geometry;
    }

    return wf::construct_box(get_origin(),
        get_main_surface()->output().get_size());
}

wf::geometry_t wf::view_interface_t::get_untransformed_bounding_box()
{
    if (!is_mapped())
    {
        return view_impl->offscreen_buffer.geometry;
    }

    wf::point_t origin = get_origin();
    wf::region_t bounding_region;
    for (auto& child : get_main_surface()->enumerate_surfaces(true))
    {
        auto dim = child.surface->output().get_size();
        auto pos = child.position + origin;
        bounding_region |= wf::construct_box(pos, dim);
    }

    return wlr_box_from_pixman_box(bounding_region.get_extents());
}

wlr_box wf::view_interface_t::get_bounding_box(std::string transformer)
{
    return get_bounding_box(get_transformer(transformer));
}

wlr_box wf::view_interface_t::get_bounding_box(
    nonstd::observer_ptr<wf::view_transformer_t> transformer)
{
    return transform_region(get_untransformed_bounding_box(), transformer);
}

wlr_box wf::view_interface_t::transform_region(const wlr_box& region,
    nonstd::observer_ptr<wf::view_transformer_t> upto)
{
    auto box  = region;
    auto view = get_untransformed_bounding_box();

    bool computed_region = false;
    view_impl->transforms.for_each([&] (auto& tr)
    {
        if (computed_region || (tr->transform.get() == upto.get()))
        {
            computed_region = true;

            return;
        }

        box  = tr->transform->get_bounding_box(view, box);
        view = tr->transform->get_bounding_box(view, view);
    });

    return box;
}

wlr_box wf::view_interface_t::transform_region(const wlr_box& region,
    std::string transformer)
{
    return transform_region(region, get_transformer(transformer));
}

wlr_box wf::view_interface_t::transform_region(const wlr_box& region)
{
    return transform_region(region,
        nonstd::observer_ptr<wf::view_transformer_t>(nullptr));
}

wf::pointf_t wf::view_interface_t::transform_point(const wf::pointf_t& point)
{
    auto result = point;
    auto view   = get_untransformed_bounding_box();

    view_impl->transforms.for_each([&] (auto& tr)
    {
        result = tr->transform->transform_point(view, result);
        view   = tr->transform->get_bounding_box(view, view);
    });

    return result;
}

bool wf::view_interface_t::intersects_region(const wlr_box& region)
{
    /* fallback to the whole transformed boundingbox, if it exists */
    if (!is_mapped())
    {
        return region & get_bounding_box();
    }

    auto origin = get_origin();
    for (auto& child : get_main_surface()->enumerate_surfaces(true))
    {
        auto box = wf::construct_box(
            child.position + origin,
            child.surface->output().get_size());
        box = transform_region(box);

        if (region & box)
        {
            return true;
        }
    }

    return false;
}

wf::region_t wf::view_interface_t::get_transformed_opaque_region()
{
    if (!is_mapped())
    {
        return {};
    }

    // TODO: FIXME: !!!
    auto& maximal_shrink_constraint =
        wf::surface_interface_t::impl::active_shrink_constraint;
    int saved_shrink_constraint = maximal_shrink_constraint;

    /* Fullscreen views take up the whole screen, so plugins can't request
     * padding for them (nothing below is visible).
     *
     * In this case, we hijack the maximal_shrink_constraint, but we must
     * restore it immediately after subtracting the opaque region */
    if (topl() && topl()->current().fullscreen)
    {
        maximal_shrink_constraint = 0;
    }

    auto obox   = get_untransformed_bounding_box();
    auto origin = get_origin();

    wf::region_t opaque;
    for (auto& surf : get_main_surface()->enumerate_surfaces(true))
    {
        auto surf_opaque = surf.surface->output().get_opaque_region();
        surf_opaque += origin + surf.position;
        opaque |= surf_opaque;
    }

    auto bbox = obox;
    this->view_impl->transforms.for_each(
        [&] (const std::shared_ptr<view_transform_block_t> tr)
    {
        opaque = tr->transform->transform_opaque_region(bbox, opaque);
        bbox   = tr->transform->get_bounding_box(bbox, bbox);
    });

    maximal_shrink_constraint = saved_shrink_constraint;
    return opaque;
}

bool wf::view_interface_t::render_transformed(const wf::framebuffer_t& framebuffer,
    const wf::region_t& damage)
{
    if (!is_mapped() && !view_impl->offscreen_buffer.valid())
    {
        return false;
    }

    wf::geometry_t obox = get_untransformed_bounding_box();
    wf::texture_t previous_texture;
    float texture_scale;

    bool can_direct_scanout = is_mapped();
    can_direct_scanout &= get_main_surface()->enumerate_surfaces(true).size() == 1;
    can_direct_scanout &= get_main_surface()->get_wlr_surface() != nullptr;

    if (can_direct_scanout)
    {
        /* Optimized case: there is a single mapped surface.
         * We can directly start with its texture */
        previous_texture =
            wf::texture_t{this->get_main_surface()->get_wlr_surface()};
        texture_scale = get_main_surface()->get_wlr_surface()->current.scale;
    } else
    {
        take_snapshot();
        previous_texture = wf::texture_t{view_impl->offscreen_buffer.tex};
        texture_scale    = view_impl->offscreen_buffer.scale;
    }

    /* We keep a shared_ptr to the previous transform which we executed, so that
     * even if it gets removed, its texture remains valid.
     *
     * NB: we do not call previous_transform's transformer functions after the
     * cycle is complete, because the memory might have already been freed.
     * We only know that the texture is still alive. */
    std::shared_ptr<view_transform_block_t> previous_transform = nullptr;

    /* final_transform is the one that should render to the screen */
    std::shared_ptr<view_transform_block_t> final_transform = nullptr;

    /* Render the view passing its snapshot through the transformers.
     * For each transformer except the last we render on offscreen buffers,
     * and the last one is rendered to the real fb. */
    auto& transforms = view_impl->transforms;
    transforms.for_each([&] (auto& transform) -> void
    {
        /* Last transform is handled separately */
        if (transform == transforms.back())
        {
            final_transform = transform;

            return;
        }

        /* Calculate size after this transform */
        auto transformed_box =
            transform->transform->get_bounding_box(obox, obox);
        int scaled_width  = transformed_box.width * texture_scale;
        int scaled_height = transformed_box.height * texture_scale;

        /* Prepare buffer to store result after the transform */
        OpenGL::render_begin();
        transform->fb.allocate(scaled_width, scaled_height);
        transform->fb.scale    = texture_scale;
        transform->fb.geometry = transformed_box;
        transform->fb.bind(); // bind buffer to clear it
        OpenGL::clear({0, 0, 0, 0});
        OpenGL::render_end();

        /* Actually render the transform to the next framebuffer */
        transform->transform->render_with_damage(previous_texture, obox,
            wf::region_t{transformed_box}, transform->fb);

        previous_transform = transform;
        previous_texture   = previous_transform->fb.tex;
        obox = transformed_box;
    });

    /* This can happen in two ways:
     * 1. The view is unmapped, and no snapshot
     * 2. The last transform was deleted while iterating, so now the last
     *    transform is invalid in the list
     *
     * In both cases, we simply render whatever contents we have to the
     * framebuffer. */
    if (final_transform == nullptr)
    {
        OpenGL::render_begin(framebuffer);
        auto matrix = framebuffer.get_orthographic_projection();
        gl_geometry src_geometry = {
            1.0f * obox.x, 1.0f * obox.y,
            1.0f * obox.x + 1.0f * obox.width,
            1.0f * obox.y + 1.0f * obox.height,
        };

        for (const auto& rect : damage)
        {
            framebuffer.logic_scissor(wlr_box_from_pixman_box(rect));
            OpenGL::render_transformed_texture(previous_texture, src_geometry,
                {}, matrix);
        }

        OpenGL::render_end();
    } else
    {
        /* Regular case, just call the last transformer, but render directly
         * to the target framebuffer */
        final_transform->transform->render_with_damage(previous_texture, obox,
            damage & framebuffer.geometry, framebuffer);
    }

    return true;
}

wf::view_transform_block_t::view_transform_block_t()
{}
wf::view_transform_block_t::~view_transform_block_t()
{
    OpenGL::render_begin();
    this->fb.release();
    OpenGL::render_end();
}

const wf::framebuffer_t& wf::view_interface_t::take_snapshot()
{
    if (!is_mapped())
    {
        return view_impl->offscreen_buffer;
    }

    auto& offscreen_buffer = view_impl->offscreen_buffer;

    auto buffer_geometry = get_untransformed_bounding_box();
    offscreen_buffer.geometry = buffer_geometry;

    float scale = get_output()->handle->scale;

    offscreen_buffer.cached_damage &= buffer_geometry;
    /* Nothing has changed, the last buffer is still valid */
    if (offscreen_buffer.cached_damage.empty())
    {
        return view_impl->offscreen_buffer;
    }

    int scaled_width  = buffer_geometry.width * scale;
    int scaled_height = buffer_geometry.height * scale;
    if ((scaled_width != offscreen_buffer.viewport_width) ||
        (scaled_height != offscreen_buffer.viewport_height))
    {
        offscreen_buffer.cached_damage |= buffer_geometry;
    }

    OpenGL::render_begin();
    offscreen_buffer.allocate(scaled_width, scaled_height);
    offscreen_buffer.scale = scale;
    offscreen_buffer.bind();
    for (auto& box : offscreen_buffer.cached_damage)
    {
        offscreen_buffer.logic_scissor(wlr_box_from_pixman_box(box));
        OpenGL::clear({0, 0, 0, 0});
    }

    OpenGL::render_end();

    auto origin   = get_origin();
    auto children = get_main_surface()->enumerate_surfaces(true);
    for (auto& child : wf::reverse(children))
    {
        auto pos = child.position + origin;
        auto child_box = wf::construct_box(
            pos, child.surface->output().get_size());
        child.surface->output().simple_render(offscreen_buffer,
            pos, offscreen_buffer.cached_damage & child_box);
    }

    offscreen_buffer.cached_damage.clear();

    return view_impl->offscreen_buffer;
}

wf::view_interface_t::view_interface_t(
    std::shared_ptr<wf::surface_interface_t> main_surface)
{
    this->view_impl = std::make_unique<wf::view_interface_t::view_priv_impl>();
    set_main_surface(main_surface);
    take_ref();
}

void wf::view_interface_t::set_main_surface(
    std::shared_ptr<wf::surface_interface_t> main_surface)
{
    assert(!this->view_impl->main_surface);
    this->view_impl->main_surface = std::move(main_surface);

    view_impl->on_main_surface_damage.set_callback([=] (void *data)
    {
        auto ev = static_cast<wf::surface_damage_signal*>(data);
        auto damaged = ev->damage + get_origin();

        view_impl->offscreen_buffer.cached_damage |= damaged;
        for (auto& box : damaged)
        {
            auto wbox = wlr_box_from_pixman_box(box);
            view_damage_raw(self(), transform_region(wbox));
        }
    });

    view_impl->main_surface->connect_signal("damage",
        &view_impl->on_main_surface_damage);
}

void wf::view_interface_t::set_desktop_surface(wf::dsurface_sptr_t dsurface)
{
    assert(!this->view_impl->desktop_surface);
    this->view_impl->desktop_surface = dsurface;
}

void wf::view_interface_t::take_ref()
{
    ++view_impl->ref_cnt;
}

void wf::view_interface_t::unref()
{
    --view_impl->ref_cnt;
    if (view_impl->ref_cnt <= 0)
    {
        destruct();
    }
}

void wf::view_interface_t::initialize()
{}

void wf::view_interface_t::deinitialize()
{
    auto children = this->children;
    for (auto ch : children)
    {
        ch->set_toplevel_parent(nullptr);
    }

    this->view_impl->transforms.clear();
    this->_clear_data();

    OpenGL::render_begin();
    this->view_impl->offscreen_buffer.release();
    OpenGL::render_end();
}

wf::view_interface_t::~view_interface_t()
{
    /* Note: at this point, it is invalid to call most functions */
    unset_toplevel_parent(self());
}

void wf::view_damage_raw(wayfire_view view, const wlr_box& box)
{
    auto output = view->get_output();
    if (!output)
    {
        return;
    }

    /* Sticky views are visible on all workspaces. */
    if (view->is_sticky())
    {
        auto wsize = output->workspace->get_workspace_grid_size();
        auto cws   = output->workspace->get_current_workspace();

        /* Damage only the visible region of the shell view.
         * This prevents hidden panels from spilling damage onto other workspaces */
        wlr_box ws_box = output->get_relative_geometry();
        wlr_box visible_damage = geometry_intersection(box, ws_box);
        for (int i = 0; i < wsize.width; i++)
        {
            for (int j = 0; j < wsize.height; j++)
            {
                const int dx = (i - cws.x) * ws_box.width;
                const int dy = (j - cws.y) * ws_box.height;
                output->render->damage(visible_damage + wf::point_t{dx, dy});
            }
        }
    } else
    {
        output->render->damage(box);
    }

    view->emit_signal("region-damaged", nullptr);
}

void wf::view_interface_t::destruct()
{
    view_impl->is_alive = false;
    wf::get_core_impl().erase_view(self());
}

view_geometry_changed_signal data;
data.view = self();
data.old_geometry = old_geometry;

view_damage_raw(self(), last_bounding_box);
/* obox.x - wm.x is the current difference in the output and wm geometry */
geometry.x = x + obox.x - wm.x;
geometry.y = y + obox.y - wm.y;
damage()
/* Make sure that if we move the view while it is unmapped, its snapshot
 * is still valid coordinates */
if (view_impl->offscreen_buffer.valid())
{
    view_impl->offscreen_buffer.geometry.x += x - data.old_geometry.x;
    view_impl->offscreen_buffer.geometry.y += y - data.old_geometry.y;
}

last_bounding_box = get_bounding_box();
