/* This file is part of the Project Athena Zephyr Notification System.
 * It contains declarations for many standard UNIX library functions,
 * and macros for aiding in interfacing to them.
 *
 * Created by Ken Raeburn.
 *
 * $Source$
 * $Author$
 * $Id$
 *
 * Copyright (c) 1990 by the Massachusetts Institute of Technology.
 * For copying and distribution information, see the file
 * "mit-copyright.h".
 */

#include <zephyr/mit-copyright.h>

#ifndef ZSERVER_UNIX_H__

extern "C" {
    /* found in libc.a */
#ifndef __GNUG__
    void *malloc(unsigned), *realloc(void *, unsigned), free (void *);
#endif
    long random(void);

    /*
     * Queue-handling functions.  This structure is basically a dummy;
     * as long as the start of another structure looks like this,
     * we're okay.
     */
    struct qelem {
	struct qelem *q_forw;
	struct qelem *q_back;
	char *q_data;
    };
    void insque (qelem*, qelem*);
    void remque (qelem *);
#ifdef __GNUG__
#if defined (ultrix)
    void openlog (char *, int);
#undef LOG_DEBUG
#define LOG_DEBUG LOG_ERR
#else
    void openlog (char *, int, int); /* ??? */
#endif
#endif /* G++? */
    void syslog (int, const char *, ...);
    int setsockopt (int, int, int, const char *, int);
    extern int strcasecmp (const char*, const char*);
#ifdef __GNUG__
    extern void setservent (int);
    extern void endservent (void);
#endif
    extern void moncontrol (int);

    /* From the Error table library */
    char *error_message(long);

    /* Kerberos */
    extern int krb_get_lrealm (...);
    extern int dest_tkt (...);
    extern int krb_get_svc_in_tkt (...);
    extern int krb_rd_req (...);
    extern int krb_mk_req (...);
    extern int krb_get_cred (...);
    extern int des_quad_cksum (...);

    /* hacked acl code */
    extern void acl_cache_reset (void);
}

#ifdef vax
#define HAVE_ALLOCA
#endif

#if defined (__GNUC__) || defined (__GNUG__)

/* GCC/G++ has a built-in function for allocating automatic storage.  */
#define LOCAL_ALLOC(X)	__builtin_alloca(X)
#define LOCAL_FREE(X)

#else /* not gcc or g++ */

#if defined (ibm032)
/*
 * Unfortunately, there's no way to get cfront to access _Alloca.  So
 * we compile with -ma and call alloca.  Sigh.
 */
#define LOCAL_ALLOC(X)	alloca(X)
#define LOCAL_FREE(X)
extern "C" void * alloca (unsigned int);

#else /* none of above */
#ifdef HAVE_ALLOCA
#define LOCAL_ALLOC(X)	alloca(X)
#define LOCAL_FREE(X)
#endif
#endif
#endif

#ifndef LOCAL_ALLOC
#define LOCAL_ALLOC(X)	malloc(X)
#define LOCAL_FREE(X)	free(X)
#endif

#define	xfree(foo)	free((caddr_t) (foo))
#define	xinsque(a,b)	insque((struct qelem *)(a), (struct qelem *)(b))
#define xremque(a)	remque((struct qelem *)(a))
#define	xmalloc(a)	malloc((unsigned)(a))

#define ZSERVER_UNIX_H__
#endif