/*
 * repmgrd-physical.c - physical (streaming) replication functionality for repmgrd
 *
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

#include <signal.h>

#include "repmgr.h"
#include "repmgrd.h"
#include "repmgrd-physical.h"

#include "controldata.h"

typedef enum
{
	FAILOVER_STATE_UNKNOWN = -1,
	FAILOVER_STATE_NONE,
	FAILOVER_STATE_PROMOTED,
	FAILOVER_STATE_PROMOTION_FAILED,
	FAILOVER_STATE_PRIMARY_REAPPEARED,
	FAILOVER_STATE_LOCAL_NODE_FAILURE,
	FAILOVER_STATE_WAITING_NEW_PRIMARY,
	FAILOVER_STATE_FOLLOW_NEW_PRIMARY,
	FAILOVER_STATE_REQUIRES_MANUAL_FAILOVER,
	FAILOVER_STATE_FOLLOWED_NEW_PRIMARY,
	FAILOVER_STATE_FOLLOWING_ORIGINAL_PRIMARY,
	FAILOVER_STATE_NO_NEW_PRIMARY,
	FAILOVER_STATE_FOLLOW_FAIL,
	FAILOVER_STATE_NODE_NOTIFICATION_ERROR,
	FAILOVER_STATE_ELECTION_RERUN
} FailoverState;


typedef enum
{
	ELECTION_NOT_CANDIDATE = -1,
	ELECTION_WON,
	ELECTION_LOST,
	ELECTION_CANCELLED,
	ELECTION_RERUN
} ElectionResult;
/*
typedef struct
{
    XLogRecPtr last_lsn_before_Act;
    XLogRecPtr curr_lsn;
}Node_lsn;
*/
typedef enum
{
    DO_NOTHING,
    DO_REJOIN,
    DO_STOP
}BS_ACTION;

typedef enum
{
    TL_LOW,
    TL_HIGH,
    TL_SAME,
    TL_UNKNOWN
}TL_RET;

static PGconn *upstream_conn = NULL;
static PGconn *primary_conn = NULL;
static short touch_label = 0; //highgo
static instr_time unreachable_sync_standby_start; //highgo

static FailoverState failover_state = FAILOVER_STATE_UNKNOWN;

static int	primary_node_id = UNKNOWN_NODE_ID;
static t_node_info upstream_node_info = T_NODE_INFO_INITIALIZER;

static instr_time last_monitoring_update;


static ElectionResult do_election(NodeInfoList *sibling_nodes, int *new_primary_id);
static const char *_print_election_result(ElectionResult result);

static FailoverState promote_self(void);
static void notify_followers(NodeInfoList *standby_nodes, int follow_node_id);

static void check_connection(t_node_info *node_info, PGconn **conn);

static bool check_primary_status(int degraded_monitoring_elapsed);

static bool wait_primary_notification(int *new_primary_id);
static FailoverState follow_new_primary(int new_primary_id);
static FailoverState witness_follow_new_primary(int new_primary_id);

static void reset_node_voting_status(void);

static bool do_primary_failover(void);
static bool do_upstream_standby_failover(void);
static bool do_witness_failover(void);

static void update_monitoring_history(void);

static void handle_sighup(PGconn **conn, t_server_type server_type);

static const char *format_failover_state(FailoverState failover_state);
static const char * format_failover_state(FailoverState failover_state);
static ElectionResult execute_failover_validation_command(t_node_info *node_info);
static void parse_failover_validation_command(const char *template,  t_node_info *node_info, PQExpBufferData *out);
static bool check_node_can_follow(PGconn *local_conn, XLogRecPtr local_xlogpos, PGconn *follow_target_conn, t_node_info *follow_target_node_info);
static void check_disk(void); //highgo
static void signalAlarm(int); //highgo
static bool check_network_card_status(PGconn *conn, int node_id); //highgo
static bool check_service_status_command(const char *command, PQExpBufferData *outputbuf);//highgo
static NodeStatus check_service_status_is_shutdown_cleanly(const char *node_status_output, XLogRecPtr *checkPoint);//highgo
static void exec_node_rejoin_primary(NodeInfoList *my_node_list); //highgo
static void check_sync_async(NodeInfoList *my_node_list); //highgo
static bool only_one_sync_node(char *sync_names); //highgo
static BS_ACTION check_BS(NodeInfoList *my_node_list); //highgo
static TL_RET check_timeline(PGconn *remote_conn,t_node_info *peer_node_info);


void
handle_sigint_physical(SIGNAL_ARGS)
{
	PGconn *writeable_conn;
	PQExpBufferData event_details;

	initPQExpBuffer(&event_details);

	appendPQExpBuffer(&event_details,
					  _("%s signal received"),
					  postgres_signal_arg == SIGTERM
					  ? "TERM" : "INT");

	log_notice("%s", event_details.data);

	if (local_node_info.type == PRIMARY)
		writeable_conn = local_conn;
	else
		writeable_conn = primary_conn;

	if (PQstatus(writeable_conn) == CONNECTION_OK)
		create_event_notification(writeable_conn,
								  &config_file_options,
								  config_file_options.node_id,
								  "repmgrd_shutdown",
								  true,
								  event_details.data);

	termPQExpBuffer(&event_details);

	terminate(SUCCESS);
}

/* perform some sanity checks on the node's configuration */

void
do_physical_node_check(void)
{
	/*
	 * Check if node record is active - if not, and `failover=automatic`, the
	 * node won't be considered as a promotion candidate; this often happens
	 * when a failed primary is recloned and the node was not re-registered,
	 * giving the impression failover capability is there when it's not. In
	 * this case abort with an error and a hint about registering.
	 *
	 * If `failover=manual`, repmgrd can continue to passively monitor the
	 * node, but we should nevertheless issue a warning and the same hint.
	 */

	if (local_node_info.active == false)
	{
		char	   *hint = "Check that \"repmgr (primary|standby) register\" was executed for this node";

		switch (config_file_options.failover)
		{
			/* "failover" is an enum, all values should be covered here */

			case FAILOVER_AUTOMATIC:
				log_error(_("this node is marked as inactive and cannot be used as a failover target"));
				log_hint(_("%s"), hint);

				create_event_notification(NULL,
										  &config_file_options,
										  config_file_options.node_id,
										  "repmgrd_shutdown",
										  false,
										  "node is inactive and cannot be used as a failover target");
        // comment out the terminate, monitoring logic may have chance to update inactive to active 
		//		terminate(ERR_BAD_CONFIG);

			case FAILOVER_MANUAL:
				log_warning(_("this node is marked as inactive and will be passively monitored only"));
				log_hint(_("%s"), hint);
				break;
		}
	}

	if (config_file_options.failover == FAILOVER_AUTOMATIC)
	{
		/*
		 * Check that "promote_command" and "follow_command" are defined, otherwise repmgrd
		 * won't be able to perform any useful action in a failover situation.
		 */

		bool		required_param_missing = false;

		if (config_file_options.promote_command[0] == '\0')
		{
			log_error(_("\"promote_command\" must be defined in the configuration file"));

			if (config_file_options.service_promote_command[0] != '\0')
			{
				/*
				 * "service_promote_command" is *not* a substitute for "promote_command";
				 * it is intended for use in those systems (e.g. Debian) where there's a service
				 * level promote command (e.g. pg_ctlcluster).
				 *
				 * "promote_command" should either execute "repmgr standby promote" directly, or
				 * a script which executes "repmgr standby promote". This is essential, as the
				 * repmgr metadata is updated by "repmgr standby promote".
				 *
				 * "service_promote_command", if set, will be executed by "repmgr standby promote",
				 * but never by repmgrd.
				 *
				 */
				log_hint(_("\"service_promote_command\" is set, but can only be executed by \"repmgr standby promote\""));
			}

			required_param_missing = true;
		}

		if (config_file_options.follow_command[0] == '\0')
		{
			log_error(_("\"follow_command\" must be defined in the configuration file"));
			required_param_missing = true;
		}

		if (required_param_missing == true)
		{
			log_hint(_("add the missing configuration parameter(s) and start repmgrd again"));
			terminate(ERR_BAD_CONFIG);
		}
	}
}



/*
 * repmgrd running on the primary server
 */
void
monitor_streaming_primary(void)
{
	instr_time	log_status_interval_start;
    NodeInfoList mynodes = T_NODE_INFO_LIST_INITIALIZER;

	reset_node_voting_status();

    /*
    if((config_file_options.check_brain_split) && is_BS(local_conn,local_node_info.node_id))
    {
        log_error("Brain split status is not clear, will stop...\n" 
                "pls make sure the brain split has been handled correctly and\n"
                "use \"repmgr primary CLEARBS\" to clear repmgr status");
        exit(ERR_BRAIN_SPLIT);
    }
    */
	{
		PQExpBufferData event_details;
		initPQExpBuffer(&event_details);

		appendPQExpBuffer(&event_details,
						  _("monitoring cluster primary \"%s\" (node ID: %i)"),
						  local_node_info.node_name,
						  local_node_info.node_id);

		/* Log startup event */
		if (startup_event_logged == false)
		{
			create_event_notification(local_conn,
									  &config_file_options,
									  config_file_options.node_id,
									  "repmgrd_start",
									  true,
									  event_details.data);

			startup_event_logged = true;
		}
		else
		{
			create_event_notification(local_conn,
									  &config_file_options,
									  config_file_options.node_id,
									  "repmgrd_reload",
									  true,
									  event_details.data);
		}

		log_notice("%s", event_details.data);

		termPQExpBuffer(&event_details);
	}

	INSTR_TIME_SET_CURRENT(log_status_interval_start);
	local_node_info.node_status = NODE_STATUS_UP;

    /* highgo: read nodes for auto rejoin */
    get_all_node_records(local_conn, &mynodes);
	while (true)
	{
		/*
		 * TODO: cache node list here, refresh at `node_list_refresh_interval`
		 * also return reason for inavailability so we can log it
		 */

		check_connection(&local_node_info, &local_conn);

        if(PQstatus(local_conn) == CONNECTION_OK)
            (void) connection_ping(local_conn);

        //highgo check disk is writable
        check_disk();

        /* highgo: to prevent network is down while db is still accessible locally
         * further check network state
         */
		if ((PQstatus(local_conn) != CONNECTION_OK) || 
           (check_network_card_status(local_conn, local_node_info.node_id)==false))
		{
			/* local node is down, we were expecting it to be up */
			if (local_node_info.node_status == NODE_STATUS_UP)
			{
                PQExpBufferData command_str;
				instr_time	local_node_unreachable_start;

				INSTR_TIME_SET_CURRENT(local_node_unreachable_start);

				{
					PQExpBufferData event_details;
					initPQExpBuffer(&event_details);

					appendPQExpBufferStr(&event_details,
										 _("unable to connect to local node"));

					log_warning("%s", event_details.data);


					/*
					 * as we're monitoring the primary, no point in trying to
					 * write the event to the database
					 *
					 * XXX possible pre-action event
					 */
					create_event_notification(NULL,
											  &config_file_options,
											  config_file_options.node_id,
											  "repmgrd_local_disconnect",
											  true,
											  event_details.data);

					termPQExpBuffer(&event_details);
				}

				local_node_info.node_status = NODE_STATUS_UNKNOWN;

                /* highgo: for db issue, try to reconect 
                 * for network issue, go to degraded directly
                 * */
                if(PQstatus(local_conn) != CONNECTION_OK)
				    try_reconnect(&local_conn, &local_node_info);

				if (local_node_info.node_status == NODE_STATUS_UP)
				{
					int			local_node_unreachable_elapsed = calculate_elapsed(local_node_unreachable_start);
					int 		stored_local_node_id = UNKNOWN_NODE_ID;
					PQExpBufferData event_details;

					initPQExpBuffer(&event_details);

					appendPQExpBuffer(&event_details,
									  _("reconnected to local node after %i seconds"),
									  local_node_unreachable_elapsed);
					log_notice("%s", event_details.data);

					create_event_notification(local_conn,
											  &config_file_options,
											  config_file_options.node_id,
											  "repmgrd_local_reconnect",
											  true,
											  event_details.data);
					termPQExpBuffer(&event_details);

					/*
					 * If the local node was restarted, we'll need to reinitialise values
					 * stored in shared memory.
					 */

					stored_local_node_id = repmgrd_get_local_node_id(local_conn);
					if (stored_local_node_id == UNKNOWN_NODE_ID)
					{
						repmgrd_set_local_node_id(local_conn, config_file_options.node_id);
						repmgrd_set_pid(local_conn, getpid(), pid_file);
					}

					/*
					 * check that the local node is still primary, otherwise switch
					 * to standby monitoring
					 */
					if (check_primary_status(NO_DEGRADED_MONITORING_ELAPSED) == false)
						return;

					goto loop;
				}

				monitoring_state = MS_DEGRADED;
                PQfinish(local_conn);
                local_conn = NULL;
				INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

                /* highgo: When old primary node has stopped from replication cluster, unbind the virtual ip */
                if(unbind_virtual_ip(config_file_options.virtual_ip, config_file_options.network_card))
                    log_notice(_("unbind the virual ip from primary server when it's in degraded status"));

				log_notice(_("unable to connect to local node, falling back to degraded monitoring"));
                 /* highgo: When old primary node is in degraded mode, stop the service*/
                initPQExpBuffer(&command_str);
                appendPQExpBuffer(&command_str,
                        "%s/pg_ctl -D %s stop -m fast",
                        config_file_options.pg_bindir, config_file_options.data_directory);
                system(command_str.data);
                termPQExpBuffer(&command_str);

                /*Then begin to exec 'node rejoin' -- tiabing*/
                sleep(config_file_options.primary_notification_timeout);
                log_debug("exec node rejoin");
                exec_node_rejoin_primary(&mynodes);
			}
            else
            {
                /* highgo: local node status has been set 'NODE_STATUS_DOWN'
                 * try node rejoin */
                log_debug("exec node rejoin, NODE_STATUS_DOWN");
                exec_node_rejoin_primary(&mynodes);
            }
		}
        else /* highgo: local node is reachable */
        {
            check_sync_async(&mynodes);
        }


		if (monitoring_state == MS_DEGRADED)
		{
			int			degraded_monitoring_elapsed = calculate_elapsed(degraded_monitoring_start);

			if (config_file_options.degraded_monitoring_timeout > 0
				&& degraded_monitoring_elapsed > config_file_options.degraded_monitoring_timeout)
			{
				PQExpBufferData event_details;

				initPQExpBuffer(&event_details);

				appendPQExpBuffer(&event_details,
								  _("degraded monitoring timeout (%i seconds) exceeded, terminating"),
								  degraded_monitoring_elapsed);

				log_notice("%s", event_details.data);

				create_event_notification(NULL,
										  &config_file_options,
										  config_file_options.node_id,
										  "repmgrd_shutdown",
										  true,
										  event_details.data);

				termPQExpBuffer(&event_details);
				terminate(ERR_MONITORING_TIMEOUT);
			}

			log_debug("monitoring node in degraded state for %i seconds", degraded_monitoring_elapsed);

			if (is_server_available(local_node_info.conninfo) == true)
			{
				local_conn = establish_db_connection(local_node_info.conninfo, false);

				if (PQstatus(local_conn) != CONNECTION_OK)
				{
					log_warning(_("node appears to be up but no connection could be made"));
					close_connection(&local_conn);
				}
				else
				{
					local_node_info.node_status = NODE_STATUS_UP;

					if (check_primary_status(degraded_monitoring_elapsed) == false)
						return;

					goto loop;
				}
			}


			/*
			 * possibly attempt to find another node from cached list check if
			 * there's a new primary - if so add hook for fencing? loop, if
			 * starts up check status, switch monitoring mode
			 */
		}
loop:

		/* check node is still primary, if not restart monitoring */
		if (check_primary_status(NO_DEGRADED_MONITORING_ELAPSED) == false)
			return;

		/* emit "still alive" log message at regular intervals, if requested */
		if (config_file_options.log_status_interval > 0)
		{
			int			log_status_interval_elapsed = calculate_elapsed(log_status_interval_start);

			if (log_status_interval_elapsed >= config_file_options.log_status_interval)
			{
				log_info(_("monitoring primary node \"%s\" (node ID: %i) in %s state"),
						 local_node_info.node_name,
						 local_node_info.node_id,
						 print_monitoring_state(monitoring_state));

				if (monitoring_state == MS_DEGRADED)
				{
					log_detail(_("waiting for the node to become available"));
				}

				INSTR_TIME_SET_CURRENT(log_status_interval_start);
			}
		}

		if (got_SIGHUP)
		{
			handle_sighup(&local_conn, PRIMARY);
		}

        /* highgo: refresh the node_list in case any new node registered or unregistered */
        if(is_server_available(local_node_info.conninfo) && (local_node_info.node_status == NODE_STATUS_UP))
        {
            hg_get_all_node_records(local_conn, &mynodes);

            if(config_file_options.check_brain_split)
            {
                BS_ACTION ret;
                ret=check_BS(&mynodes);
                if(ret == DO_STOP)
                {
                    PQExpBufferData stop_service_command_str;
                    log_error("Brain split! more than 2 nodes are running as primary. Database would be stop.");
                    unbind_virtual_ip(config_file_options.virtual_ip, config_file_options.network_card);
//                    update_node_record_is_BS(local_conn,true);

                    sleep(5); //sleep to wait for replication copy to other nodes
                    initPQExpBuffer(&stop_service_command_str);
                    appendPQExpBuffer(&stop_service_command_str,
                            "%s/pg_ctl -D %s stop",
                            config_file_options.pg_bindir, config_file_options.data_directory);

                    system(stop_service_command_str.data);
                    termPQExpBuffer(&stop_service_command_str);
                    sleep(2);
                    exit(ERR_BRAIN_SPLIT);
                }
                else if(ret==DO_REJOIN)
                {
                    PQExpBufferData command_str;

                    monitoring_state = MS_DEGRADED;
                    PQfinish(local_conn);
                    local_conn = NULL;
                    INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

                    /* highgo: When old primary node has stopped from replication cluster, unbind the virtual ip */
                    if(unbind_virtual_ip(config_file_options.virtual_ip, config_file_options.network_card))
                        log_notice(_("unbind the virual ip from primary server when it's in degraded status"));

                    /* highgo: When old primary node is in degraded mode, stop the service*/
                    initPQExpBuffer(&command_str);
                    appendPQExpBuffer(&command_str,
                            "%s/pg_ctl -D %s stop -m fast",
                            config_file_options.pg_bindir, config_file_options.data_directory);
                    system(command_str.data);
                    termPQExpBuffer(&command_str);

                    /*Then begin to exec 'node rejoin' -- tiabing*/
                    sleep(config_file_options.primary_notification_timeout);
                    log_debug("exec node rejoin");
                    exec_node_rejoin_primary(&mynodes);
                }
            }
        }
   

		log_verbose(LOG_DEBUG, "sleeping %i seconds (parameter \"monitor_interval_secs\")",
					config_file_options.monitor_interval_secs);

		sleep(config_file_options.monitor_interval_secs);
	}
}


/*
 * If monitoring a primary, it's possible that after an outage of the local node
 * (due to e.g. a switchover), the node has come back as a standby. We therefore
 * need to verify its status and if everything looks OK, restart monitoring in
 * standby mode.
 *
 * Returns "true" to indicate repmgrd should continue monitoring the node as
 * a primary; "false" indicates repmgrd should start monitoring the node as
 * a standby.
 */
bool
check_primary_status(int degraded_monitoring_elapsed)
{
	PGconn *new_primary_conn;
	RecordStatus record_status;
	bool resume_monitoring = true;
	RecoveryType recovery_type = get_recovery_type(local_conn);

	if (recovery_type == RECTYPE_UNKNOWN)
	{
		log_warning(_("unable to determine node recovery status"));
		/* "true" to indicate repmgrd should continue monitoring in degraded state */
		return true;
	}

	/* node is still primary - resume monitoring */
	if (recovery_type == RECTYPE_PRIMARY)
	{
		if (degraded_monitoring_elapsed != NO_DEGRADED_MONITORING_ELAPSED)
		{
			PQExpBufferData event_details;

			monitoring_state = MS_NORMAL;

			initPQExpBuffer(&event_details);
			appendPQExpBuffer(&event_details,
							  _("reconnected to primary node after %i seconds, resuming monitoring"),
							  degraded_monitoring_elapsed);

			create_event_notification(local_conn,
									  &config_file_options,
									  config_file_options.node_id,
									  "repmgrd_local_reconnect",
									  true,
									  event_details.data);

			log_notice("%s", event_details.data);
			termPQExpBuffer(&event_details);
		}

		return true;
	}

	/* the node is now a standby */

	{
		PQExpBufferData event_details;
		initPQExpBuffer(&event_details);

		if (degraded_monitoring_elapsed != NO_DEGRADED_MONITORING_ELAPSED)
		{
			appendPQExpBuffer(&event_details,
							  _("reconnected to node after %i seconds, node is now a standby, switching to standby monitoring"),
							  degraded_monitoring_elapsed);
		}
		else
		{
			appendPQExpBufferStr(&event_details,
								 _("node is now a standby, switching to standby monitoring"));
		}

		log_notice("%s", event_details.data);
		termPQExpBuffer(&event_details);
	}

	primary_node_id = UNKNOWN_NODE_ID;

	new_primary_conn = get_primary_connection_quiet(local_conn, &primary_node_id, NULL);

	if (PQstatus(new_primary_conn) != CONNECTION_OK)
	{
		if (primary_node_id == UNKNOWN_NODE_ID)
		{
			log_warning(_("unable to determine a new primary node"));
		}
		else
		{
			log_warning(_("unable to connect to new primary node %i"), primary_node_id);
			log_detail("\n%s", PQerrorMessage(new_primary_conn));
		}

		close_connection(&new_primary_conn);

		/* "true" to indicate repmgrd should continue monitoring in degraded state */
		return true;
	}

	log_debug("primary node id is now %i", primary_node_id);

	record_status = get_node_record(new_primary_conn, config_file_options.node_id, &local_node_info);

	/*
	 * If, for whatever reason, the new primary has no record of this node,
	 * we won't be able to perform proper monitoring. In that case
	 * terminate and let the user sort out the situation.
	 */
	if (record_status == RECORD_NOT_FOUND)
	{
		PQExpBufferData event_details;

		initPQExpBuffer(&event_details);

		appendPQExpBuffer(&event_details,
						  _("no metadata record found for this node on current primary %i"),
						  primary_node_id);

		log_error("%s", event_details.data);
		log_hint(_("check that 'repmgr (primary|standby) register' was executed for this node"));

		close_connection(&new_primary_conn);

		create_event_notification(NULL,
								  &config_file_options,
								  config_file_options.node_id,
								  "repmgrd_shutdown",
								  false,
								  event_details.data);
		termPQExpBuffer(&event_details);

		terminate(ERR_BAD_CONFIG);
	}

	log_debug("node %i is registered with type = %s",
			  config_file_options.node_id,
			  get_node_type_string(local_node_info.type));

	/*
	 * node has recovered but metadata not updated - we can do that ourselves,
	 */
	if (local_node_info.type == PRIMARY)
	{
		log_notice(_("node \"%s\" (ID: %i) still registered as primary, setting to standby"),
				   config_file_options.node_name,
				   config_file_options.node_id);

		if (update_node_record_set_active_standby(new_primary_conn, config_file_options.node_id) == false)
		{
			resume_monitoring = false;
		}
		else
		{
			/* refresh our copy of the node record from the primary */
			record_status = get_node_record(new_primary_conn, config_file_options.node_id, &local_node_info);

			/* this is unlikley to happen */
			if (record_status != RECORD_FOUND)
			{
				log_warning(_("unable to retrieve local node record from primary node %i"), primary_node_id);
				resume_monitoring = false;
			}
		}
	}

	if (resume_monitoring == true)
	{
		PQExpBufferData event_details;
		initPQExpBuffer(&event_details);

		if (degraded_monitoring_elapsed != NO_DEGRADED_MONITORING_ELAPSED)
		{
			monitoring_state = MS_NORMAL;

			log_notice(_("former primary has been restored as standby after %i seconds, updating node record and resuming monitoring"),
					   degraded_monitoring_elapsed);

			appendPQExpBuffer(&event_details,
							  _("node restored as standby after %i seconds, monitoring connection to upstream node %i"),
							  degraded_monitoring_elapsed,
							  local_node_info.upstream_node_id);
		}
		else
		{
			appendPQExpBuffer(&event_details,
							  _("node has become a standby, monitoring connection to upstream node %i"),
							  local_node_info.upstream_node_id);
		}

		create_event_notification(new_primary_conn,
								  &config_file_options,
								  config_file_options.node_id,
								  "repmgrd_standby_reconnect",
								  true,
								  event_details.data);

		termPQExpBuffer(&event_details);

		close_connection(&new_primary_conn);

		/* restart monitoring as standby */
		return false;
	}

	/* continue monitoring as before */
	return true;
}


/*
 * repmgrd running on a standby server
 */
void
monitor_streaming_standby(void)
{
	RecordStatus record_status;
	instr_time	log_status_interval_start;

	MonitoringState local_monitoring_state = MS_NORMAL;
	instr_time	local_degraded_monitoring_start;

	int last_known_upstream_node_id = UNKNOWN_NODE_ID;
    int i=0;

	log_debug("monitor_streaming_standby()");

	reset_node_voting_status();

	INSTR_TIME_SET_ZERO(last_monitoring_update);

	/*
	 * If no upstream node id is specified in the metadata, we'll try and
	 * determine the current cluster primary in the assumption we should
	 * connect to that by default.
	 */
	if (local_node_info.upstream_node_id == UNKNOWN_NODE_ID)
	{
		upstream_conn = get_primary_connection(local_conn, &local_node_info.upstream_node_id, NULL);

		/*
		 * Terminate if there doesn't appear to be an active cluster primary.
		 * There could be one or more nodes marked as inactive primaries, and
		 * one of them could actually be a primary, but we can't sensibly
		 * monitor in that state.
		 */
		if (local_node_info.upstream_node_id == NODE_NOT_FOUND)
		{
			log_error(_("unable to determine an active primary for this cluster, terminating"));
			terminate(ERR_BAD_CONFIG);
		}

		log_debug("upstream node ID determined as %i", local_node_info.upstream_node_id);

		(void) get_node_record(upstream_conn, local_node_info.upstream_node_id, &upstream_node_info);
        if (PQstatus(upstream_conn) != CONNECTION_OK)
        {
            log_error(_("unable connect to upstream node (ID: %i), terminating"),
                    local_node_info.upstream_node_id);
            terminate(ERR_DB_CONN);
        }
	}
	else
	{
		log_debug("upstream node ID in local node record is %i", local_node_info.upstream_node_id);

		record_status = get_node_record(local_conn, local_node_info.upstream_node_id, &upstream_node_info);

		/*
		 * Terminate if we can't find the record for the node we're supposed to
		 * monitor. This is a "fix-the-config" situation, not a lot else we can
		 * do.
		 */
		if (record_status == RECORD_NOT_FOUND)
		{
			log_error(_("no record found for upstream node (ID: %i), terminating"),
					  local_node_info.upstream_node_id);
			log_hint(_("ensure the upstream node is registered correctly"));

			terminate(ERR_DB_CONN);
		}
		else if (record_status == RECORD_ERROR)
		{
			log_error(_("unable to retrieve record for upstream node (ID: %i), terminating"),
					  local_node_info.upstream_node_id);

			terminate(ERR_DB_CONN);
		}

		log_debug("connecting to upstream node %i: \"%s\"", upstream_node_info.node_id, upstream_node_info.conninfo);

		upstream_conn = establish_db_connection(upstream_node_info.conninfo, false);
        if (PQstatus(upstream_conn) != CONNECTION_OK)
        {
            bool upstream_ok=false;
            int wait_sec = config_file_options.standby_wait_timeout * 60; // minutes->secends
            log_hint(_("upstream node not running when repmgrd start, wait for %d mins"),config_file_options.standby_wait_timeout);
            for(i=0; i<wait_sec; i+=10)
            {
                log_error("sleep 10s and try to connect upstream node %d again",local_node_info.upstream_node_id);
                sleep(10);
                if(PQstatus(local_conn) == CONNECTION_OK)
                    (void) connection_ping(local_conn); //keep alive
                upstream_conn = establish_db_connection(upstream_node_info.conninfo, false);
                if (PQstatus(upstream_conn) != CONNECTION_OK)
                    continue;
                else
                {
                    upstream_ok=true;
                    break;
                }
            }
            if(upstream_ok == false)
            {
                last_known_upstream_node_id = local_node_info.upstream_node_id;
                primary_conn=NULL;
                primary_node_id = get_primary_node_id(local_conn);
                INSTR_TIME_SET_CURRENT(log_status_interval_start);
                if (upstream_node_info.type == STANDBY)
                {
                    log_error(_("upstream node is standby,goto degraded state"));
                    upstream_node_info.node_status = NODE_STATUS_DOWN;
                    monitoring_state = MS_DEGRADED;
                    goto DEGRADED_LABEL;
                }
                else //upsteam is primary
                {
                    log_error(_("upstream node is not up, goto monitor loop and wait for failover"));
                    monitoring_state = MS_NORMAL;
                    upstream_node_info.node_status = NODE_STATUS_UP;
                    goto NO_UPSTREAM_LABEL;
                }
            }
        }
	}


	/*
	 * Upstream node must be running at repmgrd startup.
	 *
	 * We could possibly have repmgrd skip to degraded monitoring mode until
	 * it comes up, but there doesn't seem to be much point in doing that.
	 */
	/*if (PQstatus(upstream_conn) != CONNECTION_OK)
	{
		log_error(_("unable connect to upstream node (ID: %i), terminating"),
				  local_node_info.upstream_node_id);
		log_hint(_("upstream node must be running before repmgrd can start"));
		terminate(ERR_DB_CONN);
	}*/

	record_status = get_node_record(upstream_conn, local_node_info.node_id, &local_node_info);

	if (upstream_node_info.node_id == local_node_info.node_id)
	{
		PQfinish(upstream_conn);
		upstream_conn = NULL;
		return;
	}

	last_known_upstream_node_id = local_node_info.upstream_node_id;

	/*
	 * refresh upstream node record from upstream node, so it's as up-to-date
	 * as possible
	 */
	record_status = get_node_record(upstream_conn, upstream_node_info.node_id, &upstream_node_info);

	if (upstream_node_info.type == STANDBY)
	{
		log_debug("upstream node is standby, connecting to primary");
		/*
		 * Currently cascaded standbys need to be able to connect to the
		 * primary. We could possibly add a limited connection mode for cases
		 * where this isn't possible, but that will complicate things further.
		 */
		primary_conn = establish_primary_db_connection(upstream_conn, false);

		if (PQstatus(primary_conn) != CONNECTION_OK)
		{
			log_error(_("unable to connect to primary node"));
			log_hint(_("ensure the primary node is reachable from this node"));

			terminate(ERR_DB_CONN);
		}

		log_verbose(LOG_DEBUG, "connected to primary");
	}
	else
	{
		log_debug("upstream node is primary");
		primary_conn = upstream_conn;
	}

	/*
	 * It's possible monitoring has been restarted after some outage which
	 * resulted in the local node being marked as inactive; if so mark it
	 * as active again.
	 */
	if (local_node_info.active == false)
	{
		if (update_node_record_set_active(primary_conn, local_node_info.node_id, true) == true)
		{
			PQExpBufferData event_details;

			initPQExpBuffer(&event_details);

			local_node_info.active = true;
		}
	}

	if (PQstatus(primary_conn) == CONNECTION_OK)
	{
		primary_node_id = get_primary_node_id(primary_conn);
		log_debug("primary_node_id is %i", primary_node_id);
	}
	else
	{
		primary_node_id = get_primary_node_id(local_conn);
		log_debug("primary_node_id according to local records is %i", primary_node_id);
	}


	/* Log startup event */
	if (startup_event_logged == false)
	{
		PQExpBufferData event_details;

		initPQExpBuffer(&event_details);

		appendPQExpBuffer(&event_details,
						  _("monitoring connection to upstream node \"%s\" (node ID: %i)"),
						  upstream_node_info.node_name,
						  upstream_node_info.node_id);

		create_event_notification(primary_conn,
								  &config_file_options,
								  config_file_options.node_id,
								  "repmgrd_start",
								  true,
								  event_details.data);

		startup_event_logged = true;

		log_info("%s", event_details.data);

		termPQExpBuffer(&event_details);
	}

	monitoring_state = MS_NORMAL;
	INSTR_TIME_SET_CURRENT(log_status_interval_start);
	upstream_node_info.node_status = NODE_STATUS_UP;

NO_UPSTREAM_LABEL:
	while (true)
	{
        /* highgo: check local and auto rejoin */
        if (is_server_available(local_node_info.conninfo) == false)
        {
            PQExpBufferData node_rejoin_command_str;
            PQExpBufferData output_buf;
            PQExpBufferData check_clean_shutdown_str;
            bool    success = false;
            int     r;

            initPQExpBuffer(&output_buf);
            initPQExpBuffer(&check_clean_shutdown_str);
            appendPQExpBuffer(&check_clean_shutdown_str,
                    "%s/repmgr node status --is-shutdown-cleanly;",
                    config_file_options.pg_bindir);

            success = check_service_status_command(check_clean_shutdown_str.data, &output_buf);
            termPQExpBuffer(&check_clean_shutdown_str);

            if (success == true)
            {
                XLogRecPtr      checkpoint_lsn = InvalidXLogRecPtr;
                NodeStatus      status = check_service_status_is_shutdown_cleanly(output_buf.data, &checkpoint_lsn);
                if (status == NODE_STATUS_UNCLEAN_SHUTDOWN)
                {
                    /*start and stop the service*/
                    PQExpBufferData stop_service_command_str;
                    log_notice("unclean shutdown detected, start and stop db to clean");

                    initPQExpBuffer(&stop_service_command_str);
                    appendPQExpBuffer(&stop_service_command_str,
                            "%s/pg_ctl -D %s start;",
                            config_file_options.pg_bindir, config_file_options.data_directory);
                    appendPQExpBuffer(&stop_service_command_str,
                            "%s/pg_ctl -D %s stop",
                            config_file_options.pg_bindir, config_file_options.data_directory);

                    system(stop_service_command_str.data);

                    termPQExpBuffer(&stop_service_command_str);
                }
            }
            termPQExpBuffer(&output_buf);

            initPQExpBuffer(&node_rejoin_command_str);

            appendPQExpBuffer(&node_rejoin_command_str,
                    "repmgr -d \'%s\' node rejoin --force-rewind",
                    upstream_node_info.conninfo);

            if (local_node_info.failed_connect_times >= config_file_options.reconnect_attempts)
            {
                r = system(node_rejoin_command_str.data);
                if (r != 0)
                {
                    log_warning(_("unable to exec 'node rejoin' "));
                }

            }
            local_node_info.failed_connect_times++;
            termPQExpBuffer(&node_rejoin_command_str);
        }
        else
        {
            local_node_info.failed_connect_times = 0;
        }

		log_verbose(LOG_DEBUG, "checking %s", upstream_node_info.conninfo);
		if (check_upstream_connection(&upstream_conn, upstream_node_info.conninfo) == true)
		{
			set_upstream_last_seen(local_conn);
		}
		else
		{
			/* upstream node is down, we were expecting it to be up */
			if (upstream_node_info.node_status == NODE_STATUS_UP)
			{
				instr_time	upstream_node_unreachable_start;

				INSTR_TIME_SET_CURRENT(upstream_node_unreachable_start);


				upstream_node_info.node_status = NODE_STATUS_UNKNOWN;

				{
					PQExpBufferData event_details;
					initPQExpBuffer(&event_details);

					appendPQExpBuffer(&event_details,
									  _("unable to connect to upstream node \"%s\" (node ID: %i)"),
									  upstream_node_info.node_name, upstream_node_info.node_id);

					/* XXX possible pre-action event */
					if (upstream_node_info.type == STANDBY)
					{
						create_event_record(primary_conn,
											&config_file_options,
											config_file_options.node_id,
											"repmgrd_upstream_disconnect",
											true,
											event_details.data);
					}
					else
					{
						/* primary connection lost - script notification only */
						create_event_record(NULL,
											&config_file_options,
											config_file_options.node_id,
											"repmgrd_upstream_disconnect",
											true,
											event_details.data);
					}

					log_warning("%s", event_details.data);
					termPQExpBuffer(&event_details);
				}

				/*
				 * if local node is unreachable, make a last-minute attempt to reconnect
				 * before continuing with the failover process
				 */

				if (PQstatus(local_conn) != CONNECTION_OK)
				{
					check_connection(&local_node_info, &local_conn);
				}

				try_reconnect(&upstream_conn, &upstream_node_info);

				/* Upstream node has recovered - log and continue */
				if (upstream_node_info.node_status == NODE_STATUS_UP)
				{
					int			upstream_node_unreachable_elapsed = calculate_elapsed(upstream_node_unreachable_start);
					PQExpBufferData event_details;

					initPQExpBuffer(&event_details);

					appendPQExpBuffer(&event_details,
									  _("reconnected to upstream node after %i seconds"),
									  upstream_node_unreachable_elapsed);
					log_notice("%s", event_details.data);

					if (upstream_node_info.type == PRIMARY)
					{
						primary_conn = upstream_conn;

						if (get_recovery_type(primary_conn) == RECTYPE_STANDBY)
						{
							ExecStatusType ping_result;

							/*
							 * we're returning at the end of this block and no longer require the
							 * event details buffer
							 */
							termPQExpBuffer(&event_details);

							log_notice(_("current upstream node \"%s\" (node ID: %i) is not primary, restarting monitoring"),
									   upstream_node_info.node_name, upstream_node_info.node_id);
							PQfinish(upstream_conn);
							upstream_conn = NULL;
							local_node_info.upstream_node_id = UNKNOWN_NODE_ID;

							/* check local connection */
							ping_result = connection_ping(local_conn);

							if (ping_result != PGRES_TUPLES_OK)
							{
								int i;

								PQfinish(local_conn);

								for (i = 0; i < config_file_options.repmgrd_standby_startup_timeout; i++)
								{
									local_conn = establish_db_connection(local_node_info.conninfo, false);

									if (PQstatus(local_conn) == CONNECTION_OK)
										break;

									log_debug("sleeping 1 second; %i of %i attempts to reconnect to local node",
											  i + 1,
											  config_file_options.repmgrd_standby_startup_timeout);
									sleep(1);
								}
							}

							return;
						}
					}

					create_event_notification(primary_conn,
											  &config_file_options,
											  config_file_options.node_id,
											  "repmgrd_upstream_reconnect",
											  true,
											  event_details.data);
					termPQExpBuffer(&event_details);

					goto loop;
				}


				/* upstream is still down after reconnect attempt(s) */
				if (upstream_node_info.node_status == NODE_STATUS_DOWN)
				{
					bool		failover_done = false;

					if (PQstatus(local_conn) == CONNECTION_OK && repmgrd_is_paused(local_conn))
					{
						log_notice(_("repmgrd on this node is paused"));
						log_detail(_("no failover will be carried out"));
						log_hint(_("execute \"repmgr daemon unpause\" to resume normal failover mode"));
						monitoring_state = MS_DEGRADED;
						INSTR_TIME_SET_CURRENT(degraded_monitoring_start);
					}
                    /* if cluster is in brain split, do not do failover */
                    /*
                    else if(is_BS(local_conn,local_node_info.node_id))
                    {
                        log_warning(_("The cluster is in Brain split, do not take failover"));
                        log_detail(_("no failover will be carried out"));
                        monitoring_state = MS_DEGRADED;
                        INSTR_TIME_SET_CURRENT(degraded_monitoring_start);
                    }*/
					else
					{
						if (upstream_node_info.type == PRIMARY)
						{
							failover_done = do_primary_failover();
						}
						else if (upstream_node_info.type == STANDBY)
						{

							failover_done = do_upstream_standby_failover();

							if (failover_done == false)
							{
								monitoring_state = MS_DEGRADED;
								INSTR_TIME_SET_CURRENT(degraded_monitoring_start);
							}
						}

						/*
						 * XXX it's possible it will make sense to return in all
						 * cases to restart monitoring
						 */
						if (failover_done == true)
						{
							primary_node_id = get_primary_node_id(local_conn);
							return;
						}
					}
				}
			}
		}
DEGRADED_LABEL:
		if (monitoring_state == MS_DEGRADED)
		{
			int			degraded_monitoring_elapsed = calculate_elapsed(degraded_monitoring_start);

			if (config_file_options.degraded_monitoring_timeout > 0
				&& degraded_monitoring_elapsed > config_file_options.degraded_monitoring_timeout)
			{
				PQExpBufferData event_details;

				initPQExpBuffer(&event_details);

				appendPQExpBuffer(&event_details,
								  _("degraded monitoring timeout (%i seconds) exceeded, terminating"),
								  degraded_monitoring_elapsed);

				log_notice("%s", event_details.data);

				create_event_notification(NULL,
										  &config_file_options,
										  config_file_options.node_id,
										  "repmgrd_shutdown",
										  true,
										  event_details.data);

				termPQExpBuffer(&event_details);
				terminate(ERR_MONITORING_TIMEOUT);
			}

			log_debug("monitoring upstream node %i in degraded state for %i seconds",
					  upstream_node_info.node_id,
					  degraded_monitoring_elapsed);

			if (check_upstream_connection(&upstream_conn, upstream_node_info.conninfo) == true)
			{
				if (config_file_options.connection_check_type != CHECK_QUERY)
					upstream_conn = establish_db_connection(upstream_node_info.conninfo, false);

				if (PQstatus(upstream_conn) == CONNECTION_OK)
				{
					PQExpBufferData event_details;

					log_debug("upstream node %i has recovered",
							  upstream_node_info.node_id);

					/* XXX check here if upstream is still primary */
					/*
					 * -> will be a problem if another node was promoted in
					 * the meantime
					 */
					/* and upstream is now former primary */
					/* XXX scan other nodes to see if any has become primary */

					upstream_node_info.node_status = NODE_STATUS_UP;
					monitoring_state = MS_NORMAL;

					if (upstream_node_info.type == PRIMARY)
					{
						primary_conn = upstream_conn;
					}
					else
					{
						if (primary_conn == NULL || PQstatus(primary_conn) != CONNECTION_OK)
						{
							primary_conn = establish_primary_db_connection(upstream_conn, false);
						}
					}

					initPQExpBuffer(&event_details);

					appendPQExpBuffer(&event_details,
									  _("reconnected to upstream node %i after %i seconds, resuming monitoring"),
									  upstream_node_info.node_id,
									  degraded_monitoring_elapsed);

					create_event_notification(primary_conn,
											  &config_file_options,
											  config_file_options.node_id,
											  "repmgrd_upstream_reconnect",
											  true,
											  event_details.data);

					log_notice("%s", event_details.data);
					termPQExpBuffer(&event_details);

					goto loop;
				}
			}
			else
			{
				/*
				 * unable to connect to former primary - check if another node
				 * has been promoted
				 */

				NodeInfoListCell *cell;
				int			follow_node_id = UNKNOWN_NODE_ID;

				/* local node has been promoted */
				if (get_recovery_type(local_conn) == RECTYPE_PRIMARY)
				{
					log_notice(_("local node is primary, checking local node state"));

					/*
					 * It's possible the promote command timed out, but the promotion itself
					 * succeeded. In this case failover state will be FAILOVER_STATE_PROMOTION_FAILED;
					 * we can update the node record ourselves and resume primary monitoring.
					 */
					if (failover_state == FAILOVER_STATE_PROMOTION_FAILED)
					{
						int			degraded_monitoring_elapsed;
						int			former_upstream_node_id = local_node_info.upstream_node_id;
						NodeInfoList sibling_nodes = T_NODE_INFO_LIST_INITIALIZER;
						PQExpBufferData event_details;

						update_node_record_set_primary(local_conn,  local_node_info.node_id);
						record_status = get_node_record(local_conn, local_node_info.node_id, &local_node_info);

						degraded_monitoring_elapsed = calculate_elapsed(degraded_monitoring_start);

						log_notice(_("resuming monitoring as primary node after %i seconds"),
								   degraded_monitoring_elapsed);

						initPQExpBuffer(&event_details);
						appendPQExpBufferStr(&event_details,
											 _("promotion command failed but promotion completed successfully"));
						create_event_notification(local_conn,
												  &config_file_options,
												  local_node_info.node_id,
												  "repmgrd_failover_promote",
												  true,
												  event_details.data);

						termPQExpBuffer(&event_details);

						/* notify former siblings that they should now follow this node */
						get_active_sibling_node_records(local_conn,
														local_node_info.node_id,
														former_upstream_node_id,
														&sibling_nodes);
						notify_followers(&sibling_nodes, local_node_info.node_id);

						clear_node_info_list(&sibling_nodes);

						/* this will restart monitoring in primary mode */
						monitoring_state = MS_NORMAL;
						return;
					}

					/*
					 * There may be a delay between the node being promoted
					 * and the local record being updated, so if the node
					 * record still shows it as a standby, do nothing, we'll
					 * catch the update during the next loop. (e.g. node was
					 * manually promoted) we'll do nothing, as the repmgr
					 * metadata is now out-of-sync. If it does get fixed,
					 * we'll catch it here on a future iteration.
					 */

					/* refresh own internal node record */
					record_status = refresh_node_record(local_conn, local_node_info.node_id, &local_node_info);

					if (local_node_info.type == PRIMARY)
					{
						int			degraded_monitoring_elapsed = calculate_elapsed(degraded_monitoring_start);

						log_notice(_("resuming monitoring as primary node after %i seconds"),
								   degraded_monitoring_elapsed);

						/* this will restart monitoring in primary mode */
						monitoring_state = MS_NORMAL;
						return;
					}
				}


				if (config_file_options.failover == FAILOVER_AUTOMATIC && repmgrd_is_paused(local_conn) == false)
				{
					NodeInfoList sibling_nodes = T_NODE_INFO_LIST_INITIALIZER;

					get_active_sibling_node_records(local_conn,
													local_node_info.node_id,
													local_node_info.upstream_node_id,
													&sibling_nodes);

					if (sibling_nodes.node_count > 0)
					{
						log_debug("scanning %i node records to detect new primary...", sibling_nodes.node_count);
						for (cell = sibling_nodes.head; cell; cell = cell->next)
						{
							/* skip local node check, we did that above */
							if (cell->node_info->node_id == local_node_info.node_id)
							{
								continue;
							}

							/* skip witness node - we can't possibly "follow" that */

							if (cell->node_info->type == WITNESS)
							{
								continue;
							}

							cell->node_info->conn = establish_db_connection(cell->node_info->conninfo, false);

							if (PQstatus(cell->node_info->conn) != CONNECTION_OK)
							{
								log_debug("unable to connect to %i ... ", cell->node_info->node_id);
								continue;
							}

							if (get_recovery_type(cell->node_info->conn) == RECTYPE_PRIMARY)
							{
								follow_node_id = cell->node_info->node_id;
								close_connection(&cell->node_info->conn);
								break;
							}
							close_connection(&cell->node_info->conn);
						}

						if (follow_node_id != UNKNOWN_NODE_ID)
						{
							follow_new_primary(follow_node_id);
						}
					}

					clear_node_info_list(&sibling_nodes);
				}
			}
		}

loop:

		/* emit "still alive" log message at regular intervals, if requested */
		if (config_file_options.log_status_interval > 0)
		{
			int			log_status_interval_elapsed = calculate_elapsed(log_status_interval_start);

			if (log_status_interval_elapsed >= config_file_options.log_status_interval)
			{
				PQExpBufferData monitoring_summary;

				initPQExpBuffer(&monitoring_summary);

				appendPQExpBuffer(&monitoring_summary,
								  _("node \"%s\" (node ID: %i) monitoring upstream node \"%s\" (node ID: %i) in %s state"),
								  local_node_info.node_name,
								  local_node_info.node_id,
								  upstream_node_info.node_name,
								  upstream_node_info.node_id,
								  print_monitoring_state(monitoring_state));

				if (config_file_options.failover == FAILOVER_MANUAL)
				{
					appendPQExpBufferStr(&monitoring_summary,
										 _(" (automatic failover disabled)"));
				}

				log_info("%s", monitoring_summary.data);
				termPQExpBuffer(&monitoring_summary);

				if (monitoring_state == MS_DEGRADED && config_file_options.failover == FAILOVER_AUTOMATIC)
				{
					if (PQstatus(local_conn) == CONNECTION_OK && repmgrd_is_paused(local_conn))
					{
						log_detail(_("repmgrd paused by administrator"));
						log_hint(_("execute \"repmgr daemon unpause\" to resume normal failover mode"));
					}
					else
					{
						log_detail(_("waiting for upstream or another primary to reappear"));
					}
				}

				/*
				 * Add update about monitoring updates.
				 *
				 * Note: with cascaded replication, it's possible we're still able to write
				 * monitoring history to the primary even if the upstream is still reachable.
				 */

				if (PQstatus(primary_conn) == CONNECTION_OK && config_file_options.monitoring_history == true)
				{
					if (INSTR_TIME_IS_ZERO(last_monitoring_update))
					{
						log_detail(_("no monitoring statistics have been written yet"));
					}
					else
					{
						log_detail(_("last monitoring statistics update was %i seconds ago"),
								   calculate_elapsed(last_monitoring_update));
					}
				}

				INSTR_TIME_SET_CURRENT(log_status_interval_start);
			}
		}

		if (PQstatus(primary_conn) == CONNECTION_OK && config_file_options.monitoring_history == true)
		{
			update_monitoring_history();
		}
		else
		{
			if (config_file_options.monitoring_history == true)
			{
				log_verbose(LOG_WARNING, _("monitoring_history requested but primary connection not available"));
			}

			/*
			 * if monitoring not in use, we'll need to ensure the local connection
			 * handle isn't stale
			 */
            if(PQstatus(local_conn) == CONNECTION_OK)
			    (void) connection_ping(local_conn);
		}

		/*
		 * handle local node failure
		 *
		 * currently we'll just check the connection, and try to reconnect
		 *
		 * TODO: add timeout, after which we run in degraded state
		 */

		check_connection(&local_node_info, &local_conn);

		if (PQstatus(local_conn) != CONNECTION_OK)
		{
			if (local_node_info.active == true)
			{
				bool success = true;
				PQExpBufferData event_details;

				initPQExpBuffer(&event_details);

				local_node_info.active = false;

				appendPQExpBuffer(&event_details,
								  _("unable to connect to local node \"%s\" (ID: %i), marking inactive"),
								  local_node_info.node_name,
								  local_node_info.node_id);
				log_notice("%s", event_details.data);

				if (PQstatus(primary_conn) == CONNECTION_OK)
				{
					if (update_node_record_set_active(primary_conn, local_node_info.node_id, false) == false)
					{
						success = false;
						log_warning(_("unable to mark node \"%s\" (ID: %i) as inactive"),
									  local_node_info.node_name,
									  local_node_info.node_id);
					}
				}

				create_event_notification(primary_conn,
										  &config_file_options,
										  local_node_info.node_id,
										  "standby_failure",
										  success,
										  event_details.data);

				termPQExpBuffer(&event_details);
			}

			if (local_monitoring_state == MS_NORMAL)
			{
				log_info("entering degraded monitoring for the local node");
				local_monitoring_state = MS_DEGRADED;
				INSTR_TIME_SET_CURRENT(local_degraded_monitoring_start);
			}
		}
		else
		{
			int stored_local_node_id = UNKNOWN_NODE_ID;

			if (local_monitoring_state == MS_DEGRADED)
			{
				log_info(_("connection to local node recovered after %i seconds"),
						 calculate_elapsed(local_degraded_monitoring_start));
				local_monitoring_state = MS_NORMAL;

				/*
				 * Check if anything has changed since the local node came back on line;
				 * we may need to restart monitoring.
				 */
				refresh_node_record(local_conn, local_node_info.node_id, &local_node_info);

				if (last_known_upstream_node_id != local_node_info.upstream_node_id)
				{
					log_notice(_("local node %i upstream appears to have changed, restarting monitoring"),
							   local_node_info.node_id);
					log_detail(_("currently monitoring upstream %i; new upstream is %i"),
							   last_known_upstream_node_id,
							   local_node_info.upstream_node_id);
					close_connection(&upstream_conn);
					return;
				}

				/*
				 *
				 */
				if (local_node_info.type != STANDBY)
				{
					log_notice(_("local node %i is no longer a standby, restarting monitoring"),
							   local_node_info.node_id);
					close_connection(&upstream_conn);
					return;
				}
			}

			/*
			 * If the local node was restarted, we'll need to reinitialise values
			 * stored in shared memory.
			 */
			stored_local_node_id = repmgrd_get_local_node_id(local_conn);

			if (stored_local_node_id == UNKNOWN_NODE_ID)
			{
				repmgrd_set_local_node_id(local_conn, config_file_options.node_id);
				repmgrd_set_pid(local_conn, getpid(), pid_file);
			}

			if (PQstatus(primary_conn) == CONNECTION_OK)
			{
				if (get_recovery_type(primary_conn) == RECTYPE_STANDBY)
				{
					log_notice(_("current upstream node \"%s\" (node ID: %i) is not primary, restarting monitoring"),
							   upstream_node_info.node_name, upstream_node_info.node_id);
					PQfinish(primary_conn);
					primary_conn = NULL;

					local_node_info.upstream_node_id = UNKNOWN_NODE_ID;
					return;
				}
			}

			/* we've reconnected to the local node after an outage */
			if (local_node_info.active == false)
			{
				if (PQstatus(primary_conn) == CONNECTION_OK)
				{
					if (update_node_record_set_active(primary_conn, local_node_info.node_id, true) == true)
					{
						PQExpBufferData event_details;

						initPQExpBuffer(&event_details);

						local_node_info.active = true;
						appendPQExpBuffer(&event_details,
										  _("reconnected to local node \"%s\" (ID: %i), marking active"),
										  local_node_info.node_name,
										  local_node_info.node_id);

						log_notice("%s", event_details.data);

						create_event_notification(primary_conn,
												  &config_file_options,
												  local_node_info.node_id,
												  "standby_recovery",
												  true,
												  event_details.data);

						termPQExpBuffer(&event_details);
					}
				}
			}
		}


		if (got_SIGHUP)
		{
			handle_sighup(&local_conn, STANDBY);
		}

		refresh_node_record(local_conn, local_node_info.node_id, &local_node_info);

		if (local_monitoring_state == MS_NORMAL && last_known_upstream_node_id != local_node_info.upstream_node_id)
        {
            /*
             * It's possible that after a change of upstream, the local node record will not
             * yet have been updated with the new upstream node ID. Therefore we check the
             * node record on the upstream, and if that matches "last_known_upstream_node_id",
             * take that as the correct value.
             */

            if (monitoring_state == MS_NORMAL)
            {
                t_node_info node_info_on_upstream = T_NODE_INFO_INITIALIZER;
                record_status = get_node_record(primary_conn, config_file_options.node_id, &node_info_on_upstream);

                if (last_known_upstream_node_id == node_info_on_upstream.upstream_node_id)
                {
                    local_node_info.upstream_node_id = last_known_upstream_node_id;
                }
            }

            if (last_known_upstream_node_id != local_node_info.upstream_node_id)
            {
                log_notice(_("local node %i's upstream appears to have changed, restarting monitoring"),
                        local_node_info.node_id);
                log_detail(_("currently monitoring upstream %i; new upstream is %i"),
                        last_known_upstream_node_id,
                        local_node_info.upstream_node_id);
                close_connection(&upstream_conn);
                return;
            }
        }

		log_verbose(LOG_DEBUG, "sleeping %i seconds (parameter \"monitor_interval_secs\")",
					config_file_options.monitor_interval_secs);


		sleep(config_file_options.monitor_interval_secs);
	}
}


void
monitor_streaming_witness(void)
{
	instr_time	log_status_interval_start;
	instr_time	witness_sync_interval_start;

	RecordStatus record_status;

	int primary_node_id = UNKNOWN_NODE_ID;

	reset_node_voting_status();

	log_debug("monitor_streaming_witness()");

	/*
	 * At this point we can't trust the local copy of "repmgr.nodes", as
	 * it may not have been updated. We'll scan the cluster to find the
	 * current primary and refresh the copy from that before proceeding
	 * further.
	 */
	primary_conn = get_primary_connection_quiet(local_conn, &primary_node_id, NULL);

	/*
	 * Primary node should be running at repmgrd startup.
	 *
	 * Otherwise we'll skip to degraded monitoring.
	 */
	if (PQstatus(primary_conn) == CONNECTION_OK)
	{
		PQExpBufferData event_details;

		char *event_type = startup_event_logged == false
			? "repmgrd_start"
			: "repmgrd_upstream_reconnect";

		/* synchronise local copy of "repmgr.nodes", in case it was stale */
		witness_copy_node_records(primary_conn, local_conn);

		/*
		 * refresh upstream node record from primary, so it's as up-to-date
		 * as possible
		 */
		record_status = get_node_record(primary_conn, primary_node_id, &upstream_node_info);

		/*
		 * This is unlikely to happen; if it does emit a warning for diagnostic
		 * purposes and plough on regardless.
		 *
		 * A check for the existence of the record will have already been carried out
		 * in main().
		 */
		if (record_status != RECORD_FOUND)
		{
			log_warning(_("unable to retrieve node record from primary"));
		}

		initPQExpBuffer(&event_details);

		appendPQExpBuffer(&event_details,
						  _("witness monitoring connection to primary node \"%s\" (node ID: %i)"),
						  upstream_node_info.node_name,
						  upstream_node_info.node_id);

		log_info("%s", event_details.data);

		create_event_notification(primary_conn,
								  &config_file_options,
								  config_file_options.node_id,
								  event_type,
								  true,
								  event_details.data);

		if (startup_event_logged == false)
			startup_event_logged = true;

		termPQExpBuffer(&event_details);

		monitoring_state = MS_NORMAL;
		INSTR_TIME_SET_CURRENT(log_status_interval_start);
		INSTR_TIME_SET_CURRENT(witness_sync_interval_start);

		upstream_node_info.node_status = NODE_STATUS_UP;
	}
	else
	{
		log_warning(_("unable to connect to primary"));
		log_detail("\n%s", PQerrorMessage(primary_conn));
		/*
		 * Here we're unable to connect to a primary despite having scanned all
		 * known nodes, so we'll grab the record of the node we think is primary
		 * and continue straight to degraded monitoring in the hope a primary
		 * will appear.
		 */

		primary_node_id = get_primary_node_id(local_conn);

		log_notice(_("setting primary_node_id to last known ID %i"), primary_node_id);

		record_status = get_node_record(local_conn, primary_node_id, &upstream_node_info);

		/*
		 * This is unlikely to happen, but if for whatever reason there's
		 * no primary record in the local table, we should just give up
		 */
		if (record_status != RECORD_FOUND)
		{
			log_error(_("unable to retrieve node record for last known primary %i"),
						primary_node_id);
			log_hint(_("execute \"repmgr witness register --force\" to sync the local node records"));
			PQfinish(local_conn);
			terminate(ERR_BAD_CONFIG);
		}

		monitoring_state = MS_DEGRADED;
		INSTR_TIME_SET_CURRENT(degraded_monitoring_start);
		upstream_node_info.node_status = NODE_STATUS_DOWN;
	}

	while (true)
	{
		if (check_upstream_connection(&primary_conn, upstream_node_info.conninfo) == true)
		{
			set_upstream_last_seen(local_conn);
		}
		else
		{
			if (upstream_node_info.node_status == NODE_STATUS_UP)
			{
				instr_time		upstream_node_unreachable_start;

				INSTR_TIME_SET_CURRENT(upstream_node_unreachable_start);

				upstream_node_info.node_status = NODE_STATUS_UNKNOWN;

				{
					PQExpBufferData event_details;

					initPQExpBuffer(&event_details);

					appendPQExpBuffer(&event_details,
									  _("unable to connect to primary node \"%s\" (node ID: %i)"),
									  upstream_node_info.node_name, upstream_node_info.node_id);

					create_event_record(NULL,
										&config_file_options,
										config_file_options.node_id,
										"repmgrd_upstream_disconnect",
										true,
										event_details.data);
					termPQExpBuffer(&event_details);
				}

				try_reconnect(&primary_conn, &upstream_node_info);

				/* Node has recovered - log and continue */
				if (upstream_node_info.node_status == NODE_STATUS_UP)
				{
					int			upstream_node_unreachable_elapsed = calculate_elapsed(upstream_node_unreachable_start);
					PQExpBufferData event_details;

					initPQExpBuffer(&event_details);

					appendPQExpBuffer(&event_details,
									  _("reconnected to upstream node after %i seconds"),
									  upstream_node_unreachable_elapsed);
					log_notice("%s", event_details.data);

					/* check upstream is still primary */
					if (get_recovery_type(primary_conn) != RECTYPE_PRIMARY)
					{
						log_notice(_("current upstream node \"%s\" (node ID: %i) is not primary, restarting monitoring"),
								   upstream_node_info.node_name, upstream_node_info.node_id);
						PQfinish(primary_conn);
						primary_conn = NULL;
						termPQExpBuffer(&event_details);
						return;
					}

					create_event_notification(primary_conn,
											  &config_file_options,
											  config_file_options.node_id,
											  "repmgrd_upstream_reconnect",
											  true,
											  event_details.data);
					termPQExpBuffer(&event_details);

					goto loop;
				}

				/* still down after reconnect attempt(s) */
				if (upstream_node_info.node_status == NODE_STATUS_DOWN)
				{
					bool		failover_done = false;


					failover_done = do_witness_failover();

					/*
					 * XXX it's possible it will make sense to return in all
					 * cases to restart monitoring
					 */
					if (failover_done == true)
					{
						primary_node_id = get_primary_node_id(local_conn);
						return;
					}
				}
			}
		}


		if (monitoring_state == MS_DEGRADED)
		{
			int			degraded_monitoring_elapsed = calculate_elapsed(degraded_monitoring_start);

			log_debug("monitoring node %i in degraded state for %i seconds",
					  upstream_node_info.node_id,
					  degraded_monitoring_elapsed);

			if (check_upstream_connection(&primary_conn, upstream_node_info.conninfo) == true)
			{
				if (config_file_options.connection_check_type != CHECK_QUERY)
					primary_conn = establish_db_connection(upstream_node_info.conninfo, false);

				if (PQstatus(primary_conn) == CONNECTION_OK)
				{
					PQExpBufferData event_details;

					upstream_node_info.node_status = NODE_STATUS_UP;
					monitoring_state = MS_NORMAL;

					initPQExpBuffer(&event_details);

					appendPQExpBuffer(&event_details,
									  _("reconnected to upstream node %i after %i seconds, resuming monitoring"),
									  upstream_node_info.node_id,
									  degraded_monitoring_elapsed);

					log_notice("%s", event_details.data);

					/* check upstream is still primary */
					if (get_recovery_type(primary_conn) != RECTYPE_PRIMARY)
					{
						log_notice(_("current upstream node \"%s\" (node ID: %i) is not primary, restarting monitoring"),
								   upstream_node_info.node_name, upstream_node_info.node_id);
						PQfinish(primary_conn);
						primary_conn = NULL;
						termPQExpBuffer(&event_details);
						return;
					}

					create_event_notification(primary_conn,
											  &config_file_options,
											  config_file_options.node_id,
											  "repmgrd_upstream_reconnect",
											  true,
											  event_details.data);
					termPQExpBuffer(&event_details);

					goto loop;
				}
			}
			else
			{
				/*
				 * unable to connect to former primary - check if another node
				 * has been promoted
				 */

				NodeInfoListCell *cell;
				int			follow_node_id = UNKNOWN_NODE_ID;
				NodeInfoList sibling_nodes = T_NODE_INFO_LIST_INITIALIZER;

				get_active_sibling_node_records(local_conn,
												local_node_info.node_id,
												local_node_info.upstream_node_id,
												&sibling_nodes);

				if (sibling_nodes.node_count > 0)
				{
					log_debug("scanning %i node records to detect new primary...", sibling_nodes.node_count);
					for (cell = sibling_nodes.head; cell; cell = cell->next)
					{
						/* skip local node check, we did that above */
						if (cell->node_info->node_id == local_node_info.node_id)
						{
							continue;
						}

						/* skip node if configured as a witness node - we can't possibly "follow" that */
						if (cell->node_info->type == WITNESS)
						{
							continue;
						}

						cell->node_info->conn = establish_db_connection(cell->node_info->conninfo, false);

						if (PQstatus(cell->node_info->conn) != CONNECTION_OK)
						{
							log_debug("unable to connect to %i ... ", cell->node_info->node_id);
							continue;
						}

						if (get_recovery_type(cell->node_info->conn) == RECTYPE_PRIMARY)
						{
							follow_node_id = cell->node_info->node_id;
							close_connection(&cell->node_info->conn);
							break;
						}
						close_connection(&cell->node_info->conn);
					}

					if (follow_node_id != UNKNOWN_NODE_ID)
					{
						witness_follow_new_primary(follow_node_id);
					}
				}
				clear_node_info_list(&sibling_nodes);
			}
		}
loop:

		/*
		 * handle local node failure
		 *
		 * currently we'll just check the connection, and try to reconnect
		 *
		 * TODO: add timeout, after which we run in degraded state
		 */

		check_connection(&local_node_info, &local_conn);
        if(PQstatus(local_conn) == CONNECTION_OK)
            (void) connection_ping(local_conn);

		if (PQstatus(local_conn) != CONNECTION_OK)
		{
			if (local_node_info.active == true)
			{
				bool success = true;
				PQExpBufferData event_details;

				initPQExpBuffer(&event_details);

				local_node_info.active = false;

				appendPQExpBuffer(&event_details,
								  _("unable to connect to local node \"%s\" (ID: %i), marking inactive"),
								  local_node_info.node_name,
								  local_node_info.node_id);
				log_notice("%s", event_details.data);

				if (PQstatus(primary_conn) == CONNECTION_OK)
				{
					if (update_node_record_set_active(primary_conn, local_node_info.node_id, false) == false)
					{
						success = false;
						log_warning(_("unable to mark node \"%s\" (ID: %i) as inactive"),
									  local_node_info.node_name,
									  local_node_info.node_id);
					}
				}

				create_event_notification(primary_conn,
										  &config_file_options,
										  local_node_info.node_id,
										  "standby_failure",
										  success,
										  event_details.data);

				termPQExpBuffer(&event_details);
			}
		}
		else
		{
			/* we've reconnected to the local node after an outage */
			if (local_node_info.active == false)
			{
				int stored_local_node_id = UNKNOWN_NODE_ID;

				if (PQstatus(primary_conn) == CONNECTION_OK)
				{
					if (update_node_record_set_active(primary_conn, local_node_info.node_id, true) == true)
					{
						PQExpBufferData event_details;

						initPQExpBuffer(&event_details);

						local_node_info.active = true;

						appendPQExpBuffer(&event_details,
										  _("reconnected to local node \"%s\" (ID: %i), marking active"),
										  local_node_info.node_name,
										  local_node_info.node_id);

						log_notice("%s", event_details.data);

						create_event_notification(primary_conn,
												  &config_file_options,
												  local_node_info.node_id,
												  "standby_recovery",
												  true,
												  event_details.data);

						termPQExpBuffer(&event_details);
					}
				}

				/*
				 * If the local node was restarted, we'll need to reinitialise values
				 * stored in shared memory.
				 */

				stored_local_node_id = repmgrd_get_local_node_id(local_conn);
				if (stored_local_node_id == UNKNOWN_NODE_ID)
				{
					repmgrd_set_local_node_id(local_conn, config_file_options.node_id);
					repmgrd_set_pid(local_conn, getpid(), pid_file);
				}
			}
		}


		/*
		 * Refresh repmgr.nodes after "witness_sync_interval" seconds, and check if primary
		 * has changed
		 */

		if (PQstatus(primary_conn) == CONNECTION_OK)
		{
			int witness_sync_interval_elapsed = calculate_elapsed(witness_sync_interval_start);

			if (witness_sync_interval_elapsed >= config_file_options.witness_sync_interval)
			{
				if (get_recovery_type(primary_conn) != RECTYPE_PRIMARY)
				{
					log_notice(_("current upstream node \"%s\" (node ID: %i) is not primary, restarting monitoring"),
							   upstream_node_info.node_name, upstream_node_info.node_id);
					PQfinish(primary_conn);
					primary_conn = NULL;
					return;
				}

				log_debug("synchronising witness node records");
				witness_copy_node_records(primary_conn, local_conn);

				INSTR_TIME_SET_CURRENT(witness_sync_interval_start);
			}
		}

		/* emit "still alive" log message at regular intervals, if requested */
		if (config_file_options.log_status_interval > 0)
		{
			int			log_status_interval_elapsed = calculate_elapsed(log_status_interval_start);

			if (log_status_interval_elapsed >= config_file_options.log_status_interval)
			{
				PQExpBufferData monitoring_summary;

				initPQExpBuffer(&monitoring_summary);

				appendPQExpBuffer(&monitoring_summary,
								  _("witness node \"%s\" (node ID: %i) monitoring primary node \"%s\" (node ID: %i) in %s state"),
								  local_node_info.node_name,
								  local_node_info.node_id,
								  upstream_node_info.node_name,
								  upstream_node_info.node_id,
								  print_monitoring_state(monitoring_state));

				log_info("%s", monitoring_summary.data);
				termPQExpBuffer(&monitoring_summary);
				if (monitoring_state == MS_DEGRADED && config_file_options.failover == FAILOVER_AUTOMATIC)
				{
					log_detail(_("waiting for current or new primary to reappear"));
				}

				INSTR_TIME_SET_CURRENT(log_status_interval_start);
			}
		}



		if (got_SIGHUP)
		{
			handle_sighup(&local_conn, WITNESS);
		}

		log_verbose(LOG_DEBUG, "sleeping %i seconds (parameter \"monitor_interval_secs\")",
					config_file_options.monitor_interval_secs);

		sleep(config_file_options.monitor_interval_secs);
	}

	return;

}


static bool
do_primary_failover(void)
{
	ElectionResult election_result;
	bool final_result = false;
	NodeInfoList sibling_nodes = T_NODE_INFO_LIST_INITIALIZER;
	int new_primary_id = UNKNOWN_NODE_ID;

	/*
	 * Double-check status of the local connection
	 */
	check_connection(&local_node_info, &local_conn);

	/*
	 * if requested, disable WAL receiver and wait until WAL receivers on all
	 * sibling nodes are disconnected
	 */
	if (config_file_options.standby_disconnect_on_failover == true)
	{
		NodeInfoListCell *cell = NULL;
		NodeInfoList check_sibling_nodes = T_NODE_INFO_LIST_INITIALIZER;
		int i;

		bool sibling_node_wal_receiver_connected = false;

		if (PQserverVersion(local_conn) < 90500)
		{
			log_warning(_("\"standby_disconnect_on_failover\" specified, but not available for this PostgreSQL version"));
			/* TODO: format server version */
			log_detail(_("available from PostgreSQL 9.5, this PostgreSQL version is %i"), PQserverVersion(local_conn));
		}
		else
		{
			disable_wal_receiver(local_conn);

			/*
			 * Loop through all reachable sibling nodes to determine whether
			 * they have disabled their WAL receivers.
			 *
			 * TODO: do_election() also calls get_active_sibling_node_records(),
			 * consolidate calls if feasible
			 *
			 */
			get_active_sibling_node_records(local_conn,
											local_node_info.node_id,
											local_node_info.upstream_node_id,
											&check_sibling_nodes);

			for (i = 0; i < config_file_options.sibling_nodes_disconnect_timeout; i++)
			{
				for (cell = check_sibling_nodes.head; cell; cell = cell->next)
				{
					pid_t sibling_wal_receiver_pid;

					if (cell->node_info->conn == NULL)
						cell->node_info->conn = establish_db_connection(cell->node_info->conninfo, false);

					sibling_wal_receiver_pid = (pid_t)get_wal_receiver_pid(cell->node_info->conn);

					if (sibling_wal_receiver_pid == UNKNOWN_PID)
					{
						log_warning(_("unable to query WAL receiver PID on node %i"),
									cell->node_info->node_id);
					}
					else if (sibling_wal_receiver_pid > 0)
					{
						log_info(_("WAL receiver PID on node %i is %i"),
								 cell->node_info->node_id,
								 sibling_wal_receiver_pid);
						sibling_node_wal_receiver_connected = true;
					}
				}

				if (sibling_node_wal_receiver_connected == false)
				{
					log_notice(_("WAL receiver disconnected on all sibling nodes"));
					break;
				}

				log_debug("sleeping %i of max %i seconds (\"sibling_nodes_disconnect_timeout\")",
						  i + 1, config_file_options.sibling_nodes_disconnect_timeout);
				sleep(1);
			}

			if (sibling_node_wal_receiver_connected == true)
			{
				/* TODO: prevent any such nodes becoming promotion candidates */
				log_warning(_("WAL receiver still connected on at least one sibling node"));
			}
			else
			{
				log_info(_("WAL receiver disconnected on all %i sibling nodes"),
						 check_sibling_nodes.node_count);
			}

			clear_node_info_list(&check_sibling_nodes);
		}
	}

	/* attempt to initiate voting process */
	election_result = do_election(&sibling_nodes, &new_primary_id);

	/* TODO add pre-event notification here */
	failover_state = FAILOVER_STATE_UNKNOWN;

	log_debug("election result: %s", _print_election_result(election_result));

	/* Reenable WAL receiver, if disabled */
	if (config_file_options.standby_disconnect_on_failover == true)
	{
		/* adjust "wal_retrieve_retry_interval" but don't wait for WAL receiver to start */
		enable_wal_receiver(local_conn, false);
	}

	/* election was cancelled and do_election() did not determine a new primary */
	if (election_result == ELECTION_CANCELLED)
	{
		if (new_primary_id == UNKNOWN_NODE_ID)
		{
			log_notice(_("election cancelled"));
			clear_node_info_list(&sibling_nodes);
			return false;
		}

		log_info(_("follower node intending to follow new primary %i"), new_primary_id);

		failover_state = FAILOVER_STATE_FOLLOW_NEW_PRIMARY;
	}
	else if (election_result == ELECTION_RERUN)
	{
		log_notice(_("promotion candidate election will be rerun"));
		/* notify siblings that they should rerun the election too */
		notify_followers(&sibling_nodes, ELECTION_RERUN_NOTIFICATION);

		failover_state = FAILOVER_STATE_ELECTION_RERUN;
	}
	else if (election_result == ELECTION_WON)
	{
		if (sibling_nodes.node_count > 0)
		{
			log_notice("this node is the winner, will now promote itself and inform other nodes");
		}
		else
		{
			log_notice("this node is the only available candidate and will now promote itself");
		}

		failover_state = promote_self();

        /* highgo: When new primary node has promoted successful, bind virtual ip to the node's network card */
        if(failover_state==FAILOVER_STATE_PROMOTED)
        {
            if(check_vip_conf(config_file_options.virtual_ip, config_file_options.network_card))
            {
                if(bind_virtual_ip(config_file_options.virtual_ip, config_file_options.network_card))
                    log_notice(_("bind the virtual ip when promoting local node to new primary server"));
            }
        }
	}
	else if (election_result == ELECTION_LOST || election_result == ELECTION_NOT_CANDIDATE)
	{
		/*
		 * if the node couldn't be promoted as it's not in the same location as the primary,
		 * add an explanatory notice
		 */
		if (election_result == ELECTION_NOT_CANDIDATE && strncmp(upstream_node_info.location, local_node_info.location, MAXLEN) != 0)
		{
			log_notice(_("this node's location (\"%s\") is not the primary node location (\"%s\"), so node cannot be promoted"),
					   local_node_info.location,
					   upstream_node_info.location);
		}

		log_info(_("follower node awaiting notification from a candidate node"));

		failover_state = FAILOVER_STATE_WAITING_NEW_PRIMARY;
	}

	/*
	 * node has determined a new primary is already available
	 */
	if (failover_state == FAILOVER_STATE_FOLLOW_NEW_PRIMARY)
	{
		failover_state = follow_new_primary(new_primary_id);
	}

	/*
	 * node has decided it is a follower, so will await notification from the
	 * candidate that it has promoted itself and can be followed
	 */
	else if (failover_state == FAILOVER_STATE_WAITING_NEW_PRIMARY)
	{
		/* TODO: rerun election if new primary doesn't appear after timeout */

		/* either follow, self-promote or time out; either way resume monitoring */
		if (wait_primary_notification(&new_primary_id) == true)
		{
			/* if primary has reappeared, no action needed */
			if (new_primary_id == upstream_node_info.node_id)
			{
				failover_state = FAILOVER_STATE_FOLLOWING_ORIGINAL_PRIMARY;
			}
			/* if new_primary_id is self, promote */
			else if (new_primary_id == local_node_info.node_id)
			{
				log_notice(_("this node is promotion candidate, promoting"));

				failover_state = promote_self();

				get_active_sibling_node_records(local_conn,
												local_node_info.node_id,
												upstream_node_info.node_id,
												&sibling_nodes);

			}
			/* election rerun */
			else if (new_primary_id == ELECTION_RERUN_NOTIFICATION)
			{
				log_notice(_("received notification from promotion candidate to rerun election"));
				failover_state = FAILOVER_STATE_ELECTION_RERUN;
			}
			else if (config_file_options.failover == FAILOVER_MANUAL)
			{
				/* automatic failover disabled */

				t_node_info new_primary = T_NODE_INFO_INITIALIZER;
				RecordStatus record_status = RECORD_NOT_FOUND;
				PGconn	   *new_primary_conn;

				record_status = get_node_record(local_conn, new_primary_id, &new_primary);

				if (record_status != RECORD_FOUND)
				{
					log_error(_("unable to retrieve metadata record for new primary node (ID: %i)"),
							  new_primary_id);
				}
				else
				{
					PQExpBufferData event_details;

					initPQExpBuffer(&event_details);
					appendPQExpBuffer(&event_details,
									  _("node %i is in manual failover mode and is now disconnected from streaming replication"),
									  local_node_info.node_id);

					new_primary_conn = establish_db_connection(new_primary.conninfo, false);

					create_event_notification(new_primary_conn,
											  &config_file_options,
											  local_node_info.node_id,
											  "standby_disconnect_manual",
											  /*
											   * here "true" indicates the action has occurred as expected
											   */
											  true,
											  event_details.data);
					close_connection(&new_primary_conn);
					termPQExpBuffer(&event_details);

				}
				failover_state = FAILOVER_STATE_REQUIRES_MANUAL_FAILOVER;
			}
			else
			{
				failover_state = follow_new_primary(new_primary_id);
			}
		}
		else
		{
			failover_state = FAILOVER_STATE_NO_NEW_PRIMARY;
		}
	}

	log_verbose(LOG_DEBUG, "failover state is %s",
				format_failover_state(failover_state));

	switch (failover_state)
	{
		case FAILOVER_STATE_PROMOTED:
			/* notify former siblings that they should now follow this node */
			notify_followers(&sibling_nodes, local_node_info.node_id);

			/* pass control back down to start_monitoring() */
			log_info(_("switching to primary monitoring mode"));

			failover_state = FAILOVER_STATE_NONE;

			final_result = true;

			break;

		case FAILOVER_STATE_ELECTION_RERUN:

			/* we no longer care about our former siblings */
			clear_node_info_list(&sibling_nodes);

			log_notice(_("rerunning election after %i seconds (\"election_rerun_interval\")"),
					   config_file_options.election_rerun_interval);
			sleep(config_file_options.election_rerun_interval);

			log_info(_("election rerun will now commence"));
			/*
			 * mark the upstream node as "up" so another election is triggered
			 * after we fall back to monitoring
			 */
			upstream_node_info.node_status = NODE_STATUS_UP;
			failover_state = FAILOVER_STATE_NONE;

			final_result = false;
			break;

		case FAILOVER_STATE_PRIMARY_REAPPEARED:

			/*
			 * notify siblings that they should resume following the original
			 * primary
			 */
			notify_followers(&sibling_nodes, upstream_node_info.node_id);

			/* pass control back down to start_monitoring() */

			log_info(_("resuming %s monitoring mode"), get_node_type_string(local_node_info.type));
			log_detail(_("original primary \"%s\" (node ID: %i) reappeared"),
					   upstream_node_info.node_name, upstream_node_info.node_id);

			failover_state = FAILOVER_STATE_NONE;

			final_result = true;
			break;

		case FAILOVER_STATE_FOLLOWED_NEW_PRIMARY:
			log_info(_("resuming %s monitoring mode"), get_node_type_string(local_node_info.type));
			log_detail(_("following new primary \"%s\" (node id: %i)"),
					   upstream_node_info.node_name, upstream_node_info.node_id);
			failover_state = FAILOVER_STATE_NONE;

			final_result = true;
			break;

		case FAILOVER_STATE_FOLLOWING_ORIGINAL_PRIMARY:
			log_info(_("resuming %s monitoring mode"), get_node_type_string(local_node_info.type));
			log_detail(_("following original primary \"%s\" (node id: %i)"),
					   upstream_node_info.node_name, upstream_node_info.node_id);
			failover_state = FAILOVER_STATE_NONE;

			final_result = true;
			break;

		case FAILOVER_STATE_PROMOTION_FAILED:
			monitoring_state = MS_DEGRADED;
			INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

			final_result = false;
			break;

		case FAILOVER_STATE_FOLLOW_FAIL:

			/*
			 * for whatever reason we were unable to follow the new primary -
			 * continue monitoring in degraded state
			 */
			monitoring_state = MS_DEGRADED;
			INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

			final_result = false;
			break;

		case FAILOVER_STATE_REQUIRES_MANUAL_FAILOVER:
			log_info(_("automatic failover disabled for this node, manual intervention required"));

			monitoring_state = MS_DEGRADED;
			INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

			final_result = false;
			break;

		case FAILOVER_STATE_NO_NEW_PRIMARY:
		case FAILOVER_STATE_WAITING_NEW_PRIMARY:
			/* pass control back down to start_monitoring() */
			final_result = false;
			break;

		case FAILOVER_STATE_NODE_NOTIFICATION_ERROR:
		case FAILOVER_STATE_LOCAL_NODE_FAILURE:
		case FAILOVER_STATE_UNKNOWN:
		case FAILOVER_STATE_NONE:

			final_result = false;
			break;

		default:	/* should never reach here */
			log_warning(_("unhandled failover state %i"), failover_state);
			break;
	}

	/* we no longer care about our former siblings */
	clear_node_info_list(&sibling_nodes);

	return final_result;
}




static void
update_monitoring_history(void)
{
	ReplInfo	replication_info;
	XLogRecPtr	primary_last_wal_location = InvalidXLogRecPtr;

	long long unsigned int apply_lag_bytes = 0;
	long long unsigned int replication_lag_bytes = 0;

	/* both local and primary connections must be available */
	if (PQstatus(primary_conn) != CONNECTION_OK)
	{
		log_warning(_("primary connection is not available, unable to update monitoring history"));
		return;
	}

	if (PQstatus(local_conn) != CONNECTION_OK)
	{
		log_warning(_("local connection is not available, unable to update monitoring history"));
		return;
	}

	init_replication_info(&replication_info);

	if (get_replication_info(local_conn, STANDBY, &replication_info) == false)
	{
		log_warning(_("unable to retrieve replication status information, unable to update monitoring history"));
		return;
	}

	/*
	 * This can be the case when a standby is starting up after following
	 * a new primary, or when it has dropped back to archive recovery.
	 * As long as we can connect to the primary, we can still provide lag information.
	 */
	if (replication_info.receiving_streamed_wal == false)
	{
		log_verbose(LOG_WARNING, _("standby %i not connected to streaming replication"),
					local_node_info.node_id);
	}

	primary_last_wal_location = get_primary_current_lsn(primary_conn);

	if (primary_last_wal_location == InvalidXLogRecPtr)
	{
		log_warning(_("unable to retrieve primary's current LSN"));
		return;
	}

	/* calculate apply lag in bytes */
	if (replication_info.last_wal_receive_lsn >= replication_info.last_wal_replay_lsn)
	{
		apply_lag_bytes = (long long unsigned int) (replication_info.last_wal_receive_lsn - replication_info.last_wal_replay_lsn);
	}
	else
	{
		/* if this happens, it probably indicates archive recovery */
		apply_lag_bytes = 0;
	}

	/* calculate replication lag in bytes */

	if (primary_last_wal_location >= replication_info.last_wal_receive_lsn)
	{
		replication_lag_bytes = (long long unsigned int) (primary_last_wal_location - replication_info.last_wal_receive_lsn);
	}
	else
	{
		/*
		 * This should never happen, but in case it does set replication lag
		 * to zero
		 */
		log_warning("primary xlog (%X/%X) location appears less than standby receive location (%X/%X)",
					format_lsn(primary_last_wal_location),
					format_lsn(replication_info.last_wal_receive_lsn));
		replication_lag_bytes = 0;
	}

	add_monitoring_record(primary_conn,
						  local_conn,
						  primary_node_id,
						  local_node_info.node_id,
						  replication_info.current_timestamp,
						  primary_last_wal_location,
						  replication_info.last_wal_receive_lsn,
						  replication_info.last_xact_replay_timestamp,
						  replication_lag_bytes,
						  apply_lag_bytes);

	INSTR_TIME_SET_CURRENT(last_monitoring_update);
}


/*
 * do_upstream_standby_failover()
 *
 * Attach cascaded standby to another node, currently the primary.
 *
 * Note that in contrast to a primary failover, where one of the downstrean
 * standby nodes will become a primary, a cascaded standby failover (where the
 * upstream standby has gone away) is "just" a case of attaching the standby to
 * another node.
 *
 * Currently we will try to attach the node to the cluster primary.
 *
 * TODO: As of repmgr 4.3, "repmgr standby follow" supports attaching a standby to another
 * standby node. We need to provide a selection of reconnection strategies as different
 * behaviour might be desirable in different situations.
 */

static bool
do_upstream_standby_failover(void)
{
	t_node_info primary_node_info = T_NODE_INFO_INITIALIZER;
	RecordStatus record_status = RECORD_NOT_FOUND;
	RecoveryType primary_type = RECTYPE_UNKNOWN;
	int			i, standby_follow_result;
	char		parsed_follow_command[MAXPGPATH] = "";

	close_connection(&upstream_conn);

	/*
	 *
	 */
	if (config_file_options.failover == FAILOVER_MANUAL)
	{
		log_notice(_("this node is not configured for automatic failover"));
		return false;
	}

	if (get_primary_node_record(local_conn, &primary_node_info) == false)
	{
		log_error(_("unable to retrieve primary node record"));
		return false;
	}

	/*
	 * Verify that we can still talk to the cluster primary, even though the
	 * node's upstream is not available
	 */

	check_connection(&primary_node_info, &primary_conn);

	if (PQstatus(primary_conn) != CONNECTION_OK)
	{
		log_error(_("unable to connect to last known primary \"%s\" (ID: %i)"),
				  primary_node_info.node_name,
				  primary_node_info.node_id);

		close_connection(&primary_conn);
		monitoring_state = MS_DEGRADED;
		INSTR_TIME_SET_CURRENT(degraded_monitoring_start);
		return false;
	}

	primary_type = get_recovery_type(primary_conn);

	if (primary_type != RECTYPE_PRIMARY)
	{
		if (primary_type == RECTYPE_STANDBY)
		{
			log_error(_("last known primary \"%s\" (ID: %i) is in recovery, not following"),
					  primary_node_info.node_name,
					  primary_node_info.node_id);
		}
		else
		{
			log_error(_("unable to determine status of last known primary \"%s\" (ID: %i), not following"),
					  primary_node_info.node_name,
					  primary_node_info.node_id);
		}

		close_connection(&primary_conn);
		monitoring_state = MS_DEGRADED;
		INSTR_TIME_SET_CURRENT(degraded_monitoring_start);
		return false;
	}

	/* Close the connection to this server */
	close_connection(&local_conn);

	log_debug(_("standby follow command is:\n  \"%s\""),
			  config_file_options.follow_command);

	/*
	 * replace %n in "config_file_options.follow_command" with ID of primary
	 * to follow.
	 */
	parse_follow_command(parsed_follow_command, config_file_options.follow_command, primary_node_info.node_id);

	standby_follow_result = system(parsed_follow_command);

	if (standby_follow_result != 0)
	{
		PQExpBufferData event_details;

		initPQExpBuffer(&event_details);

		appendPQExpBuffer(&event_details,
						  _("unable to execute follow command:\n %s"),
						  config_file_options.follow_command);

		log_error("%s", event_details.data);

		/*
		 * It may not possible to write to the event notification table but we
		 * should be able to generate an external notification if required.
		 */
		create_event_notification(primary_conn,
								  &config_file_options,
								  local_node_info.node_id,
								  "repmgrd_failover_follow",
								  false,
								  event_details.data);

		termPQExpBuffer(&event_details);
	}

	/*
	 * It's possible that the standby is still starting up after the "follow_command"
	 * completes, so poll for a while until we get a connection.
	 *
	 * NOTE: we've previously closed the local connection, so even if the follow command
	 * failed for whatever reason and the local node remained up, we can re-open
	 * the local connection.
	 */

	for (i = 0; i < config_file_options.repmgrd_standby_startup_timeout; i++)
	{
		local_conn = establish_db_connection(local_node_info.conninfo, false);

		if (PQstatus(local_conn) == CONNECTION_OK)
			break;

		log_debug("sleeping 1 second; %i of %i (\"repmgrd_standby_startup_timeout\") attempts to reconnect to local node",
				  i + 1,
				  config_file_options.repmgrd_standby_startup_timeout);
		sleep(1);
	}

	if (PQstatus(local_conn) != CONNECTION_OK)
	{
		log_error(_("unable to reconnect to local node %i"),
				  local_node_info.node_id);
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	/* refresh shared memory settings which will have been zapped by the restart */
	repmgrd_set_local_node_id(local_conn, config_file_options.node_id);
	repmgrd_set_pid(local_conn, getpid(), pid_file);

	/*
	 *
	 */

	if (standby_follow_result != 0)
	{
		monitoring_state = MS_DEGRADED;
		INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	/*
	 * update upstream_node_id to primary node (but only if follow command
	 * was successful)
	 */

	{
		if (update_node_record_set_upstream(primary_conn,
											local_node_info.node_id,
											primary_node_info.node_id) == false)
		{
			PQExpBufferData event_details;

			initPQExpBuffer(&event_details);
			appendPQExpBuffer(&event_details,
							  _("unable to set node %i's new upstream ID to %i"),
							  local_node_info.node_id,
							  primary_node_info.node_id);

			log_error("%s", event_details.data);

			create_event_notification(NULL,
									  &config_file_options,
									  local_node_info.node_id,
									  "repmgrd_failover_follow",
									  false,
									  event_details.data);

			termPQExpBuffer(&event_details);

			terminate(ERR_BAD_CONFIG);
		}
	}

	/* refresh own internal node record */
	record_status = get_node_record(primary_conn, local_node_info.node_id, &local_node_info);

	/*
	 * highly improbable this will happen, but in case we're unable to
	 * retrieve our node record from the primary, update it ourselves, and
	 * hope for the best
	 */
	if (record_status != RECORD_FOUND)
	{
		local_node_info.upstream_node_id = primary_node_info.node_id;
	}

	{
		PQExpBufferData event_details;

		initPQExpBuffer(&event_details);

		appendPQExpBuffer(&event_details,
						  _("node %i is now following primary node %i"),
						  local_node_info.node_id,
						  primary_node_info.node_id);

		log_notice("%s", event_details.data);

		create_event_notification(primary_conn,
								  &config_file_options,
								  local_node_info.node_id,
								  "repmgrd_failover_follow",
								  true,
								  event_details.data);

		termPQExpBuffer(&event_details);
	}

	/* keep the primary connection open */

	return true;
}


static FailoverState
promote_self(void)
{
	char	   *promote_command;
	int			r;

	/* Store details of the failed node here */
	t_node_info failed_primary = T_NODE_INFO_INITIALIZER;
	RecordStatus record_status;

	/*
	 * optionally add a delay before promoting the standby; this is mainly
	 * useful for testing (e.g. for reappearance of the original primary) and
	 * is not documented.
	 */
	if (config_file_options.promote_delay > 0)
	{
		log_debug("sleeping %i seconds before promoting standby",
				  config_file_options.promote_delay);
		sleep(config_file_options.promote_delay);
	}

	record_status = get_node_record(local_conn, local_node_info.upstream_node_id, &failed_primary);

	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve metadata record for failed upstream (ID: %i)"),
				  local_node_info.upstream_node_id);
		return FAILOVER_STATE_PROMOTION_FAILED;
	}

	/* the presence of this command has been established already */
	promote_command = config_file_options.promote_command;

	log_info(_("promote_command is:\n  \"%s\""),
			  promote_command);

	if (log_type == REPMGR_STDERR && *config_file_options.log_file)
	{
		fflush(stderr);
	}

	r = system(promote_command);

	/* connection should stay up, but check just in case */
	if (PQstatus(local_conn) != CONNECTION_OK)
	{
		log_warning(_("local database connection not available"));
		log_detail("\n%s", PQerrorMessage(local_conn));

		local_conn = establish_db_connection(local_node_info.conninfo, true);

		/* assume node failed */
		if (PQstatus(local_conn) != CONNECTION_OK)
		{
			log_error(_("unable to reconnect to local node"));
			log_detail("\n%s", PQerrorMessage(local_conn));
			/* XXX handle this */
			return FAILOVER_STATE_LOCAL_NODE_FAILURE;
		}
	}

	if (r != 0)
	{
		int			primary_node_id;

		upstream_conn = get_primary_connection(local_conn,
											   &primary_node_id,
											   NULL);

		if (PQstatus(upstream_conn) == CONNECTION_OK && primary_node_id == failed_primary.node_id)
		{
			PQExpBufferData event_details;

			log_notice(_("original primary (id: %i) reappeared before this standby was promoted - no action taken"),
					   failed_primary.node_id);

			initPQExpBuffer(&event_details);
			appendPQExpBuffer(&event_details,
							  _("original primary \"%s\" (node ID: %i) reappeared"),
							  failed_primary.node_name,
							  failed_primary.node_id);

			create_event_notification(upstream_conn,
									  &config_file_options,
									  local_node_info.node_id,
									  "repmgrd_failover_abort",
									  true,
									  event_details.data);

			termPQExpBuffer(&event_details);

			/* XXX handle this! */
			/* -> we'll need to let the other nodes know too.... */
			/* no failover occurred but we'll want to restart connections */

			return FAILOVER_STATE_PRIMARY_REAPPEARED;
		}


		log_error(_("promote command failed"));

		create_event_notification(NULL,
								  &config_file_options,
								  local_node_info.node_id,
								  "repmgrd_promote_error",
								  true,
								  "");

		return FAILOVER_STATE_PROMOTION_FAILED;
	}

	/* bump the electoral term */
	increment_current_term(local_conn);

	{
		PQExpBufferData event_details;

		/* update own internal node record */
		record_status = get_node_record(local_conn, local_node_info.node_id, &local_node_info);

		/*
		 * XXX here we're assuming the promote command updated metadata
		 */
		initPQExpBuffer(&event_details);

		appendPQExpBuffer(&event_details,
						  _("node %i promoted to primary; old primary %i marked as failed"),
						  local_node_info.node_id,
						  failed_primary.node_id);

		/* local_conn is now the primary connection */
		create_event_notification(local_conn,
								  &config_file_options,
								  local_node_info.node_id,
								  "repmgrd_failover_promote",
								  true,
								  event_details.data);

		termPQExpBuffer(&event_details);
	}

	return FAILOVER_STATE_PROMOTED;
}




/*
 * Notify follower nodes about which node to follow. Normally this
 * will be the current node, however if the original primary reappeared
 * before this node could be promoted, we'll inform the followers they
 * should resume monitoring the original primary.
 */
static void
notify_followers(NodeInfoList *standby_nodes, int follow_node_id)
{
	NodeInfoListCell *cell;

	log_info(_("%i followers to notify"),
			 standby_nodes->node_count);

	for (cell = standby_nodes->head; cell; cell = cell->next)
	{
		log_verbose(LOG_DEBUG, "intending to notify node %i...", cell->node_info->node_id);

		if (PQstatus(cell->node_info->conn) != CONNECTION_OK)
		{
			log_info(_("reconnecting to node \"%s\" (node ID: %i)..."),
					 cell->node_info->node_name,
					 cell->node_info->node_id);

			cell->node_info->conn = establish_db_connection(cell->node_info->conninfo, false);
		}

		if (PQstatus(cell->node_info->conn) != CONNECTION_OK)
		{
			log_warning(_("unable to reconnect to \"%s\" (node ID: %i)"),
						cell->node_info->node_name,
						cell->node_info->node_id);
			log_detail("\n%s", PQerrorMessage(cell->node_info->conn));

			continue;
		}

		if (follow_node_id == ELECTION_RERUN_NOTIFICATION)
		{
			log_notice(_("notifying node \"%s\" (node ID: %i) to rerun promotion candidate selection"),
					   cell->node_info->node_name,
					   cell->node_info->node_id);
		}
		else
		{
			log_notice(_("notifying node \"%s\" (node ID: %i) to follow node %i"),
					   cell->node_info->node_name,
					   cell->node_info->node_id,
					   follow_node_id);
		}
		notify_follow_primary(cell->node_info->conn, follow_node_id);
	}
}


static bool
wait_primary_notification(int *new_primary_id)
{
	int			i;

	for (i = 0; i < config_file_options.primary_notification_timeout; i++)
	{
		if (get_new_primary(local_conn, new_primary_id) == true)
		{
			log_debug("new primary is %i; elapsed: %i seconds",
					  *new_primary_id, i);
			return true;
		}

		log_verbose(LOG_DEBUG, "waiting for new primary notification, %i of max %i seconds (\"primary_notification_timeout\")",
					i, config_file_options.primary_notification_timeout);

		sleep(1);
	}

	log_warning(_("no notification received from new primary after %i seconds"),
				config_file_options.primary_notification_timeout);

	monitoring_state = MS_DEGRADED;
	INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

	return false;
}


static FailoverState
follow_new_primary(int new_primary_id)
{
	char		parsed_follow_command[MAXPGPATH] = "";
	int			i, r;

	/* Store details of the failed node here */
	t_node_info failed_primary = T_NODE_INFO_INITIALIZER;
	t_node_info new_primary = T_NODE_INFO_INITIALIZER;
	RecordStatus record_status = RECORD_NOT_FOUND;
	bool		new_primary_ok = false;

	log_verbose(LOG_DEBUG, "follow_new_primary(): new primary id is %i", new_primary_id);

	record_status = get_node_record(local_conn, new_primary_id, &new_primary);

	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve metadata record for new primary node (ID: %i)"),
				  new_primary_id);
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

    log_notice(_("attempting to follow new primary \"%s\" (node ID: %i)"),
                   new_primary.node_name,
                   new_primary_id);

	record_status = get_node_record(local_conn, local_node_info.upstream_node_id, &failed_primary);

	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve metadata record for failed primary (ID: %i)"),
				  local_node_info.upstream_node_id);
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	/* XXX check if new_primary_id == failed_primary.node_id? */

	if (log_type == REPMGR_STDERR && *config_file_options.log_file)
	{
		fflush(stderr);
	}

	upstream_conn = establish_db_connection(new_primary.conninfo, false);

	if (PQstatus(upstream_conn) == CONNECTION_OK)
	{
		RecoveryType primary_recovery_type = get_recovery_type(upstream_conn);

		if (primary_recovery_type == RECTYPE_PRIMARY)
		{
			new_primary_ok = true;
		}
		else
		{
			new_primary_ok = false;
			log_warning(_("new primary is not in recovery"));
			close_connection(&upstream_conn);
		}
	}

	if (new_primary_ok == false)
	{
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	/*
	 * disconnect from local node, as follow operation will result in a server
	 * restart
	 */

	close_connection(&local_conn);

	/*
	 * replace %n in "config_file_options.follow_command" with ID of primary
	 * to follow.
	 */
	parse_follow_command(parsed_follow_command, config_file_options.follow_command, new_primary_id);

	log_debug(_("standby follow command is:\n  \"%s\""),
			  parsed_follow_command);

	/* execute the follow command */
	r = system(parsed_follow_command);

	if (r != 0)
	{
		PGconn	   *old_primary_conn;

		/*
		 * The "standby follow" command could still fail due to the original primary
		 * reappearing before the candidate could promote itself ("repmgr
		 * standby follow" will refuse to promote another node if the primary
		 * is available). However the new primary will only instruct the other
		 * nodes to follow it after it's successfully promoted itself, so this
		 * case is highly unlikely. A slightly more likely scenario would
		 * be the new primary becoming unavailable just after it's sent notifications
		 * to its follower nodes, and the old primary becoming available again.
		 */
		old_primary_conn = establish_db_connection(failed_primary.conninfo, false);

		if (PQstatus(old_primary_conn) == CONNECTION_OK)
		{
			RecoveryType upstream_recovery_type = get_recovery_type(old_primary_conn);

			if (upstream_recovery_type == RECTYPE_PRIMARY)
			{
				PQExpBufferData event_details;

				initPQExpBuffer(&event_details);
				appendPQExpBufferStr(&event_details,
									 _("original primary reappeared - no action taken"));

				log_notice("%s", event_details.data);

				create_event_notification(old_primary_conn,
										  &config_file_options,
										  local_node_info.node_id,
										  "repmgrd_failover_aborted",
										  true,
										  event_details.data);

				termPQExpBuffer(&event_details);

				close_connection(&old_primary_conn);

				return FAILOVER_STATE_PRIMARY_REAPPEARED;
			}

			log_notice(_("original primary reappeared as standby"));

			close_connection(&old_primary_conn);
		}

		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	/*
	 * refresh local copy of local and primary node records - we get these
	 * directly from the primary to ensure they're the current version
	 */

	record_status = get_node_record(upstream_conn, new_primary_id, &upstream_node_info);

	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve metadata record found for node %i"),
				  new_primary_id);
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	record_status = get_node_record(upstream_conn, local_node_info.node_id, &local_node_info);
	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve metadata record found for node %i"),
				  local_node_info.node_id);
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	/*
	 * It's possible that the standby is still starting up after the "follow_command"
	 * completes, so poll for a while until we get a connection.
	 */

	for (i = 0; i < config_file_options.repmgrd_standby_startup_timeout; i++)
	{
		local_conn = establish_db_connection(local_node_info.conninfo, false);

		if (PQstatus(local_conn) == CONNECTION_OK)
			break;

		log_debug("sleeping 1 second; %i of %i attempts to reconnect to local node",
				  i + 1,
				  config_file_options.repmgrd_standby_startup_timeout);
		sleep(1);
	}

	if (PQstatus(local_conn) != CONNECTION_OK)
	{
		log_error(_("unable to reconnect to local node %i"),
				  local_node_info.node_id);
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	/* refresh shared memory settings which will have been zapped by the restart */
	repmgrd_set_local_node_id(local_conn, config_file_options.node_id);
	repmgrd_set_pid(local_conn, getpid(), pid_file);

	{
		PQExpBufferData event_details;

		initPQExpBuffer(&event_details);
		appendPQExpBuffer(&event_details,
						  _("node %i now following new upstream node %i"),
						  local_node_info.node_id,
						  upstream_node_info.node_id);

		log_notice("%s", event_details.data);

		create_event_notification(upstream_conn,
								  &config_file_options,
								  local_node_info.node_id,
								  "repmgrd_failover_follow",
								  true,
								  event_details.data);

		termPQExpBuffer(&event_details);
	}

	return FAILOVER_STATE_FOLLOWED_NEW_PRIMARY;
}


static FailoverState
witness_follow_new_primary(int new_primary_id)
{
	t_node_info new_primary = T_NODE_INFO_INITIALIZER;
	RecordStatus record_status = RECORD_NOT_FOUND;
	bool		new_primary_ok = false;

	record_status = get_node_record(local_conn, new_primary_id, &new_primary);

	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve metadata record for new primary node (ID: %i)"),
				  new_primary_id);
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	/* TODO: check if new_primary_id == failed_primary.node_id? */

	if (log_type == REPMGR_STDERR && *config_file_options.log_file)
	{
		fflush(stderr);
	}

	upstream_conn = establish_db_connection(new_primary.conninfo, false);

	if (PQstatus(upstream_conn) == CONNECTION_OK)
	{
		RecoveryType primary_recovery_type = get_recovery_type(upstream_conn);

		switch (primary_recovery_type)
		{
			case RECTYPE_PRIMARY:
				new_primary_ok = true;
				break;
			case RECTYPE_STANDBY:
				new_primary_ok = false;
				log_warning(_("new primary is not in recovery"));
				break;
			case RECTYPE_UNKNOWN:
				new_primary_ok = false;
				log_warning(_("unable to determine status of new primary"));
				break;
		}
	}

	if (new_primary_ok == false)
	{
		close_connection(&upstream_conn);

		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	/* set new upstream node ID on primary */
	update_node_record_set_upstream(upstream_conn, local_node_info.node_id, new_primary_id);

	witness_copy_node_records(upstream_conn, local_conn);

	/*
	 * refresh local copy of local and primary node records - we get these
	 * directly from the primary to ensure they're the current version
	 */

	record_status = get_node_record(upstream_conn, new_primary_id, &upstream_node_info);

	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve metadata record found for node %i"),
				  new_primary_id);
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	record_status = get_node_record(upstream_conn, local_node_info.node_id, &local_node_info);
	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve metadata record found for node %i"),
				  local_node_info.node_id);
		return FAILOVER_STATE_FOLLOW_FAIL;
	}

	{
		PQExpBufferData event_details;

		initPQExpBuffer(&event_details);
		appendPQExpBuffer(&event_details,
						  _("witness node %i now following new primary node %i"),
						  local_node_info.node_id,
						  upstream_node_info.node_id);

		log_notice("%s", event_details.data);

		create_event_notification(upstream_conn,
								  &config_file_options,
								  local_node_info.node_id,
								  "repmgrd_failover_follow",
								  true,
								  event_details.data);

		termPQExpBuffer(&event_details);
	}

	return FAILOVER_STATE_FOLLOWED_NEW_PRIMARY;
}


static const char *
_print_election_result(ElectionResult result)
{
	switch (result)
	{
		case ELECTION_NOT_CANDIDATE:
			return "NOT CANDIDATE";

		case ELECTION_WON:
			return "WON";

		case ELECTION_LOST:
			return "LOST";

		case ELECTION_CANCELLED:
			return "CANCELLED";

		case ELECTION_RERUN:
			return "RERUN";
	}

	/* should never reach here */
	return "UNKNOWN";
}


/*
 * Failover decision for nodes attached to the current primary.
 *
 * NB: this function sets "sibling_nodes"; caller (do_primary_failover)
 * expects to be able to read this list
 */
static ElectionResult
do_election(NodeInfoList *sibling_nodes, int *new_primary_id)
{
	int			electoral_term = -1;

	/* we're visible */
	int			visible_nodes = 1;
	int			total_nodes = 0;

	NodeInfoListCell *cell = NULL;

	t_node_info *candidate_node = NULL;

	ReplInfo	local_replication_info;

	/* To collate details of nodes with primary visible for logging purposes */
	PQExpBufferData nodes_with_primary_visible;

	/*
	 * Check if at least one server in the primary's location is visible; if
	 * not we'll assume a network split between this node and the primary
	 * location, and not promote any standby.
	 *
	 * NOTE: this function is only ever called by standbys attached to the
	 * current (unreachable) primary, so "upstream_node_info" will always
	 * contain the primary node record.
	 */
	bool		primary_location_seen = false;


	int			nodes_with_primary_still_visible = 0;

	electoral_term = get_current_term(local_conn);

	if (electoral_term == -1)
	{
		log_error(_("unable to determine electoral term"));

		return ELECTION_NOT_CANDIDATE;
	}

	log_debug("do_election(): electoral term is %i", electoral_term);

	if (config_file_options.failover == FAILOVER_MANUAL)
	{
		log_notice(_("this node is not configured for automatic failover so will not be considered as promotion candidate, and will not follow the new primary"));
		log_detail(_("\"failover\" is set to \"manual\" in repmgr.conf"));
		log_hint(_("manually execute \"repmgr standby follow\" to have this node follow the new primary"));

		return ELECTION_NOT_CANDIDATE;
	}

	/* node priority is set to zero - don't become a candidate, and lose by default */
	if (local_node_info.priority <= 0)
	{
		log_notice(_("this node's priority is %i so will not be considered as an automatic promotion candidate"),
				   local_node_info.priority);

		return ELECTION_LOST;
	}

	/* get all active nodes attached to upstream, excluding self */
	get_active_sibling_node_records(local_conn,
									local_node_info.node_id,
									upstream_node_info.node_id,
									sibling_nodes);

	total_nodes = sibling_nodes->node_count + 1;

	if (strncmp(upstream_node_info.location, local_node_info.location, MAXLEN) != 0)
	{
		log_info(_("primary node \"%s\" (ID: %i) has location \"%s\", this node's location is \"%s\""),
				 upstream_node_info.node_name,
				 upstream_node_info.node_id,
				 upstream_node_info.location,
				 local_node_info.location);
	}
	else
	{
		log_info(_("primary and this node have the same location (\"%s\")"),
				 local_node_info.location);
	}

	local_node_info.last_wal_receive_lsn = InvalidXLogRecPtr;

	/* fast path if no other standbys (or witness) exists - normally win by default */
	if (sibling_nodes->node_count == 0)
	{
		if (strncmp(upstream_node_info.location, local_node_info.location, MAXLEN) == 0)
		{
			if (config_file_options.failover_validation_command[0] != '\0')
			{
				return execute_failover_validation_command(&local_node_info);
			}

			log_info(_("no other sibling nodes - we win by default"));

			return ELECTION_WON;
		}
		else
		{
			/*
			 * If primary and standby have different locations set, the assumption
			 * is that no action should be taken as we can't tell whether there's
			 * been a network interruption or not.
			 *
			 * Normally a situation with primary and standby in different physical
			 * locations would be handled by leaving the location as "default" and
			 * setting up a witness server in the primary's location.
			 */
			log_debug("no other nodes, but primary and standby locations differ");

			monitoring_state = MS_DEGRADED;
			INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

			return ELECTION_NOT_CANDIDATE;
		}
	}
	else
	{
		/* standby nodes found - check if we're in the primary location before checking theirs */
		if (strncmp(upstream_node_info.location, local_node_info.location, MAXLEN) == 0)
		{
			primary_location_seen = true;
		}
	}

	/* get our lsn */
	if (get_replication_info(local_conn, STANDBY, &local_replication_info) == false)
	{
		log_error(_("unable to retrieve replication information for local node"));
		return ELECTION_LOST;
	}

	/* check if WAL replay on local node is paused */
	if (local_replication_info.wal_replay_paused == true)
	{
		log_debug("WAL replay is paused");
		if (local_replication_info.last_wal_receive_lsn > local_replication_info.last_wal_replay_lsn)
		{
			log_warning(_("WAL replay on this node is paused and WAL is pending replay"));
			log_detail(_("replay paused at %X/%X; last WAL received is %X/%X"),
					   format_lsn(local_replication_info.last_wal_replay_lsn),
					   format_lsn(local_replication_info.last_wal_receive_lsn));
		}

		/* attempt to resume WAL replay - unlikely this will fail, but just in case */
		if (resume_wal_replay(local_conn) == false)
		{
			log_error(_("unable to resume WAL replay"));
			log_detail(_("this node cannot be reliably promoted"));
			return ELECTION_LOST;
		}

		log_notice(_("WAL replay forcibly resumed"));
	}

	local_node_info.last_wal_receive_lsn = local_replication_info.last_wal_receive_lsn;

	log_info(_("local node's last receive lsn: %X/%X"), format_lsn(local_node_info.last_wal_receive_lsn));

	/* pointer to "winning" node, initially self */
	candidate_node = &local_node_info;

	initPQExpBuffer(&nodes_with_primary_visible);

	for (cell = sibling_nodes->head; cell; cell = cell->next)
	{
		ReplInfo	sibling_replication_info;

		/* assume the worst case */
		cell->node_info->node_status = NODE_STATUS_UNKNOWN;

		cell->node_info->conn = establish_db_connection(cell->node_info->conninfo, false);

		if (PQstatus(cell->node_info->conn) != CONNECTION_OK)
		{
			continue;
		}

		cell->node_info->node_status = NODE_STATUS_UP;

		visible_nodes++;

		/*
		 * see if the node is in the primary's location (but skip the check if
		 * we've seen a node there already)
		 */
		if (primary_location_seen == false)
		{
			if (strncmp(cell->node_info->location, upstream_node_info.location, MAXLEN) == 0)
			{
				primary_location_seen = true;
			}
		}

		/*
		 * check if repmgrd running - skip if not
		 *
		 * TODO: include pid query in replication info query?
		 *
		 * NOTE: from Pg12 we could execute "pg_promote()" from a running repmgrd;
		 * here we'll need to find a way of ensuring only one repmgrd does this
		 */
		if (repmgrd_get_pid(cell->node_info->conn) == UNKNOWN_PID)
		{
			log_warning(_("repmgrd not running on node \"%s\" (ID: %i), skipping"),
						cell->node_info->node_name,
						cell->node_info->node_id);
			continue;
		}

		if (get_replication_info(cell->node_info->conn, cell->node_info->type, &sibling_replication_info) == false)
		{
			log_warning(_("unable to retrieve replication information for node \"%s\" (ID: %i), skipping"),
						cell->node_info->node_name,
						cell->node_info->node_id);
			continue;
		}

		/*
		 * Check if node is not in recovery - it may have been promoted
		 * outside of the failover mechanism, in which case we may be able
		 * to follow it.
		 */

		if (sibling_replication_info.in_recovery == false)
		{
			bool can_follow;

			log_warning(_("node \"%s\" (ID: %i) is not in recovery"),
						cell->node_info->node_name,
						cell->node_info->node_id);

			can_follow = check_node_can_follow(local_conn,
											   local_node_info.last_wal_receive_lsn,
											   cell->node_info->conn,
											   cell->node_info);

			if (can_follow == true)
			{
				*new_primary_id = cell->node_info->node_id;
				termPQExpBuffer(&nodes_with_primary_visible);
				return ELECTION_CANCELLED;
			}

			/*
			 * Tricky situation here - we'll assume the node is a rogue primary
			 */
			log_warning(_("not possible to attach to node \"%s\" (ID: %i), ignoring"),
						cell->node_info->node_name,
						cell->node_info->node_id);
			continue;
		}

		/* check if WAL replay on node is paused */
		if (sibling_replication_info.wal_replay_paused == true)
		{
			/*
			 * Theoretically the repmgrd on the node should have resumed WAL play
			 * at this point.
			 */
			if (sibling_replication_info.last_wal_receive_lsn > sibling_replication_info.last_wal_replay_lsn)
			{
				log_warning(_("WAL replay on node \"%s\" (ID: %i) is paused and WAL is pending replay"),
							cell->node_info->node_name,
							cell->node_info->node_id);
			}
		}

		/*
		 * Check if node has seen primary "recently" - if so, we may have "partial primary visibility".
		 * For now we'll assume the primary is visible if it's been seen less than
		 * monitor_interval_secs * 2 seconds ago. We may need to adjust this, and/or make the value
		 * configurable.
		 */

		if (sibling_replication_info.upstream_last_seen >= 0 && sibling_replication_info.upstream_last_seen < (config_file_options.monitor_interval_secs * 2))
		{
			nodes_with_primary_still_visible++;
			log_notice(_("node %i last saw primary node %i second(s) ago, considering primary still visible"),
					   cell->node_info->node_id,
					   sibling_replication_info.upstream_last_seen);
			appendPQExpBuffer(&nodes_with_primary_visible,
							  " - node \"%s\" (ID: %i): %i second(s) ago\n",
							  cell->node_info->node_name,
							  cell->node_info->node_id,
							  sibling_replication_info.upstream_last_seen);
		}
		else
		{
			log_info(_("node %i last saw primary node %i second(s) ago"),
					 cell->node_info->node_id,
					 sibling_replication_info.upstream_last_seen);
		}


		/* don't interrogate a witness server */
		if (cell->node_info->type == WITNESS)
		{
			log_debug("node %i is witness, not querying state", cell->node_info->node_id);
			continue;
		}

		/* don't check 0-priority nodes */
		if (cell->node_info->priority <= 0)
		{
			log_info(_("node %i has priority of %i, skipping"),
					   cell->node_info->node_id,
					   cell->node_info->priority);
			continue;
		}


		/* get node's last receive LSN - if "higher" than current winner, current node is candidate */
		cell->node_info->last_wal_receive_lsn = sibling_replication_info.last_wal_receive_lsn;

		log_info(_("last receive LSN for sibling node \"%s\" (ID: %i) is: %X/%X"),
				 cell->node_info->node_name,
				 cell->node_info->node_id,
				 format_lsn(cell->node_info->last_wal_receive_lsn));

		/* compare LSN */
		if (cell->node_info->last_wal_receive_lsn > candidate_node->last_wal_receive_lsn)
		{
			/* other node is ahead */
			log_info(_("node \"%s\" (ID: %i) is ahead of current candidate \"%s\" (ID: %i)"),
					 cell->node_info->node_name,
					 cell->node_info->node_id,
					 candidate_node->node_name,
					 candidate_node->node_id);

			candidate_node = cell->node_info;
		}
		/* LSN is same - tiebreak on priority, then node_id */
		else if (cell->node_info->last_wal_receive_lsn == candidate_node->last_wal_receive_lsn)
		{
			log_info(_("node \"%s\" (ID: %i) has same LSN as current candidate \"%s\" (ID: %i)"),
					 cell->node_info->node_name,
					 cell->node_info->node_id,
					 candidate_node->node_name,
					 candidate_node->node_id);

			if (cell->node_info->priority > candidate_node->priority)
			{
				log_info(_("node \"%s\" (ID: %i) has higher priority (%i) than current candidate \"%s\" (ID: %i) (%i)"),
						 cell->node_info->node_name,
						 cell->node_info->node_id,
						 cell->node_info->priority,
						 candidate_node->node_name,
						 candidate_node->node_id,
						 candidate_node->priority);

				candidate_node = cell->node_info;
			}
			else if (cell->node_info->priority == candidate_node->priority)
			{
				if (cell->node_info->node_id < candidate_node->node_id)
				{
					log_info(_("node \"%s\" (ID: %i) has same priority but lower node_id than current candidate \"%s\" (ID: %i)"),
							 cell->node_info->node_name,
							 cell->node_info->node_id,
							 candidate_node->node_name,
							 candidate_node->node_id);

					candidate_node = cell->node_info;
				}
			}
			else
			{
				log_info(_("node \"%s\" (ID: %i) has lower priority (%i) than current candidate \"%s\" (ID: %i) (%i)"),
						 cell->node_info->node_name,
						 cell->node_info->node_id,
						 cell->node_info->priority,
						 candidate_node->node_name,
						 candidate_node->node_id,
						 candidate_node->priority);
			}
		}
	}

	if (primary_location_seen == false)
	{
		log_notice(_("no nodes from the primary location \"%s\" visible - assuming network split"),
				   upstream_node_info.location);
		log_detail(_("node will enter degraded monitoring state waiting for reconnect"));

		monitoring_state = MS_DEGRADED;
		INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

		reset_node_voting_status();

		termPQExpBuffer(&nodes_with_primary_visible);

		return ELECTION_CANCELLED;
	}

	if (nodes_with_primary_still_visible > 0)
	{
		log_info(_("%i nodes can see the primary"),
				   nodes_with_primary_still_visible);

		log_detail(_("following nodes can see the primary:\n%s"),
				   nodes_with_primary_visible.data);

		if (config_file_options.primary_visibility_consensus == true)
		{
			log_notice(_("cancelling failover as some nodes can still see the primary"));
			monitoring_state = MS_DEGRADED;
			INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

			reset_node_voting_status();

			termPQExpBuffer(&nodes_with_primary_visible);

			return ELECTION_CANCELLED;
		}
	}

	termPQExpBuffer(&nodes_with_primary_visible);

	log_info(_("visible nodes: %i; total nodes: %i; no nodes have seen the primary within the last %i seconds"),
			  visible_nodes,
			 total_nodes,
			 (config_file_options.monitor_interval_secs * 2));

	if (visible_nodes <= (total_nodes / 2.0))
	{
		log_notice(_("unable to reach a qualified majority of nodes"));
		log_detail(_("node will enter degraded monitoring state waiting for reconnect"));

		monitoring_state = MS_DEGRADED;
		INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

		reset_node_voting_status();

		return ELECTION_CANCELLED;
	}

	log_notice(_("promotion candidate is \"%s\" (ID: %i), last_received_lsn:%lu"),
			   candidate_node->node_name,
			   candidate_node->node_id,
               candidate_node->last_wal_receive_lsn);

	if (candidate_node->node_id == local_node_info.node_id)
	{
		/*
		 * If "failover_validation_command" is set, execute that command
		 * and decide the result based on the command's output
		 */

		if (config_file_options.failover_validation_command[0] != '\0')
		{
			return execute_failover_validation_command(candidate_node);
		}

		return ELECTION_WON;
	}

	return ELECTION_LOST;
}

/*
 * "failover" for the witness node; the witness has no part in the election
 * other than being reachable, so just needs to await notification from the
 * new primary
 */
static
bool do_witness_failover(void)
{
	int new_primary_id = UNKNOWN_NODE_ID;

	/* TODO add pre-event notification here */
	failover_state = FAILOVER_STATE_UNKNOWN;

	if (wait_primary_notification(&new_primary_id) == true)
	{
		/* if primary has reappeared, no action needed */
		if (new_primary_id == upstream_node_info.node_id)
		{
			failover_state = FAILOVER_STATE_FOLLOWING_ORIGINAL_PRIMARY;
		}
		else
		{
			failover_state = witness_follow_new_primary(new_primary_id);
		}
	}
	else
	{
		failover_state = FAILOVER_STATE_NO_NEW_PRIMARY;
	}


	log_verbose(LOG_DEBUG, "failover state is %s",
				format_failover_state(failover_state));

	switch (failover_state)
	{
		case FAILOVER_STATE_PRIMARY_REAPPEARED:
			/* pass control back down to start_monitoring() */
			log_info(_("resuming %s monitoring mode"),get_node_type_string(local_node_info.type));
			log_detail(_("original primary \"%s\" (node ID: %i) reappeared"),
					   upstream_node_info.node_name, upstream_node_info.node_id);

			failover_state = FAILOVER_STATE_NONE;
			return true;


		case FAILOVER_STATE_FOLLOWED_NEW_PRIMARY:
			log_info(_("resuming %s monitoring mode"),get_node_type_string(local_node_info.type));
			log_detail(_("following new primary \"%s\" (node id: %i)"),
					   upstream_node_info.node_name, upstream_node_info.node_id);
			failover_state = FAILOVER_STATE_NONE;

			return true;

		case FAILOVER_STATE_FOLLOWING_ORIGINAL_PRIMARY:
			log_info(_("resuming %s monitoring mode"),get_node_type_string(local_node_info.type));
			log_detail(_("following original primary \"%s\" (node id: %i)"),
					   upstream_node_info.node_name, upstream_node_info.node_id);
			failover_state = FAILOVER_STATE_NONE;

			return true;

		case FAILOVER_STATE_FOLLOW_FAIL:
			/*
			 * for whatever reason we were unable to follow the new primary -
			 * continue monitoring in degraded state
			 */
			monitoring_state = MS_DEGRADED;
			INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

			return false;

		default:
			monitoring_state = MS_DEGRADED;
			INSTR_TIME_SET_CURRENT(degraded_monitoring_start);

			return false;
	}
	/* should never reach here */
	return false;
}


static void
reset_node_voting_status(void)
{
	failover_state = FAILOVER_STATE_NONE;

	if (PQstatus(local_conn) != CONNECTION_OK)
	{
		log_error(_("reset_node_voting_status(): local_conn not set"));
		log_detail("\n%s", PQerrorMessage(local_conn));
		return;
	}
	reset_voting_status(local_conn);
}


static void
check_connection(t_node_info *node_info, PGconn **conn)
{
	if (is_server_available(node_info->conninfo) == false)
	{
		log_warning(_("connection to node \"%s\" (ID: %i) lost"),
					node_info->node_name,
					node_info->node_id);
		log_detail("\n%s", PQerrorMessage(*conn));
		PQfinish(*conn);
		*conn = NULL;
	}

	if (PQstatus(*conn) != CONNECTION_OK)
	{
		log_info(_("attempting to reconnect to node \"%s\" (ID: %i)"),
				 node_info->node_name,
				 node_info->node_id);
        PQfinish(*conn);
		*conn = establish_db_connection(node_info->conninfo, false);

		if (PQstatus(*conn) != CONNECTION_OK)
		{
            PQfinish(*conn);
			*conn = NULL;
			log_warning(_("reconnection to node \"%s\" (ID: %i) failed"),
						node_info->node_name,
						node_info->node_id);
		}
		else
		{
			int 		stored_local_node_id = UNKNOWN_NODE_ID;

			log_info(_("reconnected to node \"%s\" (ID: %i)"),
					 node_info->node_name,
					 node_info->node_id);

			stored_local_node_id = repmgrd_get_local_node_id(*conn);
			if (stored_local_node_id == UNKNOWN_NODE_ID)
			{
				repmgrd_set_local_node_id(*conn, config_file_options.node_id);
				repmgrd_set_pid(local_conn, getpid(), pid_file);
			}

		}
	}
}


static const char *
format_failover_state(FailoverState failover_state)
{
	switch(failover_state)
	{
		case FAILOVER_STATE_UNKNOWN:
			return "UNKNOWN";
		case FAILOVER_STATE_NONE:
			return "NONE";
		case FAILOVER_STATE_PROMOTED:
			return "PROMOTED";
		case FAILOVER_STATE_PROMOTION_FAILED:
			return "PROMOTION_FAILED";
		case FAILOVER_STATE_PRIMARY_REAPPEARED:
			return "PRIMARY_REAPPEARED";
		case FAILOVER_STATE_LOCAL_NODE_FAILURE:
			return "LOCAL_NODE_FAILURE";
		case FAILOVER_STATE_WAITING_NEW_PRIMARY:
			return "WAITING_NEW_PRIMARY";
		case FAILOVER_STATE_FOLLOW_NEW_PRIMARY:
			return "FOLLOW_NEW_PRIMARY";
		case FAILOVER_STATE_REQUIRES_MANUAL_FAILOVER:
			return "REQUIRES_MANUAL_FAILOVER";
		case FAILOVER_STATE_FOLLOWED_NEW_PRIMARY:
			return "FOLLOWED_NEW_PRIMARY";
		case FAILOVER_STATE_FOLLOWING_ORIGINAL_PRIMARY:
			return "FOLLOWING_ORIGINAL_PRIMARY";
		case FAILOVER_STATE_NO_NEW_PRIMARY:
			return "NO_NEW_PRIMARY";
		case FAILOVER_STATE_FOLLOW_FAIL:
			return "FOLLOW_FAIL";
		case FAILOVER_STATE_NODE_NOTIFICATION_ERROR:
			return "NODE_NOTIFICATION_ERROR";
		case FAILOVER_STATE_ELECTION_RERUN:
			return "ELECTION_RERUN";
	}

	/* should never reach here */
	return "UNKNOWN_FAILOVER_STATE";
}


static void
handle_sighup(PGconn **conn, t_server_type server_type)
{
	log_debug("SIGHUP received");

	if (reload_config(&config_file_options, server_type))
	{
		PQfinish(*conn);
		*conn = establish_db_connection(config_file_options.conninfo, true);
	}

	if (*config_file_options.log_file)
	{
		FILE	   *fd;

		log_debug("reopening %s", config_file_options.log_file);

		fd = freopen(config_file_options.log_file, "a", stderr);
		if (fd == NULL)
		{
			fprintf(stderr, "error reopening stderr to \"%s\": %s",
					config_file_options.log_file, strerror(errno));
		}
	}

	got_SIGHUP = false;
}

static ElectionResult
execute_failover_validation_command(t_node_info *node_info)
{
	PQExpBufferData failover_validation_command;
	PQExpBufferData command_output;
	int return_value = -1;

	initPQExpBuffer(&failover_validation_command);
	initPQExpBuffer(&command_output);

	parse_failover_validation_command(config_file_options.failover_validation_command,
									  node_info,
									  &failover_validation_command);

	log_notice(_("executing \"failover_validation_command\""));
	log_detail("%s", failover_validation_command.data);

	/* we determine success of the command by the value placed into return_value */
	(void) local_command_return_value(failover_validation_command.data,
									  &command_output,
									  &return_value);

	termPQExpBuffer(&failover_validation_command);

	if (command_output.data[0] != '\0')
	{
		log_info("output returned by failover validation command:\n%s", command_output.data);
	}
	else
	{
		log_info(_("no output returned from command"));
	}

	termPQExpBuffer(&command_output);

	if (return_value != 0)
	{
		/* create event here? */
		log_notice(_("failover validation command returned a non-zero value: %i"),
				   return_value);
		return ELECTION_RERUN;
	}

	log_notice(_("failover validation command returned zero"));

	return ELECTION_WON;
}


static void
parse_failover_validation_command(const char *template, t_node_info *node_info, PQExpBufferData *out)
{
	const char *src_ptr;

	for (src_ptr = template; *src_ptr; src_ptr++)
	{
		if (*src_ptr == '%')
		{
			switch (src_ptr[1])
			{
				case '%':
					/* %%: replace with % */
					src_ptr++;
					appendPQExpBufferChar(out, *src_ptr);
					break;
				case 'n':
					/* %n: node id */
					src_ptr++;
					appendPQExpBuffer(out, "%i", node_info->node_id);
					break;
				case 'a':
					/* %a: node name */
					src_ptr++;
					appendPQExpBufferStr(out, node_info->node_name);
					break;

				default:
					/* otherwise treat the % as not special */
					appendPQExpBufferChar(out, *src_ptr);

					break;
			}
		}
		else
		{
			appendPQExpBufferChar(out, *src_ptr);
		}
	}

	return;
}


/*
 * Sanity-check whether the local node can follow the proposed upstream node.
 *
 * Note this function is very similar to check_node_can_attach() in
 * repmgr-client.c, however the later is very focussed on client-side
 * functionality (including log output related to --dry-run, pg_rewind etc.)
 * which we don't want here.
 */
static bool
check_node_can_follow(PGconn *local_conn, XLogRecPtr local_xlogpos, PGconn *follow_target_conn, t_node_info *follow_target_node_info)
{
	t_conninfo_param_list local_repl_conninfo = T_CONNINFO_PARAM_LIST_INITIALIZER;
	PGconn	   *local_repl_conn = NULL;
	t_system_identification local_identification = T_SYSTEM_IDENTIFICATION_INITIALIZER;

	t_conninfo_param_list follow_target_repl_conninfo = T_CONNINFO_PARAM_LIST_INITIALIZER;
	PGconn	   *follow_target_repl_conn = NULL;
	t_system_identification follow_target_identification = T_SYSTEM_IDENTIFICATION_INITIALIZER;
	TimeLineHistoryEntry *follow_target_history = NULL;

	bool can_follow = true;
	bool success;

	/* Check local replication connection - we want to execute IDENTIFY_SYSTEM
	 * to get the current timeline ID, which might not yet be written to
	 * pg_control.
	 *
	 * TODO: from 9.6, query "pg_stat_wal_receiver" via the existing local connection
	 */

	initialize_conninfo_params(&local_repl_conninfo, false);

	conn_to_param_list(local_conn, &local_repl_conninfo);

	/* Set the replication user from the node record */
	param_set(&local_repl_conninfo, "user", local_node_info.repluser);
	param_set(&local_repl_conninfo, "replication", "1");

	local_repl_conn = establish_db_connection_by_params(&local_repl_conninfo, false);
	free_conninfo_params(&local_repl_conninfo);

	if (PQstatus(local_repl_conn) != CONNECTION_OK)
	{
		log_error(_("unable to establish a replication connection to the local node"));
		PQfinish(local_repl_conn);

		return false;
	}
	success = identify_system(local_repl_conn, &local_identification);
	PQfinish(local_repl_conn);

	if (success == false)
	{
		log_error(_("unable to query the local node's system identification"));

		return false;
	}

	/* check replication connection */
	initialize_conninfo_params(&follow_target_repl_conninfo, false);

	conn_to_param_list(follow_target_conn, &follow_target_repl_conninfo);

	if (strcmp(param_get(&follow_target_repl_conninfo, "user"), follow_target_node_info->repluser) != 0)
	{
		param_set(&follow_target_repl_conninfo, "user", follow_target_node_info->repluser);
		param_set(&follow_target_repl_conninfo, "dbname", "replication");
	}

	param_set(&follow_target_repl_conninfo, "replication", "1");

	follow_target_repl_conn = establish_db_connection_by_params(&follow_target_repl_conninfo, false);

	free_conninfo_params(&follow_target_repl_conninfo);

	if (PQstatus(follow_target_repl_conn) != CONNECTION_OK)
	{
		log_error(_("unable to establish a replication connection to the follow target node"));
		return false;
	}

	/* check system_identifiers match */
	if (identify_system(follow_target_repl_conn, &follow_target_identification) == false)
	{
		log_error(_("unable to query the follow target node's system identification"));

		PQfinish(follow_target_repl_conn);
		return false;
	}

	/*
	 * Check for thing that should never happen, but expect the unexpected anyway.
	 */
	if (follow_target_identification.system_identifier != local_identification.system_identifier)
	{
		log_error(_("this node is not part of the follow target node's replication cluster"));
		log_detail(_("this node's system identifier is %lu, follow target node's system identifier is %lu"),
				   local_identification.system_identifier,
				   follow_target_identification.system_identifier);
		PQfinish(follow_target_repl_conn);
		return false;
	}

	/* check timelines */

	log_verbose(LOG_DEBUG, "local timeline: %i; follow target timeline: %i",
				local_identification.timeline,
				follow_target_identification.timeline);

	/* upstream's timeline is lower than ours - impossible case */
	if (follow_target_identification.timeline < local_identification.timeline)
	{
		log_error(_("this node's timeline is ahead of the follow target node's timeline"));
		log_detail(_("this node's timeline is %i, follow target node's timeline is %i"),
				   local_identification.timeline,
				   follow_target_identification.timeline);
		PQfinish(follow_target_repl_conn);
		return false;
	}

	/* timeline is the same - check relative positions */
	if (follow_target_identification.timeline == local_identification.timeline)
	{
		XLogRecPtr follow_target_xlogpos = get_node_current_lsn(follow_target_conn);

		if (local_xlogpos == InvalidXLogRecPtr || follow_target_xlogpos == InvalidXLogRecPtr)
		{
			log_error(_("unable to compare LSN positions"));
			PQfinish(follow_target_repl_conn);
			return false;
		}

		if (local_xlogpos <= follow_target_xlogpos)
		{
			log_info(_("timelines are same, this server is not ahead"));
			log_detail(_("local node lsn is %X/%X, follow target lsn is %X/%X"),
					   format_lsn(local_xlogpos),
					   format_lsn(follow_target_xlogpos));
		}
		else
		{
			log_error(_("this node is ahead of the follow target"));
			log_detail(_("local node lsn is %X/%X, follow target lsn is %X/%X"),
					   format_lsn(local_xlogpos),
					   format_lsn(follow_target_xlogpos));

			can_follow = false;
		}
	}
	else
	{
		/*
		 * upstream has higher timeline - check where it forked off from this node's timeline
		 */
		follow_target_history = get_timeline_history(follow_target_repl_conn,
													 local_identification.timeline + 1);

		if (follow_target_history == NULL)
		{
			/* get_timeline_history() will emit relevant error messages */
			PQfinish(follow_target_repl_conn);
			return false;
		}

		log_debug("local tli: %i; local_xlogpos: %X/%X; follow_target_history->tli: %i; follow_target_history->end: %X/%X",
				  (int)local_identification.timeline,
				  format_lsn(local_xlogpos),
				  follow_target_history->tli,
				  format_lsn(follow_target_history->end));

		/*
		 * Local node has proceeded beyond the follow target's fork, so we
		 * definitely can't attach.
		 *
		 * This could be the case if the follow target was promoted, but does
		 * not contain all changes which are being replayed to this standby.
		 */
		if (local_xlogpos > follow_target_history->end)
		{
			log_error(_("this node cannot attach to follow target node %i"),
					  follow_target_node_info->node_id);
			can_follow = false;

			log_detail(_("follow target server's timeline %lu forked off current database system timeline %lu before current recovery point %X/%X"),
					   local_identification.system_identifier + 1,
					   local_identification.system_identifier,
					   format_lsn(local_xlogpos));
		}

		if (can_follow == true)
		{
			log_info(_("local node %i can attach to follow target node %i"),
					 config_file_options.node_id,
					 follow_target_node_info->node_id);

			log_detail(_("local node's recovery point: %X/%X; follow target node's fork point: %X/%X"),
					   format_lsn(local_xlogpos),
					   format_lsn(follow_target_history->end));
		}
	}

	PQfinish(follow_target_repl_conn);

	if (follow_target_history)
		pfree(follow_target_history);


	return can_follow;
}

/*
 * highgo:
 * check if the data directory is writable
 * if not, kill the db process
 */
static
void check_disk(void)
{
    PQExpBufferData disk_check_command_str;
    int	r = -1;
    int 	i;

    initPQExpBuffer(&disk_check_command_str);
    appendPQExpBuffer(&disk_check_command_str,
            "touch %s/hg_repmgr_test", config_file_options.data_directory);

    /* use SIGALRM to handle the touch cmd hunging case */
    signal(SIGALRM, signalAlarm);
    alarm(config_file_options.device_check_timeout);

    for(i = 0; i < config_file_options.device_check_times; i++)
    {
        r = system(disk_check_command_str.data);

//        if (config_file_options.log_switch)
//            log_notice("%d times try, result is %d... ", i + 1, r);

        if (r != 0)
        {
            alarm(0);
            sleep(config_file_options.device_check_timeout);
            alarm(config_file_options.device_check_timeout);
            continue;
        }
        else
            break;
    }
    termPQExpBuffer(&disk_check_command_str);

    if (0 == touch_label)
        alarm(0);
/*
    if (config_file_options.log_switch)
    {
        log_notice("timeout == %d, times == %d", config_file_options.device_check_timeout, config_file_options.device_check_times);
        log_notice("r == %d, touch_label == %d", r, touch_label);
    }
*/
    if (r != 0 || 1 == touch_label)
    {	
        PQExpBufferData stop_service_command_str;
        int rt;
        touch_label = 0;

        log_warning(_("PGDATA in which storage is not working"));			
        /*stop locale primary service*/
        initPQExpBuffer(&stop_service_command_str);
        appendPQExpBuffer(&stop_service_command_str,
                "ps -ef | grep postgres|grep -v grep |awk '{print  $2}'|xargs kill -9");

        rt = system(stop_service_command_str.data);

        log_notice("kill -9 postgres service result:%d\n", rt);

        if (rt ==0)
            log_warning(_("local HighGo Database server is stopped")); 

        termPQExpBuffer(&stop_service_command_str);
        PQfinish(local_conn);
        local_conn=NULL;
    }
    return;
}

/*
 * highgo: alarm called func
 */
static void 
signalAlarm(int signum)
{
	touch_label = 1;
}

/* 
 * check network card status
 * UP return 1, DOWN return 0
 * tianbing
 */
static bool
check_network_card_status(PGconn *conn, int node_id)
{
	FILE *pf = NULL;
	char    network_card[MAXLEN] = "";
	PQExpBufferData str;
	int value;

	if(NULL == conn)
	{
		log_notice(_("end check_network_card_status, conn is null"));
		return true;
	}

	if (get_network_card(conn, node_id, network_card))
	{
		initPQExpBuffer(&str);
		appendPQExpBuffer(&str, "/sys/class/net/%s/carrier", network_card);
        	
		pf = fopen(str.data, "r");
                termPQExpBuffer(&str);

		if(!pf)
		{
			log_warning(_("can not open file:%s"), str.data);
			return true;
		}

		value = fgetc(pf) - 48;
		
		fclose(pf);

		if (value == 1)
		{
			//if (config_file_options.log_switch)
	//			log_debug(_("end check_network_card_status, return true"));
			return true;
		}
		else
		{
			log_warning(_("end check network card,return false, value:%d, status is DOWN "), value);
			return false;
		}
		
	}
	else
	{
	//	if (config_file_options.log_switch)
	//		log_debug(_("didn't get network card, return true "));

		return true;
	}
}

/*
 * highgo: check servce status command
 * tianbing
 */
static bool
check_service_status_command(const char *command, PQExpBufferData *outputbuf)
{
    FILE       *fp = NULL;
    char            output[MAXLEN];
    int                     retval = 0;
    bool            success;

    log_verbose(LOG_DEBUG, "executing:\n  %s", command);

    if (outputbuf == NULL)
    {
        retval = system(command);
        return (retval == 0) ? true : false;

    }

    fp = popen(command, "r");

    if (fp == NULL)
    {
        log_error(_("unable to execute local command:\n%s"), command);
        return false;
    }

    while (fgets(output, MAXLEN, fp) != NULL)
    {
        appendPQExpBuffer(outputbuf, "%s", output);
        if (!feof(fp))
        {
            break;
        }
    }

    retval = pclose(fp);

    /*  */
    success = (WEXITSTATUS(retval) == 0 || WEXITSTATUS(retval) == 141) ? true : false;

    log_verbose(LOG_DEBUG, "result of command was %i (%i)", WEXITSTATUS(retval), retval);

    if (outputbuf->data != NULL)
        log_verbose(LOG_DEBUG, "local_command(): output returned was:\n%s", outputbuf->data);
    else
        log_verbose(LOG_DEBUG, "local_command(): no output returned");

    return success;
}


static NodeStatus
check_service_status_is_shutdown_cleanly(const char *node_status_output, XLogRecPtr *checkPoint)
{
    NodeStatus      node_status = NODE_STATUS_UNKNOWN;

    int                     c = 0,
                            argc_item = 0;
    char      **argv_array = NULL;
    int                     optindex = 0;

    /* We're only interested in these options */
    struct option node_status_options[] =
    {
        {"last-checkpoint-lsn", required_argument, NULL, 'L'},
        {"state", required_argument, NULL, 'S'},
        {NULL, 0, NULL, 0}
    };

    /* Don't attempt to tokenise an empty string */
    if (!strlen(node_status_output))
    {
        *checkPoint = InvalidXLogRecPtr;
        return node_status;
    }

    argc_item = parse_output_to_argv(node_status_output, &argv_array);

    /* Reset getopt's optind variable */
    optind = 0;

    /* Prevent getopt from emitting errors */
    opterr = 0;

    while ((c = getopt_long(argc_item, argv_array, "L:S:", node_status_options,
                    &optindex)) != -1)
    {
        switch (c)
        {
            /* --last-checkpoint-lsn */
            case 'L':
                *checkPoint = parse_lsn(optarg);
                break;
                /* --state */
            case 'S':
                {
                    if (strncmp(optarg, "RUNNING", MAXLEN) == 0)
                    {
                        node_status = NODE_STATUS_UP;
                    }
                    else if (strncmp(optarg, "SHUTDOWN", MAXLEN) == 0)
                    {
                        node_status = NODE_STATUS_DOWN;
                    }
                    else if (strncmp(optarg, "UNCLEAN_SHUTDOWN", MAXLEN) == 0)
                    {
                        node_status = NODE_STATUS_UNCLEAN_SHUTDOWN;
                    }
                    else if (strncmp(optarg, "UNKNOWN", MAXLEN) == 0)
                    {
                        node_status = NODE_STATUS_UNKNOWN;
                    }
                }
                break;
        }
    }

    free_parsed_argv(&argv_array);

    return node_status;
}
/*
 * highgo: function that auto exec 'node rejoin'
 * tianbing
 */
static void 
exec_node_rejoin_primary(NodeInfoList *my_node_list)
{
	NodeInfoListCell *mycell = NULL;
	PQExpBufferData node_rejoin_command_str;
    PQExpBufferData check_clean_shutdown_str;
	XLogRecPtr      checkpoint_lsn = InvalidXLogRecPtr;
	PQExpBufferData output_buf;
    bool    success;
	int 	r;

    log_debug("exec_node_rejoin_primary entered");
	/* check if the old primary is cleanly shutdown V2 -- tianbing */
    initPQExpBuffer(&output_buf);
	initPQExpBuffer(&check_clean_shutdown_str);
    appendPQExpBuffer(&check_clean_shutdown_str,
						"%s/repmgr node status --is-shutdown-cleanly;",
						config_file_options.pg_bindir);

	success = check_service_status_command(check_clean_shutdown_str.data, &output_buf);
    termPQExpBuffer(&check_clean_shutdown_str);

	if (success == true)
    {
        NodeStatus      status = check_service_status_is_shutdown_cleanly(output_buf.data, &checkpoint_lsn);
        if (status == NODE_STATUS_UNCLEAN_SHUTDOWN)
        {
            /*start and stop the service*/
            PQExpBufferData stop_service_command_str;
            log_notice("unclean shutdown detected, start and stop db to clean");

            initPQExpBuffer(&stop_service_command_str);
            appendPQExpBuffer(&stop_service_command_str,
                    "%s/pg_ctl -D %s start;",
                    config_file_options.pg_bindir, config_file_options.data_directory);
            appendPQExpBuffer(&stop_service_command_str,
                    "%s/pg_ctl -D %s stop",
                    config_file_options.pg_bindir, config_file_options.data_directory);

            system(stop_service_command_str.data);

            termPQExpBuffer(&stop_service_command_str);
        }
    }
	termPQExpBuffer(&output_buf);

	/*begin exec 'node rejoin' command*/
	for (mycell = my_node_list->head; mycell; mycell = mycell->next)
	{
		initPQExpBuffer(&node_rejoin_command_str);
		appendPQExpBuffer(&node_rejoin_command_str,
							"repmgr -d \'%s\' node rejoin --force-rewind",
							mycell->node_info->conninfo);

        log_debug("try repmgr -d %s node rejoin --force-rewind",mycell->node_info->conninfo);
		r = system(node_rejoin_command_str.data);
		if (r != 0)
		{
			termPQExpBuffer(&node_rejoin_command_str);
			continue;
		}
		else
		{
			termPQExpBuffer(&node_rejoin_command_str);
			break;

		}
	}
}

/*
 * highgo:
 * check if need to switch repliction mode 
 * between sync <-> async 
 */
static void
check_sync_async(NodeInfoList *my_node_list)
{
    char	sync_names[MAXLEN] = "";
    bool	one_sync_node = false;
    PQExpBufferData query;
    PGresult   *res = NULL;
    int 	records;
    PQExpBufferData command_conf;
    int unreachable_standby_elapsed;
    static bool switch_async_mode=false;
    static short unreachable_standby_counts = 0;

  //  log_debug("local node is reachable");

    /*
     * For one sync node, check if the sync standby is unreachable
     * tbing
     */

    initPQExpBuffer(&query);
    appendPQExpBuffer(&query, "SELECT count(*) from pg_stat_replication;");
    res = PQexec(local_conn, query.data);
    termPQExpBuffer(&query);

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
        log_error(_("unable to execute query\n"));

    records = atoi(PQgetvalue(res, 0, 0));

    if (records == 0)	// no records in pg_stat_replication
    {
        /*check that if there's only one sync standby*/
        get_pg_setting(local_conn, "synchronous_standby_names", sync_names);
        one_sync_node = only_one_sync_node(sync_names);

        if (one_sync_node)
        {
            /*sync standby is unreachable*/
            unreachable_standby_counts++;
            if (unreachable_standby_counts == 1)
                INSTR_TIME_SET_CURRENT(unreachable_sync_standby_start);

            unreachable_standby_elapsed = calculate_elapsed(unreachable_sync_standby_start);
            if (!switch_async_mode)
                log_notice(_("synchronous standby node has been unreached for %i seconds ..."), unreachable_standby_elapsed);

            if (unreachable_standby_elapsed > 30 && !switch_async_mode)
            {
                log_warning(_("synchronous standby node has been unreached in 30s timeout"));

                /*sync mode transform to async mode*/
                initPQExpBuffer(&command_conf);
                appendPQExpBuffer(&command_conf,
                        "sed -i 's/synchronous_standby_names/#synchronous_standby_names/g' %s/postgresql.conf",
                        config_file_options.data_directory);

                system(command_conf.data);
                termPQExpBuffer(&command_conf);

                system("pg_ctl reload");

                log_warning(_("synchronous mode has been transformed to asynchronous mode"));

                /*set lable that is in async mode*/
                switch_async_mode = true;
            }
        }
    }
    else
    {
        unreachable_standby_counts = 0;

        /*sync standby is reachable*/
        if (switch_async_mode)
        {
            NodeInfoListCell *mycell = NULL;
            XLogRecPtr      primary_last_wal_location = InvalidXLogRecPtr;
            XLogRecPtr      last_wal_receive_lsn;
            long long unsigned int  lag_bytes;

            primary_last_wal_location = get_primary_current_lsn(local_conn);

            for (mycell = my_node_list->head; mycell; mycell = mycell->next)
            {
                mycell->node_info->conn = establish_db_connection(mycell->node_info->conninfo, false);

                if (get_recovery_type(mycell->node_info->conn) != RECTYPE_PRIMARY)
                {
                    last_wal_receive_lsn = get_last_wal_receive_location(mycell->node_info->conn);
                    if (primary_last_wal_location != InvalidXLogRecPtr && primary_last_wal_location >= last_wal_receive_lsn)
                    {
                        lag_bytes = (long long unsigned int) (primary_last_wal_location - last_wal_receive_lsn);
                        log_notice(_("synchronous standby node's LSN is lag Primary for %lld MB  ..."), lag_bytes/1048576 );

                        if(lag_bytes <= 1024 * 1024 * 5)
                        {
                            log_warning(_("synchronous standby node's LSN is lag Primary node for 5 MB bound"));

                            /*async mode recover to sync mode*/
                            initPQExpBuffer(&command_conf);
                            appendPQExpBuffer(&command_conf,
                                    "sed -i 's/#synchronous_standby_names/synchronous_standby_names/g' %s/postgresql.conf",
                                    config_file_options.data_directory);
                            system(command_conf.data);
                            termPQExpBuffer(&command_conf);
                            system("pg_ctl reload");

                            /*set lable that is in sync mode*/
                            switch_async_mode = false;
                            log_warning(_("asynchronous mode has been recovery to synchronous mode"));
                        }
                    }
                }
                PQfinish(mycell->node_info->conn);
            }
        }
    }
    return;
}
/* 
 * check if there is only one sync node 
 * tianbing
 */
static bool 
only_one_sync_node(char *sync_names)
{
	int i, len;
	bool one_sync = true;
	bool r = false;

	/*
  	 * Follow example
 	 * synchronous_standby_names='s1, s2'
 	 * synchronous_standby_names='(s1, s2)'
 	 * synchronous_standby_names='1 (s1, s2)' 
 	 * synchronous_standby_names='FIRST (s1, s2)'
 	 * synchronous_standby_names='FIRST 1(s1, s2)' 
 	 * synchronous_standby_names='ANY 1(s1, s2)'
 	 */

	len = strlen(sync_names);

	if (len == 0)
		return false;

	for (i = 0; i <= len; i++)
	{
		if (sync_names[i] == '(')
			r = true;
		else
			continue;
	}

	for (i = 0; i <= len; i++)
	{
		if (!r)
			break;

		if (sync_names[i] == '(')
			break;

		if (sync_names[i] > '1' && sync_names[i] <= '9')
		{
			one_sync = false;
			break;
		}
		else
			continue;
	}

	return one_sync;
}

/**
 * As a primary node, check timely if any other nodes
 * are also runing as primary (brain split)
 * If brain split, need to stop the unexpected ones or
 * stop the whole cluster case by case
 */
static BS_ACTION
check_BS(NodeInfoList *my_node_list)
{
    NodeInfoListCell *mycell = NULL;
    int found_other_primary=0;

    for(mycell = my_node_list->head; mycell; mycell = mycell->next)
    {
        mycell->node_info->conn = establish_db_connection(mycell->node_info->conninfo, false);
        if((mycell->node_info->conn == NULL) || (PQstatus(mycell->node_info->conn) != CONNECTION_OK))
        {
            log_error("check_brain_split:unable to establish a connection to the %d",mycell->node_info->node_id);
            PQfinish(mycell->node_info->conn);
            mycell->node_info->conn=NULL;
            continue;
        }
        if((mycell->node_info->node_id == local_node_info.node_id) || (mycell->node_info->type == WITNESS))
        {
            continue;
        }
        if(get_recovery_type(mycell->node_info->conn) == RECTYPE_PRIMARY)
        {
            found_other_primary++;
        }
    }
    if(found_other_primary == 0)
    {
        log_debug("check_BS():did not found brain split");
        return DO_NOTHING;
    }
    else if(found_other_primary>1) //the cluster has more than 2 priamry nodes, stop the whole cluster
    {
        log_error("Brain split, more than 2 primary nodes were detacted. STOP");
        return DO_STOP;
    }
    else //the cluster has two primary nodes
    {
        bool found_peer = false;
        int remote_priority=0;
        int remote_node_id=0;
        PGconn *remote_conn=NULL;
        t_node_info peer_node_info=T_NODE_INFO_INITIALIZER;

        log_debug("found 2 primary nodes");

        for(mycell = my_node_list->head; mycell; mycell = mycell->next)
        {
            if((mycell->node_info->node_id == local_node_info.node_id) || (mycell->node_info->type == WITNESS))
            {
                continue;
            }
            if(get_recovery_type(mycell->node_info->conn) == RECTYPE_PRIMARY)
            {
                if(RECORD_FOUND==get_node_record(mycell->node_info->conn, mycell->node_info->node_id, &peer_node_info))
                {
                    found_peer=true;
                    remote_conn = mycell->node_info->conn;
                    remote_priority = mycell->node_info->priority;
                    remote_node_id = mycell->node_info->node_id;
                    log_error("found another primary node, id:%d",
                            mycell->node_info->node_id);
                    break;
                }
                else
                {
                    log_error("can not get the other primary node's record");
                    return DO_NOTHING;
                }
            }
        }
        if(found_peer)
        {
            TL_RET tli_ret=check_timeline(remote_conn,&peer_node_info);
            if(tli_ret==TL_LOW)
            {
                log_error("the primary nodes have the same last lsn, local timeline < another active nodeid %d",remote_node_id);
                return DO_REJOIN;
            }
            else if(tli_ret==TL_HIGH || tli_ret==TL_UNKNOWN)
            {
                log_error("the primary nodes have the same last lsn, local timeline > another active nodeid %d, do nothing",remote_node_id);
                return DO_NOTHING;
            }
            else
            {
                if(local_node_info.priority < remote_priority)
                {
                    log_debug("local priority < another active node, do rejoin");
                    return DO_REJOIN;
                }
                else if(local_node_info.priority > remote_priority)
                {
                    log_debug("local priority > another active node, keep in active");
                    return DO_NOTHING;
                }
                else
                {
                    log_debug("local priority == another active node, compare node id");
                    if(local_node_info.node_id < remote_node_id)
                    {
                        log_debug("local nodeid %d < another active node %d, keep in active.",local_node_info.node_id,remote_node_id);
                        return DO_NOTHING;
                    }
                    else
                    {
                        log_debug("local nodeid %d > another active node %d, do rejoin.",local_node_info.node_id,remote_node_id);
                        return DO_REJOIN;
                    }
                }
            }
        }
        else
        {
            return DO_NOTHING;
        }
    }
}

static TL_RET
check_timeline(PGconn *remote_conn, t_node_info *peer_node_info)
{
    TimeLineID local_tli;
    t_system_identification remote_identification = T_SYSTEM_IDENTIFICATION_INITIALIZER;
    t_conninfo_param_list follow_target_repl_conninfo = T_CONNINFO_PARAM_LIST_INITIALIZER;
    PGconn     *follow_target_repl_conn = NULL;

    initialize_conninfo_params(&follow_target_repl_conninfo, false);

    conn_to_param_list(remote_conn, &follow_target_repl_conninfo);

    if (strcmp(param_get(&follow_target_repl_conninfo, "user"), peer_node_info->repluser) != 0)
    {
        param_set(&follow_target_repl_conninfo, "user", peer_node_info->repluser);
        param_set(&follow_target_repl_conninfo, "dbname", "replication");
    }

    param_set(&follow_target_repl_conninfo, "replication", "1");

    follow_target_repl_conn = establish_db_connection_by_params(&follow_target_repl_conninfo, false);

    free_conninfo_params(&follow_target_repl_conninfo);

    if (PQstatus(follow_target_repl_conn) != CONNECTION_OK)
    {
        log_error(_("unable to establish a replication connection to the follow target node"));
        PQfinish(follow_target_repl_conn);
        return TL_UNKNOWN;
    }

    local_tli = get_timeline(config_file_options.data_directory);

    if(identify_system(follow_target_repl_conn, &remote_identification) == false)
    {
        log_error("unable to query remote active node system identification");
        PQfinish(follow_target_repl_conn);
        return TL_UNKNOWN;
    }

    PQfinish(follow_target_repl_conn);

    if(local_tli < remote_identification.timeline)
    {
        return TL_LOW;
    }
    else if(local_tli > remote_identification.timeline)
    {
        return TL_HIGH;
    }
    else
    {
        return TL_SAME;
    }
}
