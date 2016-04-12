#include <linux/percpu_pool.h>
#include <linux/cpu.h>

void percpu_pool_add_balance(struct percpu_pool *pool, u64 amount)
{
	u64 pool_size;
	int nr = num_online_cpus();
	int cpu;

	spin_lock(&pool->lock);
	pool_size = div64_u64(amount, nr);
	for_each_online_cpu(cpu) {
		u64 *p = per_cpu_ptr(pool->pools, cpu);
		*p = pool_size;
		amount -= pool_size;
	}

	/* add any remaining to the current cpu pool. */
	if (amount)
		this_cpu_add(*pool->pools, amount);
	spin_unlock(&pool->lock);
}
EXPORT_SYMBOL_GPL(percpu_pool_add_balance);

void percpu_pool_add(struct percpu_pool *pool, u64 amount)
{
	preempt_disable();
	this_cpu_add(*pool->pools, amount);
	preempt_enable();
}
EXPORT_SYMBOL_GPL(percpu_pool_add);

int percpu_pool_sub(struct percpu_pool *pool, u64 amount)
{
	u64 count;
	int ret = 0;

	preempt_disable();
	count = __this_cpu_read(*pool->pools);
	if (count < amount) {
		int cpu;

		spin_lock(&pool->lock);
		count = 0;
		for_each_online_cpu(cpu) {
			u64 *p = per_cpu_ptr(pool->pools, cpu);
			if (*p + count < amount) {
				count += *p;
				*p = 0;
			} else {
				*p -= amount - count;
				count = amount;
				break;
			}
		}
		/*
		 * We don't have enough space, add what we stole back to the
		 * current cpu's count.
		 */
		if (count != amount) {
			__this_cpu_add(*pool->pools, count);
			ret = -ENOSPC;
		}
		spin_unlock(&pool->lock);
	} else {
		this_cpu_sub(*pool->pools, amount);
	}
	preempt_enable();
	return ret;
}
EXPORT_SYMBOL_GPL(percpu_pool_sub);

int percpu_pool_init(struct percpu_pool *pool, u64 amount, gfp_t gfp)
{

	spin_lock_init(&pool->lock);
	pool->pools = alloc_percpu_gfp(u64, gfp);
	if (!pool->pools)
		return -ENOMEM;

	percpu_pool_add_balance(pool, amount);
	return 0;
}
EXPORT_SYMBOL_GPL(percpu_pool_init);
