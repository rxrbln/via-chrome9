/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __CHROME9_CBU_H__
#define __CHROME9_CBU_H__

#include <linux/types.h>

struct chrome9_device;

int chrome9_cbu_init(struct chrome9_device *cdev);

#endif
