/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#ifndef RPL_SLAVE_H
#define RPL_SLAVE_H

/**
  MASTER_DELAY can be at most (1 << 31) - 1.
*/
#define MASTER_DELAY_MAX (0x7FFFFFFF)
#if INT_MAX < 0x7FFFFFFF
#error "don't support platforms where INT_MAX < 0x7FFFFFFF"
#endif

/**
  @defgroup Replication Replication
  @{

  @file
*/

/** 
   Some of defines are need in parser even though replication is not 
   compiled in (embedded).
*/

/**
   The maximum is defined as (ULONG_MAX/1000) with 4 bytes ulong
*/
#define SLAVE_MAX_HEARTBEAT_PERIOD 4294967

#ifdef HAVE_REPLICATION

#include "log.h"
#include "binlog.h"
#include "my_list.h"
#include "rpl_filter.h"
#include "rpl_tblmap.h"

#define SLAVE_NET_TIMEOUT  3600

#define MAX_SLAVE_ERROR    2000

// Forward declarations
class Relay_log_info;
class Master_info;

extern bool server_id_supplied;

/*****************************************************************************

  MySQL Replication

  Replication is implemented via two types of threads:

    I/O Thread - One of these threads is started for each master server.
                 They maintain a connection to their master server, read log
                 events from the master as they arrive, and queues them into
                 a single, shared relay log file.  A Master_info 
                 represents each of these threads.

    SQL Thread - One of these threads is started and reads from the relay log
                 file, executing each event.  A Relay_log_info 
                 represents this thread.

  Buffering in the relay log file makes it unnecessary to reread events from
  a master server across a slave restart.  It also decouples the slave from
  the master where long-running updates and event logging are concerned--ie
  it can continue to log new events while a slow query executes on the slave.

*****************************************************************************/

/*
  MUTEXES in replication:

  LOCK_active_mi: [note: this was originally meant for multimaster, to switch
  from a master to another, to protect active_mi] It is used to SERIALIZE ALL
  administrative commands of replication: START SLAVE, STOP SLAVE, CHANGE
  MASTER, RESET SLAVE, end_slave() (when mysqld stops) [init_slave() does not
  need it it's called early]. Any of these commands holds the mutex from the
  start till the end. This thus protects us against a handful of deadlocks
  (consider start_slave_thread() which, when starting the I/O thread, releases
  mi->run_lock, keeps rli->run_lock, and tries to re-acquire mi->run_lock).

  Currently active_mi never moves (it's created at startup and deleted at
  shutdown, and not changed: it always points to the same Master_info struct),
  because we don't have multimaster. So for the moment, mi does not move, and
  mi->rli does not either.

  In Master_info: run_lock, data_lock
  run_lock protects all information about the run state: slave_running, thd
  and the existence of the I/O thread (to stop/start it, you need this mutex).
  data_lock protects some moving members of the struct: counters (log name,
  position) and relay log (MYSQL_BIN_LOG object).

  In Relay_log_info: run_lock, data_lock
  see Master_info
  However, note that run_lock does not protect
  Relay_log_info.run_state; that is protected by data_lock.
  
  Order of acquisition: if you want to have LOCK_active_mi and a run_lock, you
  must acquire LOCK_active_mi first.

  In MYSQL_BIN_LOG: LOCK_log, LOCK_index of the binlog and the relay log
  LOCK_log: when you write to it. LOCK_index: when you create/delete a binlog
  (so that you have to update the .index file).
*/

extern ulong master_retry_count;
extern MY_BITMAP slave_error_mask;
extern char slave_skip_error_names[];
extern bool use_slave_mask;
extern char *slave_load_tmpdir;
extern char *master_info_file, *relay_log_info_file;
extern char *opt_relay_logname, *opt_relaylog_index_name;
extern my_bool opt_skip_slave_start, opt_reckless_slave;
extern my_bool opt_log_slave_updates;
extern char *opt_slave_skip_errors;
extern ulonglong relay_log_space_limit;

/*
  3 possible values for Master_info::slave_running and
  Relay_log_info::slave_running.
  The values 0,1,2 are very important: to keep the diff small, I didn't
  substitute places where we use 0/1 with the newly defined symbols. So don't change
  these values.
  The same way, code is assuming that in Relay_log_info we use only values
  0/1.
  I started with using an enum, but
  enum_variable=1; is not legal so would have required many line changes.
*/
#define MYSQL_SLAVE_NOT_RUN         0
#define MYSQL_SLAVE_RUN_NOT_CONNECT 1
#define MYSQL_SLAVE_RUN_CONNECT     2

/*
  If the following is set, if first gives an error, second will be
  tried. Otherwise, if first fails, we fail.
*/
#define SLAVE_FORCE_ALL 4

int start_slave(THD* thd, Master_info* mi, bool net_report);
int stop_slave(THD* thd, Master_info* mi, bool net_report);
bool change_master(THD* thd, Master_info* mi);
int cmp_master_pos(const char* log_file_name1, ulonglong log_pos1,
		   const char* log_file_name2, ulonglong log_pos2);
int reset_slave(THD *thd, Master_info* mi);
int init_slave();
int init_recovery(Master_info* mi, const char** errmsg);
int init_info(Master_info* mi, bool ignore_if_no_info, int thread_mask);
void end_info(Master_info* mi);
int reset_info(Master_info* mi);
int flush_master_info(Master_info* mi, bool force);
void init_slave_skip_errors(const char* arg);
int register_slave_on_master(MYSQL* mysql);
int terminate_slave_threads(Master_info* mi, int thread_mask,
			     bool skip_lock = 0);
int start_slave_threads(bool need_slave_mutex, bool wait_for_start,
			Master_info* mi, int thread_mask);
/*
  cond_lock is usually same as start_lock. It is needed for the case when
  start_lock is 0 which happens if start_slave_thread() is called already
  inside the start_lock section, but at the same time we want a
  mysql_cond_wait() on start_cond, start_lock
*/
int start_slave_thread(
#ifdef HAVE_PSI_INTERFACE
                       PSI_thread_key thread_key,
#endif
                       pthread_handler h_func,
                       mysql_mutex_t *start_lock,
                       mysql_mutex_t *cond_lock,
                       mysql_cond_t *start_cond,
                       volatile uint *slave_running,
                       volatile ulong *slave_run_id,
                       Master_info *mi);

/* retrieve table from master and copy to slave*/
int fetch_master_table(THD* thd, const char* db_name, const char* table_name,
		       Master_info* mi, MYSQL* mysql, bool overwrite);

bool show_master_info(THD* thd, Master_info* mi);
bool show_binlog_info(THD* thd);
bool rpl_master_has_bug(const Relay_log_info *rli, uint bug_id, bool report,
                        bool (*pred)(const void *), const void *param);
bool rpl_master_erroneous_autoinc(THD* thd);

const char *print_slave_db_safe(const char *db);
void skip_load_data_infile(NET* net);

void end_slave(); /* release slave threads */
void close_active_mi(); /* clean up slave threads data */
void clear_until_condition(Relay_log_info* rli);
void clear_slave_error(Relay_log_info* rli);
void lock_slave_threads(Master_info* mi);
void unlock_slave_threads(Master_info* mi);
void init_thread_mask(int* mask,Master_info* mi,bool inverse);
void set_slave_thread_options(THD* thd);
void set_slave_thread_default_charset(THD *thd, Relay_log_info const *rli);
int apply_event_and_update_pos(Log_event* ev, THD* thd, Relay_log_info* rli);
void rotate_relay_log(Master_info* mi);

pthread_handler_t handle_slave_io(void *arg);
pthread_handler_t handle_slave_sql(void *arg);
bool net_request_file(NET* net, const char* fname);

extern bool volatile abort_loop;
extern Master_info *active_mi;      /* active_mi  for multi-master */
extern LIST master_list;
extern my_bool replicate_same_server_id;

extern int disconnect_slave_event_count, abort_slave_event_count ;

/* the master variables are defaults read from my.cnf or command line */
extern uint master_port, master_connect_retry, report_port;
extern char * master_user, *master_password, *master_host;
extern char *master_info_file, *relay_log_info_file, *report_user;
extern char *report_host, *report_password;

extern my_bool master_ssl;
extern char *master_ssl_ca, *master_ssl_capath, *master_ssl_cert;
extern char *master_ssl_cipher, *master_ssl_key;
       
extern I_List<THD> threads;

#endif /* HAVE_REPLICATION */

/* masks for start/stop operations on io and sql slave threads */
#define SLAVE_IO  1
#define SLAVE_SQL 2

/**
  @} (end of group Replication)
*/
#endif