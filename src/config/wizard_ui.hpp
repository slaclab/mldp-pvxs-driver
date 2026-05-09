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

// FTXUI headers
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config::wizard_ui {

using namespace ftxui;

// ---------------------------------------------------------------------------
// PhaseHeader — non-interactive banner showing "Phase N of M — title"
// ---------------------------------------------------------------------------
inline Element PhaseHeader(const std::string& title, int phase, int total)
{
    return hbox({
        text(" Phase " + std::to_string(phase) + " of " + std::to_string(total)),
        text(" — "),
        text(title) | bold,
        text(" "),
    }) | inverted;
}

// ---------------------------------------------------------------------------
// InputField — single-line input with optional validator
// validator returns "" if valid, else error message
// ---------------------------------------------------------------------------
inline Component InputField(
    const std::string&                           label,
    std::string*                                 value,
    std::function<std::string(const std::string&)> validator = {},
    std::function<void()>                        on_change  = {})
{
    auto opt = InputOption::Default();
    opt.multiline  = false;
    opt.on_change  = on_change ? on_change : []{};

    auto input = Input(value, label, opt);

    // Wrap in Renderer that shows label + field + optional error
    return Renderer(input, [=]() -> Element {
        std::string err = validator ? validator(*value) : "";
        Element field = input->Render();
        if (!err.empty()) {
            field = field | color(Color::Red);
            return vbox({ hbox({text(label + ": "), field}), text("  ✗ " + err) | color(Color::Red) });
        }
        return hbox({text(label + ": "), field});
    });
}

// ---------------------------------------------------------------------------
// TypeMenu — vertical arrow-key menu for type selection
// ---------------------------------------------------------------------------
inline Component TypeMenu(const std::vector<std::string>* choices, int* selected)
{
    return Menu(choices, selected, MenuOption::Vertical());
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
    for (std::size_t i = 0; i < items->size(); ++i) {
        boxes.push_back(Checkbox(&(*items)[i],
                                 reinterpret_cast<bool*>((*selected).data() + i)));
    }
    return Container::Vertical(std::move(boxes));
}

// ---------------------------------------------------------------------------
// ConfirmButton — a simple OK button that calls a callback and exits screen
// ---------------------------------------------------------------------------
inline Component ConfirmButton(
    const std::string&    label,
    std::function<void()> on_confirm,
    ScreenInteractive*    screen)
{
    return Button(label, [=]{ on_confirm(); screen->Exit(); }, ButtonOption::Simple());
}

// ---------------------------------------------------------------------------
// YesNoToggle — renders "Yes / No" as two buttons; sets *result to true/false
// ---------------------------------------------------------------------------
inline Component YesNoButtons(
    bool*                 result,
    std::function<void()> on_done,
    ScreenInteractive*    screen)
{
    auto yes = Button("Yes", [=]{ *result = true;  on_done(); screen->Exit(); });
    auto no  = Button("No",  [=]{ *result = false; on_done(); screen->Exit(); });
    return Container::Horizontal({yes, no});
}

} // namespace mldp_pvxs_driver::config::wizard_ui
