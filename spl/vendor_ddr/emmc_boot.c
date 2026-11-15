// SPDX-License-Identifier: GPL-2.0-only
/* Source user-area boot loader. Read-only eMMC, fixed payload destinations.
 * eMMC is already the BootROM's boot medium; its VCC/VCCQ power is not changed.
 * Keep board recovery usable before DDR: Ctrl-C, then the fixed 'efex' command.
 */
#include "platform.h"
#include "emmc.h"
#include <stdint.h>
extern int printf(const char *,...);
extern void t736_enter_a64(void) __attribute__((noreturn));
extern void t736_enter_brom_fel(void) __attribute__((noreturn));
#define UART 0x02500000u
static uint8_t workspace[512];
static uint32_t header[128];
static uint32_t crc32(const uint8_t *p,uint32_t n){
 uint32_t c=~0u;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;
}
static void stop(void) __attribute__((noreturn));
static void stop(void){printf("SPL stopped; power-cycle and Ctrl-C for recovery\n");for(;;)__asm__ volatile("wfi");}
static int rx(void){if(t736_read32(UART+0x14)&1)return t736_read32(UART)&255;return -1;}
static void enter_fel(void) __attribute__((noreturn));
static void enter_fel(void){
 /* RTC EFEX is consumed by vendor Boot0, not by BootROM. We replace Boot0,
  * so enter this chip's ROM FEL entry directly, before any DDR/PLL changes.
  * Board golden Boot0: 0x5eecc loads 0x20; 0x48ec6 branches to it in ARM.
  * The address is a ROM ABI, not a copied vendor executable. */
 t736_write32(0x07090108,0);
 if(t736_read32(0x07090108))stop();
 printf("SPL fixed efex: entering BootROM FEL at 0x20\n");
 for(unsigned n=0;n<T736_POLL_LIMIT;n++){
  if(t736_read32(UART+0x14)&0x40)t736_enter_brom_fel();
 }
 stop();
}
void spl_recovery_window(void){
 t736_clrset32(0x02000104,0xff0,0x220);t736_clrset32(0x02002538,0x0700001f,0);t736_clrset32(0x02002e00,0,0x10001);
 t736_write32(UART+4,0);t736_write32(UART+12,0x80);t736_write32(UART,13);t736_write32(UART+4,0);t736_write32(UART+12,3);t736_write32(UART+8,7);
 if(t736_timer_init())stop();
#ifdef T736_PERIPHERAL_POWER
 printf("T736 source eMMC SPL v4; fixed LPDDR4x and peripheral power; Ctrl-C within 3 seconds for recovery\n");
#else
 printf("T736 source eMMC SPL v3; fixed board LPDDR4x; Ctrl-C within 3 seconds for recovery\n");
#endif
 printf("CCU before DDR: PERI0=%x AHB=%x APB0=%x\n",t736_read32(0x020020a0),t736_read32(0x02002500),t736_read32(0x02002510));
 uint32_t start=t736_counter_low();int interrupted=0;
 while((uint32_t)(t736_counter_low()-start)<72000000u){if(rx()==3){interrupted=1;break;}if(t736_delay_us(100))stop();}
 if(!interrupted)return;
 /* The board-scoped recovery tool recognizes a standalone U-Boot prompt. */
 printf("\n=> ");char command[8];unsigned used=0;
 for(;;){int c=rx();if(c<0){if(t736_delay_us(100))stop();continue;}
  if(c=='\r'||c=='\n'){
   if(used==4&&command[0]=='e'&&command[1]=='f'&&command[2]=='e'&&command[3]=='x')enter_fel();
   used=0;printf("\nOnly efex recovery is supported\n=> ");
  }else if(c==3){used=0;}else if(used<sizeof(command))command[used++]=(char)c;
 }
}
static uint32_t io_read(void *ctx,uint32_t a){(void)ctx;return t736_read32(a);}
static void io_write(void *ctx,uint32_t a,uint32_t v){(void)ctx;t736_write32(a,v);}
static int io_delay(void *ctx,uint32_t us){(void)ctx;return t736_delay_us(us);}
void emmc_boot_main(void){
 printf("CCU after DDR: PERI0=%x AHB=%x APB0=%x\n",t736_read32(0x020020a0),t736_read32(0x02002500),t736_read32(0x02002510));
 static const uint8_t pins[]={0,1,5,6,8,9,10,11,13,14,15,16};
 for(unsigned i=0;i<sizeof(pins);i++){
  uint32_t shift=(pins[i]%8)*4,a=0x02000180+(pins[i]/8)*4;
  t736_clrset32(a,15u<<shift,4u<<shift);
 }
 t736_write32(0x02002d30,1u<<31);t736_clrset32(0x02002d3c,1u<<16,0);t736_delay_us(10);t736_clrset32(0x02002d3c,0,0x10001);
 struct t736_emmc card={0};const struct t736_emmc_io io={0,io_read,io_write,io_delay};
 int rc=t736_emmc_open(&card,&io,workspace);
 printf("SPL eMMC open rc=%d sectors=%u irq=%x r1=%x\n",rc,card.sectors,card.last_irq,card.last_r1);
 if(rc||card.sectors!=122142720u)stop();
 if(t736_emmc_read(&card,24576,(uint8_t*)header))stop();
 uint32_t saved=header[4];header[4]=0;
 if(header[0]!=0x36333754||header[1]!=0x314c5053||header[2]!=1||header[3]!=3||crc32((uint8_t*)header,512)!=saved){printf("SPL payload header invalid\n");stop();}
 static const uint32_t addresses[]={0x47000000,0x48000000,0x52000000};
 static const uint32_t limits[]={8192,0x200000,0x100000};
 uint32_t previous=24577;
 for(unsigned i=0;i<3;i++){
  uint32_t *h=&header[8+i*4],sector=h[0],address=h[1],length=h[2],crc=h[3];
  uint32_t count=(length+511)/512;
  if(address!=addresses[i]||!length||length>limits[i]||sector<previous||sector+count>32800)stop();
  previous=sector+count;printf("SPL load addr=%x bytes=%u LBA=%u\n",address,length,sector);
  for(uint32_t j=0;j<count;j++){
   rc=t736_emmc_read(&card,sector+j,workspace);if(rc){printf("SPL read failed rc=%d irq=%x r1=%x\n",rc,card.last_irq,card.last_r1);stop();}
   uint32_t bytes=length-j*512;if(bytes>512)bytes=512;
   for(uint32_t k=0;k<bytes;k++)((volatile uint8_t*)address)[j*512+k]=workspace[k];
  }
  __asm__ volatile("dsb sy" ::: "memory");
  if(crc32((const uint8_t*)address,length)!=crc){printf("SPL payload CRC failed\n");stop();}
 }
 printf("SPL payload CRCs passed; source AArch64 handoff\n");
 t736_enter_a64();
}
