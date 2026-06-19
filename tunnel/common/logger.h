#ifndef TUNNEL_COMMON_LOGGER_H_
#define TUNNEL_COMMON_LOGGER_H_

#include <cstdio>
#include <ctime>
#include <cstring>

// 简易日志宏: 带时间戳、日志级别、文件名与行号
// 用法: LOG_INFO("client %d connected", fd);
// 输出: [2026-06-19 12:00:00] [INFO ] [logger.h:25] client 5 connected

namespace tunnel {

enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
};

// 全局最低输出级别, 默认 INFO。可在 main 里调用 set_log_level 调整。
void set_log_level(LogLevel level);
LogLevel get_log_level();

}  // namespace tunnel

#define LOG(level, fmt, ...)                                                   \
    do {                                                                       \
        if ((level) >= ::tunnel::get_log_level()) {                            \
            char time_buf[20];                                                 \
            time_t now = time(nullptr);                                        \
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",          \
                     localtime(&now));                                         \
            const char* level_str =                                            \
                (level) == ::tunnel::LOG_DEBUG ? "DEBUG" :                     \
                (level) == ::tunnel::LOG_INFO  ? "INFO " :                     \
                (level) == ::tunnel::LOG_WARN  ? "WARN " : "ERROR";            \
            fprintf(stderr, "[%s] [%s] [%s:%d] " fmt "\n", time_buf, level_str,\
                    __FILE__, __LINE__, ##__VA_ARGS__);                        \
        }                                                                      \
    } while (0)

#define LOG_DEBUG(fmt, ...) LOG(::tunnel::LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG(::tunnel::LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG(::tunnel::LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG(::tunnel::LOG_ERROR, fmt, ##__VA_ARGS__)

#endif  // TUNNEL_COMMON_LOGGER_H_
