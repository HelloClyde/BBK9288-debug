#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef uint8_t u8;
#include "../device/profiles.h"

static const probe_profile *fixture;
static unsigned corrupt_table = 6;
static unsigned corrupt_byte = 120;
static unsigned byte_reads;
static unsigned corrupt_ros = 4;
static FILE *kernel;

static u32 read_word(u32 address)
{
    unsigned index = (address - 0x02000200u) / 4u;
    if (kernel) {
        unsigned char bytes[4];
        assert(fseek(kernel, (long)(address - 0x02000000u + 0x40u), SEEK_SET)==0);
        assert(fread(bytes,1,sizeof bytes,kernel)==sizeof bytes);
        return (u32)bytes[0] | ((u32)bytes[1]<<8) | ((u32)bytes[2]<<16) | ((u32)bytes[3]<<24);
    }
    if (address >= fixture->tables[0]) {
        static const unsigned slots[]={9,10,23,24};
        static const u32 addresses[][4]={
            {0x021682b8,0x0216832a,0x02166d06,0x02166d30},
            {0x021cfe60,0x021cfed2,0x021ce8ae,0x021ce8d8},
        };
        for (index=0; index<4; ++index)
            if (address==fixture->tables[0]+4u*slots[index])
                return addresses[fixture->id-1u][index] ^ (index==corrupt_ros);
        assert(0);
    }
    assert(index < 6);
    return fixture->tables[index] ^ (index == corrupt_table);
}

static u8 read_byte(u32 address)
{
    unsigned index;
    if (kernel) {
        int byte;
        assert(fseek(kernel, (long)(address - 0x02000000u + 0x40u), SEEK_SET)==0);
        byte=fgetc(kernel);
        assert(byte!=EOF);
        return (u8)byte;
    }
    ++byte_reads;
    for (index = 0; index < 3; ++index) {
        u32 offset = address - fixture->signature_addresses[index];
        if (offset < 40)
            return fixture->signatures[index][offset] ^ (index * 40u + offset == corrupt_byte);
    }
    assert(0);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned index;
    for (index = 0; index < PROBE_PROFILE_COUNT; ++index) {
        d88_ros_entries resolved;
        fixture = &profiles[index];
        assert(d88_match_firmware(read_word, read_byte) == fixture);
        assert(d88_resolve_usb_ros(fixture,read_word,read_byte,0,&resolved));
        assert(resolved.suspend==read_word(fixture->tables[0]+36u));
        for (corrupt_ros=0; corrupt_ros<4; ++corrupt_ros) {
            d88_ros_entries copy=resolved;
            assert(!d88_resolve_usb_ros(fixture,read_word,read_byte,0,&copy));
            assert(!memcmp(&copy,&resolved,sizeof copy));
        }
        for (corrupt_byte = 0; corrupt_byte < 120; ++corrupt_byte)
            assert(!d88_match_firmware(read_word, read_byte));
        for (corrupt_table = 0; corrupt_table < 6; ++corrupt_table) {
            byte_reads = 0;
            assert(!d88_match_firmware(read_word, read_byte));
            assert(byte_reads == 0);
        }
    }
    puts("V1.3 / V1.5 identification and corruption checks passed");
    if (argc==2) {
        d88_ros_entries resolved;
        const probe_profile *profile;
        kernel=fopen(argv[1],"rb");
        assert(kernel);
        profile=d88_match_firmware(read_word,read_byte);
        assert(profile && profile->id==1);
        assert(d88_resolve_usb_ros(profile,read_word,read_byte,0,&resolved));
        assert(read_word(0x020000a8u)==0x02133df4u);
        fclose(kernel);
        puts("Original V1.5 kernel table, signatures, ROS33 entries and Timer3B vector matched");
    }
    return 0;
}
