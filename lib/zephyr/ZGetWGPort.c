/* This file is part of the Project Athena Zephyr Notification System.
 * It contains source for the ZGetWGPort function.
 *
 *	Created by:	Robert French
 *
 *	$Source$
 *	$Author$
 *
 *	Copyright (c) 1987 by the Massachusetts Institute of Technology.
 *	For copying and distribution information, see the file
 *	"mit-copyright.h". 
 */
/* $Header$ */

#ifndef lint
static char rcsid_ZGetWGPort_c[] = "$Header$";
#endif lint

#include <zephyr/mit-copyright.h>

#include <zephyr/zephyr_internal.h>

extern char *getenv();
extern uid_t getuid();

u_short ZGetWGPort()
{
    char *envptr, name[128];
    FILE *fp;
    u_short wgport;
	
    envptr = getenv("WGFILE");
    if (!envptr) {
	(void) sprintf(name, "/tmp/wg.%d", getuid());
	envptr = name;
    } 
    if (!(fp = fopen(envptr, "r")))
	return (-1);

    /* if fscanf fails, return -1 via wgport */
    if (fscanf(fp, "%d", &wgport) != 1)
	    wgport = -1;

    (void) fclose(fp);

    return (wgport);
}
