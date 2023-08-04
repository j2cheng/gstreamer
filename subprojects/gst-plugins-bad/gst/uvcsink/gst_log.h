#pragma once

#include <gst/gst.h>
#include <gst/gstinfo.h>

#define LOG_ERROR(fmt, ...) GST_ERROR(fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) GST_WARNING(fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) GST_INFO(fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) GST_DEBUG(fmt, ##__VA_ARGS__)
#define LOG_TRACE(fmt, ...) GST_TRACE(fmt, ##__VA_ARGS__)
