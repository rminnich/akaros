/* Copyright (c) 2009, 2010 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details. */

#ifdef __SHARC__
#pragma nosharc
#endif

#include <ros/bcq.h>
#include <event.h>
#include <arch/arch.h>
#include <bitmask.h>
#include <process.h>
#include <atomic.h>
#include <smp.h>
#include <pmap.h>
#include <trap.h>
#include <schedule.h>
#include <manager.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <hashtable.h>
#include <slab.h>
#include <sys/queue.h>
#include <frontend.h>
#include <monitor.h>
#include <resource.h>
#include <elf.h>
#include <arsc_server.h>
#include <devfs.h>

struct kmem_cache *proc_cache;

/* Other helpers, implemented later. */
static void __proc_startcore(struct proc *p, trapframe_t *tf);
static bool is_mapped_vcore(struct proc *p, uint32_t pcoreid);
static uint32_t get_vcoreid(struct proc *p, uint32_t pcoreid);
static uint32_t try_get_pcoreid(struct proc *p, uint32_t vcoreid);
static uint32_t get_pcoreid(struct proc *p, uint32_t vcoreid);
static void __proc_free(struct kref *kref);

/* PID management. */
#define PID_MAX 32767 // goes from 0 to 32767, with 0 reserved
static DECL_BITMASK(pid_bmask, PID_MAX + 1);
spinlock_t pid_bmask_lock = SPINLOCK_INITIALIZER;
struct hashtable *pid_hash;
spinlock_t pid_hash_lock; // initialized in proc_init

/* Finds the next free entry (zero) entry in the pid_bitmask.  Set means busy.
 * PID 0 is reserved (in proc_init).  A return value of 0 is a failure (and
 * you'll also see a warning, for now).  Consider doing this with atomics. */
static pid_t get_free_pid(void)
{
	static pid_t next_free_pid = 1;
	pid_t my_pid = 0;

	spin_lock(&pid_bmask_lock);
	// atomically (can lock for now, then change to atomic_and_return
	FOR_CIRC_BUFFER(next_free_pid, PID_MAX + 1, i) {
		// always points to the next to test
		next_free_pid = (next_free_pid + 1) % (PID_MAX + 1);
		if (!GET_BITMASK_BIT(pid_bmask, i)) {
			SET_BITMASK_BIT(pid_bmask, i);
			my_pid = i;
			break;
		}
	}
	spin_unlock(&pid_bmask_lock);
	if (!my_pid)
		warn("Shazbot!  Unable to find a PID!  You need to deal with this!\n");
	return my_pid;
}

/* Return a pid to the pid bitmask */
static void put_free_pid(pid_t pid)
{
	spin_lock(&pid_bmask_lock);
	CLR_BITMASK_BIT(pid_bmask, pid);
	spin_unlock(&pid_bmask_lock);
}

/* While this could be done with just an assignment, this gives us the
 * opportunity to check for bad transitions.  Might compile these out later, so
 * we shouldn't rely on them for sanity checking from userspace.  */
int __proc_set_state(struct proc *p, uint32_t state)
{
	uint32_t curstate = p->state;
	/* Valid transitions:
	 * C   -> RBS
	 * C   -> D
	 * RBS -> RGS
	 * RGS -> RBS
	 * RGS -> W
	 * RGM -> W
	 * W   -> RBS
	 * W   -> RBM
	 * RGS -> RBM
	 * RBM -> RGM
	 * RGM -> RBM
	 * RGM -> RBS
	 * RGS -> D
	 * RGM -> D
	 *
	 * These ought to be implemented later (allowed, not thought through yet).
	 * RBS -> D
	 * RBM -> D
	 */
	#if 1 // some sort of correctness flag
	switch (curstate) {
		case PROC_CREATED:
			if (!(state & (PROC_RUNNABLE_S | PROC_DYING)))
				panic("Invalid State Transition! PROC_CREATED to %02x", state);
			break;
		case PROC_RUNNABLE_S:
			if (!(state & (PROC_RUNNING_S | PROC_DYING)))
				panic("Invalid State Transition! PROC_RUNNABLE_S to %02x", state);
			break;
		case PROC_RUNNING_S:
			if (!(state & (PROC_RUNNABLE_S | PROC_RUNNABLE_M | PROC_WAITING |
			               PROC_DYING)))
				panic("Invalid State Transition! PROC_RUNNING_S to %02x", state);
			break;
		case PROC_WAITING:
			if (!(state & (PROC_RUNNABLE_S | PROC_RUNNABLE_M)))
				panic("Invalid State Transition! PROC_WAITING to %02x", state);
			break;
		case PROC_DYING:
			if (state != PROC_CREATED) // when it is reused (TODO)
				panic("Invalid State Transition! PROC_DYING to %02x", state);
			break;
		case PROC_RUNNABLE_M:
			if (!(state & (PROC_RUNNING_M | PROC_DYING)))
				panic("Invalid State Transition! PROC_RUNNABLE_M to %02x", state);
			break;
		case PROC_RUNNING_M:
			if (!(state & (PROC_RUNNABLE_S | PROC_RUNNABLE_M | PROC_WAITING |
			               PROC_DYING)))
				panic("Invalid State Transition! PROC_RUNNING_M to %02x", state);
			break;
	}
	#endif
	p->state = state;
	return 0;
}

/* Returns a pointer to the proc with the given pid, or 0 if there is none.
 * This uses get_not_zero, since it is possible the refcnt is 0, which means the
 * process is dying and we should not have the ref (and thus return 0).  We need
 * to lock to protect us from getting p, (someone else removes and frees p),
 * then get_not_zero() on p.
 * Don't push the locking into the hashtable without dealing with this. */
struct proc *pid2proc(pid_t pid)
{
	spin_lock(&pid_hash_lock);
	struct proc *p = hashtable_search(pid_hash, (void*)(long)pid);
	if (p)
		if (!kref_get_not_zero(&p->p_kref, 1))
			p = 0;
	spin_unlock(&pid_hash_lock);
	return p;
}

/* Performs any initialization related to processes, such as create the proc
 * cache, prep the scheduler, etc.  When this returns, we should be ready to use
 * any process related function. */
void proc_init(void)
{
	/* Catch issues with the vcoremap and TAILQ_ENTRY sizes */
	static_assert(sizeof(TAILQ_ENTRY(vcore)) == sizeof(void*) * 2);
	proc_cache = kmem_cache_create("proc", sizeof(struct proc),
	             MAX(HW_CACHE_ALIGN, __alignof__(struct proc)), 0, 0, 0);
	/* Init PID mask and hash.  pid 0 is reserved. */
	SET_BITMASK_BIT(pid_bmask, 0);
	spinlock_init(&pid_hash_lock);
	spin_lock(&pid_hash_lock);
	pid_hash = create_hashtable(100, __generic_hash, __generic_eq);
	spin_unlock(&pid_hash_lock);
	schedule_init();

	atomic_init(&num_envs, 0);
}

/* Be sure you init'd the vcore lists before calling this. */
static void proc_init_procinfo(struct proc* p)
{
	p->procinfo->pid = p->pid;
	p->procinfo->ppid = p->ppid;
	p->procinfo->max_vcores = max_vcores(p);
	p->procinfo->tsc_freq = system_timing.tsc_freq;
	p->procinfo->heap_bottom = (void*)UTEXT;
	/* 0'ing the arguments.  Some higher function will need to set them */
	memset(p->procinfo->argp, 0, sizeof(p->procinfo->argp));
	memset(p->procinfo->argbuf, 0, sizeof(p->procinfo->argbuf));
	/* 0'ing the vcore/pcore map.  Will link the vcores later. */
	memset(&p->procinfo->vcoremap, 0, sizeof(p->procinfo->vcoremap));
	memset(&p->procinfo->pcoremap, 0, sizeof(p->procinfo->pcoremap));
	p->procinfo->num_vcores = 0;
	p->procinfo->is_mcp = FALSE;
	p->procinfo->coremap_seqctr = SEQCTR_INITIALIZER;
	/* For now, we'll go up to the max num_cpus (at runtime).  In the future,
	 * there may be cases where we can have more vcores than num_cpus, but for
	 * now we'll leave it like this. */
	for (int i = 0; i < num_cpus; i++) {
		TAILQ_INSERT_TAIL(&p->inactive_vcs, &p->procinfo->vcoremap[i], list);
	}
}

static void proc_init_procdata(struct proc *p)
{
	memset(p->procdata, 0, sizeof(struct procdata));
}

/* Allocates and initializes a process, with the given parent.  Currently
 * writes the *p into **pp, and returns 0 on success, < 0 for an error.
 * Errors include:
 *  - ENOFREEPID if it can't get a PID
 *  - ENOMEM on memory exhaustion */
error_t proc_alloc(struct proc **pp, struct proc *parent)
{
	error_t r;
	struct proc *p;

	if (!(p = kmem_cache_alloc(proc_cache, 0)))
		return -ENOMEM;

	{ INITSTRUCT(*p)

	/* one reference for the proc existing, and one for the ref we pass back. */
	kref_init(&p->p_kref, __proc_free, 2);
	// Setup the default map of where to get cache colors from
	p->cache_colors_map = global_cache_colors_map;
	p->next_cache_color = 0;
	/* Initialize the address space */
	if ((r = env_setup_vm(p)) < 0) {
		kmem_cache_free(proc_cache, p);
		return r;
	}
	if (!(p->pid = get_free_pid())) {
		kmem_cache_free(proc_cache, p);
		return -ENOFREEPID;
	}
	/* Set the basic status variables. */
	spinlock_init(&p->proc_lock);
	p->exitcode = 1337;	/* so we can see processes killed by the kernel */
	p->ppid = parent ? parent->pid : 0;
	p->state = PROC_CREATED; /* shouldn't go through state machine for init */
	p->env_flags = 0;
	p->env_entry = 0; // cheating.  this really gets set later
	p->heap_top = (void*)UTEXT;	/* heap_bottom set in proc_init_procinfo */
	memset(&p->resources, 0, sizeof(p->resources));
	memset(&p->env_ancillary_state, 0, sizeof(p->env_ancillary_state));
	memset(&p->env_tf, 0, sizeof(p->env_tf));
	spinlock_init(&p->mm_lock);
	TAILQ_INIT(&p->vm_regions); /* could init this in the slab */
	/* Initialize the vcore lists, we'll build the inactive list so that it includes
	 * all vcores when we initialize procinfo.  Do this before initing procinfo. */
	TAILQ_INIT(&p->online_vcs);
	TAILQ_INIT(&p->bulk_preempted_vcs);
	TAILQ_INIT(&p->inactive_vcs);
	/* Init procinfo/procdata.  Procinfo's argp/argb are 0'd */
	proc_init_procinfo(p);
	proc_init_procdata(p);

	/* Initialize the generic sysevent ring buffer */
	SHARED_RING_INIT(&p->procdata->syseventring);
	/* Initialize the frontend of the sysevent ring buffer */
	FRONT_RING_INIT(&p->syseventfrontring,
	                &p->procdata->syseventring,
	                SYSEVENTRINGSIZE);

	/* Init FS structures TODO: cleanup (might pull this out) */
	kref_get(&default_ns.kref, 1);
	p->ns = &default_ns;
	spinlock_init(&p->fs_env.lock);
	p->fs_env.umask = parent ? parent->fs_env.umask : S_IWGRP | S_IWOTH;
	p->fs_env.root = p->ns->root->mnt_root;
	kref_get(&p->fs_env.root->d_kref, 1);
	p->fs_env.pwd = parent ? parent->fs_env.pwd : p->fs_env.root;
	kref_get(&p->fs_env.pwd->d_kref, 1);
	memset(&p->open_files, 0, sizeof(p->open_files));	/* slightly ghetto */
	spinlock_init(&p->open_files.lock);
	p->open_files.max_files = NR_OPEN_FILES_DEFAULT;
	p->open_files.max_fdset = NR_FILE_DESC_DEFAULT;
	p->open_files.fd = p->open_files.fd_array;
	p->open_files.open_fds = (struct fd_set*)&p->open_files.open_fds_init;
	/* Init the ucq hash lock */
	p->ucq_hashlock = (struct hashlock*)&p->ucq_hl_noref;
	hashlock_init(p->ucq_hashlock, HASHLOCK_DEFAULT_SZ);

	atomic_inc(&num_envs);
	frontend_proc_init(p);
	printd("[%08x] new process %08x\n", current ? current->pid : 0, p->pid);
	} // INIT_STRUCT
	*pp = p;
	return 0;
}

/* We have a bunch of different ways to make processes.  Call this once the
 * process is ready to be used by the rest of the system.  For now, this just
 * means when it is ready to be named via the pidhash.  In the future, we might
 * push setting the state to CREATED into here. */
void __proc_ready(struct proc *p)
{
	spin_lock(&pid_hash_lock);
	hashtable_insert(pid_hash, (void*)(long)p->pid, p);
	spin_unlock(&pid_hash_lock);
}

/* Creates a process from the specified file, argvs, and envps.  Tempted to get
 * rid of proc_alloc's style, but it is so quaint... */
struct proc *proc_create(struct file *prog, char **argv, char **envp)
{
	struct proc *p;
	error_t r;
	if ((r = proc_alloc(&p, current)) < 0)
		panic("proc_create: %e", r);	/* one of 3 quaint usages of %e */
	procinfo_pack_args(p->procinfo, argv, envp);
	assert(load_elf(p, prog) == 0);
	/* Connect to stdin, stdout, stderr */
	assert(insert_file(&p->open_files, dev_stdin,  0) == 0);
	assert(insert_file(&p->open_files, dev_stdout, 0) == 1);
	assert(insert_file(&p->open_files, dev_stderr, 0) == 2);
	__proc_ready(p);
	return p;
}

/* This is called by kref_put(), once the last reference to the process is
 * gone.  Don't call this otherwise (it will panic).  It will clean up the
 * address space and deallocate any other used memory. */
static void __proc_free(struct kref *kref)
{
	struct proc *p = container_of(kref, struct proc, p_kref);
	physaddr_t pa;

	printd("[PID %d] freeing proc: %d\n", current ? current->pid : 0, p->pid);
	// All parts of the kernel should have decref'd before __proc_free is called
	assert(kref_refcnt(&p->p_kref) == 0);

	kref_put(&p->fs_env.root->d_kref);
	kref_put(&p->fs_env.pwd->d_kref);
	destroy_vmrs(p);
	frontend_proc_free(p);	/* TODO: please remove me one day */
	/* Free any colors allocated to this process */
	if (p->cache_colors_map != global_cache_colors_map) {
		for(int i = 0; i < llc_cache->num_colors; i++)
			cache_color_free(llc_cache, p->cache_colors_map);
		cache_colors_map_free(p->cache_colors_map);
	}
	/* Remove us from the pid_hash and give our PID back (in that order). */
	spin_lock(&pid_hash_lock);
	if (!hashtable_remove(pid_hash, (void*)(long)p->pid))
		panic("Proc not in the pid table in %s", __FUNCTION__);
	spin_unlock(&pid_hash_lock);
	put_free_pid(p->pid);
	/* Flush all mapped pages in the user portion of the address space */
	env_user_mem_free(p, 0, UVPT);
	/* These need to be free again, since they were allocated with a refcnt. */
	free_cont_pages(p->procinfo, LOG2_UP(PROCINFO_NUM_PAGES));
	free_cont_pages(p->procdata, LOG2_UP(PROCDATA_NUM_PAGES));

	env_pagetable_free(p);
	p->env_pgdir = 0;
	p->env_cr3 = 0;

	atomic_dec(&num_envs);

	/* Dealloc the struct proc */
	kmem_cache_free(proc_cache, p);
}

/* Whether or not actor can control target.  Note we currently don't need
 * locking for this. TODO: think about that, esp wrt proc's dying. */
bool proc_controls(struct proc *actor, struct proc *target)
{
	return ((actor == target) || (target->ppid == actor->pid));
}

/* Helper to incref by val.  Using the helper to help debug/interpose on proc
 * ref counting.  Note that pid2proc doesn't use this interface. */
void proc_incref(struct proc *p, unsigned int val)
{
	kref_get(&p->p_kref, val);
}

/* Helper to decref for debugging.  Don't directly kref_put() for now. */
void proc_decref(struct proc *p)
{
	kref_put(&p->p_kref);
}

/* Helper, makes p the 'current' process, dropping the old current/cr3.  This no
 * longer assumes the passed in reference already counted 'current'.  It will
 * incref internally when needed. */
static void __set_proc_current(struct proc *p)
{
	/* We use the pcpui to access 'current' to cut down on the core_id() calls,
	 * though who know how expensive/painful they are. */
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	/* If the process wasn't here, then we need to load its address space. */
	if (p != pcpui->cur_proc) {
		proc_incref(p, 1);
		lcr3(p->env_cr3);
		/* This is "leaving the process context" of the previous proc.  The
		 * previous lcr3 unloaded the previous proc's context.  This should
		 * rarely happen, since we usually proactively leave process context,
		 * but this is the fallback. */
		if (pcpui->cur_proc)
			proc_decref(pcpui->cur_proc);
		pcpui->cur_proc = p;
	}
}

/* Dispatches a _S process to run on the current core.  This should never be
 * called to "restart" a core.   
 *
 * This will always return, regardless of whether or not the calling core is
 * being given to a process. (it used to pop the tf directly, before we had
 * cur_tf).
 *
 * Since it always returns, it will never "eat" your reference (old
 * documentation talks about this a bit). */
void proc_run_s(struct proc *p)
{
	spin_lock(&p->proc_lock);
	switch (p->state) {
		case (PROC_DYING):
			spin_unlock(&p->proc_lock);
			printk("Process %d not starting due to async death\n", p->pid);
			return;
		case (PROC_RUNNABLE_S):
			assert(current != p);
			__proc_set_state(p, PROC_RUNNING_S);
			/* We will want to know where this process is running, even if it is
			 * only in RUNNING_S.  can use the vcoremap, which makes death easy.
			 * Also, this is the signal used in trap.c to know to save the tf in
			 * env_tf. */
			__seq_start_write(&p->procinfo->coremap_seqctr);
			p->procinfo->num_vcores = 0;	/* TODO (VC#) */
			/* TODO: For now, we won't count this as an active vcore (on the
			 * lists).  This gets unmapped in resource.c and yield_s, and needs
			 * work. */
			__map_vcore(p, 0, core_id()); // sort of.  this needs work.
			__seq_end_write(&p->procinfo->coremap_seqctr);
			/* incref, since we're saving a reference in owning proc */
			proc_incref(p, 1);
			/* redundant with proc_startcore, might be able to remove that one*/
			__set_proc_current(p);
			/* We restartcore, instead of startcore, since startcore is a bit
			 * lower level and we want a chance to process kmsgs before starting
			 * the process. */
			spin_unlock(&p->proc_lock);
			disable_irq();		/* before mucking with cur_tf / owning_proc */
			/* this is one of the few times cur_tf != &actual_tf */
			current_tf = &p->env_tf;	/* no need for irq disable yet */
			/* storing the passed in ref of p in owning_proc */
			per_cpu_info[core_id()].owning_proc = p;
			/* When the calling core idles, it'll call restartcore and run the
			 * _S process's context. */
			return;
		default:
			spin_unlock(&p->proc_lock);
			panic("Invalid process state %p in %s()!!", p->state, __FUNCTION__);
	}
}

/* Helper: sends preempt messages to all vcores on the bulk preempt list, and
 * moves them to the inactive list. */
static void __send_bulkp_events(struct proc *p)
{
	struct vcore *vc_i, *vc_temp;
	struct event_msg preempt_msg = {0};
	/* Send preempt messages for any left on the BP list.  No need to set any
	 * flags, it all was done on the real preempt.  Now we're just telling the
	 * process about any that didn't get restarted and are still preempted. */
	TAILQ_FOREACH_SAFE(vc_i, &p->bulk_preempted_vcs, list, vc_temp) {
		/* Note that if there are no active vcores, send_k_e will post to our
		 * own vcore, the last of which will be put on the inactive list and be
		 * the first to be started.  We could have issues with deadlocking,
		 * since send_k_e() could grab the proclock (if there are no active
		 * vcores) */
		preempt_msg.ev_type = EV_VCORE_PREEMPT;
		preempt_msg.ev_arg2 = vcore2vcoreid(p, vc_i);	/* arg2 is 32 bits */
		send_kernel_event(p, &preempt_msg, 0);
		/* TODO: we may want a TAILQ_CONCAT_HEAD, or something that does that.
		 * We need a loop for the messages, but not necessarily for the list
		 * changes.  */
		TAILQ_REMOVE(&p->bulk_preempted_vcs, vc_i, list);
		TAILQ_INSERT_HEAD(&p->inactive_vcs, vc_i, list);
	}
}

/* Run an _M.  Can be called safely on one that is already running.  Hold the
 * lock before calling.  Other than state checks, this just starts up the _M's
 * vcores, much like the second part of give_cores_running.  More specifically,
 * give_cores_runnable puts cores on the online list, which this then sends
 * messages to.  give_cores_running immediately puts them on the list and sends
 * the message.  the two-step style may go out of fashion soon.
 *
 * This expects that the "instructions" for which core(s) to run this on will be
 * in the vcoremap, which needs to be set externally (give_cores()). */
void __proc_run_m(struct proc *p)
{
	struct vcore *vc_i;
	switch (p->state) {
		case (PROC_DYING):
			printk("Process %d not starting due to async death\n", p->pid);
			return;
		case (PROC_RUNNABLE_M):
			/* vcoremap[i] holds the coreid of the physical core allocated to
			 * this process.  It is set outside proc_run.  For the kernel
			 * message, a0 = struct proc*, a1 = struct trapframe*.   */
			if (p->procinfo->num_vcores) {
				__send_bulkp_events(p);
				__proc_set_state(p, PROC_RUNNING_M);
				/* Up the refcnt, to avoid the n refcnt upping on the
				 * destination cores.  Keep in sync with __startcore */
				proc_incref(p, p->procinfo->num_vcores * 2);
				/* Send kernel messages to all online vcores (which were added
				 * to the list and mapped in __proc_give_cores()), making them
				 * turn online */
				TAILQ_FOREACH(vc_i, &p->online_vcs, list) {
					send_kernel_message(vc_i->pcoreid, __startcore, (long)p,
					                    0, 0, KMSG_IMMEDIATE);
				}
			} else {
				warn("Tried to proc_run() an _M with no vcores!");
			}
			/* There a subtle race avoidance here (when we unlock after sending
			 * the message).  __proc_startcore can handle a death message, but
			 * we can't have the startcore come after the death message.
			 * Otherwise, it would look like a new process.  So we hold the lock
			 * til after we send our message, which prevents a possible death
			 * message.
			 * - Note there is no guarantee this core's interrupts were on, so
			 *   it may not get the message for a while... */
			return;
		case (PROC_RUNNING_M):
		case (PROC_WAITING):
			return;
		default:
			/* unlock just so the monitor can call something that might lock*/
			spin_unlock(&p->proc_lock);
			panic("Invalid process state %p in %s()!!", p->state, __FUNCTION__);
	}
}

/* Actually runs the given context (trapframe) of process p on the core this
 * code executes on.  This is called directly by __startcore, which needs to
 * bypass the routine_kmsg check.  Interrupts should be off when you call this.
 *
 * A note on refcnting: this function will not return, and your proc reference
 * will end up stored in current.  This will make no changes to p's refcnt, so
 * do your accounting such that there is only the +1 for current.  This means if
 * it is already in current (like in the trap return path), don't up it.  If
 * it's already in current and you have another reference (like pid2proc or from
 * an IPI), then down it (which is what happens in __startcore()).  If it's not
 * in current and you have one reference, like proc_run(non_current_p), then
 * also do nothing.  The refcnt for your *p will count for the reference stored
 * in current. */
static void __proc_startcore(struct proc *p, trapframe_t *tf)
{
	assert(!irq_is_enabled());
	__set_proc_current(p);
	/* need to load our silly state, preferably somewhere other than here so we
	 * can avoid the case where the context was just running here.  it's not
	 * sufficient to do it in the "new process" if-block above (could be things
	 * like page faults that cause us to keep the same process, but want a
	 * different context.
	 * for now, we load this silly state here. (TODO) (HSS)
	 * We also need this to be per trapframe, and not per process...
	 * For now / OSDI, only load it when in _S mode.  _M mode was handled in
	 * __startcore.  */
	if (p->state == PROC_RUNNING_S)
		env_pop_ancillary_state(p);
	/* Clear the current_tf, since it is no longer used */
	current_tf = 0;	/* TODO: might not need this... */
	env_pop_tf(tf);
}

/* Restarts/runs the current_tf, which must be for the current process, on the
 * core this code executes on.  Calls an internal function to do the work.
 *
 * In case there are pending routine messages, like __death, __preempt, or
 * __notify, we need to run them.  Alternatively, if there are any, we could
 * self_ipi, and run the messages immediately after popping back to userspace,
 * but that would have crappy overhead.
 *
 * Refcnting: this will not return, and it assumes that you've accounted for
 * your reference as if it was the ref for "current" (which is what happens when
 * returning from local traps and such. */
void proc_restartcore(void)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	assert(!pcpui->cur_sysc);
	/* Try and get any interrupts before we pop back to userspace.  If we didn't
	 * do this, we'd just get them in userspace, but this might save us some
	 * effort/overhead. */
	enable_irq();
	/* Need ints disabled when we return from processing (race on missing
	 * messages/IPIs) */
	disable_irq();
	process_routine_kmsg(pcpui->cur_tf);
	/* If there is no owning process, just idle, since we don't know what to do.
	 * This could be because the process had been restarted a long time ago and
	 * has since left the core, or due to a KMSG like __preempt or __death. */
	if (!pcpui->owning_proc) {
		abandon_core();
		smp_idle();
	}
	assert(pcpui->cur_tf);
	__proc_startcore(pcpui->owning_proc, pcpui->cur_tf);
}

/*
 * Destroys the given process.  This may be called from another process, a light
 * kernel thread (no real process context), asynchronously/cross-core, or from
 * the process on its own core.
 *
 * Here's the way process death works:
 * 0. grab the lock (protects state transition and core map)
 * 1. set state to dying.  that keeps the kernel from doing anything for the
 * process (like proc_running it).
 * 2. figure out where the process is running (cross-core/async or RUNNING_M)
 * 3. IPI to clean up those cores (decref, etc).
 * 4. Unlock
 * 5. Clean up your core, if applicable
 * (Last core/kernel thread to decref cleans up and deallocates resources.)
 *
 * Note that some cores can be processing async calls, but will eventually
 * decref.  Should think about this more, like some sort of callback/revocation.
 *
 * This function will now always return (it used to not return if the calling
 * core was dying).  However, when it returns, a kernel message will eventually
 * come in, making you abandon_core, as if you weren't running.  It may be that
 * the only reference to p is the one you passed in, and when you decref, it'll
 * get __proc_free()d. */
void proc_destroy(struct proc *p)
{
	spin_lock(&p->proc_lock);
	switch (p->state) {
		case PROC_DYING: // someone else killed this already.
			spin_unlock(&p->proc_lock);
			return;
		case PROC_RUNNABLE_M:
			/* Need to reclaim any cores this proc might have, even though it's
			 * not running yet. */
			__proc_take_allcores_dumb(p, FALSE);
			// fallthrough
		case PROC_RUNNABLE_S:
			/* might need to pull from lists, though i'm currently a fan of the
			 * model where external refs notice DYING (if it matters to them)
			 * and decref when they are done.  the ksched will notice the proc
			 * is dying and handle it accordingly (which delay the reaping til
			 * the next call to schedule()) */
			break;
		case PROC_RUNNING_S:
			#if 0
			// here's how to do it manually
			if (current == p) {
				lcr3(boot_cr3);
				proc_decref(p);		/* this decref is for the cr3 */
				current = NULL;
			}
			#endif
			send_kernel_message(get_pcoreid(p, 0), __death, 0, 0, 0,
			                    KMSG_IMMEDIATE);
			__seq_start_write(&p->procinfo->coremap_seqctr);
			// TODO: might need to sort num_vcores too later (VC#)
			/* vcore is unmapped on the receive side */
			__seq_end_write(&p->procinfo->coremap_seqctr);
			#if 0
			/* right now, RUNNING_S only runs on a mgmt core (0), not cores
			 * managed by the idlecoremap.  so don't do this yet. */
			put_idle_core(get_pcoreid(p, 0));
			#endif
			break;
		case PROC_RUNNING_M:
			/* Send the DEATH message to every core running this process, and
			 * deallocate the cores.
			 * The rule is that the vcoremap is set before proc_run, and reset
			 * within proc_destroy */
			__proc_take_allcores_dumb(p, FALSE);
			break;
		case PROC_CREATED:
			break;
		default:
			panic("Weird state(%s) in %s()", procstate2str(p->state),
			      __FUNCTION__);
	}
	__proc_set_state(p, PROC_DYING);
	/* This prevents processes from accessing their old files while dying, and
	 * will help if these files (or similar objects in the future) hold
	 * references to p (preventing a __proc_free()). */
	close_all_files(&p->open_files, FALSE);
	/* This decref is for the process's existence. */
	proc_decref(p);
	/* Unlock.  A death IPI should be on its way, either from the RUNNING_S one,
	 * or from proc_take_cores with a __death.  in general, interrupts should be
	 * on when you call proc_destroy locally, but currently aren't for all
	 * things (like traphandlers). */
	spin_unlock(&p->proc_lock);
	return;
}

/* Turns *p into an MCP.  Needs to be called from a local syscall of a RUNNING_S
 * process.  Currently, this ignores whether or not you are an _M already.  You
 * should hold the lock before calling. */
void __proc_switch_to_m(struct proc *p)
{
	int8_t state = 0;
	switch (p->state) {
		case (PROC_RUNNING_S):
			/* issue with if we're async or not (need to preempt it)
			 * either of these should trip it. TODO: (ACR) async core req
			 * TODO: relies on vcore0 being the caller (VC#) */
			if ((current != p) || (get_pcoreid(p, 0) != core_id()))
				panic("We don't handle async RUNNING_S core requests yet.");
			/* save the tf so userspace can restart it.  Like in __notify,
			 * this assumes a user tf is the same as a kernel tf.  We save
			 * it in the preempt slot so that we can also save the silly
			 * state. */
			struct preempt_data *vcpd = &p->procdata->vcore_preempt_data[0];
			disable_irqsave(&state);	/* protect cur_tf */
			/* Note this won't play well with concurrent proc kmsgs, but
			 * since we're _S and locked, we shouldn't have any. */
			assert(current_tf);
			/* Copy uthread0's context to the notif slot */
			vcpd->notif_tf = *current_tf;
			clear_owning_proc(core_id());	/* so we don't restart */
			save_fp_state(&vcpd->preempt_anc);
			enable_irqsave(&state);
			/* Userspace needs to not fuck with notif_disabled before
			 * transitioning to _M. */
			if (vcpd->notif_disabled) {
				printk("[kernel] user bug: notifs disabled for vcore 0\n");
				vcpd->notif_disabled = FALSE;
			}
			/* in the async case, we'll need to remotely stop and bundle
			 * vcore0's TF.  this is already done for the sync case (local
			 * syscall). */
			/* this process no longer runs on its old location (which is
			 * this core, for now, since we don't handle async calls) */
			__seq_start_write(&p->procinfo->coremap_seqctr);
			// TODO: (VC#) might need to adjust num_vcores
			// TODO: (ACR) will need to unmap remotely (receive-side)
			__unmap_vcore(p, 0);	/* VC# keep in sync with proc_run_s */
			__seq_end_write(&p->procinfo->coremap_seqctr);
			/* change to runnable_m (it's TF is already saved) */
			__proc_set_state(p, PROC_RUNNABLE_M);
			p->procinfo->is_mcp = TRUE;
			break;
		case (PROC_RUNNABLE_S):
			/* Issues: being on the runnable_list, proc_set_state not liking
			 * it, and not clearly thinking through how this would happen.
			 * Perhaps an async call that gets serviced after you're
			 * descheduled? */
			panic("Not supporting RUNNABLE_S -> RUNNABLE_M yet.\n");
			break;
		case (PROC_DYING):
			warn("Dying, core request coming from %d\n", core_id());
		default:
			break;
	}
}

/* Old code to turn a RUNNING_M to a RUNNING_S, with the calling context
 * becoming the new 'thread0'.  Don't use this. */
void __proc_switch_to_s(struct proc *p)
{
	int8_t state = 0;
	printk("[kernel] trying to transition _M -> _S (deprecated)!\n");
	assert(p->state == PROC_RUNNING_M); // TODO: (ACR) async core req
	/* save the context, to be restarted in _S mode */
	disable_irqsave(&state);	/* protect cur_tf */
	assert(current_tf);
	p->env_tf = *current_tf;
	clear_owning_proc(core_id());	/* so we don't restart */
	enable_irqsave(&state);
	env_push_ancillary_state(p); // TODO: (HSS)
	/* sending death, since it's not our job to save contexts or anything in
	 * this case.  also, if this returns true, we will not return down
	 * below, and need to eat the reference to p */
	__proc_take_allcores_dumb(p, FALSE);
	__proc_set_state(p, PROC_RUNNABLE_S);
}

/* Helper function.  Is the given pcore a mapped vcore?  No locking involved, be
 * careful. */
static bool is_mapped_vcore(struct proc *p, uint32_t pcoreid)
{
	return p->procinfo->pcoremap[pcoreid].valid;
}

/* Helper function.  Find the vcoreid for a given physical core id for proc p.
 * No locking involved, be careful.  Panics on failure. */
static uint32_t get_vcoreid(struct proc *p, uint32_t pcoreid)
{
	assert(is_mapped_vcore(p, pcoreid));
	return p->procinfo->pcoremap[pcoreid].vcoreid;
}

/* Helper function.  Try to find the pcoreid for a given virtual core id for
 * proc p.  No locking involved, be careful.  Use this when you can tolerate a
 * stale or otherwise 'wrong' answer. */
static uint32_t try_get_pcoreid(struct proc *p, uint32_t vcoreid)
{
	return p->procinfo->vcoremap[vcoreid].pcoreid;
}

/* Helper function.  Find the pcoreid for a given virtual core id for proc p.
 * No locking involved, be careful.  Panics on failure. */
static uint32_t get_pcoreid(struct proc *p, uint32_t vcoreid)
{
	assert(vcore_is_mapped(p, vcoreid));
	return try_get_pcoreid(p, vcoreid);
}

/* Helper function: yields / wraps up current_tf and schedules the _S */
void __proc_yield_s(struct proc *p, struct trapframe *tf)
{
	assert(p->state == PROC_RUNNING_S);
	p->env_tf= *tf;
	env_push_ancillary_state(p);			/* TODO: (HSS) */
	__unmap_vcore(p, 0);	/* VC# keep in sync with proc_run_s */
	__proc_set_state(p, PROC_RUNNABLE_S);
	schedule_scp(p);
}

/* Yields the calling core.  Must be called locally (not async) for now.
 * - If RUNNING_S, you just give up your time slice and will eventually return.
 * - If RUNNING_M, you give up the current vcore (which never returns), and
 *   adjust the amount of cores wanted/granted.
 * - If you have only one vcore, you switch to RUNNABLE_M.  When you run again,
 *   you'll have one guaranteed core, starting from the entry point.
 *
 * - RES_CORES amt_wanted will be the amount running after taking away the
 *   yielder, unless there are none left, in which case it will be 1.
 *
 * If the call is being nice, it means that it is in response to a preemption
 * (which needs to be checked).  If there is no preemption pending, just return.
 * No matter what, don't adjust the number of cores wanted.
 *
 * This usually does not return (smp_idle()), so it will eat your reference.
 * Also note that it needs a non-current/edible reference, since it will abandon
 * and continue to use the *p (current == 0, no cr3, etc).
 *
 * We disable interrupts for most of it too, since we need to protect current_tf
 * and not race with __notify (which doesn't play well with concurrent
 * yielders). */
void proc_yield(struct proc *SAFE p, bool being_nice)
{
	uint32_t vcoreid, pcoreid = core_id();
	struct vcore *vc;
	struct preempt_data *vcpd;
	int8_t state = 0;
	/* Need to disable before even reading vcoreid, since we could be unmapped
	 * by a __preempt or __death.  _S also needs ints disabled, so we'll just do
	 * it immediately. */
	disable_irqsave(&state);
	/* Need to lock before checking the vcoremap to find out who we are, in case
	 * we're getting __preempted and __startcored, from a remote core (in which
	 * case we might have come in thinking we were vcore X, but had X preempted
	 * and Y restarted on this pcore, and we suddenly are the wrong vcore
	 * yielding).  Arguably, this is incredibly rare, since you'd need to
	 * preempt the core, then decide to give it back with another grant in
	 * between. */
	spin_lock(&p->proc_lock); /* horrible scalability.  =( */
	switch (p->state) {
		case (PROC_RUNNING_S):
			__proc_yield_s(p, current_tf);	/* current_tf 0'd in abandon core */
			goto out_yield_core;
		case (PROC_RUNNING_M):
			break;				/* will handle this stuff below */
		case (PROC_DYING):		/* incoming __death */
		case (PROC_RUNNABLE_M):	/* incoming (bulk) preempt/myield TODO:(BULK) */
			goto out_failed;
		default:
			panic("Weird state(%s) in %s()", procstate2str(p->state),
			      __FUNCTION__);
	}
	/* If we're already unmapped (__preempt or a __death hit us), bail out.
	 * Note that if a __death hit us, we should have bailed when we saw
	 * PROC_DYING. */
	if (!is_mapped_vcore(p, pcoreid))
		goto out_failed;
	vcoreid = get_vcoreid(p, pcoreid);
	vc = vcoreid2vcore(p, vcoreid);
	vcpd = &p->procdata->vcore_preempt_data[vcoreid];
	/* no reason to be nice, return */
	if (being_nice && !vc->preempt_pending)
		goto out_failed;
	/* Fate is sealed, return and take the preempt message when we enable_irqs.
	 * Note this keeps us from mucking with our lists, since we were already
	 * removed from the online_list.  We have a similar concern with __death,
	 * but we check for DYING to handle that. */
	if (vc->preempt_served)
		goto out_failed;
	/* At this point, AFAIK there should be no preempt/death messages on the
	 * way, and we're on the online list.  So we'll go ahead and do the yielding
	 * business. */
	/* no need to preempt later, since we are yielding (nice or otherwise) */
	if (vc->preempt_pending)
		vc->preempt_pending = 0;
	/* Don't let them yield if they are missing a notification.  Userspace must
	 * not leave vcore context without dealing with notif_pending.  pop_ros_tf()
	 * handles leaving via uthread context.  This handles leaving via a yield.
	 *
	 * This early check is an optimization.  The real check is below when it
	 * works with the online_vcs list (syncing with event.c and INDIR/IPI
	 * posting). */
	if (vcpd->notif_pending)
		goto out_failed;
	/* Now we'll actually try to yield */
	printd("[K] Process %d (%p) is yielding on vcore %d\n", p->pid, p,
	       get_vcoreid(p, coreid));
	/* Remove from the online list, add to the yielded list, and unmap
	 * the vcore, which gives up the core. */
	TAILQ_REMOVE(&p->online_vcs, vc, list);
	/* Now that we're off the online list, check to see if an alert made
	 * it through (event.c sets this) */
	wrmb();	/* prev write must hit before reading notif_pending */
	/* Note we need interrupts disabled, since a __notify can come in
	 * and set pending to FALSE */
	if (vcpd->notif_pending) {
		/* We lost, put it back on the list and abort the yield */
		TAILQ_INSERT_TAIL(&p->online_vcs, vc, list); /* could go HEAD */
		goto out_failed;
	}
	/* We won the race with event sending, we can safely yield */
	TAILQ_INSERT_HEAD(&p->inactive_vcs, vc, list);
	/* Note this protects stuff userspace should look at, which doesn't
	 * include the TAILQs. */
	__seq_start_write(&p->procinfo->coremap_seqctr);
	/* Next time the vcore starts, it starts fresh */
	vcpd->notif_disabled = FALSE;
	__unmap_vcore(p, vcoreid);
	/* Adjust implied resource desires */
	p->resources[RES_CORES].amt_granted = --(p->procinfo->num_vcores);
	if (!being_nice)
		p->resources[RES_CORES].amt_wanted = p->procinfo->num_vcores;
	__seq_end_write(&p->procinfo->coremap_seqctr);
	/* Hand the now-idle core to the ksched */
	put_idle_core(pcoreid);
	// last vcore?  then we really want 1, and to yield the gang
	if (p->procinfo->num_vcores == 0) {
		p->resources[RES_CORES].amt_wanted = 1;
		/* wait on an event (not supporting 'being nice' for now */
		__proc_set_state(p, PROC_WAITING);
	}
	goto out_yield_core;
out_failed:
	/* for some reason we just want to return, either to take a KMSG that cleans
	 * us up, or because we shouldn't yield (ex: notif_pending). */
	spin_unlock(&p->proc_lock);
	enable_irqsave(&state);
	return;
out_yield_core:			/* successfully yielded the core */
	spin_unlock(&p->proc_lock);
	proc_decref(p);			/* need to eat the ref passed in */
	/* Clean up the core and idle.  Need to do this before enabling interrupts,
	 * since once we put_idle_core() and unlock, we could get a startcore. */
	clear_owning_proc(pcoreid);	/* so we don't restart */
	abandon_core();
	smp_idle();				/* will reenable interrupts */
}

/* Sends a notification (aka active notification, aka IPI) to p's vcore.  We
 * only send a notification if one they are enabled.  There's a bunch of weird
 * cases with this, and how pending / enabled are signals between the user and
 * kernel - check the documentation.  Note that pending is more about messages.
 * The process needs to be in vcore_context, and the reason is usually a
 * message.  We set pending here in case we were called to prod them into vcore
 * context (like via a sys_self_notify. */
void proc_notify(struct proc *p, uint32_t vcoreid)
{
	struct preempt_data *vcpd = &p->procdata->vcore_preempt_data[vcoreid];
	vcpd->notif_pending = TRUE;
	wrmb();	/* must write notif_pending before reading notif_disabled */
	if (!vcpd->notif_disabled) {
		/* GIANT WARNING: we aren't using the proc-lock to protect the
		 * vcoremap.  We want to be able to use this from interrupt context,
		 * and don't want the proc_lock to be an irqsave.  Spurious
		 * __notify() kmsgs are okay (it checks to see if the right receiver
		 * is current). */
		if ((p->state & PROC_RUNNING_M) && // TODO: (VC#) (_S state)
		              vcore_is_mapped(p, vcoreid)) {
			printd("[kernel] sending notif to vcore %d\n", vcoreid);
			/* This use of try_get_pcoreid is racy, might be unmapped */
			send_kernel_message(try_get_pcoreid(p, vcoreid), __notify, (long)p,
			                    0, 0, KMSG_IMMEDIATE);
		}
	}
}

/* Hold the lock before calling this.  If the process is WAITING, it will wake
 * it up and schedule it. */
void __proc_wakeup(struct proc *p)
{
	if (p->state != PROC_WAITING)
		return;
	if (__proc_is_mcp(p)) {
		/* TODO: adjust amt_wanted here, instead of in yield */
		__proc_set_state(p, PROC_RUNNABLE_M);
	} else {
		__proc_set_state(p, PROC_RUNNABLE_S);
		schedule_scp(p);
	}
}

/* Is the process in multi_mode / is an MCP or not?  */
bool __proc_is_mcp(struct proc *p)
{
	/* in lieu of using the amount of cores requested, or having a bunch of
	 * states (like PROC_WAITING_M and _S), I'll just track it with a bool. */
	return p->procinfo->is_mcp;
}

/************************  Preemption Functions  ******************************
 * Don't rely on these much - I'll be sure to change them up a bit.
 *
 * Careful about what takes a vcoreid and what takes a pcoreid.  Also, there may
 * be weird glitches with setting the state to RUNNABLE_M.  It is somewhat in
 * flux.  The num_vcores is changed after take_cores, but some of the messages
 * (or local traps) may not yet be ready to handle seeing their future state.
 * But they should be, so fix those when they pop up.
 *
 * Another thing to do would be to make the _core functions take a pcorelist,
 * and not just one pcoreid. */

/* Sets a preempt_pending warning for p's vcore, to go off 'when'.  If you care
 * about locking, do it before calling.  Takes a vcoreid! */
void __proc_preempt_warn(struct proc *p, uint32_t vcoreid, uint64_t when)
{
	struct event_msg local_msg = {0};
	/* danger with doing this unlocked: preempt_pending is set, but never 0'd,
	 * since it is unmapped and not dealt with (TODO)*/
	p->procinfo->vcoremap[vcoreid].preempt_pending = when;

	/* Send the event (which internally checks to see how they want it) */
	local_msg.ev_type = EV_PREEMPT_PENDING;
	local_msg.ev_arg1 = vcoreid;
	send_kernel_event(p, &local_msg, vcoreid);

	/* TODO: consider putting in some lookup place for the alarm to find it.
	 * til then, it'll have to scan the vcoremap (O(n) instead of O(m)) */
}

/* Warns all active vcores of an impending preemption.  Hold the lock if you
 * care about the mapping (and you should). */
void __proc_preempt_warnall(struct proc *p, uint64_t when)
{
	struct vcore *vc_i;
	TAILQ_FOREACH(vc_i, &p->online_vcs, list)
		__proc_preempt_warn(p, vcore2vcoreid(p, vc_i), when);
	/* TODO: consider putting in some lookup place for the alarm to find it.
	 * til then, it'll have to scan the vcoremap (O(n) instead of O(m)) */
}

// TODO: function to set an alarm, if none is outstanding

/* Raw function to preempt a single core.  If you care about locking, do it
 * before calling. */
void __proc_preempt_core(struct proc *p, uint32_t pcoreid)
{
	uint32_t vcoreid = get_vcoreid(p, pcoreid);
	struct event_msg preempt_msg = {0};
	p->procinfo->vcoremap[vcoreid].preempt_served = TRUE;
	// expects a pcorelist.  assumes pcore is mapped and running_m
	__proc_take_corelist(p, &pcoreid, 1, TRUE);
	/* Send a message about the preemption. */
	preempt_msg.ev_type = EV_VCORE_PREEMPT;
	preempt_msg.ev_arg2 = vcoreid;
	send_kernel_event(p, &preempt_msg, 0);
}

/* Raw function to preempt every vcore.  If you care about locking, do it before
 * calling. */
void __proc_preempt_all(struct proc *p)
{
	/* instead of doing this, we could just preempt_served all possible vcores,
	 * and not just the active ones.  We would need to sort out a way to deal
	 * with stale preempt_serveds first.  This might be just as fast anyways. */
	struct vcore *vc_i;
	/* TODO:(BULK) PREEMPT - don't bother with this, set a proc wide flag, or
	 * just make us RUNNABLE_M. */
	TAILQ_FOREACH(vc_i, &p->online_vcs, list)
		vc_i->preempt_served = TRUE;
	__proc_take_allcores_dumb(p, TRUE);
}

/* Warns and preempts a vcore from p.  No delaying / alarming, or anything.  The
 * warning will be for u usec from now. */
void proc_preempt_core(struct proc *p, uint32_t pcoreid, uint64_t usec)
{
	uint64_t warn_time = read_tsc() + usec2tsc(usec);

	/* DYING could be okay */
	if (p->state != PROC_RUNNING_M) {
		warn("Tried to preempt from a non RUNNING_M proc!");
		return;
	}
	spin_lock(&p->proc_lock);
	if (is_mapped_vcore(p, pcoreid)) {
		__proc_preempt_warn(p, get_vcoreid(p, pcoreid), warn_time);
		__proc_preempt_core(p, pcoreid);
		put_idle_core(pcoreid);
	} else {
		warn("Pcore doesn't belong to the process!!");
	}
	if (!p->procinfo->num_vcores) {
		__proc_set_state(p, PROC_RUNNABLE_M);
	}
	spin_unlock(&p->proc_lock);
}

/* Warns and preempts all from p.  No delaying / alarming, or anything.  The
 * warning will be for u usec from now. */
void proc_preempt_all(struct proc *p, uint64_t usec)
{
	uint64_t warn_time = read_tsc() + usec2tsc(usec);

	spin_lock(&p->proc_lock);
	/* DYING could be okay */
	if (p->state != PROC_RUNNING_M) {
		warn("Tried to preempt from a non RUNNING_M proc!");
		spin_unlock(&p->proc_lock);
		return;
	}
	__proc_preempt_warnall(p, warn_time);
	__proc_preempt_all(p);
	assert(!p->procinfo->num_vcores);
	__proc_set_state(p, PROC_RUNNABLE_M);
	spin_unlock(&p->proc_lock);
}

/* Give the specific pcore to proc p.  Lots of assumptions, so don't really use
 * this.  The proc needs to be _M and prepared for it.  the pcore needs to be
 * free, etc. */
void proc_give(struct proc *p, uint32_t pcoreid)
{
	warn("Your idlecoremap is now screwed up");	/* TODO (IDLE) */
	spin_lock(&p->proc_lock);
	// expects a pcorelist, we give it a list of one
	__proc_give_cores(p, &pcoreid, 1);
	spin_unlock(&p->proc_lock);
}

/* Global version of the helper, for sys_get_vcoreid (might phase that syscall
 * out). */
uint32_t proc_get_vcoreid(struct proc *SAFE p, uint32_t pcoreid)
{
	uint32_t vcoreid;
	// TODO: the code currently doesn't track the vcoreid properly for _S (VC#)
	spin_lock(&p->proc_lock);
	switch (p->state) {
		case PROC_RUNNING_S:
			spin_unlock(&p->proc_lock);
			return 0; // TODO: here's the ugly part
		case PROC_RUNNING_M:
			vcoreid = get_vcoreid(p, pcoreid);
			spin_unlock(&p->proc_lock);
			return vcoreid;
		case PROC_DYING: // death message is on the way
			spin_unlock(&p->proc_lock);
			return 0;
		default:
			spin_unlock(&p->proc_lock);
			panic("Weird state(%s) in %s()", procstate2str(p->state),
			      __FUNCTION__);
	}
}

/* TODO: make all of these static inlines when we gut the env crap */
bool vcore_is_mapped(struct proc *p, uint32_t vcoreid)
{
	return p->procinfo->vcoremap[vcoreid].valid;
}

/* Can do this, or just create a new field and save it in the vcoremap */
uint32_t vcore2vcoreid(struct proc *p, struct vcore *vc)
{
	return (vc - p->procinfo->vcoremap);
}

struct vcore *vcoreid2vcore(struct proc *p, uint32_t vcoreid)
{
	return &p->procinfo->vcoremap[vcoreid];
}

/********** Core granting (bulk and single) ***********/

/* Helper: gives pcore to the process, mapping it to the next available vcore
 * from list vc_list.  Returns TRUE if we succeeded (non-empty). */
static bool __proc_give_a_pcore(struct proc *p, uint32_t pcore,
                                struct vcore_tailq *vc_list)
{
	struct vcore *new_vc;
	new_vc = TAILQ_FIRST(vc_list);
	if (!new_vc)
		return FALSE;
	printd("setting vcore %d to pcore %d\n", vcore2vcoreid(p, new_vc),
	       pcorelist[i]);
	TAILQ_REMOVE(vc_list, new_vc, list);
	TAILQ_INSERT_TAIL(&p->online_vcs, new_vc, list);
	__map_vcore(p, vcore2vcoreid(p, new_vc), pcore);
	return TRUE;
}

static void __proc_give_cores_runnable(struct proc *p, uint32_t *pc_arr,
                                       uint32_t num)
{
	assert(p->state == PROC_RUNNABLE_M);
	assert(num);	/* catch bugs */
	/* add new items to the vcoremap */
	__seq_start_write(&p->procinfo->coremap_seqctr);/* unncessary if offline */
	p->procinfo->num_vcores += num;
	for (int i = 0; i < num; i++) {
		/* Try from the bulk list first */
		if (__proc_give_a_pcore(p, pc_arr[i], &p->bulk_preempted_vcs))
			continue;
		/* o/w, try from the inactive list.  at one point, i thought there might
		 * be a legit way in which the inactive list could be empty, but that i
		 * wanted to catch it via an assert. */
		assert(__proc_give_a_pcore(p, pc_arr[i], &p->inactive_vcs));
	}
	__seq_end_write(&p->procinfo->coremap_seqctr);
}

static void __proc_give_cores_running(struct proc *p, uint32_t *pc_arr,
                                      uint32_t num)
{
	/* Up the refcnt, since num cores are going to start using this
	 * process and have it loaded in their owning_proc and 'current'. */
	proc_incref(p, num * 2);	/* keep in sync with __startcore */
	__seq_start_write(&p->procinfo->coremap_seqctr);
	p->procinfo->num_vcores += num;
	assert(TAILQ_EMPTY(&p->bulk_preempted_vcs));
	for (int i = 0; i < num; i++) {
		assert(__proc_give_a_pcore(p, pc_arr[i], &p->inactive_vcs));
		send_kernel_message(pc_arr[i], __startcore, (long)p, 0, 0,
		                    KMSG_IMMEDIATE);
	}
	__seq_end_write(&p->procinfo->coremap_seqctr);
}

/* Gives process p the additional num cores listed in pcorelist.  You must be
 * RUNNABLE_M or RUNNING_M before calling this.  If you're RUNNING_M, this will
 * startup your new cores at the entry point with their virtual IDs (or restore
 * a preemption).  If you're RUNNABLE_M, you should call __proc_run_m after this
 * so that the process can start to use its cores.
 *
 * If you're *_S, make sure your core0's TF is set (which is done when coming in
 * via arch/trap.c and we are RUNNING_S), change your state, then call this.
 * Then call __proc_run_m().
 *
 * The reason I didn't bring the _S cases from core_request over here is so we
 * can keep this family of calls dealing with only *_Ms, to avoiding caring if
 * this is called from another core, and to avoid the _S -> _M transition.
 *
 * WARNING: You must hold the proc_lock before calling this! */
void __proc_give_cores(struct proc *p, uint32_t *pc_arr, uint32_t num)
{
	/* should never happen: */
	assert(num + p->procinfo->num_vcores <= MAX_NUM_CPUS);
	switch (p->state) {
		case (PROC_RUNNABLE_S):
		case (PROC_RUNNING_S):
			panic("Don't give cores to a process in a *_S state!\n");
			break;
		case (PROC_DYING):
		case (PROC_WAITING):
			/* can't accept, give the cores back to the ksched and return */
			for (int i = 0; i < num; i++)
				put_idle_core(pc_arr[i]);
			return;
		case (PROC_RUNNABLE_M):
			__proc_give_cores_runnable(p, pc_arr, num);
			break;
		case (PROC_RUNNING_M):
			__proc_give_cores_running(p, pc_arr, num);
			break;
		default:
			panic("Weird state(%s) in %s()", procstate2str(p->state),
			      __FUNCTION__);
	}
	p->resources[RES_CORES].amt_granted += num;
}

/********** Core revocation (bulk and single) ***********/

/* Revokes a single vcore from a process (unmaps or sends a KMSG to unmap). */
static void __proc_revoke_core(struct proc *p, uint32_t vcoreid, bool preempt)
{
	uint32_t pcoreid = get_pcoreid(p, vcoreid);
	struct preempt_data *vcpd;
	if (preempt) {
		/* Lock the vcore's state (necessary for preemption recovery) */
		vcpd = &p->procdata->vcore_preempt_data[vcoreid];
		atomic_or(&vcpd->flags, VC_K_LOCK);
		send_kernel_message(pcoreid, __preempt, (long)p, 0, 0, KMSG_IMMEDIATE);
	} else {
		send_kernel_message(pcoreid, __death, 0, 0, 0, KMSG_IMMEDIATE);
	}
}

/* Revokes all cores from the process (unmaps or sends a KMSGS). */
static void __proc_revoke_allcores(struct proc *p, bool preempt)
{
	struct vcore *vc_i;
	/* TODO: if we ever get broadcast messaging, use it here (still need to lock
	 * the vcores' states for preemption) */
	TAILQ_FOREACH(vc_i, &p->online_vcs, list)
		__proc_revoke_core(p, vcore2vcoreid(p, vc_i), preempt);
}

/* Might be faster to scan the vcoremap than to walk the list... */
static void __proc_unmap_allcores(struct proc *p)
{
	struct vcore *vc_i;
	TAILQ_FOREACH(vc_i, &p->online_vcs, list)
		__unmap_vcore(p, vcore2vcoreid(p, vc_i));
}

/* Takes (revoke via kmsg or unmap) from process p the num cores listed in
 * pc_arr.  Will preempt if 'preempt' is set.  o/w, no state will be saved, etc.
 * Don't use this for taking all of a process's cores.
 *
 * Make sure you hold the lock when you call this. */
void __proc_take_corelist(struct proc *p, uint32_t *pc_arr, uint32_t num,
                          bool preempt)
{
	struct vcore *vc;
	uint32_t vcoreid;
	__seq_start_write(&p->procinfo->coremap_seqctr);
	for (int i = 0; i < num; i++) {
		vcoreid = get_vcoreid(p, pc_arr[i]);
		/* Sanity check */
		assert(pc_arr[i] == get_pcoreid(p, vcoreid));
		/* Revoke / unmap core */
		if (p->state == PROC_RUNNING_M) {
			__proc_revoke_core(p, vcoreid, preempt);
		} else {
			assert(p->state == PROC_RUNNABLE_M);
			__unmap_vcore(p, vcoreid);
		}
		/* Change lists for the vcore.  Note, the messages are already in flight
		 * (or the vcore is already unmapped), if applicable.  The only code
		 * that looks at the lists without holding the lock is event code, and
		 * it doesn't care if the vcore was unmapped (it handles that) */
		vc = vcoreid2vcore(p, vcoreid);
		TAILQ_REMOVE(&p->online_vcs, vc, list);
		/* even for single preempts, we use the inactive list.  bulk preempt is
		 * only used for when we take everything. */
		TAILQ_INSERT_HEAD(&p->inactive_vcs, vc, list);
	}
	p->procinfo->num_vcores -= num;
	__seq_end_write(&p->procinfo->coremap_seqctr);
	p->resources[RES_CORES].amt_granted -= num;
}

/* Takes all cores from a process (revoke via kmsg or unmap), putting them on
 * the appropriate vcore list, and fills pc_arr with the pcores revoked, and
 * returns the number of entries in pc_arr.
 *
 * Make sure pc_arr is big enough to handle num_vcores().
 * Make sure you hold the lock when you call this. */
uint32_t __proc_take_allcores(struct proc *p, uint32_t *pc_arr, bool preempt)
{
	struct vcore *vc_i, *vc_temp;
	uint32_t num = 0;
	__seq_start_write(&p->procinfo->coremap_seqctr);
	/* Write out which pcores we're going to take */
	TAILQ_FOREACH(vc_i, &p->online_vcs, list)
		pc_arr[num++] = vc_i->pcoreid;
	/* Revoke if they are running, o/w unmap.  Both of these need the online
	 * list to not be changed yet. */
	if (p->state == PROC_RUNNING_M) {
		__proc_revoke_allcores(p, preempt);
	} else {
		assert(p->state == PROC_RUNNABLE_M);
		__proc_unmap_allcores(p);
	}
	/* Move the vcores from online to the head of the appropriate list */
	TAILQ_FOREACH_SAFE(vc_i, &p->online_vcs, list, vc_temp) {
		/* TODO: we may want a TAILQ_CONCAT_HEAD, or something that does that */
		TAILQ_REMOVE(&p->online_vcs, vc_i, list);
		/* Put the cores on the appropriate list */
		if (preempt)
			TAILQ_INSERT_HEAD(&p->bulk_preempted_vcs, vc_i, list);
		else
			TAILQ_INSERT_HEAD(&p->inactive_vcs, vc_i, list);
	}
	assert(TAILQ_EMPTY(&p->online_vcs));
	assert(num == p->procinfo->num_vcores);
	p->procinfo->num_vcores = 0;
	__seq_end_write(&p->procinfo->coremap_seqctr);
	p->resources[RES_CORES].amt_granted = 0;
	return num;
}

/* Dumb legacy helper, simply takes all cores and just puts them on the idle
 * core map (which belongs in the scheduler.
 *
 * TODO: no one should call this; the ksched should handle this internally */
void __proc_take_allcores_dumb(struct proc *p, bool preempt)
{
	uint32_t num_revoked;
	uint32_t pc_arr[p->procinfo->num_vcores];
	num_revoked = __proc_take_allcores(p, pc_arr, preempt);
	for (int i = 0; i < num_revoked; i++)
		put_idle_core(pc_arr[i]);
}

/* Helper to do the vcore->pcore and inverse mapping.  Hold the lock when
 * calling. */
void __map_vcore(struct proc *p, uint32_t vcoreid, uint32_t pcoreid)
{
	while (p->procinfo->vcoremap[vcoreid].valid)
		cpu_relax();
	p->procinfo->vcoremap[vcoreid].pcoreid = pcoreid;
	wmb();
	p->procinfo->vcoremap[vcoreid].valid = TRUE;
	p->procinfo->pcoremap[pcoreid].vcoreid = vcoreid;
	wmb();
	p->procinfo->pcoremap[pcoreid].valid = TRUE;
}

/* Helper to unmap the vcore->pcore and inverse mapping.  Hold the lock when
 * calling. */
void __unmap_vcore(struct proc *p, uint32_t vcoreid)
{
	p->procinfo->pcoremap[p->procinfo->vcoremap[vcoreid].pcoreid].valid = FALSE;
	wmb();
	p->procinfo->vcoremap[vcoreid].valid = FALSE;
}

/* Stop running whatever context is on this core and load a known-good cr3.
 * Note this leaves no trace of what was running. This "leaves the process's
 * context.  Also, we want interrupts disabled, to not conflict with kmsgs
 * (__launch_kthread, proc mgmt, etc).
 *
 * This does not clear the owning proc.  Use the other helper for that. */
void abandon_core(void)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	assert(!irq_is_enabled());
	/* Syscalls that don't return will ultimately call abadon_core(), so we need
	 * to make sure we don't think we are still working on a syscall. */
	pcpui->cur_sysc = 0;
	if (pcpui->cur_proc)
		__abandon_core();
}

/* Helper to clear the core's owning processor and manage refcnting.  Pass in
 * core_id() to save a couple core_id() calls. */
void clear_owning_proc(uint32_t coreid)
{
	struct per_cpu_info *pcpui = &per_cpu_info[coreid];
	struct proc *p = pcpui->owning_proc;
	assert(!irq_is_enabled());
	pcpui->owning_proc = 0;
	pcpui->cur_tf = 0;			/* catch bugs for now (will go away soon) */
	if (p);
		proc_decref(p);
}

/* Switches to the address space/context of new_p, doing nothing if we are
 * already in new_p.  This won't add extra refcnts or anything, and needs to be
 * paired with switch_back() at the end of whatever function you are in.  Don't
 * migrate cores in the middle of a pair.  Specifically, the uncounted refs are
 * one for the old_proc, which is passed back to the caller, and new_p is
 * getting placed in cur_proc. */
struct proc *switch_to(struct proc *new_p)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	struct proc *old_proc;
	int8_t irq_state = 0;
	disable_irqsave(&irq_state);
	old_proc = pcpui->cur_proc;					/* uncounted ref */
	/* If we aren't the proc already, then switch to it */
	if (old_proc != new_p) {
		pcpui->cur_proc = new_p;				/* uncounted ref */
		lcr3(new_p->env_cr3);
	}
	enable_irqsave(&irq_state);
	return old_proc;
}

/* This switches back to old_proc from new_p.  Pair it with switch_to(), and
 * pass in its return value for old_proc. */
void switch_back(struct proc *new_p, struct proc *old_proc)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	int8_t irq_state = 0;
	if (old_proc != new_p) {
		disable_irqsave(&irq_state);
		pcpui->cur_proc = old_proc;
		if (old_proc)
			lcr3(old_proc->env_cr3);
		else
			lcr3(boot_cr3);
		enable_irqsave(&irq_state);
	}
}

/* Will send a TLB shootdown message to every vcore in the main address space
 * (aka, all vcores for now).  The message will take the start and end virtual
 * addresses as well, in case we want to be more clever about how much we
 * shootdown and batching our messages.  Should do the sanity about rounding up
 * and down in this function too.
 *
 * Would be nice to have a broadcast kmsg at this point.  Note this may send a
 * message to the calling core (interrupting it, possibly while holding the
 * proc_lock).  We don't need to process routine messages since it's an
 * immediate message. */
void proc_tlbshootdown(struct proc *p, uintptr_t start, uintptr_t end)
{
	struct vcore *vc_i;
	/* TODO: we might be able to avoid locking here in the future (we must hit
	 * all online, and we can check __mapped).  it'll be complicated. */
	spin_lock(&p->proc_lock);
	switch (p->state) {
		case (PROC_RUNNING_S):
			tlbflush();
			break;
		case (PROC_RUNNING_M):
			/* TODO: (TLB) sanity checks and rounding on the ranges */
			TAILQ_FOREACH(vc_i, &p->online_vcs, list) {
				send_kernel_message(vc_i->pcoreid, __tlbshootdown, start, end,
				                    0, KMSG_IMMEDIATE);
			}
			break;
		case (PROC_DYING):
			/* if it is dying, death messages are already on the way to all
			 * cores, including ours, which will clear the TLB. */
			break;
		default:
			/* will probably get this when we have the short handlers */
			warn("Unexpected case %s in %s", procstate2str(p->state),
			     __FUNCTION__);
	}
	spin_unlock(&p->proc_lock);
}

/* Helper, used by __startcore and change_to_vcore, which sets up cur_tf to run
 * a given process's vcore.  Caller needs to set up things like owning_proc and
 * whatnot.  Note that we might not have p loaded as current. */
static void __set_curtf_to_vcoreid(struct proc *p, uint32_t vcoreid)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	struct preempt_data *vcpd = &p->procdata->vcore_preempt_data[vcoreid];

	/* We could let userspace do this, though they come into vcore entry many
	 * times, and we just need this to happen when the cores comes online the
	 * first time.  That, and they want this turned on as soon as we know a
	 * vcore *WILL* be online.  We could also do this earlier, when we map the
	 * vcore to its pcore, though we don't always have current loaded or
	 * otherwise mess with the VCPD in those code paths. */
	vcpd->can_rcv_msg = TRUE;
	/* Mark that this vcore as no longer preempted.  No danger of clobbering
	 * other writes, since this would get turned on in __preempt (which can't be
	 * concurrent with this function on this core), and the atomic is just
	 * toggling the one bit (a concurrent VC_K_LOCK will work) */
	atomic_and(&vcpd->flags, ~VC_PREEMPTED);
	printd("[kernel] startcore on physical core %d for process %d's vcore %d\n",
	       core_id(), p->pid, vcoreid);
	/* If notifs are disabled, the vcore was in vcore context and we need to
	 * restart the preempt_tf.  o/w, we give them a fresh vcore (which is also
	 * what happens the first time a vcore comes online).  No matter what,
	 * they'll restart in vcore context.  It's just a matter of whether or not
	 * it is the old, interrupted vcore context. */
	if (vcpd->notif_disabled) {
		restore_fp_state(&vcpd->preempt_anc);
		/* copy-in the tf we'll pop, then set all security-related fields */
		pcpui->actual_tf = vcpd->preempt_tf;
		proc_secure_trapframe(&pcpui->actual_tf);
	} else { /* not restarting from a preemption, use a fresh vcore */
		assert(vcpd->transition_stack);
		/* TODO: consider 0'ing the FP state.  We're probably leaking. */
		proc_init_trapframe(&pcpui->actual_tf, vcoreid, p->env_entry,
		                    vcpd->transition_stack);
		/* Disable/mask active notifications for fresh vcores */
		vcpd->notif_disabled = TRUE;
	}
	/* cur_tf was built above (in actual_tf), now use it */
	pcpui->cur_tf = &pcpui->actual_tf;
	/* this cur_tf will get run when the kernel returns / idles */
}

/* Changes calling vcore to be vcoreid.  enable_my_notif tells us about how the
 * state calling vcore wants to be left in.  It will look like caller_vcoreid
 * was preempted.  Note we don't care about notif_pending.  */
void proc_change_to_vcore(struct proc *p, uint32_t new_vcoreid,
                          bool enable_my_notif)
{
	uint32_t caller_vcoreid, pcoreid = core_id();
	struct preempt_data *caller_vcpd;
	struct vcore *caller_vc, *new_vc;
	struct event_msg preempt_msg = {0};
	int8_t state = 0;
	/* Need to disable before even reading caller_vcoreid, since we could be
	 * unmapped by a __preempt or __death, like in yield. */
	disable_irqsave(&state);
	/* Need to lock before reading the vcoremap, like in yield */
	spin_lock(&p->proc_lock);
	/* new_vcoreid is already runing, abort */
	if (vcore_is_mapped(p, new_vcoreid))
		goto out_failed;
	/* Need to make sure our vcore is allowed to switch.  We might have a
	 * __preempt, __death, etc, coming in.  Similar to yield. */
	switch (p->state) {
		case (PROC_RUNNING_M):
			break;				/* the only case we can proceed */
		case (PROC_RUNNING_S):	/* user bug, just return */
		case (PROC_DYING):		/* incoming __death */
		case (PROC_RUNNABLE_M):	/* incoming (bulk) preempt/myield TODO:(BULK) */
			goto out_failed;
		default:
			panic("Weird state(%s) in %s()", procstate2str(p->state),
			      __FUNCTION__);
	}
	/* Make sure we're still mapped in the proc. */
	if (!is_mapped_vcore(p, pcoreid))
		goto out_failed;
	/* Get all our info */
	caller_vcoreid = get_vcoreid(p, pcoreid);
	caller_vcpd = &p->procdata->vcore_preempt_data[caller_vcoreid];
	caller_vc = vcoreid2vcore(p, caller_vcoreid);
	/* Should only call from vcore context */
	if (!caller_vcpd->notif_disabled) {
		printk("[kernel] You tried to change vcores from uthread ctx\n");
		goto out_failed;
	}
	/* Return and take the preempt message when we enable_irqs. */
	if (caller_vc->preempt_served)
		goto out_failed;
	/* Ok, we're clear to do the switch.  Lets figure out who the new one is */
	new_vc = vcoreid2vcore(p, new_vcoreid);
	printd("[kernel] changing vcore %d to vcore %d\n", caller_vcoreid,
	       new_vcoreid);
	/* enable_my_notif signals how we'll be restarted */
	if (enable_my_notif) {
		/* if they set this flag, then the vcore can just restart from scratch,
		 * and we don't care about either the notif_tf or the preempt_tf. */
		caller_vcpd->notif_disabled = FALSE;
	} else {
		/* need to set up the calling vcore's tf so that it'll get restarted by
		 * __startcore, to make the caller look like it was preempted. */
		caller_vcpd->preempt_tf = *current_tf;
		save_fp_state(&caller_vcpd->preempt_anc);
		/* Mark our core as preempted (for userspace recovery). */
		atomic_or(&caller_vcpd->flags, VC_PREEMPTED);
	}
	/* Either way, unmap and offline our current vcore */
	/* Move the caller from online to inactive */
	TAILQ_REMOVE(&p->online_vcs, caller_vc, list);
	/* We don't bother with the notif_pending race.  note that notif_pending
	 * could still be set.  this was a preempted vcore, and userspace will need
	 * to deal with missed messages (preempt_recover() will handle that) */
	TAILQ_INSERT_HEAD(&p->inactive_vcs, caller_vc, list);
	/* Move the new one from inactive to online */
	TAILQ_REMOVE(&p->inactive_vcs, new_vc, list);
	TAILQ_INSERT_TAIL(&p->online_vcs, new_vc, list);
	/* Change the vcore map (TODO: might get rid of this seqctr) */
	__seq_start_write(&p->procinfo->coremap_seqctr);
	__unmap_vcore(p, caller_vcoreid);
	__map_vcore(p, new_vcoreid, pcoreid);
	__seq_end_write(&p->procinfo->coremap_seqctr);
	/* Send either a PREEMPT msg or a CHECK_MSGS msg.  If they said to
	 * enable_my_notif, then all userspace needs is to check messages, not a
	 * full preemption recovery. */
	preempt_msg.ev_type = (enable_my_notif ? EV_CHECK_MSGS : EV_VCORE_PREEMPT);
	preempt_msg.ev_arg2 = caller_vcoreid;	/* arg2 is 32 bits */
	send_kernel_event(p, &preempt_msg, new_vcoreid);
	/* Change cur_tf so we'll be the new vcoreid */
	__set_curtf_to_vcoreid(p, new_vcoreid);
	/* Fall through to exit (we didn't fail) */
out_failed:
	spin_unlock(&p->proc_lock);
	enable_irqsave(&state);
}

/* Kernel message handler to start a process's context on this core, when the
 * core next considers running a process.  Tightly coupled with __proc_run_m().
 * Interrupts are disabled. */
void __startcore(struct trapframe *tf, uint32_t srcid, long a0, long a1, long a2)
{
	uint32_t vcoreid, coreid = core_id();
	struct per_cpu_info *pcpui = &per_cpu_info[coreid];
	struct proc *p_to_run = (struct proc *CT(1))a0;

	assert(p_to_run);
	/* Can not be any TF from a process here already */
	assert(!pcpui->owning_proc);
	/* the sender of the amsg increfed already for this saved ref to p_to_run */
	pcpui->owning_proc = p_to_run;
	/* sender increfed again, assuming we'd install to cur_proc.  only do this
	 * if no one else is there.  this is an optimization, since we expect to
	 * send these __startcores to idles cores, and this saves a scramble to
	 * incref when all of the cores restartcore/startcore later.  Keep in sync
	 * with __proc_give_cores() and __proc_run_m(). */
	if (!pcpui->cur_proc) {
		pcpui->cur_proc = p_to_run;	/* install the ref to cur_proc */
		lcr3(p_to_run->env_cr3);	/* load the page tables to match cur_proc */
	} else {
		proc_decref(p_to_run);		/* can't install, decref the extra one */
	}
	/* Note we are not necessarily in the cr3 of p_to_run */
	vcoreid = get_vcoreid(p_to_run, coreid);
	/* Now that we sorted refcnts and know p / which vcore it should be, set up
	 * pcpui->cur_tf so that it will run that particular vcore */
	__set_curtf_to_vcoreid(p_to_run, vcoreid);
}

/* Bail out if it's the wrong process, or if they no longer want a notif.  Don't
 * use the TF we passed in, we care about cur_tf.  Try not to grab locks or
 * write access to anything that isn't per-core in here. */
void __notify(struct trapframe *tf, uint32_t srcid, long a0, long a1, long a2)
{
	uint32_t vcoreid, coreid = core_id();
	struct per_cpu_info *pcpui = &per_cpu_info[coreid];
	struct preempt_data *vcpd;
	struct proc *p = (struct proc*)a0;

	/* Not the right proc */
	if (p != pcpui->owning_proc)
		return;
	/* Common cur_tf sanity checks */
	assert(pcpui->cur_tf);
	assert(pcpui->cur_tf == &pcpui->actual_tf);
	assert(!in_kernel(pcpui->cur_tf));
	/* We shouldn't need to lock here, since unmapping happens on the pcore and
	 * mapping would only happen if the vcore was free, which it isn't until
	 * after we unmap. */
	vcoreid = get_vcoreid(p, coreid);
	vcpd = &p->procdata->vcore_preempt_data[vcoreid];
	printd("received active notification for proc %d's vcore %d on pcore %d\n",
	       p->procinfo->pid, vcoreid, coreid);
	/* sort signals.  notifs are now masked, like an interrupt gate */
	if (vcpd->notif_disabled)
		return;
	vcpd->notif_disabled = TRUE;
	/* This bit shouldn't be important anymore */
	vcpd->notif_pending = FALSE; // no longer pending - it made it here
	/* save the old tf in the notify slot, build and pop a new one.  Note that
	 * silly state isn't our business for a notification. */
	vcpd->notif_tf = *pcpui->cur_tf;
	memset(pcpui->cur_tf, 0, sizeof(struct trapframe));
	proc_init_trapframe(pcpui->cur_tf, vcoreid, p->env_entry,
	                    vcpd->transition_stack);
	/* this cur_tf will get run when the kernel returns / idles */
}

void __preempt(struct trapframe *tf, uint32_t srcid, long a0, long a1, long a2)
{
	uint32_t vcoreid, coreid = core_id();
	struct per_cpu_info *pcpui = &per_cpu_info[coreid];
	struct preempt_data *vcpd;
	struct proc *p = (struct proc*)a0;

	assert(p);
	if (p != pcpui->owning_proc) {
		panic("__preempt arrived for a process (%p) that was not owning (%p)!",
		      p, pcpui->owning_proc);
	}
	/* Common cur_tf sanity checks */
	assert(pcpui->cur_tf);
	assert(pcpui->cur_tf == &pcpui->actual_tf);
	assert(!in_kernel(pcpui->cur_tf));
	/* We shouldn't need to lock here, since unmapping happens on the pcore and
	 * mapping would only happen if the vcore was free, which it isn't until
	 * after we unmap. */
	vcoreid = get_vcoreid(p, coreid);
	p->procinfo->vcoremap[vcoreid].preempt_served = FALSE;
	/* either __preempt or proc_yield() ends the preempt phase. */
	p->procinfo->vcoremap[vcoreid].preempt_pending = 0;
	vcpd = &p->procdata->vcore_preempt_data[vcoreid];
	printd("[kernel] received __preempt for proc %d's vcore %d on pcore %d\n",
	       p->procinfo->pid, vcoreid, coreid);
	/* if notifs are disabled, the vcore is in vcore context (as far as we're
	 * concerned), and we save it in the preempt slot. o/w, we save the
	 * process's cur_tf in the notif slot, and it'll appear to the vcore when it
	 * comes back up that it just took a notification. */
	if (vcpd->notif_disabled)
		vcpd->preempt_tf = *pcpui->cur_tf;
	else
		vcpd->notif_tf = *pcpui->cur_tf;
	/* either way, we save the silly state (FP) */
	save_fp_state(&vcpd->preempt_anc);
	/* Mark the vcore as preempted and unlock (was locked by the sender). */
	atomic_or(&vcpd->flags, VC_PREEMPTED);
	atomic_and(&vcpd->flags, ~VC_K_LOCK);
	wmb();	/* make sure everything else hits before we unmap */
	__unmap_vcore(p, vcoreid);
	/* We won't restart the process later.  current gets cleared later when we
	 * notice there is no owning_proc and we have nothing to do (smp_idle,
	 * restartcore, etc) */
	clear_owning_proc(coreid);
}

/* Kernel message handler to clean up the core when a process is dying.
 * Note this leaves no trace of what was running.
 * It's okay if death comes to a core that's already idling and has no current.
 * It could happen if a process decref'd before __proc_startcore could incref. */
void __death(struct trapframe *tf, uint32_t srcid, long a0, long a1, long a2)
{
	uint32_t vcoreid, coreid = core_id();
	struct per_cpu_info *pcpui = &per_cpu_info[coreid];
	struct proc *p = pcpui->owning_proc;
	if (p) {
		vcoreid = get_vcoreid(p, coreid);
		printd("[kernel] death on physical core %d for process %d's vcore %d\n",
		       coreid, p->pid, vcoreid);
		__unmap_vcore(p, vcoreid);
		/* We won't restart the process later.  current gets cleared later when
		 * we notice there is no owning_proc and we have nothing to do
		 * (smp_idle, restartcore, etc) */
		clear_owning_proc(coreid);
	}
}

/* Kernel message handler, usually sent IMMEDIATE, to shoot down virtual
 * addresses from a0 to a1. */
void __tlbshootdown(struct trapframe *tf, uint32_t srcid, long a0, long a1,
                    long a2)
{
	/* TODO: (TLB) something more intelligent with the range */
	tlbflush();
}

void print_allpids(void)
{
	void print_proc_state(void *item)
	{
		struct proc *p = (struct proc*)item;
		assert(p);
		printk("%8d %s\n", p->pid, procstate2str(p->state));
	}
	printk("PID      STATE    \n");
	printk("------------------\n");
	spin_lock(&pid_hash_lock);
	hash_for_each(pid_hash, print_proc_state);
	spin_unlock(&pid_hash_lock);
}

void print_proc_info(pid_t pid)
{
	int j = 0;
	struct proc *p = pid2proc(pid);
	struct vcore *vc_i;
	if (!p) {
		printk("Bad PID.\n");
		return;
	}
	spinlock_debug(&p->proc_lock);
	//spin_lock(&p->proc_lock); // No locking!!
	printk("struct proc: %p\n", p);
	printk("PID: %d\n", p->pid);
	printk("PPID: %d\n", p->ppid);
	printk("State: %s (%p)\n", procstate2str(p->state), p->state);
	printk("Refcnt: %d\n", atomic_read(&p->p_kref.refcount) - 1);
	printk("Flags: 0x%08x\n", p->env_flags);
	printk("CR3(phys): 0x%08x\n", p->env_cr3);
	printk("Num Vcores: %d\n", p->procinfo->num_vcores);
	printk("Vcore Lists (may be in flux w/o locking):\n----------------------\n");
	printk("Online:\n");
	TAILQ_FOREACH(vc_i, &p->online_vcs, list)
		printk("\tVcore %d -> Pcore %d\n", vcore2vcoreid(p, vc_i), vc_i->pcoreid);
	printk("Bulk Preempted:\n");
	TAILQ_FOREACH(vc_i, &p->bulk_preempted_vcs, list)
		printk("\tVcore %d\n", vcore2vcoreid(p, vc_i));
	printk("Inactive / Yielded:\n");
	TAILQ_FOREACH(vc_i, &p->inactive_vcs, list)
		printk("\tVcore %d\n", vcore2vcoreid(p, vc_i));
	printk("Resources:\n------------------------\n");
	for (int i = 0; i < MAX_NUM_RESOURCES; i++)
		printk("\tRes type: %02d, amt wanted: %08d, amt granted: %08d\n", i,
		       p->resources[i].amt_wanted, p->resources[i].amt_granted);
	printk("Open Files:\n");
	struct files_struct *files = &p->open_files;
	spin_lock(&files->lock);
	for (int i = 0; i < files->max_files; i++)
		if (files->fd_array[i].fd_file) {
			printk("\tFD: %02d, File: %08p, File name: %s\n", i,
			       files->fd_array[i].fd_file,
			       file_name(files->fd_array[i].fd_file));
		}
	spin_unlock(&files->lock);
	/* No one cares, and it clutters the terminal */
	//printk("Vcore 0's Last Trapframe:\n");
	//print_trapframe(&p->env_tf);
	/* no locking / unlocking or refcnting */
	// spin_unlock(&p->proc_lock);
	proc_decref(p);
}

/* Debugging function, checks what (process, vcore) is supposed to run on this
 * pcore.  Meant to be called from smp_idle() before halting. */
void check_my_owner(void)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	void shazbot(void *item)
	{
		struct proc *p = (struct proc*)item;
		struct vcore *vc_i;
		assert(p);
		spin_lock(&p->proc_lock);
		TAILQ_FOREACH(vc_i, &p->online_vcs, list) {
			/* this isn't true, a __startcore could be on the way and we're
			 * already "online" */
			if (vc_i->pcoreid == core_id()) {
				/* Immediate message was sent, we should get it when we enable
				 * interrupts, which should cause us to skip cpu_halt() */
				if (!STAILQ_EMPTY(&pcpui->immed_amsgs))
					continue;
				printk("Owned pcore (%d) has no owner, by %08p, vc %d!\n",
				       core_id(), p, vcore2vcoreid(p, vc_i));
				spin_unlock(&p->proc_lock);
				spin_unlock(&pid_hash_lock);
				monitor(0);
			}
		}
		spin_unlock(&p->proc_lock);
	}
	assert(!irq_is_enabled());
	extern int booting;
	if (!booting && !pcpui->owning_proc) {
		spin_lock(&pid_hash_lock);
		hash_for_each(pid_hash, shazbot);
		spin_unlock(&pid_hash_lock);
	}
}
