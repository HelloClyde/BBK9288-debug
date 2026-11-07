/* Host model of the production bridge; no firmware or device executes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define D88_GUI_JOBS_TEST 1
#define DEBUG_IMAGE_ID 0x12345678u
#include "../device/gui_jobs.c"
#define BASE 0x02000000u
#define RING 0x02508000u
static u8 memory[0x800000u];
static u32 psr,gp,step_address,corrupt_on_lock;
static unsigned lock_calls,unlock_calls,post_calls,steps,step_mode;
static int depth,fail_lock,fail_unlock,post_result,step_seen_context;
static u32 accepted[4];

static u8 *at(u32 a,unsigned n)
{assert(a>=BASE && a<0x02800000u && n<=0x02800000u-a);return memory+a-BASE;}
static u32 get(u32 a)
{const u8 *p=at(a,4);return p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);}
static void put(u32 a,u32 v)
{u8 *p=at(a,4);p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24);}
u8 d88_gui_test_read8(u32 a) {return *at(a,1);}
u32 d88_gui_test_read32(u32 a)
{
    if(a>=DESKTOP_QUEUE+0x10u && a<=DESKTOP_QUEUE+0x1cu) assert(depth==1);
    return get(a);
}
void d88_gui_test_write32(u32 a,u32 v) {assert(depth==1 && a==HOOK_SLOT);put(a,v);}
u32 d88_gui_test_psr(void) {return psr;}
u32 d88_gui_test_gp(void) {return gp;}
u32 d88_gui_test_step_address(void (*p)(void)) {(void)p;return step_address;}
int d88_gui_test_lock(void)
{
    lock_calls++;assert(!depth);
    if(fail_lock) {fail_lock=0;return -1;}
    assert((psr&0x710u)==0x10u && *at(0x0206866cu,1)==0 && !(*at(0x0206866du,1)&7u));
    depth=1;psr&=~0x10u;*at(0x0206866du,1)|=3u;
    if(corrupt_on_lock) {*at(corrupt_on_lock,1)^=0x80u;corrupt_on_lock=0;}
    return 0;
}
int d88_gui_test_unlock(void)
{
    unlock_calls++;assert(depth==1);depth=0;psr|=0x10u;*at(0x0206866du,1)&=(u8)~3u;
    if(fail_unlock) {fail_unlock=0;return -1;}
    return 0;
}
int d88_gui_test_post(u32 h,u32 m,u32 n,u32 c)
{
    u32 ring,count,rd,wr,next;
    assert(depth==1 && get(0x02068664u)==WORKER_TCB && h==0);
    post_calls++;
    if(post_result) return post_result;
    ring=get(DESKTOP_QUEUE+16u);count=get(DESKTOP_QUEUE+20u);
    rd=get(DESKTOP_QUEUE+24u);wr=get(DESKTOP_QUEUE+28u);
    assert(count>=2u && count<=1024u && rd<count && wr<count);(void)at(ring,count*28u);
    next=(wr+1u)%count;if(next==rd) return -2;
    put(ring+wr*28u,h);put(ring+wr*28u+4u,m);put(ring+wr*28u+8u,n);put(ring+wr*28u+12u,c);
    put(DESKTOP_QUEUE+28u,next);accepted[0]=h;accepted[1]=m;accepted[2]=n;accepted[3]=c;
    return 0;
}
static void seed(u32 a,const char *h)
{
    while(*h) {
        unsigned x=*h<='9'?(unsigned)(*h-'0'):(unsigned)(*h-'a'+10);h++;
        unsigned y=*h<='9'?(unsigned)(*h-'0'):(unsigned)(*h-'a'+10);h++;
        *at(a++,1)=(u8)((x<<4)|y);
    }
}
static void worker(void) {put(0x02068664u,WORKER_TCB);}
static void gui(void) {put(0x02068664u,GUI_TCB);}
static void step(void)
{
    unsigned before=post_calls;
    assert(!depth && (psr&0x710u)==0x10u && get(0x02068664u)==GUI_TCB);
    step_seen_context=d88_gui_jobs_context();assert(step_seen_context==1);steps++;
    assert(!d88_gui_jobs_idle());assert(d88_gui_jobs_stop()==D88_GUI_JOBS_BUSY);
    if(step_mode==1u && steps==1u) {
        assert(gui_hook(accepted[0],accepted[1],accepted[2],accepted[3])==1);
        assert(d88_gui_jobs_request()==0);
        assert(d88_gui_jobs_poll()==D88_GUI_JOBS_CONTEXT);
        worker();assert(d88_gui_jobs_poll()==0);gui();
        assert(post_calls==before); /* Neither reentry nor a blocked GUI self-post. */
    }
    if(step_mode==2u && steps<3u) assert(d88_gui_jobs_request()==0);
    assert(d88_gui_jobs_run_inline_one()==D88_GUI_JOBS_BUSY);
}
static void other_step(void) {assert(0);}
static void reset(void)
{
    memset(memory,0,sizeof(memory));memset(accepted,0,sizeof(accepted));
    active=wanted=pending=busy=sequence=pending_nonce=0u;step_function=0;last_error=0;
    psr=0x10u;gp=0;step_address=0x02502000u;corrupt_on_lock=0;
    lock_calls=unlock_calls=post_calls=steps=step_mode=0;depth=fail_lock=fail_unlock=post_result=step_seen_context=0;
    put(0x02000204u,0x021e7b54u);put(0x021e7b90u,0x02182c3au);
    put(0x02000200u,0x021e81e8u);put(0x021e8244u,0x021ce8aeu);put(0x021e8248u,0x021ce8d8u);
    put(0x022a9820u,TCB_BASE);put(GUI_TCB+24u,0x0218339au);gui();
    put(DESKTOP_QUEUE+16u,RING);put(DESKTOP_QUEUE+20u,8u);
    /* Independent literal captured bytes; signature tests also compare the
     * production arrays against the original private firmware file. */
    seed(0x02182c3au,"03020184612e722e095cf8dffbdf7d1d832e402e00683a19f46f02c0126b061a043040c000c004742f1e1cc0063014c0073000c092c2581d166018c00530542a2519e46f1cc00530542e348c5426248c10c005305422413c04c0423c08c0433c05500cc0453c1cc0043014601cc0043c14c00530542a040d046c1cc0043c043000c800c00474043c046c018043024006");
    seed(0x0218231au,"0668041bf66b101ff46f0519652e542814680318091f046c02c0542814690419542eb8c06430400606680718f8dfffdfe81cdcc04430041e1cd0d0c3f4224006");
    seed(0x021d5120u,"62a0708b486c09a4709370937093709370937093709370931864f71b90a07097009b400734a4");
    seed(0x021ce8aeu,"34d06cc6f424046800c00d1a34d06dc6f424347434d06dc6f43404a4f47240a0041f046cfedfb46f4006");
    seed(0x021ce8d8u,"34d06cc6f42404680518fedfb46f00c0141e34d06dc6f42403c0c47334d06dc6f43434d06ec6f0a8041800c013c0381c04a4047540a0046c4006");
    seed(0x021813c2u,"03020384712e822e1cd07cc5f43004680619902e0406046802c01d18");
    seed(0x0218339au,"0002018400c066c1331c1cd0e8c9f43cf8dff5df8e1c0468");
    seed(0x021813deu,"142e03c00467f4690911162e272ef8dffddf641d082e02c0101e11690f1b142e1cd074c4f63006680919172e282e00c00cc0151d092e1c1f142e0465f4681811142e1cd084c4f63006680a19172e282ef8dfc4df301d092e04680a19142e162e272ef8dffadf861d082e01c0e61e1464e4681f11272e042e84888488245c1cd084c4f63006680d1903ad172e382ef8dfc4df111d492e04680c19162e031f272e162e2950f8dffbdf401d382e01c0c51e04c0016a01c0c119046c142e02c0146702c0e46b01c0b811248cfdd0dccaf522542244308406");
    seed(0x02181814u,"046c038043024006");
    assert(profile());
}
static void begin(void) {assert(d88_gui_jobs_start(step)==0);assert(get(HOOK_SLOT)==HOOK_ADDRESS);worker();}
static void enqueue(void) {assert(d88_gui_jobs_request()==0);assert(d88_gui_jobs_poll()==0);assert(pending);}
static int deliver(void)
{
    u32 rd=get(DESKTOP_QUEUE+24u),count=get(DESKTOP_QUEUE+20u),a=get(DESKTOP_QUEUE+16u)+rd*28u;
    int r;assert(rd!=get(DESKTOP_QUEUE+28u));gui();
    put(DESKTOP_QUEUE+24u,(rd+1u)%count);
    r=gui_hook(get(a),get(a+4u),get(a+8u),get(a+12u));worker();return r;
}
static void assert_balanced(void) {assert(!depth && unlock_calls==lock_calls);}

static void lifecycle(void)
{
    unsigned i;reset();begin();assert(d88_gui_jobs_idle());
    assert(d88_gui_jobs_request()==0);
    for(i=0;i<10;i++) assert(d88_gui_jobs_request()==0);
    assert(!d88_gui_jobs_idle());assert(d88_gui_jobs_stop()==D88_GUI_JOBS_BUSY);
    assert(d88_gui_jobs_poll()==0);assert(post_calls==1u && pending);
    for(i=0;i<10;i++) {assert(d88_gui_jobs_request()==0);assert(d88_gui_jobs_poll()==0);}
    assert(post_calls==1u);assert(d88_gui_jobs_stop()==D88_GUI_JOBS_BUSY);
    assert(deliver()==0 && steps==1u && step_seen_context);
    assert(d88_gui_jobs_idle());assert(d88_gui_jobs_poll()==0 && post_calls==1u);
    assert(d88_gui_jobs_stop()==0 && !get(HOOK_SLOT));assert(d88_gui_jobs_idle());
    assert(!d88_gui_jobs_context());assert(d88_gui_jobs_stop()==0);
    assert(d88_gui_jobs_request()==D88_GUI_JOBS_INACTIVE);assert_balanced();
}
static void ownership(void)
{
    reset();put(HOOK_SLOT,0x02503000u);assert(d88_gui_jobs_start(step)==D88_GUI_JOBS_OWNERSHIP);
    assert(get(HOOK_SLOT)==0x02503000u && !active);assert_balanced();
    reset();assert(d88_gui_jobs_start(0)==D88_GUI_JOBS_PROFILE);
    step_address=0x02701000u;assert(d88_gui_jobs_start(step)==D88_GUI_JOBS_PROFILE);
    step_address=0x02502001u;assert(d88_gui_jobs_start(step)==D88_GUI_JOBS_PROFILE);
    step_address=0x02502000u;begin();assert(d88_gui_jobs_start(step)==0);
    assert(d88_gui_jobs_start(other_step)==D88_GUI_JOBS_OWNERSHIP);
    put(HOOK_SLOT,0x02503000u);
    assert(d88_gui_jobs_request()==D88_GUI_JOBS_OWNERSHIP);
    assert(d88_gui_jobs_poll()==D88_GUI_JOBS_OWNERSHIP);
    assert(!d88_gui_jobs_idle());assert(d88_gui_jobs_stop()==D88_GUI_JOBS_OWNERSHIP);
    assert(get(HOOK_SLOT)==0x02503000u && active && !steps && !post_calls);assert_balanced();
}
static void queues(void)
{
    static const u32 cases[][2]={{16,0},{16,RING+1u},{16,0x027ffff0u},{20,0},{20,1},{20,1025},
        {20,0xffffffffu},{24,8},{24,0xffffffffu},{28,8},{28,0xffffffffu}};
    unsigned i;
    for(i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        reset();begin();assert(d88_gui_jobs_request()==0);put(DESKTOP_QUEUE+cases[i][0],cases[i][1]);
        assert(d88_gui_jobs_poll()==D88_GUI_JOBS_QUEUE && !post_calls && !pending && wanted);assert_balanced();
    }
    reset();begin();put(DESKTOP_QUEUE+28u,7u);assert(d88_gui_jobs_request()==0);
    assert(d88_gui_jobs_poll()==D88_GUI_JOBS_BUSY && !pending && wanted);
    assert(d88_gui_jobs_poll()==D88_GUI_JOBS_BUSY && !pending && wanted);
    put(DESKTOP_QUEUE+24u,3u);assert(d88_gui_jobs_poll()==0 && pending);
    assert(deliver()==1); /* Old empty fixture entries are unrelated messages. */
    assert(!steps); /* Deliver the accepted message explicitly. */
    gui();assert(gui_hook(0,PRIVATE_MESSAGE,pending_nonce,IMAGE_COOKIE)==0);worker();assert(steps==1u);
    assert(d88_gui_jobs_idle());assert_balanced();
    reset();begin();assert(d88_gui_jobs_request()==0);post_result=-1;
    assert(d88_gui_jobs_poll()==D88_GUI_JOBS_QUEUE && !pending && wanted);
    post_result=0;assert(d88_gui_jobs_poll()==0 && pending);assert(deliver()==0);assert_balanced();
}
static void contexts(void)
{
    unsigned i;
    for(i=0;i<9u;i++) {
        reset();
        if(i==0) gp=1u;
        if(i==1) psr=0u;
        if(i==2) psr=0x110u;
        if(i==3) psr=0x210u;
        if(i==4) psr=0x410u;
        if(i==5) *at(0x0206866cu,1)=1u;
        if(i==6) *at(0x0206866du,1)=1u;
        if(i==7) *at(0x0206866du,1)=2u;
        if(i==8) put(0x02068664u,TCB_BASE+6u*72u);
        assert(d88_gui_jobs_start(step)==D88_GUI_JOBS_CONTEXT && !lock_calls && !get(HOOK_SLOT));
        assert(!d88_gui_jobs_context());
    }
    for(i=0;i<8u;i++) {
        reset();begin();enqueue();gui();
        if(i==0) worker();
        if(i==1) gp=1u;
        if(i==2) psr=0u;
        if(i==3) psr=0x110u;
        if(i==4) *at(0x0206866cu,1)=1u;
        if(i==5) *at(0x0206866du,1)=1u;
        if(i==6) *at(0x0206866du,1)=2u;
        if(i==7) put(0x02068664u,TCB_BASE+6u*72u);
        assert(gui_hook(0,PRIVATE_MESSAGE,pending_nonce,IMAGE_COOKIE)==1 && !steps && pending);
        gp=0;psr=0x10u;*at(0x0206866cu,1)=*at(0x0206866du,1)=0;gui();
        assert(gui_hook(0,PRIVATE_MESSAGE,pending_nonce,IMAGE_COOKIE)==0 && steps==1u);assert_balanced();
    }
    reset();begin();enqueue();gui();
    assert(gui_hook(1,PRIVATE_MESSAGE,pending_nonce,IMAGE_COOKIE)==1);
    assert(gui_hook(0,PRIVATE_MESSAGE+1u,pending_nonce,IMAGE_COOKIE)==1);
    assert(gui_hook(0,PRIVATE_MESSAGE,pending_nonce+1u,IMAGE_COOKIE)==1);
    assert(gui_hook(0,PRIVATE_MESSAGE,pending_nonce,IMAGE_COOKIE+1u)==1);
    assert(!steps && pending);assert(d88_gui_jobs_poll()==D88_GUI_JOBS_CONTEXT);
    assert(gui_hook(0,PRIVATE_MESSAGE,pending_nonce,IMAGE_COOKIE)==0);
    assert(gui_hook(0,PRIVATE_MESSAGE,pending_nonce,IMAGE_COOKIE)==1 && steps==1u);assert_balanced();
}
static void reentry(void)
{
    reset();begin();step_mode=1u;enqueue();assert(deliver()==0);
    assert(steps==1u && !pending && wanted && !busy && post_calls==1u);
    assert(!d88_gui_jobs_idle());assert(d88_gui_jobs_poll()==0 && pending && post_calls==2u);
    assert(deliver()==0 && steps==2u && !pending && !wanted && !busy);
    assert(d88_gui_jobs_idle());assert(d88_gui_jobs_stop()==0);assert_balanced();
}
static void stale_and_restart(void)
{
    u32 old[4];reset();begin();enqueue();memcpy(old,accepted,sizeof(old));assert(deliver()==0);
    assert(d88_gui_jobs_stop()==0);assert(d88_gui_jobs_start(step)==0);enqueue();
    assert(pending_nonce!=old[2]);gui();assert(gui_hook(old[0],old[1],old[2],old[3])==1 && steps==1u);
    worker();assert(deliver()==0 && steps==2u);assert(d88_gui_jobs_stop()==0);
    assert(d88_gui_jobs_start(step)==0);sequence=0xffffffffu;assert(d88_gui_jobs_request()==0);
    assert(d88_gui_jobs_poll()==D88_GUI_JOBS_EXHAUSTED && !pending && wanted && sequence==0xffffffffu);
    assert(d88_gui_jobs_stop()==D88_GUI_JOBS_BUSY);assert_balanced();
}
static void fingerprints(void)
{
    static const u32 ranges[][2]={{0x02182c3au,144},{0x0218231au,64},{0x021d5120u,38},
        {0x021ce8aeu,42},{0x021ce8d8u,58},{0x021813c2u,28},{0x0218339au,24},
        {0x021813deu,214},{0x02181814u,8}};
    static const u32 roots[]={0x02000204u,0x021e7b90u,0x02000200u,0x021e8244u,0x021e8248u,
        0x022a9820u,GUI_TCB+24u};
    unsigned i,j,before;
    for(i=0;i<sizeof(ranges)/sizeof(ranges[0]);i++) for(j=0;j<ranges[i][1];j++) {
        reset();begin();assert(d88_gui_jobs_request()==0);before=lock_calls;*at(ranges[i][0]+j,1)^=0x80u;
        assert(d88_gui_jobs_poll()==D88_GUI_JOBS_PROFILE && !post_calls && lock_calls==before);assert_balanced();
    }
    for(i=0;i<sizeof(roots)/sizeof(roots[0]);i++) {
        reset();begin();assert(d88_gui_jobs_request()==0);put(roots[i],get(roots[i])^4u);
        assert(d88_gui_jobs_poll()==D88_GUI_JOBS_PROFILE && !post_calls);assert_balanced();
    }
    reset();begin();assert(d88_gui_jobs_request()==0);corrupt_on_lock=0x02182c3au;
    assert(d88_gui_jobs_poll()==D88_GUI_JOBS_PROFILE && !post_calls && !pending);assert_balanced();
    reset();begin();enqueue();gui();corrupt_on_lock=0x021813c2u;
    assert(gui_hook(0,PRIVATE_MESSAGE,pending_nonce,IMAGE_COOKIE)==1 && !steps && pending);assert_balanced();
    reset();begin();fail_lock=1;assert(d88_gui_jobs_request()==D88_GUI_JOBS_CONTEXT && !wanted);
    assert(lock_calls==unlock_calls+1u && !depth);
    reset();begin();assert(d88_gui_jobs_request()==0);fail_unlock=1;
    assert(d88_gui_jobs_poll()==D88_GUI_JOBS_CONTEXT && pending && post_calls==1u);assert_balanced();
}
static void inline_stop_drain(void)
{
    u32 old[4];reset();begin();step_mode=2u;enqueue();memcpy(old,accepted,sizeof(old));
    assert(d88_gui_jobs_run_inline_one()==D88_GUI_JOBS_CONTEXT && !steps && pending);
    gui();assert(d88_gui_jobs_run_inline_one()==0 && steps==1u && wanted && !pending);
    assert(gui_hook(old[0],old[1],old[2],old[3])==1 && steps==1u);
    worker();put(DESKTOP_QUEUE+28u,7u); /* Full GUI queue while launcher waits. */
    assert(d88_gui_jobs_poll()==D88_GUI_JOBS_BUSY && wanted && !pending);
    gui();assert(d88_gui_jobs_run_inline_one()==0 && steps==2u && wanted);
    assert(d88_gui_jobs_run_inline_one()==0 && steps==3u && !wanted && !pending);
    assert(d88_gui_jobs_run_inline_one()==0 && steps==3u);assert(d88_gui_jobs_idle());
    assert(d88_gui_jobs_stop()==0 && !get(HOOK_SLOT));
    assert(gui_hook(old[0],old[1],old[2],old[3])==1 && steps==3u);
    assert(d88_gui_jobs_run_inline_one()==D88_GUI_JOBS_INACTIVE);assert_balanced();
    reset();begin();enqueue();gui();put(HOOK_SLOT,0x02503000u);
    assert(d88_gui_jobs_run_inline_one()==D88_GUI_JOBS_OWNERSHIP && !steps && pending);
    assert(get(HOOK_SLOT)==0x02503000u);assert_balanced();
}
int main(int argc,char **argv)
{
    int n=argc==2?atoi(argv[1]):-1;
    switch(n) {case 0:lifecycle();break;case 1:ownership();break;case 2:queues();break;
    case 3:contexts();break;case 4:reentry();break;case 5:stale_and_restart();break;
    case 6:fingerprints();break;case 7:inline_stop_drain();break;default:return 2;}
    printf("PASS GUI bridge case %d\n",n);return 0;
}
