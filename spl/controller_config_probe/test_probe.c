// SPDX-License-Identifier: GPL-2.0-only
/* Native-only harness faults and independent write-gate validation. */
#define T736_CONFIG_PROBE_NATIVE 1
#include "probe.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct slot { uintptr_t address; uint32_t value; };
static struct slot slots[128];
static uint32_t slot_count, mmio_reads, mmio_writes, fail_write, drift_final;

static uint32_t *get(uintptr_t address)
{
	uint32_t i;
	for (i = 0; i < slot_count; i++) if (slots[i].address == address) return &slots[i].value;
	assert(slot_count < 128u);
	slots[slot_count] = (struct slot){address, 0xa5a55a5au};
	return &slots[slot_count++].value;
}

uint32_t config_test_read32(uintptr_t address)
{
	mmio_reads++;
	if (drift_final && mmio_writes == 64u && address == 0x0a510014u) return 1u;
	return *get(address);
}

void config_test_write32(uintptr_t address, uint32_t value)
{
	mmio_writes++;
	assert(address != 0x0a110510u && address != 0x0a510510u);
	assert(address != 0x0a110c80u && address != 0x0a510c80u);
	assert(address != 0x0a110180u && address != 0x0a510180u);
	if (mmio_writes == fail_write) return;
	*get(address) = value;
}

static void setup(void)
{
	uint32_t i;
	memset(slots, 0, sizeof(slots)); slot_count = mmio_reads = mmio_writes = fail_write = drift_final = 0;
	memset((void *)&probe_report, 0, sizeof(probe_report)); timing_before = 0;
	for (i = 0; i < 6u; i++) {
		*get(context_address[i]) = context_expected[i];
		*get(ctl_address[i]) = i % 3u ? 0u : 0x03040000u;
	}
}

static void successful_configuration(void)
{
	uint32_t i;
	struct rule r;
	setup(); assert(probe_main() == 0);
	assert(probe_report.status == 2u && probe_report.phase == 6u && probe_report.result == 0u);
	assert(mmio_writes == 64u && probe_report.write_attempts == 64u && probe_report.writes_completed == 64u);
	assert(probe_report.readbacks == 64u && probe_report.timing_pre_reads == 42u && !probe_report.pending_flags);
	assert(probe_report.layout.verified_writes == 22u && probe_report.timing.verified_writes == 42u);
	assert(probe_report.context_after_valid == 63u && probe_report.ctl_after_valid == 63u);
	for (i = 0; i < 64u; i++) {
		assert(!rule_at(i, &r));
		assert(probe_report.write_readback[i] == ((0xa5a55a5au & ~r.mask) | r.value));
	}
	for (i = 0; i < 6u; i++) {
		assert(probe_report.masked_before[i] == 0xa5a55a5au);
		assert(probe_report.context_after[i] == context_expected[i]);
		assert(probe_report.ctl_after[i] == (i % 3u ? 0u : 0x03081002u));
	}
}

static void failed_readbacks_stop(void)
{
	uint32_t i;
	for (i = 1; i <= 64u; i++) {
		setup(); fail_write = i;
		assert(probe_main() == T736_CTL_CHECK_VERIFY);
		assert(mmio_writes == i && probe_report.writes_completed == i && probe_report.readbacks == i);
		assert(probe_report.status == (i <= 22u ? 0x202u : 0x204u));
		assert(probe_report.layout.verified_writes + probe_report.timing.verified_writes == i - 1u);
	}
}

static void prerequisite_and_poststate_drift(void)
{
	uint32_t i;
	for (i = 0; i < 6u; i++) {
		setup(); *get(context_address[i]) ^= 1u;
		assert(probe_main() && probe_report.status == 0x101u && !mmio_writes);
		setup(); *get(ctl_address[i]) ^= 1u;
		assert(probe_main() && probe_report.status == 0x102u && !mmio_writes);
	}
	setup(); drift_final = 1;
	assert(probe_main() && probe_report.status == 0x205u && mmio_writes == 64u);
	assert(probe_report.ctl_after[4] == 1u && probe_report.phase == 5u);
}

static void independent_gate(void)
{
	uint32_t value, writes;
	struct rule r;
	setup(); assert(fixed_write(0, 0x0a110000u, 0x03081002u) && !mmio_writes);
	probe_report.observed_channels = 3;
	probe_report.context_before_valid = probe_report.ctl_before_valid = 63;
	probe_report.phase = 2;
	assert(fixed_write(0, 0x0a110510u, 1u) && !mmio_writes);
	assert(fixed_write(0, 0x0a110000u, 0x03081003u) && !mmio_writes);
	assert(!fixed_write(0, 0x0a110000u, 0x03081002u));
	assert(fixed_write(0, 0x0a130004u, 0x18u) && mmio_writes == 1u);
	assert(fixed_read(0, 0x02002020u, &value)); /* Immediate paired readback required. */
	assert(!fixed_read(0, 0x0a110000u, &value));
	assert(fixed_write(0, 0x0a13000cu, 0x00070707u) && mmio_writes == 1u);
	assert(!fixed_write(0, 0x0a130004u, 0x18u));
	setup();
	probe_report.observed_channels = 3;
	probe_report.context_before_valid = probe_report.ctl_before_valid = probe_report.ctl_after_valid = 63;
	probe_report.phase = 4; probe_report.layout.verified_writes = 22;
	probe_report.write_attempts = probe_report.writes_completed = probe_report.readbacks = 40;
	assert(!rule_at(40, &r) && r.mask == 0x8000ffffu);
	assert(fixed_write(0, r.address, r.value) && !mmio_writes); /* Pre-read absent. */
	assert(!fixed_read(0, r.address, &value));
	writes = mmio_writes;
	assert(fixed_write(0, r.address, r.value) && mmio_writes == writes); /* Reserved bits lost. */
	assert(fixed_read(0, 0x02002020u, &value)); /* No unrelated read while pending. */
	assert(!fixed_write(0, r.address, (timing_before & ~r.mask) | r.value));
	assert(mmio_writes == writes + 1u);
}

int main(int argc, char **argv)
{
	successful_configuration();
	if (argc == 2) {
		FILE *f = fopen(argv[1], "wb");
		assert(f && fwrite((const void *)&probe_report, sizeof(probe_report), 1, f) == 1);
		assert(fclose(f) == 0);
	} else assert(argc == 1);
	failed_readbacks_stop(); prerequisite_and_poststate_drift(); independent_gate();
	puts("PASS: 64-write configuration, all 64 readback failures stop, 12 cold-state drifts, final INIT drift and independent gate");
	return 0;
}
