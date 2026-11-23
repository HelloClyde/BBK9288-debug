#ifndef D88_PROFILES_H
#define D88_PROFILES_H
/* Read-only profiles. Captured profile is bound to actual v2 table evidence
 * and the previously verified NAND capture; never infer addresses by a delta. */
typedef struct probe_profile {
    u32 id; const char *name; u32 tables[6];
    u32 tcb_pointer, heap_state, usb_desktop, usb_phase, usb_phase2, usb_io_base, usb_flags;
    u32 signature_addresses[3]; u8 signatures[3][40];
    u32 usb_timer_entry, usb_task_stack, tcb_aliases[2];
} probe_profile;
static const probe_profile profiles[] = {
    {
        1u, "local-release-020f5df4",
        {0x021806ecu, 0x02180058u, 0x0217ffe4u, 0x0217fed4u, 0x0200021cu, 0x0217ff40u},
        0x02240d88u, 0x022a9facu, 0x024ef004u, 0x0228a60cu, 0x0228a654u, 0x0228a610u, 0x022411b4u,
        {0x0216715eu, 0x0216b764u, 0x02133648u},
        {
            {0x06, 0x68, 0x05, 0x09, 0x86, 0x68, 0xff, 0xdf, 0xd4, 0x6d, 0x2b, 0x1e, 0x05, 0x0f, 0x18, 0x64, 0xff, 0xdf, 0xc4, 0x6c, 0x26, 0x1e, 0x78, 0x68, 0x22, 0x11, 0x65, 0x2e, 0x35, 0x8c, 0x65, 0x22, 0x35, 0x8c, 0x20, 0xd1, 0x88, 0xcd, 0xf4, 0x30},
            {0x03, 0x02, 0x06, 0x68, 0x7e, 0x19, 0x04, 0x6c, 0x54, 0xd1, 0xb0, 0xdf, 0xf4, 0x30, 0x41, 0x2e, 0x54, 0xd1, 0xb8, 0xdf, 0xf4, 0x30, 0x54, 0xd1, 0xb4, 0xdf, 0xf5, 0x30, 0x54, 0x26, 0x74, 0x68, 0x6b, 0x0f, 0x81, 0x64, 0x08, 0x6c, 0x54, 0xd1},
            {0x03, 0x02, 0x03, 0x84, 0x45, 0xd1, 0x10, 0xc6, 0xf5, 0x30, 0x83, 0xc2, 0x56, 0xb0, 0x02, 0x6c, 0x03, 0xc0, 0xf3, 0x6f, 0x77, 0xd2, 0x04, 0xd0, 0xf4, 0x30, 0x14, 0x68, 0x00, 0xc0, 0x2e, 0x1a, 0x06, 0xc0, 0x3c, 0xc0, 0xf5, 0x6e, 0x02, 0xc0},
        },
        0x02133df4u, 0x0226b1fcu, {0u,0u}
    },
    {
        2u, "hardware-capture-0215ebc4",
        {0x021e81e8u, 0x021e7b54u, 0x021e7ae0u, 0x021e79d0u, 0x0200021cu, 0x021e7a3cu},
        0x022a9820u, 0x02069000u, 0x024ef004u, 0x02049798u, 0x020497e0u, 0x0204979cu, 0x02000350u,
        {0x021ced06u, 0x021d330cu, 0x0219c1f2u},
        {
            {0x06, 0x68, 0x05, 0x09, 0x86, 0x68, 0xff, 0xdf, 0xd4, 0x6d, 0x2b, 0x1e, 0x05, 0x0f, 0x18, 0x64, 0xff, 0xdf, 0xc4, 0x6c, 0x26, 0x1e, 0x78, 0x68, 0x22, 0x11, 0x65, 0x2e, 0x35, 0x8c, 0x65, 0x22, 0x35, 0x8c, 0x54, 0xd1, 0x20, 0xd8, 0xf4, 0x30},
            {0x03, 0x02, 0x06, 0x68, 0x7e, 0x19, 0x04, 0x6c, 0x34, 0xd0, 0x04, 0xd0, 0xf4, 0x30, 0x41, 0x2e, 0x34, 0xd0, 0x0c, 0xd0, 0xf4, 0x30, 0x34, 0xd0, 0x08, 0xd0, 0xf5, 0x30, 0x54, 0x26, 0x74, 0x68, 0x6b, 0x0f, 0x81, 0x64, 0x08, 0x6c, 0x34, 0xd0},
            {0x03, 0x02, 0x03, 0x84, 0x24, 0xd0, 0x9c, 0xd7, 0xf5, 0x30, 0x83, 0xc2, 0x56, 0xb0, 0x02, 0x6c, 0x03, 0xc0, 0xf3, 0x6f, 0x77, 0xd2, 0x04, 0xd0, 0xf4, 0x30, 0x14, 0x68, 0x00, 0xc0, 0x2e, 0x1a, 0x06, 0xc0, 0x3c, 0xc0, 0xf5, 0x6e, 0x02, 0xc0},
        },
        0x0219c9acu, 0x0202a394u, {0x022a9864u,0x022a9868u}
    },
};
#define PROBE_PROFILE_COUNT (sizeof(profiles)/sizeof(profiles[0]))

static inline u32 d88_firmware_code_crc(u8 (*read_byte)(u32), u32 start, unsigned size)
{
    u32 crc=0xffffffffu;
    unsigned index, bit;
    for(index=0;index<size;++index) {
        crc^=read_byte(start+index);
        for(bit=0;bit<8u;++bit)
            crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return ~crc;
}

/* Read the shared table first. Only inspect a profile's code addresses
 * when its entire table matches the running firmware. */
static inline const probe_profile *d88_match_firmware(
    u32 (*read_word)(u32), u8 (*read_byte)(u32))
{
    unsigned candidate;
    for (candidate = 0; candidate < PROBE_PROFILE_COUNT; ++candidate) {
        const probe_profile *profile = &profiles[candidate];
        unsigned table, signature;
        for (table = 0; table < 6u; ++table)
            if (read_word(0x02000200u + table * 4u) != profile->tables[table])
                break;
        if (table != 6u) continue;

        for (signature = 0; signature < 3u; ++signature) {
            unsigned byte;
            for (byte = 0; byte < 40u; ++byte)
                if (read_byte(profile->signature_addresses[signature] + byte) !=
                    profile->signatures[signature][byte]) break;
            if (byte != 40u) break;
        }
        if (signature == 3u) return profile;
    }
    return 0;
}

typedef struct d88_ros_entries {
    u32 suspend, resume, lock, unlock;
} d88_ros_entries;
static const unsigned d88_usb_ros_slots[]={9u,10u,23u,24u};
static const u32 d88_usb_ros_original[][4]={
    {0x021682b8u,0x0216832au,0x02166d06u,0x02166d30u},
    {0x021cfe60u,0x021cfed2u,0x021ce8aeu,0x021ce8d8u},
};
static const unsigned d88_usb_ros_sizes[]={114u,114u,42u,58u};

static inline unsigned d88_code_word(u8 (*read_byte)(u32),u32 address)
{
    return read_byte(address)|((unsigned)read_byte(address+1u)<<8);
}

static inline u32 d88_branch_target(u32 pc,unsigned opcode,const unsigned *prefix,unsigned count)
{
    u32 displacement=(opcode&255u)<<1;
    if(count==0) return pc+((displacement^256u)-256u);
    displacement|=(prefix[count-1u]&8191u)<<9;
    if(count==1) return pc+((displacement^0x200000u)-0x200000u);
    return pc+displacement+((prefix[0]&8191u)>>3<<22);
}

/* Compare complete C33 instruction bodies. A PC-relative branch may change
 * encoding only if it still reaches the same local offset or native callee. */
static inline int d88_match_moved_ros(u8 (*read_byte)(u32),u32 original,u32 candidate,unsigned size)
{
    unsigned offset=0;
    if((candidate&1u) || candidate<0x0200021cu || candidate>0x02800000u-size) return 0;
    while(offset<size) {
        unsigned a[2],b[2],n=0,i;
        unsigned left=d88_code_word(read_byte,original+offset);
        unsigned right=d88_code_word(read_byte,candidate+offset);
        while((left&0xe000u)==0xc000u && n<2u) {
            if((right&0xe000u)!=0xc000u || offset+2u>=size) return 0;
            a[n]=left;b[n++]=right;offset+=2u;
            left=d88_code_word(read_byte,original+offset);
            right=d88_code_word(read_byte,candidate+offset);
        }
        if(left>=0x0800u && left<=0x1fffu) {
            u32 target=d88_branch_target(original+offset,left,a,n);
            u32 relocated=d88_branch_target(candidate+offset,right,b,n);
            if((left&0xff00u)!=(right&0xff00u)) return 0;
            if(target>=original && target-original<size) target=candidate+(target-original);
            if(relocated!=target) return 0;
        } else {
            if(left!=right) return 0;
            for(i=0;i<n;++i) if(a[i]!=b[i]) return 0;
        }
        offset+=2u;
    }
    return 1;
}

/* After either firmware check, read the ABI slots from the live table. */
static inline int d88_resolve_usb_ros(const probe_profile *profile,
                                     u32 (*read_word)(u32), u8 (*read_byte)(u32),
                                     int scanned, d88_ros_entries *entries)
{
    u32 address[4];
    unsigned index;
    u32 table=read_word(0x02000200u);
    if (!profile || profile->id < 1u || profile->id > 2u) return 0;
    if ((table&3u) || table<0x0200021cu || table>0x02800000u-55u*4u) return 0;
    for (index=0; index<4u; ++index) {
        u32 original=d88_usb_ros_original[profile->id-1u][index];
        address[index]=read_word(table+4u*d88_usb_ros_slots[index]);
        if(address[index]!=original && (!scanned || !d88_match_moved_ros(
                read_byte,original,address[index],d88_usb_ros_sizes[index]))) return 0;
    }
    entries->suspend=address[0];
    entries->resume=address[1];
    entries->lock=address[2];
    entries->unlock=address[3];
    return 1;
}

static inline u32 d88_normalized_ros_table(u32 (*read_word)(u32), u32 table,unsigned profile_index)
{
    u32 crc=0xffffffffu;
    unsigned slot,index,bit;
    for(slot=0;slot<55u;++slot) {
        u32 value=read_word(table+4u*slot);
        for(index=0;index<4u;++index)
            if(slot==d88_usb_ros_slots[index]) value=d88_usb_ros_original[profile_index][index];
        for(index=0;index<4u;++index) {
            crc^=(value>>(8u*index))&255u;
            for(bit=0;bit<8u;++bit) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
        }
    }
    return ~crc;
}

/* Recognize an unknown build with a verified USB/scheduler implementation.
 * The ROS table may move, but calls and globals embedded in these code ranges
 * must still refer to the audited layout. This path grants only base services. */
static inline const probe_profile *d88_select_firmware(
    u32 (*read_word)(u32), u8 (*read_byte)(u32), int *scanned)
{
    static const struct {
        u32 ros_code,ros_size,ros_crc,usb_code,usb_crc,table_crc,gui_crc,crtl_crc,allocator_crc;
    } signatures[]={
        {0x02166000u,0x4000u,0xa3f8c054u,0x02133000u,0x55e0dff1u,0x274c44c3u,
         0x08a8988du,0x88d34709u,0xe82fe168u},
        {0x021ce000u,0x6000u,0xdd5468c1u,0x0219c000u,0x1536c67eu,0xbfb3af34u,
         0xe6cfff84u,0xcc0dbbd5u,0xc26a18e3u},
    };
    const probe_profile *profile=d88_match_firmware(read_word,read_byte);
    u32 ros;
    unsigned index;
    d88_ros_entries entries;
    *scanned=0;
    if(profile && d88_resolve_usb_ros(profile,read_word,read_byte,0,&entries)) return profile;
    ros=read_word(0x02000200u);
    if((ros&3u) || ros<0x0200021cu || ros>0x02800000u-55u*4u) return 0;
    for(index=0;index<PROBE_PROFILE_COUNT;++index) {
        profile=&profiles[index];
        if(read_word(0x02000204u)!=profile->tables[1] ||
           read_word(0x02000210u)!=profile->tables[4]) continue;
        if(d88_normalized_ros_table(read_word,ros,index)!=signatures[index].table_crc ||
           d88_firmware_code_crc(read_byte,signatures[index].ros_code,signatures[index].ros_size)!=signatures[index].ros_crc ||
           d88_firmware_code_crc(read_byte,signatures[index].usb_code,4096u)!=signatures[index].usb_crc ||
           d88_firmware_code_crc(read_byte,profile->tables[1],1684u)!=signatures[index].gui_crc ||
           d88_firmware_code_crc(read_byte,profile->tables[4],128u)!=signatures[index].crtl_crc ||
           d88_firmware_code_crc(read_byte,profile->signature_addresses[1],256u)!=signatures[index].allocator_crc ||
           !d88_resolve_usb_ros(profile,read_word,read_byte,1,&entries))
            continue;
        *scanned=1;
        return profile;
    }
    return 0;
}
#endif
