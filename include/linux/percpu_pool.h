#ifndef _LINUX_PERCPU_POOL_H
#define _LINUX_PERCPU_POOL_H

#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/gfp.h>

struct percpu_pool {
	spinlock_t lock;
	u64 __percpu *pools;
};

void percpu_pool_add_balance(struct percpu_pool *pool, u64 amount);
void percpu_pool_add(struct percpu_pool *pool, u64 amount);
int percpu_pool_sub(struct percpu_pool *pool, u64 amount);
int percpu_pool_init(struct percpu_pool *pool, u64 amount, gfp_t gfp);

#endif
