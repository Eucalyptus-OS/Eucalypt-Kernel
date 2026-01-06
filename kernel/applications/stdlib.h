#ifndef STDLIB_H
#define STDLIB_H
#include <stdint.h>
#include <stddef.h>

static inline int atoi(const char *str) {
    int result = 0;
    int sign = 1;
    
    while (*str == ' ' || *str == '\t') {
        str++;
    }
    
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}

static inline long atol(const char *str) {
    long result = 0;
    int sign = 1;
    
    while (*str == ' ' || *str == '\t') {
        str++;
    }
    
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}

static inline int abs(int n) {
    return n < 0 ? -n : n;
}

static inline long labs(long n) {
    return n < 0 ? -n : n;
}

static inline void swap(void *a, void *b, size_t size) {
    uint8_t *p1 = (uint8_t*)a;
    uint8_t *p2 = (uint8_t*)b;
    while (size--) {
        uint8_t temp = *p1;
        *p1++ = *p2;
        *p2++ = temp;
    }
}

static inline int min(int a, int b) {
    return a < b ? a : b;
}

static inline int max(int a, int b) {
    return a > b ? a : b;
}

#define NULL ((void*)0)
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif