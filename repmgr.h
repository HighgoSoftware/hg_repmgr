/*
 * repmgr.h
 * Copyright (c) 2009-2020, HighGo Software Co.,Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _REPMGR_CONFIG_H
#define _REPMGR_CONFIG_H
#include <config.h>
#endif

#ifndef _REPMGR_H_
#define _REPMGR_H_

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <libpq-fe.h>
#include <postgres_fe.h>
#include <pqexpbuffer.h>


#include "repmgr_version.h"
#include "errcode.h"
#include "strutil.h"
#include "configfile.h"
#include "dbutils.h"
#include "log.h"
#include "sysutils.h"

#define MIN_SUPPORTED_VERSION		"9.3"
#define MIN_SUPPORTED_VERSION_NUM	90300

#define REPLICATION_TYPE_PHYSICAL 1
#define REPLICATION_TYPE_BDR	  2

#define UNKNOWN_SERVER_VERSION_NUM -1
#define UNKNOWN_BDR_VERSION_NUM -1
#define UNKNOWN_REPMGR_VERSION_NUM -1

#define UNKNOWN_TIMELINE_ID -1
#define UNKNOWN_SYSTEM_IDENTIFIER 0
#define UNKNOWN_PID			-1
#define UNKNOWN_REPLICATION_LAG	-1

#define NODE_NOT_FOUND		-1
#define NO_UPSTREAM_NODE	-1
#define UNKNOWN_NODE_ID		-1
#define MIN_NODE_ID          1
#define ELECTION_RERUN_NOTIFICATION -2
#define VOTING_TERM_NOT_SET -1
#define ARCHIVE_STATUS_DIR_ERROR -1
#define NO_DEGRADED_MONITORING_ELAPSED -1

#define BDR2_REPLICATION_SET_NAME "repmgr"

/*
 * various default values - ensure repmgr.conf.sample is update
 * if any of these are changed
 */
#define DEFAULT_LOCATION                     "default"
#define DEFAULT_PRIORITY		             100
#define DEFAULT_RECONNECTION_ATTEMPTS        6	 /* seconds */
#define DEFAULT_RECONNECTION_INTERVAL        10  /* seconds */
#define DEFAULT_MONITORING_INTERVAL          2	 /* seconds */
#define DEFAULT_ASYNC_QUERY_TIMEOUT          60  /* seconds */
#define DEFAULT_PRIMARY_NOTIFICATION_TIMEOUT 60  /* seconds */
#define DEFAULT_PRIMARY_FOLLOW_TIMEOUT       60  /* seconds */
#define DEFAULT_STANDBY_FOLLOW_TIMEOUT       30  /* seconds */
#define DEFAULT_BDR_RECOVERY_TIMEOUT         30  /* seconds */
#define DEFAULT_ARCHIVE_READY_WARNING        16  /* WAL files */
#define DEFAULT_ARCHIVE_READY_CRITICAL       128 /* WAL files */
#define	DEFAULT_REPLICATION_LAG_WARNING      300 /* seconds */
#define DEFAULT_REPLICATION_LAG_CRITICAL     600 /* seconds */
#define DEFAULT_WITNESS_SYNC_INTERVAL        15  /* seconds */
#define DEFAULT_WAIT_START                   30  /* seconds */
#define DEFAULT_PROMOTE_CHECK_TIMEOUT        60  /* seconds */
#define DEFAULT_PROMOTE_CHECK_INTERVAL       1   /* seconds */
#define DEFAULT_SHUTDOWN_CHECK_TIMEOUT       60  /* seconds */
#define DEFAULT_STANDBY_RECONNECT_TIMEOUT    60  /* seconds */
#define DEFAULT_NODE_REJOIN_TIMEOUT          60  /* seconds */
#define DEFAULT_WAL_RECEIVE_CHECK_TIMEOUT    30  /* seconds */
#define DEFAULT_SIBLING_NODES_DISCONNECT_TIMEOUT 30 /* seconds */
#define DEFAULT_ELECTION_RERUN_INTERVAL      15  /* seconds */
#define DEVICE_CHECK_TIMEOUT                 60  /* seconds */  /* highgo */
#define DEVICE_CHECK_TIMES                   3   /* times */    /* highgo */
#define DEFAULT_STANDBY_WAIT_TIMEOUT         10  /* mins */ /* highgo */

#define WALRECEIVER_DISABLE_TIMEOUT_VALUE    86400000 /* milliseconds */

#ifndef RECOVERY_COMMAND_FILE
#define RECOVERY_COMMAND_FILE "recovery.conf"
#endif

#ifndef STANDBY_SIGNAL_FILE
#define STANDBY_SIGNAL_FILE "standby.signal"
#define RECOVERY_SIGNAL_FILE "recovery.signal"
#endif

#ifndef TABLESPACE_MAP
#define TABLESPACE_MAP "tablespace_map"
#endif



#endif							/* _REPMGR_H_ */
