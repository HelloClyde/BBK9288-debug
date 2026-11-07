#include <assert.h>
#include <stdio.h>
#include <string.h>

#define D88_APP_REFRESH_TEST
#include "../device/app_refresh.c"

static unsigned char memory[0x800000],saved[0x800000];
static unsigned psr_value,gp_value,fs_ready,gui_owned,scans,posts,mutation;
static int post_result;
static const unsigned home=0x026e0000u;

unsigned char d88_refresh_test_read8(unsigned address)
{
    assert(address>=0x02000000u && address<0x02800000u);
    return memory[address-0x02000000u];
}
unsigned d88_refresh_test_read32(unsigned address)
{
    unsigned result=0,i;
    for(i=0;i<4u;++i) result|=(unsigned)d88_refresh_test_read8(address+i)<<(8u*i);
    return result;
}
static void write32(unsigned address,unsigned value)
{
    unsigned i;
    for(i=0;i<4u;++i) memory[address-0x02000000u+i]=(unsigned char)(value>>(8u*i));
}
static void code(unsigned address,const unsigned char *bytes,unsigned count)
{memcpy(memory+address-0x02000000u,bytes,count);}
unsigned d88_refresh_test_psr(void) {return psr_value;}
unsigned d88_refresh_test_gp(void) {return gp_value;}
int d88_file_native_available(void) {return (int)fs_ready;}
int d88_gui_jobs_context(void) {return (int)gui_owned;}
void d88_refresh_test_scan(void)
{
    assert(gui_owned && psr_value==0x10u && gp_value==0u);
    assert(d88_refresh_test_read32(0x02068664u)==GUI_TCB);
    assert(!d88_refresh_test_read8(0x0206866cu) && !d88_refresh_test_read8(0x0206866du));
    ++scans;
    write32(TOTAL_COUNT,11u);write32(GAME_COUNT,5u);
    if(mutation==1u) write32(0x02038474u,home+4u);
    if(mutation==2u) write32(HOOK_SLOT,0x02501200u);
    if(mutation==3u) write32(home+0xdcu,0x02039000u);
    if(mutation==4u) gui_owned=0u;
    if(mutation==5u) memory[SCAN_ENTRY-0x02000000u]^=1u;
    if(mutation==6u) write32(TOTAL_COUNT,129u);
    if(mutation==7u) write32(GAME_COUNT,12u);
    if(mutation==8u) write32(0x0202a3b4u,0u);
}
int d88_refresh_test_post(unsigned window,unsigned message,unsigned a,unsigned b)
{
    assert(window==home && message==0xb1u && !a && !b && scans==1u);
    assert(gui_owned && d88_refresh_test_read32(HOOK_SLOT)==0x02501100u);
    ++posts;
    if(mutation==9u) write32(home+0xdcu,0x02039000u);
    return post_result;
}

static void setup(void)
{
    memset(memory,0,sizeof(memory));
    psr_value=0x10u;gp_value=0u;fs_ready=gui_owned=1u;scans=posts=mutation=0u;post_result=0;
    write32(0x02000200u,0x021e81e8u);write32(0x02000204u,0x021e7b54u);
    write32(0x02000214u,0x021e7a3cu);write32(0x021e7ad4u,SCAN_ENTRY);
    write32(0x021e7b90u,POST_ENTRY);write32(0x022a9820u,0x02068b34u);
    code(SCAN_ENTRY,scan_wrapper_signature,sizeof(scan_wrapper_signature));
    code(0x0215f5ccu,scan_helper_signature,sizeof(scan_helper_signature));
    code(0x0215f4a0u,build_record_signature,sizeof(build_record_signature));
    code(POST_ENTRY,post_signature,sizeof(post_signature));
    code(0x021e75dcu,(const unsigned char *)"A:\\" "\xcf\xb5\xcd\xb3" "\\" "\xb3\xcc\xd0\xf2" "\\",14u);
    code(0x021e7668u,(const unsigned char *)"exe",4u);
    write32(0x02068664u,GUI_TCB);
    write32(0x0202a3b8u,home);write32(0x02038474u,home);
    write32(home,0x00110001u);write32(home+0x74u,0x0216022au);
    write32(home+0xdcu,0x020383d0u);write32(HOOK_SLOT,0x02501100u);
    write32(0x0202a3b4u,1u);
    write32(TOTAL_COUNT,7u);write32(GAME_COUNT,2u);
}
static void rejected(int expected)
{
    d88_app_refresh_result result;
    memset(&result,0xa5,sizeof(result));
    memcpy(saved,memory,sizeof(memory));
    assert(d88_app_refresh_execute(&result)==expected);
    assert(!scans && !posts && !result.scanner_calls && !result.post_calls);
    assert(!memcmp(saved,memory,sizeof(memory)));
}
static void test_success_and_repeat(void)
{
    d88_app_refresh_result result;
    setup();memcpy(saved,memory,sizeof(memory));
    assert(d88_app_refresh_available());
    assert(!memcmp(saved,memory,sizeof(memory)) && !scans && !posts);
    assert(!d88_app_refresh_execute(&result));
    assert(result.before_total==7u && result.after_total==11u);
    assert(result.before_cat8==2u && result.after_cat8==5u);
    assert(result.scanner_calls==1u && result.post_calls==1u && !result.post_result && result.home==home);
    assert(scans==1u && posts==1u);
    /* No implicit replay: another explicit command is a second scan. */
    scans=posts=0;assert(!d88_app_refresh_execute(&result));
    assert(result.before_total==11u && result.after_total==11u && scans==1u && posts==1u);
}
static void test_context_and_idle_guards(void)
{
    setup();assert(d88_app_refresh_execute(0)==D88_FILE_E_ARGUMENT && !scans && !posts);
    setup();gui_owned=0u;rejected(D88_FILE_E_PROFILE);
    setup();gp_value=4u;rejected(D88_FILE_E_PROFILE);
    setup();psr_value=0u;rejected(D88_FILE_E_PROFILE);
    setup();psr_value=0x110u;rejected(D88_FILE_E_PROFILE);
    setup();write32(0x02068664u,0x02068d74u);rejected(D88_FILE_E_PROFILE);
    setup();memory[0x6866c]=1;rejected(D88_FILE_E_PROFILE);
    setup();memory[0x6866d]=3;rejected(D88_FILE_E_PROFILE);
    setup();fs_ready=0;assert(!d88_app_refresh_available());rejected(D88_FILE_E_PROFILE);
    setup();write32(0x02038474u,home+4u);rejected(D88_FILE_E_BUSY);
    setup();write32(0x0202a3b8u,0x02700000u);rejected(D88_FILE_E_BUSY);
    setup();write32(0x0202a3b8u,home+1u);rejected(D88_FILE_E_BUSY);
    setup();write32(home,0u);rejected(D88_FILE_E_BUSY);
    setup();write32(home+0x74u,0x0216022cu);rejected(D88_FILE_E_BUSY);
    setup();write32(0x0202a3b4u,0u);rejected(D88_FILE_E_BUSY);
    setup();write32(0x0202a3b0u,1u);rejected(D88_FILE_E_BUSY);
    setup();write32(0x0202a4e4u,1u);rejected(D88_FILE_E_BUSY);
    setup();write32(TOTAL_COUNT,129u);rejected(D88_FILE_E_PROFILE);
    setup();write32(GAME_COUNT,8u);rejected(D88_FILE_E_PROFILE);
}
static void test_every_native_signature_byte_and_roots(void)
{
    unsigned i,j;
    const unsigned address[]={SCAN_ENTRY,0x0215f5ccu,0x0215f4a0u,POST_ENTRY,0x021e75dcu,0x021e7668u};
    const unsigned bytes[]={sizeof(scan_wrapper_signature),sizeof(scan_helper_signature),sizeof(build_record_signature),sizeof(post_signature),14u,4u};
    const unsigned roots[]={0x02000200u,0x02000204u,0x02000214u,0x021e7ad4u,0x021e7b90u,0x022a9820u};
    d88_app_refresh_result result;
    setup();memcpy(saved,memory,sizeof(memory));
    for(i=0u;i<sizeof(address)/sizeof(address[0]);++i) for(j=0u;j<bytes[i];++j) {
        memory[address[i]-0x02000000u+j]^=1u;
        assert(!d88_app_refresh_available());
        assert(d88_app_refresh_execute(&result)==D88_FILE_E_PROFILE && !scans && !posts);
        memory[address[i]-0x02000000u+j]^=1u;
    }
    for(i=0u;i<sizeof(roots)/sizeof(roots[0]);++i) {
        memory[roots[i]-0x02000000u]^=1u;
        assert(!d88_app_refresh_available());
        assert(d88_app_refresh_execute(&result)==D88_FILE_E_PROFILE && !scans && !posts);
        memory[roots[i]-0x02000000u]^=1u;
    }
    assert(!memcmp(saved,memory,sizeof(memory)));
}
static void test_scan_side_effects_and_post_failures_are_not_replayed(void)
{
    unsigned i;
    d88_app_refresh_result result;
    for(i=1u;i<=8u;++i) {
        setup();mutation=i;
        assert(d88_app_refresh_execute(&result)==D88_FILE_E_PROFILE);
        assert(scans==1u && !posts && result.scanner_calls==1u && !result.post_calls);
        assert(result.after_total==(i==6u?129u:11u));
    }
    setup();post_result=-2;
    assert(d88_app_refresh_execute(&result)==D88_FILE_E_IO);
    assert(scans==1u && posts==1u && result.scanner_calls==1u && result.post_calls==1u);
    assert(result.post_result==0xfffffffeu && result.after_total==11u);
    setup();mutation=9u;
    assert(d88_app_refresh_execute(&result)==D88_FILE_E_PROFILE && scans==1u && posts==1u);
}
int main(void)
{
    test_success_and_repeat();test_context_and_idle_guards();
    test_every_native_signature_byte_and_roots();
    test_scan_side_effects_and_post_failures_are_not_replayed();
    puts("PASS native app scanner guards, complete bounded fingerprints and one-shot PAINT");
    return 0;
}
