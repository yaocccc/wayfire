#pragma once

#include <vector>
#include <wayfire/nonstd/observer_ptr.h>

#include <wayfire/surface.hpp>
#include <wayfire/region.hpp>
#include <wayfire/desktop-surface.hpp>
#include <wayfire/toplevel.hpp>

namespace wf
{
class view_interface_t;
class decorator_frame_t_t;
class view_transformer_t;

using surface_sptr_t  = std::shared_ptr<surface_interface_t>;
using dsurface_sptr_t = std::shared_ptr<desktop_surface_t>;
}

using wayfire_view = nonstd::observer_ptr<wf::view_interface_t>;

namespace wf
{
class output_t;

/**
 * view_interface_t is the base class for all "toplevel windows", i.e surfaces
 * which have no parent.
 */
class view_interface_t : public wf::object_base_t
{
  public:
    /**
     * The parent of the view, for ex. the main view of a file chooser dialogue.
     * This usually corresponds with the parent of the view's toplevel_t parent.
     */
    wayfire_view parent = nullptr;

    /**
     * A list of the children views.
     */
    std::vector<wayfire_view> children;

    /**
     * Get the view's main surface.
     */
    const surface_sptr_t& get_main_surface() const;

    /**
     * Get the desktop surface associated with this view.
     */
    const dsurface_sptr_t& dsurf() const;

    /**
     * Get the toplevel associated with this view, if it exists.
     * Not all views must have an associated toplevel.
     */
    const toplevel_sptr_t& topl() const;

    /**
     * Generate a list of all views in the view's tree.
     * This includes the view itself, its @children and so on.
     *
     * @param mapped_only Whether to include only mapped views.
     *
     * @return A list of all views in the view's tree. This includes the view
     *   itself, its @children and so on.
     */
    std::vector<wayfire_view> enumerate_views(bool mapped_only = true);

    /**
     * Set the toplevel parent of the view, and adjust the children's list of
     * the parent.
     */
    void set_toplevel_parent(wayfire_view parent);

    /** Get a textual identifier for this view.  */
    std::string to_string() const;

    /** Wrap the view into a nonstd::observer_ptr<> */
    wayfire_view self();

    /**
     * Set the view's output.
     *
     * If the new output is different from the previous, the view will be
     * removed from the layer it was on the old output.
     */
    void set_output(wf::output_t *new_output);

    /**
     * Get the view's output.
     */
    wf::output_t *get_output();

    /**
     * Find the bounding box of the view in the coordinate system of its output.
     * The bounding box encompasses the view and all its subsurfaces after
     * transforming them with the currently applied transformers, but excludes
     * child views and menus.
     */
    wlr_box get_bounding_box();

    /**
     * Find the surface in the view's tree which contains the given point.
     *
     * @param cursor The coordinate of the point to search at
     * @param local The coordinate of the point relative to the returned surface
     *
     * @return The surface which is at the given point, or nullptr if no such
     *         surface was found (in which case local has no meaning)
     */
    surface_interface_t *map_input_coordinates(
        wf::pointf_t cursor, wf::pointf_t& local);

    /**
     * Transform the given point's coordinates into the local coordinate system
     * of the given surface in the view's surface tree, after applying all
     * transforms of the view.
     *
     * @param arg The point in global (output-local) coordinates
     * @param surface The reference surface, or null for view-local coordinates
     * @return The point in surface-local coordinates
     */
    wf::pointf_t global_to_local_point(const wf::pointf_t& arg,
        surface_interface_t *surface);

    /** @return true if the view is visible */
    bool is_visible();

    /**
     * Change the view visibility. Visibility requests are counted, i.e if the
     * view is made invisible two times, it needs to be made visible two times
     * before it is visible again.
     */
    void set_visible(bool visible);

    /** Set the views's sticky state. */
    void set_sticky(bool sticky);

    /**
     * Get the view's sticky state.
     * A sticky view should be visible on all workspaces.
     */
    bool is_sticky();

    /** Damage the whole view and add the damage to its output */
    void damage();

    /**
     * Get the minimize target for this view, i.e when displaying a minimize
     * animation, where the animation's target should be. Defaults to {0,0,0,0}.
     *
     * @return the minimize target
     */
    wlr_box get_minimize_hint();

    /**
     * Sets the minimize target for this view, i.e when displaying a minimize
     * animation, where the animation's target should be.
     * @param hint The new minimize target rectangle, in output-local coordinates.
     */
    void set_minimize_hint(wlr_box hint);

    /*
     *                        View transforms
     * A view transform can be any kind of transformation, for example 3D
     * rotation, wobbly effect or similar. When we speak of transforms, a
     * "view" is defined as a toplevel window (including decoration) and also
     * all of its subsurfaces/popups. The transformation then is applied to
     * this group of surfaces together.
     *
     * When a view has a custom transform, then internally all these surfaces
     * are rendered to a FBO, and then the custom transformation renders the
     * resulting texture as it sees fit. In case of multiple transforms, we do
     * multiple render passes where each transform is fed the result of the
     * previous transforms.
     *
     * Damage tracking for transformed views is done on the boundingbox of the
     * damaged region after applying the transformation, but all damaged parts
     * of the internal FBO are updated.
     * */

    void add_transformer(std::unique_ptr<wf::view_transformer_t> transformer);

    /**
     * Add a transformer with the given name. Note that you can add multiple
     * transforms with the same name.
     */
    void add_transformer(std::unique_ptr<wf::view_transformer_t> transformer,
        std::string name);

    /** @return a transformer with the give name, or null */
    nonstd::observer_ptr<wf::view_transformer_t> get_transformer(
        std::string name);

    /** remove a transformer */
    void pop_transformer(
        nonstd::observer_ptr<wf::view_transformer_t> transformer);
    /** remove all transformers with the given name */
    void pop_transformer(std::string name);
    /** @return true if the view has active transformers */
    bool has_transformer();

    /** @return the bounding box of the view up to the given transformer */
    wlr_box get_bounding_box(std::string transformer);
    /** @return the bounding box of the view up to the given transformer */
    wlr_box get_bounding_box(
        nonstd::observer_ptr<wf::view_transformer_t> transformer);

    /**
     * Transform a point with the view's transformers.
     *
     * @param point The point in output-local coordinates, before applying the
     *              view transformers.
     * @return The point in output-local coordinates after applying the
     *         view transformers.
     */
    wf::pointf_t transform_point(const wf::pointf_t& point);

    /** @return a bounding box of the given box after applying the
     * transformers of the view */
    wlr_box transform_region(const wlr_box & box);

    /** @return a bounding box of the given box after applying the transformers
     * of the view up to the given transformer */
    wlr_box transform_region(const wlr_box& box, std::string transformer);
    /** @return a bounding box of the given box after applying the transformers
     * of the view up to the given transformer */
    wlr_box transform_region(const wlr_box& box,
        nonstd::observer_ptr<wf::view_transformer_t> transformer);

    /**
     * @return true if the region intersects any of the surfaces in the view's
     * surface tree.
     */
    bool intersects_region(const wlr_box& region);

    /**
     * Get the transformed opaque region of the view and its subsurfaces.
     * The returned region is in output-local coordinates.
     */
    wf::region_t get_transformed_opaque_region();

    /**
     * Render all the surfaces of the view using the view's transforms.
     * If the view is unmapped, this operation will try to read from any
     * snapshot created by take_snapshot() or when transformers were applied,
     * and use that buffer.
     *
     * Child views like dialogues are considered a part of the view's surface
     * tree, however they are not transformed by the view's transformers.
     *
     * @param framebuffer The framebuffer to render to. Geometry needs to be
     *   in output-local coordinate system.
     * @param damage The damaged region of the view, in output-local coordinate
     *   system.
     *
     * @return true if the render operation was successful, and false if the
     *   view is both unmapped and has no snapshot.
     */
    bool render_transformed(const framebuffer_t& framebuffer,
        const region_t& damage);

    /**
     * A snapshot of the view is a copy of the view's contents into a
     * framebuffer. It is used to get an image of the view while it is mapped,
     * and continue displaying it afterwards. Additionally, return the captured
     * framebuffter
     */
    const wf::framebuffer_t& take_snapshot();

    /**
     * View lifetime is managed by reference counting. To take a reference,
     * use take_ref(). Note that one reference is automatically made when the
     * view is created.
     */
    void take_ref();

    /**
     * Drop a reference to the surface. When the reference count reaches 0, the
     * destruct() method is called.
     */
    void unref();

    virtual ~view_interface_t();

    view_interface_t(const view_interface_t &) = delete;
    view_interface_t(view_interface_t &&) = delete;
    view_interface_t& operator =(const view_interface_t&) = delete;
    view_interface_t& operator =(view_interface_t&&) = delete;

    class view_priv_impl;
    std::unique_ptr<view_priv_impl> view_impl;

    /**
     * The last time(nanoseconds since epoch) when the view was focused.
     * Updated automatically by core.
     */
    uint64_t last_focus_timestamp = 0;

    /**
     * @return the bounding box of the view before transformers,
     *  in output-local coordinates.
     */
    wf::geometry_t get_untransformed_bounding_box();

    /**
     * Get the untransformed 'main' part of the view's surface tree.
     * For views with associated toplevels, this is their current geometry,
     * translated to the view's output if it is different.
     * For other views, this is just the geometry of the main surface.
     *
     * This geometry serves as a rough approximation of where the view 'is',
     * regardless of its type.
     */
    wf::geometry_t get_main_geometry();

    /**
     * Get the coordinates of the top-left corner of the view in the coordinate
     * system of its output.
     *
     * If you need to create a view for a desktop surface without a toplevel,
     * you should subclass view_interface_t and implement this method, as the
     * default implementation works only for views with a toplevel attached.
     */
    virtual wf::point_t get_origin() = 0;

    /**
     * Check whether the view is mapped.
     *
     * The mapped state generally indicates that the view is ready to interact
     * with user input.
     *
     * A mapped view should have its primary surface mapped as well.
     */
    virtual bool is_mapped() const = 0;

  protected:
    /**
     * Create a new view.
     *
     * After creating a view, its main and desktop surfaces should be
     * immediately set.
     */
    view_interface_t();

    /**
     * Set the main surface.
     * This should be done only once in the lifetime of the view and must
     * happen before initializing the view!
     */
    void set_main_surface(std::shared_ptr<wf::surface_interface_t> main_surface);

    /**
     * Set the desktop surface.
     * This should be done only once in the lifetime of the view and must
     * happen before initializing the view!
     */
    void set_desktop_surface(wf::dsurface_sptr_t dsurface);

    /**
     * Set the toplevel.
     * This should be done at most once in the lifetime of the view and must
     * happen before initializing the view!
     */
    void set_toplevel(wf::toplevel_sptr_t toplevel);

    friend class compositor_core_impl_t;
    /**
     * View initialization happens in three stages:
     *
     * First, memory for the view is allocated and default members are set.
     * Second, the view is added to core, which assigns the view to an output.
     * Third, core calls @initialize() on the view. This is where the real view
     *   initialization should happen.
     *
     * Note that generally most of the operations in 3. can be also done in 1.,
     * except when they require an output.
     */
    virtual void initialize();

    /**
     * When a view is being destroyed, all associated objects like subsurfaces,
     * transformers and custom data are destroyed.
     *
     * In general, we want to make sure that these associated objects are freed
     * before the actual view object destruction starts. Thus, deinitialize()
     * is called from core just before destroying the view.
     */
    virtual void deinitialize();

    /**
     * Called when the reference count reaches 0.
     * It destructs the object and deletes it, so "this" may not be
     * accessed after destruct() is called.
     */
    virtual void destruct();
};

wayfire_view wl_surface_to_wayfire_view(wl_resource *surface);

/**
 * Emit the view map signal. It indicates that a view has been mapped, i.e.
 * plugins can now "work" with it. Note that not all views will emit the map
 * event.
 */
void emit_view_map(wayfire_view view);

/**
 * Emit the view unmap signal. It indicates that the view is in the process of
 * being destroyed. Most plugins should stop any actions they have on the view.
 */
void emit_view_unmap(wayfire_view view);

/**
 * Emit the view pre-unmap signal. It is emitted right before the view
 * destruction start. At this moment a plugin can still take a snapshot of the
 * view. Note that not all views emit the pre-unmap signal, however the unmap
 * signal is mandatory for all views.
 */
void emit_view_pre_unmap(wayfire_view view);
}
