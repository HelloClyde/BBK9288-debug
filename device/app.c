/* Foreground experimental server; USB takeover remains gated in usb_cdc.c. */
#include "dsys.h"
#include "debug_core.h"
#include "usb_cdc.h"
#ifndef DEBUG_IMAGE_ID
#define DEBUG_IMAGE_ID 0u
#endif

static dbg_state state;
static unsigned char snapshot[320u*240u];
static T_GUI_HWND main_window;
static unsigned running, session, tx_offset, stop_queued;
static unsigned char incoming[64];
static unsigned incoming_count, incoming_offset;
static unsigned pending_scan, pending_up;

static dbg_u32 ticks(void *p)
{ (void)p; return 0; } /* Firmware tick unit is unverified; flag bit2 marks unavailable. */
static int read_ram(void *p, dbg_u32 a, unsigned char *out, dbg_u32 n)
{
    unsigned i;
    (void)p;
    if (a<0x02000000u || a>=0x02800000u || n>0x02800000u-a) return -1;
    for(i=0;i<n;i++) out[i]=*(volatile unsigned char *)(a+i);
    return 0;
}
static int key(void *p, dbg_u32 scan)
{
    (void)p;
    if (!scan || scan>127u) return -(int)DBG_INVALID;
    if (pending_scan || pending_up) return -(int)DBG_BUSY;
    pending_scan=scan;
    return 0;
}
static int screen(void *p, unsigned char *out, dbg_u32 capacity, dbg_screen *m)
{
    unsigned mode=*(volatile unsigned char*)0x00380200u;
    unsigned bpp=1u<<(mode&7u), x,y,i,v,shift,off;
    unsigned char palette[256];
    (void)p;
    if ((mode&7u)>2u || capacity<320u*240u) return -1;
    /* This profile supports the original 320x240 display beginning at IVRAM0. */
    if ((*(volatile unsigned short*)0x00380042u+1u)*8u!=320u ||
        *(volatile unsigned short*)0x0038004cu+1u!=240u ||
        *(volatile unsigned short*)0x00380210u!=0u) return -1;
    for(i=0;i<(1u<<bpp);i++) {
        if(mode&0x10u) palette[i]=(unsigned char)(bpp<8u ? i*(bpp==1u?255u:(bpp==2u?85u:17u)) : ((i>>2)<<2)|(i>>6));
        else { v=*(volatile unsigned char*)(0x00380401u+i*4u); palette[i]=(unsigned char)((v&0xfcu)|(v>>6)); }
    }
    for(y=0;y<240;y++) for(x=0;x<320;x++) {
        off=y*(320u*bpp/8u)+x*bpp/8u;
        shift=8u-bpp-((x*bpp)&7u);
        out[y*320u+x]=palette[(*(volatile unsigned char*)(0x003c0000u+off)>>shift)&((1u<<bpp)-1u)];
    }
    m->width=320; m->height=240; m->bpp=8; m->stride=320; m->format=1; m->flags=0;
    return 320*240;
}
static void poll(void)
{
    unsigned n, consumed;
    int count;
    const unsigned char *reply;
    T_GUI_HWND target;
    if(!running) return;
    usb_poll();
    if(session!=usb_get_session()) {
        session=usb_get_session(); dbg_reset_session(&state);
        tx_offset=0; incoming_count=incoming_offset=0; pending_scan=0;
        /* A DOWN already accepted by the GUI still needs its matching UP. */
    }
    /* GUI thread owns message dispatch and all queue writes. */
    if(pending_up) {
        if(tpDL_GUITable->PostMessage(main_window,MSG_KEYUP,pending_up,0)==0) pending_up=0;
    }
    else if(pending_scan) {
        target=main_window; /* This first build only controls its own foreground window. */
        if(tpDL_GUITable->PostMessage(target,MSG_KEYDOWN,pending_scan,0)==0) {
            pending_up=pending_scan; pending_scan=0;
        }
    }
    reply=dbg_reply(&state,&n);
    if(reply) {
        if(tx_offset<n) {
            count=usb_send(reply+tx_offset,(int)(n-tx_offset));
            if(count>0) tx_offset+=(unsigned)count;
        } else {
            /* usb_tx_idle confirms the final packet/ZLP has been ACKed. */
            if(usb_tx_idle()) { dbg_consume_reply(&state); tx_offset=0; }
        }
    } else if(usb_get_dtr()) {
        if(incoming_offset==incoming_count) {
            count=usb_receive(incoming,sizeof(incoming));
            incoming_count=count>0 ? (unsigned)count : 0u;
            incoming_offset=0;
        }
        if(incoming_count>incoming_offset) {
            consumed=dbg_feed(&state,incoming+incoming_offset,incoming_count-incoming_offset);
            incoming_offset+=consumed;
        }
    }
    if(state.stop_requested && !pending_scan && !pending_up && !stop_queued) {
        if(tpDL_GUITable->PostMessage(main_window,MSG_CLOSE,0,0)==0) stop_queued=1;
    }
}
static T_WORD window_proc(T_GUI_HWND w,T_WORD msg,T_GUI_WPARAM wp,T_GUI_LPARAM lp)
{
    T_GUI_HDC dc;
    switch(msg) {
    case MSG_TIMER: poll(); return 0;
    case MSG_PAINT:
        dc=tpDL_GUITable->GetDC(w);
        tpDL_GUITable->TextOutLen(dc,12,36,(const T_BYTE*)"9288 Debug - experimental",25);
        tpDL_GUITable->TextOutLen(dc,12,64,(const T_BYTE*)"Foreground USB development build",32);
        tpDL_GUITable->TextOutLen(dc,12,92,(const T_BYTE*)"ESC: stop and return",20);
        tpDL_GUITable->ReleaseDC(dc); return 0;
    case MSG_KEYDOWN:
        if(LOUHWORD(wp)==SCANCODE_ESCAPE) tpDL_GUITable->PostMessage(w,MSG_CLOSE,0,0);
        break;
    case MSG_CLOSE:
        running=0; tpDL_GUITable->KillTimer(w,1); usb_stop();
        tpDL_GUITable->DestroyMainWindow(w); tpDL_GUITable->PostQuitMessage(w); return 0;
    }
    return tpDL_GUITable->DefaultMainWinProc(w,msg,wp,lp);
}
T_WORD App_Main(void)
{
    dbg_platform platform;
    T_GUI_MainWinCreate c;
    T_GUI_Msg msg;
    unsigned char *p;
    unsigned i;
    int result;
    if(*(volatile unsigned*)0x02000200u!=0x021806ecu ||
       *(volatile unsigned*)0x02000204u!=0x02180058u) return -1;
    result=usb_start();
    if(result!=0) {
        tpDL_GUITable->MessageBox(0,(const T_BYTE*)"USB ownership not verified. Run DebugProbe first. No USB state changed.",(const T_BYTE*)"9288 Debug",0);
        return result;
    }
    p=(unsigned char*)&platform; for(i=0;i<sizeof(platform);i++) p[i]=0;
    platform.image_id=DEBUG_IMAGE_ID;
    platform.caps=DBG_CAP_ALL; platform.flags=DBG_FLAG_FOREGROUND|DBG_FLAG_HARDWARE_UNVERIFIED|DBG_FLAG_UPTIME_UNAVAILABLE;
    platform.scratch_address=(dbg_u32)state.scratch;
    platform.snapshot=snapshot; platform.snapshot_capacity=sizeof(snapshot);
    platform.uptime_ms=ticks; platform.read=read_ram; platform.key=key; platform.capture=screen;
    dbg_init(&state,&platform);
    p=(unsigned char*)&c; for(i=0;i<sizeof(c);i++) p[i]=0;
    c.dwStyle=WS_VISIBLE|WS_CAPTION; c.spCaption=(const T_BYTE*)"9288 Debug";
    c.MainWindowProc=window_proc; c.rx=320; c.by=240; c.iBkColor=COLOR_LIGHTWHITE;
    main_window=tpDL_GUITable->CreateMainWindow(&c);
    if(main_window==(T_GUI_HWND)HWND_INVALID) { usb_stop(); return -1; }
    running=1; stop_queued=0; session=usb_get_session();
    tpDL_GUITable->SetTimer(main_window,1,1);
    while(running && tpDL_GUITable->GetMessage(&msg,main_window)) {
        tpDL_GUITable->TranslateMessage(&msg); tpDL_GUITable->DispatchMessage(&msg);
        poll();
    }
    usb_stop(); tpDL_GUITable->ThrowAwayMessages(main_window);
    tpDL_GUITable->MainWindowCleanup(main_window);
    return 0;
}
