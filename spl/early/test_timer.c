// SPDX-License-Identifier: GPL-2.0-only
#include "timer_init.h"
#include <assert.h>
#include <stdio.h>

struct fake { uint32_t ctrl, fid, freq, count, writes, addresses[4]; int stalled, drop_fid, drop_en; };
static uint32_t rd(void *p, uint32_t a)
{
	struct fake *f = p;
	assert(a == T736_TIMESTAMP_CTRL || a == T736_TIMESTAMP_FREQID);
	return a == T736_TIMESTAMP_CTRL ? f->ctrl : f->fid;
}
static void wr(void *p, uint32_t a, uint32_t v)
{
	struct fake *f = p;
	assert(a == T736_TIMESTAMP_CTRL || a == T736_TIMESTAMP_FREQID);
	assert(f->writes < 4u); f->addresses[f->writes++] = a;
	if (a == T736_TIMESTAMP_CTRL) { if (!f->drop_en) f->ctrl = v; }
	else if (!f->drop_fid) f->fid = v;
}
static uint32_t count(void *p)
{
	struct fake *f = p;
	if ((f->ctrl & 1u) && !f->stalled) f->count++;
	return f->count;
}
static uint32_t freq(void *p) { return ((struct fake *)p)->freq; }
static int run(struct fake *f, struct t736_timer_state *s)
{
	const struct t736_timer_io io = {f, rd, wr, count, freq};
	return t736_timestamp_prepare(&io, s, 100);
}
int main(void)
{
	struct t736_timer_state s;
	struct fake f = {.ctrl=1, .fid=T736_TIMESTAMP_HZ, .count=100};
	assert(run(&f,&s)==0 && f.writes==0 && s.raw_cntfrq==0 && s.advanced_before==1 && s.effective_hz==T736_TIMESTAMP_HZ);
	f=(struct fake){.count=1234};
	assert(run(&f,&s)==0 && f.writes==2 && f.count>1234 && s.advanced_before==0);
	assert(f.addresses[0]==T736_TIMESTAMP_FREQID && f.addresses[1]==T736_TIMESTAMP_CTRL);
	f=(struct fake){.ctrl=1, .count=0xfffffffeu};
	assert(run(&f,&s)==0 && f.writes==1 && s.advanced_before==1);
	f=(struct fake){.ctrl=1, .fid=T736_TIMESTAMP_HZ, .freq=19200000};
	assert(run(&f,&s)==-2 && f.writes==0 && s.effective_hz==0);
	f=(struct fake){.ctrl=1, .fid=19200000};
	assert(run(&f,&s)==-2 && f.writes==0);
	f=(struct fake){.drop_fid=1};
	assert(run(&f,&s)==-3 && f.writes==1 && f.ctrl==0);
	f=(struct fake){.drop_en=1};
	assert(run(&f,&s)==-3 && f.writes==2 && s.effective_hz==0);
	f=(struct fake){.stalled=1};
	assert(run(&f,&s)==-4 && s.effective_hz==0 && f.count==0);
	f=(struct fake){.ctrl=1, .fid=T736_TIMESTAMP_HZ, .stalled=1};
	assert(run(&f,&s)==-4 && f.writes==0);
	f=(struct fake){.ctrl=1, .fid=T736_TIMESTAMP_HZ, .freq=T736_TIMESTAMP_HZ};
	assert(run(&f,&s)==0 && f.writes==0);
	puts("Timer tests PASS: raw CNTFRQ zero, enabled/no-write, metadata-only, stopped/enable, wrap, mismatches, failed readbacks, no counter-value/CNTFRQ writes");
	return 0;
}
