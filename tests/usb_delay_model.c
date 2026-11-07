/* Executes the production delay guard. It models native sleep completion;
 * it does not make a host wall-clock or physical USB timing claim. */
#include <string.h>
#define USB_DELAY_TEST 1
#include "../device/usb_delay.c"

static unsigned char memory[0x02300000u];
static unsigned psr,waits,last_ticks,advance,mutation_address,mutation_value;
static unsigned mutation_width,psr_after;
static int wait_result;

void model_write8(unsigned address,unsigned value)
{ if(address<sizeof(memory)) memory[address]=(unsigned char)value; }
void model_write32(unsigned address,unsigned value)
{
    unsigned i;
    for(i=0;i<4u;i++) model_write8(address+i,value>>(8u*i));
}
unsigned char usb_delay_test_read8(unsigned address)
{ return address<sizeof(memory)?memory[address]:0; }
unsigned usb_delay_test_read32(unsigned address)
{
    unsigned i,value=0;
    for(i=0;i<4u;i++) value|=(unsigned)usb_delay_test_read8(address+i)<<(8u*i);
    return value;
}
unsigned usb_delay_test_psr(void) {return psr;}
int usb_delay_test_wait(unsigned ticks)
{
    waits++;last_ticks=ticks;
    model_write32(0x02068674u,usb_delay_test_read32(0x02068674u)+advance);
    if(mutation_width==1u) model_write8(mutation_address,mutation_value);
    if(mutation_width==4u) model_write32(mutation_address,mutation_value);
    if(psr_after!=0xffffffffu) psr=psr_after;
    return wait_result;
}
void model_reset(void)
{
    memset(memory,0,sizeof(memory));
    psr=0x12u;waits=last_ticks=mutation_address=mutation_value=mutation_width=0;
    psr_after=0xffffffffu;advance=65u;wait_result=0;
    model_write32(0x02000200u,0x021e81e8u);
    model_write32(0x021e81e8u+27u*4u,0x021cf58eu);
    model_write32(0x021e81e8u+54u*4u,0x021d11b8u);
    model_write32(0x02000098u,0x02198dbcu);
    model_write32(0x022a981cu,1u);
    model_write32(0x02068674u,1000u);
    memcpy(memory+0x021cf58eu,delay_signature,sizeof(delay_signature));
    memcpy(memory+0x021d11b8u,clock_signature,sizeof(clock_signature));
    memcpy(memory+0x02198dbcu,irq_signature,sizeof(irq_signature));
    model_write8(0x40149u,0x0fu);
    model_write8(0x48192u,0x24u);model_write8(0x48193u,1u);
    model_write8(0x48196u,1u);model_write8(0x40273u,0x44u);
    model_write8(0x40180u,0x3fu);
}
void model_set_psr(unsigned value) {psr=value;}
void model_set_psr_after(unsigned value) {psr_after=value;}
void model_set_wait_result(int value) {wait_result=value;}
void model_set_advance(unsigned value) {advance=value;}
void model_mutate_during_wait(unsigned address,unsigned value,unsigned width)
{mutation_address=address;mutation_value=value;mutation_width=width;}
unsigned model_waits(void) {return waits;}
unsigned model_last_ticks(void) {return last_ticks;}
unsigned model_detach_ms(void) {return D88_USB_DETACH_MS;}
