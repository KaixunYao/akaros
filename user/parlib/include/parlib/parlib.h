// Main public header file for our user-land support library,
// whose code lives in the lib directory.
// This library is roughly our OS's version of a standard C library,
// and is intended to be linked into all user-mode applications
// (NOT the kernel or boot loader).

#pragma once

#ifndef __ASSEMBLER__

#include <parlib/common.h>
#include <parlib/vcore.h>
#include <parlib/core_set.h>
#include <ros/memlayout.h>
#include <ros/syscall.h>
#include <ros/procinfo.h>
#include <ros/procdata.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <ros/fdtap.h>

__BEGIN_DECLS

enum {
	PG_RDONLY = 4,
	PG_RDWR   = 6,
};

extern const char *const __syscall_tbl[];
extern int __syscall_tbl_sz;

int         sys_null(void);
size_t      sys_getpcoreid(void);
int         sys_proc_destroy(int pid, int exitcode);
void        sys_yield(bool being_nice);
int         sys_proc_create(const char *path, size_t path_l, char *const argv[],
                            char *const envp[], int flags);
int         sys_proc_run(int pid);
ssize_t     sys_shared_page_alloc(void **addr, pid_t p2, 
                                  int p1_flags, int p2_flags);
ssize_t     sys_shared_page_free(void *addr, pid_t p2);
void        sys_reboot();
void 		*sys_mmap(void *addr, size_t length, int prot, int flags,
                      int fd, size_t offset);
int			sys_provision(int pid, unsigned int res_type, long res_val);
int         sys_notify(int pid, unsigned int ev_type, struct event_msg *u_msg);
int         sys_self_notify(uint32_t vcoreid, unsigned int ev_type,
                            struct event_msg *u_msg, bool priv);
int         sys_send_event(struct event_queue *ev_q, struct event_msg *ev_msg,
                           uint32_t vcoreid);
int         sys_halt_core(unsigned long usec);
void*		sys_init_arsc();
int         sys_block(unsigned long usec);
int         sys_change_vcore(uint32_t vcoreid, bool enable_my_notif);
int         sys_change_to_m(void);
int         sys_poke_ksched(int pid, unsigned int res_type);
int         sys_abort_sysc(struct syscall *sysc);
int         sys_abort_sysc_fd(int fd);
int         sys_tap_fds(struct fd_tap_req *tap_reqs, size_t nr_reqs);

void		syscall_async(struct syscall *sysc, unsigned long num, ...);
void        syscall_async_evq(struct syscall *sysc, struct event_queue *evq,
                              unsigned long num, ...);

/* Control variables */
extern bool parlib_wants_to_be_mcp;	/* instructs the 2LS to be an MCP */
extern bool parlib_never_yield;		/* instructs the 2LS to not yield vcores */
extern bool parlib_never_vc_request;/* 2LS: do not request vcores */

/* Process Management */
pid_t create_child(const char *exe, int argc, char *const argv[],
                   char *const envp[]);
pid_t create_child_with_stdfds(const char *exe, int argc, char *const argv[],
                               char *const envp[]);
int provision_core_set(pid_t pid, const struct core_set *cores);

/* Once */
typedef struct {
	bool ran_once;
	bool is_running;
} parlib_once_t;

#define PARLIB_ONCE_INIT {FALSE, FALSE}

/* Makes sure func is run exactly once.  Can handle concurrent callers, and
 * other callers spin til the func is complete. */
static inline void parlib_run_once(parlib_once_t *once_ctl,
                                   void (*init_fn)(void *), void *arg)
{
	if (!once_ctl->ran_once) {
		/* fetch and set TRUE, without a header or test_and_set weirdness */
		if (!__sync_fetch_and_or(&once_ctl->is_running, TRUE)) {
			/* we won the race and get to run the func */
			init_fn(arg);
			wmb();	/* don't let the ran_once write pass previous writes */
			once_ctl->ran_once = TRUE;
		} else {
			/* someone else won, wait til they are done to break out */
			while (!once_ctl->ran_once)
				cpu_relax_any();
		}
	}
}

/* Unprotected, single-threaded version, makes sure func is run exactly once */
static inline void parlib_run_once_racy(parlib_once_t *once_ctl,
                                        void (*init_fn)(void *), void *arg)
{
	if (!once_ctl->ran_once) {
		init_fn(arg);
		once_ctl->ran_once = TRUE;
	}
}

static inline void parlib_set_ran_once(parlib_once_t *once_ctl)
{
	once_ctl->ran_once = TRUE;
}

/* Aborts with 'retcmd' if this function has already been called.  Compared to
 * run_once, this is put at the top of a function that can be called from
 * multiple sources but should only execute once. */
#define parlib_init_once_racy(retcmd)                                          \
do {                                                                           \
	static bool initialized = FALSE;                                           \
	if (initialized) {                                                         \
		retcmd;                                                                \
	}                                                                          \
	initialized = TRUE;                                                        \
} while (0)

__END_DECLS

#endif	// !ASSEMBLER
