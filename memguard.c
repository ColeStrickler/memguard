/**
 * Memory bandwidth controller for multi-core systems
 *
 * Copyright (C) 2013  Heechul Yun <heechul@illinois.edu>
 *           (C) 2022  Heechul Yun <heechul.yun@ku.edu>
 *
 * This file is distributed under GPL v2 License. 
 * See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define DEBUG(x)
#define DEBUG_RECLAIM(x) x
#define DEBUG_USER(x)
#define DEBUG_PROFILE(x) x

/**************************************************************************
 * Included Files
 **************************************************************************/
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/smp.h> /* IPI calls */
#include <linux/irq_work.h>
#include <linux/hardirq.h>
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <asm/atomic.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/io.h>

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 0, 0)
#  include <uapi/linux/sched/types.h>
#elif LINUX_VERSION_CODE > KERNEL_VERSION(4, 13, 0)
#  include <linux/sched/types.h>
#elif LINUX_VERSION_CODE > KERNEL_VERSION(3, 8, 0)
#  include <linux/sched/rt.h>
#endif
#include <linux/sched.h>
#define FIRESIM
/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define CACHE_LINE_SIZE 64
#define BUF_SIZE 256
#define PREDICTOR 1  /* 0 - used, 1 - ewma(a=1/2), 2 - ewma(a=1/4) */

#define DEFAULT_RD_BUDGET_MB 1000
#define DEFAULT_WR_BUDGET_MB  500
#define DEFAULT_QMIN_MB       500
#define uint64_t __u64



#define WRITE_BOOL(addr, value)		(writeb(value, addr))
#define WRITE_UINT8(addr, value)	(writeb(value, addr))
#define WRITE_UINT16(addr, value)	(writew(value, addr))
#define WRITE_UINT32(addr, value)	(writel(value, addr))
#define WRITE_UINT64(addr, value)	(writeq(value, addr))

#define READ_BOOL(addr)		(readb(addr))
#define READ_UINT8(addr)	(readb(addr))
#define READ_UINT16(addr)	(readw(addr))
#define READ_UINT32(addr)	(readl(addr))
#define READ_UINT64(addr)	(readq(addr))

#ifdef FIRESIM

void __iomem* PLIC_physAddr = NULL;
void __iomem* L2CACHE_physAddr = NULL;
#define PLIC_BASE_OFFSET 0xC000000
#define PLIC_BASE_ADDR PLIC_physAddr // WE MUST MAP THIS INTO VIRTUAL ADDRESS SPACE
#define PLIC_MAX_OFFSET 0x3FFFFFC
#define L2CACHE_BASE_OFFSET 0x2010000
#define L2CACHE_MAX_OFFSET 0xfff
#define L2CACHE_BASE_ADDR L2CACHE_physAddr
#define L2CACHE_ACCESS_COUNTER_ADDR(core) ((L2CACHE_BASE_ADDR + 0x20 + (core * 0x8)))
#define L2CACHE_MISS_COUNTER_ADDR(core) ((L2CACHE_BASE_ADDR + 0x100 + (core * 0x8)))
#define L2CACHE_EN_COUNT_INSTFETCH_ADDR ((L2CACHE_BASE_ADDR + 0x300))
#define L2CACHE_EN_CORE_INTERRUPT_ADDR(core) ((L2CACHE_BASE_ADDR + 0x308 + (core * 0x8)))
#define L2CACHE_CORE_BUDGET_ADDR(core) ((L2CACHE_BASE_ADDR + 0X400 + (core * 8)))
#define L2CACHE_PERIOD_LEN_ADDR(core) ((L2CACHE_BASE_ADDR + 0x500 + (core * 8)))
#define L2CACHE_DEFAULT_PERIOD 0x1000
void PLIC_EnableSource(uint64_t base, int ctx, int src) 
{
    uint64_t addr = base + 0x2000 + (ctx*0x80);
    uint32_t currVal = READ_UINT32(addr);
    currVal |= (1 << ctx);
    WRITE_UINT32(addr, currVal);
}
void PLIC_SetSourcePriority(uint64_t base, int src, int priority)
{
    uint64_t addr = base + (src*0x4);
    WRITE_UINT32(addr, priority);
}
void PLIC_SetContextThreshold(uint64_t base, int ctx, int threshold)
{
    uint64_t addr = base + 0x200000 + (0x1000 * ctx);
    WRITE_UINT32(addr, threshold);
}
/*
    Only works for interrupt sources 0-31
*/
int PLIC_ReadPendingBit(uint64_t base, int src)
{
    uint64_t addr = base + 0x1000;
    uint32_t val = READ_UINT32(addr);
    return (val >> src) & 0x1;
}
void PLIC_ClearPendingBit(uint64_t base, int src)
{
    uint64_t addr = base + 0x1000;
    uint32_t val = READ_UINT32(addr);
    val &= ~(1 << src); // clear bit
    WRITE_UINT32(addr, val);
}

void PLIC_Setup(uint64_t base, int cpu)
{
	int pending = PLIC_ReadPendingBit(base, cpu+1);
	if (pending)
		PLIC_ClearPendingBit(base, cpu+1);
	PLIC_SetSourcePriority(base, cpu+1, 5);
	PLIC_SetContextThreshold(base, cpu, 0); // Let everything through > threshold 0=everything
	PLIC_EnableSource(base, cpu, cpu+1); // Sources start from 1 not 0
}

#endif

#if defined(__aarch64__) || defined(__arm__)
#  define PMU_LLC_MISS_COUNTER_ID 0x17   // LINE_REFILL
#  define PMU_LLC_WB_COUNTER_ID   0x18   // LINE_WB
#elif defined(__x86_64__) || defined(__i386__)
#  define PMU_LLC_MISS_COUNTER_ID 0x08b0 // OFFCORE_REQUESTS.ALL_DATA_RD
#  define PMU_LLC_WB_COUNTER_ID   0x40b0 // OFFCORE_REQUESTS.WB
#elif defined(__riscv)
// Note: These performance counters are specific to the T-Head C910.
// 		 They may not function correctly on other RISC-V designs.
#  define PMU_LLC_MISS_COUNTER_ID 0x11   // LL_CACHE_READ_MISS
#  define PMU_LLC_WB_COUNTER_ID   0x13   // LL_CACHE_WRITE_MISS
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 10, 0) // somewhere between 4.4-4.10
#  define TM_NS(x) (x)
#else
#  define TM_NS(x) (x).tv64
#endif

/**************************************************************************
 * Public Types
 **************************************************************************/
struct memstat{
	u64 used_read_budget;    /* used read budget */
	u64 used_write_budget;	 /* used write budget */
	u64 assigned_read_budget;  /* assigned read budget */
	u64 assigned_write_budget; /* assigned write budget */
	u64 throttled_time_ns;   
	int throttled;           /* throttled period count */
	u64 throttled_error;     /* throttled & error */
	int throttled_error_dist[10]; /* pct distribution */
	int exclusive;           /* exclusive period count */
	u64 exclusive_ns;        /* exclusive mode real-time duration */
	u64 exclusive_bw;        /* exclusive mode used bandwidth */
};

/* percpu info */
struct core_info {
#ifdef FIRESIM
	/* linux virtual IRQ # - Firesim only */
	int irq;
	int cpunum;
	int isThrottled;
#endif

	/* user configurations */
	int read_limit;          /* read limit mode */
	int write_limit;	 /* write limit mode */

	/* for control logic */
	int read_budget;         /* assigned read budget */
	int write_budget;        /* assigned write budged */

	int cur_read_budget;     /* currently available read budget */
	int cur_write_budget;    /* currently available write budget */

	struct task_struct * throttled_task;
	
	ktime_t throttled_time;  /* absolute time when throttled */

	u64 old_read_val;        /* hold previous read counter value */
	u64 old_write_val;	 /* hold previous write counter value */
	int prev_read_throttle_error;  /* check whether there was throttle error in 
				    the previous period for the read counter */
    	int prev_write_throttle_error; /* check whether there was throttle error in 
				    the previous period for the write counter */

	int exclusive_mode;      /* 1 - if in exclusive (best-effort) mode */
	ktime_t exclusive_time;  /* time when exclusive mode begins */

	struct irq_work	read_pending;  /* delayed work for NMIs */
	struct perf_event *read_event; /* PMC: LLC misses */

	struct irq_work write_pending;   /* delayed work for NMIs */
	struct perf_event *write_event;  /* PMC: LLC writebacks */
    
	struct task_struct *throttle_thread;  /* forced throttle idle thread */
	wait_queue_head_t throttle_evt; /* throttle wait queue */

	/* statistics */
	struct memstat overall;  /* stat for overall periods. reset by user */
	int read_used[3];        /* EWMA memory load */
	int write_used[3];	 /* EWMA memory load */
	int64_t period_cnt;      /* active periods count */

	int rtcore;              /* never throttle an rt core */
	/* per-core hr timer */
	struct hrtimer hr_timer;
};

/* global info */
struct memguard_info {
	ktime_t period_in_ktime;
	cpumask_var_t throttle_mask;
	cpumask_var_t active_mask;
};


/**************************************************************************
 * Global Variables
 **************************************************************************/
static struct memguard_info memguard_info;
static struct core_info __percpu *core_info;

static int g_period_us = 1000;

static int g_use_reclaim = 0;   /* 1 - enable reclaim of "guaranteed" bw */
static int g_use_exclusive = 0; /* 2 - spare bw sharing (rtas'13) 
				   5 - propotional sharing (tc'15) */
static int g_qmin = INT_MAX;
static int g_read_counter_id = PMU_LLC_MISS_COUNTER_ID;
static int g_write_counter_id = PMU_LLC_WB_COUNTER_ID; 

static struct dentry *memguard_dir;


/**************************************************************************
 * External Function Prototypes
 **************************************************************************/

/**************************************************************************
 * Local Function Prototypes
 **************************************************************************/
static void period_timer_callback_slave(struct core_info *cinfo);
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer);
static void memguard_read_process_overflow(struct irq_work *entry);
static void memguard_write_process_overflow(struct irq_work *entry);    
static int throttle_thread(void *arg);
static void memguard_on_each_cpu_mask(const struct cpumask *mask,
				      smp_call_func_t func,
				      void *info, bool wait);

/**************************************************************************
 * Module parameters
 **************************************************************************/

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 10, 0)
module_param(g_read_counter_id, hexint,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(g_write_counter_id, hexint,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
#else
module_param(g_read_counter_id, int,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(g_write_counter_id, int,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
#endif

module_param(g_period_us, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_period_us, "throttling period in usec");

/**************************************************************************
 * Module main code
 **************************************************************************/
/* similar to on_each_cpu_mask(), but this must be called with IRQ disabled */
static void memguard_on_each_cpu_mask(const struct cpumask *mask, 
				      smp_call_func_t func,
				      void *info, bool wait)
{
	int cpu = smp_processor_id();
	smp_call_function_many(mask, func, info, wait);
	if (cpumask_test_cpu(cpu, mask)) {
		func(info);
	}
}

/** convert MB/s to #of events (i.e., LLC miss counts) per 1ms */
static inline u64 convert_mb_to_events(int mb)
{
	return div64_u64((u64)mb*1024*1024,
			 CACHE_LINE_SIZE * (1000000/g_period_us));
}
static inline int convert_events_to_mb(u64 events)
{
	int divisor = g_period_us*1024*1024;
	int mb = div64_u64(events*CACHE_LINE_SIZE*1000000 + (divisor-1), divisor);
	return mb;
}

static inline void print_current_context(void)
{
	trace_printk("in_interrupt(%ld)(hard(%ld),softirq(%d)"
		     ",in_nmi(%d)),irqs_disabled(%d)\n",
		     in_interrupt(), in_irq(), (int)in_softirq(),
		     (int)in_nmi(), (int)irqs_disabled());
}

/** read current counter value. */
static inline u64 perf_event_count(struct perf_event *event)
{
	return local64_read(&event->count) + 
		atomic64_read(&event->child_count);
}

/** return used event in the current period */
static inline u64 memguard_read_event_used(struct core_info *cinfo)
{
#ifdef FIRESIM
	return READ_UINT64(L2CACHE_MISS_COUNTER_ADDR(smp_processor_id()));
#else
	return perf_event_count(cinfo->read_event) - cinfo->old_read_val;
#endif
}

/** return used event in the current period */
static inline u64 memguard_write_event_used(struct core_info *cinfo)
{
#ifdef FIRESIM
	return 0;
#else
	return perf_event_count(cinfo->write_event) - cinfo->old_write_val;
#endif
}

static void print_core_info(int cpu, struct core_info *cinfo)
{
	pr_info("CPU%d: budget: %d, cur_budget: %d, period: %ld\n", 
		cpu, cinfo->read_budget, cinfo->cur_read_budget, (long)cinfo->period_cnt);
}

/**
 * update per-core usage statistics
 */
void update_statistics(struct core_info *cinfo)
{
	/* counter must be stopped by now. */
	s64 read_new, write_new;
	int read_used, write_used;



/*
	We will probably wanna alter this eventually
*/
#ifndef FIRESIM
	read_new = perf_event_count(cinfo->read_event);
	read_used = (int)(read_new - cinfo->old_read_val); 
	write_new = perf_event_count(cinfo->write_event);
	write_used = (int)(write_new - cinfo->old_write_val); 
#else
	read_used = memguard_read_event_used(cinfo);
	write_used = memguard_write_event_used(cinfo);
#endif
	cinfo->old_read_val = read_new;
	cinfo->overall.used_read_budget += read_used;
	cinfo->overall.assigned_read_budget += cinfo->read_budget;
	
	

	cinfo->old_write_val = write_new;
	cinfo->overall.used_write_budget += write_used;
	cinfo->overall.assigned_write_budget += cinfo->write_budget;

	/* EWMA filtered per-core usage statistics */
	cinfo->read_used[0] = read_used;
	cinfo->read_used[1] = (cinfo->read_used[1] * (2-1) + read_used) >> 1; 
	/* used[1]_k = 1/2 used[1]_k-1 + 1/2 used */
	cinfo->read_used[2] = (cinfo->read_used[2] * (4-1) + read_used) >> 2; 
	/* used[2]_k = 3/4 used[2]_k-1 + 1/4 used */
	
	/* EWMA filtered per-core usage statistics */
	cinfo->write_used[0] = write_used;
	cinfo->write_used[1] = (cinfo->write_used[1] * (2-1) + write_used) >> 1; 
	/* used[1]_k = 1/2 used[1]_k-1 + 1/2 used */
	cinfo->write_used[2] = (cinfo->write_used[2] * (4-1) + write_used) >> 2;                                          
	/* used[2]_k = 3/4 used[2]_k-1 + 1/4 used */

	/* core is currently throttled. */
	if (cinfo->throttled_task) {
		cinfo->overall.throttled_time_ns +=
			(TM_NS(ktime_get()) - TM_NS(cinfo->throttled_time));
		cinfo->overall.throttled++;
	}

	/* throttling error condition:
	   I was too aggressive in giving up "unsed" budget */
	if (cinfo->prev_read_throttle_error && read_used < cinfo->read_budget) {
		int diff = cinfo->read_budget - read_used;
		int idx;
		cinfo->overall.throttled_error ++; // += diff;
		BUG_ON(cinfo->read_budget == 0);
		idx = (int)(diff * 10 / cinfo->read_budget);
		cinfo->overall.throttled_error_dist[idx]++;
		trace_printk("ERR: read_throttled_error: %d < %d\n", read_used, cinfo->read_budget);
		/* compensation for error to catch-up*/
		cinfo->read_used[PREDICTOR] = cinfo->read_budget + diff;
	}
	cinfo->prev_read_throttle_error = 0;
    
    	if (cinfo->prev_write_throttle_error && write_used < cinfo->write_budget) {
		int diff = cinfo->write_budget - write_used;
		int idx;
		cinfo->overall.throttled_error ++; // += diff;
		BUG_ON(cinfo->write_budget == 0);
		idx = (int)(diff * 10 / cinfo->write_budget);
		cinfo->overall.throttled_error_dist[idx]++;
		trace_printk("ERR: write_throttled_error: %d < %d\n", write_used, cinfo->write_budget);
		/* compensation for error to catch-up*/
		cinfo->write_used[PREDICTOR] = cinfo->write_budget + diff;
	}
	cinfo->prev_write_throttle_error = 0;           

	/* I was the lucky guy who used the DRAM exclusively */
	if (cinfo->exclusive_mode) {
		u64 exclusive_ns, exclusive_bw;

		/* used time */
		exclusive_ns = (TM_NS(ktime_get()) -
				TM_NS(cinfo->exclusive_time));
		
		/* used bw */
		exclusive_bw = (cinfo->read_used[0] - cinfo->read_budget);

		cinfo->overall.exclusive_ns += exclusive_ns;
		cinfo->overall.exclusive_bw += exclusive_bw;
		cinfo->exclusive_mode = 0;
		cinfo->overall.exclusive++;
	}
	DEBUG_PROFILE(trace_printk("used: %d %d read: %d %d write: %d %d period: %ld\n",
				   read_used, write_used,
				   cinfo->read_budget,
				   cinfo->cur_read_budget,
				   cinfo->write_budget,
				   cinfo->cur_write_budget,
				   (long) cinfo->period_cnt));
}


/**
 * budget is used up. PMU generate an interrupt
 * this run in hardirq, nmi context with irq disabled
 * 
 * this is called from kernel/events/core.c in the function:
 *  static int __perf_event_overflow(struct perf_event *event,
 *			 int throttle, struct perf_sample_data *data,
 *			 struct pt_regs *
 * 
 */
static void event_overflow_callback(struct perf_event *event,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
				    int nmi,
#endif
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo);
	irq_work_queue(&cinfo->read_pending);
}

static void event_write_overflow_callback(struct perf_event *event,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
				    int nmi,
#endif
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo);
	irq_work_queue(&cinfo->write_pending);
}


/**
 * called by process_overflow
 */
static void __unthrottle_core(void *info)
{
	struct memguard_info *global = &memguard_info;
	struct core_info *cinfo = this_cpu_ptr(core_info);
	if (cinfo->throttled_task) {
		cinfo->exclusive_mode = 1;
		cinfo->exclusive_time = ktime_get();
#ifdef FIRESIM
		// Re-enable interrupts
		// WRITE_BOOL(L2CACHE_EN_CORE_INTERRUPT_ADDR(smp_processor_id()), true);
#endif
		cinfo->throttled_task = NULL;
		DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
	}
	// We added this - not sure why?
	// cpumask_clear_cpu(cinfo->cpunum, global->throttle_mask);
	
}

static void __newperiod(void *info)
{
	// zero counters for new period
	WRITE_BOOL(L2CACHE_PERIOD_LEN_ADDR(smp_processor_id()), true);
	ktime_t start = ktime_get();
	struct memguard_info *global = &memguard_info;
	struct core_info *cinfo = this_cpu_ptr(core_info);
	
	ktime_t new_expire = ktime_add(start, global->period_in_ktime);

	/* arrived before timer interrupt is called */
	hrtimer_start_range_ns(&cinfo->hr_timer, new_expire,
			       0, HRTIMER_MODE_ABS_PINNED);

	DEBUG(trace_printk("begin new period\n"));
	period_timer_callback_slave(cinfo);
}

/**
 * memory overflow handler.
 * must not be executed in NMI context. but in hard irq context
 * 
 * 
 * We enqueue this function as IRQ_WORK, set up in driver_init
 * 
 * 
 */
static void memguard_read_process_overflow(struct irq_work *entry)
{
	
	struct core_info *cinfo = this_cpu_ptr(core_info);
	//pr_info("READ PROCESS OVERFLOW CPU %d accesses: %llu\n", cinfo->cpunum, READ_UINT64(L2CACHE_MISS_COUNTER_ADDR(cinfo->cpunum)));
	struct memguard_info *global = &memguard_info;
	ktime_t start;
	s64 read_budget_used;//, write_budget_used;

	start = ktime_get();

	BUG_ON(in_nmi() || !in_irq());

	read_budget_used = memguard_read_event_used(cinfo);

	/* erroneous overflow, that could have happend before period timer
	   stop the pmu */
	if (read_budget_used < cinfo->cur_read_budget) {
		trace_printk("ERR: used %lld < cur_budget %d. ignore\n",
			     read_budget_used, cinfo->cur_read_budget);
		return;
	}

	if (g_use_reclaim) {
		int amount, i;
		/* reclaim back the budget I donated (if exist) */
		if (cinfo->cur_read_budget < cinfo->read_budget) {
			amount = cinfo->read_budget - cinfo->cur_read_budget;
			cinfo->cur_read_budget += amount;
#ifndef FIRESIM
			local64_set(&cinfo->read_event->hw.period_left, amount);
#endif
			DEBUG_RECLAIM(trace_printk("locally reclaimed %d\n",
						   amount));
			return;
		}
		/* try to reclaim from the global pool */
		amount = 0;
		for_each_online_cpu(i) {
			struct core_info *ci = per_cpu_ptr(core_info, i);
			if (i == smp_processor_id())
				continue;
			if (ci->cur_read_budget < ci->read_budget) {
				/* reclaim other core's donated budget */
				int tmp = ci->read_budget - ci->cur_read_budget;
				ci->cur_read_budget += tmp;
				amount += tmp;
			}
			if (amount > g_qmin)
				break;
		}
		if (amount > 0) {
#ifndef FIRESIM
			local64_set(&cinfo->read_event->hw.period_left, amount);
			DEBUG_RECLAIM(trace_printk("globally reclaimed %d\n",
						   amount));
#endif
			return;
		}
	}

    /* no more overflow interrupt -->  Will get reset on new period */
#ifdef FIRESIM
	// we should already disable this in hardware, no need to do this here
	//WRITE_BOOL(L2CACHE_EN_CORE_INTERRUPT_ADDR(cinfo->cpunum), false);
#else
	local64_set(&cinfo->read_event->hw.period_left, 0xfffffff);
#endif

	/* check if we donated too much */
	if (read_budget_used < cinfo->read_budget) {
		trace_printk("ERR: throttling error\n");
		cinfo->prev_read_throttle_error = 1;
	}

	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		trace_printk("ERR: not active\n");
		cinfo->throttled_task = NULL;
		return;
	}


//#ifdef FIRESIM
//	/*
//		If throttle mask is set and we get another interrupt, it means
//		that this is a new period and we unthrottle the CPU
//	*/
//	if (cpumask_test_cpu(smp_processor_id(), global->throttle_mask))
//	{
//		
//		memguard_on_each_cpu_mask(global->throttle_mask, __unthrottle_core, NULL, 0);
//		
//		// zero throttle mask? 
//	}
//#endif
//



	/* we are going to be throttled */
	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);
	smp_mb(); // w -> r ordering of the local cpu.

	if (cpumask_equal(global->throttle_mask, global->active_mask)) {
		/* all other cores are alreay throttled */
		if (g_use_exclusive == 2) {
			/* SP: wakeup all (no regulation until next period) */
			memguard_on_each_cpu_mask(global->throttle_mask, __unthrottle_core, NULL, 0);
			return;
		} else if (g_use_exclusive == 5) {
			/* PS: begin a new period */
			memguard_on_each_cpu_mask(global->active_mask, __newperiod, NULL, 0);
			return;
		} else if (g_use_exclusive > 5) {
			trace_printk("ERR: Unsupported exclusive mode %d\n", 
				     g_use_exclusive);
			return;
		} else if (g_use_exclusive != 0 &&
			   cpumask_weight(global->active_mask) == 1) {
			trace_printk("ERR: don't throttle one active core\n");
			return;
		}
	}

	if (cinfo->prev_read_throttle_error)
		return;
	/*
	 * fail to reclaim. now throttle this core
	 */
	DEBUG_RECLAIM(trace_printk("fail to reclaim after %lld nsec. throttle %s\n",
				   TM_NS(ktime_get()) - TM_NS(start), current->comm));

	/* wake-up throttle task */
	cinfo->throttled_task = current;
	cinfo->throttled_time = start;

	// WARN_ON_ONCE(!strncmp(current->comm, "swapper", 7));
	wake_up_interruptible(&cinfo->throttle_evt);
}


static void memguard_write_process_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;
	ktime_t start;
	s64 write_budget_used;

	start = ktime_get();

	BUG_ON(in_nmi() || !in_irq());
    
	write_budget_used = memguard_write_event_used(cinfo);
    
	if (write_budget_used < cinfo->cur_write_budget)
	{
		trace_printk("ERR: overflow in write timer. used %lld < cur_budget %d. ignore\n",
			     write_budget_used, cinfo->cur_write_budget);
		return;
	}

	if (g_use_reclaim) {
		int amount, i;
		/* reclaim back the budget I donated (if exist) */
		if (cinfo->cur_write_budget < cinfo->write_budget) {
			amount = cinfo->write_budget - cinfo->cur_write_budget;
			cinfo->cur_write_budget += amount;
			local64_set(&cinfo->write_event->hw.period_left, amount);
			DEBUG_RECLAIM(trace_printk("locally reclaimed %d\n",
						   amount));
			return;
		}
		/* try to reclaim from the global pool */
		amount = 0;
		for_each_online_cpu(i) {
			struct core_info *ci = per_cpu_ptr(core_info, i);
			if (i == smp_processor_id())
				continue;
			if (ci->cur_write_budget < ci->write_budget) {
				/* reclaim other core's donated budget */
				int tmp = ci->write_budget - ci->cur_write_budget;
				ci->cur_write_budget += tmp;
				amount += tmp;
			}
			if (amount > g_qmin)
				break;
		}
		if (amount > 0) {
			local64_set(&cinfo->write_event->hw.period_left, amount);
			DEBUG_RECLAIM(trace_printk("globally reclaimed %d\n",
						   amount));
			return;
		}
	}

#ifndef FIRESIM // no perf events created
	local64_set(&cinfo->write_event->hw.period_left, 0xfffffff);
#endif
	if (write_budget_used < cinfo->write_budget) {
		trace_printk("ERR: throttling error\n");
		cinfo->prev_write_throttle_error = 1;
	}

	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		trace_printk("ERR: not active\n");
		cinfo->throttled_task = NULL;
		return;
	}

	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);
	smp_mb(); // w -> r ordering of the local cpu.
	if (cpumask_equal(global->throttle_mask, global->active_mask)) {
		if (g_use_exclusive == 2) {
			/* SP: wakeup all (no regulation until next period) */
			memguard_on_each_cpu_mask(global->throttle_mask, __unthrottle_core, NULL, 0);
			return;
		} else if (g_use_exclusive == 5) {
			/* PS: begin a new period */
			memguard_on_each_cpu_mask(global->active_mask, __newperiod, NULL, 0);
			return;
		} else if (g_use_exclusive > 5) {
			trace_printk("ERR: Unsupported exclusive mode %d\n", 
				     g_use_exclusive);
			return;
		} else if (g_use_exclusive != 0 &&
			   cpumask_weight(global->active_mask) == 1) {
			trace_printk("ERR: don't throttle one active core\n");
			return;
		}
	}

	if (cinfo->prev_write_throttle_error)
		return;
	
	DEBUG_RECLAIM(trace_printk("fail to reclaim after %lld nsec. throttle %s\n",
				   TM_NS(ktime_get()) - TM_NS(start), current->comm));

	cinfo->throttled_task = current;
	cinfo->throttled_time = start;

	// WARN_ON_ONCE(!strncmp(current->comm, "swapper", 7));
	wake_up_interruptible(&cinfo->throttle_evt);
}

/**
 * per-core period timer callback
 *
 *   called while cpu_base->lock is held by hrtimer_interrupt()
 *
 */
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;
	int orun;

	/* must be irq disabled. hard irq */
	BUG_ON(!irqs_disabled());
	// WARN_ON_ONCE(!in_interrupt());

#ifndef FIRESIM
	/* stop counter */
	cinfo->read_event->pmu->stop(cinfo->read_event, PERF_EF_UPDATE);
	cinfo->write_event->pmu->stop(cinfo->write_event, PERF_EF_UPDATE);
#else
	// stop counter
	WRITE_BOOL(L2CACHE_PERIOD_LEN_ADDR(smp_processor_id()), true);
#endif
	/* forward timer */
	orun = hrtimer_forward_now(timer, global->period_in_ktime);
	BUG_ON(orun == 0);
	if (orun > 1)
		trace_printk("ERR: timer overrun %d at period %ld\n",
			     orun, (long)cinfo->period_cnt);

	/* assign local period */
	cinfo->period_cnt += orun;

	period_timer_callback_slave(cinfo);

	return HRTIMER_RESTART;
}

/**
 * period_timer algorithm:
 *	excess = 0;
 *	if predict < budget:
 *	   excess = budget - predict;
 *	   global += excess
 *	set interrupt at (budget - excess)
 *
 */
static void period_timer_callback_slave(struct core_info *cinfo)
{
	struct memguard_info *global = &memguard_info;
	int cpu = smp_processor_id();

	/* I'm actively participating */
	cpumask_clear_cpu(cpu, global->throttle_mask);
	smp_mb();
	cpumask_set_cpu(cpu, global->active_mask);

	/* update statistics. */
	update_statistics(cinfo);

	/* new budget assignment from user */
	if (cinfo->read_limit > 0)
		cinfo->read_budget = max(cinfo->read_limit, 1);
	if (cinfo->write_limit > 0)
		cinfo->write_budget = max(cinfo->write_limit, 1);
#ifndef FIRESIM
	if (cinfo->read_event->hw.sample_period != cinfo->read_budget) {
		/* new budget is assigned */
		trace_printk("MSG: new budget %d is assigned\n", 
				   cinfo->read_budget);
		cinfo->read_event->hw.sample_period = cinfo->read_budget;
	}
	if (cinfo->write_event->hw.sample_period != cinfo->write_budget) {
		/* new budget is assigned */
		trace_printk("MSG: new write budget %d is assigned\n", 
				   cinfo->write_budget);
		cinfo->write_event->hw.sample_period = cinfo->write_budget;
	}
#endif

	/* per-task donation policy */
        if (g_use_reclaim) {
		/* donate 'expected surplus' ahead of time. */
		int sur_rd, sur_wr;
		sur_rd = max(cinfo->read_budget - cinfo->read_used[PREDICTOR], 0);
		cinfo->cur_read_budget = cinfo->read_budget - sur_rd;
		sur_wr = max(cinfo->write_budget - cinfo->write_used[PREDICTOR], 0);
		cinfo->cur_write_budget = cinfo->write_budget - sur_wr;
		DEBUG_RECLAIM(trace_printk("donated %d %d\n", sur_rd, sur_wr));
        } else {
		cinfo->cur_read_budget = cinfo->read_budget;
		cinfo->cur_write_budget = cinfo->write_budget;
        }

	/* unthrottle tasks (if any) */
	cinfo->throttled_task = NULL;

#ifndef FIRESIM
        /* setup overflow interrupts except RT cores */
	if (!cinfo->rtcore) {
		/* setup an interrupt */
		local64_set(&cinfo->read_event->hw.period_left, cinfo->cur_read_budget);
		local64_set(&cinfo->write_event->hw.period_left, cinfo->cur_write_budget);
	}

	/* enable performance counter */
	cinfo->read_event->pmu->start(cinfo->read_event, PERF_EF_RELOAD);
	cinfo->write_event->pmu->start(cinfo->write_event, PERF_EF_RELOAD);
#else
	
	WRITE_BOOL(L2CACHE_PERIOD_LEN_ADDR(smp_processor_id()), false);
#endif
}

static struct perf_event *init_counter(int cpu, int budget, int counter_id, void *callback)
{

#ifdef FIRESIM // we do not have to deal with callbacks
	WRITE_UINT64(L2CACHE_CORE_BUDGET_ADDR(cpu), budget);
	pr_info("Wrote initial budget of %llu for CPU %d\n", (int)READ_UINT64(L2CACHE_CORE_BUDGET_ADDR(cpu)), cpu);
#else
	struct perf_event *event = NULL;
	struct perf_event_attr sched_perf_hw_attr = {
		.type		= PERF_TYPE_RAW,
		.size		= sizeof(struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
		.config         = counter_id,
		.sample_period  = budget, 
		.exclude_kernel = 1,   /* TODO: 1 mean, no kernel mode counting */
	};
	/* Try to register using hardware perf events */
	event = perf_event_create_kernel_counter(
		&sched_perf_hw_attr,
		cpu, NULL,
		callback
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0)
		, NULL
#endif
		);

	if (!event)
		return NULL;

	if (IS_ERR(event)) {
		/* vary the KERN level based on the returned errno */
		if (PTR_ERR(event) == -EOPNOTSUPP)
			pr_info("cpu%d. not supported\n", cpu);
		else if (PTR_ERR(event) == -ENOENT)
			pr_info("cpu%d. not h/w event\n", cpu);
		else
			pr_err("cpu%d. unable to create perf event: %ld\n",
			       cpu, PTR_ERR(event));
		return NULL;
	}
#endif


	/* success path */
	pr_info("cpu%d enabled counter 0x%x\n", cpu, counter_id);
#ifdef FIRESIM
	return NULL;
#else
	return event;
#endif
}

static void __start_counter(void *info)
{
	struct memguard_info *global = &memguard_info;
	struct core_info *cinfo = this_cpu_ptr(core_info);
#ifndef FIRESIM
	BUG_ON(!cinfo->read_event);
        BUG_ON(!cinfo->write_event);
#endif

	/* initialize hr timer */
        hrtimer_init(&cinfo->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
        cinfo->hr_timer.function = &period_timer_callback_master;

	/* start timer */
        hrtimer_start(&cinfo->hr_timer, global->period_in_ktime,
                      HRTIMER_MODE_REL_PINNED);

	/* initialize */
	cinfo->throttled_task = NULL;
	cinfo->period_cnt = 0;

	/* start performance counter */
	/* cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD); */
}

static void __stop_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo->read_event);
	BUG_ON(!cinfo->write_event);
	
	/* stop the kthrottle/i */
	cinfo->throttled_task = NULL;
	cinfo->period_cnt = -1; // done

	/* stop the counter */
	cinfo->read_event->pmu->stop(cinfo->read_event, PERF_EF_UPDATE);
	cinfo->write_event->pmu->stop(cinfo->write_event, PERF_EF_UPDATE);

	/* stop timer */
	hrtimer_cancel(&cinfo->hr_timer);
}


/**************************************************************************
 * Local Functions
 **************************************************************************/

static ssize_t memguard_control_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
		return 0;
	if (!strncmp(p, "rt ", 3)) {
		int i, val;
		sscanf(p+3, "%d %d", &i, &val);
		if (i >=0 && i < num_online_cpus())
		{
			// if real time core, disable interrupts
			per_cpu_ptr(core_info, i)->rtcore = val;
			if (!val)
				WRITE_BOOL(L2CACHE_EN_CORE_INTERRUPT_ADDR(i), true); // Enable interrupts on core
			else
				WRITE_BOOL(L2CACHE_EN_CORE_INTERRUPT_ADDR(i), false); 
		}
			
	} else if (!strncmp(p, "reclaim ", 8))
		sscanf(p+8, "%d", &g_use_reclaim);
	else if (!strncmp(p, "exclusive ", 10))
		sscanf(p+10, "%d", &g_use_exclusive);
	else
		pr_info("ERROR: %s\n", p);
	return cnt;
}

static int memguard_control_show(struct seq_file *m, void *v)
{
	struct memguard_info *global = &memguard_info;
	char buf[BUF_SIZE];
	seq_printf(m, "reclaim: %d\n", g_use_reclaim);
	seq_printf(m, "exclusive: %d\n", g_use_exclusive);
	cpumap_print_to_pagebuf(1, buf, global->active_mask);
	seq_printf(m, "active: %s\n", buf);
	cpumap_print_to_pagebuf(1, buf, global->throttle_mask);	
	seq_printf(m, "throttle: %s\n", buf);
	return 0;
}

static int memguard_control_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_control_show, NULL);
}

static const struct file_operations memguard_control_fops = {
	.open		= memguard_control_open,
	.write          = memguard_control_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static void __update_budget(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);

	if ((unsigned long)info == 0) {
		pr_info("ERR: Requested budget is zero\n");
		return;
	}

#ifdef FIRESIM
	WRITE_UINT64(L2CACHE_CORE_BUDGET_ADDR(smp_processor_id()), (unsigned long)info);
#endif
	cinfo->read_limit = (unsigned long)info;
	pr_info("Budget update on %d of %d\n",smp_processor_id(), cinfo->read_limit);
	DEBUG_USER(trace_printk("MSG: New read budget of Core%d is %d\n",
				smp_processor_id(), cinfo->read_budget));

}

static void __update_write_budget(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	
	if ((unsigned long)info == 0) {
		pr_info("ERR: Requested budget is zero\n");
		return;
	}
#ifdef FIRESIM
	WRITE_UINT64(L2CACHE_CORE_BUDGET_ADDR(smp_processor_id()), (unsigned long)info);
#endif
	cinfo->write_limit = (unsigned long)info;
	DEBUG_USER(trace_printk("MSG: New write budget of Core%d is %d\n",
				smp_processor_id(), cinfo->write_budget));

}
static ssize_t memguard_read_limit_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	int i;
	int use_mb = 0;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0) 
		return 0;

	if (!strncmp(p, "mb ", 3)) {
		use_mb = 1;
		p+=3;
	}
	for_each_online_cpu(i) {
		int input;
		unsigned long events;
		sscanf(p, "%d", &input);
		if (input == 0) {
			pr_err("ERR: CPU%d: input is zero: %s.\n",i, p);
			continue;
		}
		if (use_mb)
			events = (unsigned long)convert_mb_to_events(input);
		else
			events = input;

		pr_info("CPU%d: New budget=%ld (%d %s)\n", i, 
			events, input, (use_mb)?"MB/s": "events");
		smp_call_function_single(i, __update_budget,
					 (void *)events, 0);
		
		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}
	return cnt;
}

static int memguard_read_limit_show(struct seq_file *m, void *v)
{
	int i, cpu;
	cpu = get_cpu();

	seq_printf(m, "cpu  |budget (MB/s)\t RT?\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0;
		if (cinfo->read_limit > 0)
			budget = cinfo->read_limit;

		WARN_ON_ONCE(budget == 0);
		seq_printf(m, "CPU%d: %d (%dMB/s)\t %s\n", 
			   i, budget, convert_events_to_mb(budget),
			   cinfo->rtcore?"Yes":"No");
	}

	put_cpu();
	return 0;
}

static int memguard_read_limit_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_read_limit_show, NULL);
}

static const struct file_operations memguard_read_limit_fops = {
	.open		= memguard_read_limit_open,
	.write          = memguard_read_limit_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};



/**
 * Display usage statistics
 *
 * TODO: use IPI
 */
static int memguard_usage_show(struct seq_file *m, void *v)
{
	int i, j;

	/* current utilization */
	for (j = 0; j < 3; j++) {
		seq_printf(m, "EWMA[%d]: ", j);
		for_each_online_cpu(i) {
			struct core_info *cinfo = per_cpu_ptr(core_info, i);
			u64 budget, used, util;

			budget = cinfo->read_budget;
			used = cinfo->read_used[j];
			util = div64_u64(used * 100, (budget) ? budget : 1);
			seq_printf(m, "%llu ", util);
		}
		seq_printf(m, "\n");
	}
	seq_printf(m, "<overall>----\n");

	/* overall utilization
	   WARN: assume budget did not changed */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		u64 total_budget, total_used, result;

		total_budget = cinfo->overall.assigned_read_budget;
		total_used   = cinfo->overall.used_read_budget;
		result       = div64_u64(total_used * 100, 
					 (total_budget) ? total_budget : 1 );
		seq_printf(m, "%lld ", result);
	}
        seq_printf(m, "\n");
	return 0;
}

static int memguard_usage_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_usage_show, NULL);
}

static const struct file_operations memguard_usage_fops = {
	.open		= memguard_usage_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static ssize_t memguard_write_limit_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	int i;
	int use_mb = 0;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0) 
		return 0;

	if (!strncmp(p, "mb ", 3)) {
		use_mb = 1;
		p+=3;
	}
	for_each_online_cpu(i) {
		int input;
		unsigned long events;
		sscanf(p, "%d", &input);
		if (input == 0) {
			pr_err("ERR: CPU%d: input is zero: %s.\n",i, p);
			continue;
		}
		if (use_mb)
			events = (unsigned long)convert_mb_to_events(input);
		else
			events = input;

		pr_info("CPU%d: New budget=%ld (%d %s)\n", i, 
			events, input, (use_mb)?"MB/s": "events");
		smp_call_function_single(i, __update_write_budget,
					 (void *)events, 0);
		
		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}
	return cnt;
}

static int memguard_write_limit_show(struct seq_file *m, void *v)
{
	int i, cpu;
	cpu = get_cpu();

	seq_printf(m, "cpu  |budget (MB/s)\t RT?\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0;
		if (cinfo->write_limit > 0)
			budget = cinfo->write_limit;

		WARN_ON_ONCE(budget == 0);
		seq_printf(m, "CPU%d: %d (%dMB/s) %s\n", 
			   i, budget, convert_events_to_mb(budget),
			   cinfo->rtcore?"Yes":"No");
	}

	put_cpu();
	return 0;
}

static int memguard_write_limit_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_write_limit_show, NULL);
}

static const struct file_operations memguard_write_limit_fops = {
	.open		= memguard_write_limit_open,
	.write          = memguard_write_limit_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
static int memguard_init_debugfs(void)
{

	memguard_dir = debugfs_create_dir("memguard", NULL);
	BUG_ON(!memguard_dir);
	debugfs_create_file("control", 0444, memguard_dir, NULL,
			    &memguard_control_fops);

	debugfs_create_file("read_limit", 0444, memguard_dir, NULL,
			    &memguard_read_limit_fops);
				
	debugfs_create_file("write_limit", 0444, memguard_dir, NULL,
			    &memguard_write_limit_fops);

	debugfs_create_file("usage", 0666, memguard_dir, NULL,
			    &memguard_usage_fops);
	return 0;
}

static int throttle_thread(void *arg)
{
	int cpunr = (unsigned long)arg;
	struct core_info *cinfo = per_cpu_ptr(core_info, cpunr);

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 9, 0)
	sched_set_fifo(current);
#else
	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO/2,
	};

	sched_setscheduler(current, SCHED_FIFO, &param);
#endif

	while (!kthread_should_stop() && cpu_online(cpunr)) {

		DEBUG(trace_printk("wait an event\n"));
		wait_event_interruptible(cinfo->throttle_evt,
					 cinfo->throttled_task ||
					 kthread_should_stop());

		//DEBUG(trace_printk("got an event\n"));
		trace_printk("got an event\n");
		if (kthread_should_stop())
			break;

		while (cinfo->throttled_task && !kthread_should_stop())
		{
			smp_mb();
			cpu_relax();
			/* TODO: mwait */
		}
	}

	DEBUG(trace_printk("exit\n"));
	return 0;
}


#ifdef FIRESIM
typedef irqreturn_t (*hwirq_handler)(int, void *);
int RegisterIRQ(struct core_info* cinfo, int cpunum, hwirq_handler handler)
{
    struct device_node *node;
    int ret = 0;
	int irq = 0;
    int online_cpus = num_online_cpus();
    // Locate device node from the device tree
    node = of_find_compatible_node(NULL, NULL, "sifive,inclusivecache0");
    if (!node) {
        pr_err("Failed to find device node\n");
        return -ENODEV;
    }

    irq = of_irq_get(node, cpunum);  // 0 = first interrupt in "interrupts" property
    if (irq < 0) {
        pr_err("Failed to get IRQ from device tree\n");
        return irq;
    }
    pr_info("Mapped hwirq to Linux IRQ: %d for cpu %d\n", irq, cpunum);

    ret = request_irq(irq, handler, 0, "memguard", NULL);
    if (ret) {
        printk(KERN_ERR "Failed to request IRQ %d --> %d\n", irq, ret);
        return ret;
    }
	
	cinfo->irq = irq; // This is the virtual
	pr_info("Successfully requested Linux IRQ %d\n", irq);
	return 0; 
}


/*
	This does the same thing as event_overflow_callback()
*/
static irqreturn_t my_irq_handler(int irq, void *dev_id) {
    struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo);
	irq_work_queue(&cinfo->read_pending);
    return IRQ_HANDLED; // Return IRQ_HANDLED to indicate that it was handled
}
#endif


int init_module( void )
{
	int i;

	struct memguard_info *global = &memguard_info;
#ifdef FIRESIM
	PLIC_physAddr = ioremap(PLIC_BASE_OFFSET, PLIC_MAX_OFFSET);
	L2CACHE_physAddr = ioremap(L2CACHE_BASE_OFFSET, L2CACHE_MAX_OFFSET);

	if (!PLIC_physAddr || ! L2CACHE_physAddr)
	{
		printk(KERN_INFO "Could not map L2 or PLIC MMIO registers\n");
		return -ENODEV;
	}
#endif


	/* initialized memguard_info structure */
	memset(global, 0, sizeof(struct memguard_info));
	zalloc_cpumask_var(&global->throttle_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&global->active_mask, GFP_NOWAIT);
	if (g_period_us < 0 || g_period_us > 1000000) {
		printk(KERN_INFO "Must be 0 < period < 1 sec\n");
		return -ENODEV;
	}

	global->period_in_ktime = ktime_set(0, g_period_us * 1000);

	/* initialize all online cpus to be active */
	cpumask_copy(global->active_mask, cpu_online_mask);

	pr_info("NR_CPUS: %d, online: %d\n", NR_CPUS, num_online_cpus());
	if (g_read_counter_id >= 0)
		pr_info("RAW HW READ COUNTER ID: 0x%x\n", g_read_counter_id);
	if (g_write_counter_id >= 0)
		pr_info("RAW HW WRITE COUNTER ID: 0x%x\n", g_write_counter_id);	
	pr_info("HZ=%d, g_period_us=%d\n", HZ, g_period_us);

	g_qmin = convert_mb_to_events(DEFAULT_QMIN_MB); // default 1000MB/s

	pr_info("Initilizing perf counter\n");
	core_info = alloc_percpu(struct core_info);

	for_each_online_cpu(i) {
		
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int read_budget, write_budget;


#ifdef FIRESIM
		int cpunum = i;
		pr_info("RegisterIRQ() for cpu %d\n", i);
		if (RegisterIRQ(cinfo, smp_processor_id(), my_irq_handler) < 0)
		{
			pr_info("Could not register irq for cpu %d\n", i);
			return -1;
		}
		cinfo->cpunum = cpunum;
#endif
		/* initialize counter h/w & event structure */
		read_budget = convert_mb_to_events(DEFAULT_RD_BUDGET_MB);
		write_budget = convert_mb_to_events(DEFAULT_WR_BUDGET_MB);

		/* initialize per-core data structure */
		memset(cinfo, 0, sizeof(struct core_info));

		/* create performance counter */
		pr_info("read_budget %d, write_budget %d\n", read_budget, write_budget);
#ifndef FIRESIM // We are using only DRAM accesses
		cinfo->read_event = init_counter(i, read_budget, g_read_counter_id,
						 event_overflow_callback);
		cinfo->write_event = init_counter(i, write_budget, g_write_counter_id,
						  event_write_overflow_callback);
#else
	init_counter(smp_processor_id(), read_budget, g_read_counter_id,
						 event_overflow_callback);
#endif

#ifndef FIRESIM
		if (!cinfo->read_event || !cinfo->write_event)
			break;
		/* initialize budget */
		cinfo->read_budget 	= cinfo->read_limit = cinfo->read_event->hw.sample_period;
		cinfo->write_budget = cinfo->write_limit = cinfo->write_event->hw.sample_period;
#else
	cinfo->read_budget = read_budget;
	cinfo->write_budget = write_budget;
#endif
		

		/* throttled task pointer */
		cinfo->throttled_task = NULL;
		pr_info("Init waitqueue throttle_evt\n");
		init_waitqueue_head(&cinfo->throttle_evt);

		/* initialize statistics */
		/* update local period information */
		cinfo->period_cnt = 0;
		cinfo->read_used[0] = cinfo->read_used[1] = cinfo->read_used[2] =
			cinfo->read_budget; /* initial condition */
		cinfo->cur_read_budget = cinfo->read_budget;
		cinfo->overall.used_read_budget = 0;
		cinfo->overall.assigned_read_budget = 0;
        
        	cinfo->cur_write_budget = cinfo->write_budget;
        	cinfo->overall.used_write_budget = 0;
        	cinfo->overall.assigned_write_budget = 0;
        
		cinfo->overall.throttled_time_ns = 0;
		cinfo->overall.throttled = 0;
		cinfo->overall.throttled_error = 0;

		memset(cinfo->overall.throttled_error_dist, 0, sizeof(int)*10);
		cinfo->throttled_time = ktime_set(0,0);

		print_core_info(smp_processor_id(), cinfo);

		/* initialize nmi irq_work_queue */
		init_irq_work(&cinfo->read_pending, memguard_read_process_overflow);
        init_irq_work(&cinfo->write_pending, memguard_write_process_overflow);

		/* create and wake-up throttle threads */
		cinfo->throttle_thread =
			kthread_create_on_node(throttle_thread,
					       (void *)((unsigned long)i),
					       cpu_to_node(i),
					       "kthrottle/%d", i);
#ifndef FIRESIM
		perf_event_enable(cinfo->read_event);
		perf_event_enable(cinfo->write_event);	
#endif
		BUG_ON(IS_ERR(cinfo->throttle_thread));
		kthread_bind(cinfo->throttle_thread, i);
		wake_up_process(cinfo->throttle_thread);
	}

	memguard_init_debugfs();




#ifdef FIRESIM
	for_each_online_cpu(i) {
		pr_info("PLIC enable core %d interrupt\n", i);
		WRITE_BOOL(L2CACHE_EN_CORE_INTERRUPT_ADDR(i), true); // Enable interrupts on all cores
		PLIC_Setup(PLIC_BASE_ADDR, i); // Enable interrupts in PLIC

		// Start new period
		WRITE_BOOL(L2CACHE_PERIOD_LEN_ADDR(i), true);
		WRITE_BOOL(L2CACHE_PERIOD_LEN_ADDR(i), false);
	}
	
	
#endif

	/* start timer and perf counters */
	pr_info("Start period timer (period=%lld us)\n",
		div64_u64(TM_NS(global->period_in_ktime), 1000));
	on_each_cpu(__start_counter, NULL, 0);

	return 0;
}

void cleanup_module( void )
{
	int i;

	struct memguard_info *global = &memguard_info;

	/* stop perf_event counters and timers */
	on_each_cpu(__stop_counter, NULL, 0);
	pr_info("LLC bandwidth throttling disabled\n");

	/* destroy perf objects */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
#ifdef FIRESIM
		/*
			We should add custom cleanup here 
		*/
		WRITE_BOOL(L2CACHE_EN_CORE_INTERRUPT_ADDR(i), false); // disable interrupts for core
		WRITE_BOOL(L2CACHE_PERIOD_LEN_ADDR(i), true); // hold period reset high to zero all counters 
		free_irq(cinfo->irq, NULL); // free the allocated IRQ



#endif


		
		pr_info("Stopping kthrottle/%d\n", i);
		kthread_stop(cinfo->throttle_thread);
		perf_event_disable(cinfo->read_event);
		perf_event_release_kernel(cinfo->read_event); 
		cinfo->read_event = NULL; 
        
	    perf_event_disable(cinfo->write_event);
       	perf_event_release_kernel(cinfo->write_event);
        cinfo->write_event = NULL;
	}

	/* remove debugfs entries */
	debugfs_remove_recursive(memguard_dir);

	/* free allocated data structure */
	free_cpumask_var(global->throttle_mask);
	free_cpumask_var(global->active_mask);
	free_percpu(core_info);
	pr_info("module uninstalled successfully\n");
	return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Heechul Yun <heechul@illinois.edu>");
