/* This file is part of the Project Athena Zephyr Notification System.
 * It contains functions for the Zephyr server class manager subsystem.
 *
 *	Created by:	John T. Kohl
 *
 *	$Source$
 *	$Author$
 *
 *	Copyright (c) 1987 by the Massachusetts Institute of Technology.
 *	For copying and distribution information, see the file
 *	"mit-copyright.h". 
 */

#include <zephyr/mit-copyright.h>

#ifndef lint
#ifndef SABER
static char rcsid_class_s_c[] = "$Header$";
#endif SABER
#endif lint

#include "zserver.h"			/* includes zephyr/zephyr.h */

/*
 * Class manager subsystem.
 *
 *
 * External functions are:
 *
 * Code_t class_register(client, class)
 *
 * Code_t class_deregister(client, class)
 *
 * ZClientList_t *class_lookup(class)
 *	ZClient_t *client;
 *	char *class;
 *
 * ZAcl_t *class_get_acl(notice)
 *
 * int class_is_admin(notice)
 *
 * int class_is_ulogin(notice)
 *
 * int class_is_ulocate(notice)
 *
 * int class_is_control(notice)
 *	ZNotice_t *notice;
 *
 * Code_t class_restrict(class, acl)
 *	char *class;
 *	ZAcl_t *acl;
 *
 * Code_t class_setup_restricted(class, acl)
 *	char *class;
 *	ZAcl_t *acl;
 */

/*
 * The data structure used for the class manager is an array of hash buckets
 * each containing a pointer to a doubly linked circular list (in the style
 * of insque/remque).  Each element of this list contains a class name (which
 * hashes into the bucket associated with this list) and a doubly linked list
 * of clients which are interested in this class.
 * The data pointed to by these clients is owned by other modules.  Care
 * must be taken by the caller not to register a free()'d client
 * structure.
 *
 * If any hash bucket is empty, the pointer is null.
 *
 * The first element in the hash bucket is a special header unused for
 * storing classes, and is used for finding the end of the list.
 *
 * If any list of interested clients is empty, the class name is garbage
 * collected, unless the class has been registered as restricted.
 */

/* Private variables */ 
#define	EMPTY_CLASS	2000

#define	HASHSIZE	511
#define	HASHMUL		243

static ZClass_t *class_bucket[511];	/* the hash table of pointers */

static Code_t remove_client(), insert_client();
static void free_class();
static ZClientList_t *client_alloc();
static ZClass_t *class_alloc();
static unsigned int hash();

/* public routines */

/* register the client as interested in class */

Code_t
class_register(client, class)
ZClient_t *client;
char *class;
{
	register ZClass_t *ptr, *ptr2;
	int hashval = hash(class);

	if (!(ptr = class_bucket[hashval])) {
		/* not registered */
		
		if (!(ptr = class_alloc(class)))
			return(ENOMEM);

		/* allocate the head of the bucket */
		if (!(ptr2 = (ZClass_t *) xmalloc(sizeof(ZClass_t))))
			return(ENOMEM);

		ptr2->q_forw = ptr;
		ptr2->q_back = ptr;
		ptr->q_forw = ptr2;
		ptr->q_back = ptr2;

		class_bucket[hashval] = ptr2;
		return(insert_client(ptr, client));

	} else {
		for (ptr2 = ptr->q_forw; ptr2 != ptr; ptr2 = ptr2->q_forw)
			/* walk down the list, looking for a match */
			if (!strcmp(ptr2->zct_classname, class))
				return(insert_client(ptr2, client));

		/* fell off the end, no match */
		if (!(ptr2 = class_alloc(class)))
			return(ENOMEM);
		xinsque(ptr2, ptr);	/* insert new class into hash bucket */
		return(insert_client(ptr2, client));
	}
}

/* dissociate client from the class, garbage collecting if appropriate */

Code_t
class_deregister(client, class)
ZClient_t *client;
char *class;
{
	register ZClass_t *ptr, *ptr2;
	int retval = -1;
	int hashval = hash(class);
 
	zdbug((LOG_DEBUG, "class_dereg"));
	if (!(ptr = class_bucket[hashval]))
		/* no such class to deregister */
		return(ZSRV_BADASSOC);
	
	for (ptr2 = ptr->q_forw; ptr2 != ptr; ptr2 = ptr2->q_forw) {
		/* walk down the list, looking for a match */
		if (!strcmp(ptr2->zct_classname, class)) {
			if ((retval = remove_client(ptr2, client)) == EMPTY_CLASS) {
				zdbug((LOG_DEBUG,"empty class"));
				/* Don't free up restricted classes. */
				if (ptr2->zct_acl)
					return(ZERR_NONE);
				else {
					xremque(ptr2);
					free_class(ptr2);
					return(ZERR_NONE);
				}
			}
			/* if not EMPTY_CLASS, it's either ZSRV_BADASSOC
			   (not found) or ZERR_NONE (found and removed),
			   so break */
			break;
		}
	}

	/* fell off: either client not found or client found
	   and removed, retval contains the result */
	return(retval);
}

/* return a linked list of what clients are interested in this class */

ZClientList_t *
class_lookup(class)
char *class;
{
	register ZClass_t *ptr, *ptr2;
	int hashval = hash(class);

	if (!(ptr = class_bucket[hashval]))
		return(NULLZCLT);			/* no such class */
	else { /* go search the list for the class */
		for (ptr2 = ptr->q_forw; ptr2 != ptr; ptr2 = ptr2->q_forw) {
			/* walk down the list, looking for a match */
			if (!strcmp(ptr2->zct_classname, class))
				return(ptr2->zct_clientlist);
		}
		/* fell off the end */
		return(NULLZCLT);
	}
}

/*
 * These routines return 0 or one depending on whether the indicated
 * notice is of the type named by the routine name.
 */

int
class_is_control(notice)
ZNotice_t *notice;
{
	return(!strcmp(notice->z_class, ZEPHYR_CTL_CLASS));
}

int
class_is_admin(notice)
ZNotice_t *notice;
{
	return(!strcmp(notice->z_class, ZEPHYR_ADMIN_CLASS));
}

int
class_is_hm(notice)
ZNotice_t *notice;
{
	return(!strcmp(notice->z_class, HM_CTL_CLASS));
}

int
class_is_ulogin(notice)
ZNotice_t *notice;
{
	return(!strcmp(notice->z_class, LOGIN_CLASS));
}

int
class_is_ulocate(notice)
ZNotice_t *notice;
{
	return(!strcmp(notice->z_class, LOCATE_CLASS));
}

/*
 * return the acl structure associated with class, or NULLZACLT if there is
 * no such acl struct
 */

ZAcl_t *
class_get_acl(class)
char *class;
{
	register ZClass_t *ptr, *ptr2;
	int hashval = hash(class);

	if (!(ptr = class_bucket[hashval]))
		return(NULLZACLT);

	/* walk down the list, looking for a match */
	for (ptr2 = ptr->q_forw; ptr2 != ptr; ptr2 = ptr2->q_forw)
		if (!strcmp(ptr2->zct_classname, class))
			return(ptr2->zct_acl);

	/* fell off the end, no match ==> not restricted */
	return(NULLZACLT);
}

/*
 * restrict class by associating it with the acl structure acl.
 * return ZERR_NONE if no error, or ZSRV_NOCLASS if there is no such
 * class, or ZSRV_CLASSRESTRICTED if it is already restricted.
 */

Code_t
class_restrict(class, acl)
char *class;
ZAcl_t *acl;
{
	register ZClass_t *ptr, *ptr2;
	int hashval = hash(class);

	if (!(ptr = class_bucket[hashval]))
		return(ZSRV_NOCLASS);
	for (ptr2 = ptr->q_forw; ptr2 != ptr; ptr2 = ptr2->q_forw)
		/* walk down the list, looking for a match */
		if (!strcmp(ptr2->zct_classname, class)) {
			if (ptr2->zct_acl)
				return(ZSRV_CLASSRESTRICTED);
			ptr2->zct_acl = acl;
			return(ZERR_NONE);
		}

	/* fell off the end, no match */
	return(ZSRV_NOCLASS);
}

/*
 * restrict class by registering it and  associating it with the acl
 * structure acl.  return ZERR_NONE if no error, or ZSRV_CLASSXISTS
 * if the class is already registered, or ENOMEM in case of malloc failure.
 */
  
Code_t
class_setup_restricted(class, acl)
char *class;
ZAcl_t *acl;
{
	register ZClass_t *ptr, *ptr2;
	int hashval = hash(class);

	if (!(ptr = class_bucket[hashval])) {
		/* not registered */
		
		if (!(ptr = class_alloc(class)))
			return(ENOMEM);

		ptr->zct_acl = acl;

		/* allocate the head of the bucket */
		if (!(ptr2 = (ZClass_t *) xmalloc(sizeof(ZClass_t))))
			return(ENOMEM);

		ptr2->q_forw = ptr;
		ptr2->q_back = ptr;
		ptr->q_forw = ptr2;
		ptr->q_back = ptr2;

		class_bucket[hashval] = ptr2;
		return(ZERR_NONE);
	} else
		return(ZSRV_CLASSXISTS);
}

/* private routines */

/* the hash function */

static unsigned int
hash(string)
char *string;
{
	register unsigned int hval = 0;
	register unsigned char *cp = (unsigned char *) string;

	while (*cp)
		hval = (hval + (*cp++) * HASHMUL) % HASHSIZE;
	return(hval);
}

/* allocate space for a class structure */

static ZClass_t *
class_alloc(class)
char *class;
{
	register ZClass_t *ptr;
	ZClientList_t *clist;

	if (!(ptr = (ZClass_t *) xmalloc(sizeof(ZClass_t))))
		return(NULLZCT);

	ptr->q_forw = ptr->q_back = ptr;

	ptr->zct_classname = strsave(class);
	if (!(clist = (ZClientList_t *) xmalloc(sizeof(ZClientList_t)))) {
		xfree(ptr);
		return(NULLZCT);
	}
	clist->q_forw = clist->q_back = clist;
	ptr->zct_clientlist = clist;
	ptr->zct_acl = NULLZACLT;

	return(ptr);
}

/* free up the space used by this class structure */

static void
free_class(ptr)
ZClass_t *ptr;
{
	xfree(ptr->zct_classname);
	xfree(ptr->zct_clientlist);
	xfree(ptr);
}

/* allocate space for a client entry */

static ZClientList_t *
client_alloc(client)
ZClient_t *client;
{
	register ZClientList_t *ptr;
	if (!(ptr = (ZClientList_t *) xmalloc(sizeof(ZClientList_t))))
		return(NULLZCLT);

	ptr->zclt_client = client;
	return(ptr);
}

/* insert a client into the list associated with the class *ptr */

static Code_t
insert_client(ptr, client)
ZClass_t *ptr;
ZClient_t *client;
{
	register ZClientList_t *listp, *clist;

	for (clist = ptr->zct_clientlist->q_forw;
	     clist != ptr->zct_clientlist;
	     clist = clist->q_forw)
		/* don't duplicate */
		if (clist->zclt_client == client)
			return(ZERR_NONE);

	if (!(listp = client_alloc(client)))
		return(ENOMEM);

	xinsque(listp, ptr->zct_clientlist);
	return(ZERR_NONE);
}

/* 
 * remove the client from the list associated with class *ptr, garbage
 * collecting if appropriate
 */

static Code_t remove_client(ptr, client)
ZClass_t *ptr;
ZClient_t *client;
{
	register ZClientList_t *listp = ptr->zct_clientlist; 
	register ZClientList_t *listp2;

	if (!listp)
		return(ZSRV_BADASSOC);
	for (listp2 = listp->q_forw;
	     listp2 != listp;
	     listp2 = listp2->q_forw)
		/* walk down list, looking for him */
		if (listp2->zclt_client == client) {
			xremque(listp2);
			xfree(listp2);
			if (listp->q_forw == listp)
				return(EMPTY_CLASS);
			else
				return(ZERR_NONE);
		}
	return(ZSRV_BADASSOC);
}
