// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include <linux/align.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/hash.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/random.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/xxhash.h>

#include "chrome9_progcache.h"

#define CHROME9_PROGCACHE_MIN_BITS	4U
#define CHROME9_PROGCACHE_MAX_BITS	16U
#define CHROME9_ARENA_MIN		(64U * 1024)
/* A larger chunk is freed instead of being kept for the next ioctl. */
#define CHROME9_ARENA_KEEP		(512U * 1024)

static uint chrome9_program_cache_kib = 512;
module_param_named(program_cache_kib, chrome9_program_cache_kib, uint, 0600);
MODULE_PARM_DESC(program_cache_kib,
	"Compiled program cache per DRM file in KiB, 0 disables (default: 512)");

struct chrome9_program_entry {
	struct hlist_node node;
	struct list_head lru;
	refcount_t ref;
	u64 hash;
	size_t params_size;
	size_t code_size;
	size_t value_size;
	size_t bytes;
	/* Value, then params and code of the key. */
	u8 data[] __aligned(8);
};

struct chrome9_arena_chunk {
	struct chrome9_arena_chunk *next;
	size_t size;
	size_t used;
	u8 data[] __aligned(8);
};

struct chrome9_progcache {
	struct mutex lock;
	/* Random per file: lookups of one file cannot be steered by another. */
	u64 seed;
	struct hlist_head *buckets;
	unsigned int bits;
	u32 count;
	size_t bytes;
	/* Most recently used first. */
	struct list_head lru;
	struct chrome9_arena_chunk *spare;
};

static void chrome9_arena_chunk_free(struct chrome9_arena_chunk *chunk)
{
	kvfree(chunk);
}

struct chrome9_progcache *chrome9_progcache_create(void)
{
	struct chrome9_progcache *cache = kzalloc_obj(*cache);

	if (!cache)
		return NULL;
	cache->bits = CHROME9_PROGCACHE_MIN_BITS;
	cache->buckets = kcalloc(1U << cache->bits, sizeof(*cache->buckets),
				 GFP_KERNEL);
	if (!cache->buckets) {
		kfree(cache);
		return NULL;
	}
	mutex_init(&cache->lock);
	cache->seed = get_random_u64();
	INIT_LIST_HEAD(&cache->lru);
	return cache;
}

void chrome9_program_entry_put(struct chrome9_program_entry *entry)
{
	if (entry && refcount_dec_and_test(&entry->ref))
		kvfree(entry);
}

static void chrome9_progcache_remove(struct chrome9_progcache *cache,
				     struct chrome9_program_entry *entry)
{
	hlist_del(&entry->node);
	list_del(&entry->lru);
	cache->count--;
	cache->bytes -= entry->bytes;
	chrome9_program_entry_put(entry);
}

void chrome9_progcache_destroy(struct chrome9_progcache *cache)
{
	struct chrome9_program_entry *entry, *next;

	if (!cache)
		return;
	list_for_each_entry_safe(entry, next, &cache->lru, lru)
		chrome9_progcache_remove(cache, entry);
	chrome9_arena_chunk_free(cache->spare);
	kfree(cache->buckets);
	mutex_destroy(&cache->lock);
	kfree(cache);
}

static u64 chrome9_progcache_hash(const struct chrome9_progcache *cache,
				  const struct chrome9_program_key *key)
{
	return xxh64(key->code, key->code_size,
		     xxh64(key->params, key->params_size, cache->seed));
}

static struct chrome9_program_entry *
chrome9_progcache_find(struct chrome9_progcache *cache,
		       const struct chrome9_program_key *key, u64 hash)
{
	struct chrome9_program_entry *entry;

	hlist_for_each_entry(entry, &cache->buckets[hash_64(hash, cache->bits)],
			     node) {
		const u8 *params = entry->data + ALIGN(entry->value_size, 8);

		if (entry->hash == hash &&
		    entry->params_size == key->params_size &&
		    entry->code_size == key->code_size &&
		    !memcmp(params, key->params, key->params_size) &&
		    !memcmp(params + key->params_size, key->code,
			    key->code_size)) {
			list_move(&entry->lru, &cache->lru);
			refcount_inc(&entry->ref);
			return entry;
		}
	}
	return NULL;
}

const void *chrome9_progcache_get(struct chrome9_progcache *cache,
				  const struct chrome9_program_key *key,
				  struct chrome9_program_entry **entry)
{
	u64 hash;

	*entry = NULL;
	if (!cache || !READ_ONCE(chrome9_program_cache_kib))
		return NULL;
	hash = chrome9_progcache_hash(cache, key);
	mutex_lock(&cache->lock);
	*entry = chrome9_progcache_find(cache, key, hash);
	mutex_unlock(&cache->lock);
	return *entry ? (*entry)->data : NULL;
}

/* Keep about one entry per bucket; a failed resize keeps the old table. */
static void chrome9_progcache_grow(struct chrome9_progcache *cache)
{
	struct chrome9_program_entry *entry;
	struct hlist_head *buckets;
	unsigned int bits = cache->bits + 1;

	if (cache->count <= 1U << cache->bits ||
	    bits > CHROME9_PROGCACHE_MAX_BITS)
		return;
	buckets = kcalloc(1U << bits, sizeof(*buckets), GFP_KERNEL);
	if (!buckets)
		return;
	list_for_each_entry(entry, &cache->lru, lru) {
		hlist_del(&entry->node);
		hlist_add_head(&entry->node, &buckets[hash_64(entry->hash, bits)]);
	}
	kfree(cache->buckets);
	cache->buckets = buckets;
	cache->bits = bits;
}

/*
 * Copy a compiled program into the cache and return the cached copy with
 * a reference, or NULL when caching is off or fails (the caller keeps
 * using its own copy).  Least recently used entries beyond the budget are
 * dropped; draws still holding one keep it alive.
 */
const void *chrome9_progcache_add(struct chrome9_progcache *cache,
				  const struct chrome9_program_key *key,
				  const void *value, size_t value_size,
				  struct chrome9_program_entry **entry)
{
	size_t limit = (size_t)READ_ONCE(chrome9_program_cache_kib) * 1024;
	struct chrome9_program_entry *new, *old;
	size_t bytes;
	u8 *params;

	*entry = NULL;
	if (!cache || !limit)
		return NULL;
	bytes = size_add(size_add(struct_size(new, data,
					      ALIGN(value_size, 8)),
				  key->params_size), key->code_size);
	if (bytes > limit / 4)
		return NULL;
	new = kvmalloc(bytes, GFP_KERNEL);
	if (!new)
		return NULL;
	new->hash = chrome9_progcache_hash(cache, key);
	new->params_size = key->params_size;
	new->code_size = key->code_size;
	new->value_size = value_size;
	new->bytes = bytes;
	refcount_set(&new->ref, 2);
	memcpy(new->data, value, value_size);
	params = new->data + ALIGN(value_size, 8);
	memcpy(params, key->params, key->params_size);
	memcpy(params + key->params_size, key->code, key->code_size);

	mutex_lock(&cache->lock);
	/* A concurrent ioctl of this file may have added it meanwhile. */
	old = chrome9_progcache_find(cache, key, new->hash);
	if (old) {
		mutex_unlock(&cache->lock);
		kvfree(new);
		*entry = old;
		return old->data;
	}
	hlist_add_head(&new->node,
		       &cache->buckets[hash_64(new->hash, cache->bits)]);
	list_add(&new->lru, &cache->lru);
	cache->count++;
	cache->bytes += bytes;
	while (cache->bytes > limit) {
		old = list_last_entry(&cache->lru, struct chrome9_program_entry,
				      lru);
		chrome9_progcache_remove(cache, old);
	}
	chrome9_progcache_grow(cache);
	mutex_unlock(&cache->lock);
	*entry = new;
	return new->data;
}

void chrome9_arena_init(struct chrome9_arena *arena,
			struct chrome9_progcache *owner)
{
	arena->owner = owner;
	arena->head = NULL;
}

void *chrome9_arena_alloc(struct chrome9_arena *arena, size_t size)
{
	struct chrome9_arena_chunk *chunk = arena->head;
	size_t bytes;
	void *p;

	size = ALIGN(size, 8);
	if (!chunk && arena->owner) {
		chunk = xchg(&arena->owner->spare, NULL);
		if (chunk && chunk->size < size) {
			chrome9_arena_chunk_free(chunk);
			chunk = NULL;
		}
		if (chunk) {
			chunk->next = NULL;
			chunk->used = 0;
			arena->head = chunk;
		}
	}
	if (!chunk || chunk->size - chunk->used < size) {
		bytes = max_t(size_t, CHROME9_ARENA_MIN,
			      chunk ? 2 * struct_size(chunk, data, chunk->size) :
				      0);
		bytes = max(bytes, struct_size(chunk, data, size));
		if (bytes == SIZE_MAX)
			return NULL;
		chunk = kvmalloc(bytes, GFP_KERNEL);
		if (!chunk)
			return NULL;
		chunk->size = bytes - sizeof(*chunk);
		chunk->used = 0;
		chunk->next = arena->head;
		arena->head = chunk;
	}
	p = chunk->data + chunk->used;
	chunk->used += size;
	return p;
}

/* memdup_array_user() into the arena. */
void *chrome9_arena_copy_user(struct chrome9_arena *arena, u64 user,
			      size_t count, size_t size)
{
	size_t bytes;
	void *p;

	if (check_mul_overflow(count, size, &bytes))
		return ERR_PTR(-EOVERFLOW);
	p = chrome9_arena_alloc(arena, bytes);
	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(p, u64_to_user_ptr(user), bytes))
		return ERR_PTR(-EFAULT);
	return p;
}

/* The largest chunk that is not oversized serves the file's next ioctl. */
void chrome9_arena_fini(struct chrome9_arena *arena)
{
	struct chrome9_arena_chunk *chunk, *next, *keep = NULL;

	for (chunk = arena->head; chunk; chunk = chunk->next)
		if (chunk->size <= CHROME9_ARENA_KEEP &&
		    (!keep || chunk->size > keep->size))
			keep = chunk;
	if (!arena->owner || cmpxchg(&arena->owner->spare, NULL, keep))
		keep = NULL;
	for (chunk = arena->head; chunk; chunk = next) {
		next = chunk->next;
		if (chunk != keep)
			chrome9_arena_chunk_free(chunk);
	}
	arena->head = NULL;
}
