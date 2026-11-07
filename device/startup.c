#include "dsys.h"

T_GUI_RelocationTable *tpDL_GUITable;
T_CRTL_RelocationTable *tpDL_CRTLTable;
T_FS_RelocationTable *tpDL_FSTable;
T_ROS33_RelocationTable *tpDL_ROS33Table;
T_Audio_RelocationTable *tpDL_AudioTable;
T_Dict_RelocationTable *tpDL_DictTable;

extern unsigned char __bss_start, __bss_end;
extern T_WORD App_Main(void);

__attribute__((section(".text.startup")))
long DL_AppMain(long argument)
{
    volatile unsigned char *cursor = &__bss_start;
    T_GeneralRelocationTable *table =
        (T_GeneralRelocationTable *)0x02000200u;
    (void)argument;
    while (cursor < &__bss_end) *cursor++ = 0;
    tpDL_ROS33Table =
        (T_ROS33_RelocationTable *)table->ROS33_RelocationAddress;
    tpDL_GUITable = (T_GUI_RelocationTable *)table->GUI_RelocationAddress;
    tpDL_FSTable = (T_FS_RelocationTable *)table->FS_RelocationAddress;
    tpDL_CRTLTable = (T_CRTL_RelocationTable *)table->CRTL_RelocationAddress;
    tpDL_AudioTable = (T_Audio_RelocationTable *)table->Audio_RelocationAddress;
    tpDL_DictTable = (T_Dict_RelocationTable *)table->Dict_RelocationAddress;
    return (long)App_Main();
}
