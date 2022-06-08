#include <wayfire/scene.hpp>
#include <set>
#include <algorithm>

namespace wf
{
namespace scene
{
node_t::~node_t()
{}

node_t::node_t(bool is_structure)
{
    this->_is_structure = is_structure;
}

inner_node_t::inner_node_t(bool _is_structure) : node_t(_is_structure)
{}

std::optional<input_node_t> inner_node_t::find_node_at(const wf::pointf_t& at)
{
    for (auto& node : get_children())
    {
        auto child_node = node->find_node_at(at);
        if (child_node.has_value())
        {
            return child_node;
        }
    }

    return {};
}

static std::vector<node_t*> extract_structure_nodes(
    const std::vector<node_ptr>& list)
{
    std::vector<node_t*> structure;
    for (auto& node : list)
    {
        if (node->is_structure_node())
        {
            structure.push_back(node.get());
        }
    }

    return structure;
}

bool inner_node_t::set_children_list(std::vector<node_ptr> new_list)
{
    // Structure nodes should be sorted in both sequences and be the same.
    // For simplicity, we just extract the nodes in new vectors and check that
    // they are the same.
    //
    // FIXME: this could also be done with a merge-sort-like algorithm in place,
    // but is it worth it here? The scenegraph is supposed to stay static for
    // most of the time.
    if (extract_structure_nodes(children) != extract_structure_nodes(new_list))
    {
        return false;
    }

    set_children_unchecked(std::move(new_list));
    return true;
}

void inner_node_t::set_children_unchecked(std::vector<node_ptr> new_list)
{
    for (auto& node : new_list)
    {
        node->_parent = this;
    }

    this->children = std::move(new_list);
}

output_node_t::output_node_t() : inner_node_t(true)
{
    this->_static = std::make_shared<inner_node_t>(true);
    this->dynamic = std::make_shared<inner_node_t>(true);
    set_children_unchecked({_static, dynamic});
}

layer_node_t::layer_node_t() : inner_node_t(true)
{}

const std::shared_ptr<output_node_t>& layer_node_t::node_for_output(
    wf::output_t *output)
{
    auto it = outputs.find(output);
    if (it != outputs.end())
    {
        return it->second;
    }

    // FIXME: ...
    static std::shared_ptr<output_node_t> null_output = nullptr;
    return null_output;
}

void layer_node_t::handle_outputs_changed(wf::output_t *output, bool add)
{
    auto list = this->get_children();

    if (add)
    {
        outputs[output] = std::make_shared<output_node_t>();
        list.push_back(outputs[output]);
    } else
    {
        node_ptr target = outputs[output];
        outputs.erase(output);
        list.erase(std::remove(list.begin(), list.end(), target));
    }

    set_children_unchecked(list);
}

root_node_t::root_node_t() : inner_node_t(true)
{
    std::vector<node_ptr> children;

    for (int i = (int)layer::ALL_LAYERS - 1; i >= 0; i--)
    {
        layers[i] = std::make_shared<layer_node_t>();
        children.push_back(layers[i]);
    }

    set_children_unchecked(children);
}
} // namespace scene
}
