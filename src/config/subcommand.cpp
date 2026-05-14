//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <argparse/argparse.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <config/Config.h>
#include <config/edit.h>
#include <config/subcommand.h>
#include <config/template.h>
#include <config/validate.h>
#include <config/wizard.h>

using namespace argparse;

namespace mldp_pvxs_driver::config {

int runConfigSubcommand(int argc, char** argv)
{
    ArgumentParser program("config");
    program.add_description(
        "Configuration utilities.\n"
        "\n"
        "Sub-commands:\n"
        "  wizard    Interactive TUI wizard — generate or amend a config.yaml\n"
        "  validate  Validate a YAML file and report field-level errors/warnings\n"
        "  template  Print a ready-to-use YAML template (minimal or full)\n"
        "  list      Show all writers, readers, routing rules and metrics settings\n"
        "  add       Add a writer, reader or routing entry to an existing config\n"
        "  remove    Remove a named writer, reader or routing entry");

    // ── template sub-subcommand ───────────────────────────────────────────
    ArgumentParser cmd_template("template");
    cmd_template.add_description("Print a YAML configuration template to stdout.");
    cmd_template.add_argument("--minimal")
        .help("Print the minimal template (mldp writer + epics-pvxs reader) [default]")
        .default_value(false)
        .implicit_value(true);
    cmd_template.add_argument("--full")
        .help("Print the full template (mldp + hdf5 writers, pvxs + base readers)")
        .default_value(false)
        .implicit_value(true);

    // ── validate sub-subcommand ───────────────────────────────────────────
    ArgumentParser cmd_validate("validate");
    cmd_validate.add_description("Validate a configuration YAML file and report diagnostics.");
    cmd_validate.add_argument("path")
        .help("Path to the configuration YAML file to validate")
        .metavar("PATH");

    // ── wizard sub-subcommand ─────────────────────────────────────────────
    ArgumentParser cmd_wizard("wizard");
    cmd_wizard.add_description("Interactively generate or amend a configuration file using a guided TUI wizard.");
    cmd_wizard.add_argument("--output")
        .help("Path for the generated configuration file")
        .default_value(std::string("config.yaml"))
        .metavar("PATH");
    cmd_wizard.add_argument("--from")
        .help("Seed the wizard from an existing configuration file")
        .default_value(std::string(""))
        .metavar("PATH");

    // ── list sub-subcommand ───────────────────────────────────────────────
    ArgumentParser cmd_list("list");
    cmd_list.add_description("Show all writers, readers, routing rules, and metrics settings.");
    cmd_list.add_argument("path")
        .help("Path to the configuration YAML file")
        .default_value(std::string("config.yaml"))
        .metavar("PATH")
        .nargs(argparse::nargs_pattern::optional);

    // ── remove sub-subcommand ─────────────────────────────────────────────
    ArgumentParser cmd_remove("remove");
    cmd_remove.add_description("Remove a named writer, reader, or routing entry.");
    cmd_remove.add_argument("kind")
        .help("Entry type: writer, reader, or routing")
        .metavar("KIND");
    cmd_remove.add_argument("path")
        .help("Path to the configuration YAML file [default: config.yaml]")
        .metavar("PATH");
    cmd_remove.add_argument("--name")
        .help("Name of the entry to remove")
        .required()
        .metavar("NAME");
    cmd_remove.add_argument("--no-backup")
        .help("Skip writing .bak file")
        .default_value(false)
        .implicit_value(true);
    cmd_remove.add_argument("--dry-run")
        .help("Print resulting YAML without modifying the file")
        .default_value(false)
        .implicit_value(true);

    // ── add sub-subcommand ────────────────────────────────────────────────
    ArgumentParser cmd_add("add");
    cmd_add.add_description("Add a writer, reader, or routing entry to an existing config (interactive).");
    cmd_add.add_argument("kind")
        .help("Entry type: writer, reader, or routing (optional — prompted when omitted)")
        .metavar("KIND")
        .nargs(argparse::nargs_pattern::optional)
        .default_value(std::string(""));
    cmd_add.add_argument("path")
        .help("Path to the configuration YAML file")
        .metavar("PATH");
    cmd_add.add_argument("--no-backup")
        .default_value(false).implicit_value(true);
    cmd_add.add_argument("--dry-run")
        .default_value(false).implicit_value(true);

    program.add_subparser(cmd_template);
    program.add_subparser(cmd_validate);
    program.add_subparser(cmd_wizard);
    program.add_subparser(cmd_list);
    program.add_subparser(cmd_remove);
    program.add_subparser(cmd_add);

    try
    {
        program.parse_args(argc, argv);
    }
    catch (const std::exception& err)
    {
        std::cerr << "Error: " << err.what() << "\n\n";
        std::cerr << program;
        return 1;
    }

    // ── dispatch ──────────────────────────────────────────────────────────

    if (program.is_subcommand_used("template"))
    {
        const bool full    = cmd_template.get<bool>("--full");
        const bool minimal = cmd_template.get<bool>("--minimal");

        TemplateKind kind = TemplateKind::MldpOnly; // default
        if (full && !minimal)
        {
            kind = TemplateKind::MldpAndHdf5;
        }

        std::cout << getConfigTemplate(kind);
        return 0;
    }

    if (program.is_subcommand_used("validate"))
    {
        const std::string path = cmd_validate.get<std::string>("path");

        // Check the file is readable and non-empty before handing to ryml,
        // which aborts (assert) on a completely empty YAML document.
        {
            std::ifstream probe{path};
            if (!probe) {
                std::cerr << "ERROR  " << path << "  cannot open file\n";
                return 1;
            }
            probe.seekg(0, std::ios::end);
            if (probe.tellg() == 0) {
                std::cout << "ERROR  <root>  file is empty — no YAML content\n";
                std::cout << "FAIL  " << path << " — 1 errors, 0 warnings\n";
                return 1;
            }
        }

        Config cfg;
        try
        {
            cfg = Config::configFromFile(path);
        }
        catch (const std::exception& err)
        {
            std::cerr << "ERROR  " << err.what() << "\n";
            return 1;
        }

        const auto diagnostics = validateConfig(cfg);

        int error_count = 0;
        int warn_count  = 0;

        for (const auto& diag : diagnostics)
        {
            if (diag.severity == ConfigDiagnostic::Severity::ERROR)
            {
                ++error_count;
                std::cout << "ERROR  " << diag.field_path << "  " << diag.message << "\n";
            }
            else
            {
                ++warn_count;
                std::cout << "WARN   " << diag.field_path << "  " << diag.message << "\n";
            }
        }

        if (error_count == 0)
        {
            std::cout << "OK    " << path << " — " << error_count << " errors, " << warn_count << " warnings\n";
            return 0;
        }
        else
        {
            std::cout << "FAIL  " << path << " — " << error_count << " errors, " << warn_count << " warnings\n";
            return 1;
        }
    }

    if (program.is_subcommand_used("wizard"))
    {
        const std::string output = cmd_wizard.get<std::string>("--output");
        const std::string from   = cmd_wizard.get<std::string>("--from");
        return runWizard(output, from);
    }

    if (program.is_subcommand_used("list"))
    {
        EditListOptions opts;
        opts.path = cmd_list.get<std::string>("path");
        return runList(opts);
    }

    if (program.is_subcommand_used("remove"))
    {
        EditRemoveOptions opts;
        opts.path      = cmd_remove.get<std::string>("path");
        opts.kind      = cmd_remove.get<std::string>("kind");
        opts.name      = cmd_remove.get<std::string>("--name");
        opts.no_backup = cmd_remove.get<bool>("--no-backup");
        opts.dry_run   = cmd_remove.get<bool>("--dry-run");
        return runRemove(opts);
    }

    if (program.is_subcommand_used("add"))
    {
        const std::string add_path = cmd_add.get<std::string>("path");
        const std::string add_kind = cmd_add.get<std::string>("kind");

#ifdef MLDP_WIZARD_ENABLED
        return runAddInteractive(
            add_path, add_kind,
            cmd_add.get<bool>("--no-backup"),
            cmd_add.get<bool>("--dry-run"));
#else
        std::cerr << "ERROR  'add' requires interactive mode (build with MLDP_WIZARD=ON)\n";
        return 1;
#endif
    }

    // No subcommand given — print help.
    std::cerr << program;
    return 1;
}

} // namespace mldp_pvxs_driver::config
