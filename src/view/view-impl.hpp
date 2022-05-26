#ifndef VIEW_IMPL_HPP
#define VIEW_IMPL_HPP

#include <wayfire/nonstd/safe-list.hpp>
#include <wayfire/view.hpp>
#include <wayfire/opengl.hpp>

#include "surface-impl.hpp"
#include <wayfire/nonstd/wlroots-full.hpp>

// for emit_map_*()
#include <wayfire/compositor-view.hpp>

struct wlr_seat;
namespace wf
{
struct sublayer_t;
struct view_transform_block_t
{
    std::string plugin_name = "";
    std::unique_ptr<wf::view_transformer_t> transform;
    wf::framebuffer_t fb;

    view_transform_block_t();
    ~view_transform_block_t();

    view_transform_block_t(const view_transform_block_t &) = delete;
    view_transform_block_t(view_transform_block_t &&) = delete;
    view_transform_block_t& operator =(const view_transform_block_t&) = delete;
    view_transform_block_t& operator =(view_transform_block_t&&) = delete;
};

/** Private data used by the default view_interface_t implementation */
class view_interface_t::view_priv_impl
{
  public:
    /**
     * A view is alive as long as it is possible for it to become mapped in the
     * future. For wlr views, this means that their role object hasn't been
     * destroyed and they still have the internal surface reference.
     */
    bool is_alive = true;
    /** Reference count to the view */
    int ref_cnt = 0;

    size_t last_view_cnt = 0;

    bool sticky = false;
    int visibility_counter = 1;

    wf::safe_list_t<std::shared_ptr<view_transform_block_t>> transforms;

    struct offscreen_buffer_t : public wf::framebuffer_t
    {
        wf::region_t cached_damage;
        bool valid()
        {
            return this->fb != (uint32_t)-1;
        }
    } offscreen_buffer;

    wlr_box minimize_hint = {0, 0, 0, 0};

    /** The sublayer of the view. For workspace-manager. */
    nonstd::observer_ptr<sublayer_t> sublayer;
    /* Promoted to the fullscreen layer? For workspace-manager. */
    bool is_promoted = false;

    wf::signal_connection_t on_main_surface_damage;

    wf::output_t *output = nullptr;

    surface_sptr_t main_surface;
    dsurface_sptr_t desktop_surface;
    toplevel_sptr_t toplevel; // can be NULL
};

/**
 * Damage the given box, assuming the damage belongs to the given view.
 * The given box is assumed to have been transformed with the view's
 * transformers.
 *
 * The main difference with directly damaging the output is that this will
 * add the damage to all workspaces the view is visible on, in case of shell
 * views.
 */
void view_damage_raw(wayfire_view view, const wlr_box& box);

/**
 * Base class for the default view controllers.
 */
class wlr_view_t : public wf::view_interface_t
{
  public:
    wlr_view_t();
    virtual ~wlr_view_t() = default;
    wlr_view_t(const wlr_view_t &) = delete;
    wlr_view_t(wlr_view_t &&) = delete;
    wlr_view_t& operator =(const wlr_view_t&) = delete;
    wlr_view_t& operator =(wlr_view_t&&) = delete;

    /** @return The offset from the surface coordinates to the actual geometry */
    virtual wf::point_t get_window_offset();
    virtual void emit_map();

    wf::point_t get_origin() final;
    bool is_mapped() const final;

  protected:
    bool mapped = false;
    wf::point_t origin = {0, 0};
    void set_position(wf::point_t point);

    /**
     * The bounding box of the view the last time it was rendered.
     *
     * This is used to damage the view when it is resized, because when a
     * transformer changes because the view is resized, we can't reliably
     * calculate the old view region to damage.
     */
    wf::geometry_t last_bounding_box{0, 0, 0, 0};
    void update_bbox();

    wf::signal_connection_t on_toplevel_geometry_changed;
    // Track toplevel for position, damage whenever it changes
    void setup_toplevel_tracking();

  public:
    virtual void commit();
    virtual void map();
    virtual void unmap();

    /* Handle the destruction of the underlying wlroots object */
    virtual void destroy();
};

/** Emit the map signal for the given view */
void emit_view_map_signal(wayfire_view view, bool has_position);
void emit_ping_timeout_signal(desktop_surface_t *dsurface);

/**
 * Emit @signal_name on the @toplevel with @data as signal data.
 * If the toplevel has an associated output, emit the signal there as well, with
 * prefix `toplevel-`.
 */
void emit_toplevel_signal(wf::toplevel_t *toplevel,
    std::string_view signal_name, wf::signal_data_t *data);

wf::geometry_t adjust_geometry_for_gravity(wf::geometry_t wmg,
    uint32_t edges, wf::dimensions_t new_size);

wf::geometry_t shrink_by_margins(
    wf::geometry_t g, wf::decoration_margin_t margin);

wf::geometry_t expand_with_margins(
    wf::geometry_t g, wf::decoration_margin_t margin);

wayfire_view find_toplevel_parent(wayfire_view view);

/**
 * Emit @signal_name on the @view with @data as signal data.
 * If the view has an associated output, emit the signal there as well, with
 * prefix `view-`.
 */
void emit_view_signal(wf::view_interface_t *view,
    std::string_view signal_name, wf::signal_data_t *data);

wf::surface_interface_t *wf_surface_from_void(void *handle);

void init_xdg_shell();
void init_xwayland();
void init_layer_shell();

std::string xwayland_get_display();
void xwayland_update_default_cursor();

/* Ensure that the given surface is on top of the Xwayland stack order. */
void xwayland_bring_to_front(wlr_surface *surface);

/* Get the current Xwayland drag icon, if it exists. */
wayfire_view get_xwayland_drag_icon();

void init_desktop_apis();
}

#endif /* end of include guard: VIEW_IMPL_HPP */
