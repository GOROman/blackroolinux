/*
 *  kernel/taskinfo.c
 *
 *  sys_blackroo_tasks() - the process table, for userspace, without /proc.
 *
 *  CONFIG_PROC_FS costs 34 KB on a machine with 2 MB of RAM, and the only
 *  thing here that ever wanted it was a process viewer. This is the same
 *  information for a few hundred bytes: hold the task list lock, walk it, copy
 *  out a flat array.
 *
 *  Attribution: New Blackroo work (2026, GPL v2)
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/blackroo.h>

#include <asm/uaccess.h>

asmlinkage long sys_blackroo_tasks(struct blackroo_taskinfo *buf, int max)
{
	struct task_struct *p;
	struct blackroo_taskinfo t;
	int n = 0, i;

	if (max <= 0)
		return -EINVAL;
	if (!access_ok(VERIFY_WRITE, buf, max * sizeof(t)))
		return -EFAULT;

	read_lock(&tasklist_lock);

	for_each_task(p) {
		if (n >= max)
			break;

		t.pid        = p->pid;
		t.ppid       = (p->p_pptr) ? p->p_pptr->pid : 0;
		t.state      = p->state;
		t.utime      = p->times.tms_utime;
		t.stime      = p->times.tms_stime;
		t.start_time = p->start_time;

		for (i = 0; i < 15; i++)
			t.comm[i] = p->comm[i];
		t.comm[15] = '\0';

		/* copy_to_user can sleep on a normal kernel; it cannot here (no
		 * MMU, no faults on a valid address), but the lock is held, so
		 * keep the copy to a stack object and do it in one go. */
		if (copy_to_user(&buf[n], &t, sizeof(t))) {
			read_unlock(&tasklist_lock);
			return -EFAULT;
		}
		n++;
	}

	read_unlock(&tasklist_lock);

	return n;
}
