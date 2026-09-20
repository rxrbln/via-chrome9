/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_PUBLIC_H
#define CHROME9_PUBLIC_H

#include <stdbool.h>
#include <stdint.h>

struct pipe_screen;
struct pipe_screen_config;

struct pipe_screen *
chrome9_screen_create(int fd, const struct pipe_screen_config *config);

bool chrome9_screen_has_capability(struct pipe_screen *screen,
                                   uint64_t capability);

#endif
