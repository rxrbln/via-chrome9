/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_BLIT_H
#define CHROME9_BLIT_H

struct pipe_context;
struct pipe_blit_info;

void chrome9_blit(struct pipe_context *pipe, const struct pipe_blit_info *info);

#endif
