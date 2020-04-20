/*
 * repmgr-action-daemon.h
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

#ifndef _REPMGR_ACTION_DAEMON_H_
#define _REPMGR_ACTION_DAEMON_H_


extern void do_daemon_status(void);
extern void do_daemon_pause(void);
extern void do_daemon_unpause(void);
extern void do_daemon_start(void);
extern void do_daemon_stop(void);

extern void do_daemon_help(void);
#endif
