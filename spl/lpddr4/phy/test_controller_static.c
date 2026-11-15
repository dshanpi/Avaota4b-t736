// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "controller_static.c"

struct slot {uint32_t address,value;};
struct model {
 struct slot slots[128];uint32_t count,calls,reads,writes,delays,waited;
 uint32_t fail_call,drift_read,verify_fault,stall_swctl,micro;
 uint32_t write_address[22],write_value[22];
};
static uint32_t *slot(struct model *m,uint32_t address){
 uint32_t i;for(i=0;i<m->count;i++)if(m->slots[i].address==address)return &m->slots[i].value;
 assert(m->count<128);m->slots[m->count]=(struct slot){address,0};return &m->slots[m->count++].value;
}
static int call(struct model *m){m->calls++;return m->calls==m->fail_call;}
static uint32_t drift_mask(uint32_t a){return (a==0x0a110d04 || a==0x0a510d04)?0x10000u:1u;}
static int read32(void *context,uint32_t a,uint32_t *v){
 struct model *m=context;assert(!(a&3u));m->reads++;
 if(call(m))return -1;
 *v=*slot(m,a);
 if(m->drift_read==m->reads)*v^=drift_mask(a);
 if(m->verify_fault==m->writes && m->writes && a==m->write_address[m->writes-1]){*v^=1u;m->verify_fault=0;}
 return 0;
}
static int read16(void *context,uint32_t a,uint16_t *v){
 struct model *m=context;assert(!(a&1u));m->reads++;
 assert(a==0x0aaa0000 || a==0x0aaa0132 || a==0x0aa80100 || a==0x0a9400c0 || a==0x0a940620 || a==0x0a940622);
 if(call(m))return -1;
 *v=(uint16_t)*slot(m,a);if(m->drift_read==m->reads)*v^=1u;return 0;
}
static int write32(void *context,uint32_t a,uint32_t v){
 static const uint32_t order[22]={
  0x0a110c80,0x0a510c80,0x0a110ca4,0x0a510ca4,
  0x0a110d00,0x0a110d04,0x0a100500,0x0a100504,0x0a100508,0x0a10050c,0x0a100d00,
  0x0a510d00,0x0a510d04,0x0a500500,0x0a500504,0x0a500508,0x0a50050c,0x0a500d00,
  0x0a110ca4,0x0a510ca4,0x0a110c80,0x0a510c80};
 struct model *m=context;int failure;
 assert(m->writes<22 && a==order[m->writes] && !(a&3u));
 /* This independent whitelist excludes MRCTRL, PWRCTL, DFI, PHY and the
  * existing64-word geometry/timing subset. */
 if(m->writes<2)assert(v==0);
 if(m->writes>=2 && m->writes<4)assert(v==1);
 if(m->writes>=4 && m->writes<18){
  uint32_t ch=m->writes>=11u,i=(m->writes-4u)%7u;
  static const uint32_t expected[7]={0x1036f,0x580000,0xfc003f,0x330008,0x40072,0x24001d,1};
  uint32_t old=*slot(m,a),mask=i==0?0x03ff1fffu:i==1?0x03ff0000u:i==6?1u:UINT32_MAX;
  (void)ch;assert(v==((old&~mask)|expected[i]));
  assert(*slot(m,0x0a110c80)==0 && *slot(m,0x0a510c80)==0);
  assert(*slot(m,0x0a110ca4)==1 && *slot(m,0x0a510ca4)==1);
 }
 if(m->writes==18 || m->writes==19)assert(v==0);
 if(m->writes>=20)assert(v==1);
 m->write_address[m->writes]=a;m->write_value[m->writes]=v;m->writes++;
 failure=call(m);
 /* Model even a failed callback as having changed hardware. */
 *slot(m,a)=v;
 if((a==0x0a110c80 || a==0x0a510c80) && m->writes!=m->stall_swctl)*slot(m,a+4)=v;
 return failure?-1:0;
}
static int delay_us(void *context,uint32_t us){
 struct model *m=context;assert(us==1 || us==20);m->delays++;
 if(call(m))return -1;
 m->waited+=us;return 0;
}
static void setup(struct model *m,uint32_t micro){
 uint32_t i,ch;memset(m,0,sizeof(*m));m->micro=micro;
 for(i=0;i<12;i++)*slot(m,context_address[i])=context_value[i];
 for(ch=0;ch<2;ch++)for(i=0;i<13;i++)*slot(m,0x0a110000+ch*0x400000+held_offset[i])=held_value[i];
 for(i=0;i<6;i++)*slot(m,phy_address[i])=i==1?micro:phy_value[i];
 for(ch=0;ch<2;ch++)for(i=0;i<7;i++)*slot(m,config_address[i]+ch*0x400000)=baseline[i];
}
static struct t736_phy_io io_for(struct model *m){return (struct t736_phy_io){m,read16,0,read32,write32,delay_us};}
static void success(struct model *m,struct t736_init_report *r){
 struct t736_phy_io io=io_for(m);
 assert(t736_init_apply_static(&io,1800,450,m->micro,3,r)==T736_INIT_CONFIGURED_ONLY);
 assert(m->writes==22 && m->reads==142 && m->calls==165 && m->delays==1 && m->waited==20);
 assert(r->stage==8 && r->final_valid && r->configured_channels==3 && !r->swctl_maybe_open && !r->static_maybe_open);
 assert(r->reads_attempted==142 && r->reads_completed==142 && r->writes_attempted==22 && r->writes_completed==22 && r->waited_us==20);
 assert(*slot(m,0x0a110c80)==1 && *slot(m,0x0a510c80)==1 && !*slot(m,0x0a110ca4) && !*slot(m,0x0a510ca4));
 assert(*slot(m,0x0aaa0132)==m->micro && *slot(m,0x0a9400c0)==2);
 assert(*slot(m,0x0a110080)==0x30 && *slot(m,0x0a510080)==0x30);
}
int main(void){
 struct model m;struct t736_init_report r;struct t736_phy_io io;struct t736_init_profile p,old;
 uint32_t i,a=0xfeed,b=0xbeef,callbacks;int rc;
 assert(!t736_init_us_to_x1024(2000,450,13,1,&a) && a==879);
 assert(!t736_init_us_to_x1024(2,450,10,1,&a) && a==1);
 assert(!t736_init_us_to_x1024(200,450,10,1,&a) && a==88);
 assert(!t736_init_us_to_x1024(1024,1,13,1,&a) && a==1);
 assert(!t736_init_us_to_x1024(1025,1,13,1,&a) && a==2);
 assert(!t736_init_us_to_x1024(1,1,10,7,&a) && a==7);
 a=0xfeed;assert(t736_init_us_to_x1024(UINT32_MAX,UINT32_MAX,32,1,&a)==T736_INIT_UNSUPPORTED && a==0xfeed);
 assert(t736_init_us_to_x1024(1,1,3,8,&a)==T736_INIT_UNSUPPORTED && a==0xfeed);
 assert(t736_init_us_to_x1024(0,1,3,1,&a)==T736_INIT_INVALID);
 assert(t736_init_us_to_x1024(1,0,3,1,&a)==T736_INIT_INVALID);
 assert(t736_init_us_to_x1024(1,1,33,1,&a)==T736_INIT_INVALID);
 assert(t736_init_us_to_x1024(1,1,3,1,0)==T736_INIT_INVALID);
 assert(!t736_init_encode_latency(36,18,40,&a,&b) && a==0xfc && b==0x3f);
 assert(!t736_init_encode_latency(32,16,34,&a,&b) && a==0xec && b==0x36);
 a=0xfeed;b=0xbeef;assert(t736_init_encode_latency(36,16,40,&a,&b)==T736_INIT_UNSUPPORTED && a==0xfeed && b==0xbeef);
 assert(!t736_init_build_profile(1800,450,&p));assert(p.value[0]==0x1036f && p.value[1]==0x580000 && p.mr[3]==8);
 old=p;assert(t736_init_build_profile(1800,1800,&p)==T736_INIT_UNSUPPORTED && !memcmp(&p,&old,sizeof(p)));
 assert(t736_init_build_profile(400,100,&p)==T736_INIT_UNSUPPORTED && !memcmp(&p,&old,sizeof(p)));
 setup(&m,9);success(&m,&r);callbacks=m.calls;
 setup(&m,1);success(&m,&r);
 /* Every callback can fail, including post-commit verification and settle.
  * No retry or cleanup callback is permitted after the failure. */
 for(i=1;i<=callbacks;i++){
  setup(&m,9);m.fail_call=i;io=io_for(&m);
  rc=t736_init_apply_static(&io,1800,450,9,3,&r);
  assert(rc==T736_INIT_IO || rc==T736_INIT_TIMER);assert(m.calls==i && !r.final_valid);
  assert(r.writes_attempted==m.writes);
 }
 for(i=1;i<=22;i++){
  setup(&m,9);m.verify_fault=i;io=io_for(&m);
  assert(t736_init_apply_static(&io,1800,450,9,3,&r)==T736_INIT_VERIFY);
  assert(m.writes==i && r.writes_completed==i && !r.final_valid);
 }
 for(i=1;i<=58;i++){
  setup(&m,9);m.drift_read=i;io=io_for(&m);
  assert(t736_init_apply_static(&io,1800,450,9,3,&r)==T736_INIT_PRECONDITION);
  assert(m.reads==i && !m.writes);
 }
 /* Preserve unrelated fields without preserving stale MR operands. */
 setup(&m,9);*slot(&m,0x0a110d00)|=0x04008000;*slot(&m,0x0a110d04)|=0x40000055;*slot(&m,0x0a100d00)=0x80000200;
 success(&m,&r);assert(*slot(&m,0x0a110d00)==0x0401836f && *slot(&m,0x0a110d04)==0x40580055 && *slot(&m,0x0a100d00)==0x80000201);
 setup(&m,9);*slot(&m,0x0a110d00)|=0x40000000;io=io_for(&m);
 assert(t736_init_apply_static(&io,1800,450,9,3,&r)==T736_INIT_PRECONDITION && !m.writes);
 /* SWSTAT waits are time bounded for both opening and committing. */
 for(i=0;i<4;i++){
  static const uint32_t sw_writes[4]={1,2,21,22};
  setup(&m,9);m.stall_swctl=sw_writes[i];io=io_for(&m);
  assert(t736_init_apply_static(&io,1800,450,9,3,&r)==T736_INIT_TIMEOUT);
  assert(m.writes==sw_writes[i] && m.delays==3 && m.waited==3 && !r.final_valid && r.swctl_maybe_open);
 }
 setup(&m,9);io=io_for(&m);memset(&r,0x5a,sizeof(r));
 assert(t736_init_apply_static(&io,1800,450,2,3,&r)==T736_INIT_INVALID && !m.calls);
 assert(t736_init_apply_static(&io,1800,1800,9,3,&r)==T736_INIT_UNSUPPORTED && !m.calls);
 assert(t736_init_apply_static(&io,1800,450,9,0,&r)==T736_INIT_INVALID && !m.calls);
 assert(t736_init_apply_static(&io,1800,450,9,1000001,&r)==T736_INIT_INVALID && !m.calls);
 assert(r.stage==0x5a5a5a5a);
 puts("PASS: checked1024-cycle waits, latency/MR encoding,450MHz qualification, micro1/9,22 exact writes,142 reads,165 callback failures,22 readback faults,58 preflight drifts,4 SWSTAT timeouts,no cleanup,unrelated-bit preservation");
 return 0;
}
