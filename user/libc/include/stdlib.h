#pragma once
#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
int atoi(const char *s);
double strtod(const char *s, char **end);
double atof(const char *s);
__attribute__((noreturn)) void exit(int code);
