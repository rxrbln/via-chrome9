/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __CHROME9_PROGCACHE_H__
#define __CHROME9_PROGCACHE_H__

#include <linux/types.h>

struct chrome9_progcache;
struct chrome9_program_entry;

/* A compiled program is identified by all inputs that determine it. */
struct chrome9_program_key {
	const void *params;
	size_t params_size;
	const void *code;
	size_t code_size;
};

/* Per-ioctl bump allocator; memory is not zeroed and lives until fini. */
struct chrome9_arena {
	struct chrome9_progcache *owner;
	struct chrome9_arena_chunk *head;
};

struct chrome9_progcache *chrome9_progcache_create(void);
void chrome9_progcache_destroy(struct chrome9_progcache *cache);
const void *chrome9_progcache_get(struct chrome9_progcache *cache,
				  const struct chrome9_program_key *key,
				  struct chrome9_program_entry **entry);
const void *chrome9_progcache_add(struct chrome9_progcache *cache,
				  const struct chrome9_program_key *key,
				  const void *value, size_t value_size,
				  struct chrome9_program_entry **entry);
void chrome9_program_entry_put(struct chrome9_program_entry *entry);

void chrome9_arena_init(struct chrome9_arena *arena,
			struct chrome9_progcache *owner);
void *chrome9_arena_alloc(struct chrome9_arena *arena, size_t size);
void *chrome9_arena_copy_user(struct chrome9_arena *arena, u64 user,
			      size_t count, size_t size);
void chrome9_arena_fini(struct chrome9_arena *arena);

#endif
