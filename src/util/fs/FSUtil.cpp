//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <util/fs/FSUtil.h>

#include <regex>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;
using namespace mldp_pvxs_driver::util::fsutil;

namespace {

bool containsGlobTokens(std::string_view value)
{
    return value.find('*') != std::string_view::npos ||
           value.find('?') != std::string_view::npos;
}

std::string escapeRegex(char c)
{
    static constexpr std::string_view special = R"(\.^$|()[]{}+)";

    if (special.find(c) != std::string_view::npos)
    {
        return std::string("\\") + c;
    }

    return std::string(1, c);
}

std::string globToRegex(std::string_view glob)
{
    std::string regex = "^";

    for (std::size_t index = 0; index < glob.size(); ++index)
    {
        const char c = glob[index];

        if (c == '*')
        {
            if (index + 1 < glob.size() && glob[index + 1] == '*')
            {
                if (index + 2 < glob.size() &&
                    (glob[index + 2] == '/' || glob[index + 2] == '\\'))
                {
                    regex += R"((?:.*[/\\])?)";
                    index += 2;
                }
                else
                {
                    regex += ".*";
                    ++index;
                }
            }
            else
            {
                regex += "[^/]*";
            }

            continue;
        }

        if (c == '?')
        {
            regex += "[^/]";
            continue;
        }

        if (c == '/' || c == '\\')
        {
            regex += R"([/\\])";
            continue;
        }

        regex += escapeRegex(c);
    }

    regex += "$";
    return regex;
}

fs::path extractSearchRoot(const fs::path& pattern)
{
    fs::path root = pattern.is_absolute() ? pattern.root_path() : fs::path{};

    for (const auto& part : pattern.relative_path())
    {
        const auto value = part.string();
        if (containsGlobTokens(value))
        {
            break;
        }

        root /= part;
    }

    return root.empty() ? fs::path(".") : root;
}

} // namespace

std::set<fs::path> FSUtil::findFilesByGlob(const fs::path& globPattern)
{
    std::set<fs::path> result;
    if (globPattern.empty())
    {
        return result;
    }

    std::error_code error;
    const fs::path  patternPath = globPattern.lexically_normal();
    const fs::path  searchRoot = extractSearchRoot(patternPath);

    if (!fs::exists(searchRoot, error) || error)
    {
        return result;
    }

    const fs::path absolutePatternPath = fs::absolute(patternPath, error).lexically_normal();
    if (error)
    {
        return result;
    }

    const std::string absolutePattern = absolutePatternPath.generic_string();
    const std::regex  matcher(globToRegex(absolutePattern), std::regex::ECMAScript);

    if (fs::is_regular_file(searchRoot, error))
    {
        if (!error)
        {
            const auto candidate = fs::absolute(searchRoot, error).lexically_normal().generic_string();
            if (!error && std::regex_match(candidate, matcher))
            {
                result.insert(searchRoot);
            }
        }

        return result;
    }

    const auto options = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator it(searchRoot, options, error), end; it != end; it.increment(error))
    {
        if (error)
        {
            error.clear();
            continue;
        }

        const auto& entry = *it;
        if (!entry.is_regular_file(error) || error)
        {
            error.clear();
            continue;
        }

        const auto candidate = fs::absolute(entry.path(), error).lexically_normal().generic_string();
        if (!error && std::regex_match(candidate, matcher))
        {
            result.insert(entry.path());
        }
    }

    return result;
}
