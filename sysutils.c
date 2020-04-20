/*
 * sysutils.c
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
 */

#include <signal.h>

#include "repmgr.h"

static bool _local_command(const char *command, PQExpBufferData *outputbuf, bool simple, int *return_value);


/*
 * Execute a command locally. "outputbuf" should either be an
 * initialised PQExpPuffer, or NULL
 */
bool
local_command(const char *command, PQExpBufferData *outputbuf)
{
	return _local_command(command, outputbuf, false, NULL);
}

bool
local_command_return_value(const char *command, PQExpBufferData *outputbuf, int *return_value)
{
	return _local_command(command, outputbuf, false, return_value);
}


bool
local_command_simple(const char *command, PQExpBufferData *outputbuf)
{
	return _local_command(command, outputbuf, true, NULL);
}


static bool
_local_command(const char *command, PQExpBufferData *outputbuf, bool simple, int *return_value)
{
	FILE	   *fp = NULL;
	char		output[MAXLEN];
	int			retval = 0;
	bool		success;

	log_verbose(LOG_DEBUG, "executing:\n  %s", command);

	if (outputbuf == NULL)
	{
		retval = system(command);

		if (return_value != NULL)
			*return_value = WEXITSTATUS(retval);

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
		appendPQExpBufferStr(outputbuf, output);

		if (!feof(fp) && simple == false)
		{
			break;
		}
	}

	retval = pclose(fp);

	/*  */
	success = (WEXITSTATUS(retval) == 0 || WEXITSTATUS(retval) == 141) ? true : false;

	log_verbose(LOG_DEBUG, "result of command was %i (%i)", WEXITSTATUS(retval), retval);

	if (return_value != NULL)
		*return_value = WEXITSTATUS(retval);

	if (outputbuf->data != NULL && outputbuf->data[0] != '\0')
		log_verbose(LOG_DEBUG, "local_command(): output returned was:\n%s", outputbuf->data);
	else
		log_verbose(LOG_DEBUG, "local_command(): no output returned");

	return success;
}


/*
 * Execute a command via ssh on the remote host.
 *
 * TODO: implement SSH calls using libssh2.
 */
bool
remote_command(const char *host, const char *user, const char *command, const char *ssh_options, PQExpBufferData *outputbuf)
{
	FILE	   *fp;
	PQExpBufferData ssh_command;
	PQExpBufferData ssh_host;

	char		output[MAXLEN] = "";

	initPQExpBuffer(&ssh_host);

	if (*user != '\0')
	{
		appendPQExpBuffer(&ssh_host, "%s@", user);
	}

	appendPQExpBufferStr(&ssh_host, host);

    initPQExpBuffer(&ssh_command);
    appendPQExpBuffer(&ssh_command,
                     "ssh -o Batchmode=yes %s %s %s",
                     ssh_options,
                     ssh_host.data,
                     command);

	termPQExpBuffer(&ssh_host);

	log_debug("remote_command():\n  %s", ssh_command.data);

	fp = popen(ssh_command.data, "r");

	if (fp == NULL)
	{
		log_error(_("unable to execute remote command:\n  %s"), ssh_command.data);
        termPQExpBuffer(&ssh_command);
		return false;
	}

    termPQExpBuffer(&ssh_command);

	if (outputbuf != NULL)
	{
		/* TODO: better error handling */
		while (fgets(output, MAXLEN, fp) != NULL)
		{
			appendPQExpBufferStr(outputbuf, output);
		}
	}
	else
	{
		while (fgets(output, MAXLEN, fp) != NULL)
		{
			if (!feof(fp))
			{
				break;
			}
		}
	}

	pclose(fp);

	if (outputbuf != NULL)
	{
		if (outputbuf->data != NULL && outputbuf->data[0] != '\0')
			log_verbose(LOG_DEBUG, "remote_command(): output returned was:\n%s", outputbuf->data);
		else
			log_verbose(LOG_DEBUG, "remote_command(): no output returned");
	}

	return true;
}


pid_t
disable_wal_receiver(PGconn *conn)
{
	char buf[MAXLEN];
	int wal_retrieve_retry_interval, new_wal_retrieve_retry_interval;
	pid_t wal_receiver_pid = UNKNOWN_PID;
	int kill_ret;
	int i, j;
	int max_retries = 2;

	if (is_superuser_connection(conn, NULL) == false)
	{
		log_error(_("superuser connection required"));
		return UNKNOWN_PID;
	}

	if (get_recovery_type(conn) == RECTYPE_PRIMARY)
	{
		log_error(_("node is not in recovery"));
		log_detail(_("wal receiver can only run on standby nodes"));
		return UNKNOWN_PID;
	}

	wal_receiver_pid = (pid_t)get_wal_receiver_pid(conn);

	if (wal_receiver_pid == UNKNOWN_PID)
	{
		log_warning(_("unable to retrieve wal receiver PID"));
		return UNKNOWN_PID;
	}

	get_pg_setting(conn, "wal_retrieve_retry_interval", buf);

	/* TODO: potentially handle atoi error, though unlikely at this point */
	wal_retrieve_retry_interval = atoi(buf);

	new_wal_retrieve_retry_interval = wal_retrieve_retry_interval + WALRECEIVER_DISABLE_TIMEOUT_VALUE;

	if (wal_retrieve_retry_interval < WALRECEIVER_DISABLE_TIMEOUT_VALUE)
	{
		log_notice(_("setting \"wal_retrieve_retry_interval\" to %i milliseconds"),
				   new_wal_retrieve_retry_interval);
		alter_system_int(conn, "wal_retrieve_retry_interval", new_wal_retrieve_retry_interval);
		pg_reload_conf(conn);
	}

	/*
	 * If, at this point, the WAL receiver is not running, we don't need to (and indeed can't)
	 * kill it.
	 */
	if (wal_receiver_pid == 0)
	{
		log_warning(_("wal receiver not running"));
		return UNKNOWN_PID;
	}


	/* why 5? */
	log_info(_("sleeping 5 seconds"));
	sleep(5);

	/* see comment below as to why we need a loop here */
	for (i = 0; i < max_retries; i++)
	{
		log_notice(_("killing WAL receiver with PID %i"), (int)wal_receiver_pid);

		kill((int)wal_receiver_pid, SIGTERM);

		for (j = 0; j < 30; j++)
		{
			kill_ret = kill(wal_receiver_pid, 0);

			if (kill_ret != 0)
			{
				log_info(_("WAL receiver with pid %i killed"), (int)wal_receiver_pid);
				break;
			}
			sleep(1);
		}

		/*
		 * Wait briefly to check that the WAL receiver has indeed gone away -
		 * for reasons as yet unclear, after a server start/restart, immediately
		 * after the first time a WAL receiver is killed, a new one is started
		 * straight away, so we'll need to kill that too.
		 */
		sleep(1);
		wal_receiver_pid = (pid_t)get_wal_receiver_pid(conn);
		if (wal_receiver_pid == UNKNOWN_PID || wal_receiver_pid == 0)
			break;
	}

	return wal_receiver_pid;
}

pid_t
enable_wal_receiver(PGconn *conn, bool wait_startup)
{
	char buf[MAXLEN];
	int wal_retrieve_retry_interval;
	pid_t wal_receiver_pid = UNKNOWN_PID;

	/* make timeout configurable */
	int i, timeout = 30;

	if (is_superuser_connection(conn, NULL) == false)
	{
		log_error(_("superuser connection required"));
		return UNKNOWN_PID;
	}

	if (get_recovery_type(conn) == RECTYPE_PRIMARY)
	{
		log_error(_("node is not in recovery"));
		log_detail(_("wal receiver can only run on standby nodes"));
		return UNKNOWN_PID;
	}

	if (get_pg_setting(conn, "wal_retrieve_retry_interval", buf) == false)
	{
		log_error(_("unable to retrieve \"wal_retrieve_retry_interval\""));
		return UNKNOWN_PID;
	}

	/* TODO: potentially handle atoi error, though unlikely at this point */
	wal_retrieve_retry_interval = atoi(buf);

	if (wal_retrieve_retry_interval > WALRECEIVER_DISABLE_TIMEOUT_VALUE)
	{
		int new_wal_retrieve_retry_interval = wal_retrieve_retry_interval - WALRECEIVER_DISABLE_TIMEOUT_VALUE;
		bool success;

		log_notice(_("setting \"wal_retrieve_retry_interval\" to %i ms"),
				   new_wal_retrieve_retry_interval);

		success = alter_system_int(conn,
								   "wal_retrieve_retry_interval",
								   new_wal_retrieve_retry_interval);

		if (success == false)
		{
			log_warning(_("unable to change \"wal_retrieve_retry_interval\""));
			return UNKNOWN_PID;
		}

		pg_reload_conf(conn);
	}
	else
	{
		/* TODO: add threshold sanity check */
		log_info(_("\"wal_retrieve_retry_interval\" is %i, not changing"),
				 wal_retrieve_retry_interval);
	}

	if (wait_startup == false)
		return UNKNOWN_PID;

	for (i = 0; i < timeout; i++)
	{
		wal_receiver_pid = (pid_t)get_wal_receiver_pid(conn);

		if (wal_receiver_pid > 0)
			break;

		log_info(_("sleeping %i of maximum %i seconds waiting for WAL receiver to start up"),
				 i + 1, timeout)
		sleep(1);
	}

	if (wal_receiver_pid == UNKNOWN_PID)
	{
		log_warning(_("unable to retrieve WAL receiver PID"));
		return UNKNOWN_PID;
	}
	else if (wal_receiver_pid == 0)
	{
		log_error(_("WAL receiver did not start up after %i seconds"), timeout);
		return UNKNOWN_PID;
	}

	log_info(_("WAL receiver started up with PID %i"), (int)wal_receiver_pid);

	return wal_receiver_pid;
}
