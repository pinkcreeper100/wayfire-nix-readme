#include <wayfire/view.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/util/log.hpp>
#include "wayfire/scene-operations.hpp"
#include "wayfire/scene.hpp"
#include "wm-actions-signals.hpp"

class wayfire_wm_actions_t : public wf::plugin_interface_t
{
    wf::scene::floating_inner_ptr always_above;
    bool showdesktop_active = false;

    wf::option_wrapper_t<wf::activatorbinding_t> toggle_showdesktop{
        "wm-actions/toggle_showdesktop"};
    wf::option_wrapper_t<wf::activatorbinding_t> minimize{
        "wm-actions/minimize"};
    wf::option_wrapper_t<wf::activatorbinding_t> toggle_maximize{
        "wm-actions/toggle_maximize"};
    wf::option_wrapper_t<wf::activatorbinding_t> toggle_above{
        "wm-actions/toggle_always_on_top"};
    wf::option_wrapper_t<wf::activatorbinding_t> toggle_fullscreen{
        "wm-actions/toggle_fullscreen"};
    wf::option_wrapper_t<wf::activatorbinding_t> toggle_sticky{
        "wm-actions/toggle_sticky"};
    wf::option_wrapper_t<wf::activatorbinding_t> send_to_back{
        "wm-actions/send_to_back"};

    bool set_keep_above_state(wayfire_view view, bool above)
    {
        if (!view || !output->can_activate_plugin(this->grab_interface))
        {
            return false;
        }

        wf::scene::remove_child(view->get_root_node());
        if (above)
        {
            wf::scene::add_front(always_above, view->get_root_node());
            view->store_data(std::make_unique<wf::custom_data_t>(),
                "wm-actions-above");
        } else
        {
            if (view->has_data("wm-actions-above"))
            {
                output->workspace->add_view(view, wf::LAYER_WORKSPACE);
                view->erase_data("wm-actions-above");
            }
        }

        wf::wm_actions_above_changed data;
        data.view = view;
        output->emit_signal("wm-actions-above-changed", &data);

        return true;
    }

    /**
     * Find the selected toplevel view, or nullptr if the selected view is not
     * toplevel.
     */
    wayfire_view choose_view(wf::activator_source_t source)
    {
        wayfire_view view;
        if (source == wf::activator_source_t::BUTTONBINDING)
        {
            view = wf::get_core().get_cursor_focus_view();
        }

        view = output->get_active_view();
        if (!view || (view->role != wf::VIEW_ROLE_TOPLEVEL))
        {
            return nullptr;
        } else
        {
            return view;
        }
    }

    /**
     * Calling a specific view / specific keep_above action via signal
     */
    wf::signal_connection_t on_set_above_state_signal =
    {[=] (wf::signal_data_t *data)
        {
            auto signal = static_cast<wf::wm_actions_set_above_state*>(data);

            if (!set_keep_above_state(signal->view, signal->above))
            {
                LOG(wf::log::LOG_LEVEL_DEBUG,
                    "view above action failed via signal.");
            }
        }
    };

    /**
     * Ensures views marked as above are still above if their output changes.
     */
    wf::signal_connection_t on_view_output_changed
    {[=] (wf::signal_data_t *data)
        {
            auto signal = static_cast<wf::view_moved_to_output_signal*>(data);
            if (signal->new_output != output)
            {
                return;
            }

            auto view = signal->view;

            if (!view)
            {
                return;
            }

            if (view->has_data("wm-actions-above"))
            {
                wf::scene::remove_child(view->get_root_node());
                wf::scene::add_front(always_above, view->get_root_node());
            }
        }
    };

    /**
     * Ensures views marked as above are still above if they are minimized and
     * unminimized.
     */
    wf::signal_connection_t on_view_minimized
    {[=] (wf::signal_data_t *data)
        {
            auto signal = static_cast<wf::view_minimized_signal*>(data);
            auto view   = signal->view;

            if (!view)
            {
                return;
            }

            if (view->get_output() != output)
            {
                return;
            }

            if (view->has_data("wm-actions-above") && (signal->state == false))
            {
                wf::scene::remove_child(view->get_root_node());
                wf::scene::add_front(always_above, view->get_root_node());
            }
        }
    };

    /**
     * Disables show desktop if the workspace is changed or any view is attached,
     * mapped or unminimized.
     */
    wf::signal_connection_t view_attached = [this] (wf::signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        if ((view->role != wf::VIEW_ROLE_TOPLEVEL) || !view->is_mapped())
        {
            return;
        }

        disable_showdesktop();
    };

    wf::signal_connection_t workspace_changed = [this] (wf::signal_data_t *data)
    {
        disable_showdesktop();
    };

    wf::signal_connection_t view_minimized = [this] (wf::signal_data_t *data)
    {
        auto ev = static_cast<wf::view_minimized_signal*>(data);

        if ((ev->view->role != wf::VIEW_ROLE_TOPLEVEL) || !ev->view->is_mapped())
        {
            return;
        }

        if (!ev->state)
        {
            disable_showdesktop();
        }
    };

    /**
     * Execute for_view on the selected view, if available.
     */
    bool execute_for_selected_view(wf::activator_source_t source,
        std::function<bool(wayfire_view)> for_view)
    {
        auto view = choose_view(source);
        if (!view || !output->can_activate_plugin(this->grab_interface))
        {
            return false;
        }

        return for_view(view);
    }

    /**
     * The default activator bindings.
     */
    wf::activator_callback on_toggle_above = [=] (auto ev) -> bool
    {
        auto view = choose_view(ev.source);

        return set_keep_above_state(view, !view->has_data("wm-actions-above"));
    };

    wf::activator_callback on_minimize = [=] (auto ev) -> bool
    {
        return execute_for_selected_view(ev.source, [] (wayfire_view view)
        {
            view->minimize_request(!view->minimized);
            return true;
        });
    };

    wf::activator_callback on_toggle_maximize = [=] (auto ev) -> bool
    {
        return execute_for_selected_view(ev.source, [] (wayfire_view view)
        {
            view->tile_request(view->tiled_edges ==
                wf::TILED_EDGES_ALL ? 0 : wf::TILED_EDGES_ALL);
            return true;
        });
    };

    wf::activator_callback on_toggle_fullscreen = [=] (auto ev) -> bool
    {
        return execute_for_selected_view(ev.source, [] (wayfire_view view)
        {
            view->fullscreen_request(view->get_output(), !view->fullscreen);
            return true;
        });
    };

    wf::activator_callback on_toggle_sticky = [=] (auto ev) -> bool
    {
        return execute_for_selected_view(ev.source, [] (wayfire_view view)
        {
            view->set_sticky(view->sticky ^ 1);
            return true;
        });
    };

    wf::activator_callback on_toggle_showdesktop = [=] (auto ev) -> bool
    {
        showdesktop_active = !showdesktop_active;

        if (showdesktop_active)
        {
            for (auto& view : output->workspace->get_views_in_layer(wf::WM_LAYERS))
            {
                if (!view->minimized)
                {
                    view->minimize_request(true);
                    view->store_data(
                        std::make_unique<wf::custom_data_t>(),
                        "wm-actions-showdesktop");
                }
            }

            output->connect_signal("view-layer-attached", &view_attached);
            output->connect_signal("view-mapped", &view_attached);
            output->connect_signal("workspace-changed", &workspace_changed);
            output->connect_signal("view-minimized", &view_minimized);
            return true;
        }

        disable_showdesktop();

        return true;
    };

    void do_send_to_back(wayfire_view view)
    {
        auto view_root = view->get_root_node();

        if (auto parent =
                dynamic_cast<wf::scene::floating_inner_node_t*>(view_root->parent()))
        {
            auto parent_children = parent->get_children();
            parent_children.erase(
                std::remove(parent_children.begin(), parent_children.end(),
                    view_root),
                parent_children.end());
            parent_children.push_back(view_root);
            parent->set_children_list(parent_children);
            wf::scene::update(parent->shared_from_this(),
                wf::scene::update_flag::CHILDREN_LIST);
        }
    }

    wf::activator_callback on_send_to_back = [=] (auto ev) -> bool
    {
        return execute_for_selected_view(ev.source, [this] (wayfire_view view)
        {
            auto ws    = view->get_output()->workspace->get_current_workspace();
            auto views =
                view->get_output()->workspace->get_views_on_workspace(ws,
                    wf::LAYER_WORKSPACE);
            wayfire_view bottom_view = views[views.size() - 1];
            if (view != bottom_view)
            {
                do_send_to_back(view);
                // Change focus to the last focused view on this workspace
                views =
                    view->get_output()->workspace->get_views_on_workspace(ws,
                        wf::LAYER_WORKSPACE);
                view->get_output()->focus_view(views[0], false);
            }

            return true;
        });
    };

    void disable_showdesktop()
    {
        view_attached.disconnect();
        workspace_changed.disconnect();
        view_minimized.disconnect();

        for (auto& view : output->workspace->get_views_in_layer(
            wf::ALL_LAYERS, true))
        {
            if (view->has_data("wm-actions-showdesktop"))
            {
                view->erase_data("wm-actions-showdesktop");
                view->minimize_request(false);
            }
        }

        showdesktop_active = false;
    }

  public:
    void init() override
    {
        always_above = std::make_shared<wf::scene::floating_inner_node_t>(false);
        wf::scene::add_front(
            output->node_for_layer(wf::scene::layer::WORKSPACE),
            always_above);

        output->add_activator(toggle_showdesktop, &on_toggle_showdesktop);
        output->add_activator(minimize, &on_minimize);
        output->add_activator(toggle_maximize, &on_toggle_maximize);
        output->add_activator(toggle_above, &on_toggle_above);
        output->add_activator(toggle_fullscreen, &on_toggle_fullscreen);
        output->add_activator(toggle_sticky, &on_toggle_sticky);
        output->add_activator(send_to_back, &on_send_to_back);
        output->connect_signal("wm-actions-set-above-state",
            &on_set_above_state_signal);
        output->connect_signal("view-minimized", &on_view_minimized);
        wf::get_core().connect_signal("view-moved-to-output",
            &on_view_output_changed);
    }

    void fini() override
    {
        for (auto view : output->workspace->get_views_in_layer(wf::ALL_LAYERS, true))
        {
            if (view->has_data("wm-actions-above"))
            {
                set_keep_above_state(view, false);
            }
        }

        wf::scene::remove_child(always_above);
        output->rem_binding(&on_toggle_showdesktop);
        output->rem_binding(&on_minimize);
        output->rem_binding(&on_toggle_maximize);
        output->rem_binding(&on_toggle_above);
        output->rem_binding(&on_toggle_fullscreen);
        output->rem_binding(&on_toggle_sticky);
        output->rem_binding(&on_send_to_back);
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_wm_actions_t);
