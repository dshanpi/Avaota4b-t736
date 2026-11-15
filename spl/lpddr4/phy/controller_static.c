// SPDX-License-Identifier: GPL-2.0-only
/* Provenance/design: controller-init-timing-audit-20261114.json.
 * Scalar host configuration only; no vendor executable or instruction data.
 */
#include "controller_static.h"

static const uint32_t context_address[12]={0x02002020,0x02002c00,0x02002c0c,0x0a020010,0x0a020014,0x07010250,0x08020000,0x08020020,0x0a02021c,0x0a020220,0x0a020224,0x0a020228};
static const uint32_t context_value[12]={0xf9004a00,0x80000003,0x10001,0x273f,2,1,1,24000000,0,0,0,0};
static const uint32_t held_offset[13]={0,0x14,0x90,0x180,0x510,0x508,0x500,0x518,0xc80,0xc84,0x514,0xca4,0x80};
static const uint32_t held_value[13]={0x03081002,0,0,0x800,0x10014,0xc0000000,0,0x25000000,1,1,0,0,0x30};
static const uint32_t phy_address[6]={0x0aaa0000,0x0aaa0132,0x0aa80100,0x0a9400c0,0x0a940620,0x0a940622};
static const uint32_t phy_value[6]={0,9,3,2,0,0};
/* Absolute CH0 words avoid conflating global CTL and timing-bank offsets. */
static const uint32_t config_address[T736_INIT_WORDS]={0x0a110d00,0x0a110d04,0x0a100500,0x0a100504,0x0a100508,0x0a10050c,0x0a100d00};
static const uint32_t config_mask[T736_INIT_WORDS]={0x03ff1fff,0x03ff0000,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,1};
static const uint32_t baseline[T736_INIT_WORDS]={0x0002004e,0,0x510,0,0,0,0};

int t736_init_us_to_x1024(uint32_t us,uint32_t mhz,uint32_t width,uint32_t minimum,uint32_t *out){
 uint64_t cycles,ticks,maximum;
 if(!out || !us || !mhz || !width || width>32)return T736_INIT_INVALID;
 /* Two uint32 operands multiply exactly in uint64. Division by1024 is a
  * shift, keeping the SRAM component independent of a division runtime. */
 cycles=(uint64_t)us*mhz;ticks=(cycles>>10)+((cycles&1023u)!=0u);
 if(ticks<minimum)ticks=minimum;
 maximum=width==32?UINT32_MAX:((UINT64_C(1)<<width)-1u);
 if(ticks>maximum)return T736_INIT_UNSUPPORTED;
 *out=(uint32_t)ticks;return 0;
}
int t736_init_encode_latency(uint32_t rl,uint32_t wl,uint32_t nwr,uint32_t *mr1,uint32_t *mr2){
 static const uint32_t read_latency[8]={6,10,14,20,24,28,32,36};
 static const uint32_t write_latency[8]={4,6,8,10,12,14,16,18};
 static const uint32_t write_recovery[8]={6,10,16,20,24,30,34,40};
 uint32_t i;
 if(!mr1 || !mr2)return T736_INIT_INVALID;
 for(i=0;i<8;i++)if(rl==read_latency[i] && wl==write_latency[i] && nwr==write_recovery[i])break;
 if(i==8)return T736_INIT_UNSUPPORTED;
 *mr1=(1u<<7)|(i<<4)|(1u<<3)|(1u<<2);*mr2=(i<<3)|i;return 0;
}
int t736_init_build_profile(uint32_t ck_mhz,uint32_t counter_mhz,struct t736_init_profile *out){
 struct t736_init_profile p;volatile struct t736_init_profile *dst=out;uint32_t i;
 if(!out)return T736_INIT_INVALID;
 if(ck_mhz!=1800 || counter_mhz!=450)return T736_INIT_UNSUPPORTED;
 p.ck_mhz=ck_mhz;p.counter_mhz=counter_mhz;p.rl=36;p.wl=18;p.nwr=40;
 if(t736_init_us_to_x1024(2000,counter_mhz,13,1,&p.pre_cke_ticks) ||
    t736_init_us_to_x1024(2,counter_mhz,10,1,&p.post_cke_ticks) ||
    t736_init_us_to_x1024(200,counter_mhz,10,1,&p.reset_low_ticks))return T736_INIT_UNSUPPORTED;
 if(t736_init_encode_latency(p.rl,p.wl,p.nwr,&p.mr[0],&p.mr[1]))return T736_INIT_UNSUPPORTED;
 p.mr[2]=(6u<<3)|(1u<<1)|1u; /* MR3: RZQ/6, postamble bit1, VDDQ/3, DBIoff. */
 p.mr[3]=1u<<3;              /* MR13: VRCG, not DM-disable(bit5). */
 p.mr[4]=4u;                 /* MR11: DQ RZQ/4, CAoff. */
 p.mr[5]=(1u<<6)|50u;        /* MR12: explicit historical CA seed. */
 p.mr[6]=(1u<<5)|4u;         /* MR22: ODTD-CA and SoC RZQ/4. */
 p.mr[7]=29u;                /* MR14: explicit historical DQ seed, not trained. */
 p.value[0]=(p.post_cke_ticks<<16)|p.pre_cke_ticks;
 p.value[1]=p.reset_low_ticks<<16;
 for(i=0;i<4;i++)p.value[2+i]=(p.mr[2*i]<<16)|p.mr[2*i+1];
 p.value[6]=1u;              /* TMGCFG: selected controller1:4 mode. */
 /* Explicit aligned scalar stores avoid an implicit libc memcpy in SRAM. */
 dst->ck_mhz=p.ck_mhz;dst->counter_mhz=p.counter_mhz;
 dst->rl=p.rl;dst->wl=p.wl;dst->nwr=p.nwr;
 dst->pre_cke_ticks=p.pre_cke_ticks;dst->post_cke_ticks=p.post_cke_ticks;
 dst->reset_low_ticks=p.reset_low_ticks;
 for(i=0;i<8;i++)dst->mr[i]=p.mr[i];
 for(i=0;i<T736_INIT_WORDS;i++)dst->value[i]=p.value[i];
 return 0;
}

static int read_word(const struct t736_phy_io *io,struct t736_init_report *r,uint32_t a,uint32_t *v){
 r->last_operation=1;r->last_address=a;r->reads_attempted++;
 if(io->read32(io->context,a,v))return T736_INIT_IO;
 r->last_value=*v;r->reads_completed++;return 0;
}
static int expect_word(const struct t736_phy_io *io,struct t736_init_report *r,uint32_t a,uint32_t want){
 uint32_t v;int rc=read_word(io,r,a,&v);if(rc)return rc;
 return v==want?0:T736_INIT_PRECONDITION;
}
static int guards(const struct t736_phy_io *io,struct t736_init_report *r,uint32_t micro){
 uint32_t i,ch;uint16_t v;int rc;
 for(i=0;i<12;i++){rc=expect_word(io,r,context_address[i],context_value[i]);if(rc)return rc;}
 for(ch=0;ch<2;ch++)for(i=0;i<13;i++){
  rc=expect_word(io,r,0x0a110000+ch*0x400000+held_offset[i],held_value[i]);if(rc)return rc;
 }
 for(i=0;i<6;i++){
  r->last_operation=3;r->last_address=phy_address[i];r->reads_attempted++;
  if(io->read16(io->context,phy_address[i],&v))return T736_INIT_IO;
  r->last_value=v;r->reads_completed++;
  if(v!=(i==1?micro:phy_value[i]))return T736_INIT_PRECONDITION;
 }
 return 0;
}
static int write_verify(const struct t736_phy_io *io,struct t736_init_report *r,uint32_t a,uint32_t value){
 uint32_t got;int rc;
 r->last_operation=2;r->last_address=a;r->last_value=value;r->writes_attempted++;
 if(io->write32(io->context,a,value))return T736_INIT_IO;
 r->writes_completed++;
 rc=read_word(io,r,a,&got);if(rc)return rc;
 return got==value?0:T736_INIT_VERIFY;
}
static int poll_swstat(const struct t736_phy_io *io,struct t736_init_report *r,uint32_t ch,uint32_t wanted,uint32_t timeout){
 uint32_t elapsed,v;int rc;
 for(elapsed=0;;elapsed++){
  rc=read_word(io,r,0x0a110c84+ch*0x400000,&v);if(rc)return rc;
  if(v==wanted)return 0;
  if(elapsed==timeout)return T736_INIT_TIMEOUT;
  if(io->delay_us(io->context,1))return T736_INIT_TIMER;
  r->waited_us++;
 }
}
int t736_init_apply_static(const struct t736_phy_io *io,uint32_t ck_mhz,uint32_t counter_mhz,uint32_t micro,uint32_t timeout,struct t736_init_report *r){
 struct t736_init_profile p;uint32_t i,ch,v;int rc;volatile uint32_t *clear;
 if(!io || !io->read16 || !io->read32 || !io->write32 || !io->delay_us || !r ||
    (micro!=1 && micro!=9) || !timeout || timeout>1000000u)return T736_INIT_INVALID;
 rc=t736_init_build_profile(ck_mhz,counter_mhz,&p);if(rc)return rc;
 clear=(volatile uint32_t *)r;for(i=0;i<sizeof(*r)/sizeof(uint32_t);i++)clear[i]=0;
 r->stage=1;rc=guards(io,r,micro);if(rc)return rc;
 /* Inspect both channels and every prospective field before first write. */
 for(ch=0;ch<2;ch++)for(i=0;i<T736_INIT_WORDS;i++){
  rc=read_word(io,r,config_address[i]+ch*0x400000,&v);if(rc)return rc;
  r->before[ch][i]=v;
  if((v&config_mask[i])!=baseline[i] || (i==0 && (v&0xc0000000u)))return T736_INIT_PRECONDITION;
  r->programmed[ch][i]=(v&~config_mask[i])|p.value[i];
 }
 r->stage=2;
 for(ch=0;ch<2;ch++){
  r->swctl_maybe_open|=1u<<ch;
  rc=write_verify(io,r,0x0a110c80+ch*0x400000,0);if(rc)return rc;
  rc=poll_swstat(io,r,ch,0,timeout);if(rc)return rc;
 }
 r->stage=3;
 for(ch=0;ch<2;ch++){
  r->static_maybe_open|=1u<<ch;
  rc=write_verify(io,r,0x0a110ca4+ch*0x400000,1);if(rc)return rc;
 }
 r->stage=4;
 for(ch=0;ch<2;ch++){
  for(i=0;i<T736_INIT_WORDS;i++){
   rc=write_verify(io,r,config_address[i]+ch*0x400000,r->programmed[ch][i]);if(rc)return rc;
  }
  r->configured_channels|=1u<<ch;
 }
 r->stage=5;
 for(ch=0;ch<2;ch++){
  rc=write_verify(io,r,0x0a110ca4+ch*0x400000,0);if(rc)return rc;
  r->static_maybe_open&=~(1u<<ch);
 }
 r->stage=6;
 for(ch=0;ch<2;ch++){
  rc=write_verify(io,r,0x0a110c80+ch*0x400000,1);if(rc)return rc;
  rc=poll_swstat(io,r,ch,1,timeout);if(rc)return rc;
  r->swctl_maybe_open&=~(1u<<ch);
 }
 r->stage=7;
 /* Source20us settle follows both commits. It is not an init/training wait. */
 if(io->delay_us(io->context,20))return T736_INIT_TIMER;
 r->waited_us+=20;
 rc=guards(io,r,micro);if(rc)return rc;
 for(ch=0;ch<2;ch++)for(i=0;i<T736_INIT_WORDS;i++){
  rc=read_word(io,r,config_address[i]+ch*0x400000,&v);if(rc)return rc;
  if(v!=r->programmed[ch][i])return T736_INIT_VERIFY;
 }
 r->final_valid=1;r->stage=8;return T736_INIT_CONFIGURED_ONLY;
}
