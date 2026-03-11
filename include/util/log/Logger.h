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

/**
 * @file Logger.h
 * @brief Backwards-compatible include for the logging API.
 *
 * New code should prefer:
 * - <util/log/ILog.h> for the interface only.
 */

#include <util/log/CoutLogger.h>
#include <util/log/ILog.h>
#include <util/StringFormat.h>

#include <string>

namespace mldp_pvxs_driver::util::log {

/**
 * @brief Emit a pre-formatted message using the global logger.
 */
inline void log(Level level, std::string_view message)
{
	auto& logger = getLogger();
	if (!logger.shouldLog(level))
	{
		return;
	}
	logger.log(level, message);
}

/**
 * @brief Format and emit a message using the global logger.
 */
template <typename... Args>
inline void logf(Level level, std::string_view fmt, Args&&... args)
{
	auto& logger = getLogger();
	if (!logger.shouldLog(level))
	{
		return;
	}

#if HAVE_STD_FORMAT
	try
	{
		// libstdc++'s std::make_format_args expects lvalue references.
		// Named parameters are lvalues, so pass without forwarding.
		logger.log(level, std::vformat(fmt, std::make_format_args(args...)));
	}
	catch (const std::format_error& ex)
	{
		logger.log(Level::Error, std::string("Log format error: ") + ex.what());
		logger.log(level, fmt);
	}
#else
	// Fallback using util::format_string for older C++ standards
	logger.log(level, format_string(std::string(fmt), std::forward<Args>(args)...));
#endif
}

// Convenience level helpers (global logger)
inline void trace(std::string_view message) { log(Level::Trace, message); }
inline void debug(std::string_view message) { log(Level::Debug, message); }
inline void info(std::string_view message) { log(Level::Info, message); }
inline void warn(std::string_view message) { log(Level::Warn, message); }
inline void error(std::string_view message) { log(Level::Error, message); }
inline void critical(std::string_view message) { log(Level::Critical, message); }

template <typename... Args>
inline void tracef(std::string_view fmt, Args&&... args) { logf(Level::Trace, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void debugf(std::string_view fmt, Args&&... args) { logf(Level::Debug, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void infof(std::string_view fmt, Args&&... args) { logf(Level::Info, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void warnf(std::string_view fmt, Args&&... args) { logf(Level::Warn, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void errorf(std::string_view fmt, Args&&... args) { logf(Level::Error, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void criticalf(std::string_view fmt, Args&&... args) { logf(Level::Critical, fmt, std::forward<Args>(args)...); }

// Overloads for an explicit logger instance (useful for helper functions)
inline void log(ILogger& logger, Level level, std::string_view message)
{
	if (!logger.shouldLog(level))
	{
		return;
	}
	logger.log(level, message);
}

template <typename... Args>
inline void logf(ILogger& logger, Level level, std::string_view fmt, Args&&... args)
{
	if (!logger.shouldLog(level))
	{
		return;
	}

#if HAVE_STD_FORMAT
	try
	{
		logger.log(level, std::vformat(fmt, std::make_format_args(args...)));
	}
	catch (const std::format_error& ex)
	{
		logger.log(Level::Error, std::string("Log format error: ") + ex.what());
		logger.log(level, fmt);
	}
#else
	// Fallback using util::format_string for older C++ standards
	logger.log(level, format_string(std::string(fmt), std::forward<Args>(args)...));
#endif
}

template <typename... Args>
inline void tracef(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Trace, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void debugf(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Debug, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void infof(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Info, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void warnf(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Warn, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void errorf(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Error, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void criticalf(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Critical, fmt, std::forward<Args>(args)...); }

} // namespace mldp_pvxs_driver::util::log
