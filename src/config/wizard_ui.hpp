//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

// FTXUI headers (only included in wizard/FTXUI contexts)
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "wizard_tree_types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config::wizard_ui {

using namespace ftxui;

// Sidebar: Menu with Unicode tree chars, cyan focus highlight
// selected_index is owned by caller
Component SidebarPanel(const std::vector<TreeNode>* nodes, int* selected_index);

// ---------------------------------------------------------------------------
// InputField — single-line input with optional validator
// validator returns "" if valid, else error message
// ---------------------------------------------------------------------------
inline Component InputField(
    const std::string&                             label,
    std::string*                                   value,
    std::function<std::string(const std::string&)> validator = {},
    std::function<void()>                          on_change = {},
    std::function<void()>                          on_focus = {})
{
    auto opt = InputOption::Default();
    opt.multiline = false;
    opt.on_change = on_change ? on_change : [] {};

    auto input = Input(value, label, opt);

    // self_ref lets the render lambda call Focused() on this specific Renderer.
    // ComponentBase::Focused() returns true when parent_->ActiveChild() == this,
    // which is set by Container::Vertical when this field is the active child.
    auto self_ref = std::make_shared<Component>();
    auto result = Renderer(input, [=]() -> Element
                           {
                               if (on_focus && *self_ref && (*self_ref)->Focused())
                                   on_focus();
                               std::string err = validator ? validator(*value) : "";
                               Element     field = input->Render();
                               if (!err.empty())
                               {
                                   field = field | color(Color::Red);
                                   return vbox({hbox({text(label + ": "), field}), text("  ✗ " + err) | color(Color::Red)});
                               }
                               return hbox({text(label + ": "), field});
                           });
    *self_ref = result;
    return result;
}

// ---------------------------------------------------------------------------
// TypeMenu — vertical arrow-key menu with styled focus highlight
// ---------------------------------------------------------------------------
inline Component TypeMenu(const std::vector<std::string>* choices, int* selected)
{
    auto opt = MenuOption::Vertical();
    opt.entries_option.transform = [](const EntryState& s) -> Element
    {
        auto e = text((s.focused ? "  > " : "    ") + s.label);
        if (s.focused)
            return e | bold | color(Color::Cyan);
        if (s.active)
            return e | bold;
        return e | color(Color::GrayLight);
    };
    return Menu(choices, selected, opt);
}

// ---------------------------------------------------------------------------
// MultiSelectList — vertical list of checkboxes
// items and selected must have the same size and outlive the component.
// Uses std::vector<int> (not std::vector<bool>) to avoid proxy-ref issues.
// ---------------------------------------------------------------------------
inline Component MultiSelectList(
    const std::vector<std::string>* items,
    std::vector<int>*               selected)
{
    Components boxes;
    for (std::size_t i = 0; i < items->size(); ++i)
    {
        boxes.push_back(Checkbox(&(*items)[i],
                                 reinterpret_cast<bool*>((*selected).data() + i)));
    }
    return Container::Vertical(std::move(boxes));
}

} // namespace mldp_pvxs_driver::config::wizard_ui
