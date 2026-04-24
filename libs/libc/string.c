int strlen(const char *str) {
    int i = 0;
    while (*str) {
        str++;
        i++;
    }
    return i;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, int n) {
    char *d = dest;
    while (n-- && (*d++ = *src++));
    while (n-- > 0) *d++ = '\0';
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, int n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && (*d++ = *src++));
    *d = '\0';
    return dest;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, int n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    if (n < 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strchr(const char *str, int c) {
    while (*str) {
        if (*str == (char)c) return (char *)str;
        str++;
    }
    return (c == '\0') ? (char *)str : (char *)0;
}

char *strrchr(const char *str, int c) {
    const char *last = (char *)0;
    while (*str) {
        if (*str == (char)c) last = str;
        str++;
    }
    return (c == '\0') ? (char *)str : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    while (*haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
        haystack++;
    }
    return (char *)0;
}

int strspn(const char *str, const char *accept) {
    int count = 0;
    while (*str) {
        const char *a = accept;
        int found = 0;
        while (*a) { if (*str == *a++) { found = 1; break; } }
        if (!found) break;
        str++; count++;
    }
    return count;
}

int strcspn(const char *str, const char *reject) {
    int count = 0;
    while (*str) {
        const char *r = reject;
        while (*r) { if (*str == *r++) goto done; }
        str++; count++;
    }
    done:
    return count;
}

void *memset(void *ptr, int c, int n) {
    unsigned char *p = ptr;
    while (n--) *p++ = (unsigned char)c;
    return ptr;
}

void *memcpy(void *dest, const void *src, int n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, int n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *a, const void *b, int n) {
    const unsigned char *p = a, *q = b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++; q++;
    }
    return 0;
}

void *memchr(const void *ptr, int c, int n) {
    const unsigned char *p = ptr;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return (void *)0;
}