#ifndef _LINUX_UTS_H
#define _LINUX_UTS_H

/*
 * Defines for what uname() should return 
 */
#ifndef UTS_SYSNAME
#define UTS_SYSNAME "Linux"
#endif

#ifndef UTS_MACHINE
#define UTS_MACHINE "unknown"
#endif

#ifndef UTS_NODENAME
/* BLACKROO: this machine has no hostname to be set by anything - there is no
 * network, no init scripts and no /etc/hostname, so "(none)" was simply what
 * uname always returned. Give it the name it actually has. */
#define UTS_NODENAME "playstation"	/* set by sethostname() */
#endif

#ifndef UTS_DOMAINNAME
#define UTS_DOMAINNAME "(none)"	/* set by setdomainname() */
#endif

#endif
