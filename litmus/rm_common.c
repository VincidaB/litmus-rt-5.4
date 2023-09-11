/*
 * litmus/rm_common.c
*/
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>
#include <litmus/debug_trace.h>

#include <litmus/rm_common.h>


/* rm_higher_prio -  returns true if first has a higher RM priority
 *                    than second. Deadline ties are broken by PID.
 *
 * both first and second may be NULL
 */
int rm_higher_prio(struct task_struct* first,
		    struct task_struct* second)
{
	struct task_struct *first_task = first;
	struct task_struct *second_task = second;

	/* There is no point in comparing a task to itself. */
	if (first && first == second) {
		TRACE_TASK(first,
			   "WARNING: pointless edf priority comparison.\n");
		return 0;
	}


	/* check for NULL tasks */
	if (!first || !second)
		return first && !second;


	if (shorter_exec_time(first_task, second_task)) {
		return 1;
	}
	else if (get_rt_period(first_task) == get_rt_period(second_task)) {
		/* Need to tie break. All methods must set pid_break to 0/1 if
		 * first_task does not have priority over second_task.
		 */
		int pid_break;

		/* CONFIG_EDF_PID_TIE_BREAK */
		pid_break = 1; // fall through to tie-break by pid;


		/* Tie break by pid */
		if(pid_break) {
			if (first_task->pid < second_task->pid) {
				return 1;
			}
			else if (first_task->pid == second_task->pid) {
				/* If the PIDs are the same then the task with the
				 * inherited priority wins.
				 */
				if (!second->rt_param.inh_task) {
					return 1;
				}
			}
		}
	}
	return 0; /* fall-through. prio(second_task) > prio(first_task) */
}

int rm_ready_order(struct bheap_node* a, struct bheap_node* b)
{
	return rm_higher_prio(bheap2task(a), bheap2task(b));
}

void rm_domain_init(rt_domain_t* rt, check_resched_needed_t resched,
					release_jobs_t release)
{
	rt_domain_init(rt, rm_ready_order, resched, release);
}


/* need_to_preempt - check whether the task t needs to be preempted
 *                   call only with irqs disabled and with  ready_lock acquired
 *                   THIS DOES NOT TAKE NON-PREEMPTIVE SECTIONS INTO ACCOUNT!
 */
int rm_preemption_needed(rt_domain_t* rt, struct task_struct *t)
{
	/* we need the read lock for edf_ready_queue */
	/* no need to preempt if there is nothing pending */
	if (!__jobs_pending(rt))
		return 0;
	/* we need to reschedule if t doesn't exist */
	if (!t)
		return 1;

	/* NOTE: We cannot check for non-preemptibility since we
	 *       don't know what address space we're currently in.
	 */

	/* make sure to get non-rt stuff out of the way */
    return !is_realtime(t) || rm_higher_prio(__next_ready(rt), t);
}