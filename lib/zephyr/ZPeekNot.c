/* This file is part of the Project Athena Zephyr Notification System.
 * It contains source for ZPeekNotice function.
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

#include <zephyr/mit-copyright.h>

#include <zephyr/zephyr_internal.h>

Code_t ZPeekNotice(buffer,buffer_len,notice,auth)
	ZPacket_t	buffer;
	int		buffer_len;
	ZNotice_t	*notice;
	int		*auth;
{
	int len;
	Code_t retval;

	if ((retval = ZPeekPacket(buffer,buffer_len,&len)) !=
	    ZERR_NONE)
		return (retval);

	return (ZParseNotice(buffer,len,notice,auth));
}
