/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_PUBLIC_H
#define CHROME9_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;

struct pipe_screen *
chrome9_screen_create(int fd, const struct pipe_screen_config *config);

#endif
