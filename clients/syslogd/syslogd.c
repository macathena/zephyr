/*
 * Copyright (c) 1983, 1988 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef lint
char copyright[] =
"@(#) Copyright (c) 1983, 1988 Regents of the University of California.\n\
 All rights reserved.\n";

static char sccsid[] = "@(#)syslogd.c	5.24 (Berkeley) 6/18/88";
#endif /* not lint */

/*
 *  syslogd -- log system messages
 *
 * This program implements a system log. It takes a series of lines.
 * Each line may have a priority, signified as "<n>" as
 * the first characters of the line.  If this is
 * not present, a default priority is used.
 *
 * To kill syslogd, send a signal 15 (terminate).  A signal 1 (hup) will
 * cause it to reread its configuration file.
 *
 * Defined Constants:
 *
 * MAXLINE -- the maximimum line length that can be handled.
 * DEFUPRI -- the default priority for user messages
 * DEFSPRI -- the default priority for kernel messages
 *
 * Author: Eric Allman
 * extensive changes by Ralph Campbell
 * more extensive changes by Eric Allman (again)
 * changes for Zephyr and a little dynamic allocation 
 *    by Jon Rochlis (MIT), July 1987
 * fixes, dynamic allocation, autoconf changes by Greg Hudson (MIT), 1995
 * Solaris support by John Hawkinson (MIT), 1995
 */

#define	MAXLINE		1024		/* maximum line length */
#define	MAXSVLINE	120		/* maximum saved line length */
#define DEFUPRI		(LOG_USER|LOG_NOTICE)
#define DEFSPRI		(LOG_KERN|LOG_CRIT)
#define TIMERINTVL	30		/* interval for checking flush, mark */

#if defined(__sun__) && defined(__svr4__)
#define STREAMS_LOG_DRIVER
#endif
#ifdef STREAMS_LOG_DRIVER
#undef COMPAT42
#endif

#include <sysdep.h>
#ifdef HAVE_SRC
#include <spc.h>	/* For support of the SRC system */
#endif
#include <utmp.h>
#include <setjmp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <netdb.h>
#ifdef STREAMS_LOG_DRIVER
#include <sys/stream.h>
#include <sys/strlog.h>
#include <sys/log.h>
#include <poll.h>
#include <stropts.h>
#endif

#include <zephyr/zephyr.h>
#include <com_err.h>

#ifdef _PATH_VARRUN
#define PIDDIR _PATH_VARRUN
#else
#define PIDDIR "/etc/"
#endif

#define	CTTY	"/dev/console"
char	*LogName = "/dev/log";
char	ConfFile[128];
char	PidFile[128];
char	ctty[] = CTTY;

#define FDMASK(fd)	(1 << (fd))

#define	dprintf		if (Debug) printf

#define MAXUNAMES	20	/* maximum number of user names */
#define MAXFNAME	200	/* max file pathname length */

#define NOPRI		0x10	/* the "no priority" priority */
#define	LOG_MARK	LOG_MAKEPRI(LOG_NFACILITIES, 0)	/* mark "facility" */

/*
 * Flags to logmsg().
 */

#define IGN_CONS	0x001	/* don't print on console */
#define SYNC_FILE	0x002	/* do fsync on file after printing */
#define ADDDATE		0x004	/* add a date to the message */
#define MARK		0x008	/* this message is a mark */

/*
 * This structure represents the files that will have log
 * copies printed.
 */

struct filed {
	struct	filed *f_next;		/* next in linked list */
	short	f_type;			/* entry type, see below */
	short	f_file;			/* file descriptor */
	time_t	f_time;			/* time this was last written */
	u_char	f_pmask[LOG_NFACILITIES+1];	/* priority mask */
	union {
		char	*f_uname[MAXUNAMES];
		struct {
			char	f_hname[MAXHOSTNAMELEN+1];
			struct sockaddr_in	f_addr;
		} f_forw;		/* forwarding address */
		char	f_fname[MAXFNAME];
	} f_un;
	char	f_prevline[MAXSVLINE];		/* last message logged */
	char	f_lasttime[16];			/* time of last occurrence */
	char	f_prevhost[MAXHOSTNAMELEN+1];	/* host from which recd. */
	int	f_prevpri;			/* pri of f_prevline */
	int	f_prevfac;			/* fac of f_prevline */
	int	f_prevlen;			/* length of f_prevline */
	int	f_prevcount;			/* repetition cnt of prevline */
	int	f_repeatcount;			/* number of "repeated" msgs */
};

/*
 * Intervals at which we flush out "message repeated" messages,
 * in seconds after previous message is logged.  After each flush,
 * we move to the next interval until we reach the largest.
 */
int	repeatinterval[] = { 30, 120, 600 };	/* # of secs before flush */
#define	MAXREPEAT ((sizeof(repeatinterval) / sizeof(repeatinterval[0])) - 1)
#define	REPEATTIME(f)	((f)->f_time + repeatinterval[(f)->f_repeatcount])
#define	BACKOFF(f)	{ if (++(f)->f_repeatcount > MAXREPEAT) \
				 (f)->f_repeatcount = MAXREPEAT; \
			}

/* values for f_type */
#define F_UNUSED	0		/* unused entry */
#define F_FILE		1		/* regular file */
#define F_TTY		2		/* terminal */
#define F_CONSOLE	3		/* console terminal */
#define F_FORW		4		/* remote machine */
#define F_USERS		5		/* list of users */
#define F_WALL		6		/* everyone logged on */
#define F_ZEPHYR	7		/* use zephyr notification system */

char	*TypeNames[8] = {
	"UNUSED",	"FILE",		"TTY",		"CONSOLE",
	"FORW",		"USERS",	"WALL",		"ZEPHYR"
};

struct	filed *Files;
struct	filed consfile;

int	Debug;			/* debug flag */
char	LocalHostName[MAXHOSTNAMELEN+1];	/* our hostname */
char	*LocalDomain;		/* our local domain name */
int	InetInuse = 0;		/* non-zero if INET sockets are being used */
int	finet;			/* Internet datagram socket */
int	LogPort;		/* port number for INET connections */
int	Initialized = 0;	/* set when we have initialized ourselves */
int	MarkInterval = 20 * 60;	/* interval between marks in seconds */
int	MarkSeq = 0;		/* mark sequence number */
time_t	now;

ZNotice_t znotice;              /* for zephyr notices */


/* used by cfline and now zephyr ... be careful that the order is consistent
   with syslog.h or Zephyr messages will contain bogus info ... */

struct code {
	char	*c_name;
	int	c_val;
};

struct code	PriNames[] = {
	{ "panic",	LOG_EMERG },
	{ "alert",	LOG_ALERT },
	{ "crit",	LOG_CRIT },
	{ "error",	LOG_ERR },
	{ "warning",	LOG_WARNING },
	{ "notice",	LOG_NOTICE },
	{ "info",	LOG_INFO },
	{ "debug",	LOG_DEBUG },
	{ "none",	NOPRI },
	{ "emerg",	LOG_EMERG },
	{ "err",	LOG_ERR },
	{ "warn",	LOG_WARNING },
	{ NULL,		-1 }
};

/* reserved added so zephyr can use this table */
struct code	FacNames[] = {
	{ "kern",	LOG_KERN },
	{ "user",	LOG_USER },
	{ "mail",	LOG_MAIL },
	{ "daemon",	LOG_DAEMON },
	{ "auth",	LOG_AUTH },
	{ "syslog",	LOG_SYSLOG },
	{ "lpr",	LOG_LPR },
	{ "news",	LOG_NEWS },
	{ "uucp",	LOG_UUCP },
	{ "cron",	LOG_CRON },
	{ "authpriv",	LOG_AUTHPRIV },
	{ "ftp",	LOG_FTP },
	{ "reserved",	-1 },
	{ "reserved",	-1 },
	{ "reserved",	-1 },
	{ "cron",	LOG_CRON },
	{ "local0",	LOG_LOCAL0 },
	{ "local1",	LOG_LOCAL1 },
	{ "local2",	LOG_LOCAL2 },
	{ "local3",	LOG_LOCAL3 },
	{ "local4",	LOG_LOCAL4 },
	{ "local5",	LOG_LOCAL5 },
	{ "local6",	LOG_LOCAL6 },
	{ "local7",	LOG_LOCAL7 },
	{ "security",	LOG_AUTH },
	{ "mark",	LOG_MARK },
	{ NULL,		-1 }
};

static void usage __P((void));
static void untty __P((void));
static void printline __P((const char *hname, char *msg));
#ifndef STREAMS_LOG_DRIVER
static void printsys __P((char *msg));
#endif
static void logmsg __P((int pri, const char *msg, const char *from,
			int flags));
static void fprintlog __P((register struct filed *f, int flags,
			   const char *msg, int fac, int prilev));
static RETSIGTYPE endtty __P((int sig));
static void wallmsg __P((register struct filed *f, struct iovec *iov));
static RETSIGTYPE reapchild __P((int sig));
static const char *cvthname __P((struct sockaddr_in *f));
static RETSIGTYPE domark __P((int sig));
static void logerror __P((const char *type));
static RETSIGTYPE die __P((int sig));
static RETSIGTYPE init __P((int sig));
static void cfline __P((char *line, register struct filed *f));
static int decode __P((char *name, struct code *codetab));
#ifdef HAVE_SRC
static void handle_src __P((struct srcreq packet));
static void send_src_reply __P((struct srcreq orig_packet, int rtncode,
				char *packet, int len));
#endif

int main(argc, argv)
	int argc;
	char **argv;
{
	register int i;
	register char *p;
	int funix, inetm = 0, klogm, len;
#ifndef STREAMS_LOG_DRIVER
	struct sockaddr_un sunx, fromunix;
	int fklog;
#endif
	struct sockaddr_in sin, frominet;
	FILE *fp;
#ifdef _POSIX_VERSION
	struct sigaction action;
#endif
#ifdef HAVE_SRC
	int using_src = 1;
	struct sockaddr srcsockaddr, fromsrc;
	struct srcreq srcreq;
	void handle_src();
	int addrlen, src_fd;
#endif
#ifdef COMPAT42
	char line[BUFSIZ + 1];
#else
	char line[MSG_BSIZE + 1];
#endif

	strcpy(ConfFile, "/etc/syslog.conf");
	sprintf(PidFile, "%ssyslog.pid", PIDDIR);
	while (--argc > 0) {
		p = *++argv;
		if (p[0] != '-')
			usage();
		switch (p[1]) {
		case 'f':		/* configuration file */
			if (p[2] != '\0')
				strcpy(ConfFile, &p[2]);
			break;

		case 'd':		/* debug */
			Debug++;
			break;

		case 'p':		/* path */
			if (p[2] != '\0')
				LogName = &p[2];
			break;

		case 'm':		/* mark interval */
			if (p[2] != '\0')
				MarkInterval = atoi(&p[2]) * 60;
			break;

		default:
			usage();
		}
	}

#ifdef HAVE_SRC
	addrlen = sizeof(struct sockaddr);
	if (getsockname(0, &srcsockaddr, &addrlen) < 0) {
		using_src = 0;
		fprintf(stderr,"syslogd: %s: Error in getsockname: %s; continuing without SRC support",
			argv[0], strerror(errno));
	}
	if (using_src && ((src_fd = dup(0)) < 0)) {
		using_src = 0;
		fprintf(stderr,"syslogd: %s: Error in dup: %s; continuing without SRC support",
			argv[0], strerror(errno));
	}
#endif

	if (!Debug) {
#ifdef HAVE_SRC
		/* Don't fork if using SRC;
		 * SRC will think the program exited */
		if (!using_src)
#endif
		    if (fork())
			exit(0);

		for (i = 0; i < 10; i++)
			(void) close(i);
		(void) open("/", 0);
		(void) dup2(0, 1);
		(void) dup2(0, 2);
		untty();
	} else {
#ifdef _POSIX_VERSION
		static char buf[BUFSIZ];
		setvbuf (stdout, buf, _IOLBF, BUFSIZ);
#else
		setlinebuf(stdout);
#endif
	}

	consfile.f_type = F_CONSOLE;
	(void) strcpy(consfile.f_un.f_fname, ctty);
	(void) gethostname(LocalHostName, sizeof LocalHostName);
	if ((p = strchr(LocalHostName, '.')) != NULL) {
		*p++ = '\0';
		LocalDomain = p;
	}
	else
		LocalDomain = "";
#ifdef _POSIX_VERSION
	action.sa_flags = 0;
	sigemptyset(&action.sa_mask);

	action.sa_handler = die;
	sigaction(SIGTERM, &action, NULL);

	action.sa_handler = Debug ? die : SIG_IGN;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGQUIT, &action, NULL);

	action.sa_handler = reapchild;
	sigaction(SIGCHLD, &action, NULL);

	action.sa_handler = domark;
	sigaction(SIGALRM, &action, NULL);
#else
	(void) signal(SIGTERM, die);
	if (Debug) {
		(void) signal(SIGINT, die);
		(void) signal(SIGQUIT, die);
	} else {
		(void) signal(SIGINT, SIG_IGN);
		(void) signal(SIGQUIT, SIG_IGN);
	}
	(void) signal(SIGCHLD, reapchild);
	(void) signal(SIGALRM, domark);
#endif
	(void) alarm(TIMERINTVL);
 
#ifndef STREAMS_LOG_DRIVER
	(void) unlink(LogName);

	sunx.sun_family = AF_UNIX;
	(void) strncpy(sunx.sun_path, LogName, sizeof sunx.sun_path);
	funix = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (funix < 0 ||
	    bind(funix, (struct sockaddr *) &sunx, sizeof(sunx)) < 0 ||
	    chmod(LogName, 0666) < 0) {
		(void) sprintf(line, "cannot create %s", LogName);
		logerror(line);
		dprintf("cannot create %s (%d)\n", LogName, errno);
		die(0);
	}
#else /* STREAMS_LOG_DRIVER */
	if ((funix=open(LogName, O_RDONLY)) < 0) {
	  sprintf(line, "cannot open %s", LogName);
	  logerror(line);
	  dprintf("cannot open %s (%d)\n", LogName, errno);
	  die(0);
	}
	{
	  struct strioctl ioc;
	  ioc.ic_cmd = I_CONSLOG;
	  ioc.ic_timout = 0;		/* 15 seconds default */
	  ioc.ic_len = 0; ioc.ic_dp = NULL;
	  if (ioctl(funix, I_STR, &ioc) < 0) {
	    sprintf(line, "cannot setup STREAMS logging on %s", LogName);
	    logerror(line);
	    dprintf("cannot setup STREAMS logging on %s (%d)", LogName,
		    errno);
	    die(0);
	  }
	}
#endif /* STREAMS_LOG_DRIVER */
	finet = socket(AF_INET, SOCK_DGRAM, 0);
	if (finet >= 0) {
		struct servent *sp;

		sp = getservbyname("syslog", "udp");
		if (sp == NULL) {
			errno = 0;
			logerror("syslog/udp: unknown service");
			die(0);
		}
		(void) memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = LogPort = sp->s_port;
#ifdef COMPAT42
		(void) close(finet);
#else
		if (bind(finet, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
			logerror("bind");
			if (!Debug)
				die(0);
		} else {
			inetm = FDMASK(finet);
			InetInuse = 1;
		}
#endif /* COMPAT42 */
	}
#ifdef COMPAT42
	InetInuse = 1;
	inetm = 0;
	klogm = 0;
#else
#ifdef STREAMS_LOG_DRIVER
	klogm = 0;
#else
	if ((fklog = open("/dev/klog", O_RDONLY)) >= 0)
		klogm = FDMASK(fklog);
	else {
		dprintf("can't open /dev/klog (%d)\n", errno);
		klogm = 0;
	}
#endif
#endif /* COMPAT42 */

	/* tuck my process id away */
	fp = fopen(PidFile, "w");
	if (fp != NULL) {
		fprintf(fp, "%d\n", (int) getpid());
		(void) fclose(fp);
	}

	dprintf("off & running....\n");

	/* initialize zephyr stuff */
	(void) memset (&znotice, 0, sizeof (znotice));
	znotice.z_kind = UNSAFE;
	znotice.z_class = "SYSLOG";
	znotice.z_class_inst = LocalHostName;
	znotice.z_default_format = "Syslog message from $instance, level $opcode:\n$message";
	(void) ZInitialize ();

	init(0);
#ifdef _POSIX_VERSION
	action.sa_handler = init;
	sigaction(SIGHUP, &action, NULL);
#else
	(void) signal(SIGHUP, init);
#endif

	for (;;) {
		int nfds;
#ifdef STREAMS_LOG_DRIVER
		struct pollfd readfds[2] = {
		  { funix, POLLIN | POLLPRI, 0 }, 
		  { finet, POLLIN | POLLPRI, 0 } };
#  define POLLFD_unix 0
#  define POLLFD_inet 1

		errno = 0; dprintf("readfds = {(%d,%#x),(%d,%#x)}\n",
				   readfds[POLLFD_unix].fd,
				   readfds[POLLFD_unix].revents,
				   readfds[POLLFD_inet].fd,
				   readfds[POLLFD_inet].revents);
		nfds = poll(readfds, 2, INFTIM);

#else /* STREAMS_LOG_DRIVER */
		int readfds = FDMASK(funix) | inetm | klogm;

#ifdef HAVE_SRC
		if (using_src)
		  readfds |= FDMASK(src_fd);
#endif
		errno = 0;
		dprintf("readfds = %#x\n", readfds);
		nfds = select(20, (fd_set *) &readfds, (fd_set *) NULL,
				  (fd_set *) NULL, (struct timeval *) NULL);
#endif /* STREAMS_LOG_DRIVER */
		if (nfds == 0)
			continue;
		if (nfds < 0) {
			if (errno != EINTR)
#ifdef STREAMS_LOG_DRIVER
				logerror("poll");
#else
				logerror("select");
#endif
			continue;
		}
#ifdef STREAMS_LOG_DRIVER
		dprintf("got a message {(%d, %#x), (%d, %#x)}\n",
			readfds[POLLFD_unix].fd, readfds[0].revents,
			readfds[POLLFD_inet].fd, readfds[1].revents);
		if (readfds[POLLFD_unix].revents & (POLLIN|POLLPRI)) {
		  struct log_ctl logctl;
		  char datbuf[BUFSIZ];
		  struct strbuf dat = { (sizeof(int)*2+sizeof(datbuf)),
		    0, datbuf };
		  struct strbuf ctl = {
		    (sizeof(int)*2+sizeof(logctl)), 0, (char*)&logctl };
		  int flags = 0;

  		  i = getmsg(funix, &ctl, &dat, &flags);
		  if ((i==0)) {
#if (NLOGARGS != 3)
		      error "This section of code assumes that NLOGARGS is 3.";
		      error "If that's not the case, this needs to be editted";
		      error "by hand. Sorry, but sed magic was too much for";
		      error "me.";
#else
		    {
		    char null[] = "", *p;
		    char *logargs[NLOGARGS] = { null, null, null };

		    logargs[0]=dat.buf;
		    p=memchr(logargs[0], 0, sizeof(dat.buf)); /* XXX */
	            if (p != NULL)
			logargs[1]=p++;
		    p=memchr(logargs[1], 0, sizeof(dat.buf)); /* XXX */
	            if (p != NULL)
			logargs[2]=p++;
		    sprintf(line, logargs[0], logargs[1], logargs[2]);
		    }
#endif /* NLOGARGS != 3 */
		    line[strlen(line)-1]=0;
		    logmsg(logctl.pri, line, LocalHostName, 0);
		  } else if (i < 0 && errno != EINTR) {
		    logerror("getmsg() unix");
		  } else if (i > 0) {
		    sprintf(line, "getmsg() > 1 (%X)", i);
		    logerror(line);
 		  }
		}
#else /* STREAMS_LOG_DRIVER */
		dprintf("got a message (%d, %#x)\n", nfds, readfds);
		if (readfds & klogm) {
			i = read(fklog, line, sizeof(line) - 1);
			if (i > 0) {
				line[i] = '\0';
				printsys(line);
			} else if (i < 0 && errno != EINTR) {
				logerror("klog");
				fklog = -1;
				klogm = 0;
			}
		}
		if (readfds & FDMASK(funix)) {
			len = sizeof fromunix;
			i = recvfrom(funix, line, MAXLINE, 0,
				     (struct sockaddr *) &fromunix, &len);
			if (i > 0) {
				line[i] = '\0';
				printline(LocalHostName, line);
			} else if (i < 0 && errno != EINTR)
				logerror("recvfrom unix");
		}
#endif /* STREAMS_LOG_DRIVER */
#ifdef STREAMS_LOG_DRIVER
		if (readfds[POLLFD_inet].revents & (POLLIN|POLLPRI)) {
#else
		if (readfds & inetm) {
#endif
			len = sizeof frominet;
			i = recvfrom(finet, line, MAXLINE, 0,
				     (struct sockaddr *) &frominet, &len);
			if (i > 0) {
				line[i] = '\0';
				printline(cvthname(&frominet), line);
			} else if (i < 0 && errno != EINTR)
				logerror("recvfrom inet");
		} 
#ifdef HAVE_SRC
		dprintf("%d %d %d\n", using_src, readfds, FDMASK(src_fd));
		if (using_src && (readfds & (FDMASK(src_fd)))) {
		  dprintf("got a src packet\n");
		  len = sizeof(fromsrc);
		  i = recvfrom(src_fd, &srcreq, sizeof(srcreq), 0, &fromsrc,
			      &len);
		  dprintf("finished recvfrom, %d\n",i);
		}
#if 0
		  if (i > 0) {
		    handle_src(srcreq);
		  } else if (i < 0 && errno != EINTR)
		    logerror("recvfrom src");
		}
#endif
#endif /* HAVE_SRC */
	}
}

static void usage()
{
	fprintf(stderr, "usage: syslogd [-d] [-mmarkinterval] [-ppath] [-fconffile]\n");
	exit(1);
}

static void untty()
{
	int i;

	if (!Debug) {
		i = open("/dev/tty", O_RDWR);
		if (i >= 0) {
#ifdef TIOCNOTTY /* Only necessary on old systems. */
			(void) ioctl(i, (int) TIOCNOTTY, (char *)0);
#endif
			(void) close(i);
		}
	}
}

/*
 * Take a raw input line, decode the message, and print the message
 * on the appropriate log files.
 */

static void printline(hname, msg)
	const char *hname;
	char *msg;
{
	register char *p, *q;
	register int c;
	char line[MAXLINE + 1];
	int pri;

	/* test for special codes */
	pri = DEFUPRI;
	p = msg;
	if (*p == '<') {
		pri = 0;
		while (isdigit(*++p))
			pri = 10 * pri + (*p - '0');
		if (*p == '>')
			++p;
	}
	if (pri &~ (LOG_FACMASK|LOG_PRIMASK))
		pri = DEFUPRI;

	/* don't allow users to log kernel messages */
	if (LOG_FAC(pri) == LOG_KERN)
		pri = LOG_MAKEPRI(LOG_USER, LOG_PRI(pri));

	q = line;

	while ((c = *p++ & 0177) != '\0' && c != '\n' &&
	    q < &line[sizeof(line) - 1]) {
		if (iscntrl(c)) {
			*q++ = '^';
			*q++ = c ^ 0100;
		} else
			*q++ = c;
	}
	*q = '\0';

	logmsg(pri, line, hname, 0);
}

/*
 * Take a raw input line from /dev/klog, split and format similar to syslog().
 */

#ifndef STREAMS_LOG_DRIVER
static void printsys(msg)
	char *msg;
{
	register char *p, *q;
	register int c;
	char line[MAXLINE + 1];
	int pri, flags;
	char *lp;

	(void) sprintf(line, "vmunix: ");
	lp = line + strlen(line);
	for (p = msg; *p != '\0'; ) {
		flags = SYNC_FILE | ADDDATE;	/* fsync file after write */
		pri = DEFSPRI;
		if (*p == '<') {
			pri = 0;
			while (isdigit(*++p))
				pri = 10 * pri + (*p - '0');
			if (*p == '>')
				++p;
		} else {
			/* kernel printf's come out on console */
			flags |= IGN_CONS;
		}
		if (pri &~ (LOG_FACMASK|LOG_PRIMASK))
			pri = DEFSPRI;
		q = lp;
		while (*p != '\0' && (c = *p++) != '\n' &&
		    q < &line[MAXLINE])
			*q++ = c;
		*q = '\0';
		logmsg(pri, line, LocalHostName, flags);
	}
}
#endif

/*
 * Log a message to the appropriate log files, users, etc. based on
 * the priority.
 */

static void logmsg(pri, msg, from, flags)
	int pri;
	const char *msg, *from;
	int flags;
{
	register struct filed *f;
	int fac, prilev, msglen;
	const char *timestamp;
#ifdef _POSIX_VERSION
	sigset_t osig, sig;
#else
	int omask;
#endif

	dprintf("logmsg: pri %o, flags %x, from %s, msg %s\n", pri, flags, from, msg);

#ifdef _POSIX_VERSION
	(void) sigemptyset(&sig);
	(void) sigaddset(&sig, SIGHUP);
	(void) sigaddset(&sig, SIGALRM);
	(void) sigprocmask(SIG_BLOCK, &sig, &osig);
#else
	omask = sigblock(sigmask(SIGHUP)|sigmask(SIGALRM));
#endif

	/*
	 * Check to see if msg looks non-standard.
	 */
	msglen = strlen(msg);
	if (msglen < 16 || msg[3] != ' ' || msg[6] != ' ' ||
	    msg[9] != ':' || msg[12] != ':' || msg[15] != ' ')
		flags |= ADDDATE;

	(void) time(&now);
	if (flags & ADDDATE)
		timestamp = ctime(&now) + 4;
	else {
		timestamp = msg;
		msg += 16;
		msglen -= 16;
	}

	/* extract facility and priority level */
	if (flags & MARK)
		fac = LOG_NFACILITIES;
	else
		fac = LOG_FAC(pri);
	prilev = LOG_PRI(pri);

	/* log the message to the particular outputs */
	if (!Initialized) {
		f = &consfile;
		f->f_file = open(ctty, O_WRONLY);

		if (f->f_file >= 0) {
			untty();
			fprintlog(f, flags, (char *)NULL, fac, prilev);
			(void) close(f->f_file);
		}
#ifdef _POSIX_VERSION
		(void) sigprocmask(SIG_SETMASK, &osig, (sigset_t *)0);
#else
		(void) sigsetmask(omask);
#endif
		return;
	}
	for (f = Files; f; f = f->f_next) {
		/* skip messages that are incorrect priority */
		if (f->f_pmask[fac] < prilev || f->f_pmask[fac] == NOPRI)
			continue;

		if (f->f_type == F_CONSOLE && (flags & IGN_CONS))
			continue;

		/* don't output marks to recently written files */
		if ((flags & MARK) && (now - f->f_time) < MarkInterval / 2)
			continue;

		/*
		 * suppress duplicate lines to this file
		 */
		if ((flags & MARK) == 0 && msglen == f->f_prevlen &&
		    !strcmp(msg, f->f_prevline) &&
		    !strcmp(from, f->f_prevhost)) {
			(void) strncpy(f->f_lasttime, timestamp, 15);
			f->f_prevcount++;
			dprintf("msg repeated %d times, %ld sec of %d\n",
			    f->f_prevcount, (long)(now - f->f_time),
			    repeatinterval[f->f_repeatcount]);
			/*
			 * If domark would have logged this by now,
			 * flush it now (so we don't hold isolated messages),
			 * but back off so we'll flush less often
			 * in the future.
			 */
			if (now > REPEATTIME(f)) {
				fprintlog(f, flags, (char *)NULL, fac, prilev);
				BACKOFF(f);
			}
		} else {
			/* new line, save it */
			if (f->f_prevcount)
				fprintlog(f, 0, (char *)NULL, fac, prilev);
			f->f_repeatcount = 0;
			(void) strncpy(f->f_lasttime, timestamp, 15);
			(void) strncpy(f->f_prevhost, from,
					sizeof(f->f_prevhost));
			if (msglen < MAXSVLINE) {
				f->f_prevlen = msglen;
				f->f_prevpri = pri;
				f->f_prevfac = fac;
				(void) strcpy(f->f_prevline, msg);
				fprintlog(f, flags, (char *)NULL, fac, prilev);
			} else {
				f->f_prevline[0] = 0;
				f->f_prevlen = 0;
				fprintlog(f, flags, msg, fac, prilev);
			}
		}
	}
#ifdef _POSIX_VERSION
	(void) sigprocmask(SIG_SETMASK, &osig, (sigset_t *)0);
#else
	(void) sigsetmask(omask);
#endif
}

static void fprintlog(f, flags, msg, fac, prilev)
	register struct filed *f;
	int flags;
	const char *msg;
	int fac, prilev;
{
	struct iovec iov[6];
	register struct iovec *v = iov;
	register int l;
	char line[MAXLINE + 1];
	char repbuf[80];
	char pri_fac_str[35];
	int i;
	Code_t zcode;

	v->iov_base = f->f_lasttime;
	v->iov_len = 15;
	v++;
	v->iov_base = " ";
	v->iov_len = 1;
	v++;
	v->iov_base = f->f_prevhost;
	v->iov_len = strlen(v->iov_base);
	v++;
	v->iov_base = " ";
	v->iov_len = 1;
	v++;
	if (msg) {
		v->iov_base = (char *) msg;
		v->iov_len = strlen(msg);
	} else if (f->f_prevcount > 1) {
		(void) sprintf(repbuf, "last message repeated %d times",
		    f->f_prevcount);
		v->iov_base = repbuf;
		v->iov_len = strlen(repbuf);
	} else {
		v->iov_base = f->f_prevline;
		v->iov_len = f->f_prevlen;
	}
	v++;

	dprintf("Logging to %s", TypeNames[f->f_type]);
	f->f_time = now;

	switch (f->f_type) {
	case F_UNUSED:
		dprintf("\n");
		break;

	case F_FORW:
		dprintf(" %s\n", f->f_un.f_forw.f_hname);
		(void) sprintf(line, "<%d>%.15s %s", f->f_prevpri,
			iov[0].iov_base, iov[4].iov_base);
		l = strlen(line);
		if (l > MAXLINE)
			l = MAXLINE;
#ifdef COMPAT42
		if (sendto(f->f_file, line, l, 0,
			   (struct sockaddr *) &f->f_un.f_forw.f_addr,
#else
		if (sendto(finet, line, l, 0,
			   (struct sockaddr *) &f->f_un.f_forw.f_addr,
#endif
		    sizeof f->f_un.f_forw.f_addr) != l) {
			int e = errno;
			(void) close(f->f_file);
			f->f_type = F_UNUSED;
			errno = e;
			logerror("sendto");
		}
		break;

	case F_CONSOLE:
		if (flags & IGN_CONS) {
			dprintf(" (ignored)\n");
			break;
		}
		/* FALLTHROUGH */

	case F_TTY:
	case F_FILE:
		dprintf(" %s\n", f->f_un.f_fname);
		if (f->f_type != F_FILE) {
			v->iov_base = "\r\n";
			v->iov_len = 2;
		} else {
			v->iov_base = "\n";
			v->iov_len = 1;
		}
	again:
		if (writev(f->f_file, iov, 6) < 0) {
			int e = errno;
			(void) close(f->f_file);
			/*
			 * Check for EBADF on TTY's due to vhangup() XXX
			 */
			if (e == EBADF && f->f_type != F_FILE) {
				f->f_file = open(f->f_un.f_fname, O_WRONLY|O_APPEND);
				if (f->f_file < 0) {
					f->f_type = F_UNUSED;
					logerror(f->f_un.f_fname);
				} else {
					untty();
					goto again;
				}
			} else {
				f->f_type = F_UNUSED;
				errno = e;
				logerror(f->f_un.f_fname);
			}
		} else if (flags & SYNC_FILE)
			(void) fsync(f->f_file);
		break;

 	        case F_ZEPHYR:
			(void) sprintf(line, "%.15s [%s] %s",
				       iov[0].iov_base,
				       iov[2].iov_base,
				       iov[4].iov_base);
			if (!msg && f->f_prevcount > 1) {
				/* Include previous line with Zephyrgram. */
				sprintf(line + strlen(line), ":\n%*s",
					f->f_prevlen, f->f_prevline);
			}
			(void) sprintf(pri_fac_str, "%s.%s", 
				       FacNames[fac].c_name,
				       PriNames[(prilev & LOG_PRIMASK)].c_name);
			znotice.z_message = line;
			/* include the null just in case */
			znotice.z_message_len = strlen (line) + 1;
			znotice.z_opcode = pri_fac_str;
			dprintf (" z_opcode %s\n", pri_fac_str);
			for (i = 0; i < MAXUNAMES; i++) {
			  if (!f->f_un.f_uname[i]) 
			    break;
			  /* map "*" into null recipient and therefore
			     anybody who is listening */
			  if (strcmp (f->f_un.f_uname[i], "*") == 0)
			    znotice.z_recipient = "";
			  else
			    znotice.z_recipient = f->f_un.f_uname[i];
			  zcode = ZSendNotice (&znotice, ZNOAUTH); 
			  if (zcode != 0)
			    logerror (error_message (zcode));
			}
			break;

	case F_USERS:
	case F_WALL:
		dprintf("\n");
		v->iov_base = "\r\n";
		v->iov_len = 2;
		wallmsg(f, iov);
		break;
	}
	f->f_prevcount = 0;
}

#ifdef _POSIX_VERSION
sigjmp_buf ttybuf;
#else
jmp_buf ttybuf;
#endif

static RETSIGTYPE endtty(sig)
	int sig;
{
#ifdef _POSIX_VERSION
	siglongjmp(ttybuf, 1);
#else
	longjmp(ttybuf, 1);
#endif
}

/*
 *  WALLMSG -- Write a message to the world at large
 *
 *	Write the specified message to either the entire
 *	world, or a list of approved users.
 */

static void wallmsg(f, iov)
	register struct filed *f;
	struct iovec *iov;
{
	register int i;
	int ttyf, len;
	FILE *uf;
	static int reenter = 0;
	struct utmp ut;
	static char p[6+sizeof(ut.ut_line)] = "/dev/";
	char greetings[200];
#ifdef _POSIX_VERSION
	struct sigaction action;
#endif

	if (reenter++)
		return;

	/* open the user login file */
	if ((uf = fopen("/etc/utmp", "r")) == NULL) {
		logerror("/etc/utmp");
		reenter = 0;
		return;
	}

	/*
	 * Might as well fork instead of using nonblocking I/O
	 * and doing notty().
	 */
	if (fork() == 0) {
#ifdef _POSIX_VERSION
		action.sa_flags = 0;
		(void) sigemptyset(&action.sa_mask);
		
		action.sa_handler = SIG_DFL;
		(void) sigaction(SIGTERM, &action, NULL);
		
		alarm(0);
		action.sa_handler = endtty;
		(void) sigaction(SIGALRM, &action, NULL);
		
		action.sa_handler = SIG_IGN;
		(void) sigaction(SIGTTOU, &action, NULL);
		
		(void)sigprocmask(SIG_SETMASK, &action.sa_mask, (sigset_t *)0);
#else
		(void) signal(SIGTERM, SIG_DFL);
		(void) alarm(0);
		(void) signal(SIGALRM, endtty);
		(void) signal(SIGTTOU, SIG_IGN);
		(void) sigsetmask(0);
#endif
		(void) sprintf(greetings,
		    "\r\n\7Message from syslogd@%s at %.24s ...\r\n",
			iov[2].iov_base, ctime(&now));
		len = strlen(greetings);

		/* scan the user login file */
		while (fread((char *) &ut, sizeof ut, 1, uf) == 1) {
			/* is this slot used? */
			if (ut.ut_name[0] == '\0')
				continue;

			/* should we send the message to this user? */
			if (f->f_type == F_USERS) {
				for (i = 0; i < MAXUNAMES; i++) {
					if (!f->f_un.f_uname[i]) {
						i = MAXUNAMES;
						break;
					}
					if (strncmp(f->f_un.f_uname[i],
					    ut.ut_name, sizeof(ut.ut_name)) == 0)
						break;
				}
				if (i >= MAXUNAMES)
					continue;
			}

			/* compute the device name */
			strncpy(&p[5], ut.ut_line, sizeof(ut.ut_line));

			if (f->f_type == F_WALL) {
				iov[0].iov_base = greetings;
				iov[0].iov_len = len;
				iov[1].iov_len = 0;
			}
#ifdef _POSIX_VERSION
			if (sigsetjmp(ttybuf, 1) == 0)
#else
			if (setjmp(ttybuf) == 0)
#endif
			{
				(void) alarm(15);
				/* open the terminal */
				ttyf = open(p, O_WRONLY);
				if (ttyf >= 0) {
					struct stat statb;

					if (fstat(ttyf, &statb) == 0 &&
					    (statb.st_mode & S_IWRITE))
						(void) writev(ttyf, iov, 6);
					close(ttyf);
					ttyf = -1;
				}
			}
			(void) alarm(0);
		}
		exit(0);
	}
	/* close the user login file */
	(void) fclose(uf);
	reenter = 0;
}

static RETSIGTYPE reapchild(sig)
	int sig;
{
#ifdef HAVE_WAITPID
	int status;
	while (waitpid(-1, &status, WNOHANG) > 0) ;
#else
	union wait status;
	while (wait3(&status, WNOHANG, (struct rusage *) NULL) > 0) ;
#endif
}

/*
 * Return a printable representation of a host address.
 */
static const char *cvthname(f)
	struct sockaddr_in *f;
{
	struct hostent *hp;
	register char *p;
	extern char *inet_ntoa();

	dprintf("cvthname(%s)\n", inet_ntoa(f->sin_addr));

	if (f->sin_family != AF_INET) {
		dprintf("Malformed from address\n");
		return ("???");
	}
	hp = gethostbyaddr((const char *) &f->sin_addr, sizeof(struct in_addr),
			   f->sin_family);
	if (hp == 0) {
		dprintf("Host name for your address (%s) unknown\n",
			inet_ntoa(f->sin_addr));
		return (inet_ntoa(f->sin_addr));
	}
	if ((p = strchr(hp->h_name, '.')) && strcmp(p + 1, LocalDomain) == 0)
		*p = '\0';
	return (hp->h_name);
}

static RETSIGTYPE domark(sig)
	int sig;
{
	register struct filed *f;

	now = time(0);
	MarkSeq += TIMERINTVL;
	if (MarkSeq >= MarkInterval) {
		logmsg(LOG_INFO, "-- MARK --", LocalHostName, ADDDATE|MARK);
		MarkSeq = 0;
	}

	for (f = Files; f; f = f->f_next) {
		if (f->f_prevcount && now >= REPEATTIME(f)) {
			dprintf("flush %s: repeated %d times, %d sec.\n",
			    TypeNames[f->f_type], f->f_prevcount,
			    repeatinterval[f->f_repeatcount]);
			fprintlog(f, 0, (char *)NULL, f->f_prevfac,
				  f->f_prevpri);
			BACKOFF(f);
		}
	}
	(void) alarm(TIMERINTVL);
}

/*
 * Print syslogd errors some place.
 */
static void logerror(type)
	const char *type;
{
	char buf[100];

	sprintf(buf, "syslogd: %s", strerror(errno));
	errno = 0;
	dprintf("%s\n", buf);
	logmsg(LOG_SYSLOG|LOG_ERR, buf, LocalHostName, ADDDATE);
}

static RETSIGTYPE die(sig)
    	int sig;
{
	register struct filed *f;
	char buf[100];

	for (f = Files; f != NULL; f = f->f_next) {
		/* flush any pending output */
		if (f->f_prevcount)
			fprintlog(f, 0, (char *)NULL, f->f_prevfac,
				  f->f_prevpri);
	}
	if (sig) {
		dprintf("syslogd: exiting on signal %d\n", sig);
		(void) sprintf(buf, "exiting on signal %d", sig);
		errno = 0;
		logerror(buf);
	}
#ifndef STREAMS_LOG_DRIVER
  	(void) unlink(LogName);
#endif
	exit(0);
}

/*
 *  INIT -- Initialize syslogd from configuration table
 */

static RETSIGTYPE init(sig)
	int sig;
{
	register int i;
	register FILE *cf;
	register struct filed *f, *next, **nextp;
	register char *p;
	char cline[BUFSIZ];

	dprintf("init\n");

	/*
	 *  Close all open log files.
	 */
	Initialized = 0;
	for (f = Files; f != NULL; f = next) {
		/* flush any pending output */
		if (f->f_prevcount)
			fprintlog(f, 0, (char *)NULL, f->f_prevfac,
				  f->f_prevpri);
		switch (f->f_type) {
		  case F_FILE:
		  case F_TTY:
		  case F_CONSOLE:
#ifdef COMPAT42
		  case F_FORW:
#endif
			(void) close(f->f_file);
			break;

		  case F_USERS:
		  case F_ZEPHYR:

			for (i = 0; i < MAXUNAMES && f->f_un.f_uname[i]; i++)
			    free(f->f_un.f_uname[i]);
			break;
		}
		next = f->f_next;
		free((char *) f);
	}
	Files = NULL;
	nextp = &Files;

	/* open the configuration file */
	if ((cf = fopen(ConfFile, "r")) == NULL) {
		dprintf("cannot open %s\n", ConfFile);
		*nextp = (struct filed *)calloc(1, sizeof(*f));
		cfline("*.ERR\t/dev/console", *nextp);
		(*nextp)->f_next = (struct filed *)calloc(1, sizeof(*f));
		cfline("*.PANIC\t*", (*nextp)->f_next);
		Initialized = 1;
		return;
	}

	/*
	 *  Foreach line in the conf table, open that file.
	 */
	f = NULL;
	while (fgets(cline, sizeof cline, cf) != NULL) {
		/*
		 * check for end-of-section, comments, strip off trailing
		 * spaces and newline character.
		 */
		for (p = cline; isspace(*p); ++p);
		if (*p == '\0' || *p == '#')
			continue;
		for (p = strchr(cline, '\0'); isspace(*--p););
		*++p = '\0';
		f = (struct filed *)calloc(1, sizeof(*f));
		*nextp = f;
		nextp = &f->f_next;
		cfline(cline, f);
	}

	/* close the configuration file */
	(void) fclose(cf);

	Initialized = 1;

	if (Debug) {
		for (f = Files; f; f = f->f_next) {
			for (i = 0; i <= LOG_NFACILITIES; i++)
				if (f->f_pmask[i] == NOPRI)
					printf("X ");
				else
					printf("%d ", f->f_pmask[i]);
			printf("%s: ", TypeNames[f->f_type]);
			switch (f->f_type) {
			case F_FILE:
			case F_TTY:
			case F_CONSOLE:
				printf("%s", f->f_un.f_fname);
				break;

			case F_FORW:
				printf("%s", f->f_un.f_forw.f_hname);
				break;

			case F_USERS:
				for (i = 0; i < MAXUNAMES && f->f_un.f_uname[i]; i++)
					printf("%s, ", f->f_un.f_uname[i]);
				break;
			}
			printf("\n");
		}
	}

	logmsg(LOG_SYSLOG|LOG_INFO, "syslogd: restart", LocalHostName, ADDDATE);
	dprintf("syslogd: restarted\n");
}

/*
 * Crack a configuration file line
 */

static void cfline(line, f)
	char *line;
	register struct filed *f;
{
	register char *p;
	register char *q;
	register int i;
	char *bp;
	int pri;
	struct hostent *hp;
	char buf[MAXLINE];

	dprintf("cfline(%s)\n", line);

	errno = 0;	/* keep sys_errlist stuff out of logerror messages */

	/* clear out file entry */
	(void) memset((char *) f, 0, sizeof *f);
	for (i = 0; i <= LOG_NFACILITIES; i++)
		f->f_pmask[i] = NOPRI;

	/* scan through the list of selectors */
	for (p = line; *p && *p != '\t';) {

		/* find the end of this facility name list */
		for (q = p; *q && *q != '\t' && *q++ != '.'; )
			continue;

		/* collect priority name */
		for (bp = buf; *q && !strchr("\t,;", *q); )
			*bp++ = *q++;
		*bp = '\0';

		/* skip cruft */
		while (strchr(", ;", *q))
			q++;

		/* decode priority name */
		pri = decode(buf, PriNames);
		if (pri < 0) {
			char xbuf[200];

			(void) sprintf(xbuf, "unknown priority name \"%s\"", buf);
			logerror(xbuf);
			return;
		}

		/* scan facilities */
		while (*p && !strchr("\t.;", *p)) {
			int i;

			for (bp = buf; *p && !strchr("\t,;.", *p); )
				*bp++ = *p++;
			*bp = '\0';
			if (*buf == '*')
				for (i = 0; i < LOG_NFACILITIES; i++)
					f->f_pmask[i] = pri;
			else {
				i = decode(buf, FacNames);
				if (i < 0) {
					char xbuf[200];

					(void) sprintf(xbuf, "unknown facility name \"%s\"", buf);
					logerror(xbuf);
					return;
				}
				f->f_pmask[i >> 3] = pri;
			}
			while (*p == ',' || *p == ' ')
				p++;
		}

		p = q;
	}

	/* skip to action part */
	while (*p == '\t')
		p++;

	switch (*p)
	{
	case '@':
		if (!InetInuse)
			break;
		(void) strcpy(f->f_un.f_forw.f_hname, ++p);
		hp = gethostbyname(p);
		if (hp == NULL) {
			char buf[100];

			(void) sprintf(buf, "unknown host %s", p);
			errno = 0;
			logerror(buf);
			break;
		}
		(void) memset((char *) &f->f_un.f_forw.f_addr, 0,
			      sizeof f->f_un.f_forw.f_addr);
		f->f_un.f_forw.f_addr.sin_family = AF_INET;
		f->f_un.f_forw.f_addr.sin_port = LogPort;
		(void) memcpy((char *) &f->f_un.f_forw.f_addr.sin_addr,
			      hp->h_addr, hp->h_length);
#ifdef COMPAT42
		f->f_file = socket(AF_INET, SOCK_DGRAM, 0);
		if (f->f_file < 0) {
		    logerror("socket");
		    break;
		}
#endif
		f->f_type = F_FORW;
		break;

	case '/':
		(void) strcpy(f->f_un.f_fname, p);
		if ((f->f_file = open(p, O_WRONLY|O_APPEND)) < 0) {
			f->f_file = F_UNUSED;
			logerror(p);
			break;
		}
		if (isatty(f->f_file)) {
			f->f_type = F_TTY;
			untty();
		}
		else
			f->f_type = F_FILE;
		if (strcmp(p, ctty) == 0)
			f->f_type = F_CONSOLE;
		break;

        case '!':
		p++;
		if (*p == '*') {
		  f->f_type = F_WALL;
		  break;
		}
		for (i = 0; i < MAXUNAMES && *p; i++) {
			for (q = p; *q && *q != ','; )
				q++;
			f->f_un.f_uname[i] = malloc(q - p + 1);
			if (f->f_un.f_uname[i]) {
			    strncpy(f->f_un.f_uname[i], p, q - p);
			    f->f_un.f_uname[i][q - p] = 0;
			} else {
			    break;
			}
			while (*q == ',' || *q == ' ')
				q++;
			p = q;
		}
		f->f_un.f_uname[i] = NULL;
		f->f_type = F_USERS;
		break;

	default:
		for (i = 0; i < MAXUNAMES && *p; i++) {
			for (q = p; *q && *q != ','; )
				q++;
			f->f_un.f_uname[i] = malloc(q - p + 1);
			if (f->f_un.f_uname[i]) {
			    strncpy(f->f_un.f_uname[i], p, q - p);
			    f->f_un.f_uname[i][q - p] = 0;
			} else {
			    break;
			}
			while (*q == ',' || *q == ' ')
				q++;
			p = q;
		}
		f->f_un.f_uname[i] = NULL;
		f->f_type = F_ZEPHYR;
		break;
	}
}


/*
 *  Decode a symbolic name to a numeric value
 */

static int decode(name, codetab)
	char *name;
	struct code *codetab;
{
	register struct code *c;
	register char *p;
	char buf[40];

	if (isdigit(*name))
		return (atoi(name));

	(void) strcpy(buf, name);
	for (p = buf; *p; p++)
		if (isupper(*p))
			*p = tolower(*p);
	for (c = codetab; c->c_name; c++)
		if (!strcmp(buf, c->c_name))
			return (c->c_val);

	return (-1);
}

#ifdef HAVE_SRC
/* Routines for handling the SRC (System Resource Controller) system */

static void handle_src(packet)
    struct srcreq packet;
{
    void send_src_reply();

    dprintf("handling packet");
    switch(packet.subreq.action) {
    case START:
	dprintf("was start");
	send_src_reply(packet, SRC_SUBMSG, 
		       "ERROR: syslogd does not support START requests",
		       sizeof(struct srcrep));
	break;
    case STOP:
	dprintf("was stop");
	if (packet.subreq.object == SUBSYSTEM) {
	    send_src_reply(packet, SRC_OK, "", sizeof(struct srcrep));
	    die(0);
	} else {
	    send_src_reply(packet, SRC_SUBMSG, 
			   "ERROR: syslogd does not support subsystem STOP requests",
			   sizeof(struct srcrep));
	}
	break;
    case STATUS:
	send_src_reply(packet, SRC_SUBMSG, 
		       "ERROR: syslogd does not support STATUS requests",
		       sizeof(struct srcrep));
	break;
    case TRACE:
	send_src_reply(packet, SRC_SUBMSG, 
		       "ERROR: syslogd does not support TRACE requests",
		       sizeof(struct srcrep));
	break;
    case REFRESH:
	if (packet.subreq.object == SUBSYSTEM) {
	    init(0);
	    send_src_reply(packet, SRC_OK, "", sizeof(struct srcrep));
	} else {
	    send_src_reply(packet, SRC_SUBMSG, 
			   "ERROR: syslogd does not support subsystem REFRESH requests",
			   sizeof(struct srcrep));
	}
	break;
    }
}

static void send_src_reply(orig_packet, rtncode, packet,len)
    struct srcreq orig_packet;
    int rtncode;
    char *packet;
    int len;
{
    struct srcrep reply;
    ushort cont = END;
    
    reply.svrreply.rtncode = rtncode;
    strcpy(reply.svrreply.objname, "syslogd");
    reply.svrreply.objtext[0] = '\0';
    reply.svrreply.objname[0] = '\0';
    (void) memcpy(reply.svrreply.rtnmsg, packet, len);
    
    srcsrpy(srcrrqs(&orig_packet), (char *)&reply, len, cont);
}

#endif /* HAVE_SRC */
