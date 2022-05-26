#pragma once

#include <memory>
#include <wayfire/nonstd/observer_ptr.h>
#include <wayfire/object.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/nonstd/wlroots.hpp>
#include <wayfire/decorator.hpp>

namespace wf
{
class output_t;
/**
 * A bitmask consisting of all tiled edges.
 * This corresponds to a maximized state.
 */
constexpr uint32_t TILED_EDGES_ALL =
    WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;

/**
 * Describes a state of a toplevel.
 */
struct toplevel_state_t
{
    /**
     * The primary output of a toplevel.
     * The toplevel may be displayed on zero or more outputs by creating views
     * for this toplevel and its surfaces, however, the primary output serves
     * as a reference for the geometry of the toplevel.
     */
    wf::output_t *primary_output = NULL;

    /**
     * The geometry of the base surface of the toplevel, excluding shadows.
     * This may not be the final geometry of the toplevel on the screen, which
     * is also subject to transformers.
     *
     * Geometry is in the coordinate system of the toplevel's primary output.
     */
    wf::geometry_t geometry = {0, 0, 0, 0};

    /**
     * The geometry of the base surface of the toplevel, including shadows (if
     * they are drawn by the client). This may not be the final geometry of the
     * toplevel on the screen, which is also subject to transformers.
     *
     * Geometry is in the coordinate system of the toplevel's primary output.
     */
    wf::geometry_t base_geometry = {0, 0, 0, 0};

    /**
     * The fullscreen state of the toplevel.
     * A toplevel with fullscreen state typically has geometry equal to the
     * full output geometry of its primary output, and is visible only on it.
     */
    bool fullscreen = false;

    /**
     * The activated state of the toplevel.
     * An activated toplevel usually has the keyboard focus and the client
     * usually shows visual indication of that fact.
     */
    bool activated = false;

    /**
     * The minimized state of the toplevel.
     * Minimized toplevels' views are typically hidden from the user, but
     * remain on the workspace where they were last visible.
     */
    bool minimized = false;

    /**
     * The tiled edges of a toplevel.
     * If a toplevel is tiled to an edge, usually there is another surface
     * or an output edge right next to that edge. Clients typically hide
     * shadows and other decorative elements on these edges.
     *
     * If all tiled edges are set, then the toplevel is effectively maximized.
     */
    uint32_t tiled_edges = 0;

    struct
    {
        /**
         * Whether the toplevel's position has been set at least once.
         */
        bool has_position = false;
    } flags;
};

/**
 * Toplevels are desktop surfaces which represent application windows only.
 * They have additional configurable state like geometry, tiled edges, etc.
 */
class toplevel_t : public wf::object_base_t
{
  public:
    toplevel_t() = default;
    toplevel_t(const toplevel_t&) = delete;
    toplevel_t(toplevel_t&&) = delete;
    toplevel_t& operator =(const toplevel_t&) = delete;
    toplevel_t& operator =(toplevel_t&&) = delete;
    virtual ~toplevel_t() = default;

    /**
     * Check whether the toplevel should have server-side decorations.
     *
     * Not all toplevels should have these, as many clients provide client-side
     * decorations or explicitly request that their toplevel remain undecorated
     * via protocols like xdg-decoration.
     */
    virtual bool should_be_decorated() = 0;

    /**
     * Get the current state of the toplevel.
     */
    virtual toplevel_state_t& current() = 0;

    // Forward compatibility: the pending state of the toplevel once transactions
    // are here.
    virtual toplevel_state_t& pending() { return current(); }

    /** Set the minimized state of the toplevel. */
    virtual void set_minimized(bool minimized) = 0;
    /** Set the tiled edges of the toplevel */
    virtual void set_tiled(uint32_t edges) = 0;
    /** Set the fullscreen state of the toplevel */
    virtual void set_fullscreen(bool fullscreen) = 0;
    /** Set the toplevel's activated state.  */
    virtual void set_activated(bool active) = 0;

    /**
     * Request that the toplevel's geometry changes.
     *
     * The position of the toplevel will usually change according to the
     * request, but the clients are free to disobey a resize request.
     */
    virtual void set_geometry(wf::geometry_t g) = 0;

    /**
     * Temporary clutch.
     */
    virtual void move(int x, int y) = 0;

    /**
     * Set the primary output of this toplevel.
     * The primary output is a hint for plugins so that they may ignore
     * toplevels which are visible on a certain output but do not logically
     * belong to it.
     *
     * For example, if a toplevel is between two outputs and the workspace on
     * its primary output changes, it will be moved out of view. However, if the
     * workspace on a non-primary output changes, the toplevel will remain where
     * it was before the change.
     */
    virtual void set_output(wf::output_t *new_output) = 0;

    /**
     * Indicate that the toplevel is being continuously moved by a plugin.
     *
     * @param moving whether to enable or disable moving mode
     */
    virtual void set_moving(bool moving) = 0;

    /**
     * @return Whether the toplevel is in a continous move operation.
     */
    virtual bool is_moving() = 0;

    /**
     * Start a resizing mode for this toplevel. While a toplevel is resizing,
     * one edge or corner of the toplevel is made immobile (exactly the
     * edge/corner opposite to the edges which are set as resizing)
     *
     * @param resizing whether to enable or disable resizing mode
     * @param edges the edges which are being resized
     */
    virtual void set_resizing(bool resizing, uint32_t edges = 0) = 0;

    /**
     * @return Whether the toplevel is in a continous resize operation.
     */
    virtual bool is_resizing() = 0;

    /**
     * Request that the toplevel resizes to its default size.
     */
    virtual void request_native_size() = 0;

    /**
     * Set the decorator for the toplevel.
     *
     * The decorator simply controls how big the decorations around the toplevel
     * are. To actually display the decorations, you should add a custom surface
     * to the surface tree of this toplevel.
     *
     * @param frame The decorator for the surface. To avoid conflicts, plugins
     *   which provide decorations are advised to activate with the DECORATOR
     *   capability, as this call overrides the decorator set by previous calls
     *   to this method.
     */
    virtual void set_decoration(std::unique_ptr<toplevel_decorator_t> frame) = 0;
};

using toplevel_sptr_t = std::shared_ptr<toplevel_t>;
}
