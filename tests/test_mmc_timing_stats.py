#!/usr/bin/env python3
"""Exercise C diagnostic accounting, migration boundaries and fail-closed overflow."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t u64;
typedef uint32_t u32;
#include "t736_mmc_timing_stats_draft.h"

static void event(struct t736_diag_sample *s, unsigned seg,
                  unsigned cpu_a, unsigned cpu_b, uint64_t duration)
{
    t736_diag_append(s, seg, (struct t736_diag_mark){10,cpu_a},
                    (struct t736_diag_mark){10+duration,cpu_b});
}

int main(void)
{
    struct t736_diag_stats stats = {0}, before;
    struct t736_diag_sample sample = {
        .requested_bytes=65536, .completed_bytes=65536, .success=true,
        .poll_iterations=4, .poll_sleeps=3,
    };
    /* Nested intervals must remain independent, rather than double-counted
       into one synthetic service time. Two command calls are one request. */
    event(&sample,T736_DIAG_SERVICE,5,7,800);
    event(&sample,T736_DIAG_DMA_START,5,5,100);
    event(&sample,T736_DIAG_PRE_RESET,5,5,20);
    event(&sample,T736_DIAG_COMMAND_WAIT,5,7,250);
    event(&sample,T736_DIAG_COMMAND_WAIT,7,7,50);
    event(&sample,T736_DIAG_READ_COPY,7,7,300);
    t736_diag_merge(&stats,&sample);
    assert(stats.complete_samples==1 && stats.incomplete_samples==0);
    assert(stats.direction[0].requests==1 && stats.direction[0].successes==1);
    assert(stats.direction[0].requested_bytes==65536 && stats.direction[0].completed_bytes==65536);
    assert(stats.direction[0].size_bins[2]==1 && stats.direction[1].requests==0);
    const struct t736_diag_segment_stats *g=&stats.direction[0].segments[T736_DIAG_COMMAND_WAIT];
    assert(g->calls==2 && g->total_ns==300 && g->max_ns==250);
    assert(g->cpu[5].starts==1 && g->cpu[7].ends==2);
    assert(g->different_endpoints==1 && g->different_endpoint_ns==250);
    assert(g->cpu[7].same_endpoints==1 && g->cpu[7].same_endpoint_ns==50);
    assert(stats.direction[0].segments[T736_DIAG_SERVICE].total_ns==800);
    assert(stats.direction[0].segments[T736_DIAG_DMA_START].total_ns==100);
    assert(stats.direction[0].segments[T736_DIAG_PRE_RESET].total_ns==20);

    /* A DMA-prepare failure is attempted I/O, not completed bytes. */
    sample=(struct t736_diag_sample){.requested_bytes=4096,.write=true};
    event(&sample,T736_DIAG_PRE_RESET,8,9,70);
    event(&sample,T736_DIAG_DMA_START,9,9,100);
    t736_diag_merge(&stats,&sample);
    assert(stats.direction[1].failures==1 && stats.direction[1].completed_bytes==0);
    assert(stats.direction[1].requested_bytes==4096 && stats.direction[1].size_bins[0]==1);
    g=&stats.direction[1].segments[T736_DIAG_PRE_RESET];
    assert(g->cpu[8].starts==1 && g->cpu[8].ends==1);
    assert(g->different_endpoints==1 && g->cpu[8].same_endpoints==0);
    assert(stats.direction[1].segments[T736_DIAG_DMA_START].cpu[8].same_endpoints==1);

    /* An oversized event sequence drops the whole request with explicit
       loss counters, never partially contaminating totals. */
    before=stats;
    sample=(struct t736_diag_sample){.requested_bytes=16384,.success=true};
    for(unsigned i=0;i<T736_DIAG_MAX_EVENTS+2;i++)
        event(&sample,T736_DIAG_COMMAND_WAIT,0,0,i);
    assert(sample.nr_events==T736_DIAG_MAX_EVENTS && sample.overflow==2);
    t736_diag_merge(&stats,&sample);
    assert(stats.complete_samples==before.complete_samples);
    assert(stats.incomplete_samples==1 && stats.dropped_events==2);
    assert(!memcmp(stats.direction,before.direction,sizeof(stats.direction)));
    sample=(struct t736_diag_sample){0};
    t736_diag_append(&sample,T736_DIAG_SERVICE,(struct t736_diag_mark){100,0},(struct t736_diag_mark){99,0});
    event(&sample,T736_DIAG_SEGMENTS,0,0,1);
    t736_diag_merge(&stats,&sample);
    assert(stats.incomplete_samples==2 && stats.dropped_events==4);
    assert(!memcmp(stats.direction,before.direction,sizeof(stats.direction)));
    assert(sizeof(struct t736_diag_sample)<1024);
    printf("PASS: real C accounting, nested segments, multiple commands/request, failed DMA, CPU other-bucket migration, whole-sample overflow/time/order guards; sample=%zu bytes stats=%zu bytes\n",sizeof(sample),sizeof(stats));
}
'''
with tempfile.TemporaryDirectory(prefix="t736-timing-") as temporary:
    root = Path(temporary)
    source, binary = root / "test.c", root / "test"
    source.write_text(SOURCE)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "boards"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
