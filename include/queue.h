/*
 * Message queues definitions
 *
 * Copyright 1993 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __WINE_QUEUE_H
#define __WINE_QUEUE_H

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "thread.h"

struct received_message_info;
struct hook16_queue_info;

/* Message queue */
typedef struct tagMESSAGEQUEUE
{
  HQUEUE16  self;                   /* Handle to self (was: reserved) */
  HANDLE    server_queue;           /* Handle to server-side queue */
  DWORD     recursion_count;        /* Counter to prevent infinite SendMessage recursion */
  HHOOK     hook;                   /* Current hook */
  struct received_message_info *receive_info; /* Info about message being currently received */
  struct hook16_queue_info *hook16_info;      /* Opaque pointer for 16-bit hook support */

  DWORD     GetMessageTimeVal;      /* Value for GetMessageTime */
  DWORD     GetMessagePosVal;       /* Value for GetMessagePos */
  DWORD     GetMessageExtraInfoVal; /* Value for GetMessageExtraInfo */

  HCURSOR   cursor;                 /* current cursor */
  INT       cursor_count;           /* cursor show count */
} MESSAGEQUEUE;


#define MAX_SENDMSG_RECURSION  64

/* Message queue management methods */
extern MESSAGEQUEUE *QUEUE_Current(void);
extern void QUEUE_DeleteMsgQueue(void);

#endif  /* __WINE_QUEUE_H */
