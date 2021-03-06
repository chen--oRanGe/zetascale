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
 * File:   init_sdf.c
 * Author: DO
 *
 * Created on January 15, 2008, 10:04 AM
 *
 * Copyright Schooner Information Technology, Inc.
 * http://www.schoonerinfotech.com/
 *
 * $Id: init_sdf.c 10527 2009-12-12 01:55:08Z drew $
 *
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include "platform/stdlib.h"
#include "platform/logging.h"
#include "sdfmsg/sdf_msg_types.h"
#include "sdfmsg/sdf_msg_sync.h"
#include "common/sdftypes.h"
#include "open_container_map.h"
#include "name_service.h"
#include "init_sdf.h"
#include "cmc.h"
#include "name_service.h"
#include "shard_compute.h"
#include "ssd/fifo/container_meta_blob.h"
#include "utils/properties.h"
#include "api/zs.h"
#include "ssd/fifo/mcd_osd_internal.h"
#include "api/sdf_internal.h"

#include "private.h"

#define LOG_ID PLAT_LOG_ID_INITIAL
#define LOG_CAT PLAT_LOG_CAT_SDF_NAMING
#define LOG_DBG PLAT_LOG_LEVEL_DEBUG
#define LOG_INFO PLAT_LOG_LEVEL_INFO
#define LOG_ERR PLAT_LOG_LEVEL_ERROR
#define LOG_WARN PLAT_LOG_LEVEL_WARN
#define LOG_FATAL PLAT_LOG_LEVEL_FATAL


// Globals
SDF_cmc_t *theCMC = NULL;        // Container metadata container

struct SDF_shared_state sdf_shared_state;

extern SDF_status_t
zs_open_virtual_support_containers(SDF_internal_ctxt_t *pai, int flags);

#ifdef notdef
int 
(*init_container_meta_blob_put)( uint64_t shard_id, char * data, int len ) = NULL;

int 
(*init_container_meta_blob_get)( char * blobs[], int num_slots ) = NULL;
#endif

void 
init_sdf_initialize_config(struct SDF_config *config,
                           SDF_internal_ctxt_t *pai,
                           int num_objs,
			   int system_recovery,
                           unsigned rank,
#ifdef MULTIPLE_FLASH_DEV_ENABLED
                           struct flashDev *flash_dev[],
#else
                           struct flashDev *flash_dev,
#endif                           
                           uint32_t flash_dev_count,
                           uint32_t shard_count,
			   			   struct sdf_replicator *replicator) 
{
    plat_assert(pai);
    plat_assert(flash_dev);

    config->pai = pai;
    config->num_objs = num_objs;
    config->my_node = rank;
    config->flash_dev = flash_dev;
    config->flash_dev_count = flash_dev_count;
    config->replicator = replicator;
    config->shard_count = shard_count;
    config->system_recovery = system_recovery;

    /* Always use flash message */
    config->flash_msg = 0;
    config->replication_service = SDF_REPLICATION;
    config->flash_service = SDF_FLSH;
    config->response_service =  SDF_RESPONSES;

    /* 
     * Initialize the cguid counter as if we were reformatting.
     * We will recover it when SDF is up if we are in recovery mode.
     */
    config->cguid_counter = LAST_INTERNAL_CGUID;
}

// ============================================================
SDF_status_t
init_sdf_reset(SDF_internal_ctxt_t *pai)
{
    SDF_status_t status = SDF_FAILURE;

    status = cmc_destroy(pai, theCMC);
    cmap_reset();

    return (status);
}

SDF_status_t
init_sdf_initialize(const struct SDF_config *config, int restart) 
{
    SDF_status_t 			 status 	= SDF_FAILURE;
    char 					*cmc 		= CMC_PATH;
    int 					 log_level 	= LOG_ERR;

    plat_log_msg(21498, LOG_CAT, LOG_DBG, "Node: %d", init_get_my_node_id());

    memset(&sdf_shared_state, 0, sizeof (sdf_shared_state));
    sdf_shared_state.config = *config;

    /* Cause the slaves to wait until the master initializes the CMC */
    if (!restart && config->my_node != CMC_HOME)
        sdf_msg_sync();

    if (0 == cmap_init() && cmc_create(config->pai, cmc) != NULL) {
        plat_log_msg(21604, PLAT_LOG_CAT_SDF_SHARED, PLAT_LOG_LEVEL_DEBUG,
                     "CMC create succeeded");
        status = SDF_SUCCESS;
		log_level = LOG_DBG;
    } else {
        plat_log_msg(21605, PLAT_LOG_CAT_SDF_SHARED, PLAT_LOG_LEVEL_ERROR,
                     "CMC create failed");
    }

    /* The master now allows the slaves to continue */
    if (!restart && config->my_node == CMC_HOME)
        sdf_msg_sync();

    plat_log_msg(20819, LOG_CAT, log_level, "%s", SDF_Status_Strings[status]);

    return (status);
}

void
init_cmc(SDF_container_meta_t *meta) {
    memcpy(&theCMC->meta, meta, sizeof(SDF_container_meta_t));
}

uint32_t
init_get_my_node_id() {
    return (sdf_shared_state.config.my_node);
}

void
init_set_cguid_counter(SDF_cguid_t cguid_counter) {
    sdf_shared_state.config.cguid_counter = cguid_counter;
}
