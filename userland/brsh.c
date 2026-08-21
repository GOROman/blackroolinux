/*
 * brsh.c — Blackroo's first userspace program.
 *
 * No libc: this is a freestanding MIPS binary that talks to the kernel through
 * raw syscalls. It is linked at a fixed address inside the window that
 * fs/binfmt_fixed.c reserves, because this machine has no MMU and nothing here
 * is relocatable.
 *
 * Syscall numbers are MIPS o32 Linux (see tools/host/mkinit.c, where they were
 * first worked out for the FLAT experiment):
 *   read 4003  write 4004  open 4005  close 4006  exit 4001
 *
 * Build with userland/build.sh.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

/*
 * Syscall numbers for THIS kernel.
 *
 * Standard Linux/MIPS o32 uses a 4000 base (__NR_Linux = 4000) and the
 * handler subtracts it. This tree sets __NR_Linux = 0 and indexes
 * sys_call_table with v0 directly (arch/mipsnommu/kernel/scall_o32.S:
 * "sltiu t0, v0, MAX_SYSCALL_NO + 1" then "sll t0, v0, 2"), so the numbers
 * are the small ones from include/asm-mipsnommu/unistd.h.
 *
 * Using 4004 for write - as tools/host/mkinit.c does - exceeds
 * MAX_SYSCALL_NO (216), lands in illegal_syscall, and returns -ENOSYS with
 * no output and no complaint. That is why the program appeared to run and
 * say nothing.
 */
#define SYS_exit     1
#define SYS_read     3
#define SYS_write    4
#define SYS_open     5
#define SYS_close    6
#define SYS_unlink  10
#define SYS_chdir   12
#define SYS_lseek   19
#define SYS_rename  38
#define SYS_mkdir   39
#define SYS_rmdir   40
#define SYS_stat   106		/* sys_newstat  */
#define SYS_lstat  107		/* sys_newlstat */
#define SYS_getdents 141
#define SYS_getcwd 183
#define SYS_uname  122		/* sys_newuname  */
#define SYS_sysinfo 116
#define SYS_statfs  99
#define SYS_mount   21
#define SYS_umount2 52
#define SYS_ioctl   54
#define SYS_sync    36
/*
 * BLKGETSIZE, as this kernel encodes it - NOT the generic 0x1260.
 *
 * MIPS defines _IOC_NONE as 1 rather than 0 and shifts the direction into
 * bit 29, so _IO(0x12,96) comes out as 0x20001260 here. Compiled from the
 * kernel's own <linux/fs.h> to check: .data reads 60 12 00 20.
 *
 * With the wrong number the call does not fail loudly - it returns without
 * writing anything, the size stays 0, and mkfs reports "device too small".
 */
#define BLKGETSIZE  0x20001260	/* returns 512-byte sectors */
#define SYS_blackroo_tasks 217	/* kernel/taskinfo.c - no /proc here */

/*
 * Verified against arch/mipsnommu/kernel/syscalls.h by counting SYS() entries
 * from zero - NOT by trusting the numbered comments in that file, several of
 * which are off by one (sys_getitimer carries a "105" comment at index 106).
 */

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT   0x0100	/* MIPS values - see asm-mipsnommu/fcntl.h */
#define O_TRUNC   0x0200

#define S_IFMT    0170000
#define S_IFDIR   0040000
#define S_IFCHR   0020000
#define S_IFBLK   0060000
#define S_IFREG   0100000
#define S_IFLNK   0120000

static inline long sys3(long n, long a, long b, long c)
{
	register long v0 asm("$2") = n;
	register long a0 asm("$4") = a;
	register long a1 asm("$5") = b;
	register long a2 asm("$6") = c;

	asm volatile("syscall"
		     : "+r"(v0)
		     : "r"(a0), "r"(a1), "r"(a2)
		     : "memory", "$7", "$8", "$9", "$10", "$11", "$12",
		       "$13", "$14", "$15", "$24", "$25");
	return v0;
}

/*
 * Five-argument form. mount(2) needs it; everything else here fits in three.
 * o32 passes a4 and a5 on the stack, at 16(sp) and 20(sp) - the caller's
 * argument save area, which the ABI reserves whether it is used or not.
 */
static inline long sys5(long n, long a, long b, long c, long d, long e)
{
	register long v0 asm("$2") = n;
	register long a0 asm("$4") = a;
	register long a1 asm("$5") = b;
	register long a2 asm("$6") = c;
	register long a3 asm("$7") = d;

	/*
	 * Argument 5 goes on the USER STACK at 16(sp), not in a register.
	 * arch/mipsnommu/kernel/scall_o32.S, stackargs: reads it with
	 * "lw t1, 16(t0)" where t0 is the user sp saved at the trap - so the
	 * space has to exist below sp at the moment syscall executes, which is
	 * what the subu/addu pair here is for. o32 reserves 16 bytes of
	 * argument space anyway; this takes 32 to stay aligned and leave room
	 * for a sixth argument if one is ever needed.
	 */
	asm volatile(
		"subu $sp, $sp, 32\n\t"
		"sw   %5, 16($sp)\n\t"
		"syscall\n\t"
		"addu $sp, $sp, 32"
		: "+r"(v0)
		: "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(e)
		: "memory", "$8", "$9", "$10", "$11", "$12",
		  "$13", "$14", "$15", "$24", "$25");
	return v0;
}

/*
 * Raw serial output, bypassing the kernel entirely.
 *
 * There is no memory protection here, so a process can drive SIO1 itself.
 * (This comment used to say processes run in kernel mode - they do not; see
 * docs/25 §10.4. They run in user mode in KUSEG, which reaches I/O anyway.)
 * That
 * matters for diagnosis: write(1, ...) reaches sys_write and then the machine
 * goes silent, so the console path cannot be trusted to report on itself.
 * Registers as in head.S: 0x1f801050 DATA, +4 STAT bit 0 = TX ready.
 */
static void raw_putc(int c)
{
	/* KUSEG view of the I/O ports: user mode cannot reach KSEG1
	 * (0xbf80...), but this CPU maps I/O into KUSEG as well, and there is
	 * no memory protection to stop us. */
	volatile unsigned short *stat = (volatile unsigned short *)0x1f801054;
	volatile unsigned char *data = (volatile unsigned char *)0x1f801050;
	long spins = 0;

	if (c == '\n')
		raw_putc('\r');

	while (!(*stat & 1))
		if (++spins > 400000)
			return;
	*data = (unsigned char)c;
}

static void raw_out(const char *s)
{
	while (*s)
		raw_putc(*s++);
}

static void raw_hex(unsigned long v)
{
	static const char d[] = "0123456789abcdef";
	int i;

	raw_putc('0');
	raw_putc('x');
	for (i = 0; i < 8; i++)
		raw_putc(d[(v >> ((7 - i) * 4)) & 0xf]);
}

static int slen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

/*
 * Two descriptors, because input and output come from different places.
 *
 *   outfd - /dev/console. Since the vt_console_driver name fix that now
 *           resolves to the VT, so anything written here is drawn on the TV
 *           by psxvga_con AND mirrored out of SIO1 by do_con_write().
 *           Falls back to /dev/brcon if the tty layer will not open.
 *   infd  - /dev/brcon, always. Reading /dev/console would take input from
 *           the keyboard driver, and this port has none yet (kbd-no.c is a
 *           stub). A read there would simply never return.
 *
 * When a real keyboard arrives - PS/2 through the Pico on the card bus, or a
 * controller-port keyboard - infd moves to /dev/console too and the machine
 * stops needing a host PC at all.
 */
static int outfd = 1;		/* /dev/console, or /dev/brcon as fallback */
static int infd  = 0;		/* the same console, or /dev/brcon as fallback */

static void out(const char *s)
{
	sys3(SYS_write, outfd, (long)s, slen(s));
}

static void outhex(unsigned long v)
{
	static const char d[] = "0123456789abcdef";
	char b[11];
	int i;

	b[0] = '0';
	b[1] = 'x';
	for (i = 0; i < 8; i++)
		b[2 + i] = d[(v >> ((7 - i) * 4)) & 0xf];
	b[10] = '\0';
	out(b);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void outc(char c)
{
	sys3(SYS_write, outfd, (long)&c, 1);
}

static void outdec(long v)
{
	char b[12];
	int i = sizeof(b) - 1;
	int neg = 0;

	if (v < 0) {
		neg = 1;
		v = -v;
	}
	b[i] = '\0';
	do {
		b[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v && i > 1);
	if (neg)
		b[--i] = '-';
	out(b + i);
}

/* right-align a number in a field, the way ls lines up sizes */
static void outdec_w(long v, int width)
{
	long t = v;
	int n = 1;

	while (t >= 10) {
		t /= 10;
		n++;
	}
	while (n++ < width)
		outc(' ');
	outdec(v);
}

/*
 * Errors are reported by number, not by name.
 *
 * A strerror() table is 1-2 KB of strings for a machine whose whole userspace
 * window is 192 KB, and the number is enough to look up in errno.h. The sign
 * is dropped because every syscall here returns the negated value.
 */
static void fail(const char *what, const char *who, long err)
{
	out("brsh: ");
	out(what);
	if (who) {
		out(" ");
		out(who);
	}
	out(": errno ");
	outdec(-err);
	out("\n");
}

static void scopy(char *d, const char *s, int max)
{
	int i = 0;

	while (s[i] && i < max - 1) {
		d[i] = s[i];
		i++;
	}
	d[i] = '\0';
}

/* ── struct stat, exactly as asm-mipsnommu/stat.h lays it out ──────────────
 * Field order matters more than field names here: sys_newstat copies this
 * structure out byte for byte, and the st_pad members are load-bearing. */
struct k_stat {
	unsigned int	st_dev;
	long		st_pad1[3];
	unsigned long	st_ino;
	unsigned int	st_mode;
	int		st_nlink;
	int		st_uid;
	int		st_gid;
	unsigned int	st_rdev;
	long		st_pad2[2];
	long		st_size;
	long		st_pad3;
	long		st_atime;
	long		reserved0;
	long		st_mtime;
	long		reserved1;
	long		st_ctime;
	long		reserved2;
	long		st_blksize;
	long		st_blocks;
	long		st_pad4[14];
};

/* what sys_getdents writes - fs/readdir.c struct linux_dirent */
struct k_dirent {
	unsigned long	d_ino;
	unsigned long	d_off;
	unsigned short	d_reclen;
	char		d_name[1];
};

static char cwd[128] = "/";

/*
 * Resolve a user-typed path against the working directory.
 *
 * There is no chroot and no symlink resolution to worry about: this joins,
 * it does not canonicalise. "cd .." is handled in cmd_cd by trimming cwd,
 * which is why nothing here has to understand "..".
 */
static void resolve(char *dst, const char *path)
{
	int n;

	if (path[0] == '/') {
		scopy(dst, path, 128);
		return;
	}
	scopy(dst, cwd, 128);
	n = slen(dst);
	if (n > 0 && dst[n - 1] != '/' && n < 127)
		dst[n++] = '/';
	scopy(dst + n, path, 128 - n);
}

static long do_stat(const char *path, struct k_stat *st)
{
	char full[128];

	resolve(full, path);
	return sys3(SYS_lstat, (long)full, (long)st, 0);
}

static char type_char(unsigned int mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR: return 'd';
	case S_IFCHR: return 'c';
	case S_IFBLK: return 'b';
	case S_IFLNK: return 'l';
	case S_IFREG: return '-';
	default:      return '?';
	}
}

static void out_mode(unsigned int mode)
{
	static const char *rwx[8] = { "---", "--x", "-w-", "-wx",
				      "r--", "r-x", "rw-", "rwx" };
	outc(type_char(mode));
	out(rwx[(mode >> 6) & 7]);
	out(rwx[(mode >> 3) & 7]);
	out(rwx[mode & 7]);
}


/* struct new_utsname, from include/linux/utsname.h */
struct k_utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

/* struct sysinfo, from include/linux/kernel.h. This is how userspace learns
 * anything about the machine now: CONFIG_PROC_FS is off, so there is no
 * /proc/meminfo to read. sysinfo(2) carries what a fetch tool needs. */
struct k_sysinfo {
	long		uptime;
	unsigned long	loads[3];
	unsigned long	totalram;
	unsigned long	freeram;
	unsigned long	sharedram;
	unsigned long	bufferram;
	unsigned long	totalswap;
	unsigned long	freeswap;
	unsigned short	procs;
	unsigned long	totalhigh;
	unsigned long	freehigh;
	unsigned int	mem_unit;
	char		_f[8];
};

/*
 * Blackroo.
 *
 * Drawn to sit in the left 24 columns with the system information beside it,
 * inside the 78x21 the GPU console gives us. Kept to plain ASCII: the console
 * font is a texture atlas, not a Unicode table.
 */
static const char *roo_art[] = {
"      /\\       /\\      ",
"     |  \\_____/  |     ",
"     |   o   o   |     ",
"      \\    ^    /      ",
"       \\  '-'  /       ",
"        )     (        ",
"       /       \\       ",
"      |  .---.  |      ",
"      |  | o |  |      ",
"      |  '---'  |      ",
"       \\       / \\     ",
"        |     |   '--. ",
"       _|     |_     '.",
"      (___) (___)______)",
	0
};

/* ────────────────────────────────────────────────────────────────────────
 * Commands
 *
 * All internal, all in this one binary. binfmt_fixed has a single load
 * address, so there is exactly one program image resident at a time and an
 * external /bin/ls could not be run while the shell was running anyway.
 * docs/28 calls this the System+Finder model: the shell IS the utilities.
 * ──────────────────────────────────────────────────────────────────────── */

static char pathbuf[128];
static char iobuf[1024];

static void cmd_ls(int ac, char **av)
{
	char dirbuf[1024];
	const char *dir = (ac > 1) ? av[1] : ".";
	int fd, nread, longfmt = 0;
	long r;

	if (ac > 1 && streq(av[1], "-l")) {
		longfmt = 1;
		dir = (ac > 2) ? av[2] : ".";
	}

	resolve(pathbuf, dir);
	fd = (int)sys3(SYS_open, (long)pathbuf, O_RDONLY, 0);
	if (fd < 0) {
		fail("ls", dir, fd);
		return;
	}

	for (;;) {
		int off = 0;

		nread = (int)sys3(SYS_getdents, fd, (long)dirbuf, sizeof(dirbuf));
		if (nread < 0) {
			fail("getdents", dir, nread);
			break;
		}
		if (nread == 0)
			break;

		while (off < nread) {
			struct k_dirent *de = (struct k_dirent *)(dirbuf + off);
			struct k_stat st;
			char entry[128];

			if (de->d_reclen == 0)	/* malformed - do not spin */
				break;

			if (longfmt) {
				/* stat relative to the directory being listed,
				 * not to cwd */
				int dn = slen(pathbuf);

				scopy(entry, pathbuf, sizeof(entry));
				if (dn > 0 && entry[dn - 1] != '/' && dn < 127)
					entry[dn++] = '/';
				scopy(entry + dn, de->d_name, sizeof(entry) - dn);

				r = sys3(SYS_lstat, (long)entry, (long)&st, 0);
				if (r < 0) {
					out("?????????  ");
				} else {
					out_mode(st.st_mode);
					outc(' ');
					outdec_w(st.st_size, 8);
					outc(' ');
				}
			}
			out(de->d_name);
			out("\n");
			off += de->d_reclen;
		}
	}
	sys3(SYS_close, fd, 0, 0);
}

static void cmd_cat(int ac, char **av)
{
	int i;

	if (ac < 2) {
		out("usage: cat <file>...\n");
		return;
	}
	for (i = 1; i < ac; i++) {
		int fd, n;

		resolve(pathbuf, av[i]);
		fd = (int)sys3(SYS_open, (long)pathbuf, O_RDONLY, 0);
		if (fd < 0) {
			fail("cat", av[i], fd);
			continue;
		}
		while ((n = (int)sys3(SYS_read, fd, (long)iobuf,
				      sizeof(iobuf))) > 0)
			sys3(SYS_write, outfd, (long)iobuf, n);
		if (n < 0)
			fail("read", av[i], n);
		sys3(SYS_close, fd, 0, 0);
	}
}

static void cmd_hexdump(int ac, char **av)
{
	static const char hx[] = "0123456789abcdef";
	long offset = 0;
	int fd, n, i;

	if (ac < 2) {
		out("usage: hexdump <file>\n");
		return;
	}
	resolve(pathbuf, av[1]);
	fd = (int)sys3(SYS_open, (long)pathbuf, O_RDONLY, 0);
	if (fd < 0) {
		fail("hexdump", av[1], fd);
		return;
	}
	while ((n = (int)sys3(SYS_read, fd, (long)iobuf, 16)) > 0) {
		char line[80];
		int p = 0;

		for (i = 0; i < 8; i++)
			line[p++] = hx[(offset >> ((7 - i) * 4)) & 0xf];
		line[p++] = ' ';
		line[p++] = ' ';
		for (i = 0; i < 16; i++) {
			if (i < n) {
				unsigned char c = (unsigned char)iobuf[i];

				line[p++] = hx[c >> 4];
				line[p++] = hx[c & 0xf];
			} else {
				line[p++] = ' ';
				line[p++] = ' ';
			}
			line[p++] = ' ';
		}
		line[p++] = '|';
		for (i = 0; i < n; i++) {
			unsigned char c = (unsigned char)iobuf[i];

			line[p++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
		}
		line[p++] = '|';
		line[p++] = '\n';
		line[p] = '\0';
		out(line);
		offset += n;
	}
	if (n < 0)
		fail("read", av[1], n);
	sys3(SYS_close, fd, 0, 0);
}

static void cmd_cd(int ac, char **av)
{
	const char *target = (ac > 1) ? av[1] : "/";
	struct k_stat st;
	long r;
	int n;

	/* ".." is handled by trimming the string rather than by asking the
	 * kernel, so that cwd stays a clean absolute path for resolve(). */
	if (streq(target, "..")) {
		n = slen(cwd);
		while (n > 1 && cwd[n - 1] != '/')
			n--;
		if (n > 1)
			n--;		/* drop the slash unless it is root */
		cwd[n ? n : 1] = '\0';
		sys3(SYS_chdir, (long)cwd, 0, 0);
		return;
	}
	if (streq(target, "."))
		return;

	resolve(pathbuf, target);
	r = sys3(SYS_lstat, (long)pathbuf, (long)&st, 0);
	if (r < 0) {
		fail("cd", target, r);
		return;
	}
	if ((st.st_mode & S_IFMT) != S_IFDIR) {
		out("brsh: cd ");
		out(target);
		out(": not a directory\n");
		return;
	}
	r = sys3(SYS_chdir, (long)pathbuf, 0, 0);
	if (r < 0) {
		fail("cd", target, r);
		return;
	}
	scopy(cwd, pathbuf, sizeof(cwd));
}

static void cmd_mkdir(int ac, char **av)
{
	int i;
	long r;

	if (ac < 2) {
		out("usage: mkdir <dir>...\n");
		return;
	}
	for (i = 1; i < ac; i++) {
		resolve(pathbuf, av[i]);
		r = sys3(SYS_mkdir, (long)pathbuf, 0755, 0);
		if (r < 0)
			fail("mkdir", av[i], r);
	}
}

static void cmd_rm(int ac, char **av)
{
	int i;
	long r;

	if (ac < 2) {
		out("usage: rm <file>...\n");
		return;
	}
	for (i = 1; i < ac; i++) {
		resolve(pathbuf, av[i]);
		r = sys3(SYS_unlink, (long)pathbuf, 0, 0);
		if (r < 0)
			fail("rm", av[i], r);
	}
}

static void cmd_rmdir(int ac, char **av)
{
	int i;
	long r;

	if (ac < 2) {
		out("usage: rmdir <dir>...\n");
		return;
	}
	for (i = 1; i < ac; i++) {
		resolve(pathbuf, av[i]);
		r = sys3(SYS_rmdir, (long)pathbuf, 0, 0);
		if (r < 0)
			fail("rmdir", av[i], r);
	}
}

static void cmd_mv(int ac, char **av)
{
	char from[128], to[128];
	long r;

	if (ac != 3) {
		out("usage: mv <from> <to>\n");
		return;
	}
	resolve(from, av[1]);
	resolve(to, av[2]);
	r = sys3(SYS_rename, (long)from, (long)to, 0);
	if (r < 0)
		fail("mv", av[1], r);
}

static void cmd_cp(int ac, char **av)
{
	char from[128], to[128];
	int in, outf, n, w;

	if (ac != 3) {
		out("usage: cp <from> <to>\n");
		return;
	}
	resolve(from, av[1]);
	resolve(to, av[2]);

	in = (int)sys3(SYS_open, (long)from, O_RDONLY, 0);
	if (in < 0) {
		fail("cp", av[1], in);
		return;
	}
	outf = (int)sys3(SYS_open, (long)to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (outf < 0) {
		fail("cp", av[2], outf);
		sys3(SYS_close, in, 0, 0);
		return;
	}
	while ((n = (int)sys3(SYS_read, in, (long)iobuf, sizeof(iobuf))) > 0) {
		w = (int)sys3(SYS_write, outf, (long)iobuf, n);
		if (w != n) {
			fail("cp: short write to", av[2], w < 0 ? w : 0);
			break;
		}
	}
	if (n < 0)
		fail("cp: read", av[1], n);
	sys3(SYS_close, in, 0, 0);
	sys3(SYS_close, outf, 0, 0);
}

static void cmd_stat(int ac, char **av)
{
	struct k_stat st;
	long r;

	if (ac < 2) {
		out("usage: stat <path>\n");
		return;
	}
	r = do_stat(av[1], &st);
	if (r < 0) {
		fail("stat", av[1], r);
		return;
	}
	out("  file   "); out(av[1]);  out("\n");
	out("  mode   "); out_mode(st.st_mode); out(" (");
	outhex(st.st_mode); out(")\n");
	out("  size   "); outdec(st.st_size); out(" bytes\n");
	out("  inode  "); outdec((long)st.st_ino); out("\n");
	out("  links  "); outdec(st.st_nlink); out("\n");
	out("  dev    "); outhex(st.st_dev);
	if ((st.st_mode & S_IFMT) == S_IFCHR ||
	    (st.st_mode & S_IFMT) == S_IFBLK) {
		out("  rdev "); outhex(st.st_rdev);
		out("  (major "); outdec((st.st_rdev >> 8) & 0xff);
		out(" minor ");  outdec(st.st_rdev & 0xff); out(")");
	}
	out("\n");
}

/* kernel/taskinfo.c fills these. Flat and fixed-size on purpose. */
struct k_taskinfo {
	int		pid;
	int		ppid;
	long		state;
	unsigned long	utime;
	unsigned long	stime;
	unsigned long	start_time;
	char		comm[16];
};

/* asm-mipsnommu/statfs.h */
struct k_statfs {
	long f_type, f_bsize, f_frsize, f_blocks, f_bfree, f_files, f_ffree;
	long f_bavail; long f_fsid[2]; long f_namelen; long f_spare[6];
};

#define TASK_MAX 24

static const char *task_state(long st)
{
	/* include/linux/sched.h */
	if (st == 0)   return "run";
	if (st & 1)    return "sleep";
	if (st & 2)    return "disk";
	if (st & 4)    return "stop";
	if (st & 8)    return "zomb";
	return "?";
}

static void cmd_ps(void)
{
	struct k_taskinfo t[TASK_MAX];
	long n;
	int i;

	n = sys3(SYS_blackroo_tasks, (long)t, TASK_MAX, 0);
	if (n < 0) {
		fail("ps", 0, n);
		return;
	}

	out("  PID  PPID STATE      UTIME  STIME  COMMAND\n");
	for (i = 0; i < (int)n; i++) {
		outdec_w(t[i].pid, 5);   outc(' ');
		outdec_w(t[i].ppid, 5);  outc(' ');
		out(task_state(t[i].state));
		{
			int pad = 6 - slen(task_state(t[i].state));

			while (pad-- > 0)
				outc(' ');
		}
		outdec_w((long)t[i].utime, 7); outc(' ');
		outdec_w((long)t[i].stime, 6); out("  ");
		out(t[i].comm);
		out("\n");
	}
	out("\n  ");
	outdec(n);
	out(" processes\n");
}

/*
 * top - a snapshot, not a full-screen program.
 *
 * A real top redraws over itself, which needs raw mode and cursor control.
 * This prints once and returns, which is what is actually useful over a
 * 115200 baud serial line and on a console with no scrollback.
 */
static void cmd_top(void)
{
	struct k_sysinfo si;
	int i;

	for (i = 0; i < (int)sizeof(si); i++)
		((char *)&si)[i] = 0;

	if (sys3(SYS_sysinfo, (long)&si, 0, 0) >= 0) {
		out("  up ");
		outdec(si.uptime / 60); out("m ");
		outdec(si.uptime % 60); out("s   mem ");
		outdec((long)((si.totalram - si.freeram) >> 10)); out("/");
		outdec((long)(si.totalram >> 10)); out(" KB   buffers ");
		outdec((long)(si.bufferram >> 10)); out(" KB   procs ");
		outdec(si.procs);
		out("\n\n");
	}
	cmd_ps();
}

static void cmd_df(int ac, char **av)
{
	struct k_statfs st;
	const char *path = (ac > 1) ? av[1] : "/";
	long r;
	int i;

	for (i = 0; i < (int)sizeof(st); i++)
		((char *)&st)[i] = 0;

	resolve(pathbuf, path);
	r = sys3(SYS_statfs, (long)pathbuf, (long)&st, 0);
	if (r < 0) {
		fail("df", path, r);
		return;
	}

	out("Filesystem       1K-blocks      Used     Avail  Mounted on\n");
	out("                 ");
	{
		long kb    = (st.f_bsize / 1024) * st.f_blocks;
		long freek = (st.f_bsize / 1024) * st.f_bfree;

		outdec_w(kb, 9);            out(" ");
		outdec_w(kb - freek, 9);    out(" ");
		outdec_w(freek, 9);         out("  ");
		out(path);
		out("\n");
	}
	out("  block size ");
	outdec(st.f_bsize);
	out(", inodes ");
	outdec(st.f_files - st.f_ffree);
	out(" used of ");
	outdec(st.f_files);
	out("\n");
}

/*
 * fetch - what neofetch would be, on a machine with no /proc.
 *
 * Everything here comes from two syscalls: uname(2) for the kernel identity
 * and sysinfo(2) for memory, uptime and the process count. There is no
 * /proc/meminfo to read - CONFIG_PROC_FS was one of the things traded away to
 * buy the userspace window this program is running in.
 */
static void out_kb(unsigned long bytes)
{
	outdec((long)(bytes >> 10));
	out(" KB");
}

static void cmd_fetch(void)
{
	struct k_utsname u;
	struct k_sysinfo si;
	int line = 0, i, have_uts, have_si;

	for (i = 0; i < (int)sizeof(u); i++)
		((char *)&u)[i] = 0;
	for (i = 0; i < (int)sizeof(si); i++)
		((char *)&si)[i] = 0;

	have_uts = (sys3(SYS_uname,   (long)&u,  0, 0) >= 0);
	have_si  = (sys3(SYS_sysinfo, (long)&si, 0, 0) >= 0);

	out("\n");
	while (roo_art[line]) {
		out(roo_art[line]);
		out("  ");

		switch (line) {
		case 0:
			out("Blackroo Linux");
			break;
		case 1:
			out("--------------");
			break;
		case 2:
			out("host    ");
			out(have_uts ? u.nodename : "playstation");
			break;
		case 3:
			out("kernel  ");
			if (have_uts) { out(u.sysname); outc(' '); out(u.release); }
			else out("linux 2.4");
			break;
		case 4:
			out("arch    ");
			out(have_uts ? u.machine : "mips");
			out(" R3000A @ 33 MHz");
			break;
		case 5:
			out("uptime  ");
			if (have_si) {
				long m = si.uptime / 60;

				if (m >= 60) { outdec(m / 60); out("h "); }
				outdec(m % 60); out("m ");
				outdec(si.uptime % 60); out("s");
			} else
				out("?");
			break;
		case 6:
			out("memory  ");
			if (have_si) {
				/* Never subtract blind: if a port ever reports
				 * freeram > totalram again, say so rather than
				 * printing a 4 GB underflow. */
				if (si.freeram <= si.totalram) {
					out_kb(si.totalram - si.freeram);
					out(" used of ");
					out_kb(si.totalram);
				} else {
					out_kb(si.totalram);
					out(" total (kernel reports more free)");
				}
			} else
				out("?");
			break;
		case 7:
			out("free    ");
			if (have_si) out_kb(si.freeram); else out("?");
			break;
		case 8:
			out("buffers ");
			if (have_si) out_kb(si.bufferram); else out("?");
			break;
		case 9:
			out("procs   ");
			if (have_si) outdec(si.procs); else out("?");
			break;
		case 10:
			out("shell   brsh - no libc, raw syscalls");
			break;
		case 11:
			out("display GPU console on the television");
			break;
		case 12:
			out("root    see /etc/release");
			break;
		case 13:
			out("Blackroo 2022-2026");
			break;
		default:
			break;
		}

		out("\n");
		line++;
	}
	out("\n");
}

/*
 * edit - a line editor, in the ed tradition rather than the nano one.
 *
 * A full-screen editor needs raw mode, cursor addressing and a redraw loop.
 * brsh has no termios and the console has no scrollback, so a screen editor
 * would be a large amount of code that works badly over a 115200 baud line.
 * A line editor is small, works identically on the television and on serial,
 * and is what this machine can actually carry.
 *
 * Note the disc is READ-ONLY: this can open a file from /dev/psxcd but cannot
 * write one back there. Edit under /tmp, or on a memory card once card writes
 * are proven.
 */
#define ED_LINES 128
#define ED_COLS  128

static char ed_buf[ED_LINES][ED_COLS];
static int  ed_n;

static void ed_load(const char *path)
{
	char c;
	int fd, col = 0, n;

	ed_n = 0;
	ed_buf[0][0] = '\0';

	fd = (int)sys3(SYS_open, (long)path, O_RDONLY, 0);
	if (fd < 0)
		return;			/* new file */

	while ((n = (int)sys3(SYS_read, fd, (long)&c, 1)) == 1) {
		if (c == '\r')
			continue;
		if (c == '\n' || col >= ED_COLS - 1) {
			ed_buf[ed_n][col] = '\0';
			col = 0;
			if (++ed_n >= ED_LINES)
				break;
			ed_buf[ed_n][0] = '\0';
			if (c != '\n')
				ed_buf[ed_n][col++] = c;
		} else {
			ed_buf[ed_n][col++] = c;
		}
	}
	if (col > 0) {
		ed_buf[ed_n][col] = '\0';
		ed_n++;
	}
	sys3(SYS_close, fd, 0, 0);
}

static int ed_save(const char *path)
{
	int fd, i;
	long r;

	fd = (int)sys3(SYS_open, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return (int)fd;

	for (i = 0; i < ed_n; i++) {
		int len = slen(ed_buf[i]);

		if (len) {
			r = sys3(SYS_write, fd, (long)ed_buf[i], len);
			if (r < 0) { sys3(SYS_close, fd, 0, 0); return (int)r; }
		}
		r = sys3(SYS_write, fd, (long)"\n", 1);
		if (r < 0) { sys3(SYS_close, fd, 0, 0); return (int)r; }
	}
	sys3(SYS_close, fd, 0, 0);
	return 0;
}

static void ed_list(void)
{
	int i;

	if (!ed_n) {
		out("  (empty)\n");
		return;
	}
	for (i = 0; i < ed_n; i++) {
		outdec_w(i + 1, 4);
		out("  ");
		out(ed_buf[i]);
		out("\n");
	}
}

static long ed_num(const char *p)
{
	long v = 0;

	while (*p >= '0' && *p <= '9')
		v = v * 10 + (*p++ - '0');
	return v;
}

static void cmd_edit(int ac, char **av)
{
	char line[ED_COLS + 32];
	int n, dirty = 0;

	if (ac < 2) {
		out("usage: edit <file>\n");
		return;
	}
	resolve(pathbuf, av[1]);
	ed_load(pathbuf);

	out("edit ");
	out(av[1]);
	out("  -  ");
	outdec(ed_n);
	out(" lines\n"
	    "  p        print       a        append at the end\n"
	    "  <n>      replace n   d <n>    delete line n\n"
	    "  i <n>    insert before n      w  write     q  quit\n");

	for (;;) {
		out("edit> ");
		n = (int)sys3(SYS_read, infd, (long)line, sizeof(line) - 1);
		if (n <= 0)
			break;
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			n--;
		line[n] = '\0';
		if (!n)
			continue;

		if (streq(line, "q")) {
			if (dirty)
				out("  unwritten changes - 'w' to save, 'q!' to discard\n");
			else
				break;
			continue;
		}
		if (streq(line, "q!"))
			break;
		if (streq(line, "p")) { ed_list(); continue; }
		if (streq(line, "w")) {
			int r = ed_save(pathbuf);

			if (r < 0)
				fail("write", av[1], r);
			else {
				out("  wrote ");
				outdec(ed_n);
				out(" lines\n");
				dirty = 0;
			}
			continue;
		}
		if (streq(line, "a")) {
			out("  end with a single '.'\n");
			for (;;) {
				out("  + ");
				n = (int)sys3(SYS_read, infd, (long)line,
					      sizeof(line) - 1);
				if (n <= 0)
					break;
				while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r'))
					n--;
				line[n] = '\0';
				if (streq(line, "."))
					break;
				if (ed_n >= ED_LINES) {
					out("  full\n");
					break;
				}
				scopy(ed_buf[ed_n], line, ED_COLS);
				ed_n++;
				dirty = 1;
			}
			continue;
		}
		if (line[0] == 'd' && line[1] == ' ') {
			int k = (int)ed_num(line + 2) - 1;
			int j;

			if (k < 0 || k >= ed_n) { out("  no such line\n"); continue; }
			for (j = k; j < ed_n - 1; j++)
				scopy(ed_buf[j], ed_buf[j + 1], ED_COLS);
			ed_n--;
			dirty = 1;
			continue;
		}
		if (line[0] == 'i' && line[1] == ' ') {
			int k = (int)ed_num(line + 2) - 1;
			int j;

			if (k < 0 || k > ed_n || ed_n >= ED_LINES) {
				out("  no such line\n"); continue;
			}
			for (j = ed_n; j > k; j--)
				scopy(ed_buf[j], ed_buf[j - 1], ED_COLS);
			ed_buf[k][0] = '\0';
			ed_n++;
			out("  text for line ");
			outdec(k + 1);
			out(": ");
			n = (int)sys3(SYS_read, infd, (long)line, sizeof(line) - 1);
			if (n > 0) {
				while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r'))
					n--;
				line[n] = '\0';
				scopy(ed_buf[k], line, ED_COLS);
			}
			dirty = 1;
			continue;
		}
		if (line[0] >= '1' && line[0] <= '9') {
			int k = (int)ed_num(line) - 1;

			if (k < 0 || k >= ed_n) { out("  no such line\n"); continue; }
			out("  text for line ");
			outdec(k + 1);
			out(": ");
			n = (int)sys3(SYS_read, infd, (long)line, sizeof(line) - 1);
			if (n > 0) {
				while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r'))
					n--;
				line[n] = '\0';
				scopy(ed_buf[k], line, ED_COLS);
				dirty = 1;
			}
			continue;
		}
		out("  ? - p a w q, <n>, d <n>, i <n>\n");
	}
}

/*
 * mount / umount.
 *
 * The memory cards are the only writable volume this machine has - the disc is
 * read-only by construction. /dev/bul is eight cards joined, 508 KB, and once
 * it carries an ext2 filesystem this is how files get onto it:
 *
 *     mount /dev/bul /mnt
 *     cp /etc/motd /mnt/motd
 *     umount /mnt
 *
 * Card WRITES through the block driver are not yet proven on hardware - only
 * reads have run - so treat the first write to a card as an experiment and
 * keep a backup of anything on it.
 */
#define MS_RDONLY 1

static void cmd_mount(int ac, char **av)
{
	char dev[128], dir[128];
	const char *type = "ext2";
	long flags = 0;
	int i, argi = 0;
	const char *arg[3];
	long r;

	/*
	 * -r mounts read-only.
	 *
	 * Worth having, and worth defaulting to on a memory card: a read-WRITE
	 * mount makes ext2 write the superblock as part of mounting, so the
	 * very first thing that happens is a write through bu.c at ~31 KB/s.
	 * The kernel mounts the root filesystem readonly for the same reason.
	 */
	for (i = 1; i < ac; i++) {
		if (streq(av[i], "-r")) {
			flags |= MS_RDONLY;
		} else if (argi < 3) {
			arg[argi++] = av[i];
		}
	}

	if (argi < 2) {
		out("usage: mount [-r] <device> <dir> [type]\n"
		    "       mount -r /dev/bul /mnt/mcdrive\n"
		    "  -r  read-only. Try this first on a memory card - a\n"
		    "      read-write mount writes the superblock immediately.\n");
		return;
	}
	if (argi > 2)
		type = arg[2];

	resolve(dev, arg[0]);
	resolve(dir, arg[1]);

	r = sys5(SYS_mount, (long)dev, (long)dir, (long)type, flags, 0);
	if (r < 0) {
		fail("mount", arg[0], r);
		return;
	}
	out("  mounted ");
	out(arg[0]);
	out(" on ");
	out(arg[1]);
	out(" (");
	out(type);
	out(flags & MS_RDONLY ? ", read-only)\n" : ", read-write)\n");
}

static void cmd_umount(int ac, char **av)
{
	long r;

	if (ac < 2) {
		out("usage: umount <dir>\n");
		return;
	}
	resolve(pathbuf, av[1]);
	r = sys3(SYS_umount2, (long)pathbuf, 0, 0);
	if (r < 0) {
		fail("umount", av[1], r);
		return;
	}
	out("  unmounted ");
	out(av[1]);
	out("\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * lshw / peek / poke
 *
 * There is no PCI here and nothing to enumerate, but there is also no memory
 * protection: I/O is mapped into KUSEG and a user process can read the
 * console's registers directly. brsh already does it - raw_putc() drives SIO1
 * by hand when the console cannot be trusted to report on itself.
 *
 * So this is a hardware inventory read from the hardware, not from a kernel
 * table that might be out of date.
 *
 * SAFE READS ONLY. Some registers have side effects and are deliberately not
 * touched: reading 0x1f801801 pops the CD-ROM's result FIFO, and reading
 * 0x1f801040 pops SIO0's receive FIFO. Both would corrupt a driver mid-command.
 * ──────────────────────────────────────────────────────────────────────── */

#define R32(a) (*(volatile unsigned long *)(a))
#define R16(a) (*(volatile unsigned short *)(a))

#define REG_RAM_SIZE	0x1f801060
#define REG_IRQ_STAT	0x1f801070
#define REG_IRQ_MASK	0x1f801074
#define REG_DPCR	0x1f8010f0
#define REG_DICR	0x1f8010f4
#define REG_GPUSTAT	0x1f801814
#define REG_EXP1_CFG	0x1f801000
#define REG_BUS_CFG	0x1f801018
#define REG_COM_DELAY	0x1f801020
#define REG_T0_VAL	0x1f801100
#define REG_T1_VAL	0x1f801110
#define REG_T2_VAL	0x1f801120
#define EXP1_BASE	0x1f000000

static void out_bit(unsigned long v, int bit, const char *yes, const char *no)
{
	out((v & (1UL << bit)) ? yes : no);
}

static void cmd_lshw(void)
{
	unsigned long ram   = R32(REG_RAM_SIZE);
	unsigned long stat  = R32(REG_GPUSTAT);
	unsigned long dpcr  = R32(REG_DPCR);
	unsigned long istat = R32(REG_IRQ_STAT);
	unsigned long imask = R32(REG_IRQ_MASK);
	static const char *dma[7] = { "MDECin", "MDECout", "GPU", "CDROM",
				      "SPU", "PIO", "OTC" };
	int i;

	out("cpu     MIPS R3000A, 33.8688 MHz, little-endian, no MMU\n");
	out("        no FPU - the emulator is not built (NO_FPU)\n");

	out("ram     reg ");
	outhex(ram);
	out("  ");
	if ((ram & 0xfff) == 0x888)      out("2 MB (stock)");
	else if ((ram & 0xfff) == 0xb88) out("8 MB (modified)");
	else                             out("unrecognised");
	out("\n");

	out("gpu     stat ");
	outhex(stat);
	out("\n        video ");
	out_bit(stat, 20, "PAL 50 Hz", "NTSC 60 Hz");
	out(", vres ");
	out_bit(stat, 19, "480", "240");
	out(", display ");
	out_bit(stat, 23, "off", "on");
	out("\n        interlace ");
	out_bit(stat, 22, "on", "off");
	out(", dma dir ");
	outdec((long)((stat >> 29) & 3));
	out("\n");

	out("dma     ");
	for (i = 0; i < 7; i++) {
		if (dpcr & (8UL << (i * 4))) {
			out(dma[i]);
			outc(' ');
		}
	}
	out("\n        dpcr ");
	outhex(dpcr);
	out("  dicr ");
	outhex(R32(REG_DICR));
	out("\n");

	out("irq     stat ");
	outhex(istat);
	out("  mask ");
	outhex(imask);
	out("\n");

	out("timers  t0 ");
	outdec((long)(R16(REG_T0_VAL)));
	out("  t1 ");
	outdec((long)(R16(REG_T1_VAL)));
	out("  t2 ");
	outdec((long)(R16(REG_T2_VAL)));
	out("   (t2 is the kernel tick)\n");

	out("bus     exp1 cfg ");
	outhex(R32(REG_EXP1_CFG));
	out("  bus ");
	outhex(R32(REG_BUS_CFG));
	out("\n        com delay ");
	outhex(R32(REG_COM_DELAY));
	out("\n");

	/* The expansion port: a cheat cart answers with a signature here.
	 * docs/18 confirmed the layout against a real Power Replay III. */
	out("exp1    ");
	{
		volatile unsigned char *sig = (volatile unsigned char *)(EXP1_BASE + 0x84);
		int printable = 1;
		char name[17];

		for (i = 0; i < 16; i++) {
			name[i] = (char)sig[i];
			if (name[i] < 0x20 || name[i] > 0x7e) {
				if (name[i] != '\0')
					printable = 0;
				name[i] = ' ';
			}
		}
		name[16] = '\0';
		if (printable && name[0] != ' ') {
			out("cart present: ");
			out(name);
		} else {
			out("no cart signature at 0x1f000084");
		}
	}
	out("\n");

	out("storage /dev/bul  memory cards, major 208 (bu0..bu3 = 207)\n");
	out("        /dev/psxcd  CD-ROM, major 209, read-only\n");
	out("        /dev/ram0   ramdisk, major 1\n");
	out("input   psxkbd on the SIO0 controller bus\n");
	out("serial  SIO1 at 0x1f801050, 115200 8N1\n");
}

static unsigned long hex_arg(const char *p)
{
	unsigned long v = 0;

	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	while (*p) {
		int c = *p++;

		if (c >= '0' && c <= '9')      v = (v << 4) | (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') v = (v << 4) | (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v = (v << 4) | (unsigned)(c - 'A' + 10);
		else break;
	}
	return v;
}

static void cmd_peek(int ac, char **av)
{
	unsigned long a;

	if (ac < 2) {
		out("usage: peek <hex addr>   (32-bit, word aligned)\n"
		    "  NOTE some registers have side effects when read -\n"
		    "  0x1f801801 pops the CD result FIFO, 0x1f801040 pops SIO0.\n");
		return;
	}
	a = hex_arg(av[1]) & ~3UL;
	outhex(a);
	out(" = ");
	outhex(R32(a));
	out("\n");
}

static void cmd_poke(int ac, char **av)
{
	unsigned long a, v;

	if (ac < 3) {
		out("usage: poke <hex addr> <hex value>\n"
		    "  There is NO memory protection on this machine. A poke to\n"
		    "  the wrong address will take the kernel with it.\n");
		return;
	}
	a = hex_arg(av[1]) & ~3UL;
	v = hex_arg(av[2]);
	R32(a) = v;
	outhex(a);
	out(" <- ");
	outhex(v);
	out("\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * mkfs - an ext2 revision 0 filesystem, written on the console itself
 *
 * There is no mke2fs here and no way to run one: the disc is read-only and the
 * only writable volume is the memory cards. So the machine has to be able to
 * format its own storage, or it can never save anything without a host PC -
 * which is the whole point of this project.
 *
 * Revision 0 on purpose. This tree's ext2 reads nothing newer, rev 0 fixes the
 * inode size at 128 bytes and the first usable inode at 11, and it needs no
 * feature flags. One block group, 1024-byte blocks:
 *
 *   block 0        boot block, untouched
 *   block 1        superblock
 *   block 2        group descriptor
 *   block 3        block bitmap
 *   block 4        inode bitmap
 *   block 5..      inode table
 *   next block     the root directory's data
 *   the rest       free
 * ──────────────────────────────────────────────────────────────────────── */

#define FS_BSIZE   1024
#define FS_INOSIZE 128
#define FS_FIRSTINO 11

static unsigned char fsbuf[FS_BSIZE];

static void fs_zero(void)
{
	int i;

	for (i = 0; i < FS_BSIZE; i++)
		fsbuf[i] = 0;
}

static void put16(int off, unsigned long v)
{
	fsbuf[off]     = (unsigned char)(v & 0xff);
	fsbuf[off + 1] = (unsigned char)((v >> 8) & 0xff);
}

static void put32(int off, unsigned long v)
{
	fsbuf[off]     = (unsigned char)(v & 0xff);
	fsbuf[off + 1] = (unsigned char)((v >> 8) & 0xff);
	fsbuf[off + 2] = (unsigned char)((v >> 16) & 0xff);
	fsbuf[off + 3] = (unsigned char)((v >> 24) & 0xff);
}

/* write fsbuf at block number blk */
static int fs_put(int fd, unsigned long blk)
{
	long r;

	r = sys3(SYS_lseek, fd, (long)(blk * FS_BSIZE), 0 /* SEEK_SET */);
	if (r < 0)
		return (int)r;
	r = sys3(SYS_write, fd, (long)fsbuf, FS_BSIZE);
	if (r != FS_BSIZE)
		return (r < 0) ? (int)r : -1;
	return 0;
}

static void cmd_mkfs(int ac, char **av)
{
	unsigned long blocks, inodes, itblocks, i;
	unsigned long b_bitmap, i_bitmap, i_table, root_data, first_free;
	unsigned long free_blocks, free_inodes;
	int fd, err = 0;
	long r, sectors = 0;

	if (ac < 3 || !streq(av[2], "yes")) {
		out("usage: mkfs <device> yes [blocks]\n"
		    "  mkfs /dev/bul yes\n"
		    "\n"
		    "  ERASES the device completely. ext2 revision 0,\n"
		    "  1024-byte blocks, one group. The word 'yes' is\n"
		    "  required so this cannot happen by accident.\n");
		return;
	}

	resolve(pathbuf, av[1]);
	fd = (int)sys3(SYS_open, (long)pathbuf, O_RDWR, 0);
	if (fd < 0) {
		fail("mkfs", av[1], fd);
		return;
	}

	if (ac > 3) {
		blocks = 0;
		for (i = 0; av[3][i] >= '0' && av[3][i] <= '9'; i++)
			blocks = blocks * 10 + (unsigned long)(av[3][i] - '0');
	} else {
		/* ask the driver how big it is - 512-byte sectors */
		r = sys3(SYS_ioctl, fd, BLKGETSIZE, (long)&sectors);
		if (r < 0) {
			fail("mkfs: BLKGETSIZE", av[1], r);
			sys3(SYS_close, fd, 0, 0);
			return;
		}
		blocks = (unsigned long)sectors / 2;	/* 512 -> 1024 */
		if (blocks == 0) {
			out("mkfs: the driver reports a zero-sized device.\n"
			    "  Pass the block count explicitly if you know it:\n"
			    "    mkfs ");
			out(av[1]);
			out(" yes 381\n");
			sys3(SYS_close, fd, 0, 0);
			return;
		}
	}

	if (blocks < 16) {
		out("mkfs: device too small (");
		outdec((long)blocks);
		out(" blocks)\n");
		sys3(SYS_close, fd, 0, 0);
		return;
	}

	/* One inode per 4 blocks, rounded to a whole inode-table block
	 * (8 inodes of 128 bytes per 1024-byte block). */
	inodes = blocks / 4;
	if (inodes < 16)
		inodes = 16;
	inodes = (inodes + 7) & ~7UL;
	itblocks = (inodes * FS_INOSIZE + FS_BSIZE - 1) / FS_BSIZE;

	b_bitmap  = 3;
	i_bitmap  = 4;
	i_table   = 5;
	root_data = i_table + itblocks;
	first_free = root_data + 1;

	if (first_free >= blocks) {
		out("mkfs: device too small for its own metadata\n");
		sys3(SYS_close, fd, 0, 0);
		return;
	}

	free_blocks = blocks - first_free;
	free_inodes = inodes - FS_FIRSTINO + 1;	/* 1..10 reserved, 2 is root */

	out("mkfs ");
	out(av[1]);
	out(": ");
	outdec((long)blocks);
	out(" blocks of 1024, ");
	outdec((long)inodes);
	out(" inodes, ");
	outdec((long)itblocks);
	out(" inode-table blocks\n");

	/* ---- superblock, block 1 ---------------------------------------- */
	fs_zero();
	put32(0,  inodes);		/* s_inodes_count      */
	put32(4,  blocks);		/* s_blocks_count      */
	put32(8,  0);			/* s_r_blocks_count    */
	put32(12, free_blocks);		/* s_free_blocks_count */
	put32(16, free_inodes);		/* s_free_inodes_count */
	put32(20, 1);			/* s_first_data_block  */
	put32(24, 0);			/* s_log_block_size: 0 = 1024 */
	put32(28, 0);			/* s_log_frag_size     */
	/* 8192 = one bitmap block's worth of bits, which is what mke2fs uses
	 * for a 1024-byte filesystem regardless of how small it is. Setting
	 * this to the actual block count instead makes e2fsck compute the
	 * group's extent differently and complain that the bitmap padding is
	 * wrong, however the padding is written. */
	put32(32, (unsigned long)FS_BSIZE * 8);	/* s_blocks_per_group */
	put32(36, (unsigned long)FS_BSIZE * 8);	/* s_frags_per_group  */
	put32(40, inodes);		/* s_inodes_per_group  */
	put16(56, 0xEF53);		/* s_magic             */
	put16(58, 1);			/* s_state: clean      */
	put16(60, 1);			/* s_errors: continue  */
	put32(72, 0);			/* s_creator_os: Linux */
	put32(76, 0);			/* s_rev_level: rev 0  */
	put16(54, 0xffff);		/* s_max_mnt_count: never force fsck */
	if ((err = fs_put(fd, 1)) < 0) goto done;

	/* ---- group descriptor, block 2 ----------------------------------- */
	fs_zero();
	put32(0,  b_bitmap);
	put32(4,  i_bitmap);
	put32(8,  i_table);
	put16(12, free_blocks);
	put16(14, free_inodes);
	put16(16, 1);			/* one directory: the root */
	if ((err = fs_put(fd, 2)) < 0) goto done;

	/* ---- block bitmap, block 3 --------------------------------------- */
	fs_zero();
	/* blocks 1..first_free-1 are metadata and in use. Bit 0 is block 1,
	 * because s_first_data_block is 1 on a 1024-byte filesystem. */
	for (i = 1; i < first_free; i++)
		fsbuf[(i - 1) >> 3] |= (unsigned char)(1 << ((i - 1) & 7));
	/* Mark everything past the end of the device used, or fsck reports
	 * "Padding at end of block bitmap is not set".
	 *
	 * Block b maps to bit b-1, because s_first_data_block is 1. The bitmap
	 * therefore covers blocks 1..blocks-1 - block 0 is the boot block and
	 * has no bit - so the LAST valid bit is blocks-2 and padding starts at
	 * bit blocks-1, i.e. b = blocks. Off by one here and e2fsck says
	 * "Padding at end of block bitmap is not set"; checked against a real
	 * mke2fs image of the same geometry, byte for byte. */
	for (i = blocks; i <= (unsigned long)FS_BSIZE * 8; i++)
		fsbuf[(i - 1) >> 3] |= (unsigned char)(1 << ((i - 1) & 7));
	if ((err = fs_put(fd, b_bitmap)) < 0) goto done;

	/* ---- inode bitmap, block 4 --------------------------------------- */
	fs_zero();
	for (i = 1; i < FS_FIRSTINO; i++)	/* 1..10 reserved */
		fsbuf[(i - 1) >> 3] |= (unsigned char)(1 << ((i - 1) & 7));
	for (i = inodes + 1; i <= (unsigned long)FS_BSIZE * 8; i++)
		fsbuf[(i - 1) >> 3] |= (unsigned char)(1 << ((i - 1) & 7));
	if ((err = fs_put(fd, i_bitmap)) < 0) goto done;

	/* ---- inode table ------------------------------------------------- */
	for (i = 0; i < itblocks; i++) {
		fs_zero();
		if (i == 0) {
			/* inode 2 is the root directory, at offset 128 */
			int o = FS_INOSIZE;	/* (2 - 1) * 128 */

			put16(o + 0,  0x41ED);	/* mode: directory 0755 */
			put16(o + 2,  0);	/* uid */
			put32(o + 4,  FS_BSIZE);/* size */
			put16(o + 24, 0);	/* gid */
			put16(o + 26, 2);	/* links: . and .. */
			put32(o + 28, FS_BSIZE / 512);	/* i_blocks, 512-byte units */
			put32(o + 40, root_data);	/* i_block[0] */
		}
		if ((err = fs_put(fd, i_table + i)) < 0) goto done;
	}

	/* ---- the root directory's data ----------------------------------- */
	fs_zero();
	/* "."  - rev 0 stores name_len as 16 bits, so the byte after it is 0,
	 * which is also what a rev 1 filesystem would read as file_type 0. */
	put32(0, 2);			/* inode 2 */
	put16(4, 12);			/* rec_len */
	fsbuf[6] = 1;			/* name_len */
	fsbuf[8] = '.';
	/* ".." - takes the rest of the block */
	put32(12, 2);
	put16(16, (unsigned long)(FS_BSIZE - 12));
	fsbuf[18] = 2;
	fsbuf[20] = '.';
	fsbuf[21] = '.';
	if ((err = fs_put(fd, root_data)) < 0) goto done;

	/* block 0 is the boot block - leave it alone; ext2 never reads it */

	sys3(SYS_close, fd, 0, 0);
	sys3(SYS_sync, 0, 0, 0);

	out("  done. ");
	outdec((long)free_blocks);
	out(" blocks free. Now:  mount ");
	out(av[1]);
	out(" /mnt/mcdrive\n");
	return;

done:
	fail("mkfs: write", av[1], err);
	sys3(SYS_close, fd, 0, 0);
}

static void cmd_pwd(void)
{
	out(cwd);
	out("\n");
}

static void cmd_help(void)
{
	out("brsh builtins:\n"
	    "  ls [-l] [dir]      list a directory\n"
	    "  cat <file>...      print a file\n"
	    "  hexdump <file>     print a file in hex\n"
	    "  stat <path>        inode, mode, size, device\n"
	    "  cd [dir]           change directory ('..' understood)\n"
	    "  pwd                print the working directory\n"
	    "  mkdir <dir>...     create directories\n"
	    "  rmdir <dir>...     remove empty directories\n"
	    "  rm <file>...       unlink files\n"
	    "  cp <from> <to>     copy a file\n"
	    "  mv <from> <to>     rename a file\n"
	    "  edit <file>        line editor (p a w q, <n>, d, i)\n"
	    "  ps                 the process table\n"
	    "  top                memory, uptime and processes\n"
	    "  df [path]          filesystem usage\n"
	    "  mkfs <dev> yes     make an ext2 filesystem (ERASES)\n"
	    "  mount <dev> <dir>  mount a filesystem\n"
	    "  umount <dir>       unmount one\n"
	    "  lshw               hardware, read from the registers\n"
	    "  peek <addr>        read a 32-bit register\n"
	    "  poke <addr> <val>  write one (no memory protection!)\n"
	    "  fetch              the machine, and a kangaroo\n"
	    "  echo <words>       print its arguments\n"
	    "  help               this list\n"
	    "  exit               leave the shell\n");
}

/*
 * Split a line into argv in place.
 *
 * No quoting and no escapes: a filename with a space in it cannot be typed.
 * That is a deliberate omission rather than an oversight - quoting is the
 * first thing to add when something needs it, and until then it is code that
 * cannot be tested on a machine with no such filenames.
 */
static int split(char *line, char **av, int maxav)
{
	int ac = 0;
	char *p = line;

	while (*p && ac < maxav) {
		while (*p == ' ' || *p == '\t')
			*p++ = '\0';
		if (!*p)
			break;
		av[ac++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	return ac;
}

static void run(char *line)
{
	char *av[16];
	int ac = split(line, av, 16);

	if (ac == 0)
		return;

	if (streq(av[0], "help"))        { cmd_help();          return; }
	if (streq(av[0], "ls"))          { cmd_ls(ac, av);      return; }
	if (streq(av[0], "cat"))         { cmd_cat(ac, av);     return; }
	if (streq(av[0], "hexdump"))     { cmd_hexdump(ac, av); return; }
	if (streq(av[0], "stat"))        { cmd_stat(ac, av);    return; }
	if (streq(av[0], "cd"))          { cmd_cd(ac, av);      return; }
	if (streq(av[0], "pwd"))         { cmd_pwd();           return; }
	if (streq(av[0], "ps"))          { cmd_ps();            return; }
	if (streq(av[0], "top"))         { cmd_top();           return; }
	if (streq(av[0], "df"))          { cmd_df(ac, av);      return; }
	if (streq(av[0], "edit"))        { cmd_edit(ac, av);    return; }
	if (streq(av[0], "mount"))       { cmd_mount(ac, av);   return; }
	if (streq(av[0], "umount"))      { cmd_umount(ac, av);  return; }
	if (streq(av[0], "mkfs"))        { cmd_mkfs(ac, av);    return; }
	if (streq(av[0], "lshw"))        { cmd_lshw();          return; }
	if (streq(av[0], "peek"))        { cmd_peek(ac, av);    return; }
	if (streq(av[0], "poke"))        { cmd_poke(ac, av);    return; }
	if (streq(av[0], "fetch") ||
	    streq(av[0], "neofetch"))    { cmd_fetch();         return; }
	if (streq(av[0], "mkdir"))       { cmd_mkdir(ac, av);   return; }
	if (streq(av[0], "rmdir"))       { cmd_rmdir(ac, av);   return; }
	if (streq(av[0], "rm"))          { cmd_rm(ac, av);      return; }
	if (streq(av[0], "cp"))          { cmd_cp(ac, av);      return; }
	if (streq(av[0], "mv"))          { cmd_mv(ac, av);      return; }

	if (streq(av[0], "echo")) {
		int i;

		for (i = 1; i < ac; i++) {
			if (i > 1)
				outc(' ');
			out(av[i]);
		}
		out("\n");
		return;
	}

	out("brsh: unknown command: ");
	out(av[0]);
	out("  (try 'help')\n");
}

int main(int argc, char **argv)
{
	char line[256];
	int n, confd;

	/*
	 * Open our own console.
	 *
	 * /dev/console is the VT, drawn on the television, and since
	 * drivers/char/psxkbd.c exists it has an input side as well: a keyboard
	 * on the SIO0 controller bus. So the shell uses it for both directions
	 * and the PlayStation needs no host PC.
	 *
	 * /dev/brcon - the polled SIO1 character device - stays open as the
	 * fallback. It is the only way in if the console cannot be opened, and
	 * it is how this machine was driven before there was a keyboard.
	 */
	raw_out("brsh: calling open(/dev/brcon)\n");
	infd = (int)sys3(SYS_open, (long)"/dev/brcon", 2 /* O_RDWR */, 0);
	raw_out("brsh: open returned ");
	raw_hex((unsigned long)infd);
	raw_out("\n");
	if (infd < 0) {
		raw_out("brsh: cannot open /dev/brcon, err ");
		raw_hex((unsigned long)infd);
		raw_out("\n");
		sys3(SYS_exit, 1, 0, 0);
	}

	/*
	 * Now the console, for both directions: output to the GPU, input from
	 * the keyboard. If it will not open we simply stay on serial - a shell
	 * on the wire is far better than no shell.
	 */
	raw_out("brsh: calling open(/dev/console)\n");
	confd = (int)sys3(SYS_open, (long)"/dev/console", 2 /* O_RDWR */, 0);
	raw_out("brsh: console fd ");
	raw_hex((unsigned long)confd);
	raw_out("\n");
	if (confd < 0) {
		raw_out("brsh: no /dev/console - serial only\n");
	} else {
		outfd = confd;
		infd = confd;
	}

	raw_out("brsh: calling write() on that fd\n");
	out("\n");
	raw_out("brsh: first write survived\n");
	out("brsh: userspace is alive on the PlayStation.\n");
	out("  argc="); outhex(argc);
	if (argc > 0 && argv[0]) {
		out(" argv0=");
		out(argv[0]);
	}
	out("\n  a process, not the monitor. 'help' lists what it can do.\n\n");

	/*
	 * Run a command handed to us on the kernel command line, if any.
	 *
	 * The kernel passes anything it does not recognise through to init as
	 * argv, so:
	 *
	 *     init=/bin/sh mkfs /dev/bul yes
	 *
	 * runs that command before the prompt appears. Input here comes from
	 * /dev/console - the keyboard - so without this there is no way to
	 * drive the shell from the other end of the serial cable, and every
	 * test has to be typed on the console itself. Output already goes both
	 * ways, so the result comes back over the wire either way.
	 */
	if (argc > 1) {
		char cmd[256];
		int ci = 0, ai, k;

		for (ai = 1; ai < argc && argv[ai]; ai++) {
			if (ci && ci < (int)sizeof(cmd) - 1)
				cmd[ci++] = ' ';
			for (k = 0; argv[ai][k] && ci < (int)sizeof(cmd) - 1; k++)
				cmd[ci++] = argv[ai][k];
		}
		cmd[ci] = '\0';

		if (ci) {
			out("$ ");
			out(cmd);
			out("\n");
			run(cmd);
			out("\n");
		}
	}

	for (;;) {
		/* The prompt carries the working directory, because with no
		 * job control and no scrollback it is the only place the user
		 * can see where they are. */
		out(cwd);
		out(" $ ");

		n = sys3(SYS_read, infd, (long)line, sizeof(line) - 1);
		if (n <= 0) {
			/* no console input yet — do not spin on EOF */
			out("\n(read returned ");
			outhex((unsigned long)n);
			out(" — exiting)\n");
			break;
		}

		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			n--;
		line[n] = '\0';

		if (n == 0)
			continue;

		if (streq(line, "exit"))
			break;

		run(line);
	}

	out("brsh: exiting\n");
	sys3(SYS_exit, 0, 0, 0);
	return 0;
}

/* entry point: the kernel starts us here with argc/argv on the stack */
asm(
"	.set noreorder			\n"
"	.globl _start			\n"
"_start:				\n"
/*
 * Announce ourselves by poking SIO1 directly, before any syscall.
 *
 * binfmt_fixed reports that it loaded and jumped here, and then nothing is
 * heard. That could be the jump failing, or the syscall path failing. This
 * distinguishes them: no-MMU processes run in kernel mode, so writing the
 * serial data register straight out needs no kernel help at all. Seeing 'U'
 * means we are executing; not seeing it means we never arrived.
 *
 * Same registers head.S uses for its BR! marker: 0x1f801050 DATA,
 * +4 STAT (bit 0 = ready to transmit).
 */
"	li   $8, 0x1f801050		\n"
"	li   $10, 0x40000		\n"
"1:	lhu  $9, 4($8)			\n"
"	andi $9, $9, 0x0001		\n"
"	bnez $9, 2f			\n"
"	addiu $10, $10, -1		\n"
"	bgtz $10, 1b			\n"
"	nop				\n"
"	b    3f				\n"
"	nop				\n"
"2:	li   $9, 0x55			\n"	/* 'U' for userspace */
"	sb   $9, 0($8)			\n"
"3:					\n"
"	lw   $4, 0($29)			\n"	/* argc */
"	addiu $5, $29, 4		\n"	/* argv */
"	jal  main			\n"
"	nop				\n"
"	li   $2, 1			\n"	/* exit */
"	li   $4, 0			\n"
"	syscall				\n"
"	nop				\n"
"	.set reorder			\n"
);
