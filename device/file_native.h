#ifndef D88_FILE_NATIVE_H
#define D88_FILE_NATIVE_H
/* Opaque native handles. These functions are GUI task operations only. */
typedef struct { unsigned words[136]; } d88_native_find;
int d88_file_native_ready(void);
int d88_file_native_available(void); /* Pure profile/FS-initialized reads. */
int d88_file_native_lock(void);
int d88_file_native_unlock(void);
void *d88_file_native_open(const char *path,const char *mode);
int d88_file_native_close(void *stream);
unsigned d88_file_native_read(void *stream,void *data,unsigned bytes);
unsigned d88_file_native_write(void *stream,const void *data,unsigned bytes);
int d88_file_native_seek(void *stream,unsigned offset,unsigned whence);
int d88_file_native_tell(void *stream);
int d88_file_native_error(void *stream);
int d88_file_native_remove(const char *path);
int d88_file_native_rename(const char *from,const char *to);
int d88_file_native_exists(const char *path); /* 1 file, 0 absent, -1 error/directory. */
int d88_file_native_find_first(const char *pattern,unsigned attributes,d88_native_find *find);
int d88_file_native_find_next(d88_native_find *find);
int d88_file_native_find_close(d88_native_find *find);
unsigned d88_file_native_errno(void);
#endif
