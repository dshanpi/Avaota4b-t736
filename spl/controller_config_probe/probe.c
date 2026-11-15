// SPDX-License-Identifier: GPL-2.0-only
/* Fixed geometry/timing observation. Never enters normal mode or accesses PHY. */
#include "../lpddr4/phy/checked_controller.h"
#ifndef T736_CONFIG_PROBE_NATIVE
#include "../early/platform.h"
#define mmio_read32 t736_read32
#define mmio_write32 t736_write32
#else
uint32_t config_test_read32(uintptr_t address);
void config_test_write32(uintptr_t address, uint32_t value);
#define mmio_read32 config_test_read32
#define mmio_write32 config_test_write32
#endif

#define WRITE_COUNT 64u
#define VERIFY_PENDING 1u
#define TIMING_BEFORE_READY 2u
struct config_report {
	uint32_t magic, version, status, phase, sctlr, result;
	uint32_t write_attempts, writes_completed, readbacks, denied_address;
	uint32_t last_read, last_write, last_value;
	uint32_t context_before_valid, ctl_before_valid, context_after_valid, ctl_after_valid;
	struct t736_ctl_write_report layout, timing;
	uint32_t timing_pre_reads, pending_flags, observed_channels;
	uint32_t context_before[6], ctl_before[6], context_after[6], ctl_after[6];
	uint32_t masked_before[6], write_readback[WRITE_COUNT], reserved[2];
};
_Static_assert(sizeof(struct config_report) == 512, "Config report ABI");
volatile struct config_report probe_report __attribute__((section(".report")));
static uint32_t timing_before;

struct rule { uint32_t address, mask, value; };
static const uint32_t context_address[6] = {
	0x02002020u, 0x02002c00u, 0x02002c0cu, 0x0a020010u, 0x0a020014u, 0x07010250u
};
static const uint32_t context_expected[6] = {
	0xf9004a00u, 0x80000003u, 0x10001u, 0x2728u, 2u, 1u
};
static const uint32_t ctl_address[6] = {
	0x0a110000u, 0x0a110014u, 0x0a110090u, 0x0a510000u, 0x0a510014u, 0x0a510090u
};
/* Independent fixed gate for the reviewed geometry and 1800 MHz profile.
 * These are configuration words, not executable firmware or instruction data.
 */
static const struct rule map_rules[10] = {
	{0x004u, UINT32_MAX, 0x18u}, {0x00cu, UINT32_MAX, 0x00070707u},
	{0x010u, UINT32_MAX, 0x3f3fu}, {0x014u, UINT32_MAX, 0x1f000000u},
	{0x018u, UINT32_MAX, 0u}, {0x01cu, UINT32_MAX, 0x1f070707u},
	{0x020u, UINT32_MAX, 0x07070707u}, {0x024u, UINT32_MAX, 0x07070707u},
	{0x028u, UINT32_MAX, 0x07070707u}, {0x02cu, UINT32_MAX, 0x00000707u}
};
static const struct rule timing_rules[21] = {
	{0x000u, UINT32_MAX, 0x43483d4cu}, {0x004u, UINT32_MAX, 0x000f1072u},
	{0x008u, UINT32_MAX, 0x1224302du}, {0x00cu, UINT32_MAX, 0x001a4b52u},
	{0x010u, UINT32_MAX, 0x21081226u}, {0x014u, UINT32_MAX, 0x040f1b0fu},
	{0x018u, UINT32_MAX, 0x11u}, {0x01cu, UINT32_MAX, 2u},
	{0x024u, UINT32_MAX, 0x00020208u}, {0x030u, UINT32_MAX, 0x00040000u},
	{0x034u, UINT32_MAX, 0x1a200004u}, {0x038u, UINT32_MAX, 0x004802bbu},
	{0x05cu, UINT32_MAX, 0x00640002u}, {0x060u, UINT32_MAX, 0x00080f0fu},
	{0x064u, UINT32_MAX, 0x00024310u}, {0x580u, UINT32_MAX, 0x021f020eu},
	{0x584u, UINT32_MAX, 0x00060202u}, {0x588u, UINT32_MAX, 0x001f1f0eu},
	{0x600u, 0x8000ffffu, 0x1bu}, {0x604u, 0x0fff0fffu, 0x02ac0156u},
	{0x608u, 0xffff0000u, 0x0aa20000u}
};

static int rule_at(uint32_t index, struct rule *r)
{
	uint32_t channel, word;
	if (index >= WRITE_COUNT) return -1;
	if (index < 22u) {
		channel = index / 11u; word = index % 11u;
		if (!word) *r = (struct rule){0x0a110000u, UINT32_MAX, 0x03081002u};
		else { *r = map_rules[word - 1u]; r->address += 0x0a130000u; }
	} else {
		channel = (index - 22u) / 21u; word = (index - 22u) % 21u;
		*r = timing_rules[word]; r->address += 0x0a100000u;
	}
	r->address += channel * 0x00400000u;
	return 0;
}

static int deny(uint32_t address)
{
	probe_report.denied_address = address;
	return -1;
}

static int fixed_read(void *context, uint32_t address, uint32_t *value)
{
	uint32_t i, index = probe_report.writes_completed;
	struct rule r;
	(void)context;
	if (probe_report.pending_flags & VERIFY_PENDING) {
		if (!index || address != probe_report.last_write) return deny(address);
		probe_report.last_read = address;
		*value = mmio_read32(address);
		probe_report.write_readback[index - 1u] = *value;
		probe_report.readbacks++;
		probe_report.pending_flags &= ~VERIFY_PENDING;
		return 0;
	}
	if (probe_report.phase == 4u && index >= 22u && !rule_at(index, &r) && address == r.address) {
		if (probe_report.pending_flags) return deny(address);
		probe_report.last_read = address;
		*value = mmio_read32(address); timing_before = *value;
		probe_report.timing_pre_reads++;
		probe_report.pending_flags |= TIMING_BEFORE_READY;
		i = (index - 22u) % 21u;
		if (i >= 18u) probe_report.masked_before[((index - 22u) / 21u) * 3u + i - 18u] = *value;
		return 0;
	}
	/* No unpaired reads while a timing write is waiting for its exact pre-read. */
	if (probe_report.pending_flags) return deny(address);
	for (i = 0; i < 6u; i++)
		if (address == context_address[i] || address == ctl_address[i]) {
			probe_report.last_read = address;
			*value = mmio_read32(address);
			return 0;
		}
	return deny(address);
}

static int fixed_write(void *context, uint32_t address, uint32_t value)
{
	uint32_t index = probe_report.writes_completed, expected;
	struct rule r;
	(void)context;
	if (probe_report.observed_channels != 3u || probe_report.context_before_valid != 63u ||
	    probe_report.ctl_before_valid != 63u || probe_report.write_attempts != index ||
	    probe_report.readbacks != index || rule_at(index, &r) || address != r.address)
		return deny(address);
	if (index < 22u) {
		if (probe_report.phase != 2u || probe_report.pending_flags) return deny(address);
		expected = r.value;
	} else {
		if (probe_report.phase != 4u || probe_report.pending_flags != TIMING_BEFORE_READY ||
		    probe_report.layout.verified_writes != 22u || probe_report.ctl_after_valid != 63u)
			return deny(address);
		expected = (timing_before & ~r.mask) | r.value;
	}
	if (value != expected) return deny(address);
	probe_report.last_write = address; probe_report.last_value = value;
	probe_report.write_attempts++;
	mmio_write32(address, value);
	probe_report.writes_completed++;
	probe_report.pending_flags = VERIFY_PENDING;
	return 0;
}

static int snapshot_context(volatile uint32_t *words, volatile uint32_t *valid)
{
	uint32_t i, value;
	*valid = 0;
	for (i = 0; i < 6u; i++) {
		if (fixed_read(0, context_address[i], &value)) return -1;
		words[i] = value; *valid |= 1u << i;
		if (value != context_expected[i]) return -1;
	}
	return 0;
}

static int snapshot_ctl(volatile uint32_t *words, volatile uint32_t *valid, uint32_t mstr)
{
	uint32_t i, value;
	*valid = 0;
	for (i = 0; i < 6u; i++) {
		if (fixed_read(0, ctl_address[i], &value)) return -1;
		words[i] = value; *valid |= 1u << i;
		if (value != (i % 3u ? 0u : mstr)) return -1;
	}
	return 0;
}

static int finish(uint32_t status, int result)
{
	probe_report.status = status; probe_report.result = (uint32_t)result;
#ifndef T736_CONFIG_PROBE_NATIVE
	__asm__ volatile("dsb sy" ::: "memory");
#endif
	return result;
}

int probe_main(void)
{
	const struct t736_phy_io io = {0, 0, 0, fixed_read, fixed_write, 0};
	const struct t736_lp4_geometry geometry = {2, 2, 16, 17, 10, 3};
	volatile uint32_t *words = (volatile uint32_t *)&probe_report;
	uint32_t i, sctlr;
	int rc;
	for (i = 0; i < 128u; i++) words[i] = 0;
	timing_before = 0;
	probe_report.magic = 0x54374346u; /* T7CF */
	probe_report.version = 1; probe_report.status = 1; probe_report.phase = 1;
#ifndef T736_CONFIG_PROBE_NATIVE
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
#else
	sctlr = 0x00c50838u;
#endif
	probe_report.sctlr = sctlr;
	if (sctlr & 0x3005u) return finish(0x100u, -1);
	if (snapshot_context(probe_report.context_before, &probe_report.context_before_valid)) return finish(0x101u, -1);
	if (snapshot_ctl(probe_report.ctl_before, &probe_report.ctl_before_valid, 0x03040000u)) return finish(0x102u, -1);
	probe_report.observed_channels = 3;
	probe_report.phase = 2;
	rc = t736_ctl_layout_checked(&io, 3, 1, &geometry, &probe_report.layout);
	if (rc) return finish(0x202u, rc);
	probe_report.phase = 3;
	if (probe_report.writes_completed != 22u || probe_report.readbacks != 22u || probe_report.pending_flags ||
	    snapshot_ctl(probe_report.ctl_after, &probe_report.ctl_after_valid, 0x03081002u)) return finish(0x203u, -1);
	probe_report.phase = 4;
	rc = t736_ctl_timing_checked(&io, 3, 1, 1800u, &probe_report.timing);
	if (rc) return finish(0x204u, rc);
	probe_report.phase = 5;
	if (probe_report.writes_completed != WRITE_COUNT || probe_report.readbacks != WRITE_COUNT ||
	    probe_report.timing_pre_reads != 42u || probe_report.pending_flags ||
	    snapshot_ctl(probe_report.ctl_after, &probe_report.ctl_after_valid, 0x03081002u) ||
	    snapshot_context(probe_report.context_after, &probe_report.context_after_valid)) return finish(0x205u, -1);
	probe_report.phase = 6;
	return finish(2u, 0);
}
