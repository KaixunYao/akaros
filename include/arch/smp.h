/*
 * Copyright (c) 2009 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 */

#ifndef ROS_INC_SMP_H
#define ROS_INC_SMP_H

/* SMP related functions */

#include <arch/types.h>

#include <atomic.h>
#include <trap.h>
#include <workqueue.h>

// be careful changing this, esp if you go over 16
#define NUM_HANDLER_WRAPPERS		5

typedef struct HandlerWrapper {
	checklist_t* cpu_list;
	uint8_t vector;
} handler_wrapper_t;

// will want this padded out to cacheline alignment
struct per_cpu_info {
	uint32_t lock;
	struct workqueue workqueue;
};
extern struct per_cpu_info  per_cpu_info[MAX_NUM_CPUS];
extern volatile uint8_t num_cpus;

/* SMP bootup functions */
void smp_boot(void);
void smp_idle(void);

/* SMP utility functions */
int smp_call_function_self(isr_t handler, void* data,
                           handler_wrapper_t** wait_wrapper);
int smp_call_function_all(isr_t handler, void* data,
                          handler_wrapper_t** wait_wrapper);
int smp_call_function_single(uint8_t dest, isr_t handler, void* data,
                             handler_wrapper_t** wait_wrapper);
int smp_call_wait(handler_wrapper_t* wrapper);

#endif /* !ROS_INC_SMP_H */
