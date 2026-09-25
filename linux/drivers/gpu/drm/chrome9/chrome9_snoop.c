// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */

/*
 * Debug ladder for RTSF PTE bit 25 (vendor "SF snoop"), steps T1-P..T5 of
 * docs/research/chrome9-gart-snoop-re.md section 6.  Kernel-owned DMA32
 * scratch pages, one validated 2D COPY_RECT job per step, results read
 * through a plain-PTE page after CLFLUSH or with deliberate cached reads.
 */

#include <linux/bitmap.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/vmalloc.h>

#ifdef CONFIG_X86
#include <asm/cacheflush.h>
#endif

#include <drm/drm_mm.h>

#include "chrome9_drv.h"
#include "chrome9_snoop.h"

static bool chrome9_selftest_snoop;
module_param_named(selftest_snoop, chrome9_selftest_snoop, bool, 0400);
MODULE_PARM_DESC(selftest_snoop,
	"Debug: run the GART bit-25 snoop ladder at probe on scratch pages (default: false)");

static uint chrome9_selftest_snoop_soak = 1000;
module_param_named(selftest_snoop_soak, chrome9_selftest_snoop_soak, uint,
		   0400);
MODULE_PARM_DESC(selftest_snoop_soak,
	"Debug: T5 soak iterations of the snoop ladder, 0 skips T5 (default: 1000)");

static uint chrome9_selftest_snoop_soak_ms = 5000;
module_param_named(selftest_snoop_soak_ms, chrome9_selftest_snoop_soak_ms,
		   uint, 0400);
MODULE_PARM_DESC(selftest_snoop_soak_ms,
	"Debug: wall-time bound of the T5 soak in ms (default: 5000)");

#define SNOOP_LINE		64U
#define SNOOP_WORDS		(SNOOP_LINE / sizeof(u32))
#define SNOOP_LINES		(PAGE_SIZE / SNOOP_LINE)
#define SNOOP_PAGE_WORDS	(PAGE_SIZE / sizeof(u32))
#define SNOOP_POISON		0xdeadbeefU
#define SNOOP_LOAD_BYTES	SZ_8M
#define SNOOP_STREAM_EVERY	64U
#define SNOOP_CHECK_EVERY	256U
#define SNOOP_T1P_ATTEMPTS	3U
/* RTSF PTE attribute under test (vendor "SF snoop"). */
#define SNOOP_PTE_SNOOP		BIT(25)

/* Every role page sits between two guard pages of its own PTE flavour. */
enum chrome9_snoop_role {
	SNOOP_TEST_S,
	SNOOP_TEST_P,
	SNOOP_READBACK,
	SNOOP_SOURCE,
	SNOOP_ROLES
};

#define SNOOP_PAGES		(3U * SNOOP_ROLES)
#define SNOOP_ROLE_PAGE(r)	(3U * (r) + 1U)

struct chrome9_snoop_count {
	u32 fresh;
	u32 stale;
	u32 corrupt;
	/* First corrupt line, word and value. */
	int bad_line;
	u32 bad_word;
	u32 bad_value;
	u32 bad_expect;
};

struct chrome9_snoop {
	struct chrome9_device *cdev;
	struct device *dev;
	struct drm_mm_node node;
	struct page *page[SNOOP_PAGES];
	u32 *cpu[SNOOP_PAGES];
	dma_addr_t dma[SNOOP_PAGES];
	u32 *exp;
	u32 *alt;
	u64 hash;
	u16 gen;
	/* Why the ladder stopped early; NULL while it runs. */
	const char *abort;
	/* Pages may still be written by the device: never free them. */
	bool retain;
	unsigned int mapped;
	u8 *load;
	struct completion done;
};

struct chrome9_snoop_store {
	u32 *page;
	u16 gen;
	const unsigned long *mask;
	unsigned int word;
	unsigned int words;
	bool read;
	bool remote;
	int cpu;
	int remote_cpu;
};

static u32 chrome9_snoop_word(u16 gen, u32 line, u32 word)
{
	return 0x5a000000U ^ (u32)gen << 16 ^ line << 4 ^ word;
}

/* Pattern words [word, word + words) of the lines in mask (NULL: all). */
static void chrome9_snoop_fill(u32 *page, u16 gen, const unsigned long *mask,
			       unsigned int word, unsigned int words)
{
	unsigned int line;
	unsigned int w;

	for (line = 0; line < SNOOP_LINES; line++) {
		if (mask && !test_bit(line, mask))
			continue;
		for (w = word; w < word + words; w++)
			WRITE_ONCE(page[line * SNOOP_WORDS + w],
				   chrome9_snoop_word(gen, line, w));
	}
}

static void chrome9_snoop_read_lines(const u32 *page, const unsigned long *mask)
{
	unsigned int line;
	unsigned int w;

	for (line = 0; line < SNOOP_LINES; line++) {
		if (mask && !test_bit(line, mask))
			continue;
		for (w = 0; w < SNOOP_WORDS; w++)
			(void)READ_ONCE(page[line * SNOOP_WORDS + w]);
	}
}

static void chrome9_snoop_flush(void *page)
{
	mb();
#ifdef CONFIG_X86
	clflush_cache_range(page, PAGE_SIZE);
#endif
	mb();
}

/* Per line: fresh (all words exp), stale (only exp/alt words), corrupt. */
static void chrome9_snoop_classify(const u32 *page, const u32 *exp,
				   const u32 *alt,
				   struct chrome9_snoop_count *count)
{
	unsigned int line;
	unsigned int w;

	memset(count, 0, sizeof(*count));
	count->bad_line = -1;
	for (line = 0; line < SNOOP_LINES; line++) {
		bool stale = false;
		bool corrupt = false;

		for (w = 0; w < SNOOP_WORDS; w++) {
			unsigned int i = line * SNOOP_WORDS + w;
			u32 v = READ_ONCE(page[i]);

			if (v == exp[i])
				continue;
			if (v == alt[i]) {
				stale = true;
				continue;
			}
			if (!corrupt && count->bad_line < 0) {
				count->bad_line = line;
				count->bad_word = w;
				count->bad_value = v;
				count->bad_expect = exp[i];
			}
			corrupt = true;
		}
		if (corrupt)
			count->corrupt++;
		else if (stale)
			count->stale++;
		else
			count->fresh++;
	}
}

static int chrome9_snoop_classify_selftest(void)
{
	struct chrome9_snoop_count count;
	u32 *buf;
	int ret = 0;

	buf = kmalloc_array(3, PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	chrome9_snoop_fill(buf, 2, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_fill(buf + SNOOP_PAGE_WORDS, 2, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_fill(buf + 2 * SNOOP_PAGE_WORDS, 1, NULL, 0,
			   SNOOP_WORDS);
	/* Line 1 stale in one word, line 2 corrupt. */
	buf[SNOOP_WORDS + 3] = buf[2 * SNOOP_PAGE_WORDS + SNOOP_WORDS + 3];
	buf[2 * SNOOP_WORDS + 5] ^= 0x100;
	chrome9_snoop_classify(buf, buf + SNOOP_PAGE_WORDS,
			       buf + 2 * SNOOP_PAGE_WORDS, &count);
	if (count.fresh != SNOOP_LINES - 2 || count.stale != 1 ||
	    count.corrupt != 1 || count.bad_line != 2 || count.bad_word != 5 ||
	    chrome9_snoop_word(1, 0, 0) == chrome9_snoop_word(2, 0, 0))
		ret = -EINVAL;
	kfree(buf);
	return ret;
}

static void chrome9_snoop_store_fn(void *arg)
{
	struct chrome9_snoop_store *s = arg;

	s->remote_cpu = smp_processor_id();
	if (s->read)
		chrome9_snoop_read_lines(s->page, s->mask);
	else
		chrome9_snoop_fill(s->page, s->gen, s->mask, s->word,
				   s->words);
}

/* Runs with preemption disabled directly before the doorbell. */
static void chrome9_snoop_cpu_step(void *arg)
{
	struct chrome9_snoop_store *s = arg;
	unsigned int other;

	s->cpu = smp_processor_id();
	s->remote_cpu = -1;
	if (!s->remote) {
		chrome9_snoop_store_fn(s);
		s->remote_cpu = -1;
		return;
	}
	other = cpumask_any_but(cpu_online_mask, s->cpu);
	if (other < nr_cpu_ids)
		smp_call_function_single(other, chrome9_snoop_store_fn, s, 1);
}

static u16 chrome9_snoop_next_gen(struct chrome9_snoop *t)
{
	return ++t->gen;
}

static u32 *chrome9_snoop_role(struct chrome9_snoop *t,
			       enum chrome9_snoop_role role)
{
	return t->cpu[SNOOP_ROLE_PAGE(role)];
}

static u32 chrome9_snoop_sf(struct chrome9_snoop *t,
			    enum chrome9_snoop_role role)
{
	return t->node.start + SNOOP_ROLE_PAGE(role) * PAGE_SIZE;
}

static u32 chrome9_snoop_guard_word(unsigned int page, unsigned int i)
{
	return 0x6a5d0000U ^ page << 12 ^ i;
}

static bool chrome9_snoop_is_guard(unsigned int page)
{
	return page % 3 != 1;
}

/* Guards are compared after CLFLUSH, GART against the post-map hash. */
static bool chrome9_snoop_check(struct chrome9_snoop *t, const char *step)
{
	unsigned int p;
	unsigned int i;
	u64 hash;

	for (p = 0; p < SNOOP_PAGES; p++) {
		if (!chrome9_snoop_is_guard(p))
			continue;
		chrome9_snoop_flush(t->cpu[p]);
		for (i = 0; i < SNOOP_PAGE_WORDS; i++) {
			if (READ_ONCE(t->cpu[p][i]) ==
			    chrome9_snoop_guard_word(p, i))
				continue;
			dev_err(t->dev,
				"snoop %s: guard page %u word %u = %08x, expected %08x\n",
				step, p, i, READ_ONCE(t->cpu[p][i]),
				chrome9_snoop_guard_word(p, i));
			t->abort = "guard";
			t->retain = true;
			return false;
		}
	}
	hash = chrome9_snoop_gart_hash(t->cdev);
	if (hash != t->hash) {
		dev_err(t->dev, "snoop %s: GART hash %016llx, expected %016llx\n",
			step, hash, t->hash);
		t->abort = "gart-hash";
		t->retain = true;
		return false;
	}
	return true;
}

static int chrome9_snoop_copy(struct chrome9_snoop *t,
			      enum chrome9_snoop_role dst,
			      enum chrome9_snoop_role src, u32 x, u32 width,
			      struct chrome9_snoop_store *step)
{
	struct chrome9_snoop_copy copy = {
		.dst = chrome9_snoop_sf(t, dst),
		.src = chrome9_snoop_sf(t, src),
		.pitch = SNOOP_LINE,
		.x = x,
		.width = width,
		.height = SNOOP_LINES,
	};
	int ret;

	ret = chrome9_snoop_run_copy(t->cdev, &copy,
				     step ? chrome9_snoop_cpu_step : NULL, step);
	if (ret) {
		t->abort = "fence";
		t->retain = true;
	}
	return ret;
}

static bool chrome9_snoop_corrupt(struct chrome9_snoop *t, const char *step,
				  const struct chrome9_snoop_count *c)
{
	if (!c->corrupt)
		return false;
	dev_err(t->dev,
		"snoop %s: corrupt line %d word %u = %08x, expected %08x\n",
		step, c->bad_line, c->bad_word, c->bad_value, c->bad_expect);
	t->abort = "corrupt";
	t->retain = true;
	return true;
}

static bool chrome9_snoop_all_fresh(const struct chrome9_snoop_count *c)
{
	return c->fresh == SNOOP_LINES;
}

/* T1: DRAM gen0, CPU-dirty gen1, GPU copies the test page to the readback. */
static int chrome9_snoop_t1(struct chrome9_snoop *t,
			    enum chrome9_snoop_role test, bool remote,
			    const unsigned long *mask,
			    struct chrome9_snoop_count *c,
			    struct chrome9_snoop_store *s)
{
	u32 *page = chrome9_snoop_role(t, test);
	u32 *rb = chrome9_snoop_role(t, SNOOP_READBACK);
	u16 gen0 = chrome9_snoop_next_gen(t);
	u16 gen1 = chrome9_snoop_next_gen(t);
	int ret;

	chrome9_snoop_fill(page, gen0, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_flush(page);
	memset32(rb, SNOOP_POISON, SNOOP_PAGE_WORDS);
	chrome9_snoop_flush(rb);
	*s = (struct chrome9_snoop_store){
		.page = page, .gen = gen1, .mask = mask,
		.words = SNOOP_WORDS, .remote = remote,
	};
	ret = chrome9_snoop_copy(t, SNOOP_READBACK, test, 0, SNOOP_WORDS, s);
	if (ret)
		return ret;
	chrome9_snoop_fill(t->exp, gen0, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_fill(t->exp, gen1, mask, 0, SNOOP_WORDS);
	chrome9_snoop_fill(t->alt, gen0, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_flush(rb);
	chrome9_snoop_classify(rb, t->exp, t->alt, c);
	chrome9_snoop_flush(page);
	return 0;
}

/* T2: clean cached gen0 lines, GPU writes gen1 from the source page. */
static int chrome9_snoop_t2(struct chrome9_snoop *t,
			    enum chrome9_snoop_role test,
			    const unsigned long *mask,
			    struct chrome9_snoop_count *cached,
			    struct chrome9_snoop_count *flushed)
{
	u32 *page = chrome9_snoop_role(t, test);
	u32 *src = chrome9_snoop_role(t, SNOOP_SOURCE);
	u16 gen0 = chrome9_snoop_next_gen(t);
	u16 gen1 = chrome9_snoop_next_gen(t);
	struct chrome9_snoop_store s = {
		.page = page, .mask = mask, .read = true,
	};
	int ret;

	chrome9_snoop_fill(page, gen0, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_flush(page);
	chrome9_snoop_fill(src, gen1, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_flush(src);
	ret = chrome9_snoop_copy(t, test, SNOOP_SOURCE, 0, SNOOP_WORDS, &s);
	if (ret)
		return ret;
	chrome9_snoop_fill(t->exp, gen1, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_fill(t->alt, gen0, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_classify(page, t->exp, t->alt, cached);
	chrome9_snoop_flush(page);
	if (flushed)
		chrome9_snoop_classify(page, t->exp, t->alt, flushed);
	return 0;
}

/*
 * T3/T4: DRAM gen0, CPU-dirty gen2 in words [cpu_word, +cpu_words) of the
 * mask lines, GPU writes gen1 into words [x, +width) of every line.
 */
static int chrome9_snoop_t3(struct chrome9_snoop *t,
			    const unsigned long *mask, unsigned int cpu_word,
			    unsigned int cpu_words, u32 x, u32 width,
			    struct chrome9_snoop_count *cached,
			    struct chrome9_snoop_count *flushed)
{
	u32 *page = chrome9_snoop_role(t, SNOOP_TEST_S);
	u32 *src = chrome9_snoop_role(t, SNOOP_SOURCE);
	u16 gen0 = chrome9_snoop_next_gen(t);
	u16 gen1 = chrome9_snoop_next_gen(t);
	u16 gen2 = chrome9_snoop_next_gen(t);
	unsigned int lo = max(cpu_word, x);
	unsigned int hi = min(cpu_word + cpu_words, x + width);
	struct chrome9_snoop_store s = {
		.page = page, .gen = gen2, .mask = mask, .word = cpu_word,
		.words = cpu_words,
	};
	int ret;

	chrome9_snoop_fill(page, gen0, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_flush(page);
	chrome9_snoop_fill(src, gen1, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_flush(src);
	ret = chrome9_snoop_copy(t, SNOOP_TEST_S, SNOOP_SOURCE, x, width, &s);
	if (ret)
		return ret;
	/* Expected: CPU words, GPU words on top.  Stale: a lost write, or
	 * a dirty CPU line that survived where both wrote. */
	chrome9_snoop_fill(t->exp, gen0, NULL, 0, SNOOP_WORDS);
	chrome9_snoop_fill(t->exp, gen2, mask, cpu_word, cpu_words);
	chrome9_snoop_fill(t->exp, gen1, NULL, x, width);
	chrome9_snoop_fill(t->alt, gen0, NULL, 0, SNOOP_WORDS);
	if (lo < hi)
		chrome9_snoop_fill(t->alt, gen2, mask, lo, hi - lo);
	chrome9_snoop_classify(page, t->exp, t->alt, cached);
	chrome9_snoop_flush(page);
	chrome9_snoop_classify(page, t->exp, t->alt, flushed);
	return 0;
}

static int chrome9_snoop_load_fn(void *arg)
{
	u8 *buf = arg;

	while (!kthread_should_stop()) {
		memcpy(buf, buf + SNOOP_LOAD_BYTES / 2, SNOOP_LOAD_BYTES / 2);
		memcpy(buf + SNOOP_LOAD_BYTES / 2, buf, SNOOP_LOAD_BYTES / 2);
		cond_resched();
	}
	return 0;
}

struct chrome9_snoop_load {
	struct task_struct **task;
	u8 **buf;
	unsigned int count;
};

static void chrome9_snoop_load_stop(struct chrome9_snoop_load *load)
{
	unsigned int i;

	for (i = 0; i < load->count; i++) {
		if (load->task[i])
			kthread_stop(load->task[i]);
		vfree(load->buf[i]);
	}
	kfree(load->task);
	kfree(load->buf);
	memset(load, 0, sizeof(*load));
}

/* One memcpy streamer on every online CPU except the test CPU. */
static void chrome9_snoop_load_start(struct chrome9_snoop_load *load,
				     unsigned int test_cpu)
{
	unsigned int cpu;
	unsigned int n = num_online_cpus();

	load->task = kcalloc(n, sizeof(*load->task), GFP_KERNEL);
	load->buf = kcalloc(n, sizeof(*load->buf), GFP_KERNEL);
	if (!load->task || !load->buf) {
		chrome9_snoop_load_stop(load);
		return;
	}
	for_each_online_cpu(cpu) {
		struct task_struct *task;
		u8 *buf;

		if (cpu == test_cpu || load->count >= n)
			continue;
		buf = vzalloc(SNOOP_LOAD_BYTES);
		if (!buf)
			break;
		task = kthread_create(chrome9_snoop_load_fn, buf,
				      "chrome9-snoop/%u", cpu);
		if (IS_ERR(task)) {
			vfree(buf);
			break;
		}
		kthread_bind(task, cpu);
		load->task[load->count] = task;
		load->buf[load->count++] = buf;
		wake_up_process(task);
	}
}

struct chrome9_snoop_soak {
	u32 done;
	u32 lines;
	u32 stale[3];
	u32 load_cpus;
	s64 ms;
};

/* T5: T1-S, T2-S, T3-S in turn over random line subsets under load. */
static void chrome9_snoop_t5(struct chrome9_snoop *t,
			     struct chrome9_snoop_soak *soak)
{
	static const char * const names[] = { "T5/T1-S", "T5/T2-S", "T5/T3-S" };
	DECLARE_BITMAP(mask, SNOOP_LINES);
	struct chrome9_snoop_load load = {};
	struct chrome9_snoop_count c, f;
	struct chrome9_snoop_store s;
	ktime_t start = ktime_get();
	ktime_t end = ktime_add_ms(start, chrome9_selftest_snoop_soak_ms);
	unsigned int test_cpu = smp_processor_id();
	u32 i;

	memset(soak, 0, sizeof(*soak));
	chrome9_snoop_load_start(&load, test_cpu);
	soak->load_cpus = load.count;
	for (i = 0; i < chrome9_selftest_snoop_soak; i++) {
		unsigned int kind = i % 3;
		bool bad;
		int ret;

		if (ktime_after(ktime_get(), end))
			break;
		get_random_bytes(mask, sizeof(mask));
		if (bitmap_empty(mask, SNOOP_LINES))
			__set_bit(0, mask);
		/* The test CPU streams too, between steps. */
		if (t->load && i % SNOOP_STREAM_EVERY == 0) {
			memcpy(t->load, t->load + SNOOP_LOAD_BYTES / 2,
			       SNOOP_LOAD_BYTES / 2);
			cond_resched();
		}
		if (kind == 0)
			ret = chrome9_snoop_t1(t, SNOOP_TEST_S, false, mask, &c,
					       &s);
		else if (kind == 1)
			ret = chrome9_snoop_t2(t, SNOOP_TEST_S, mask, &c, NULL);
		else
			ret = chrome9_snoop_t3(t, mask, 0, SNOOP_WORDS, 0,
					       SNOOP_WORDS, &c, &f);
		if (ret)
			break;
		bad = chrome9_snoop_corrupt(t, names[kind], &c) ||
		      (kind == 2 && chrome9_snoop_corrupt(t, names[kind], &f));
		soak->done++;
		soak->lines += SNOOP_LINES;
		soak->stale[kind] += c.stale + (kind == 2 ? f.stale : 0);
		if (bad)
			break;
		if ((i + 1) % SNOOP_CHECK_EVERY == 0 &&
		    !chrome9_snoop_check(t, "T5"))
			break;
	}
	chrome9_snoop_load_stop(&load);
	soak->ms = ktime_ms_delta(ktime_get(), start);
	if (!t->abort)
		chrome9_snoop_check(t, "T5");
}

static int chrome9_snoop_setup(struct chrome9_snoop *t)
{
	struct device *dev = t->dev;
	phys_addr_t phys;
	u32 *ptes;
	unsigned int p;
	int ret;

	for (p = 0; p < SNOOP_PAGES; p++) {
		t->page[p] = alloc_page(GFP_KERNEL | GFP_DMA32 | __GFP_ZERO);
		if (!t->page[p])
			return -ENOMEM;
		t->dma[p] = dma_map_page(dev, t->page[p], 0, PAGE_SIZE,
					 DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev, t->dma[p]))
			return -ENOMEM;
		t->mapped++;
		phys = page_to_phys(t->page[p]);
		/* A bounce slot or IOMMU alias would hide the real page. */
		if (t->dma[p] != phys || upper_32_bits(t->dma[p])) {
			dev_err(dev, "snoop: page %u dma %pad phys %pa: SWIOTLB/IOMMU or above 4 GiB\n",
				p, &t->dma[p], &phys);
			t->abort = "swiotlb";
			return -EFAULT;
		}
		t->cpu[p] = page_address(t->page[p]);
		if (chrome9_snoop_is_guard(p)) {
			unsigned int i;

			for (i = 0; i < SNOOP_PAGE_WORDS; i++)
				t->cpu[p][i] = chrome9_snoop_guard_word(p, i);
		}
		chrome9_snoop_flush(t->cpu[p]);
	}
	ptes = kcalloc(SNOOP_PAGES, sizeof(*ptes), GFP_KERNEL);
	if (!ptes)
		return -ENOMEM;
	for (p = 0; p < SNOOP_PAGES; p++)
		ptes[p] = (lower_32_bits(t->dma[p] >> PAGE_SHIFT) & 0x3fffffff) |
			  (p / 3 == SNOOP_TEST_S ? SNOOP_PTE_SNOOP : 0);
	ret = chrome9_snoop_map(t->cdev, ptes, SNOOP_PAGES, &t->node);
	kfree(ptes);
	if (ret) {
		t->abort = "gart-map";
		t->retain = drm_mm_node_allocated(&t->node);
		return ret;
	}
	t->hash = chrome9_snoop_gart_hash(t->cdev);
	return 0;
}

static void chrome9_snoop_teardown(struct chrome9_snoop *t)
{
	unsigned int p;

	/* Unmapping fails on a wedged engine: its PTEs then stay live. */
	if (drm_mm_node_allocated(&t->node) &&
	    chrome9_snoop_unmap(t->cdev, &t->node))
		t->retain = true;
	if (t->retain) {
		dev_crit(t->dev,
			 "snoop: %u scratch pages retained after abort (%s)\n",
			 SNOOP_PAGES, t->abort ?: "unmap");
		return;
	}
	for (p = 0; p < SNOOP_PAGES && t->page[p]; p++) {
		if (p < t->mapped)
			dma_unmap_page(t->dev, t->dma[p], PAGE_SIZE,
				       DMA_BIDIRECTIONAL);
		__free_page(t->page[p]);
	}
}

static const char *chrome9_snoop_pass(bool pass)
{
	return pass ? "pass" : "FAIL";
}

static void chrome9_snoop_run(struct chrome9_snoop *t)
{
	struct device *dev = t->dev;
	struct chrome9_snoop_count c, f;
	struct chrome9_snoop_store s;
	struct chrome9_snoop_soak soak = {};
	bool t1p = false, t2p = false, t1s = false, t1x = false, t1x_run = false;
	bool t2s = false, t3s = false, t4s = false, t5 = false, t5_run = false;
	DECLARE_BITMAP(cpu_half, SNOOP_LINES);
	unsigned int attempt;
	char failed[64] = "";

	/* T1-P: the detector must see stale lines through a plain PTE. */
	for (attempt = 1; attempt <= SNOOP_T1P_ATTEMPTS; attempt++) {
		if (chrome9_snoop_t1(t, SNOOP_TEST_P, false, NULL, &c, &s) ||
		    chrome9_snoop_corrupt(t, "T1-P", &c))
			return;
		t1p = c.stale > 0;
		if (t1p)
			break;
	}
	dev_info(dev, "snoop T1-P cpu-write/gpu-read plain cpu=%d attempts=%u: fresh=%u stale=%u corrupt=%u %s\n",
		 s.cpu, min(attempt, SNOOP_T1P_ATTEMPTS), c.fresh, c.stale,
		 c.corrupt, t1p ? "detector ok" : "DETECTOR BLIND");
	if (!chrome9_snoop_check(t, "T1-P"))
		return;

	if (chrome9_snoop_t1(t, SNOOP_TEST_S, false, NULL, &c, &s) ||
	    chrome9_snoop_corrupt(t, "T1-S", &c))
		return;
	t1s = chrome9_snoop_all_fresh(&c);
	dev_info(dev, "snoop T1-S cpu-write/gpu-read bit25 cpu=%d: fresh=%u stale=%u corrupt=%u %s\n",
		 s.cpu, c.fresh, c.stale, c.corrupt, chrome9_snoop_pass(t1s));
	if (!chrome9_snoop_check(t, "T1-S"))
		return;

	if (num_online_cpus() > 1) {
		if (chrome9_snoop_t1(t, SNOOP_TEST_S, true, NULL, &c, &s) ||
		    chrome9_snoop_corrupt(t, "T1-X", &c))
			return;
		t1x_run = s.remote_cpu >= 0 && s.remote_cpu != s.cpu;
		t1x = t1x_run && chrome9_snoop_all_fresh(&c);
		dev_info(dev, "snoop T1-X cross-core bit25 cpu=%d store-cpu=%d: fresh=%u stale=%u corrupt=%u %s\n",
			 s.cpu, s.remote_cpu, c.fresh, c.stale, c.corrupt,
			 t1x_run ? chrome9_snoop_pass(t1x) : "SKIPPED");
		if (!chrome9_snoop_check(t, "T1-X"))
			return;
	} else {
		dev_info(dev, "snoop T1-X skipped: one online CPU\n");
	}

	/* T2-P: clean stale lines stay visible until CLFLUSH. */
	if (chrome9_snoop_t2(t, SNOOP_TEST_P, NULL, &c, &f) ||
	    chrome9_snoop_corrupt(t, "T2-P", &c) ||
	    chrome9_snoop_corrupt(t, "T2-P", &f))
		return;
	t2p = c.stale > 0 && chrome9_snoop_all_fresh(&f);
	dev_info(dev, "snoop T2-P gpu-write/cpu-read plain: cached fresh=%u stale=%u corrupt=%u, flushed fresh=%u stale=%u corrupt=%u %s\n",
		 c.fresh, c.stale, c.corrupt, f.fresh, f.stale, f.corrupt,
		 t2p ? "detector ok" : "DETECTOR BLIND");
	if (!chrome9_snoop_check(t, "T2-P"))
		return;

	if (chrome9_snoop_t2(t, SNOOP_TEST_S, NULL, &c, &f) ||
	    chrome9_snoop_corrupt(t, "T2-S", &c) ||
	    chrome9_snoop_corrupt(t, "T2-S", &f))
		return;
	t2s = chrome9_snoop_all_fresh(&c) && chrome9_snoop_all_fresh(&f);
	dev_info(dev, "snoop T2-S gpu-write/cpu-read bit25: cached fresh=%u stale=%u corrupt=%u, flushed fresh=%u stale=%u corrupt=%u %s\n",
		 c.fresh, c.stale, c.corrupt, f.fresh, f.stale, f.corrupt,
		 chrome9_snoop_pass(t2s));
	if (!chrome9_snoop_check(t, "T2-S"))
		return;

	if (chrome9_snoop_t3(t, NULL, 0, SNOOP_WORDS, 0, SNOOP_WORDS, &c, &f) ||
	    chrome9_snoop_corrupt(t, "T3-S", &c) ||
	    chrome9_snoop_corrupt(t, "T3-S", &f))
		return;
	t3s = chrome9_snoop_all_fresh(&c) && chrome9_snoop_all_fresh(&f);
	dev_info(dev, "snoop T3-S gpu-write over cpu-dirty bit25: cached fresh=%u stale=%u corrupt=%u, flushed fresh=%u stale=%u corrupt=%u %s\n",
		 c.fresh, c.stale, c.corrupt, f.fresh, f.stale, f.corrupt,
		 chrome9_snoop_pass(t3s));
	if (!chrome9_snoop_check(t, "T3-S"))
		return;

	/* T4-S: CPU dirties bytes 0..31, the GPU writes bytes 32..35. */
	bitmap_fill(cpu_half, SNOOP_LINES);
	if (chrome9_snoop_t3(t, cpu_half, 0, SNOOP_WORDS / 2, SNOOP_WORDS / 2,
			     1, &c, &f) ||
	    chrome9_snoop_corrupt(t, "T4-S", &c) ||
	    chrome9_snoop_corrupt(t, "T4-S", &f))
		return;
	t4s = chrome9_snoop_all_fresh(&c) && chrome9_snoop_all_fresh(&f);
	dev_info(dev, "snoop T4-S partial-line merge bit25: cached fresh=%u stale=%u corrupt=%u, flushed fresh=%u stale=%u corrupt=%u %s\n",
		 c.fresh, c.stale, c.corrupt, f.fresh, f.stale, f.corrupt,
		 chrome9_snoop_pass(t4s));
	if (!chrome9_snoop_check(t, "T4-S"))
		return;

	if (chrome9_selftest_snoop_soak) {
		t5_run = true;
		chrome9_snoop_t5(t, &soak);
		t5 = !t->abort && soak.done &&
		     !soak.stale[0] && !soak.stale[1] && !soak.stale[2];
		dev_info(dev, "snoop T5 soak bit25: iterations=%u/%u in %lld ms, load-cpus=%u, lines=%u stale T1=%u T2=%u T3=%u%s %s\n",
			 soak.done, chrome9_selftest_snoop_soak, soak.ms,
			 soak.load_cpus, soak.lines, soak.stale[0],
			 soak.stale[1], soak.stale[2],
			 soak.done < chrome9_selftest_snoop_soak ?
			 " (time-bounded)" : "",
			 t->abort ? "ABORT" : chrome9_snoop_pass(t5));
		if (t->abort)
			return;
	}

	snprintf(failed, sizeof(failed), "%s%s%s%s%s%s",
		 t1s ? "" : " T1-S", !t1x_run || t1x ? "" : " T1-X",
		 t2s ? "" : " T2-S", t3s ? "" : " T3-S", t4s ? "" : " T4-S",
		 !t5_run || t5 ? "" : " T5");
	if (!t1p || !t2p)
		dev_info(dev, "snoop verdict: INCONCLUSIVE (detector blind:%s%s; failed:%s)\n",
			 t1p ? "" : " T1-P", t2p ? "" : " T2-P",
			 failed[0] ? failed : " none");
	else if (!failed[0])
		dev_info(dev, "snoop verdict: COHERENT (T1-S%s T2-S T3-S T4-S%s pass)\n",
			 t1x_run ? " T1-X" : "", t5_run ? " T5" : "");
	else if (t1s && (!t1x_run || t1x) && (!t2s || !t3s))
		dev_info(dev, "snoop verdict: READ-ONLY (GPU reads snoop; failed:%s)\n",
			 failed);
	else
		dev_info(dev, "snoop verdict: NOT COHERENT (failed:%s)\n",
			 failed);
}

/* Runs bound to one CPU so the CPU steps happen on a known core. */
static int chrome9_snoop_thread(void *arg)
{
	struct chrome9_snoop *t = arg;
	int ret;

	ret = chrome9_snoop_begin(t->cdev);
	if (ret) {
		dev_err(t->dev, "snoop: engine not available: %d\n", ret);
		goto out;
	}
	ret = chrome9_snoop_setup(t);
	if (!ret) {
		dev_info(t->dev, "snoop ladder: %u pages SF %#llx..%#llx, dma32 direct, test cpu %d of %u online\n",
			 SNOOP_PAGES, t->node.start,
			 t->node.start + t->node.size - 1, smp_processor_id(),
			 num_online_cpus());
		chrome9_snoop_run(t);
	}
	if (t->abort)
		dev_err(t->dev, "snoop verdict: ABORT (%s)\n", t->abort);
	else if (ret)
		dev_err(t->dev, "snoop verdict: ABORT (setup %d)\n", ret);
	chrome9_snoop_teardown(t);
	chrome9_snoop_end(t->cdev);
out:
	complete(&t->done);
	return 0;
}

void chrome9_snoop_selftest(struct chrome9_device *cdev)
{
	struct task_struct *task;
	struct chrome9_snoop *t;

	if (!chrome9_selftest_snoop)
		return;
	if (chrome9_snoop_classify_selftest()) {
		dev_err(&cdev->pdev->dev, "snoop classifier self-test failed\n");
		return;
	}
	t = kzalloc_obj(*t);
	if (!t)
		return;
	t->cdev = cdev;
	t->dev = &cdev->pdev->dev;
	init_completion(&t->done);
	t->exp = kmalloc(PAGE_SIZE, GFP_KERNEL);
	t->alt = kmalloc(PAGE_SIZE, GFP_KERNEL);
	t->load = vzalloc(SNOOP_LOAD_BYTES);
	if (!t->exp || !t->alt)
		goto out_free;
	task = kthread_create(chrome9_snoop_thread, t, "chrome9-snoop");
	if (IS_ERR(task))
		goto out_free;
	kthread_bind(task, cpumask_first(cpu_online_mask));
	wake_up_process(task);
	wait_for_completion(&t->done);
out_free:
	vfree(t->load);
	kfree(t->alt);
	kfree(t->exp);
	kfree(t);
}
