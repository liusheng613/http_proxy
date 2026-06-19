#include "logger.h"

namespace tunnel {

namespace {
LogLevel g_log_level = LOG_INFO;
}

void set_log_level(LogLevel level) { g_log_level = level; }
LogLevel get_log_level() { return g_log_level; }

}  // namespace tunnel
