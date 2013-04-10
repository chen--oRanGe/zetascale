/*
 * FDF License Daemon
 *
 * Author: Niranjan Neelakanta
 *
 * Copyright (c) 2013 SanDisk Corporation.  All rights reserved.
 */

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "sdf.h"
#include "sdf_internal.h"
#include "fdf.h"
#include "utils/properties.h"
#include "protocol/protocol_utils.h"
#include "protocol/protocol_common.h"
#include "protocol/action/action_thread.h"
#include "protocol/action/async_puts.h"
#include "protocol/home/home_flash.h"
#include "protocol/action/async_puts.h"
#include "protocol/replication/copy_replicator.h"
#include "protocol/replication/replicator.h"
#include "protocol/replication/replicator_adapter.h"
#include "ssd/fifo/mcd_ipf.h"
#include "ssd/fifo/mcd_osd.h"
#include "ssd/fifo/mcd_bak.h"
#include "shared/init_sdf.h"
#include "shared/private.h"
#include "shared/open_container_mgr.h"
#include "shared/container_meta.h"
#include "shared/name_service.h"
#include "shared/shard_compute.h"
#include "shared/internal_blk_obj_api.h"
#include "agent/agent_common.h"
#include "agent/agent_helper.h"
#include "fdf.h"
#include "fdf_internal.h"

#include "license/interface.h"

#define LOG_ID PLAT_LOG_ID_INITIAL
#define LOG_CAT PLAT_LOG_CAT_SDF_NAMING
#define LOG_DBG PLAT_LOG_LEVEL_DEBUG
#define LOG_TRACE PLAT_LOG_LEVEL_TRACE
#define LOG_INFO PLAT_LOG_LEVEL_INFO
#define LOG_ERR PLAT_LOG_LEVEL_ERROR
#define LOG_WARN PLAT_LOG_LEVEL_WARN
#define LOG_FATAL PLAT_LOG_LEVEL_FATAL

#define	MINUTE	(60)
#define HOUR	(3600)
#define DAY	(86400)

#define FDF_INVAL_GPRD	(7 * DAY)
#define FDF_EXP_GPRD	(30 * DAY)

/*
 * Period for checking the license, once in an hour.
 */
double 		fdf_chk_prd = HOUR;

char		*ld_prod;	/* Product Name */
double		ld_frm_diff;	/* Current time - Start of license */
double		ld_to_diff;	/* End of license - Current time */
double		ld_vtime;	/* Time stamp at which we found valid license */
double		ld_cktime;	/* Time stamp at which we make last check */
enum lic_state	ld_state;	/* Current license state */
char		ld_type;	/* Type of license found */
bool		ld_valid = true;	/* Is license valid or not? */
bool		licd_init = false;	/* Is license state initialized */
/*
 * This CV serves the purpose of blocking wait for a
 * thread awaiting completion of license checking operation.
 */
pthread_cond_t	licd_cv = PTHREAD_COND_INITIALIZER;
pthread_mutex_t	licd_mutex = PTHREAD_MUTEX_INITIALIZER;;
static struct FDF_state *licd_fdf_state = NULL;

static void licd_handler_thread(uint64_t);
void update_lic_info(lic_data_t *);
void print_time_left(double, double);
void print_validity_left(lic_data_t *, lic_type);
void adjust_chk_prd(double);
void free_details(lic_data_t *);


/*
 * licd_start - Start the license daemon.
 * 
 * This routine just spans a pthread which does regular check  of license.
 * The license is read from the path passed in as argument (lic_path).
 */
bool
licd_start(const char *lic_path, struct FDF_state *fdf_state)
{
	plat_assert(lic_path);
	plat_assert(fdf_state);
	if (!lic_path) {
		plat_log_msg(160148, LOG_CAT,
				LOG_ERR, "License path not specified");
		goto out;
	}

	licd_fdf_state = fdf_state;

	if( fdf_state == NULL ) {
		plat_log_msg(160072,LOG_CAT, LOG_ERR, "Invalid FDF state");
		goto out;
	}
	/* Start the thread */
	fthResume( fthSpawn( &licd_handler_thread, MCD_FTH_STACKSIZE ),
				(uint64_t)lic_path);
	return true;
out:
	plat_log_msg(160149, LOG_CAT, LOG_WARN, "Starting Licensing daemon failed.");
	ld_valid = false;
	pthread_mutex_lock(&licd_mutex);
	licd_init = true;
	pthread_cond_broadcast(&licd_cv);
	pthread_mutex_unlock(&licd_mutex);
	return false;
}

/*
 * This is the main license handler thread. Following is the control flow:
 *
 * 1. Get the license details.
 * 2. Update the in house license information.
 * 3. Wake up any thread waiting for license to get initialized.
 * 4. Sleep for the period (fdf_chk_prd).
 * 5. Goto (1).
 */
static void
licd_handler_thread(uint64_t arg)
{
	char		*lic_path = (char *)arg;
	struct timespec abstime;
	lic_data_t	data;

	bzero(&data, sizeof(lic_data_t));

	plat_log_msg(160162, LOG_INFO, LOG_INFO,
			"Starting Licensing Daemon (license path: %s)...", lic_path);
	memset(&abstime, 0, sizeof(struct timespec));
	
	clock_gettime(CLOCK_REALTIME, &abstime);
	ld_vtime = abstime.tv_sec;
	while(1) {
		/*
		 * Get license details and update in-house info.
		 * This will get info as array of pointers.
		 */
		get_license_details(lic_path, &data);
		update_lic_info(&data);

		if (licd_init == false) {
			// If running for first time, wake up waiting threads.
			pthread_mutex_lock(&licd_mutex);
			licd_init = true;
			plat_log_msg(160151, LOG_CAT, LOG_INFO,
					"License daemon initialized\n");
			pthread_cond_broadcast(&licd_cv);
			pthread_mutex_unlock(&licd_mutex);
		}

		free_details(&data);
		// Sleep for fdf_chk_prd time
		pthread_mutex_lock(&licd_mutex);
		clock_gettime(CLOCK_REALTIME, &abstime);
		abstime.tv_sec += fdf_chk_prd;
		pthread_cond_timedwait(&licd_cv, &licd_mutex, &abstime);
		pthread_mutex_unlock(&licd_mutex);
	}

}

/*
 * Wait for license daemon to start. Threads will sleep till the daemon
 * reads license file and updates its in-house information.
 * Threads sleep only if license information is not initialized yet by
 * the daemon.
 */
void
wait_for_licd_start()
{
	pthread_mutex_lock(&licd_mutex);

	if (licd_init == false) {
		pthread_cond_wait(&licd_cv, &licd_mutex);
	}

	pthread_mutex_unlock(&licd_mutex);
}

#define getptr(__data, __indx) 		((__data)->fld_data[(__indx)])
#define getas(__ptr, __type) 		(*(__type *)(__ptr))
#define getasstr(__ptr) 		((char *)(__ptr))

/*
 * This routine will update the in-house information using the data
 * read from license file.
 */
void
update_lic_info(lic_data_t	*data)
{
	void		*p, *p1, *p2;
	struct timespec abstime;
	double		exptime;
	char		*prod;
	char		*maj = NULL, *min = NULL;
	lic_type	type = 0;

	clock_gettime(CLOCK_REALTIME, &abstime);

	/*
	 * If we couldn't read license details due to internall error, say
	 * insufficient memory, let us not fail or decide license status.
	 * Instead, read the license details again as soon as possible.
	 */
	if (data->fld_state == LS_INTERNAL_ERR) {
		adjust_chk_prd(0);
		return;
	}

	/*
	 * Only if license is valid or expired, let us check whether the license
	 * is for FDF. Else, let us mark the license as invalid and fail the
	 * application.
	 */
	ld_state = data->fld_state;
	if ((ld_state == LS_VALID) || (ld_state == LS_EXPIRED)) {
		// We always expect license type and product to be set.
		p = getptr(data, LDI_LIC_TYPE);
		plat_assert(p);
		if (p) {
			type = getas(p, lic_type);
		} else {
			ld_state = LS_INVALID;
			goto print;
		}

		p = getptr(data, LDI_PROD_NAME);
		plat_assert(p);
		if (p) {
			prod = getasstr(p);
			p1 = getptr(data, LDI_PROD_MAJ);
			if (p1) {
				maj = getasstr(p1);
			}
			p2 = getptr(data, LDI_PROD_MIN);
			if (p2) {
				min = getasstr(p2);
			}
			/*
			 * If product matches, check whether the version
			 * matches. If both, then this is a valid license.
			 */
			if (!strcmp(prod, FDF_PRODUCT_NAME)) {
#ifdef FDF_REVISION
				if (p1) {
					char ver[32] = {0};
					sprintf(ver, "%s.", maj);
					if (!strstr(FDF_REVISION, ver)){
						ld_state = LS_VER_MISMATCH;
					}
				} else {
					ld_state = LS_PROD_MISMATCH;
				}
#endif
			} else {
				ld_state = LS_PROD_MISMATCH;
			}
		} else {
			ld_state = LS_INVALID;
		}

		//If license is valid, update ld_vtime with current time.
		if (ld_state == LS_VALID) {
			ld_vtime = abstime.tv_sec;
		}
	}

	/*
	 * Print any info/warning messages based on status of license.
	 */
print:
	if (ld_state == LS_VALID) {
		// Print any warning, if we are near to expiry.
		print_validity_left(data, type);
	} else if (ld_state == LS_EXPIRED) {
		// If license has expired, then it has to be periodic.
		plat_assert(GET_PER_TYPE(type) != LPT_PERPETUAL);
		plat_log_msg(160155, LOG_CAT, LOG_WARN, 
				"License has expired. Renew the license.");
		p = getptr(data, LDI_DIFF_TO);
		plat_assert(p);
		if (p) {
			exptime = getas(p, double);
			plat_assert(exptime < 0);
			exptime = -exptime;
			//Print warning and find period we need to make next check.
			print_time_left(exptime, FDF_EXP_GPRD);
		}
	} else {
		//All other cases, license is invalid.
		plat_log_msg(160156, LOG_CAT, LOG_WARN, 
			"License is invalid. %s Install valid license.",
			lic_state_msg[ld_state]);
		//Print warning and find period we need to make next check.
		print_time_left(abstime.tv_sec - ld_vtime, FDF_INVAL_GPRD);

	}
	ld_cktime = abstime.tv_sec; 
}

/*
 * This routie to be used only if license is not valid.
 * This just prints warning message and updates the time we need to make
 * next check based on time left for end of grace period.
 * 
 * INPUT
 *	time	Seconds since the license had expired/invalid.
 *	grace	Grace period in which license is considered valid 
 *		eventhough it was expired/invalid.
 */
void
print_time_left(double time, double grace)
{
	int 		days, hrs, mins, secs;
	
	plat_assert(ld_state != LS_VALID);
	if (time > grace) {
		/*
		 * If we are beyond grace period, mark license validity as
		 * false and increase the rate at which we check the 
		 * validity of license.
		 */
		plat_log_msg( 160157, LOG_CAT, LOG_WARN, 
			"License invalid beyond grace period. FDF will fail.");
		ld_valid = false;
		adjust_chk_prd(0);
	} else {
		/*
		 * Just print the warning message and update the license
		 * check period.
		 */ 
		secs = grace - time;
		mins = secs / 60;
		hrs = mins / 60; mins = mins - hrs * 60; 
		days = hrs / 24; hrs = hrs - days * 24;
		plat_log_msg( 160158, LOG_CAT, LOG_WARN, 
			"FDF will be functional for next %d days, %d "
			"hours and %d minutes only.", days, hrs, mins);
		ld_valid = true;
		adjust_chk_prd(secs);
	}
}

/*
 * This routie to be used only if license is valid.
 * This just prints warning message and updates the time we need to make
 * next check based on time left for end of validity of license.
 * 
 * INPUT
 *	time	Seconds of validity left.
 *	grace	Period in which user needs to be warned about expiry.
 */
void
print_validity_left(lic_data_t *data, lic_type type)
{
	void		*p;
	double		exptime;
	int 		days, hrs, mins, secs;

	plat_assert(ld_state == LS_VALID);
	ld_valid = true;
	plat_log_msg(160159, LOG_CAT, LOG_INFO, 
			"Valid license found (%s/%s).",
			lic_installation_type[GET_INST_TYPE(type)],
			lic_period_type[GET_PER_TYPE(type)]);

	p = getptr(data, LDI_DIFF_TO);
	if (p) {
		exptime = getas(p, double);
		plat_assert(exptime > 0);
		if (exptime > FDF_EXP_GPRD) {
			return;
		}
		secs = exptime;
		mins = secs / 60;
		hrs = mins / 60; mins = mins - hrs * 60; 
		days = hrs / 24; hrs = hrs - days * 24;
		plat_log_msg(160160, LOG_CAT, LOG_WARN, 
			"License will expire in next %d days, %d "
			"hours and %d minutes.", days, hrs, mins);
		adjust_chk_prd(secs);
	}
}

/*
 * Adjust the time stamp at which we need to make next license check.
 *
 * INPUT
 *	secs	Seconds left for license to become invalid/expire.
 *
 * Description:
 *	1. If time left is more than an hour, check once an hour.
 *	2. In last one hour, check every 15 minutes.
 *	3. In last 15 minutes, check every minute.
 */

void
adjust_chk_prd(double secs)
{
	if (secs <= 15 * MINUTE) {
		fdf_chk_prd = MINUTE;
	} else if (secs <= HOUR) {
		fdf_chk_prd = 15 * MINUTE;
	} else {
		fdf_chk_prd = HOUR;
	}
}

/*
 * Returns state of license to calling thread/APIs.
 */
bool
is_license_valid()
{
	return ld_valid;
}

void
free_details(lic_data_t *data)
{
	int	i;
	data->fld_state = LS_VALID;

	for (i = 0; i < LDI_MAX_INDX; i++) {
		if (data->fld_data[i]) {
			free(data->fld_data[i]);
			data->fld_data[i] = NULL;
		}
	}
}
