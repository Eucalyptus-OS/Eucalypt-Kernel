#ifndef STDIO_H
#define STDIO_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "sys.h"

static inline int putchar(int c) {
    char s[2] = {(char)c, '\0'};
    print(s);
    return c;
}

static inline int puts(const char *s) {
    print(s);
    putchar('\n');
    return 0;
}

static inline void reverse(char *str, int len) {
    int i = 0, j = len - 1;
    while (i < j) {
        char temp = str[i];
        str[i] = str[j];
        str[j] = temp;
        i++;
        j--;
    }
}

static inline int itoa(int num, char *str, int base) {
    int i = 0;
    int is_neg = 0;
    
    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return i;
    }
    
    if (num < 0 && base == 10) {
        is_neg = 1;
        num = -num;
    }
    
    while (num != 0) {
        int rem = num % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        num = num / base;
    }
    
    if (is_neg) {
        str[i++] = '-';
    }
    
    str[i] = '\0';
    reverse(str, i);
    return i;
}

static inline int utoa(uint64_t num, char *str, int base) {
    int i = 0;
    
    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return i;
    }
    
    while (num != 0) {
        uint64_t rem = num % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        num = num / base;
    }
    
    str[i] = '\0';
    reverse(str, i);
    return i;
}

static inline void printf_internal(const char *format, va_list args) {
    char buffer[64];
    
    while (*format) {
        if (*format == '%') {
            format++;
            switch (*format) {
                case 'd':
                case 'i': {
                    int val = va_arg(args, int);
                    itoa(val, buffer, 10);
                    print(buffer);
                    break;
                }
                case 'u': {
                    unsigned int val = va_arg(args, unsigned int);
                    utoa(val, buffer, 10);
                    print(buffer);
                    break;
                }
                case 'x': {
                    unsigned int val = va_arg(args, unsigned int);
                    print("0x");
                    utoa(val, buffer, 16);
                    print(buffer);
                    break;
                }
                case 'p': {
                    void *val = va_arg(args, void*);
                    print("0x");
                    utoa((uint64_t)val, buffer, 16);
                    print(buffer);
                    break;
                }
                case 's': {
                    char *val = va_arg(args, char*);
                    print(val ? val : "(null)");
                    break;
                }
                case 'c': {
                    char val = (char)va_arg(args, int);
                    char s[2] = {val, '\0'};
                    print(s);
                    break;
                }
                case '%': {
                    putchar('%');
                    break;
                }
                default:
                    putchar('%');
                    putchar(*format);
                    break;
            }
        } else {
            putchar(*format);
        }
        format++;
    }
}

static inline void printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printf_internal(format, args);
    va_end(args);
}

static inline int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    char *dest = str;
    char buffer[64];
    
    while (*format) {
        if (*format == '%') {
            format++;
            char *temp = NULL;
            int len = 0;
            
            switch (*format) {
                case 'd':
                case 'i': {
                    int val = va_arg(args, int);
                    len = itoa(val, buffer, 10);
                    temp = buffer;
                    break;
                }
                case 'u': {
                    unsigned int val = va_arg(args, unsigned int);
                    len = utoa(val, buffer, 10);
                    temp = buffer;
                    break;
                }
                case 'x': {
                    unsigned int val = va_arg(args, unsigned int);
                    *dest++ = '0';
                    *dest++ = 'x';
                    len = utoa(val, buffer, 16);
                    temp = buffer;
                    break;
                }
                case 's': {
                    temp = va_arg(args, char*);
                    if (!temp) temp = "(null)";
                    while (*temp) {
                        *dest++ = *temp++;
                    }
                    break;
                }
                case 'c': {
                    *dest++ = (char)va_arg(args, int);
                    break;
                }
                case '%': {
                    *dest++ = '%';
                    break;
                }
            }
            
            if (temp && len > 0) {
                for (int i = 0; i < len; i++) {
                    *dest++ = temp[i];
                }
            }
        } else {
            *dest++ = *format;
        }
        format++;
    }
    
    *dest = '\0';
    va_end(args);
    return dest - str;
}

#endif