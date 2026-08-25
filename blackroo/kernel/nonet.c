/*
 *  kernel/nonet.c
 *
 *  Stubs for the three networking symbols the rest of the kernel references
 *  unconditionally, so that net/ need not be linked at all when CONFIG_NET
 *  is off.
 *
 *  Stock 2.4 always links net/socket.o and net/core, because socket(2) has to
 *  exist even on a machine with no network device. On the PlayStation that is
 *  28 KB of sockets, skbuffs and SCM credential passing on a 2 MB machine -
 *  6% of the kernel for an API nothing can reach. See docs/28.
 *
 *  The three call sites are init/main.c (sock_init), fs/fcntl.c (sock_fcntl)
 *  and the syscall table (sys_socketcall). Each is answered here the way the
 *  real code would answer on a kernel with no protocol families registered.
 *
 *  Built only when CONFIG_NET is not set - see kernel/Makefile.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/errno.h>

void __init sock_init(void)
{
	/* Nothing to initialise: no protocol families are compiled in. */
}

int sock_fcntl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/* Only ever reached for an S_ISSOCK inode, which cannot be created. */
	return -EINVAL;
}

asmlinkage long sys_socketcall(int call, unsigned long *args)
{
	return -ENOSYS;
}
