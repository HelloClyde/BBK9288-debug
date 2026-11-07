#ifndef PROBE_MODEL_DSYS_H
#define PROBE_MODEL_DSYS_H
#include <stddef.h>
#include <stdint.h>
typedef int T_WORD;
typedef signed char T_BYTE;
typedef struct FS_FILE FS_FILE;
typedef struct {
    T_WORD (*MessageBox)(int,const T_BYTE*,const T_BYTE*,unsigned);
} T_GUI_RelocationTable;
typedef struct {
    FS_FILE *(*fopen)(const char*,const char*);
    int (*fclose)(FS_FILE*);
    size_t (*fread)(void*,size_t,size_t,FS_FILE*);
    size_t (*fwrite)(const void*,size_t,size_t,FS_FILE*);
} T_FS_RelocationTable;
extern T_GUI_RelocationTable *tpDL_GUITable;
extern T_FS_RelocationTable *tpDL_FSTable;
#define HWND_DESKTOP 0
#define MB_OK 0
#endif
