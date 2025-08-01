// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/kernel/fork.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also entry.S and others).
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/memory.c': 'copy_page_range()'
 */

#include <linux/anon_inodes.h>
#include <linux/slab.h>
#include <linux/sched/autogroup.h>
#include <linux/sched/mm.h>
#include <linux/sched/user.h>
#include <linux/sched/numa_balancing.h>
#include <linux/sched/stat.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/cputime.h>
#include <linux/sched/ext.h>
#include <linux/seq_file.h>
#include <linux/rtmutex.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/mempolicy.h>
#include <linux/sem.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/iocontext.h>
#include <linux/key.h>
#include <linux/kmsan.h>
#include <linux/binfmts.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/memblock.h>
#include <linux/nsproxy.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cgroup.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
#include <linux/seccomp.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/syscall_user_dispatch.h>
#include <linux/jiffies.h>
#include <linux/futex.h>
#include <linux/compat.h>
#include <linux/kthread.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/rcupdate.h>
#include <linux/ptrace.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/memcontrol.h>
#include <linux/ftrace.h>
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/rmap.h>
#include <linux/ksm.h>
#include <linux/acct.h>
#include <linux/userfaultfd_k.h>
#include <linux/tsacct_kern.h>
#include <linux/cn_proc.h>
#include <linux/freezer.h>
#include <linux/delayacct.h>
#include <linux/taskstats_kern.h>
#include <linux/tty.h>
#include <linux/fs_struct.h>
#include <linux/magic.h>
#include <linux/perf_event.h>
#include <linux/posix-timers.h>
#include <linux/user-return-notifier.h>
#include <linux/oom.h>
#include <linux/khugepaged.h>
#include <linux/signalfd.h>
#include <linux/uprobes.h>
#include <linux/aio.h>
#include <linux/compiler.h>
#include <linux/sysctl.h>
#include <linux/kcov.h>
#include <linux/livepatch.h>
#include <linux/thread_info.h>
#include <linux/kstack_erase.h>
#include <linux/kasan.h>
#include <linux/scs.h>
#include <linux/io_uring.h>
#include <linux/bpf.h>
#include <linux/stackprotector.h>
#include <linux/user_events.h>
#include <linux/iommu.h>
#include <linux/rseq.h>
#include <uapi/linux/pidfd.h>
#include <linux/pidfs.h>
#include <linux/tick.h>

#include <asm/pgalloc.h>
#include <linux/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

/* For dup_mmap(). */
#include "../mm/internal.h"

#include <trace/events/sched.h>

#define CREATE_TRACE_POINTS
#include <trace/events/task.h>

#include <kunit/visibility.h>

/*
 * Minimum number of threads to boot the kernel
 */
#define MIN_THREADS 20

/*
 * Maximum number of threads
 */
#define MAX_THREADS FUTEX_TID_MASK

/*
 * Protected counters by write_lock_irq(&tasklist_lock)
 */
unsigned long total_forks;	/* Handle normal Linux uptimes. */
int nr_threads;			/* The idle threads do not count.. */

static int max_threads;		/* tunable limit on nr_threads */

#define NAMED_ARRAY_INDEX(x)	[x] = __stringify(x)

static const char * const resident_page_types[] = {
	NAMED_ARRAY_INDEX(MM_FILEPAGES),
	NAMED_ARRAY_INDEX(MM_ANONPAGES),
	NAMED_ARRAY_INDEX(MM_SWAPENTS),
	NAMED_ARRAY_INDEX(MM_SHMEMPAGES),
};

DEFINE_PER_CPU(unsigned long, process_counts) = 0;

__cacheline_aligned DEFINE_RWLOCK(tasklist_lock);  /* outer */

#ifdef CONFIG_PROVE_RCU
int lockdep_tasklist_lock_is_held(void)
{
	return lockdep_is_held(&tasklist_lock);
}
EXPORT_SYMBOL_GPL(lockdep_tasklist_lock_is_held);
#endif /* #ifdef CONFIG_PROVE_RCU */

int nr_processes(void)
{
	int cpu;
	int total = 0;

	for_each_possible_cpu(cpu)
		total += per_cpu(process_counts, cpu);

	return total;
}

void __weak arch_release_task_struct(struct task_struct *tsk)
{
}

static struct kmem_cache *task_struct_cachep;

static inline struct task_struct *alloc_task_struct_node(int node)
{
	return kmem_cache_alloc_node(task_struct_cachep, GFP_KERNEL, node);
}

static inline void free_task_struct(struct task_struct *tsk)
{
	kmem_cache_free(task_struct_cachep, tsk);
}

/*
 * Allocate pages if THREAD_SIZE is >= PAGE_SIZE, otherwise use a
 * kmemcache based allocator.
 */
# if THREAD_SIZE >= PAGE_SIZE || defined(CONFIG_VMAP_STACK)

#  ifdef CONFIG_VMAP_STACK
/*
 * vmalloc() is a bit slow, and calling vfree() enough times will force a TLB
 * flush.  Try to minimize the number of calls by caching stacks.
 */
#define NR_CACHED_STACKS 2
static DEFINE_PER_CPU(struct vm_struct *, cached_stacks[NR_CACHED_STACKS]);

struct vm_stack {
	struct rcu_head rcu;
	struct vm_struct *stack_vm_area;
};

static bool try_release_thread_stack_to_cache(struct vm_struct *vm)
{
	unsigned int i;

	for (i = 0; i < NR_CACHED_STACKS; i++) {
		struct vm_struct *tmp = NULL;

		if (this_cpu_try_cmpxchg(cached_stacks[i], &tmp, vm))
			return true;
	}
	return false;
}

static void thread_stack_free_rcu(struct rcu_head *rh)
{
	struct vm_stack *vm_stack = container_of(rh, struct vm_stack, rcu);

	if (try_release_thread_stack_to_cache(vm_stack->stack_vm_area))
		return;

	vfree(vm_stack);
}

static void thread_stack_delayed_free(struct task_struct *tsk)
{
	struct vm_stack *vm_stack = tsk->stack;

	vm_stack->stack_vm_area = tsk->stack_vm_area;
	call_rcu(&vm_stack->rcu, thread_stack_free_rcu);
}

static int free_vm_stack_cache(unsigned int cpu)
{
	struct vm_struct **cached_vm_stacks = per_cpu_ptr(cached_stacks, cpu);
	int i;

	for (i = 0; i < NR_CACHED_STACKS; i++) {
		struct vm_struct *vm_stack = cached_vm_stacks[i];

		if (!vm_stack)
			continue;

		vfree(vm_stack->addr);
		cached_vm_stacks[i] = NULL;
	}

	return 0;
}

static int memcg_charge_kernel_stack(struct vm_struct *vm)
{
	int i;
	int ret;
	int nr_charged = 0;

	BUG_ON(vm->nr_pages != THREAD_SIZE / PAGE_SIZE);

	for (i = 0; i < THREAD_SIZE / PAGE_SIZE; i++) {
		ret = memcg_kmem_charge_page(vm->pages[i], GFP_KERNEL, 0);
		if (ret)
			goto err;
		nr_charged++;
	}
	return 0;
err:
	for (i = 0; i < nr_charged; i++)
		memcg_kmem_uncharge_page(vm->pages[i], 0);
	return ret;
}

static int alloc_thread_stack_node(struct task_struct *tsk, int node)
{
	struct vm_struct *vm;
	void *stack;
	int i;

	for (i = 0; i < NR_CACHED_STACKS; i++) {
		struct vm_struct *s;

		s = this_cpu_xchg(cached_stacks[i], NULL);

		if (!s)
			continue;

		/* Reset stack metadata. */
		kasan_unpoison_range(s->addr, THREAD_SIZE);

		stack = kasan_reset_tag(s->addr);

		/* Clear stale pointers from reused stack. */
		memset(stack, 0, THREAD_SIZE);

		if (memcg_charge_kernel_stack(s)) {
			vfree(s->addr);
			return -ENOMEM;
		}

		tsk->stack_vm_area = s;
		tsk->stack = stack;
		return 0;
	}

	/*
	 * Allocated stacks are cached and later reused by new threads,
	 * so memcg accounting is performed manually on assigning/releasing
	 * stacks to tasks. Drop __GFP_ACCOUNT.
	 */
	stack = __vmalloc_node(THREAD_SIZE, THREAD_ALIGN,
				     THREADINFO_GFP & ~__GFP_ACCOUNT,
				     node, __builtin_return_address(0));
	if (!stack)
		return -ENOMEM;

	vm = find_vm_area(stack);
	if (memcg_charge_kernel_stack(vm)) {
		vfree(stack);
		return -ENOMEM;
	}
	/*
	 * We can't call find_vm_area() in interrupt context, and
	 * free_thread_stack() can be called in interrupt context,
	 * so cache the vm_struct.
	 */
	tsk->stack_vm_area = vm;
	stack = kasan_reset_tag(stack);
	tsk->stack = stack;
	return 0;
}

static void free_thread_stack(struct task_struct *tsk)
{
	if (!try_release_thread_stack_to_cache(tsk->stack_vm_area))
		thread_stack_delayed_free(tsk);

	tsk->stack = NULL;
	tsk->stack_vm_area = NULL;
}

#  else /* !CONFIG_VMAP_STACK */

static void thread_stack_free_rcu(struct rcu_head *rh)
{
	__free_pages(virt_to_page(rh), THREAD_SIZE_ORDER);
}

static void thread_stack_delayed_free(struct task_struct *tsk)
{
	struct rcu_head *rh = tsk->stack;

	call_rcu(rh, thread_stack_free_rcu);
}

static int alloc_thread_stack_node(struct task_struct *tsk, int node)
{
	struct page *page = alloc_pages_node(node, THREADINFO_GFP,
					     THREAD_SIZE_ORDER);

	if (likely(page)) {
		tsk->stack = kasan_reset_tag(page_address(page));
		return 0;
	}
	return -ENOMEM;
}

static void free_thread_stack(struct task_struct *tsk)
{
	thread_stack_delayed_free(tsk);
	tsk->stack = NULL;
}

#  endif /* CONFIG_VMAP_STACK */
# else /* !(THREAD_SIZE >= PAGE_SIZE || defined(CONFIG_VMAP_STACK)) */

static struct kmem_cache *thread_stack_cache;

static void thread_stack_free_rcu(struct rcu_head *rh)
{
	kmem_cache_free(thread_stack_cache, rh);
}

static void thread_stack_delayed_free(struct task_struct *tsk)
{
	struct rcu_head *rh = tsk->stack;

	call_rcu(rh, thread_stack_free_rcu);
}

static int alloc_thread_stack_node(struct task_struct *tsk, int node)
{
	unsigned long *stack;
	stack = kmem_cache_alloc_node(thread_stack_cache, THREADINFO_GFP, node);
	stack = kasan_reset_tag(stack);
	tsk->stack = stack;
	return stack ? 0 : -ENOMEM;
}

static void free_thread_stack(struct task_struct *tsk)
{
	thread_stack_delayed_free(tsk);
	tsk->stack = NULL;
}

void thread_stack_cache_init(void)
{
	thread_stack_cache = kmem_cache_create_usercopy("thread_stack",
					THREAD_SIZE, THREAD_SIZE, 0, 0,
					THREAD_SIZE, NULL);
	BUG_ON(thread_stack_cache == NULL);
}

# endif /* THREAD_SIZE >= PAGE_SIZE || defined(CONFIG_VMAP_STACK) */

/* SLAB cache for signal_struct structures (tsk->signal) */
static struct kmem_cache *signal_cachep;

/* SLAB cache for sighand_struct structures (tsk->sighand) */
struct kmem_cache *sighand_cachep;

/* SLAB cache for files_struct structures (tsk->files) */
struct kmem_cache *files_cachep;

/* SLAB cache for fs_struct structures (tsk->fs) */
struct kmem_cache *fs_cachep;

/* SLAB cache for mm_struct structures (tsk->mm) */
static struct kmem_cache *mm_cachep;

static void account_kernel_stack(struct task_struct *tsk, int account)
{
	if (IS_ENABLED(CONFIG_VMAP_STACK)) {
		struct vm_struct *vm = task_stack_vm_area(tsk);
		int i;

		for (i = 0; i < THREAD_SIZE / PAGE_SIZE; i++)
			mod_lruvec_page_state(vm->pages[i], NR_KERNEL_STACK_KB,
					      account * (PAGE_SIZE / 1024));
	} else {
		void *stack = task_stack_page(tsk);

		/* All stack pages are in the same node. */
		mod_lruvec_kmem_state(stack, NR_KERNEL_STACK_KB,
				      account * (THREAD_SIZE / 1024));
	}
}

void exit_task_stack_account(struct task_struct *tsk)
{
	account_kernel_stack(tsk, -1);

	if (IS_ENABLED(CONFIG_VMAP_STACK)) {
		struct vm_struct *vm;
		int i;

		vm = task_stack_vm_area(tsk);
		for (i = 0; i < THREAD_SIZE / PAGE_SIZE; i++)
			memcg_kmem_uncharge_page(vm->pages[i], 0);
	}
}

static void release_task_stack(struct task_struct *tsk)
{
	if (WARN_ON(READ_ONCE(tsk->__state) != TASK_DEAD))
		return;  /* Better to leak the stack than to free prematurely */

	free_thread_stack(tsk);
}

#ifdef CONFIG_THREAD_INFO_IN_TASK
void put_task_stack(struct task_struct *tsk)
{
	if (refcount_dec_and_test(&tsk->stack_refcount))
		release_task_stack(tsk);
}
#endif

void free_task(struct task_struct *tsk)
{
#ifdef CONFIG_SECCOMP
	WARN_ON_ONCE(tsk->seccomp.filter);
#endif
	release_user_cpus_ptr(tsk);
	scs_release(tsk);

#ifndef CONFIG_THREAD_INFO_IN_TASK
	/*
	 * The task is finally done with both the stack and thread_info,
	 * so free both.
	 */
	release_task_stack(tsk);
#else
	/*
	 * If the task had a separate stack allocation, it should be gone
	 * by now.
	 */
	WARN_ON_ONCE(refcount_read(&tsk->stack_refcount) != 0);
#endif
	rt_mutex_debug_task_free(tsk);
	ftrace_graph_exit_task(tsk);
	arch_release_task_struct(tsk);
	if (tsk->flags & PF_KTHREAD)
		free_kthread_struct(tsk);
	bpf_task_storage_free(tsk);
	free_task_struct(tsk);
}
EXPORT_SYMBOL(free_task);

void dup_mm_exe_file(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct file *exe_file;

	exe_file = get_mm_exe_file(oldmm);
	RCU_INIT_POINTER(mm->exe_file, exe_file);
	/*
	 * We depend on the oldmm having properly denied write access to the
	 * exe_file already.
	 */
	if (exe_file && exe_file_deny_write_access(exe_file))
		pr_warn_once("exe_file_deny_write_access() failed in %s\n", __func__);
}

#ifdef CONFIG_MMU
static inline int mm_alloc_pgd(struct mm_struct *mm)
{
	mm->pgd = pgd_alloc(mm);
	if (unlikely(!mm->pgd))
		return -ENOMEM;
	return 0;
}

static inline void mm_free_pgd(struct mm_struct *mm)
{
	pgd_free(mm, mm->pgd);
}
#else
#define mm_alloc_pgd(mm)	(0)
#define mm_free_pgd(mm)
#endif /* CONFIG_MMU */

#ifdef CONFIG_MM_ID
static DEFINE_IDA(mm_ida);

static inline int mm_alloc_id(struct mm_struct *mm)
{
	int ret;

	ret = ida_alloc_range(&mm_ida, MM_ID_MIN, MM_ID_MAX, GFP_KERNEL);
	if (ret < 0)
		return ret;
	mm->mm_id = ret;
	return 0;
}

static inline void mm_free_id(struct mm_struct *mm)
{
	const mm_id_t id = mm->mm_id;

	mm->mm_id = MM_ID_DUMMY;
	if (id == MM_ID_DUMMY)
		return;
	if (WARN_ON_ONCE(id < MM_ID_MIN || id > MM_ID_MAX))
		return;
	ida_free(&mm_ida, id);
}
#else /* !CONFIG_MM_ID */
static inline int mm_alloc_id(struct mm_struct *mm) { return 0; }
static inline void mm_free_id(struct mm_struct *mm) {}
#endif /* CONFIG_MM_ID */

static void check_mm(struct mm_struct *mm)
{
	int i;

	BUILD_BUG_ON_MSG(ARRAY_SIZE(resident_page_types) != NR_MM_COUNTERS,
			 "Please make sure 'struct resident_page_types[]' is updated as well");

	for (i = 0; i < NR_MM_COUNTERS; i++) {
		long x = percpu_counter_sum(&mm->rss_stat[i]);

		if (unlikely(x))
			pr_alert("BUG: Bad rss-counter state mm:%p type:%s val:%ld\n",
				 mm, resident_page_types[i], x);
	}

	if (mm_pgtables_bytes(mm))
		pr_alert("BUG: non-zero pgtables_bytes on freeing mm: %ld\n",
				mm_pgtables_bytes(mm));

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !defined(CONFIG_SPLIT_PMD_PTLOCKS)
	VM_BUG_ON_MM(mm->pmd_huge_pte, mm);
#endif
}

#define allocate_mm()	(kmem_cache_alloc(mm_cachep, GFP_KERNEL))
#define free_mm(mm)	(kmem_cache_free(mm_cachep, (mm)))

static void do_check_lazy_tlb(void *arg)
{
	struct mm_struct *mm = arg;

	WARN_ON_ONCE(current->active_mm == mm);
}

static void do_shoot_lazy_tlb(void *arg)
{
	struct mm_struct *mm = arg;

	if (current->active_mm == mm) {
		WARN_ON_ONCE(current->mm);
		current->active_mm = &init_mm;
		switch_mm(mm, &init_mm, current);
	}
}

static void cleanup_lazy_tlbs(struct mm_struct *mm)
{
	if (!IS_ENABLED(CONFIG_MMU_LAZY_TLB_SHOOTDOWN)) {
		/*
		 * In this case, lazy tlb mms are refounted and would not reach
		 * __mmdrop until all CPUs have switched away and mmdrop()ed.
		 */
		return;
	}

	/*
	 * Lazy mm shootdown does not refcount "lazy tlb mm" usage, rather it
	 * requires lazy mm users to switch to another mm when the refcount
	 * drops to zero, before the mm is freed. This requires IPIs here to
	 * switch kernel threads to init_mm.
	 *
	 * archs that use IPIs to flush TLBs can piggy-back that lazy tlb mm
	 * switch with the final userspace teardown TLB flush which leaves the
	 * mm lazy on this CPU but no others, reducing the need for additional
	 * IPIs here. There are cases where a final IPI is still required here,
	 * such as the final mmdrop being performed on a different CPU than the
	 * one exiting, or kernel threads using the mm when userspace exits.
	 *
	 * IPI overheads have not found to be expensive, but they could be
	 * reduced in a number of possible ways, for example (roughly
	 * increasing order of complexity):
	 * - The last lazy reference created by exit_mm() could instead switch
	 *   to init_mm, however it's probable this will run on the same CPU
	 *   immediately afterwards, so this may not reduce IPIs much.
	 * - A batch of mms requiring IPIs could be gathered and freed at once.
	 * - CPUs store active_mm where it can be remotely checked without a
	 *   lock, to filter out false-positives in the cpumask.
	 * - After mm_users or mm_count reaches zero, switching away from the
	 *   mm could clear mm_cpumask to reduce some IPIs, perhaps together
	 *   with some batching or delaying of the final IPIs.
	 * - A delayed freeing and RCU-like quiescing sequence based on mm
	 *   switching to avoid IPIs completely.
	 */
	on_each_cpu_mask(mm_cpumask(mm), do_shoot_lazy_tlb, (void *)mm, 1);
	if (IS_ENABLED(CONFIG_DEBUG_VM_SHOOT_LAZIES))
		on_each_cpu(do_check_lazy_tlb, (void *)mm, 1);
}

/*
 * Called when the last reference to the mm
 * is dropped: either by a lazy thread or by
 * mmput. Free the page directory and the mm.
 */
void __mmdrop(struct mm_struct *mm)
{
	BUG_ON(mm == &init_mm);
	WARN_ON_ONCE(mm == current->mm);

	/* Ensure no CPUs are using this as their lazy tlb mm */
	cleanup_lazy_tlbs(mm);

	WARN_ON_ONCE(mm == current->active_mm);
	mm_free_pgd(mm);
	mm_free_id(mm);
	destroy_context(mm);
	mmu_notifier_subscriptions_destroy(mm);
	check_mm(mm);
	put_user_ns(mm->user_ns);
	mm_pasid_drop(mm);
	mm_destroy_cid(mm);
	percpu_counter_destroy_many(mm->rss_stat, NR_MM_COUNTERS);

	free_mm(mm);
}
EXPORT_SYMBOL_GPL(__mmdrop);

static void mmdrop_async_fn(struct work_struct *work)
{
	struct mm_struct *mm;

	mm = container_of(work, struct mm_struct, async_put_work);
	__mmdrop(mm);
}

static void mmdrop_async(struct mm_struct *mm)
{
	if (unlikely(atomic_dec_and_test(&mm->mm_count))) {
		INIT_WORK(&mm->async_put_work, mmdrop_async_fn);
		schedule_work(&mm->async_put_work);
	}
}

static inline void free_signal_struct(struct signal_struct *sig)
{
	taskstats_tgid_free(sig);
	sched_autogroup_exit(sig);
	/*
	 * __mmdrop is not safe to call from softirq context on x86 due to
	 * pgd_dtor so postpone it to the async context
	 */
	if (sig->oom_mm)
		mmdrop_async(sig->oom_mm);
	kmem_cache_free(signal_cachep, sig);
}

static inline void put_signal_struct(struct signal_struct *sig)
{
	if (refcount_dec_and_test(&sig->sigcnt))
		free_signal_struct(sig);
}

void __put_task_struct(struct task_struct *tsk)
{
	WARN_ON(!tsk->exit_state);
	WARN_ON(refcount_read(&tsk->usage));
	WARN_ON(tsk == current);

	sched_ext_free(tsk);
	io_uring_free(tsk);
	cgroup_free(tsk);
	task_numa_free(tsk, true);
	security_task_free(tsk);
	exit_creds(tsk);
	delayacct_tsk_free(tsk);
	put_signal_struct(tsk->signal);
	sched_core_free(tsk);
	free_task(tsk);
}
EXPORT_SYMBOL_GPL(__put_task_struct);

void __put_task_struct_rcu_cb(struct rcu_head *rhp)
{
	struct task_struct *task = container_of(rhp, struct task_struct, rcu);

	__put_task_struct(task);
}
EXPORT_SYMBOL_GPL(__put_task_struct_rcu_cb);

void __init __weak arch_task_cache_init(void) { }

/*
 * set_max_threads
 */
static void __init set_max_threads(unsigned int max_threads_suggested)
{
	u64 threads;
	unsigned long nr_pages = memblock_estimated_nr_free_pages();

	/*
	 * The number of threads shall be limited such that the thread
	 * structures may only consume a small part of the available memory.
	 */
	if (fls64(nr_pages) + fls64(PAGE_SIZE) > 64)
		threads = MAX_THREADS;
	else
		threads = div64_u64((u64) nr_pages * (u64) PAGE_SIZE,
				    (u64) THREAD_SIZE * 8UL);

	if (threads > max_threads_suggested)
		threads = max_threads_suggested;

	max_threads = clamp_t(u64, threads, MIN_THREADS, MAX_THREADS);
}

#ifdef CONFIG_ARCH_WANTS_DYNAMIC_TASK_STRUCT
/* Initialized by the architecture: */
int arch_task_struct_size __read_mostly;
#endif

static void __init task_struct_whitelist(unsigned long *offset, unsigned long *size)
{
	/* Fetch thread_struct whitelist for the architecture. */
	arch_thread_struct_whitelist(offset, size);

	/*
	 * Handle zero-sized whitelist or empty thread_struct, otherwise
	 * adjust offset to position of thread_struct in task_struct.
	 */
	if (unlikely(*size == 0))
		*offset = 0;
	else
		*offset += offsetof(struct task_struct, thread);
}

void __init fork_init(void)
{
	int i;
#ifndef ARCH_MIN_TASKALIGN
#define ARCH_MIN_TASKALIGN	0
#endif
	int align = max_t(int, L1_CACHE_BYTES, ARCH_MIN_TASKALIGN);
	unsigned long useroffset, usersize;

	/* create a slab on which task_structs can be allocated */
	task_struct_whitelist(&useroffset, &usersize);
	task_struct_cachep = kmem_cache_create_usercopy("task_struct",
			arch_task_struct_size, align,
			SLAB_PANIC|SLAB_ACCOUNT,
			useroffset, usersize, NULL);

	/* do the arch specific task caches init */
	arch_task_cache_init();

	set_max_threads(MAX_THREADS);

	init_task.signal->rlim[RLIMIT_NPROC].rlim_cur = max_threads/2;
	init_task.signal->rlim[RLIMIT_NPROC].rlim_max = max_threads/2;
	init_task.signal->rlim[RLIMIT_SIGPENDING] =
		init_task.signal->rlim[RLIMIT_NPROC];

	for (i = 0; i < UCOUNT_COUNTS; i++)
		init_user_ns.ucount_max[i] = max_threads/2;

	set_userns_rlimit_max(&init_user_ns, UCOUNT_RLIMIT_NPROC,      RLIM_INFINITY);
	set_userns_rlimit_max(&init_user_ns, UCOUNT_RLIMIT_MSGQUEUE,   RLIM_INFINITY);
	set_userns_rlimit_max(&init_user_ns, UCOUNT_RLIMIT_SIGPENDING, RLIM_INFINITY);
	set_userns_rlimit_max(&init_user_ns, UCOUNT_RLIMIT_MEMLOCK,    RLIM_INFINITY);

#ifdef CONFIG_VMAP_STACK
	cpuhp_setup_state(CPUHP_BP_PREPARE_DYN, "fork:vm_stack_cache",
			  NULL, free_vm_stack_cache);
#endif

	scs_init();

	lockdep_init_task(&init_task);
	uprobes_init();
}

int __weak arch_dup_task_struct(struct task_struct *dst,
					       struct task_struct *src)
{
	*dst = *src;
	return 0;
}

void set_task_stack_end_magic(struct task_struct *tsk)
{
	unsigned long *stackend;

	stackend = end_of_stack(tsk);
	*stackend = STACK_END_MAGIC;	/* for overflow detection */
}

static struct task_struct *dup_task_struct(struct task_struct *orig, int node)
{
	struct task_struct *tsk;
	int err;

	if (node == NUMA_NO_NODE)
		node = tsk_fork_get_node(orig);
	tsk = alloc_task_struct_node(node);
	if (!tsk)
		return NULL;

	err = arch_dup_task_struct(tsk, orig);
	if (err)
		goto free_tsk;

	err = alloc_thread_stack_node(tsk, node);
	if (err)
		goto free_tsk;

#ifdef CONFIG_THREAD_INFO_IN_TASK
	refcount_set(&tsk->stack_refcount, 1);
#endif
	account_kernel_stack(tsk, 1);

	err = scs_prepare(tsk, node);
	if (err)
		goto free_stack;

#ifdef CONFIG_SECCOMP
	/*
	 * We must handle setting up seccomp filters once we're under
	 * the sighand lock in case orig has changed between now and
	 * then. Until then, filter must be NULL to avoid messing up
	 * the usage counts on the error path calling free_task.
	 */
	tsk->seccomp.filter = NULL;
#endif

	setup_thread_stack(tsk, orig);
	clear_user_return_notifier(tsk);
	clear_tsk_need_resched(tsk);
	set_task_stack_end_magic(tsk);
	clear_syscall_work_syscall_user_dispatch(tsk);

#ifdef CONFIG_STACKPROTECTOR
	tsk->stack_canary = get_random_canary();
#endif
	if (orig->cpus_ptr == &orig->cpus_mask)
		tsk->cpus_ptr = &tsk->cpus_mask;
	dup_user_cpus_ptr(tsk, orig, node);

	/*
	 * One for the user space visible state that goes away when reaped.
	 * One for the scheduler.
	 */
	refcount_set(&tsk->rcu_users, 2);
	/* One for the rcu users */
	refcount_set(&tsk->usage, 1);
#ifdef CONFIG_BLK_DEV_IO_TRACE
	tsk->btrace_seq = 0;
#endif
	tsk->splice_pipe = NULL;
	tsk->task_frag.page = NULL;
	tsk->wake_q.next = NULL;
	tsk->worker_private = NULL;

	kcov_task_init(tsk);
	kmsan_task_create(tsk);
	kmap_local_fork(tsk);

#ifdef CONFIG_FAULT_INJECTION
	tsk->fail_nth = 0;
#endif

#ifdef CONFIG_BLK_CGROUP
	tsk->throttle_disk = NULL;
	tsk->use_memdelay = 0;
#endif

#ifdef CONFIG_ARCH_HAS_CPU_PASID
	tsk->pasid_activated = 0;
#endif

#ifdef CONFIG_MEMCG
	tsk->active_memcg = NULL;
#endif

#ifdef CONFIG_X86_BUS_LOCK_DETECT
	tsk->reported_split_lock = 0;
#endif

#ifdef CONFIG_SCHED_MM_CID
	tsk->mm_cid = -1;
	tsk->last_mm_cid = -1;
	tsk->mm_cid_active = 0;
	tsk->migrate_from_cpu = -1;
#endif
	return tsk;

free_stack:
	exit_task_stack_account(tsk);
	free_thread_stack(tsk);
free_tsk:
	free_task_struct(tsk);
	return NULL;
}

__cacheline_aligned_in_smp DEFINE_SPINLOCK(mmlist_lock);

static unsigned long default_dump_filter = MMF_DUMP_FILTER_DEFAULT;

static int __init coredump_filter_setup(char *s)
{
	default_dump_filter =
		(simple_strtoul(s, NULL, 0) << MMF_DUMP_FILTER_SHIFT) &
		MMF_DUMP_FILTER_MASK;
	return 1;
}

__setup("coredump_filter=", coredump_filter_setup);

#include <linux/init_task.h>

static void mm_init_aio(struct mm_struct *mm)
{
#ifdef CONFIG_AIO
	spin_lock_init(&mm->ioctx_lock);
	mm->ioctx_table = NULL;
#endif
}

static __always_inline void mm_clear_owner(struct mm_struct *mm,
					   struct task_struct *p)
{
#ifdef CONFIG_MEMCG
	if (mm->owner == p)
		WRITE_ONCE(mm->owner, NULL);
#endif
}

static void mm_init_owner(struct mm_struct *mm, struct task_struct *p)
{
#ifdef CONFIG_MEMCG
	mm->owner = p;
#endif
}

static void mm_init_uprobes_state(struct mm_struct *mm)
{
#ifdef CONFIG_UPROBES
	mm->uprobes_state.xol_area = NULL;
#endif
}

static void mmap_init_lock(struct mm_struct *mm)
{
	init_rwsem(&mm->mmap_lock);
	mm_lock_seqcount_init(mm);
#ifdef CONFIG_PER_VMA_LOCK
	rcuwait_init(&mm->vma_writer_wait);
#endif
}

static struct mm_struct *mm_init(struct mm_struct *mm, struct task_struct *p,
	struct user_namespace *user_ns)
{
	mt_init_flags(&mm->mm_mt, MM_MT_FLAGS);
	mt_set_external_lock(&mm->mm_mt, &mm->mmap_lock);
	atomic_set(&mm->mm_users, 1);
	atomic_set(&mm->mm_count, 1);
	seqcount_init(&mm->write_protect_seq);
	mmap_init_lock(mm);
	INIT_LIST_HEAD(&mm->mmlist);
	mm_pgtables_bytes_init(mm);
	mm->map_count = 0;
	mm->locked_vm = 0;
	atomic64_set(&mm->pinned_vm, 0);
	memset(&mm->rss_stat, 0, sizeof(mm->rss_stat));
	spin_lock_init(&mm->page_table_lock);
	spin_lock_init(&mm->arg_lock);
	mm_init_cpumask(mm);
	mm_init_aio(mm);
	mm_init_owner(mm, p);
	mm_pasid_init(mm);
	RCU_INIT_POINTER(mm->exe_file, NULL);
	mmu_notifier_subscriptions_init(mm);
	init_tlb_flush_pending(mm);
	futex_mm_init(mm);
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !defined(CONFIG_SPLIT_PMD_PTLOCKS)
	mm->pmd_huge_pte = NULL;
#endif
	mm_init_uprobes_state(mm);
	hugetlb_count_init(mm);

	if (current->mm) {
		mm->flags = mmf_init_flags(current->mm->flags);
		mm->def_flags = current->mm->def_flags & VM_INIT_DEF_MASK;
	} else {
		mm->flags = default_dump_filter;
		mm->def_flags = 0;
	}

	if (mm_alloc_pgd(mm))
		goto fail_nopgd;

	if (mm_alloc_id(mm))
		goto fail_noid;

	if (init_new_context(p, mm))
		goto fail_nocontext;

	if (mm_alloc_cid(mm, p))
		goto fail_cid;

	if (percpu_counter_init_many(mm->rss_stat, 0, GFP_KERNEL_ACCOUNT,
				     NR_MM_COUNTERS))
		goto fail_pcpu;

	mm->user_ns = get_user_ns(user_ns);
	lru_gen_init_mm(mm);
	return mm;

fail_pcpu:
	mm_destroy_cid(mm);
fail_cid:
	destroy_context(mm);
fail_nocontext:
	mm_free_id(mm);
fail_noid:
	mm_free_pgd(mm);
fail_nopgd:
	free_mm(mm);
	return NULL;
}

/*
 * Allocate and initialize an mm_struct.
 */
struct mm_struct *mm_alloc(void)
{
	struct mm_struct *mm;

	mm = allocate_mm();
	if (!mm)
		return NULL;

	memset(mm, 0, sizeof(*mm));
	return mm_init(mm, current, current_user_ns());
}
EXPORT_SYMBOL_IF_KUNIT(mm_alloc);

static inline void __mmput(struct mm_struct *mm)
{
	VM_BUG_ON(atomic_read(&mm->mm_users));

	uprobe_clear_state(mm);
	exit_aio(mm);
	ksm_exit(mm);
	khugepaged_exit(mm); /* must run before exit_mmap */
	exit_mmap(mm);
	mm_put_huge_zero_folio(mm);
	set_mm_exe_file(mm, NULL);
	if (!list_empty(&mm->mmlist)) {
		spin_lock(&mmlist_lock);
		list_del(&mm->mmlist);
		spin_unlock(&mmlist_lock);
	}
	if (mm->binfmt)
		module_put(mm->binfmt->module);
	lru_gen_del_mm(mm);
	futex_hash_free(mm);
	mmdrop(mm);
}

/*
 * Decrement the use count and release all resources for an mm.
 */
void mmput(struct mm_struct *mm)
{
	might_sleep();

	if (atomic_dec_and_test(&mm->mm_users))
		__mmput(mm);
}
EXPORT_SYMBOL_GPL(mmput);

#ifdef CONFIG_MMU
static void mmput_async_fn(struct work_struct *work)
{
	struct mm_struct *mm = container_of(work, struct mm_struct,
					    async_put_work);

	__mmput(mm);
}

void mmput_async(struct mm_struct *mm)
{
	if (atomic_dec_and_test(&mm->mm_users)) {
		INIT_WORK(&mm->async_put_work, mmput_async_fn);
		schedule_work(&mm->async_put_work);
	}
}
EXPORT_SYMBOL_GPL(mmput_async);
#endif

/**
 * set_mm_exe_file - change a reference to the mm's executable file
 * @mm: The mm to change.
 * @new_exe_file: The new file to use.
 *
 * This changes mm's executable file (shown as symlink /proc/[pid]/exe).
 *
 * Main users are mmput() and sys_execve(). Callers prevent concurrent
 * invocations: in mmput() nobody alive left, in execve it happens before
 * the new mm is made visible to anyone.
 *
 * Can only fail if new_exe_file != NULL.
 */
int set_mm_exe_file(struct mm_struct *mm, struct file *new_exe_file)
{
	struct file *old_exe_file;

	/*
	 * It is safe to dereference the exe_file without RCU as
	 * this function is only called if nobody else can access
	 * this mm -- see comment above for justification.
	 */
	old_exe_file = rcu_dereference_raw(mm->exe_file);

	if (new_exe_file) {
		/*
		 * We expect the caller (i.e., sys_execve) to already denied
		 * write access, so this is unlikely to fail.
		 */
		if (unlikely(exe_file_deny_write_access(new_exe_file)))
			return -EACCES;
		get_file(new_exe_file);
	}
	rcu_assign_pointer(mm->exe_file, new_exe_file);
	if (old_exe_file) {
		exe_file_allow_write_access(old_exe_file);
		fput(old_exe_file);
	}
	return 0;
}

/**
 * replace_mm_exe_file - replace a reference to the mm's executable file
 * @mm: The mm to change.
 * @new_exe_file: The new file to use.
 *
 * This changes mm's executable file (shown as symlink /proc/[pid]/exe).
 *
 * Main user is sys_prctl(PR_SET_MM_MAP/EXE_FILE).
 */
int replace_mm_exe_file(struct mm_struct *mm, struct file *new_exe_file)
{
	struct vm_area_struct *vma;
	struct file *old_exe_file;
	int ret = 0;

	/* Forbid mm->exe_file change if old file still mapped. */
	old_exe_file = get_mm_exe_file(mm);
	if (old_exe_file) {
		VMA_ITERATOR(vmi, mm, 0);
		mmap_read_lock(mm);
		for_each_vma(vmi, vma) {
			if (!vma->vm_file)
				continue;
			if (path_equal(&vma->vm_file->f_path,
				       &old_exe_file->f_path)) {
				ret = -EBUSY;
				break;
			}
		}
		mmap_read_unlock(mm);
		fput(old_exe_file);
		if (ret)
			return ret;
	}

	ret = exe_file_deny_write_access(new_exe_file);
	if (ret)
		return -EACCES;
	get_file(new_exe_file);

	/* set the new file */
	mmap_write_lock(mm);
	old_exe_file = rcu_dereference_raw(mm->exe_file);
	rcu_assign_pointer(mm->exe_file, new_exe_file);
	mmap_write_unlock(mm);

	if (old_exe_file) {
		exe_file_allow_write_access(old_exe_file);
		fput(old_exe_file);
	}
	return 0;
}

/**
 * get_mm_exe_file - acquire a reference to the mm's executable file
 * @mm: The mm of interest.
 *
 * Returns %NULL if mm has no associated executable file.
 * User must release file via fput().
 */
struct file *get_mm_exe_file(struct mm_struct *mm)
{
	struct file *exe_file;

	rcu_read_lock();
	exe_file = get_file_rcu(&mm->exe_file);
	rcu_read_unlock();
	return exe_file;
}

/**
 * get_task_exe_file - acquire a reference to the task's executable file
 * @task: The task.
 *
 * Returns %NULL if task's mm (if any) has no associated executable file or
 * this is a kernel thread with borrowed mm (see the comment above get_task_mm).
 * User must release file via fput().
 */
struct file *get_task_exe_file(struct task_struct *task)
{
	struct file *exe_file = NULL;
	struct mm_struct *mm;

	if (task->flags & PF_KTHREAD)
		return NULL;

	task_lock(task);
	mm = task->mm;
	if (mm)
		exe_file = get_mm_exe_file(mm);
	task_unlock(task);
	return exe_file;
}

/**
 * get_task_mm - acquire a reference to the task's mm
 * @task: The task.
 *
 * Returns %NULL if the task has no mm.  Checks PF_KTHREAD (meaning
 * this kernel workthread has transiently adopted a user mm with use_mm,
 * to do its AIO) is not set and if so returns a reference to it, after
 * bumping up the use count.  User must release the mm via mmput()
 * after use.  Typically used by /proc and ptrace.
 */
struct mm_struct *get_task_mm(struct task_struct *task)
{
	struct mm_struct *mm;

	if (task->flags & PF_KTHREAD)
		return NULL;

	task_lock(task);
	mm = task->mm;
	if (mm)
		mmget(mm);
	task_unlock(task);
	return mm;
}
EXPORT_SYMBOL_GPL(get_task_mm);

static bool may_access_mm(struct mm_struct *mm, struct task_struct *task, unsigned int mode)
{
	if (mm == current->mm)
		return true;
	if (ptrace_may_access(task, mode))
		return true;
	if ((mode & PTRACE_MODE_READ) && perfmon_capable())
		return true;
	return false;
}

struct mm_struct *mm_access(struct task_struct *task, unsigned int mode)
{
	struct mm_struct *mm;
	int err;

	err =  down_read_killable(&task->signal->exec_update_lock);
	if (err)
		return ERR_PTR(err);

	mm = get_task_mm(task);
	if (!mm) {
		mm = ERR_PTR(-ESRCH);
	} else if (!may_access_mm(mm, task, mode)) {
		mmput(mm);
		mm = ERR_PTR(-EACCES);
	}
	up_read(&task->signal->exec_update_lock);

	return mm;
}

static void complete_vfork_done(struct task_struct *tsk)
{
	struct completion *vfork;

	task_lock(tsk);
	vfork = tsk->vfork_done;
	if (likely(vfork)) {
		tsk->vfork_done = NULL;
		complete(vfork);
	}
	task_unlock(tsk);
}

static int wait_for_vfork_done(struct task_struct *child,
				struct completion *vfork)
{
	unsigned int state = TASK_KILLABLE|TASK_FREEZABLE;
	int killed;

	cgroup_enter_frozen();
	killed = wait_for_completion_state(vfork, state);
	cgroup_leave_frozen(false);

	if (killed) {
		task_lock(child);
		child->vfork_done = NULL;
		task_unlock(child);
	}

	put_task_struct(child);
	return killed;
}

/* Please note the differences between mmput and mm_release.
 * mmput is called whenever we stop holding onto a mm_struct,
 * error success whatever.
 *
 * mm_release is called after a mm_struct has been removed
 * from the current process.
 *
 * This difference is important for error handling, when we
 * only half set up a mm_struct for a new process and need to restore
 * the old one.  Because we mmput the new mm_struct before
 * restoring the old one. . .
 * Eric Biederman 10 January 1998
 */
static void mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	uprobe_free_utask(tsk);

	/* Get rid of any cached register state */
	deactivate_mm(tsk, mm);

	/*
	 * Signal userspace if we're not exiting with a core dump
	 * because we want to leave the value intact for debugging
	 * purposes.
	 */
	if (tsk->clear_child_tid) {
		if (atomic_read(&mm->mm_users) > 1) {
			/*
			 * We don't check the error code - if userspace has
			 * not set up a proper pointer then tough luck.
			 */
			put_user(0, tsk->clear_child_tid);
			do_futex(tsk->clear_child_tid, FUTEX_WAKE,
					1, NULL, NULL, 0, 0);
		}
		tsk->clear_child_tid = NULL;
	}

	/*
	 * All done, finally we can wake up parent and return this mm to him.
	 * Also kthread_stop() uses this completion for synchronization.
	 */
	if (tsk->vfork_done)
		complete_vfork_done(tsk);
}

void exit_mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	futex_exit_release(tsk);
	mm_release(tsk, mm);
}

void exec_mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	futex_exec_release(tsk);
	mm_release(tsk, mm);
}

/**
 * dup_mm() - duplicates an existing mm structure
 * @tsk: the task_struct with which the new mm will be associated.
 * @oldmm: the mm to duplicate.
 *
 * Allocates a new mm structure and duplicates the provided @oldmm structure
 * content into it.
 *
 * Return: the duplicated mm or NULL on failure.
 */
static struct mm_struct *dup_mm(struct task_struct *tsk,
				struct mm_struct *oldmm)
{
	struct mm_struct *mm;
	int err;

	mm = allocate_mm();
	if (!mm)
		goto fail_nomem;

	memcpy(mm, oldmm, sizeof(*mm));

	if (!mm_init(mm, tsk, mm->user_ns))
		goto fail_nomem;

	uprobe_start_dup_mmap();
	err = dup_mmap(mm, oldmm);
	if (err)
		goto free_pt;
	uprobe_end_dup_mmap();

	mm->hiwater_rss = get_mm_rss(mm);
	mm->hiwater_vm = mm->total_vm;

	if (mm->binfmt && !try_module_get(mm->binfmt->module))
		goto free_pt;

	return mm;

free_pt:
	/* don't put binfmt in mmput, we haven't got module yet */
	mm->binfmt = NULL;
	mm_init_owner(mm, NULL);
	mmput(mm);
	if (err)
		uprobe_end_dup_mmap();

fail_nomem:
	return NULL;
}

static int copy_mm(unsigned long clone_flags, struct task_struct *tsk)
{
	struct mm_struct *mm, *oldmm;

	tsk->min_flt = tsk->maj_flt = 0;
	tsk->nvcsw = tsk->nivcsw = 0;
#ifdef CONFIG_DETECT_HUNG_TASK
	tsk->last_switch_count = tsk->nvcsw + tsk->nivcsw;
	tsk->last_switch_time = 0;
#endif

	tsk->mm = NULL;
	tsk->active_mm = NULL;

	/*
	 * Are we cloning a kernel thread?
	 *
	 * We need to steal a active VM for that..
	 */
	oldmm = current->mm;
	if (!oldmm)
		return 0;

	if (clone_flags & CLONE_VM) {
		mmget(oldmm);
		mm = oldmm;
	} else {
		mm = dup_mm(tsk, current->mm);
		if (!mm)
			return -ENOMEM;
	}

	tsk->mm = mm;
	tsk->active_mm = mm;
	sched_mm_cid_fork(tsk);
	return 0;
}

static int copy_fs(unsigned long clone_flags, struct task_struct *tsk)
{
	struct fs_struct *fs = current->fs;
	if (clone_flags & CLONE_FS) {
		/* tsk->fs is already what we want */
		read_seqlock_excl(&fs->seq);
		/* "users" and "in_exec" locked for check_unsafe_exec() */
		if (fs->in_exec) {
			read_sequnlock_excl(&fs->seq);
			return -EAGAIN;
		}
		fs->users++;
		read_sequnlock_excl(&fs->seq);
		return 0;
	}
	tsk->fs = copy_fs_struct(fs);
	if (!tsk->fs)
		return -ENOMEM;
	return 0;
}

static int copy_files(unsigned long clone_flags, struct task_struct *tsk,
		      int no_files)
{
	struct files_struct *oldf, *newf;

	/*
	 * A background process may not have any files ...
	 */
	oldf = current->files;
	if (!oldf)
		return 0;

	if (no_files) {
		tsk->files = NULL;
		return 0;
	}

	if (clone_flags & CLONE_FILES) {
		atomic_inc(&oldf->count);
		return 0;
	}

	newf = dup_fd(oldf, NULL);
	if (IS_ERR(newf))
		return PTR_ERR(newf);

	tsk->files = newf;
	return 0;
}

static int copy_sighand(unsigned long clone_flags, struct task_struct *tsk)
{
	struct sighand_struct *sig;

	if (clone_flags & CLONE_SIGHAND) {
		refcount_inc(&current->sighand->count);
		return 0;
	}
	sig = kmem_cache_alloc(sighand_cachep, GFP_KERNEL);
	RCU_INIT_POINTER(tsk->sighand, sig);
	if (!sig)
		return -ENOMEM;

	refcount_set(&sig->count, 1);
	spin_lock_irq(&current->sighand->siglock);
	memcpy(sig->action, current->sighand->action, sizeof(sig->action));
	spin_unlock_irq(&current->sighand->siglock);

	/* Reset all signal handler not set to SIG_IGN to SIG_DFL. */
	if (clone_flags & CLONE_CLEAR_SIGHAND)
		flush_signal_handlers(tsk, 0);

	return 0;
}

void __cleanup_sighand(struct sighand_struct *sighand)
{
	if (refcount_dec_and_test(&sighand->count)) {
		signalfd_cleanup(sighand);
		/*
		 * sighand_cachep is SLAB_TYPESAFE_BY_RCU so we can free it
		 * without an RCU grace period, see __lock_task_sighand().
		 */
		kmem_cache_free(sighand_cachep, sighand);
	}
}

/*
 * Initialize POSIX timer handling for a thread group.
 */
static void posix_cpu_timers_init_group(struct signal_struct *sig)
{
	struct posix_cputimers *pct = &sig->posix_cputimers;
	unsigned long cpu_limit;

	cpu_limit = READ_ONCE(sig->rlim[RLIMIT_CPU].rlim_cur);
	posix_cputimers_group_init(pct, cpu_limit);
}

static int copy_signal(unsigned long clone_flags, struct task_struct *tsk)
{
	struct signal_struct *sig;

	if (clone_flags & CLONE_THREAD)
		return 0;

	sig = kmem_cache_zalloc(signal_cachep, GFP_KERNEL);
	tsk->signal = sig;
	if (!sig)
		return -ENOMEM;

	sig->nr_threads = 1;
	sig->quick_threads = 1;
	atomic_set(&sig->live, 1);
	refcount_set(&sig->sigcnt, 1);

	/* list_add(thread_node, thread_head) without INIT_LIST_HEAD() */
	sig->thread_head = (struct list_head)LIST_HEAD_INIT(tsk->thread_node);
	tsk->thread_node = (struct list_head)LIST_HEAD_INIT(sig->thread_head);

	init_waitqueue_head(&sig->wait_chldexit);
	sig->curr_target = tsk;
	init_sigpending(&sig->shared_pending);
	INIT_HLIST_HEAD(&sig->multiprocess);
	seqlock_init(&sig->stats_lock);
	prev_cputime_init(&sig->prev_cputime);

#ifdef CONFIG_POSIX_TIMERS
	INIT_HLIST_HEAD(&sig->posix_timers);
	INIT_HLIST_HEAD(&sig->ignored_posix_timers);
	hrtimer_setup(&sig->real_timer, it_real_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#endif

	task_lock(current->group_leader);
	memcpy(sig->rlim, current->signal->rlim, sizeof sig->rlim);
	task_unlock(current->group_leader);

	posix_cpu_timers_init_group(sig);

	tty_audit_fork(sig);
	sched_autogroup_fork(sig);

	sig->oom_score_adj = current->signal->oom_score_adj;
	sig->oom_score_adj_min = current->signal->oom_score_adj_min;

	mutex_init(&sig->cred_guard_mutex);
	init_rwsem(&sig->exec_update_lock);

	return 0;
}

static void copy_seccomp(struct task_struct *p)
{
#ifdef CONFIG_SECCOMP
	/*
	 * Must be called with sighand->lock held, which is common to
	 * all threads in the group. Holding cred_guard_mutex is not
	 * needed because this new task is not yet running and cannot
	 * be racing exec.
	 */
	assert_spin_locked(&current->sighand->siglock);

	/* Ref-count the new filter user, and assign it. */
	get_seccomp_filter(current);
	p->seccomp = current->seccomp;

	/*
	 * Explicitly enable no_new_privs here in case it got set
	 * between the task_struct being duplicated and holding the
	 * sighand lock. The seccomp state and nnp must be in sync.
	 */
	if (task_no_new_privs(current))
		task_set_no_new_privs(p);

	/*
	 * If the parent gained a seccomp mode after copying thread
	 * flags and between before we held the sighand lock, we have
	 * to manually enable the seccomp thread flag here.
	 */
	if (p->seccomp.mode != SECCOMP_MODE_DISABLED)
		set_task_syscall_work(p, SECCOMP);
#endif
}

SYSCALL_DEFINE1(set_tid_address, int __user *, tidptr)
{
	current->clear_child_tid = tidptr;

	return task_pid_vnr(current);
}

static void rt_mutex_init_task(struct task_struct *p)
{
	raw_spin_lock_init(&p->pi_lock);
#ifdef CONFIG_RT_MUTEXES
	p->pi_waiters = RB_ROOT_CACHED;
	p->pi_top_task = NULL;
	p->pi_blocked_on = NULL;
#endif
}

static inline void init_task_pid_links(struct task_struct *task)
{
	enum pid_type type;

	for (type = PIDTYPE_PID; type < PIDTYPE_MAX; ++type)
		INIT_HLIST_NODE(&task->pid_links[type]);
}

static inline void
init_task_pid(struct task_struct *task, enum pid_type type, struct pid *pid)
{
	if (type == PIDTYPE_PID)
		task->thread_pid = pid;
	else
		task->signal->pids[type] = pid;
}

static inline void rcu_copy_process(struct task_struct *p)
{
#ifdef CONFIG_PREEMPT_RCU
	p->rcu_read_lock_nesting = 0;
	p->rcu_read_unlock_special.s = 0;
	p->rcu_blocked_node = NULL;
	INIT_LIST_HEAD(&p->rcu_node_entry);
#endif /* #ifdef CONFIG_PREEMPT_RCU */
#ifdef CONFIG_TASKS_RCU
	p->rcu_tasks_holdout = false;
	INIT_LIST_HEAD(&p->rcu_tasks_holdout_list);
	p->rcu_tasks_idle_cpu = -1;
	INIT_LIST_HEAD(&p->rcu_tasks_exit_list);
#endif /* #ifdef CONFIG_TASKS_RCU */
#ifdef CONFIG_TASKS_TRACE_RCU
	p->trc_reader_nesting = 0;
	p->trc_reader_special.s = 0;
	INIT_LIST_HEAD(&p->trc_holdout_list);
	INIT_LIST_HEAD(&p->trc_blkd_node);
#endif /* #ifdef CONFIG_TASKS_TRACE_RCU */
}

/**
 * pidfd_prepare - allocate a new pidfd_file and reserve a pidfd
 * @pid:   the struct pid for which to create a pidfd
 * @flags: flags of the new @pidfd
 * @ret_file: return the new pidfs file
 *
 * Allocate a new file that stashes @pid and reserve a new pidfd number in the
 * caller's file descriptor table. The pidfd is reserved but not installed yet.
 *
 * The helper verifies that @pid is still in use, without PIDFD_THREAD the
 * task identified by @pid must be a thread-group leader.
 *
 * If this function returns successfully the caller is responsible to either
 * call fd_install() passing the returned pidfd and pidfd file as arguments in
 * order to install the pidfd into its file descriptor table or they must use
 * put_unused_fd() and fput() on the returned pidfd and pidfd file
 * respectively.
 *
 * This function is useful when a pidfd must already be reserved but there
 * might still be points of failure afterwards and the caller wants to ensure
 * that no pidfd is leaked into its file descriptor table.
 *
 * Return: On success, a reserved pidfd is returned from the function and a new
 *         pidfd file is returned in the last argument to the function. On
 *         error, a negative error code is returned from the function and the
 *         last argument remains unchanged.
 */
int pidfd_prepare(struct pid *pid, unsigned int flags, struct file **ret_file)
{
	struct file *pidfs_file;

	/*
	 * PIDFD_STALE is only allowed to be passed if the caller knows
	 * that @pid is already registered in pidfs and thus
	 * PIDFD_INFO_EXIT information is guaranteed to be available.
	 */
	if (!(flags & PIDFD_STALE)) {
		/*
		 * While holding the pidfd waitqueue lock removing the
		 * task linkage for the thread-group leader pid
		 * (PIDTYPE_TGID) isn't possible. Thus, if there's still
		 * task linkage for PIDTYPE_PID not having thread-group
		 * leader linkage for the pid means it wasn't a
		 * thread-group leader in the first place.
		 */
		guard(spinlock_irq)(&pid->wait_pidfd.lock);

		/* Task has already been reaped. */
		if (!pid_has_task(pid, PIDTYPE_PID))
			return -ESRCH;
		/*
		 * If this struct pid isn't used as a thread-group
		 * leader but the caller requested to create a
		 * thread-group leader pidfd then report ENOENT.
		 */
		if (!(flags & PIDFD_THREAD) && !pid_has_task(pid, PIDTYPE_TGID))
			return -ENOENT;
	}

	CLASS(get_unused_fd, pidfd)(O_CLOEXEC);
	if (pidfd < 0)
		return pidfd;

	pidfs_file = pidfs_alloc_file(pid, flags | O_RDWR);
	if (IS_ERR(pidfs_file))
		return PTR_ERR(pidfs_file);

	*ret_file = pidfs_file;
	return take_fd(pidfd);
}

static void __delayed_free_task(struct rcu_head *rhp)
{
	struct task_struct *tsk = container_of(rhp, struct task_struct, rcu);

	free_task(tsk);
}

static __always_inline void delayed_free_task(struct task_struct *tsk)
{
	if (IS_ENABLED(CONFIG_MEMCG))
		call_rcu(&tsk->rcu, __delayed_free_task);
	else
		free_task(tsk);
}

static void copy_oom_score_adj(u64 clone_flags, struct task_struct *tsk)
{
	/* Skip if kernel thread */
	if (!tsk->mm)
		return;

	/* Skip if spawning a thread or using vfork */
	if ((clone_flags & (CLONE_VM | CLONE_THREAD | CLONE_VFORK)) != CLONE_VM)
		return;

	/* We need to synchronize with __set_oom_adj */
	mutex_lock(&oom_adj_mutex);
	set_bit(MMF_MULTIPROCESS, &tsk->mm->flags);
	/* Update the values in case they were changed after copy_signal */
	tsk->signal->oom_score_adj = current->signal->oom_score_adj;
	tsk->signal->oom_score_adj_min = current->signal->oom_score_adj_min;
	mutex_unlock(&oom_adj_mutex);
}

#ifdef CONFIG_RV
static void rv_task_fork(struct task_struct *p)
{
	int i;

	for (i = 0; i < RV_PER_TASK_MONITORS; i++)
		p->rv[i].da_mon.monitoring = false;
}
#else
#define rv_task_fork(p) do {} while (0)
#endif

static bool need_futex_hash_allocate_default(u64 clone_flags)
{
	if ((clone_flags & (CLONE_THREAD | CLONE_VM)) != (CLONE_THREAD | CLONE_VM))
		return false;
	return true;
}

/*
 * This creates a new process as a copy of the old one,
 * but does not actually start it yet.
 *
 * It copies the registers, and all the appropriate
 * parts of the process environment (as per the clone
 * flags). The actual kick-off is left to the caller.
 */
__latent_entropy struct task_struct *copy_process(
					struct pid *pid,
					int trace,
					int node,
					struct kernel_clone_args *args)
{
	int pidfd = -1, retval;
	struct task_struct *p;
	struct multiprocess_signals delayed;
	struct file *pidfile = NULL;
	const u64 clone_flags = args->flags;
	struct nsproxy *nsp = current->nsproxy;

	/*
	 * Don't allow sharing the root directory with processes in a different
	 * namespace
	 */
	if ((clone_flags & (CLONE_NEWNS|CLONE_FS)) == (CLONE_NEWNS|CLONE_FS))
		return ERR_PTR(-EINVAL);

	if ((clone_flags & (CLONE_NEWUSER|CLONE_FS)) == (CLONE_NEWUSER|CLONE_FS))
		return ERR_PTR(-EINVAL);

	/*
	 * Thread groups must share signals as well, and detached threads
	 * can only be started up within the thread group.
	 */
	if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
		return ERR_PTR(-EINVAL);

	/*
	 * Shared signal handlers imply shared VM. By way of the above,
	 * thread groups also imply shared VM. Blocking this case allows
	 * for various simplifications in other code.
	 */
	if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
		return ERR_PTR(-EINVAL);

	/*
	 * Siblings of global init remain as zombies on exit since they are
	 * not reaped by their parent (swapper). To solve this and to avoid
	 * multi-rooted process trees, prevent global and container-inits
	 * from creating siblings.
	 */
	if ((clone_flags & CLONE_PARENT) &&
				current->signal->flags & SIGNAL_UNKILLABLE)
		return ERR_PTR(-EINVAL);

	/*
	 * If the new process will be in a different pid or user namespace
	 * do not allow it to share a thread group with the forking task.
	 */
	if (clone_flags & CLONE_THREAD) {
		if ((clone_flags & (CLONE_NEWUSER | CLONE_NEWPID)) ||
		    (task_active_pid_ns(current) != nsp->pid_ns_for_children))
			return ERR_PTR(-EINVAL);
	}

	if (clone_flags & CLONE_PIDFD) {
		/*
		 * - CLONE_DETACHED is blocked so that we can potentially
		 *   reuse it later for CLONE_PIDFD.
		 */
		if (clone_flags & CLONE_DETACHED)
			return ERR_PTR(-EINVAL);
	}

	/*
	 * Force any signals received before this point to be delivered
	 * before the fork happens.  Collect up signals sent to multiple
	 * processes that happen during the fork and delay them so that
	 * they appear to happen after the fork.
	 */
	sigemptyset(&delayed.signal);
	INIT_HLIST_NODE(&delayed.node);

	spin_lock_irq(&current->sighand->siglock);
	if (!(clone_flags & CLONE_THREAD))
		hlist_add_head(&delayed.node, &current->signal->multiprocess);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
	retval = -ERESTARTNOINTR;
	if (task_sigpending(current))
		goto fork_out;

	retval = -ENOMEM;
	p = dup_task_struct(current, node);
	if (!p)
		goto fork_out;
	p->flags &= ~PF_KTHREAD;
	if (args->kthread)
		p->flags |= PF_KTHREAD;
	if (args->user_worker) {
		/*
		 * Mark us a user worker, and block any signal that isn't
		 * fatal or STOP
		 */
		p->flags |= PF_USER_WORKER;
		siginitsetinv(&p->blocked, sigmask(SIGKILL)|sigmask(SIGSTOP));
	}
	if (args->io_thread)
		p->flags |= PF_IO_WORKER;

	if (args->name)
		strscpy_pad(p->comm, args->name, sizeof(p->comm));

	p->set_child_tid = (clone_flags & CLONE_CHILD_SETTID) ? args->child_tid : NULL;
	/*
	 * Clear TID on mm_release()?
	 */
	p->clear_child_tid = (clone_flags & CLONE_CHILD_CLEARTID) ? args->child_tid : NULL;

	ftrace_graph_init_task(p);

	rt_mutex_init_task(p);

	lockdep_assert_irqs_enabled();
#ifdef CONFIG_PROVE_LOCKING
	DEBUG_LOCKS_WARN_ON(!p->softirqs_enabled);
#endif
	retval = copy_creds(p, clone_flags);
	if (retval < 0)
		goto bad_fork_free;

	retval = -EAGAIN;
	if (is_rlimit_overlimit(task_ucounts(p), UCOUNT_RLIMIT_NPROC, rlimit(RLIMIT_NPROC))) {
		if (p->real_cred->user != INIT_USER &&
		    !capable(CAP_SYS_RESOURCE) && !capable(CAP_SYS_ADMIN))
			goto bad_fork_cleanup_count;
	}
	current->flags &= ~PF_NPROC_EXCEEDED;

	/*
	 * If multiple threads are within copy_process(), then this check
	 * triggers too late. This doesn't hurt, the check is only there
	 * to stop root fork bombs.
	 */
	retval = -EAGAIN;
	if (data_race(nr_threads >= max_threads))
		goto bad_fork_cleanup_count;

	delayacct_tsk_init(p);	/* Must remain after dup_task_struct() */
	p->flags &= ~(PF_SUPERPRIV | PF_WQ_WORKER | PF_IDLE | PF_NO_SETAFFINITY);
	p->flags |= PF_FORKNOEXEC;
	INIT_LIST_HEAD(&p->children);
	INIT_LIST_HEAD(&p->sibling);
	rcu_copy_process(p);
	p->vfork_done = NULL;
	spin_lock_init(&p->alloc_lock);

	init_sigpending(&p->pending);

	p->utime = p->stime = p->gtime = 0;
#ifdef CONFIG_ARCH_HAS_SCALED_CPUTIME
	p->utimescaled = p->stimescaled = 0;
#endif
	prev_cputime_init(&p->prev_cputime);

#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	seqcount_init(&p->vtime.seqcount);
	p->vtime.starttime = 0;
	p->vtime.state = VTIME_INACTIVE;
#endif

#ifdef CONFIG_IO_URING
	p->io_uring = NULL;
#endif

	p->default_timer_slack_ns = current->timer_slack_ns;

#ifdef CONFIG_PSI
	p->psi_flags = 0;
#endif

	task_io_accounting_init(&p->ioac);
	acct_clear_integrals(p);

	posix_cputimers_init(&p->posix_cputimers);
	tick_dep_init_task(p);

	p->io_context = NULL;
	audit_set_context(p, NULL);
	cgroup_fork(p);
	if (args->kthread) {
		if (!set_kthread_struct(p))
			goto bad_fork_cleanup_delayacct;
	}
#ifdef CONFIG_NUMA
	p->mempolicy = mpol_dup(p->mempolicy);
	if (IS_ERR(p->mempolicy)) {
		retval = PTR_ERR(p->mempolicy);
		p->mempolicy = NULL;
		goto bad_fork_cleanup_delayacct;
	}
#endif
#ifdef CONFIG_CPUSETS
	p->cpuset_mem_spread_rotor = NUMA_NO_NODE;
	seqcount_spinlock_init(&p->mems_allowed_seq, &p->alloc_lock);
#endif
#ifdef CONFIG_TRACE_IRQFLAGS
	memset(&p->irqtrace, 0, sizeof(p->irqtrace));
	p->irqtrace.hardirq_disable_ip	= _THIS_IP_;
	p->irqtrace.softirq_enable_ip	= _THIS_IP_;
	p->softirqs_enabled		= 1;
	p->softirq_context		= 0;
#endif

	p->pagefault_disabled = 0;

#ifdef CONFIG_LOCKDEP
	lockdep_init_task(p);
#endif

#ifdef CONFIG_DEBUG_MUTEXES
	p->blocked_on = NULL; /* not blocked yet */
#endif
#ifdef CONFIG_BCACHE
	p->sequential_io	= 0;
	p->sequential_io_avg	= 0;
#endif
#ifdef CONFIG_BPF_SYSCALL
	RCU_INIT_POINTER(p->bpf_storage, NULL);
	p->bpf_ctx = NULL;
#endif

	/* Perform scheduler related setup. Assign this task to a CPU. */
	retval = sched_fork(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_policy;

	retval = perf_event_init_task(p, clone_flags);
	if (retval)
		goto bad_fork_sched_cancel_fork;
	retval = audit_alloc(p);
	if (retval)
		goto bad_fork_cleanup_perf;
	/* copy all the process information */
	shm_init_task(p);
	retval = security_task_alloc(p, clone_flags);
	if (retval)
		goto bad_fork_cleanup_audit;
	retval = copy_semundo(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_security;
	retval = copy_files(clone_flags, p, args->no_files);
	if (retval)
		goto bad_fork_cleanup_semundo;
	retval = copy_fs(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_files;
	retval = copy_sighand(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_fs;
	retval = copy_signal(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_sighand;
	retval = copy_mm(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_signal;
	retval = copy_namespaces(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_mm;
	retval = copy_io(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_namespaces;
	retval = copy_thread(p, args);
	if (retval)
		goto bad_fork_cleanup_io;

	stackleak_task_init(p);

	if (pid != &init_struct_pid) {
		pid = alloc_pid(p->nsproxy->pid_ns_for_children, args->set_tid,
				args->set_tid_size);
		if (IS_ERR(pid)) {
			retval = PTR_ERR(pid);
			goto bad_fork_cleanup_thread;
		}
	}

	/*
	 * This has to happen after we've potentially unshared the file
	 * descriptor table (so that the pidfd doesn't leak into the child
	 * if the fd table isn't shared).
	 */
	if (clone_flags & CLONE_PIDFD) {
		int flags = (clone_flags & CLONE_THREAD) ? PIDFD_THREAD : 0;

		/*
		 * Note that no task has been attached to @pid yet indicate
		 * that via CLONE_PIDFD.
		 */
		retval = pidfd_prepare(pid, flags | PIDFD_STALE, &pidfile);
		if (retval < 0)
			goto bad_fork_free_pid;
		pidfd = retval;

		retval = put_user(pidfd, args->pidfd);
		if (retval)
			goto bad_fork_put_pidfd;
	}

#ifdef CONFIG_BLOCK
	p->plug = NULL;
#endif
	futex_init_task(p);

	/*
	 * sigaltstack should be cleared when sharing the same VM
	 */
	if ((clone_flags & (CLONE_VM|CLONE_VFORK)) == CLONE_VM)
		sas_ss_reset(p);

	/*
	 * Syscall tracing and stepping should be turned off in the
	 * child regardless of CLONE_PTRACE.
	 */
	user_disable_single_step(p);
	clear_task_syscall_work(p, SYSCALL_TRACE);
#if defined(CONFIG_GENERIC_ENTRY) || defined(TIF_SYSCALL_EMU)
	clear_task_syscall_work(p, SYSCALL_EMU);
#endif
	clear_tsk_latency_tracing(p);

	/* ok, now we should be set up.. */
	p->pid = pid_nr(pid);
	if (clone_flags & CLONE_THREAD) {
		p->group_leader = current->group_leader;
		p->tgid = current->tgid;
	} else {
		p->group_leader = p;
		p->tgid = p->pid;
	}

	p->nr_dirtied = 0;
	p->nr_dirtied_pause = 128 >> (PAGE_SHIFT - 10);
	p->dirty_paused_when = 0;

	p->pdeath_signal = 0;
	p->task_works = NULL;
	clear_posix_cputimers_work(p);

#ifdef CONFIG_KRETPROBES
	p->kretprobe_instances.first = NULL;
#endif
#ifdef CONFIG_RETHOOK
	p->rethooks.first = NULL;
#endif

	/*
	 * Ensure that the cgroup subsystem policies allow the new process to be
	 * forked. It should be noted that the new process's css_set can be changed
	 * between here and cgroup_post_fork() if an organisation operation is in
	 * progress.
	 */
	retval = cgroup_can_fork(p, args);
	if (retval)
		goto bad_fork_put_pidfd;

	/*
	 * Now that the cgroups are pinned, re-clone the parent cgroup and put
	 * the new task on the correct runqueue. All this *before* the task
	 * becomes visible.
	 *
	 * This isn't part of ->can_fork() because while the re-cloning is
	 * cgroup specific, it unconditionally needs to place the task on a
	 * runqueue.
	 */
	retval = sched_cgroup_fork(p, args);
	if (retval)
		goto bad_fork_cancel_cgroup;

	/*
	 * Allocate a default futex hash for the user process once the first
	 * thread spawns.
	 */
	if (need_futex_hash_allocate_default(clone_flags)) {
		retval = futex_hash_allocate_default();
		if (retval)
			goto bad_fork_core_free;
		/*
		 * If we fail beyond this point we don't free the allocated
		 * futex hash map. We assume that another thread will be created
		 * and makes use of it. The hash map will be freed once the main
		 * thread terminates.
		 */
	}
	/*
	 * From this point on we must avoid any synchronous user-space
	 * communication until we take the tasklist-lock. In particular, we do
	 * not want user-space to be able to predict the process start-time by
	 * stalling fork(2) after we recorded the start_time but before it is
	 * visible to the system.
	 */

	p->start_time = ktime_get_ns();
	p->start_boottime = ktime_get_boottime_ns();

	/*
	 * Make it visible to the rest of the system, but dont wake it up yet.
	 * Need tasklist lock for parent etc handling!
	 */
	write_lock_irq(&tasklist_lock);

	/* CLONE_PARENT re-uses the old parent */
	if (clone_flags & (CLONE_PARENT|CLONE_THREAD)) {
		p->real_parent = current->real_parent;
		p->parent_exec_id = current->parent_exec_id;
		if (clone_flags & CLONE_THREAD)
			p->exit_signal = -1;
		else
			p->exit_signal = current->group_leader->exit_signal;
	} else {
		p->real_parent = current;
		p->parent_exec_id = current->self_exec_id;
		p->exit_signal = args->exit_signal;
	}

	klp_copy_process(p);

	sched_core_fork(p);

	spin_lock(&current->sighand->siglock);

	rv_task_fork(p);

	rseq_fork(p, clone_flags);

	/* Don't start children in a dying pid namespace */
	if (unlikely(!(ns_of_pid(pid)->pid_allocated & PIDNS_ADDING))) {
		retval = -ENOMEM;
		goto bad_fork_core_free;
	}

	/* Let kill terminate clone/fork in the middle */
	if (fatal_signal_pending(current)) {
		retval = -EINTR;
		goto bad_fork_core_free;
	}

	/* No more failure paths after this point. */

	/*
	 * Copy seccomp details explicitly here, in case they were changed
	 * before holding sighand lock.
	 */
	copy_seccomp(p);

	init_task_pid_links(p);
	if (likely(p->pid)) {
		ptrace_init_task(p, (clone_flags & CLONE_PTRACE) || trace);

		init_task_pid(p, PIDTYPE_PID, pid);
		if (thread_group_leader(p)) {
			init_task_pid(p, PIDTYPE_TGID, pid);
			init_task_pid(p, PIDTYPE_PGID, task_pgrp(current));
			init_task_pid(p, PIDTYPE_SID, task_session(current));

			if (is_child_reaper(pid)) {
				ns_of_pid(pid)->child_reaper = p;
				p->signal->flags |= SIGNAL_UNKILLABLE;
			}
			p->signal->shared_pending.signal = delayed.signal;
			p->signal->tty = tty_kref_get(current->signal->tty);
			/*
			 * Inherit has_child_subreaper flag under the same
			 * tasklist_lock with adding child to the process tree
			 * for propagate_has_child_subreaper optimization.
			 */
			p->signal->has_child_subreaper = p->real_parent->signal->has_child_subreaper ||
							 p->real_parent->signal->is_child_subreaper;
			list_add_tail(&p->sibling, &p->real_parent->children);
			list_add_tail_rcu(&p->tasks, &init_task.tasks);
			attach_pid(p, PIDTYPE_TGID);
			attach_pid(p, PIDTYPE_PGID);
			attach_pid(p, PIDTYPE_SID);
			__this_cpu_inc(process_counts);
		} else {
			current->signal->nr_threads++;
			current->signal->quick_threads++;
			atomic_inc(&current->signal->live);
			refcount_inc(&current->signal->sigcnt);
			task_join_group_stop(p);
			list_add_tail_rcu(&p->thread_node,
					  &p->signal->thread_head);
		}
		attach_pid(p, PIDTYPE_PID);
		nr_threads++;
	}
	total_forks++;
	hlist_del_init(&delayed.node);
	spin_unlock(&current->sighand->siglock);
	syscall_tracepoint_update(p);
	write_unlock_irq(&tasklist_lock);

	if (pidfile)
		fd_install(pidfd, pidfile);

	proc_fork_connector(p);
	sched_post_fork(p);
	cgroup_post_fork(p, args);
	perf_event_fork(p);

	trace_task_newtask(p, clone_flags);
	uprobe_copy_process(p, clone_flags);
	user_events_fork(p, clone_flags);

	copy_oom_score_adj(clone_flags, p);

	return p;

bad_fork_core_free:
	sched_core_free(p);
	spin_unlock(&current->sighand->siglock);
	write_unlock_irq(&tasklist_lock);
bad_fork_cancel_cgroup:
	cgroup_cancel_fork(p, args);
bad_fork_put_pidfd:
	if (clone_flags & CLONE_PIDFD) {
		fput(pidfile);
		put_unused_fd(pidfd);
	}
bad_fork_free_pid:
	if (pid != &init_struct_pid)
		free_pid(pid);
bad_fork_cleanup_thread:
	exit_thread(p);
bad_fork_cleanup_io:
	if (p->io_context)
		exit_io_context(p);
bad_fork_cleanup_namespaces:
	exit_task_namespaces(p);
bad_fork_cleanup_mm:
	if (p->mm) {
		mm_clear_owner(p->mm, p);
		mmput(p->mm);
	}
bad_fork_cleanup_signal:
	if (!(clone_flags & CLONE_THREAD))
		free_signal_struct(p->signal);
bad_fork_cleanup_sighand:
	__cleanup_sighand(p->sighand);
bad_fork_cleanup_fs:
	exit_fs(p); /* blocking */
bad_fork_cleanup_files:
	exit_files(p); /* blocking */
bad_fork_cleanup_semundo:
	exit_sem(p);
bad_fork_cleanup_security:
	security_task_free(p);
bad_fork_cleanup_audit:
	audit_free(p);
bad_fork_cleanup_perf:
	perf_event_free_task(p);
bad_fork_sched_cancel_fork:
	sched_cancel_fork(p);
bad_fork_cleanup_policy:
	lockdep_free_task(p);
#ifdef CONFIG_NUMA
	mpol_put(p->mempolicy);
#endif
bad_fork_cleanup_delayacct:
	delayacct_tsk_free(p);
bad_fork_cleanup_count:
	dec_rlimit_ucounts(task_ucounts(p), UCOUNT_RLIMIT_NPROC, 1);
	exit_creds(p);
bad_fork_free:
	WRITE_ONCE(p->__state, TASK_DEAD);
	exit_task_stack_account(p);
	put_task_stack(p);
	delayed_free_task(p);
fork_out:
	spin_lock_irq(&current->sighand->siglock);
	hlist_del_init(&delayed.node);
	spin_unlock_irq(&current->sighand->siglock);
	return ERR_PTR(retval);
}

static inline void init_idle_pids(struct task_struct *idle)
{
	enum pid_type type;

	for (type = PIDTYPE_PID; type < PIDTYPE_MAX; ++type) {
		INIT_HLIST_NODE(&idle->pid_links[type]); /* not really needed */
		init_task_pid(idle, type, &init_struct_pid);
	}
}

static int idle_dummy(void *dummy)
{
	/* This function is never called */
	return 0;
}

struct task_struct * __init fork_idle(int cpu)
{
	struct task_struct *task;
	struct kernel_clone_args args = {
		.flags		= CLONE_VM,
		.fn		= &idle_dummy,
		.fn_arg		= NULL,
		.kthread	= 1,
		.idle		= 1,
	};

	task = copy_process(&init_struct_pid, 0, cpu_to_node(cpu), &args);
	if (!IS_ERR(task)) {
		init_idle_pids(task);
		init_idle(task, cpu);
	}

	return task;
}

/*
 * This is like kernel_clone(), but shaved down and tailored to just
 * creating io_uring workers. It returns a created task, or an error pointer.
 * The returned task is inactive, and the caller must fire it up through
 * wake_up_new_task(p). All signals are blocked in the created task.
 */
struct task_struct *create_io_thread(int (*fn)(void *), void *arg, int node)
{
	unsigned long flags = CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|
				CLONE_IO;
	struct kernel_clone_args args = {
		.flags		= ((lower_32_bits(flags) | CLONE_VM |
				    CLONE_UNTRACED) & ~CSIGNAL),
		.exit_signal	= (lower_32_bits(flags) & CSIGNAL),
		.fn		= fn,
		.fn_arg		= arg,
		.io_thread	= 1,
		.user_worker	= 1,
	};

	return copy_process(NULL, 0, node, &args);
}

/*
 *  Ok, this is the main fork-routine.
 *
 * It copies the process, and if successful kick-starts
 * it and waits for it to finish using the VM if required.
 *
 * args->exit_signal is expected to be checked for sanity by the caller.
 */
pid_t kernel_clone(struct kernel_clone_args *args)
{
	u64 clone_flags = args->flags;
	struct completion vfork;
	struct pid *pid;
	struct task_struct *p;
	int trace = 0;
	pid_t nr;

	/*
	 * For legacy clone() calls, CLONE_PIDFD uses the parent_tid argument
	 * to return the pidfd. Hence, CLONE_PIDFD and CLONE_PARENT_SETTID are
	 * mutually exclusive. With clone3() CLONE_PIDFD has grown a separate
	 * field in struct clone_args and it still doesn't make sense to have
	 * them both point at the same memory location. Performing this check
	 * here has the advantage that we don't need to have a separate helper
	 * to check for legacy clone().
	 */
	if ((clone_flags & CLONE_PIDFD) &&
	    (clone_flags & CLONE_PARENT_SETTID) &&
	    (args->pidfd == args->parent_tid))
		return -EINVAL;

	/*
	 * Determine whether and which event to report to ptracer.  When
	 * called from kernel_thread or CLONE_UNTRACED is explicitly
	 * requested, no event is reported; otherwise, report if the event
	 * for the type of forking is enabled.
	 */
	if (!(clone_flags & CLONE_UNTRACED)) {
		if (clone_flags & CLONE_VFORK)
			trace = PTRACE_EVENT_VFORK;
		else if (args->exit_signal != SIGCHLD)
			trace = PTRACE_EVENT_CLONE;
		else
			trace = PTRACE_EVENT_FORK;

		if (likely(!ptrace_event_enabled(current, trace)))
			trace = 0;
	}

	p = copy_process(NULL, trace, NUMA_NO_NODE, args);
	add_latent_entropy();

	if (IS_ERR(p))
		return PTR_ERR(p);

	/*
	 * Do this prior waking up the new thread - the thread pointer
	 * might get invalid after that point, if the thread exits quickly.
	 */
	trace_sched_process_fork(current, p);

	pid = get_task_pid(p, PIDTYPE_PID);
	nr = pid_vnr(pid);

	if (clone_flags & CLONE_PARENT_SETTID)
		put_user(nr, args->parent_tid);

	if (clone_flags & CLONE_VFORK) {
		p->vfork_done = &vfork;
		init_completion(&vfork);
		get_task_struct(p);
	}

	if (IS_ENABLED(CONFIG_LRU_GEN_WALKS_MMU) && !(clone_flags & CLONE_VM)) {
		/* lock the task to synchronize with memcg migration */
		task_lock(p);
		lru_gen_add_mm(p->mm);
		task_unlock(p);
	}

	wake_up_new_task(p);

	/* forking complete and child started to run, tell ptracer */
	if (unlikely(trace))
		ptrace_event_pid(trace, pid);

	if (clone_flags & CLONE_VFORK) {
		if (!wait_for_vfork_done(p, &vfork))
			ptrace_event_pid(PTRACE_EVENT_VFORK_DONE, pid);
	}

	put_pid(pid);
	return nr;
}

/*
 * Create a kernel thread.
 */
pid_t kernel_thread(int (*fn)(void *), void *arg, const char *name,
		    unsigned long flags)
{
	struct kernel_clone_args args = {
		.flags		= ((lower_32_bits(flags) | CLONE_VM |
				    CLONE_UNTRACED) & ~CSIGNAL),
		.exit_signal	= (lower_32_bits(flags) & CSIGNAL),
		.fn		= fn,
		.fn_arg		= arg,
		.name		= name,
		.kthread	= 1,
	};

	return kernel_clone(&args);
}

/*
 * Create a user mode thread.
 */
pid_t user_mode_thread(int (*fn)(void *), void *arg, unsigned long flags)
{
	struct kernel_clone_args args = {
		.flags		= ((lower_32_bits(flags) | CLONE_VM |
				    CLONE_UNTRACED) & ~CSIGNAL),
		.exit_signal	= (lower_32_bits(flags) & CSIGNAL),
		.fn		= fn,
		.fn_arg		= arg,
	};

	return kernel_clone(&args);
}

#ifdef __ARCH_WANT_SYS_FORK
SYSCALL_DEFINE0(fork)
{
#ifdef CONFIG_MMU
	struct kernel_clone_args args = {
		.exit_signal = SIGCHLD,
	};

	return kernel_clone(&args);
#else
	/* can not support in nommu mode */
	return -EINVAL;
#endif
}
#endif

#ifdef __ARCH_WANT_SYS_VFORK
SYSCALL_DEFINE0(vfork)
{
	struct kernel_clone_args args = {
		.flags		= CLONE_VFORK | CLONE_VM,
		.exit_signal	= SIGCHLD,
	};

	return kernel_clone(&args);
}
#endif

#ifdef __ARCH_WANT_SYS_CLONE
#ifdef CONFIG_CLONE_BACKWARDS
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
		 int __user *, parent_tidptr,
		 unsigned long, tls,
		 int __user *, child_tidptr)
#elif defined(CONFIG_CLONE_BACKWARDS2)
SYSCALL_DEFINE5(clone, unsigned long, newsp, unsigned long, clone_flags,
		 int __user *, parent_tidptr,
		 int __user *, child_tidptr,
		 unsigned long, tls)
#elif defined(CONFIG_CLONE_BACKWARDS3)
SYSCALL_DEFINE6(clone, unsigned long, clone_flags, unsigned long, newsp,
		int, stack_size,
		int __user *, parent_tidptr,
		int __user *, child_tidptr,
		unsigned long, tls)
#else
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
		 int __user *, parent_tidptr,
		 int __user *, child_tidptr,
		 unsigned long, tls)
#endif
{
	struct kernel_clone_args args = {
		.flags		= (lower_32_bits(clone_flags) & ~CSIGNAL),
		.pidfd		= parent_tidptr,
		.child_tid	= child_tidptr,
		.parent_tid	= parent_tidptr,
		.exit_signal	= (lower_32_bits(clone_flags) & CSIGNAL),
		.stack		= newsp,
		.tls		= tls,
	};

	return kernel_clone(&args);
}
#endif

static noinline int copy_clone_args_from_user(struct kernel_clone_args *kargs,
					      struct clone_args __user *uargs,
					      size_t usize)
{
	int err;
	struct clone_args args;
	pid_t *kset_tid = kargs->set_tid;

	BUILD_BUG_ON(offsetofend(struct clone_args, tls) !=
		     CLONE_ARGS_SIZE_VER0);
	BUILD_BUG_ON(offsetofend(struct clone_args, set_tid_size) !=
		     CLONE_ARGS_SIZE_VER1);
	BUILD_BUG_ON(offsetofend(struct clone_args, cgroup) !=
		     CLONE_ARGS_SIZE_VER2);
	BUILD_BUG_ON(sizeof(struct clone_args) != CLONE_ARGS_SIZE_VER2);

	if (unlikely(usize > PAGE_SIZE))
		return -E2BIG;
	if (unlikely(usize < CLONE_ARGS_SIZE_VER0))
		return -EINVAL;

	err = copy_struct_from_user(&args, sizeof(args), uargs, usize);
	if (err)
		return err;

	if (unlikely(args.set_tid_size > MAX_PID_NS_LEVEL))
		return -EINVAL;

	if (unlikely(!args.set_tid && args.set_tid_size > 0))
		return -EINVAL;

	if (unlikely(args.set_tid && args.set_tid_size == 0))
		return -EINVAL;

	/*
	 * Verify that higher 32bits of exit_signal are unset and that
	 * it is a valid signal
	 */
	if (unlikely((args.exit_signal & ~((u64)CSIGNAL)) ||
		     !valid_signal(args.exit_signal)))
		return -EINVAL;

	if ((args.flags & CLONE_INTO_CGROUP) &&
	    (args.cgroup > INT_MAX || usize < CLONE_ARGS_SIZE_VER2))
		return -EINVAL;

	*kargs = (struct kernel_clone_args){
		.flags		= args.flags,
		.pidfd		= u64_to_user_ptr(args.pidfd),
		.child_tid	= u64_to_user_ptr(args.child_tid),
		.parent_tid	= u64_to_user_ptr(args.parent_tid),
		.exit_signal	= args.exit_signal,
		.stack		= args.stack,
		.stack_size	= args.stack_size,
		.tls		= args.tls,
		.set_tid_size	= args.set_tid_size,
		.cgroup		= args.cgroup,
	};

	if (args.set_tid &&
		copy_from_user(kset_tid, u64_to_user_ptr(args.set_tid),
			(kargs->set_tid_size * sizeof(pid_t))))
		return -EFAULT;

	kargs->set_tid = kset_tid;

	return 0;
}

/**
 * clone3_stack_valid - check and prepare stack
 * @kargs: kernel clone args
 *
 * Verify that the stack arguments userspace gave us are sane.
 * In addition, set the stack direction for userspace since it's easy for us to
 * determine.
 */
static inline bool clone3_stack_valid(struct kernel_clone_args *kargs)
{
	if (kargs->stack == 0) {
		if (kargs->stack_size > 0)
			return false;
	} else {
		if (kargs->stack_size == 0)
			return false;

		if (!access_ok((void __user *)kargs->stack, kargs->stack_size))
			return false;

#if !defined(CONFIG_STACK_GROWSUP)
		kargs->stack += kargs->stack_size;
#endif
	}

	return true;
}

static bool clone3_args_valid(struct kernel_clone_args *kargs)
{
	/* Verify that no unknown flags are passed along. */
	if (kargs->flags &
	    ~(CLONE_LEGACY_FLAGS | CLONE_CLEAR_SIGHAND | CLONE_INTO_CGROUP))
		return false;

	/*
	 * - make the CLONE_DETACHED bit reusable for clone3
	 * - make the CSIGNAL bits reusable for clone3
	 */
	if (kargs->flags & (CLONE_DETACHED | (CSIGNAL & (~CLONE_NEWTIME))))
		return false;

	if ((kargs->flags & (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND)) ==
	    (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND))
		return false;

	if ((kargs->flags & (CLONE_THREAD | CLONE_PARENT)) &&
	    kargs->exit_signal)
		return false;

	if (!clone3_stack_valid(kargs))
		return false;

	return true;
}

/**
 * sys_clone3 - create a new process with specific properties
 * @uargs: argument structure
 * @size:  size of @uargs
 *
 * clone3() is the extensible successor to clone()/clone2().
 * It takes a struct as argument that is versioned by its size.
 *
 * Return: On success, a positive PID for the child process.
 *         On error, a negative errno number.
 */
SYSCALL_DEFINE2(clone3, struct clone_args __user *, uargs, size_t, size)
{
	int err;

	struct kernel_clone_args kargs;
	pid_t set_tid[MAX_PID_NS_LEVEL];

#ifdef __ARCH_BROKEN_SYS_CLONE3
#warning clone3() entry point is missing, please fix
	return -ENOSYS;
#endif

	kargs.set_tid = set_tid;

	err = copy_clone_args_from_user(&kargs, uargs, size);
	if (err)
		return err;

	if (!clone3_args_valid(&kargs))
		return -EINVAL;

	return kernel_clone(&kargs);
}

void walk_process_tree(struct task_struct *top, proc_visitor visitor, void *data)
{
	struct task_struct *leader, *parent, *child;
	int res;

	read_lock(&tasklist_lock);
	leader = top = top->group_leader;
down:
	for_each_thread(leader, parent) {
		list_for_each_entry(child, &parent->children, sibling) {
			res = visitor(child, data);
			if (res) {
				if (res < 0)
					goto out;
				leader = child;
				goto down;
			}
up:
			;
		}
	}

	if (leader != top) {
		child = leader;
		parent = child->real_parent;
		leader = parent->group_leader;
		goto up;
	}
out:
	read_unlock(&tasklist_lock);
}

#ifndef ARCH_MIN_MMSTRUCT_ALIGN
#define ARCH_MIN_MMSTRUCT_ALIGN 0
#endif

static void sighand_ctor(void *data)
{
	struct sighand_struct *sighand = data;

	spin_lock_init(&sighand->siglock);
	init_waitqueue_head(&sighand->signalfd_wqh);
}

void __init mm_cache_init(void)
{
	unsigned int mm_size;

	/*
	 * The mm_cpumask is located at the end of mm_struct, and is
	 * dynamically sized based on the maximum CPU number this system
	 * can have, taking hotplug into account (nr_cpu_ids).
	 */
	mm_size = sizeof(struct mm_struct) + cpumask_size() + mm_cid_size();

	mm_cachep = kmem_cache_create_usercopy("mm_struct",
			mm_size, ARCH_MIN_MMSTRUCT_ALIGN,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			offsetof(struct mm_struct, saved_auxv),
			sizeof_field(struct mm_struct, saved_auxv),
			NULL);
}

void __init proc_caches_init(void)
{
	sighand_cachep = kmem_cache_create("sighand_cache",
			sizeof(struct sighand_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_TYPESAFE_BY_RCU|
			SLAB_ACCOUNT, sighand_ctor);
	signal_cachep = kmem_cache_create("signal_cache",
			sizeof(struct signal_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);
	files_cachep = kmem_cache_create("files_cache",
			sizeof(struct files_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);
	fs_cachep = kmem_cache_create("fs_cache",
			sizeof(struct fs_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);
	mmap_init();
	nsproxy_cache_init();
}

/*
 * Check constraints on flags passed to the unshare system call.
 */
static int check_unshare_flags(unsigned long unshare_flags)
{
	if (unshare_flags & ~(CLONE_THREAD|CLONE_FS|CLONE_NEWNS|CLONE_SIGHAND|
				CLONE_VM|CLONE_FILES|CLONE_SYSVSEM|
				CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWNET|
				CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWCGROUP|
				CLONE_NEWTIME))
		return -EINVAL;
	/*
	 * Not implemented, but pretend it works if there is nothing
	 * to unshare.  Note that unsharing the address space or the
	 * signal handlers also need to unshare the signal queues (aka
	 * CLONE_THREAD).
	 */
	if (unshare_flags & (CLONE_THREAD | CLONE_SIGHAND | CLONE_VM)) {
		if (!thread_group_empty(current))
			return -EINVAL;
	}
	if (unshare_flags & (CLONE_SIGHAND | CLONE_VM)) {
		if (refcount_read(&current->sighand->count) > 1)
			return -EINVAL;
	}
	if (unshare_flags & CLONE_VM) {
		if (!current_is_single_threaded())
			return -EINVAL;
	}

	return 0;
}

/*
 * Unshare the filesystem structure if it is being shared
 */
static int unshare_fs(unsigned long unshare_flags, struct fs_struct **new_fsp)
{
	struct fs_struct *fs = current->fs;

	if (!(unshare_flags & CLONE_FS) || !fs)
		return 0;

	/* don't need lock here; in the worst case we'll do useless copy */
	if (fs->users == 1)
		return 0;

	*new_fsp = copy_fs_struct(fs);
	if (!*new_fsp)
		return -ENOMEM;

	return 0;
}

/*
 * Unshare file descriptor table if it is being shared
 */
static int unshare_fd(unsigned long unshare_flags, struct files_struct **new_fdp)
{
	struct files_struct *fd = current->files;

	if ((unshare_flags & CLONE_FILES) &&
	    (fd && atomic_read(&fd->count) > 1)) {
		fd = dup_fd(fd, NULL);
		if (IS_ERR(fd))
			return PTR_ERR(fd);
		*new_fdp = fd;
	}

	return 0;
}

/*
 * unshare allows a process to 'unshare' part of the process
 * context which was originally shared using clone.  copy_*
 * functions used by kernel_clone() cannot be used here directly
 * because they modify an inactive task_struct that is being
 * constructed. Here we are modifying the current, active,
 * task_struct.
 */
int ksys_unshare(unsigned long unshare_flags)
{
	struct fs_struct *fs, *new_fs = NULL;
	struct files_struct *new_fd = NULL;
	struct cred *new_cred = NULL;
	struct nsproxy *new_nsproxy = NULL;
	int do_sysvsem = 0;
	int err;

	/*
	 * If unsharing a user namespace must also unshare the thread group
	 * and unshare the filesystem root and working directories.
	 */
	if (unshare_flags & CLONE_NEWUSER)
		unshare_flags |= CLONE_THREAD | CLONE_FS;
	/*
	 * If unsharing vm, must also unshare signal handlers.
	 */
	if (unshare_flags & CLONE_VM)
		unshare_flags |= CLONE_SIGHAND;
	/*
	 * If unsharing a signal handlers, must also unshare the signal queues.
	 */
	if (unshare_flags & CLONE_SIGHAND)
		unshare_flags |= CLONE_THREAD;
	/*
	 * If unsharing namespace, must also unshare filesystem information.
	 */
	if (unshare_flags & CLONE_NEWNS)
		unshare_flags |= CLONE_FS;

	err = check_unshare_flags(unshare_flags);
	if (err)
		goto bad_unshare_out;
	/*
	 * CLONE_NEWIPC must also detach from the undolist: after switching
	 * to a new ipc namespace, the semaphore arrays from the old
	 * namespace are unreachable.
	 */
	if (unshare_flags & (CLONE_NEWIPC|CLONE_SYSVSEM))
		do_sysvsem = 1;
	err = unshare_fs(unshare_flags, &new_fs);
	if (err)
		goto bad_unshare_out;
	err = unshare_fd(unshare_flags, &new_fd);
	if (err)
		goto bad_unshare_cleanup_fs;
	err = unshare_userns(unshare_flags, &new_cred);
	if (err)
		goto bad_unshare_cleanup_fd;
	err = unshare_nsproxy_namespaces(unshare_flags, &new_nsproxy,
					 new_cred, new_fs);
	if (err)
		goto bad_unshare_cleanup_cred;

	if (new_cred) {
		err = set_cred_ucounts(new_cred);
		if (err)
			goto bad_unshare_cleanup_cred;
	}

	if (new_fs || new_fd || do_sysvsem || new_cred || new_nsproxy) {
		if (do_sysvsem) {
			/*
			 * CLONE_SYSVSEM is equivalent to sys_exit().
			 */
			exit_sem(current);
		}
		if (unshare_flags & CLONE_NEWIPC) {
			/* Orphan segments in old ns (see sem above). */
			exit_shm(current);
			shm_init_task(current);
		}

		if (new_nsproxy)
			switch_task_namespaces(current, new_nsproxy);

		task_lock(current);

		if (new_fs) {
			fs = current->fs;
			read_seqlock_excl(&fs->seq);
			current->fs = new_fs;
			if (--fs->users)
				new_fs = NULL;
			else
				new_fs = fs;
			read_sequnlock_excl(&fs->seq);
		}

		if (new_fd)
			swap(current->files, new_fd);

		task_unlock(current);

		if (new_cred) {
			/* Install the new user namespace */
			commit_creds(new_cred);
			new_cred = NULL;
		}
	}

	perf_event_namespaces(current);

bad_unshare_cleanup_cred:
	if (new_cred)
		put_cred(new_cred);
bad_unshare_cleanup_fd:
	if (new_fd)
		put_files_struct(new_fd);

bad_unshare_cleanup_fs:
	if (new_fs)
		free_fs_struct(new_fs);

bad_unshare_out:
	return err;
}

SYSCALL_DEFINE1(unshare, unsigned long, unshare_flags)
{
	return ksys_unshare(unshare_flags);
}

/*
 *	Helper to unshare the files of the current task.
 *	We don't want to expose copy_files internals to
 *	the exec layer of the kernel.
 */

int unshare_files(void)
{
	struct task_struct *task = current;
	struct files_struct *old, *copy = NULL;
	int error;

	error = unshare_fd(CLONE_FILES, &copy);
	if (error || !copy)
		return error;

	old = task->files;
	task_lock(task);
	task->files = copy;
	task_unlock(task);
	put_files_struct(old);
	return 0;
}

int sysctl_max_threads(const struct ctl_table *table, int write,
		       void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int ret;
	int threads = max_threads;
	int min = 1;
	int max = MAX_THREADS;

	t = *table;
	t.data = &threads;
	t.extra1 = &min;
	t.extra2 = &max;

	ret = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	max_threads = threads;

	return 0;
}
