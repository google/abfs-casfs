// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Google LLC
 */

#include "signal.h"

void block_signals(sigset_t *oldset)
{
	int rc;
	sigset_t blocked;

	sigfillset(&blocked);
	rc = sigprocmask(SIG_BLOCK, &blocked, oldset);
	BUG_ON(rc);
}

void unblock_signals(sigset_t *oldset)
{
	int rc = sigprocmask(SIG_SETMASK, oldset, NULL);

	BUG_ON(rc);
}
