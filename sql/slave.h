#ifndef SLAVE_H
#define SLAVE_H

#include "mysql.h"
#include "my_list.h"
#define SLAVE_NET_TIMEOUT  3600
#define MAX_SLAVE_ERRMSG   1024
#define MAX_SLAVE_ERROR    2000

/*
  The replication is accomplished by starting two threads - I/O
  thread, and SQL thread. I/O thread is associated with its
  MASTER_INFO struct, so MASTER_INFO can be viewed as I/O thread
  descriptor. SQL thread is associated with RELAY_LOG_INFO struct.

  I/O thread reads maintains a connection to the master, and reads log
  events from the master as they arrive, queueing them by writing them
  out into the temporary slave binary log (relay log). The SQL thread,
  in turn, reads the slave binary log executing each event.

  Relay log is needed to be able to handle situations when there is a large
  backlog of unprocessed events from the master (eg. one particular update
  takes a day to finish), and to be able to restart the slave server without
  having to re-read the master updates.
 */

extern ulong slave_net_timeout, master_retry_count;
extern MY_BITMAP slave_error_mask;
extern bool use_slave_mask;
extern char* slave_load_tmpdir;
extern my_string master_info_file,relay_log_info_file;
extern my_string opt_relay_logname, opt_relaylog_index_name;
extern bool opt_skip_slave_start, opt_reckless_slave;
extern ulong relay_log_space_limit;
struct st_master_info;

/*
  TODO: this needs to be redone, but for now it does not matter since
  we do not have multi-master yet.
*/

#define LOCK_ACTIVE_MI { pthread_mutex_lock(&LOCK_active_mi); \
 ++active_mi_in_use; \
 pthread_mutex_unlock(&LOCK_active_mi);}

#define UNLOCK_ACTIVE_MI { pthread_mutex_lock(&LOCK_active_mi); \
 --active_mi_in_use; \
 pthread_mutex_unlock(&LOCK_active_mi); }

/*
  st_relay_log_info contains information on the current relay log and
  relay log offset, and master log name and log sequence corresponding to the
  last update. Additionally, misc information specific to the SQL thread is
  included.

  st_relay_log_info is initialized from the slave.info file if such exists.
  Otherwise, data members are intialized with defaults. The initialization is
  done with init_relay_log_info() call.

  The format of slave.info file:

  relay_log_name
  relay_log_pos
  master_log_name
  master_log_pos

  To clean up, call end_relay_log_info()
 */

typedef struct st_relay_log_info
{
  /*** The following variables can only be read when protect by data lock ****/
  /*
    info_fd - file descriptor of the info file. set only during
    initialization or clean up - safe to read anytime
    cur_log_fd - file descriptor of the current read  relay log
  */
  File info_fd,cur_log_fd;
  // name of current read relay log
  char relay_log_name[FN_REFLEN];
  // master log name corresponding to current read position
  char master_log_name[FN_REFLEN];
  // original log position of last processed event
  volatile my_off_t master_log_pos;

  /*
    current offset in the relay log.
    pending - in some cases we do not increment offset immediately after
    processing an event, because the following event needs to be processed
    atomically together with this one ( so far, there is only one type of
    such event - Intvar_event that sets auto_increment value). However, once
    both events have been processed, we need to increment by the cumulative
    offset. pending stored the extra offset to be added to the position.
  */
  ulonglong relay_log_pos, pending;

  // protected with internal locks
  // must get data_lock when resetting the logs
  MYSQL_LOG relay_log;
  LOG_INFO linfo;
  IO_CACHE cache_buf,*cur_log;

  /*** The following variables are safe to read any time ***/

  // IO_CACHE of the info file - set only during init or end
  IO_CACHE info_file;

  /*
    When we restart slave thread we need to have access to the previously
    created temporary tables. Modified only on init/end and by the SQL
    thread, read only by SQL thread.
  */
  TABLE* save_temporary_tables;

  /*
    standard lock acquistion order to avoid deadlocks:
    run_lock, data_lock, relay_log.LOCK_log, relay_log.LOCK_index
  */
  pthread_mutex_t data_lock,run_lock;

  /*
    start_cond is broadcast when SQL thread is started
    stop_cond - when stopped
    data_cond - when data protected by data_lock changes
  */
  pthread_cond_t start_cond, stop_cond, data_cond;

  // if not set, the value of other members of the structure are undefined
  bool inited;

  // parent master info structure
  struct st_master_info *mi;

  /*
    Needed to deal properly with cur_log getting closed and re-opened with
    a different log under our feet
  */
  int cur_log_init_count;
  
  volatile bool abort_slave, slave_running;

  /*
    Needed for problems when slave stops and we want to restart it
    skipping one or more events in the master log that have caused
    errors, and have been manually applied by DBA already.
  */
  volatile uint32 slave_skip_counter;
#ifndef DBUG_OFF
  int events_till_abort;
#endif  
  int last_slave_errno;
  char last_slave_error[MAX_SLAVE_ERRMSG];
  THD* sql_thd;
  bool log_pos_current;
  bool abort_pos_wait;
  bool skip_log_purge;
  ulonglong log_space_limit,log_space_total;
  pthread_mutex_t log_space_lock;
  pthread_cond_t log_space_cond;
  
  st_relay_log_info():info_fd(-1),cur_log_fd(-1),inited(0),
		      cur_log_init_count(0),
		      abort_slave(0),slave_running(0),
		      log_pos_current(0),abort_pos_wait(0),
		      skip_log_purge(0)
    {
      relay_log_name[0] = master_log_name[0] = 0;
      bzero(&info_file,sizeof(info_file));
      pthread_mutex_init(&run_lock, MY_MUTEX_INIT_FAST);
      pthread_mutex_init(&data_lock, MY_MUTEX_INIT_FAST);
      pthread_mutex_init(&log_space_lock, MY_MUTEX_INIT_FAST);
      pthread_cond_init(&data_cond, NULL);
      pthread_cond_init(&start_cond, NULL);
      pthread_cond_init(&stop_cond, NULL);
      pthread_cond_init(&log_space_cond, NULL);
    }
  ~st_relay_log_info()
    {
      pthread_mutex_destroy(&run_lock);
      pthread_mutex_destroy(&data_lock);
      pthread_mutex_destroy(&log_space_lock);
      pthread_cond_destroy(&data_cond);
      pthread_cond_destroy(&start_cond);
      pthread_cond_destroy(&stop_cond);
      pthread_cond_destroy(&log_space_cond);
    }
  inline void inc_pending(ulonglong val)
  {
    pending += val;
  }
  // TODO: this probably needs to be fixed
  inline void inc_pos(ulonglong val, ulonglong log_pos, bool skip_lock=0)
  {
    if (!skip_lock)
      pthread_mutex_lock(&data_lock);
    relay_log_pos += val+pending;
    pending = 0;
    if (log_pos)
      master_log_pos = log_pos+ val;
    pthread_cond_broadcast(&data_cond);
    if (!skip_lock)
      pthread_mutex_unlock(&data_lock);
  }
  /*
    thread safe read of position - not needed if we are in the slave thread,
    but required otherwise
  */
  inline void read_pos(ulonglong& var)
  {
    pthread_mutex_lock(&data_lock);
    var = relay_log_pos;
    pthread_mutex_unlock(&data_lock);
  }

  int wait_for_pos(THD* thd, String* log_name, ulonglong log_pos);
} RELAY_LOG_INFO;

/*
  repopen_relay_log() is called when we notice that the current "hot" log
  got rotated under our feet
*/

IO_CACHE* reopen_relay_log(RELAY_LOG_INFO* rli, const char** errmsg);
Log_event* next_event(RELAY_LOG_INFO* rli);


/*
  st_master_info contains information about how to connect to a master,
   current master log name, and current log offset, as well as misc
   control variables

   st_master_info is initialized once from the master.info file if such
   exists. Otherwise, data members corresponding to master.info fields are
   initialized with defaults specified by master-* options. The initialization
   is done through init_master_info() call.

   The format of master.info file:

   log_name
   log_pos
   master_host
   master_user
   master_pass
   master_port
   master_connect_retry

   To write out the contents of master.info file to disk ( needed every
   time we read and queue data from the master ), a call to
   flush_master_info() is required.

   To clean up, call end_master_info()
*/

   
typedef struct st_master_info
{
  char master_log_name[FN_REFLEN];
  
  my_off_t master_log_pos;
  File fd; 
  IO_CACHE file;
  
  // the variables below are needed because we can change masters on the fly
  char host[HOSTNAME_LENGTH+1];
  char user[USERNAME_LENGTH+1];
  char password[HASH_PASSWORD_LENGTH+1];
  pthread_mutex_t data_lock,run_lock;
  pthread_cond_t data_cond,start_cond,stop_cond;
  THD *io_thd;
  MYSQL* mysql;
  uint32 file_id; // for 3.23 load data infile
  RELAY_LOG_INFO rli;
  uint port;
  uint connect_retry;
#ifndef DBUG_OFF
  int events_till_abort;
#endif
  bool inited;
  bool old_format;			// master binlog is in 3.23 format
  volatile bool abort_slave, slave_running;
  bool ignore_stop_event;
  
  
  st_master_info():fd(-1), io_thd(0), inited(0), old_format(0),abort_slave(0),
		   slave_running(0)
  {
    host[0] = 0; user[0] = 0; password[0] = 0;
    bzero(&file,sizeof(file));
    pthread_mutex_init(&run_lock, MY_MUTEX_INIT_FAST);
    pthread_mutex_init(&data_lock, MY_MUTEX_INIT_FAST);
    pthread_cond_init(&data_cond, NULL);
    pthread_cond_init(&start_cond, NULL);
    pthread_cond_init(&stop_cond, NULL);
  }

  ~st_master_info()
  {
    pthread_mutex_destroy(&run_lock);
    pthread_mutex_destroy(&data_lock);
    pthread_cond_destroy(&data_cond);
    pthread_cond_destroy(&start_cond);
    pthread_cond_destroy(&stop_cond);
  }

} MASTER_INFO;

int queue_event(MASTER_INFO* mi,const char* buf,ulong event_len);

typedef struct st_table_rule_ent
{
  char* db;
  char* tbl_name;
  uint key_len;
} TABLE_RULE_ENT;

#define TABLE_RULE_HASH_SIZE   16
#define TABLE_RULE_ARR_SIZE   16
#define MAX_SLAVE_ERRMSG      1024

#define RPL_LOG_NAME (rli->master_log_name[0] ? rli->master_log_name :\
 "FIRST")
#define IO_RPL_LOG_NAME (mi->master_log_name[0] ? mi->master_log_name :\
 "FIRST")

/* masks for start/stop operations on io and sql slave threads */
#define SLAVE_IO  1
#define SLAVE_SQL 2

/*
  If the following is set, if first gives an error, second will be
  tried. Otherwise, if first fails, we fail.
*/
#define SLAVE_FORCE_ALL 4

int init_slave();
void init_slave_skip_errors(const char* arg);
int flush_master_info(MASTER_INFO* mi);
int flush_relay_log_info(RELAY_LOG_INFO* rli);
int register_slave_on_master(MYSQL* mysql);
int terminate_slave_threads(MASTER_INFO* mi, int thread_mask,
			     bool skip_lock = 0);
int terminate_slave_thread(THD* thd, pthread_mutex_t* term_mutex,
			   pthread_mutex_t* cond_lock,
			   pthread_cond_t* term_cond,
			   volatile bool* slave_running);
int start_slave_threads(bool need_slave_mutex, bool wait_for_start,
			MASTER_INFO* mi, const char* master_info_fname,
			const char* slave_info_fname, int thread_mask);
/*
  cond_lock is usually same as start_lock. It is needed for the case when
  start_lock is 0 which happens if start_slave_thread() is called already
  inside the start_lock section, but at the same time we want a
  pthread_cond_wait() on start_cond,start_lock
*/
int start_slave_thread(pthread_handler h_func, pthread_mutex_t* start_lock,
		       pthread_mutex_t *cond_lock,
		       pthread_cond_t* start_cond,
		       volatile bool* slave_running,
		       MASTER_INFO* mi);

// If fd is -1, dump to NET
int mysql_table_dump(THD* thd, const char* db,
		     const char* tbl_name, int fd = -1);

// retrieve non-exitent table from master
int fetch_master_table(THD* thd, const char* db_name, const char* table_name,
		       MASTER_INFO* mi, MYSQL* mysql);

int show_master_info(THD* thd, MASTER_INFO* mi);
int show_binlog_info(THD* thd);

// See if the query uses any tables that should not be replicated
int tables_ok(THD* thd, TABLE_LIST* tables);

/*
  Check to see if the database is ok to operate on with respect to the
  do and ignore lists - used in replication
*/
int db_ok(const char* db, I_List<i_string> &do_list,
	  I_List<i_string> &ignore_list );

int add_table_rule(HASH* h, const char* table_spec);
int add_wild_table_rule(DYNAMIC_ARRAY* a, const char* table_spec);
void init_table_rule_hash(HASH* h, bool* h_inited);
void init_table_rule_array(DYNAMIC_ARRAY* a, bool* a_inited);
char* rewrite_db(char* db);
int check_expected_error(THD* thd, RELAY_LOG_INFO* rli, int error_code);
void skip_load_data_infile(NET* net);
void slave_print_error(RELAY_LOG_INFO* rli,int err_code, const char* msg, ...);

void end_slave(); // clean up
int init_master_info(MASTER_INFO* mi, const char* master_info_fname,
		     const char* slave_info_fname);
void end_master_info(MASTER_INFO* mi);
int init_relay_log_info(RELAY_LOG_INFO* rli, const char* info_fname);
void end_relay_log_info(RELAY_LOG_INFO* rli);
void lock_slave_threads(MASTER_INFO* mi);
void unlock_slave_threads(MASTER_INFO* mi);
void init_thread_mask(int* mask,MASTER_INFO* mi,bool inverse);
int init_relay_log_pos(RELAY_LOG_INFO* rli,const char* log,ulonglong pos,
		       bool need_data_lock, const char** errmsg);

int purge_relay_logs(RELAY_LOG_INFO* rli,bool just_reset,const char** errmsg);

extern bool opt_log_slave_updates ;
pthread_handler_decl(handle_slave_io,arg);
pthread_handler_decl(handle_slave_sql,arg);
extern bool volatile abort_loop;
extern MASTER_INFO main_mi, *active_mi; // active_mi for multi-master
extern volatile int active_mi_in_use;
extern LIST master_list;
extern HASH replicate_do_table, replicate_ignore_table;
extern DYNAMIC_ARRAY  replicate_wild_do_table, replicate_wild_ignore_table;
extern bool do_table_inited, ignore_table_inited,
	    wild_do_table_inited, wild_ignore_table_inited;
extern bool table_rules_on;

extern int disconnect_slave_event_count, abort_slave_event_count ;

// the master variables are defaults read from my.cnf or command line
extern uint master_port, master_connect_retry, report_port;
extern my_string master_user, master_password, master_host,
       master_info_file, relay_log_info_file, report_user, report_host,
       report_password;

extern I_List<i_string> replicate_do_db, replicate_ignore_db;
extern I_List<i_string_pair> replicate_rewrite_db;
extern I_List<THD> threads;

#endif
