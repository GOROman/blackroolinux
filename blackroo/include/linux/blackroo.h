/*
 *  include/linux/blackroo.h
 *
 *  Blackroo-specific kernel interfaces.
 *
 *  Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef _LINUX_BLACKROO_H
#define _LINUX_BLACKROO_H

/*
 * What a process looks like to userspace.
 *
 * On a normal Linux this comes from /proc, but CONFIG_PROC_FS is off here -
 * it was 34 KB of the 96 KB that bought the userspace window, and nothing on
 * this machine reads /proc except a process viewer. So instead of paying for a
 * whole filesystem to expose a dozen numbers, one syscall copies them out.
 *
 * Deliberately fixed-size and flat: no pointers, no strings to chase, and a
 * userspace program with no libc can walk the array with a for loop.
 */
struct blackroo_taskinfo {
	int		pid;
	int		ppid;
	long		state;		/* TASK_RUNNING, TASK_INTERRUPTIBLE, ... */
	unsigned long	utime;		/* ticks in user mode */
	unsigned long	stime;		/* ticks in the kernel */
	unsigned long	start_time;	/* jiffies at creation */
	char		comm[16];
};

#ifdef __KERNEL__
asmlinkage long sys_blackroo_tasks(struct blackroo_taskinfo *buf, int max);
#endif

#endif /* _LINUX_BLACKROO_H */
