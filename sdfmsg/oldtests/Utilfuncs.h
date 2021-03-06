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

/*
 * File:   Utilfuncs.h
 * Author: HT
 *
 * Created on February 25, 2008, 3:45 PM
 *
 * (c) Copyright 2008, Schooner Information Technology, Inc.
 * http://www.schoonerinfotech.com/
 *
 * $Id: 
 */

#ifndef UTILFUNCS_H_
#define UTILFUNCS_H_

#include "sdfmsg/sdf_msg_types.h"
#include "sdfmsg/sdf_msg.h"
#include "sdfmsg/sdf_fth_mbx.h"
#include "platform/types.h"
#include "fcnl_test.h"


struct sdf_queue_pair * local_create_myqpairs(service_t protocol,
        uint32_t myid, uint32_t pnode);

struct sdf_queue_pair * local_create_myqpairs_with_pthread(service_t protocol,
        uint32_t myid, uint32_t pnode);

void TestTrace(int tracelevel, int selflevel, const char *format, ...);

int sdf_msg_say_bye(vnode_t dest_node, service_t dest_service,
        vnode_t src_node, service_t src_service, msg_type_t msg_type,
        sdf_fth_mbx_t *ar_mbx, uint64_t length);

vnode_t local_get_pnode(int localrank, int localpn, uint32_t numprocs);

void local_setmsg_payload(sdf_msg_t *msg, int size, uint32_t nodeid, int num);

void local_setmsg_mc_payload(sdf_msg_t *msg, int size, uint32_t nodeid, int index, int maxcount, uint64_t ptl);

void local_printmsg_payload(sdf_msg_t *msg, int size, uint32_t nodeid); 

int process_ret(int ret_err, int prt, int type, vnode_t myid);

uint64_t get_timestamp();

uint64_t get_passtime(struct timespec* start, struct timespec* end);

/* setup the defaults for the test config parameters */
int msgtst_setconfig(struct plat_opts_config_mpilogme *config);
/* helper funcs for setting up the configuration flags */
int msgtst_setpreflags(struct plat_opts_config_mpilogme *config);
int msgtst_setpostflags(struct plat_opts_config_mpilogme *config);

#endif


