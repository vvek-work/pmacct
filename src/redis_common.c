/*  
 * pmacct (Promiscuous mode IP Accounting package)
 *
 * Copyright (c) 2003-2022 Paolo Lucente <paolo@pmacct.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* includes */
#include "pmacct.h"
#include "pmacct-data.h"
#include "thread_pool.h"
#include <sys/time.h>

/* Global variables */
thread_pool_t *redis_pool;

/* XXX: tmp_bmp_daemon_ha stuff: probably to be moved away */
char timestamp[SHORTBUFLEN];
int count_loop_num;
int ingest_flag;
int old_ingest_flag;
int write_log_flag, old_write_log_flag;

/* Functions */
void p_redis_thread_wrapper(struct p_redis_host *redis_host)
{
  /* initialize threads pool */
  redis_pool = allocate_thread_pool(1);

  assert(redis_pool);
  assert(redis_host);

  Log(LOG_DEBUG, "DEBUG ( %s ): %d thread(s) initialized\n", redis_host->log_id, 1);

  /* giving a kick to the Redis thread */
  send_to_pool(redis_pool, p_redis_master_produce_thread, redis_host);
}

int p_redis_master_produce_thread(void *rh)
{
  struct p_redis_host *redis_host = rh;
  unsigned int ret = 0, period = 0;
  
  p_redis_connect(redis_host, TRUE);

  for (;;) {
    if (!ret) {
      (*redis_host->th_hdlr)(redis_host);
      period = PM_REDIS_DEFAULT_REFRESH_TIME;
    }
    else {
      period = ret;
    }

    ret = sleep(period);
  }

  return SUCCESS;
}

void p_redis_init(struct p_redis_host *redis_host, char *log_id, redis_thread_handler th_hdlr)
{
  if (!redis_host || !log_id || !th_hdlr) return;

  memset(redis_host, 0, sizeof(struct p_redis_host));

  if (config.redis_host) {
    p_redis_set_log_id(redis_host, log_id);
    p_redis_set_db(redis_host, config.redis_db);
    p_redis_set_exp_time(redis_host, PM_REDIS_DEFAULT_EXP_TIME);
    p_redis_set_thread_handler(redis_host, th_hdlr);
 
    if (!config.cluster_name) {
      Log(LOG_ERR, "ERROR ( %s ): redis_host requires cluster_name to be specified. Exiting...\n\n", redis_host->log_id);
      exit_gracefully(1);
    }

    p_redis_thread_wrapper(redis_host);
  }
}

int p_redis_connect(struct p_redis_host *redis_host, int fatal)
{
  if (config.tmp_bmp_daemon_ha) {
    pthread_mutex_lock(&bmp_ha_struct.mutex_rd);
    bmp_ha_struct.dump_flag = TRUE;
    pthread_mutex_unlock(&bmp_ha_struct.mutex_rd);
  }

  assert(redis_host);

  connect: //re-connect

  if (config.redis_host) {

    time_t now = time(NULL);

    if (now >= (redis_host->last_conn + PM_REDIS_DEFAULT_CONN_RETRY)) {
      int res = REDIS_OK;
      redis_host->last_conn = now;
      if (redis_host->ctx) {
        /* reconnect */
        Log(LOG_DEBUG, "DEBUG ( %s ): reconnecting to redis server (fd %d)\n",
            redis_host->log_id, redis_host->ctx->fd);
        res = redisReconnect(redis_host->ctx);
      }
      else {
        /* initial connect */
        struct sockaddr_storage dest;
        socklen_t dest_len = sizeof(dest);
        char dest_str[INET6_ADDRSTRLEN];
        int dest_port;

        /* round of parsing and validation */
        parse_hostport(config.redis_host, (struct sockaddr *)&dest, &dest_len);
        sa_to_str(dest_str, sizeof(dest_str), (struct sockaddr *)&dest, FALSE);
        sa_to_port(&dest_port, (struct sockaddr *)&dest);
        if (!dest_port) {
          dest_port = PM_REDIS_DEFAULT_PORT;
        }
        Log(LOG_INFO, "INFO ( %s ): connecting to redis server %s:%d\n",
            redis_host->log_id, dest_str, dest_port);
        redis_host->ctx = redisConnect(dest_str, dest_port);
      }

      if (res != REDIS_OK || redis_host->ctx == NULL || redis_host->ctx->err) {
	if (redis_host->ctx) {
	  if (fatal) {
            Log(LOG_ERR, "ERROR ( %s ): Can't connect to redis server: %s\n",
                redis_host->log_id, redis_host->ctx->errstr);

	    if (config.tmp_bmp_daemon_ha) {
	      pthread_mutex_lock(&bmp_ha_struct.mutex_rd);
	      bmp_ha_struct.dump_flag = TRUE;
	      pthread_mutex_unlock(&bmp_ha_struct.mutex_rd);

	      /* Retry connection instead of exiting, so that when
		 redis is unavailable during runtime it will retry
		 conecction. When redis recover the HA function will
		 recover as well. */
	      sleep(5);
	      goto connect;
	    }
	    else {
	      exit_gracefully(1);
	    }
	  }
	  else {
            Log(LOG_WARNING, "WARN ( %s ): Can't connect to redis server: %s\n",
                redis_host->log_id, redis_host->ctx->errstr);
	    return ERR;
	  }
	}
	else {
	  Log(LOG_ERR, "ERROR ( %s ): Connection error: can't allocate redis context\n", redis_host->log_id);
          exit_gracefully(1);
	}
      }
      else {
	Log(LOG_DEBUG, "DEBUG ( %s ): Connection successful\n", redis_host->log_id);
      }
    }
  }
  
  if (config.tmp_bmp_daemon_ha) {
    count_loop_num = 1;

    // Set the timestamp
    if (strcmp(config.type, "core") == 0) {
      struct timeval current_time;

      Log(LOG_DEBUG, "DEBUG ( %s ): Redis connection reset\n", redis_host->log_id);
      gettimeofday(&current_time, NULL); // Get time in micro second

      // Setting the time when redis connects as timestamp for this bmp session
      snprintf(timestamp, sizeof(timestamp), "%ld", current_time.tv_sec * 1000000 + current_time.tv_usec);
    }
  }

  return SUCCESS;
}

// Return a boolean that stands for active(1)/standby(0)
bool p_redis_get_time(struct p_redis_host *redis_host)
{ 
  char session_name[SHORTBUFLEN][SHORTBUFLEN];  // the keys to the timestamp value. Each key is stores in one row
  //throw wraning: memory corrupt
  long long session_value[SHORTBUFLEN]; // the timestamp value
  int session_num;              // the number of keys in Redis
  char *eptr;

  redis_host->reply = redisCommand(redis_host->ctx, "KEYS %s%s%d*attachment_time",config.cluster_name, PM_REDIS_DEFAULT_SEP, config.cluster_id);
  // Check if the Redis has replied as expected without freeing the object
  if (redis_host->reply){
    if (redis_host->reply->type == REDIS_REPLY_ERROR){
      Log(LOG_WARNING, "WARN ( %s ): reply='%s'\n", redis_host->log_id, redis_host->reply->str);
    }
  }
  else{
    p_redis_connect(redis_host, FALSE);
    return FALSE;
  }
  session_num = (int)redis_host->reply->elements;
  // If there is no timestamp in the Redis, set all as standby
  if (session_num == 0)
    return FALSE;
  for (int i = 0; i < session_num; i++){
    strcpy(session_name[i], redis_host->reply->element[i]->str);
  }
  freeReplyObject(redis_host->reply);

  for (int i = 0; i < session_num; i++){
    redis_host->reply = redisCommand(redis_host->ctx, "GET %s", session_name[i]);
    session_value[i] = strtoll(redis_host->reply->str, &eptr, 0);
    // If there is a timestamp larger than its timestamp, return 0
    if (strtoll(timestamp, &eptr, 0) > session_value[i]){
      p_redis_process_reply(redis_host);
      return FALSE;
    }
    // Continue if it's its timestamp
    else if (strtoll(timestamp, &eptr, 0) == session_value[i]){
      continue;
    }
  }
  p_redis_process_reply(redis_host);
  return TRUE;
}

void p_redis_close(struct p_redis_host *redis_host)
{
  redisFree(redis_host->ctx);
}

void p_redis_set_string(struct p_redis_host *redis_host, char *resource, char *value, int expire)
{
  if (expire > 0) {
    redis_host->reply = redisCommand(redis_host->ctx, "SETEX %s%s%d%s%s %d %s", config.cluster_name, PM_REDIS_DEFAULT_SEP,
				     config.cluster_id, PM_REDIS_DEFAULT_SEP, resource, redis_host->exp_time, value);
  }
  else {
    redis_host->reply = redisCommand(redis_host->ctx, "SET %s%s%d%s%s %s", config.cluster_name, PM_REDIS_DEFAULT_SEP,
				     config.cluster_id, PM_REDIS_DEFAULT_SEP, resource, value);
  }

  p_redis_process_reply(redis_host);
}

void p_redis_set_int(struct p_redis_host *redis_host, char *resource, int value, int expire)
{
  if (expire > 0) {
    redis_host->reply = redisCommand(redis_host->ctx, "SETEX %s%s%d%s%s %d %d", config.cluster_name, PM_REDIS_DEFAULT_SEP,
				     config.cluster_id, PM_REDIS_DEFAULT_SEP, resource, redis_host->exp_time, value);
  }
  else {
    redis_host->reply = redisCommand(redis_host->ctx, "SET %s%s%d%s%s %d", config.cluster_name, PM_REDIS_DEFAULT_SEP,
				     config.cluster_id, PM_REDIS_DEFAULT_SEP, resource, value);
  }

  p_redis_process_reply(redis_host);
}

void p_redis_ping(struct p_redis_host *redis_host)
{
  redis_host->reply = redisCommand(redis_host->ctx, "PING");
  p_redis_process_reply(redis_host);
}

void p_redis_select_db(struct p_redis_host *redis_host)
{
  char select_cmd[VERYSHORTBUFLEN];
  
  if (redis_host->db) {
    snprintf(select_cmd, sizeof(select_cmd), "SELECT %d", redis_host->db);
    redis_host->reply = redisCommand(redis_host->ctx, select_cmd);
    p_redis_process_reply(redis_host);
  }
}

void p_redis_process_reply(struct p_redis_host *redis_host)
{
  if (redis_host->reply) {
    if (redis_host->reply->type == REDIS_REPLY_ERROR) {
      Log(LOG_WARNING, "WARN ( %s ): reply='%s'\n", redis_host->log_id, redis_host->reply->str);
    }

    freeReplyObject(redis_host->reply);
  }
  else {
    p_redis_connect(redis_host, FALSE);
  }
}

void p_redis_set_log_id(struct p_redis_host *redis_host, char *log_id)
{
  if (redis_host) {
    strlcpy(redis_host->log_id, log_id, sizeof(redis_host->log_id));
    strncat(redis_host->log_id, "/redis", (sizeof(redis_host->log_id) - strlen(redis_host->log_id))); 
  }
}

void p_redis_set_db(struct p_redis_host *redis_host, int db)
{
  if (redis_host) redis_host->db = db;
}

void p_redis_set_exp_time(struct p_redis_host *redis_host, int exp_time)
{
  if (redis_host) redis_host->exp_time = exp_time;
}

void p_redis_set_thread_handler(struct p_redis_host *redis_host, redis_thread_handler th_hdlr)
{
  if (redis_host) redis_host->th_hdlr = th_hdlr;
}

void p_redis_thread_produce_common_core_handler(void *rh)
{
  struct p_redis_host *redis_host = rh;
  char buf[SRVBUFLEN], name_and_type[SHORTBUFLEN], daemon_type[VERYSHORTBUFLEN], name_and_time[SHORTBUFLEN];

  switch (config.acct_type) {
  case ACCT_NF:
    snprintf(daemon_type, sizeof(daemon_type), "%s", "nfacctd");
    break;
  case ACCT_SF:
    snprintf(daemon_type, sizeof(daemon_type), "%s", "sfacctd");
    break;
  case ACCT_PM:
    if (config.uacctd_group) {
      snprintf(daemon_type, sizeof(daemon_type), "%s", "uacctd");
    }
    else {
      snprintf(daemon_type, sizeof(daemon_type), "%s", "pmacctd");
    }
    break;
  case ACCT_PMBGP:
    snprintf(daemon_type, sizeof(daemon_type), "%s", "pmbgpd");
    break;
  case ACCT_PMBMP:
    snprintf(daemon_type, sizeof(daemon_type), "%s", "pmbmpd");
    break;
  case ACCT_PMTELE:
    snprintf(daemon_type, sizeof(daemon_type), "%s", "pmtelemetryd");
    break;
  default:
    break;
  }
  p_redis_set_string(redis_host, "daemon_type", daemon_type, PM_REDIS_DEFAULT_EXP_TIME);

  snprintf(name_and_type, sizeof(name_and_type), "process%s%s%s%s", PM_REDIS_DEFAULT_SEP,
	   config.name, PM_REDIS_DEFAULT_SEP, config.type);
  p_redis_set_int(redis_host, name_and_type, TRUE, PM_REDIS_DEFAULT_EXP_TIME);

  if (config.tmp_bmp_daemon_ha) {
    // Refresh the timestamp if the regenerate_timestamp_flag is set
    if (bmp_ha_struct.regenerate_timestamp_flag){
      struct timeval current_time;

      Log(LOG_DEBUG, "DEBUG ( %s ): Redis timestamp reset\n", redis_host->log_id);
      gettimeofday(&current_time, NULL);
      // Setting the time when redis connects as timestamp for this bmp session
      snprintf(timestamp, sizeof(timestamp), "%ld", current_time.tv_sec * 1000000 + current_time.tv_usec);
      bmp_ha_struct.regenerate_timestamp_flag = FALSE;
    }

    // If this thread belongs to the core process, write the current attachment time to redis
    if (strcmp(config.type, "core") == 0) {
      snprintf(name_and_time, sizeof(name_and_time), "%s%sattachment_time",
	       config.name, PM_REDIS_DEFAULT_SEP);
      p_redis_set_string(redis_host, name_and_time, timestamp, PM_REDIS_DEFAULT_EXP_TIME);
    }

    // Doing a "get timestamp" per second
    ingest_flag = p_redis_get_time(redis_host);
    write_log_flag = (ingest_flag||bmp_ha_struct.set_to_active_flag)&&!bmp_ha_struct.set_to_standby_flag;

    // Dump the queue when there is a status change and this is not the first connection
    if (ingest_flag && !old_ingest_flag && count_loop_num != 1){
      pthread_mutex_lock(&bmp_ha_struct.mutex_rd);
      bmp_ha_struct.queue_dump_flag = TRUE;
      pthread_mutex_unlock(&bmp_ha_struct.mutex_rd);
    }

    pthread_mutex_lock(&bmp_ha_struct.mutex_rd);

    if (bmp_ha_struct.set_to_active_flag) {
      bmp_ha_struct.dump_flag = TRUE;
    }
    else if (bmp_ha_struct.set_to_standby_flag) {
      bmp_ha_struct.dump_flag = FALSE;
    }
    else {
      bmp_ha_struct.dump_flag = ingest_flag;
    }

    pthread_mutex_unlock(&bmp_ha_struct.mutex_rd);

    // Write the current collector status to Log
    if ((write_log_flag != old_write_log_flag) || count_loop_num == 1) {
      Log(LOG_INFO, "INFO ( %s ): Daemon state: %s\n",
	  redis_host->log_id,
	  (ingest_flag || bmp_ha_struct.set_to_active_flag) && !bmp_ha_struct.set_to_standby_flag ? "ACTIVE" : "STANDBY");
    }

    old_ingest_flag = ingest_flag;
    old_write_log_flag = write_log_flag;

    /* count_loop_num is used to record the number of loops.
       More specificly, it's useful when identifying the first
       loop, within which we need to log initial daemon state.
       To avoid the number going too large, the range is
       limited within[2, 62] */
    count_loop_num++;
    /* use 62, so that after the first loop, the number falls
       within the range[2, 62] other than goes back to 1
       since 1 is needed before */
    count_loop_num = count_loop_num % 62 + 2;
  }

  if (config.acct_type < ACCT_FWDPLANE_MAX) {
    if (config.nfacctd_isis) {
      snprintf(buf, sizeof(buf), "%s%sisis", name_and_type, PM_REDIS_DEFAULT_SEP);
      p_redis_set_int(redis_host, buf, TRUE, PM_REDIS_DEFAULT_EXP_TIME);
    }

    if (config.bgp_daemon) {
      snprintf(buf, sizeof(buf), "%s%sbgp", name_and_type, PM_REDIS_DEFAULT_SEP);
      p_redis_set_int(redis_host, buf, TRUE, PM_REDIS_DEFAULT_EXP_TIME);
    }

    if (config.bmp_daemon) {
      snprintf(buf, sizeof(buf), "%s%sbmp", name_and_type, PM_REDIS_DEFAULT_SEP);
      p_redis_set_int(redis_host, buf, TRUE, PM_REDIS_DEFAULT_EXP_TIME);
    }

    if (config.telemetry_daemon) {
      snprintf(buf, sizeof(buf), "%s%stelemetry", name_and_type, PM_REDIS_DEFAULT_SEP);
      p_redis_set_int(redis_host, buf, TRUE, PM_REDIS_DEFAULT_EXP_TIME);
    }
  }
}

void p_redis_thread_produce_common_plugin_handler(void *rh)
{
  struct p_redis_host *redis_host = rh;
  char name_and_type[SRVBUFLEN];

  snprintf(name_and_type, sizeof(name_and_type), "process%s%s%s%s", PM_REDIS_DEFAULT_SEP,
	   config.name, PM_REDIS_DEFAULT_SEP, config.type);
  p_redis_set_int(redis_host, name_and_type, TRUE, PM_REDIS_DEFAULT_EXP_TIME);
}

