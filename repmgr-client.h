/*
 * repmgr-client.h
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
 *
 */

#ifndef _REPMGR_CLIENT_H_
#define _REPMGR_CLIENT_H_

#include <getopt_long.h>
#include "log.h"


#define NO_ACTION			   0	/* Dummy default action */
#define PRIMARY_REGISTER	   1
#define PRIMARY_UNREGISTER	   2
#define STANDBY_REGISTER	   3
#define STANDBY_UNREGISTER	   4
#define STANDBY_CLONE		   5
#define STANDBY_PROMOTE		   6
#define STANDBY_FOLLOW		   7
#define STANDBY_SWITCHOVER	   8
#define WITNESS_REGISTER       9
#define WITNESS_UNREGISTER     10
#define BDR_REGISTER		   11
#define BDR_UNREGISTER		   12
#define NODE_STATUS			   13
#define NODE_CHECK			   14
#define NODE_SERVICE		   15
#define NODE_REJOIN            16
#define NODE_CONTROL           17
#define NODE_STARTUP           18 /* highgo added to control node startup logic */
#define CLUSTER_SHOW		   19
#define CLUSTER_CLEANUP		   20
#define CLUSTER_MATRIX		   21
#define CLUSTER_CROSSCHECK	   22
#define CLUSTER_EVENT		   23
#define DAEMON_STATUS		   24
#define DAEMON_PAUSE		   25
#define DAEMON_UNPAUSE		   26
#define DAEMON_START 		   27
#define DAEMON_STOP 		   28

/* command line options without short versions */
#define OPT_HELP						   1001
#define OPT_CHECK_UPSTREAM_CONFIG		   1002
#define OPT_COPY_EXTERNAL_CONFIG_FILES	   1003
#define OPT_CSV							   1004
#define OPT_NODE						   1005
#define OPT_NODE_ID						   1006
#define OPT_NODE_NAME					   1007
#define OPT_WITHOUT_BARMAN				   1008
#define OPT_NO_UPSTREAM_CONNECTION		   1009
#define OPT_WAIT_SYNC				       1010
#define OPT_LOG_TO_FILE					   1011
#define OPT_UPSTREAM_CONNINFO			   1012
#define OPT_REPLICATION_USER			   1013
#define OPT_EVENT						   1014
#define OPT_LIMIT						   1015
#define OPT_ALL							   1016
#define OPT_DRY_RUN						   1017
#define OPT_UPSTREAM_NODE_ID			   1018
#define OPT_ACTION						   1019
#define OPT_LIST_ACTIONS				   1020
#define OPT_CHECKPOINT					   1021
#define OPT_IS_SHUTDOWN_CLEANLY			   1022
#define OPT_ALWAYS_PROMOTE				   1023
#define OPT_FORCE_REWIND				   1024
#define OPT_NAGIOS						   1025
#define OPT_ARCHIVE_READY				   1026
#define OPT_OPTFORMAT					   1027
#define OPT_REPLICATION_LAG				   1028
#define OPT_CONFIG_FILES				   1029
#define OPT_SIBLINGS_FOLLOW				   1030
#define OPT_ROLE						   1031
#define OPT_DOWNSTREAM					   1032
#define OPT_SLOTS						   1033
#define OPT_CONFIG_ARCHIVE_DIR			   1034
#define OPT_HAS_PASSFILE                   1035
#define OPT_WAIT_START                     1036
#define OPT_REPL_CONN                      1037
#define OPT_REMOTE_NODE_ID                 1038
#define OPT_RECOVERY_CONF_ONLY             1039
#define OPT_NO_WAIT                        1040
#define OPT_MISSING_SLOTS                  1041
#define OPT_REPMGRD_NO_PAUSE               1042
#define OPT_VERSION_NUMBER				   1043
#define OPT_DATA_DIRECTORY_CONFIG		   1044
#define OPT_COMPACT		                   1045
#define OPT_DISABLE_WAL_RECEIVER           1046
#define OPT_ENABLE_WAL_RECEIVER            1047

/* deprecated since 3.3 */
#define OPT_DATA_DIR						999
#define OPT_NO_CONNINFO_PASSWORD			998
#define OPT_RECOVERY_MIN_APPLY_DELAY		997


static struct option long_options[] =
{
/* general options */
	{"help", no_argument, NULL, OPT_HELP},
	{"version", no_argument, NULL, 'V'},
	{"version-number", no_argument, NULL, OPT_VERSION_NUMBER},

/* general configuration options */
	{"config-file", required_argument, NULL, 'f'},
	{"dry-run", no_argument, NULL, OPT_DRY_RUN},
	{"force", no_argument, NULL, 'F'},
	{"pg_bindir", required_argument, NULL, 'b'},
	{"wait", optional_argument, NULL, 'w'},
	{"no-wait", no_argument, NULL, 'W'},
	{"compact", no_argument, NULL, OPT_COMPACT},

/* connection options */
	{"dbname", required_argument, NULL, 'd'},
	{"host", required_argument, NULL, 'h'},
	{"port", required_argument, NULL, 'p'},
	{"remote-user", required_argument, NULL, 'R'},
	{"superuser", required_argument, NULL, 'S'},
	{"username", required_argument, NULL, 'U'},

/* general node options */
	{"pgdata", required_argument, NULL, 'D'},
	{"node-id", required_argument, NULL, OPT_NODE_ID},
	{"node-name", required_argument, NULL, OPT_NODE_NAME},
	{"remote-node-id", required_argument, NULL, OPT_REMOTE_NODE_ID},

/* logging options */
	{"log-level", required_argument, NULL, 'L'},
	{"log-to-file", no_argument, NULL, OPT_LOG_TO_FILE},
	{"quiet",  no_argument, NULL, 'q'},
	{"terse", no_argument, NULL, 't'},
	{"verbose", no_argument, NULL, 'v'},

/* output options */
	{"csv", no_argument, NULL, OPT_CSV},
	{"nagios", no_argument, NULL, OPT_NAGIOS},
	{"optformat", no_argument, NULL, OPT_OPTFORMAT},

/* "standby clone" options */
	{"copy-external-config-files", optional_argument, NULL, OPT_COPY_EXTERNAL_CONFIG_FILES},
	{"fast-checkpoint", no_argument, NULL, 'c'},
	{"no-upstream-connection", no_argument, NULL, OPT_NO_UPSTREAM_CONNECTION},
	{"recovery-min-apply-delay", required_argument, NULL, OPT_RECOVERY_MIN_APPLY_DELAY},
	{"replication-user", required_argument, NULL, OPT_REPLICATION_USER},
	{"upstream-conninfo", required_argument, NULL, OPT_UPSTREAM_CONNINFO},
	{"upstream-node-id", required_argument, NULL, OPT_UPSTREAM_NODE_ID},
	{"without-barman", no_argument, NULL, OPT_WITHOUT_BARMAN},
	{"recovery-conf-only", no_argument, NULL, OPT_RECOVERY_CONF_ONLY},

/* "standby register" options */
	{"wait-start", required_argument, NULL, OPT_WAIT_START},
	{"wait-sync", optional_argument, NULL, OPT_WAIT_SYNC},

/* "standby switchover" options
 *
 * Note: --force-rewind accepted to pass to "node rejoin"
 */
	{"always-promote", no_argument, NULL, OPT_ALWAYS_PROMOTE},
	{"siblings-follow", no_argument, NULL, OPT_SIBLINGS_FOLLOW},
	{"repmgrd-no-pause", no_argument, NULL, OPT_REPMGRD_NO_PAUSE},

/* "node status" options */
	{"is-shutdown-cleanly", no_argument, NULL, OPT_IS_SHUTDOWN_CLEANLY},

/* "node check" options */
	{"archive-ready", no_argument, NULL, OPT_ARCHIVE_READY},
	{"downstream", no_argument, NULL, OPT_DOWNSTREAM},
	{"replication-lag", no_argument, NULL, OPT_REPLICATION_LAG},
	{"role", no_argument, NULL, OPT_ROLE},
	{"slots", no_argument, NULL, OPT_SLOTS},
	{"missing-slots", no_argument, NULL, OPT_MISSING_SLOTS},
	{"has-passfile", no_argument, NULL, OPT_HAS_PASSFILE},
	{"replication-connection", no_argument, NULL, OPT_REPL_CONN},
	{"data-directory-config", no_argument, NULL, OPT_DATA_DIRECTORY_CONFIG},

/* "node rejoin" options */
	{"config-files", required_argument, NULL, OPT_CONFIG_FILES},
    {"config-archive-dir", required_argument, NULL, OPT_CONFIG_ARCHIVE_DIR},
	{"force-rewind", optional_argument, NULL, OPT_FORCE_REWIND},

/* "node service" options */
	{"action", required_argument, NULL, OPT_ACTION},
	{"list-actions", no_argument, NULL, OPT_LIST_ACTIONS},
	{"checkpoint", no_argument, NULL, OPT_CHECKPOINT},

/* "cluster event" options */
	{"all", no_argument, NULL, OPT_ALL},
	{"event", required_argument, NULL, OPT_EVENT},
	{"limit", required_argument, NULL, OPT_LIMIT},

/* "cluster cleanup" options */
	{"keep-history", required_argument, NULL, 'k'},

/* undocumented options for testing */
	{"disable-wal-receiver", no_argument, NULL, OPT_DISABLE_WAL_RECEIVER},
	{"enable-wal-receiver", no_argument, NULL, OPT_ENABLE_WAL_RECEIVER},

/* deprecated */
	{"check-upstream-config", no_argument, NULL, OPT_CHECK_UPSTREAM_CONFIG},
	{"no-conninfo-password", no_argument, NULL, OPT_NO_CONNINFO_PASSWORD},
	/* previously used by "standby switchover" */
	{"remote-config-file", required_argument, NULL, 'C'},
	/* legacy alias for -D/--pgdata */
	{"data-dir", required_argument, NULL, OPT_DATA_DIR},
	/* replaced by --node-id */
	{"node", required_argument, NULL, OPT_NODE},

	{NULL, 0, NULL, 0}
};


static void do_help(void);

static const char *action_name(const int action);

static void check_cli_parameters(const int action);

#endif							/* _REPMGR_CLIENT_H_ */
