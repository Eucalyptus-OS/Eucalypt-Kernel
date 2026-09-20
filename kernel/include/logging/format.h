#pragma once

#include <stdarg.h>

// Callback that consumes one character of formatted output.
typedef void (*format_writer_t)(char ch);

/// Format a string as described by the C specification.
/// @param writer Function to handle output
/// @returns Character count written
int format(format_writer_t writer, const char *format, va_list list);