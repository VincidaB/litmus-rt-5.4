#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <litmus/litmus.h>
#include <litmus/budget.h>
#include <litmus/edf_common.h>
#include <litmus/jobs.h>
#include <litmus/litmus_proc.h>
#include <litmus/debug_trace.h>
#include <litmus/preempt.h>
#include <litmus/rt_domain.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>

struct p_edf_cpu_state {
        rt_domain_t local_queues;
        int cpu;
        struct task_struct* scheduled;
};

static DEFINE_PER_CPU(struct p_edf_cpu_state, p_edf_cpu_state);

#define cpu_state_for(cpu_id)   (&per_cpu(p_edf_cpu_state, cpu_id))
#define local_cpu_state()       (this_cpu_ptr(&p_edf_cpu_state))
#define remote_edf(cpu)		(&per_cpu(p_edf_cpu_state, cpu).local_queues)
#define remote_pedf(cpu)	(&per_cpu(p_edf_cpu_state, cpu))
#define task_edf(task)		remote_edf(get_partition(task))

static struct domain_proc_info p_edf_domain_proc_info;

static long p_edf_get_domain_proc_info(struct domain_proc_info **ret)
{
        *ret = &p_edf_domain_proc_info;
        return 0;
}

static void p_edf_setup_domain_proc(void)
{
        int i, cpu;
        int num_rt_cpus = num_online_cpus();

        struct cd_mapping *cpu_map, *domain_map;

        memset(&p_edf_domain_proc_info, 0, sizeof(p_edf_domain_proc_info));
        init_domain_proc_info(&p_edf_domain_proc_info, num_rt_cpus, num_rt_cpus);
        p_edf_domain_proc_info.num_cpus = num_rt_cpus;
        p_edf_domain_proc_info.num_domains = num_rt_cpus;

        i = 0;
        for_each_online_cpu(cpu) {
                cpu_map = &p_edf_domain_proc_info.cpu_to_domains[i];
                domain_map = &p_edf_domain_proc_info.domain_to_cpus[i];

                cpu_map->id = cpu;
                domain_map->id = i;
                cpumask_set_cpu(i, cpu_map->mask);
                cpumask_set_cpu(cpu, domain_map->mask);
                ++i;
        }
}

/* This helper is called when task `prev` exhausted its budget or when
 * it signaled a job completion. */
static void p_edf_job_completion(struct task_struct *prev, int budget_exhausted)
{
        sched_trace_task_completion(prev, budget_exhausted);
	TRACE_TASK(prev, "job_completion(forced=%d).\n", budget_exhausted);

	tsk_rt(prev)->completed = 0;
        /* Call common helper code to compute the next release time, deadline,
         * etc. */
        prepare_for_next_period(prev);
}

/* Add the task `tsk` to the appropriate queue. Assumes the caller holds the ready lock.
 */
static void p_edf_requeue(struct task_struct *tsk, struct p_edf_cpu_state *cpu_state)
{
        if (is_released(tsk, litmus_clock())) {
                /* Uses __add_ready() instead of add_ready() because we already
                 * hold the ready lock. */
                __add_ready(&cpu_state->local_queues, tsk);
                TRACE_TASK(tsk, "added to ready queue on reschedule\n");
        } else {
                /* Uses add_release() because we DON'T have the release lock. */
                add_release(&cpu_state->local_queues, tsk);
                TRACE_TASK(tsk, "added to release queue on reschedule\n");
        }
}

static int p_edf_check_for_preemption_on_release(rt_domain_t *local_queues)
{
        struct p_edf_cpu_state *state = container_of(local_queues, struct p_edf_cpu_state,
                                                    local_queues);

        /* Because this is a callback from rt_domain_t we already hold
         * the necessary lock for the ready queue. */

        if (edf_preemption_needed(local_queues, state->scheduled)) {
                preempt_if_preemptable(state->scheduled, state->cpu);
                return 1;
        }
        return 0;
}

static long p_edf_activate_plugin(void)
{
        int cpu;
        struct p_edf_cpu_state *state;
        for_each_online_cpu(cpu) {
                TRACE("Initializing CPU%d...\n", cpu);
                state = cpu_state_for(cpu);
                state->cpu = cpu;
                state->scheduled = NULL;
                edf_domain_init(&state->local_queues,
                                p_edf_check_for_preemption_on_release,
                                NULL);
        }

        p_edf_setup_domain_proc();
        return 0;
}

static long p_edf_deactivate_plugin(void)
{
        destroy_domain_proc_info(&p_edf_domain_proc_info);
        return 0;
}



static struct task_struct* p_edf_schedule(struct task_struct * prev)
{
        struct p_edf_cpu_state *local_state = local_cpu_state();

        /* next == NULL means "schedule background work". */
        struct task_struct *next = NULL;

        /* prev's task state */
        int exists, out_of_time, job_completed, self_suspends, preempt, resched;

        raw_spin_lock(&local_state->local_queues.ready_lock);

        BUG_ON(local_state->scheduled && local_state->scheduled != prev);
        BUG_ON(local_state->scheduled && !is_realtime(prev));

        exists = local_state->scheduled != NULL;
        self_suspends = exists && !is_current_running();
        out_of_time = exists && budget_enforced(prev) && budget_exhausted(prev);
        job_completed = exists && is_completed(prev);

        /* preempt is true if task `prev` has lower priority than something on
         * the ready queue. */
        preempt = edf_preemption_needed(&local_state->local_queues, prev);

        /* check all conditions that make us reschedule */
        resched = preempt;

        /* if `prev` suspends, it CANNOT be scheduled anymore => reschedule */
        if (self_suspends) {
                resched = 1;
        }

        /* also check for (in-)voluntary job completions */
        if (out_of_time || job_completed) {
                p_edf_job_completion(prev, out_of_time);
                resched = 1;
        }

        if (resched) {
                /* First check if the previous task goes back onto the ready
                 * queue, which it does if it did not self_suspend.
                 */
                if (exists && !self_suspends) {
                        p_edf_requeue(prev, local_state);
                }
                next = __take_ready(&local_state->local_queues);
        } else {
                /* No preemption is required. */
                next = local_state->scheduled;
        }

        local_state->scheduled = next;
        if (exists && prev != next) {
                TRACE_TASK(prev, "descheduled.\n");
        }
        if (next) {
                TRACE_TASK(next, "scheduled.\n");
        }

        /* This mandatory. It triggers a transition in the LITMUS^RT remote
         * preemption state machine. Call this AFTER the plugin has made a local
         * scheduling decision.
         */
        sched_state_task_picked();

        raw_spin_unlock(&local_state->local_queues.ready_lock);
        return next;
}

static long p_edf_admit_task(struct task_struct *tsk)
{
        if (task_cpu(tsk) == get_partition(tsk)) {
                TRACE_TASK(tsk, "accepted by p_edf plugin.\n");
                return 0;
        }
        return -EINVAL;
}

static void p_edf_task_new(struct task_struct *tsk, int on_runqueue,
                          int is_running)
{
        /* We'll use this to store IRQ flags. */
        unsigned long flags;
        struct p_edf_cpu_state *state = cpu_state_for(get_partition(tsk));
        lt_t now;

        TRACE_TASK(tsk, "is a new RT task %llu (on runqueue:%d, running:%d)\n",
                   litmus_clock(), on_runqueue, is_running);

        /* Acquire the lock protecting the state and disable interrupts. */
        raw_spin_lock_irqsave(&state->local_queues.ready_lock, flags);

        now = litmus_clock();

        /* Release the first job now. */
        release_at(tsk, now);

        if (is_running) {
                /* If tsk is running, then no other task can be running
                 * on the local CPU. */
                BUG_ON(state->scheduled != NULL);
                state->scheduled = tsk;
        } else if (on_runqueue) {
                p_edf_requeue(tsk, state);
        }

        if (edf_preemption_needed(&state->local_queues, state->scheduled))
                preempt_if_preemptable(state->scheduled, state->cpu);

        raw_spin_unlock_irqrestore(&state->local_queues.ready_lock, flags);
}

static void p_edf_task_exit(struct task_struct *tsk)
{
        unsigned long flags;
        struct p_edf_cpu_state *state = cpu_state_for(get_partition(tsk));
        raw_spin_lock_irqsave(&state->local_queues.ready_lock, flags);
        rt_domain_t*		edf;

        /* For simplicity, we assume here that the task is no longer queued anywhere else. This
         * is the case when tasks exit by themselves; additional queue management is
         * is required if tasks are forced out of real-time mode by other tasks. */
     
        if (is_queued(tsk)){
                edf = task_edf(tsk);
                remove(edf,tsk);
        }

        if (state->scheduled == tsk) {
                state->scheduled = NULL;
        }
        
        preempt_if_preemptable(state->scheduled, state->cpu);
        raw_spin_unlock_irqrestore(&state->local_queues.ready_lock, flags);
}

/* Called when the state of tsk changes back to TASK_RUNNING.
 * We need to requeue the task.
 *
 * NOTE: If a sporadic task is suspended for a long time,
 * this might actually be an event-driven release of a new job.
 */
static void p_edf_task_resume(struct task_struct  *tsk)
{
        unsigned long flags;
        struct p_edf_cpu_state *state = cpu_state_for(get_partition(tsk));
        lt_t now;
        TRACE_TASK(tsk, "wake_up at %llu\n", litmus_clock());
        raw_spin_lock_irqsave(&state->local_queues.ready_lock, flags);

        now = litmus_clock();

        if (is_sporadic(tsk) && is_tardy(tsk, now)) {
                /* This sporadic task was gone for a "long" time and woke up past
                 * its deadline. Give it a new budget by triggering a job
                 * release. */
                inferred_sporadic_job_release_at(tsk, now);
                TRACE_TASK(tsk, "woke up too late.\n");
        }

        /* This check is required to avoid races with tasks that resume before
         * the scheduler "noticed" that it resumed. That is, the wake up may
         * race with the call to schedule(). */
        if (state->scheduled != tsk) {
                TRACE_TASK(tsk, "is being reqeued\n");
                p_edf_requeue(tsk, state);
                if (edf_preemption_needed(&state->local_queues, state->scheduled)) {
                        preempt_if_preemptable(state->scheduled, state->cpu);
                }
        }

        raw_spin_unlock_irqrestore(&state->local_queues.ready_lock, flags);
}


static struct sched_plugin p_edf_plugin = {
        .plugin_name            = "P-EDF",
        .schedule               = p_edf_schedule,
        .task_wake_up           = p_edf_task_resume,
        .admit_task             = p_edf_admit_task,
        .task_new               = p_edf_task_new,
        .task_exit              = p_edf_task_exit,
        .get_domain_proc_info   = p_edf_get_domain_proc_info,
        .activate_plugin        = p_edf_activate_plugin,
        .deactivate_plugin      = p_edf_deactivate_plugin,
        .complete_job           = complete_job,
};

static int __init init_p_edf(void)
{
        return register_sched_plugin(&p_edf_plugin);
}

module_init(init_p_edf);
