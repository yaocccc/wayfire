#include "xwayland-desktop-surface.hpp"
#include <wayfire/debug.hpp>
#include "../view-impl.hpp"
#include "xwayland-helpers.hpp"

wf::xwayland_desktop_surface_t::xwayland_desktop_surface_t(
    wlr_xwayland_surface *xw) : wlr_desktop_surface_t(xw->surface)
{
    this->xw = xw;
    LOGE("new xwayland surface ", xw->title,
        " class: ", xw->class_t, " instance: ", xw->instance);

    on_destroy.set_callback([&] (void*)
    {
        destroy();
    });
    on_set_title.set_callback([&] (void*)
    {
        set_title(nonull(xw->title));
    });
    on_set_class.set_callback([&] (void*)
    {
        set_app_id(nonull(xw->class_t));
    });
    on_ping_timeout.set_callback([&] (void*)
    {
        wf::emit_ping_timeout_signal(this);
    });
    on_map.set_callback([&] (void*)
    {
        update_kb_focus_enabled();
    });

    on_map.connect(&xw->events.map);
    on_destroy.connect(&xw->events.destroy);
    on_set_title.connect(&xw->events.set_title);
    on_set_class.connect(&xw->events.set_class);
    on_ping_timeout.connect(&xw->events.ping_timeout);

    on_set_title.emit(NULL);
    on_set_class.emit(NULL);
    update_kb_focus_enabled();

    switch (wf::xw::get_window_type(xw))
    {
      case xw::window_type_t::DND:
      case xw::window_type_t::OR:
        this->current_role = desktop_surface_t::role::UNMANAGED;
        break;

      case xw::window_type_t::TOPLEVEL:
      case xw::window_type_t::DIALOG:
        this->current_role = desktop_surface_t::role::TOPLEVEL;
        break;
    }
}

void wf::xwayland_desktop_surface_t::ping()
{
    if (xw)
    {
        wlr_xwayland_surface_ping(xw);
    }
}

void wf::xwayland_desktop_surface_t::close()
{
    if (xw)
    {
        wlr_xwayland_surface_close(xw);
    }
}

void wf::xwayland_desktop_surface_t::destroy()
{
    this->xw = NULL;

    on_map.disconnect();
    on_destroy.disconnect();
    on_set_title.disconnect();
    on_set_class.disconnect();
    on_ping_timeout.disconnect();
}

void wf::xwayland_desktop_surface_t::update_kb_focus_enabled()
{
    keyboard_focus_enabled = (!xw->override_redirect ||
        wlr_xwayland_or_surface_wants_focus(xw));
}
