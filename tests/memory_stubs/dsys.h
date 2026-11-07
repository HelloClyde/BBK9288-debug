#ifndef D88_MEMORY_STUB_DSYS_H
#define D88_MEMORY_STUB_DSYS_H
typedef long T_WORD;
typedef char T_BYTE;
#ifdef HWND_DESKTOP
#undef HWND_DESKTOP
#endif
#ifdef MB_OK
#undef MB_OK
#endif
#define HWND_DESKTOP 0u
#define MB_OK 0u
typedef struct {int(*MessageBox)(unsigned,const char*,const char*,unsigned);} T_GUI_RelocationTable;
typedef struct {int(*ros33_dly_tsk)(unsigned);} T_ROS33_RelocationTable;
extern T_GUI_RelocationTable *tpDL_GUITable;
extern T_ROS33_RelocationTable *tpDL_ROS33Table;
#endif
