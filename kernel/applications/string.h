#ifndef STRING_H
#define STRING_H
#include <stdint.h>
#include <stddef.h>

static inline void* memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t*)s;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return s;
}

static inline void* memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dest;
    const uint8_t *s = (const uint8_t*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

static inline void* memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dest;
    const uint8_t *s = (const uint8_t*)src;
    
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

static inline int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t*)s1;
    const uint8_t *p2 = (const uint8_t*)s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

// String functions
static inline size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) {
        len++;
    }
    return len;
}

static inline char* strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

static inline char* strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*d = *src)) {
        d++;
        src++;
        n--;
    }
    while (n--) {
        *d++ = '\0';
    }
    return dest;
}

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}

static inline int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}

static inline char* strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) {
        d++;
    }
    while ((*d++ = *src++));
    return dest;
}

static inline char* strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) {
        d++;
    }
    while (n-- && (*d = *src)) {
        d++;
        src++;
    }
    *d = '\0';
    return dest;
}

static inline char* strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char*)s;
        }
        s++;
    }
    return (*s == (char)c) ? (char*)s : NULL;
}

static inline char* strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    if (*s == (char)c) {
        return (char*)s;
    }
    return (char*)last;
}

static inline char* strstr(const char *haystack, const char *needle) {
    if (!*needle) {
        return (char*)haystack;
    }
    
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        
        if (!*n) {
            return (char*)haystack;
        }
        
        haystack++;
    }
    return NULL;
}

static inline void* memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t*)s;
    while (n--) {
        if (*p == (uint8_t)c) {
            return (void*)p;
        }
        p++;
    }
    return NULL;
}

static inline size_t strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && *s++) {
        len++;
    }
    return len;
}

#endif