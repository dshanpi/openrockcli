#ifndef OPENROCKCLI_FILE_PORTABLE_H
#define OPENROCKCLI_FILE_PORTABLE_H
#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
#include <wchar.h>
wchar_t *rk_wide_from_utf8(const char *text);
char *rk_utf8_from_wide(const wchar_t *text);
#endif
FILE *rk_fopen(const char *path, const char *mode);
FILE *rk_create_file(const char *path);
int rk_file_size(const char *path, uint64_t *size);
int rk_make_directory(const char *path);
#endif
