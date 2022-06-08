#pragma once

#include <optional>
#include <memory>
#include <vector>
#include <map>
#include <wayfire/geometry.hpp>
#include <wayfire/region.hpp>

namespace wf
{
class surface_interface_t;
class output_t;
class output_layout_t;
/**
 * Contains definitions of the common elements used in Wayfire's scenegraph.
 *
 * The scenegraph is a complete representation of the current rendering and input
 * state of Wayfire. The basic nodes forms a tree where every node is responsible
 * for managing its children's state.
 *
 * The rough structure of the scenegraph is as follows:
 *
 * Level 1: the root node, which is a simple container of other nodes.
 * Level 2: a list of layer nodes, which represent different types of content,
 *   ordered in increasing stacking order (i.e. first layer is the bottommost).
 * Level 3: in each layer, there is a special output node for each currently
 *   enabled output. By default, this node's bounding box is limited to the
 *   extents of the output, so that no nodes overlap multiple outputs.
 * Level 4: in each output node, there is a static and a dynamic container.
 *   Static containers contain views which do not change when workspace sets
 *   are changed, for example layer-shell views. Dynamic containers contain
 *   the views which are bound to the current workspace set.
 * Level 5 and beyond: These levels typically contain views and group of views,
 *   or special effects (particle systems and the like).
 *
 * Each level may contain additional nodes added by plugins (or by core in the
 * case of DnD views). The scenegraph generally allows full flexibility here,
 * but the aforementioned nodes are always available and used by most plugins
 * to ensure the most compatibility.
 *
 * The most common operations that a plugin needs to execute on the scenegraph
 * are reordering elements (and thus changing the stack order) and potentially
 * moving them between layers and outputs. In addition, the scenegraph can be
 * used in some more advanced cases:
 *
 * - The scenegraph may be used to implement what used to be custom renderers
 *   prior to Wayfire 0.8.0, i.e. override the default output of a single
 *   workspace covering the whole output. The preferred way to do that is to
 *   disable the output nodes in each layer and add a custom node in one of the
 *   layers which does the custom rendering and covers the whole output.
 *
 * - A similar 'trick' can be used for grabbing all input on a particular output
 *   and is the preferred way to do what input grabs used to do prior to Wayfire
 *   0.8.0. To emulate a grab, create an input-only scene node in the OVERRIDE
 *   layer on an output (it thus gets all touch and pointer input automatically)
 *   and make it use an exclusive keyboard input mode to grab the keyboard as
 *   well.
 *
 * - Always-on-top views are simply nodes which are placed above the dynamic
 *   container of the workspace layer of each output.
 */
namespace scene
{
class node_t;
class inner_node_t;
using node_ptr = std::shared_ptr<node_t>;
using inner_node_ptr = std::shared_ptr<inner_node_t>;

/**
 * Used as a result of an intersection of the scenegraph with the user input.
 */
struct input_node_t
{
    const node_ptr& node;
    // FIXME: In the future, this should be a separate interface, allowing
    // non-surface nodes to get user input as well.
    wf::surface_interface_t *surface;

    input_node_t(const node_ptr& _node, wf::surface_interface_t *si) :
        node(_node), surface(si)
    {}
};

/**
 * The base class for all nodes in the scenegraph.
 */
class node_t
{
  public:
    virtual ~node_t();

    /**
     * Find the input node at the given position.
     */
    virtual std::optional<input_node_t> find_node_at(const wf::pointf_t& at) = 0;

    /**
     * Structure nodes are special nodes which core usually creates when Wayfire
     * is started (e.g. layer and output nodes). These nodes should not be
     * reordered or removed from the scenegraph.
     */
    bool is_structure_node() const
    {
        return _is_structure;
    }

    /**
     * Get the parent of the current node in the scene graph.
     */
    inner_node_t *parent() const
    {
        return this->_parent;
    }

  public:
    node_t(const node_t&) = delete;
    node_t(node_t&&) = delete;
    node_t& operator =(const node_t&) = delete;
    node_t& operator =(node_t&&) = delete;

  protected:
    node_t(bool is_structure);
    bool _is_structure;
    inner_node_t *_parent;
    friend class inner_node_t;
};

/**
 * An inner node of the scenegraph tree with a floating list of children.
 * Plugins may add additional nodes and reorder them, however, special care
 * needs to be taken to avoid reordering the special `structure` nodes.
 */
class inner_node_t : public node_t
{
  public:
    inner_node_t(bool _is_structure);

    /**
     * Find the input node at the given position.
     */
    std::optional<input_node_t> find_node_at(const wf::pointf_t& at) final;

    /**
     * Obtain an immutable list of the node's children.
     * Use set_children_list() to modify the children.
     */
    const std::vector<node_ptr>& get_children() const
    {
        return children;
    }

    /**
     * Exchange the list of children of this node.
     * A typical usage (for example, bringing a node to the top):
     * 1. list = get_children()
     * 2. list.erase(target_node)
     * 3. list.insert(list.begin(), target_node)
     * 4. set_children_list(list)
     *
     * The set_children_list function also performs checks on the structure
     * nodes present in the inner node. If they were changed, the change is
     * rejected and false is returned. In all other cases, the list of
     * children is updated, and each child's parent is set to this node.
     */
    bool set_children_list(std::vector<node_ptr> new_list);

  protected:
    /**
     * A list of children nodes sorted from top to bottom.
     *
     * Note on special `structure` nodes: These nodes are typically present in
     * the normal list of children, but also accessible via a specialized pointer
     * in their parent's class.
     */
    std::vector<std::shared_ptr<node_t>> children;

    void set_children_unchecked(std::vector<node_ptr> new_list);
};

/**
 * A Level 3 node which represents each output in each layer.
 */
class output_node_t : public inner_node_t
{
  public:
    output_node_t();

    /**
     * A container for the static child nodes.
     * Static child nodes are always below the dynamic nodes of an output and
     * are usually not modified when the workspace on the output changes, so
     * things like backgrounds and panels are usually static.
     */
    std::shared_ptr<inner_node_t> _static;

    /**
     * A container for the dynamic child nodes.
     * These nodes move together with the output's workspaces.
     * These nodes are most commonly views.
     */
    std::shared_ptr<inner_node_t> dynamic;
};

/**
 * A node which represents a layer (Level 2) in the scenegraph.
 */
class layer_node_t : public inner_node_t
{
  public:
    layer_node_t();

    /**
     * Find the child node corresponding to the given output.
     */
    const std::shared_ptr<output_node_t>& node_for_output(wf::output_t *output);

  private:
    std::map<wf::output_t*, std::shared_ptr<output_node_t>> outputs;

    // Called by output-layout when the outputs change
    void handle_outputs_changed(wf::output_t *output, bool add);
    friend class wf::output_layout_t;
};

/**
 * A list of all layers in the root node.
 */
enum class layer : size_t
{
    BACKGROUND = 0,
    BOTTOM     = 1,
    WORKSPACE  = 2,
    TOP        = 3,
    UNMANAGED  = 4,
    OVERLAY    = 5,
    /** Not a real layer, but a placeholder for the number of layers. */
    ALL_LAYERS,
};

/**
 * The root (Level 1) node of the whole scenegraph.
 */
class root_node_t : public inner_node_t
{
  public:
    root_node_t();

    /**
     * An ordered list of all layers' nodes.
     */
    std::shared_ptr<layer_node_t> layers[(size_t)layer::ALL_LAYERS];
};
}
}
