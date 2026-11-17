/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded aggregate diagnostics. Caller supplies u64/u32/bool types. */
#ifndef T736_MMC_TIMING_STATS_DRAFT_H
#define T736_MMC_TIMING_STATS_DRAFT_H

#define T736_DIAG_CPU_COUNT 8
#define T736_DIAG_CPU_BUCKETS (T736_DIAG_CPU_COUNT + 1)
#define T736_DIAG_MAX_EVENTS 32

enum t736_diag_segment {
	T736_DIAG_DISPATCH,
	T736_DIAG_SERVICE,
	T736_DIAG_DMA_START,
	T736_DIAG_PRE_RESET,
	T736_DIAG_WRITE_COPY,
	T736_DIAG_COMMAND_WAIT,
	T736_DIAG_BUSY_WAIT,
	T736_DIAG_POST_RESET,
	T736_DIAG_READ_COPY,
	T736_DIAG_COMPLETION,
	T736_DIAG_WORKER_TOTAL,
	T736_DIAG_SEGMENTS
};

struct t736_diag_mark { u64 ns; u32 cpu; };
struct t736_diag_event {
	u64 ns;
	u32 segment, cpu_start, cpu_end;
};
struct t736_diag_sample {
	struct t736_diag_event events[T736_DIAG_MAX_EVENTS];
	u32 nr_events, overflow;
	u64 requested_bytes, completed_bytes, poll_iterations, poll_sleeps;
	bool write, success;
};
struct t736_diag_cpu_stats {
	u64 starts, ends, same_endpoints, same_endpoint_ns;
};
struct t736_diag_segment_stats {
	u64 calls, total_ns, max_ns, different_endpoints, different_endpoint_ns;
	struct t736_diag_cpu_stats cpu[T736_DIAG_CPU_BUCKETS];
};
struct t736_diag_direction_stats {
	u64 requests, successes, failures, requested_bytes, completed_bytes;
	u64 size_bins[4], poll_iterations, poll_sleeps;
	struct t736_diag_segment_stats segments[T736_DIAG_SEGMENTS];
};
struct t736_diag_stats {
	u64 complete_samples, incomplete_samples, dropped_events;
	struct t736_diag_direction_stats direction[2];
};

static inline void t736_diag_append(struct t736_diag_sample *s,
				   enum t736_diag_segment segment,
				   struct t736_diag_mark start,
				   struct t736_diag_mark end)
{
	struct t736_diag_event *e;

	if (s->nr_events >= T736_DIAG_MAX_EVENTS || end.ns < start.ns ||
	    (unsigned int)segment >= T736_DIAG_SEGMENTS) {
		s->overflow++;
		return;
	}
	e = &s->events[s->nr_events++];
	e->ns = end.ns - start.ns;
	e->segment = segment;
	e->cpu_start = start.cpu;
	e->cpu_end = end.cpu;
}

static inline unsigned int t736_diag_cpu_bucket(unsigned int cpu)
{
	return cpu < T736_DIAG_CPU_COUNT ? cpu : T736_DIAG_CPU_COUNT;
}

/* Only complete samples enter totals. An overflow is visible and drops the
 * entire request, so partial segment totals cannot masquerade as a sample.
 */
static inline void t736_diag_merge(struct t736_diag_stats *stats,
				  const struct t736_diag_sample *s)
{
	struct t736_diag_direction_stats *d;
	unsigned int i, bin;

	if (s->overflow) {
		stats->incomplete_samples++;
		stats->dropped_events += s->overflow;
		return;
	}
	stats->complete_samples++;
	d = &stats->direction[s->write ? 1 : 0];
	d->requests++;
	d->successes += s->success;
	d->failures += !s->success;
	d->requested_bytes += s->requested_bytes;
	d->completed_bytes += s->completed_bytes;
	bin = s->requested_bytes <= 4096 ? 0 :
	      s->requested_bytes <= 16384 ? 1 :
	      s->requested_bytes <= 65536 ? 2 : 3;
	d->size_bins[bin]++;
	d->poll_iterations += s->poll_iterations;
	d->poll_sleeps += s->poll_sleeps;
	for (i = 0; i < s->nr_events; i++) {
		const struct t736_diag_event *e = &s->events[i];
		struct t736_diag_segment_stats *g = &d->segments[e->segment];
		unsigned int a = t736_diag_cpu_bucket(e->cpu_start);
		unsigned int b = t736_diag_cpu_bucket(e->cpu_end);

		g->calls++;
		g->total_ns += e->ns;
		if (e->ns > g->max_ns)
			g->max_ns = e->ns;
		g->cpu[a].starts++;
		g->cpu[b].ends++;
		/* Compare real CPU IDs, not the shared out-of-range bucket. */
		if (e->cpu_start == e->cpu_end) {
			g->cpu[a].same_endpoints++;
			g->cpu[a].same_endpoint_ns += e->ns;
		} else {
			g->different_endpoints++;
			g->different_endpoint_ns += e->ns;
		}
	}
}
#endif
