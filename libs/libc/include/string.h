#pragma once

int   strlen(const char *str);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, int n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, int n);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, int n);
char *strchr(const char *str, int c);
char *strrchr(const char *str, int c);
char *strstr(const char *haystack, const char *needle);
int   strspn(const char *str, const char *accept);
int   strcspn(const char *str, const char *reject);
void *memset(void *ptr, int c, int n);
void *memcpy(void *dest, const void *src, int n);
void *memmove(void *dest, const void *src, int n);
int   memcmp(const void *a, const void *b, int n);
void *memchr(const void *ptr, int c, int n);
