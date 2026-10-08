#include <file_portable.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
wchar_t *rk_wide_from_utf8(const char *text)
{
 int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
 wchar_t *out = n ? malloc((size_t)n * sizeof(*out)) : NULL;
 if(out && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, n)) {
  free(out); out = NULL;
 }
 return out;
}
char *rk_utf8_from_wide(const wchar_t *text)
{
 int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
 char *out = n ? malloc(n) : NULL;
 if(out && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out, n, NULL, NULL)) {
  free(out); out = NULL;
 }
 return out;
}
#else
#include <unistd.h>
#endif
/* Atomic no-clobber creation also rejects existing symlinks/hardlinks. */
FILE *rk_create_file(const char *path)
{
#ifdef _WIN32
 wchar_t *wide = rk_wide_from_utf8(path);
 if(!wide) return NULL;
 int fd = _wopen(wide, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
 free(wide);
 if(fd < 0) return NULL;
 FILE *file = _fdopen(fd, "wb");
 if(!file) _close(fd);
#else
 int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0666);
 if(fd < 0) return NULL;
 FILE *file = fdopen(fd, "wb");
 if(!file) close(fd);
#endif
 return file;
}
FILE *rk_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
 wchar_t *p = rk_wide_from_utf8(path), *m = rk_wide_from_utf8(mode);
 FILE *f = p && m ? _wfopen(p, m) : NULL;
 free(p); free(m);
 return f;
#else
 return fopen(path, mode);
#endif
}
int rk_file_size(const char *path, uint64_t *size)
{
#ifdef _WIN32
 struct _stat64 st;
 wchar_t *p = rk_wide_from_utf8(path);
 if(!p) return -1;
 int rc = _wstat64(p, &st);
 free(p);
#else
 struct stat st;
 int rc = stat(path, &st);
#endif
 if(rc || !S_ISREG(st.st_mode) || st.st_size < 0) return -1;
 *size = (uint64_t)st.st_size;
 return 0;
}
int rk_make_directory(const char *path)
{
#ifdef _WIN32
 struct _stat64 st;
 wchar_t *p = rk_wide_from_utf8(path);
 if(!p) return -1;
 int rc = _wmkdir(p);
 if(rc && errno == EEXIST && !_wstat64(p, &st) && S_ISDIR(st.st_mode)) rc = 0;
 free(p);
#else
 struct stat st;
 int rc = mkdir(path, 0755);
 if(rc && errno == EEXIST && !stat(path, &st) && S_ISDIR(st.st_mode)) rc = 0;
#endif
 return rc;
}
