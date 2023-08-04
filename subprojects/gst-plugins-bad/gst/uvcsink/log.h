#pragma once

#include <stdio.h>

#include <unistd.h>

#ifndef BASE_FILE_NAME
#define BASE_FILE_NAME                                                        ""
#endif

#ifdef __cplusplus
extern "C" {
#endif

int log_level(void);

#ifdef __cplusplus
} // extern "C"
#endif

#define LOG_IMPL(fd, level, prefix, fmt, ...) \
    do \
    { \
        if(log_level() > level) \
        { \
        fprintf( \
            fd, prefix " [%-6d] %s:%-4d %s " fmt "\n", \
            getpid(), BASE_FILE_NAME, __LINE__, __FUNCTION__, ##__VA_ARGS__); \
        } \
    } while(0)

#define log_ERROR(fmt, ...)   LOG_IMPL(stderr, 0, "ERROR", fmt, ##__VA_ARGS__)
#define log_WARNING(fmt, ...) LOG_IMPL(stderr, 1, "WARN ", fmt, ##__VA_ARGS__)
#define log_INFO(fmt, ...)    LOG_IMPL(stdout, 2, "INFO ", fmt, ##__VA_ARGS__)
#define log_DEBUG(fmt, ...)   LOG_IMPL(stdout, 3, "DEBUG", fmt, ##__VA_ARGS__)
#define log_TRACE(fmt, ...)   LOG_IMPL(stdout, 4, "TRACE", fmt, ##__VA_ARGS__)

#ifndef LOG_ERROR
#define LOG_ERROR log_ERROR
#endif

#ifndef LOG_WARNING
#define LOG_WARNING log_WARNING
#endif

#ifndef LOG_INFO
#define LOG_INFO log_INFO
#endif

#ifndef LOG_DEBUG
#define LOG_DEBUG log_DEBUG
#endif

#ifndef LOG_TRACE
#define LOG_TRACE log_TRACE
#endif
