//----------------------------------------------------------------------------
// ZetaScale
// Copyright (c) 2016, SanDisk Corp. and/or all its affiliates.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published by the Free
// Software Foundation;
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License v2.1 for more details.
//
// A copy of the GNU Lesser General Public License v2.1 is provided with this package and
// can also be found at: http://opensource.org/licenses/LGPL-2.1
// You should have received a copy of the GNU Lesser General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 59 Temple
// Place, Suite 330, Boston, MA 02111-1307 USA.
//----------------------------------------------------------------------------

#ifndef PLATFORM_THREAD_H
#define PLATFORM_THREAD_H 1

/*
 * File:   $HeadURL: svn://svn.schoonerinfotech.net/schooner-trunk/trunk/sdf/platform/thread.h $
 * Author: drew
 *
 * Created on January 25, 2008
 *
 * (c) Copyright 2008, Schooner Information Technology, Inc.
 * http://www.schoonerinfotech.com/
 *
 * $Id: thread.h 902 2008-04-03 23:33:02Z drew $
 */

/*
 * Thin wrappers for unix functions to accomodate interception for
 * 1) Blocking behavior in user-scheduled threads
 * 2) The simulated cluster environment
 */

#include <sys/cdefs.h>

/*
 * Hide functions which have been (or should be) replaced, producing 
 * compile time warnings which become errors with -Werror.  Where
 * multiple 'C' source in platform must use a system function 
 * multple defined() clauses can be used.
 */
#ifndef PLATFORM_THREAD_C
#define pthread_create phread_create_use_plat
#define pthread_exit phread_exit_use_plat
#define pthread_join phread_join_use_plat
#endif

#include <pthread.h>

/*
 * Undef to avoid collisions with with struct field names and local 
 * functions.
 */
#ifndef PLATFORM_THREAD_C
#undef pthread_create
#undef pthread_exit
#undef pthread_join
#endif


/** 
 * Opaque kernel thread type.  User threads may have a different constructor
 * so that they can be parented off a different scheduler than the thread
 * creating them.
 */
typedef pthread_t plat_kthread_t;

#ifndef PLATFORM_THREAD_C
#define pthread_t pthread_t_use_plat
#endif

__BEGIN_DECLS

int plat_kthread_create(plat_kthread_t *thread,
                        void * (*start_routine)(void *), void * arg);

int plat_kthread_join(plat_kthread_t thread, void **ret);

void plat_kthread_exit(void *ret) __attribute__((noreturn));

__END_DECLS

#endif /* ndef PLATFORM_THREAD_H */
