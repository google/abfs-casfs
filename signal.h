/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Google LLC
 */

#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <linux/signal.h>

extern void block_signals(sigset_t *oldset);

extern void unblock_signals(sigset_t *oldset);

#endif /* _SIGNAL_H */
