#include <mtev_defines.h>
#include <mtev_conf.h>
#include <mtev_console.h>
#include <mtev_dso.h>
#include <mtev_listener.h>
#include <mtev_main.h>
#include <mtev_memory.h>
#include <mtev_rest.h>
#include <mtev_capabilities_listener.h>
#include <mtev_events_rest.h>
#include <mtev_stats.h>
#include <mtev_heap_profiler.h>
#include <mtev_uuid.h>
#include <eventer/eventer.h>

#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <inttypes.h>
#include <errno.h>

#define APPNAME "example1"
#define CLUSTER_NAME "ponies"
static char *config_file = NULL;
static int debug = 0;
static int foreground = 0;
static enum {
  PROC_OP_START,
  PROC_OP_STOP,
  PROC_OP_STATUS,
  PROC_OP_ERROR
} proc_op = PROC_OP_START;
static char *droptouser = NULL, *droptogroup = NULL;

static int
usage(const char *prog) {
  fprintf(stderr, "%s <-c conffile> [-k <start|stop|status>] [-D] [-d]\n\n", prog);
  fprintf(stderr, "\t-c conffile\tthe configuration file to load\n");
  fprintf(stderr, "\t-D\t\trun in the foreground (don't daemonize)\n");
  fprintf(stderr, "\t-d\t\tturn on debugging\n");
  fprintf(stderr, "\t-k <op>\t\tstart, stop or check a running instance\n");
  return 2;
}
static void
parse_cli_args(int argc, char * const *argv) {
  int c;
  while((c = getopt(argc, argv, "c:Ddk:l:L:")) != EOF) {
    switch(c) {
      case 'c':
        config_file = optarg;
        break;
      case 'd': debug = 1; break;
      case 'D': foreground++; break;
      case 'k': 
        if(!strcmp(optarg, "start")) proc_op = PROC_OP_START;
        else if(!strcmp(optarg, "stop")) proc_op = PROC_OP_STOP;
        else if(!strcmp(optarg, "status")) proc_op = PROC_OP_STATUS;
        else proc_op = PROC_OP_ERROR;
        break;
      case 'l':
        mtev_main_enable_log(optarg);
        break;
      case 'L':
        mtev_main_disable_log(optarg);
    }
  }
}

static void asynch_hello(void *closure) {
  mtev_http_rest_closure_t *restc = closure;
  mtev_http_session_ctx *ctx = restc->http_ctx;
  sleep(1);
  mtev_http_response_append_str(ctx, "Hello world.\n");
}
void subcall2(mtev_http_rest_closure_t *restc) {
  eventer_aco_simple_asynch(asynch_hello, restc);
  mtevL(mtev_debug, "subcall2 return\n");
}
void subcall1(mtev_http_rest_closure_t *restc) {
  subcall2(restc);
  mtevL(mtev_debug, "subcall1 return\n");
}
static int hello_handler(mtev_http_rest_closure_t *restc,
                         int npats, char **pats) {
  mtev_http_session_ctx *ctx = restc->http_ctx;
  mtev_http_response_ok(ctx, "text/plain");
  subcall1(restc);
  mtev_http_response_end(ctx);
  return 0;
}

static void
workie(void *closure) {
  mtevL(mtev_error, "doing asynch stuff...\n");
  sleep(1);
  mtevL(mtev_error, "done asynch stuff...\n");
}
static void
listen_to_me(void) {
  eventer_aco_t e = eventer_aco_arg();

  struct timeval two_sec = { 2, 0 };

  while(1) {
    int rv;
    char buff[1024], out[1024];
    memset(buff, 1, sizeof(buff));
    rv = eventer_aco_read(e, buff, sizeof(buff), &two_sec);
    if(rv == -1) {
      if(errno != ETIME) break;
      rv = eventer_aco_write(e, "idle\n", 5, NULL);
    } else {
      if(rv >= 4 && !strncasecmp(buff, "quit", 4)) {
        rv = eventer_aco_write(e, "bye!\n", 5, NULL);
        break;
      }
      eventer_aco_simple_asynch(workie, NULL);
      snprintf(out, sizeof(out), "I just read %d bytes '%.*s', errno %d\n", rv, (rv > 2) ? rv - 2 : 0, buff, errno);
      rv = eventer_aco_write(e, out, strlen(out), NULL);
    }
    if(rv == -1) break;
  }

  eventer_aco_close(e);
  eventer_aco_free(e);
}

static int
child_main(void) {
  /* reload out config, to make sure we have the most current */

  if(mtev_conf_load(NULL) == -1) {
    mtevL(mtev_error, "Cannot load config: '%s'\n", config_file);
    exit(2);
  }
  eventer_init();

  mtev_listener_register_aco_function("listen_to_me", listen_to_me);

  mtev_dso_init();
  mtev_console_init(APPNAME);
  mtev_console_conf_init();
  mtev_http_rest_init();
  mtev_capabilities_listener_init();
  mtev_events_rest_init();
  mtev_stats_rest_init();
  mtev_heap_profiler_rest_init();
  mtev_listener_init(APPNAME);
  mtev_dso_post_init();

  mtev_conf_coalesce_changes(10); /* 10 seconds of no changes before we write */
  mtev_conf_watch_and_journal_watchdog(NULL, NULL);

  mtev_rest_mountpoint_t *rule = mtev_http_rest_new_rule(
    "GET", "/", "^hello$", hello_handler
  );
  mtev_rest_mountpoint_set_auth(rule, mtev_http_rest_client_cert_auth);
  mtev_rest_mountpoint_set_aco(rule, mtev_true);

  mtev_http_rest_register_auth(
    "GET", "/", "^(.*)$", mtev_rest_simple_file_handler,
           mtev_http_rest_client_cert_auth
  );

  /* Lastly, spin up the event loop */
  eventer_loop();
  return 0;
}

int main(int argc, char **argv) {
  pid_t pid, pgid;
  parse_cli_args(argc, argv);
  if(!config_file) exit(usage(argv[0]));
  mtev_memory_init();
  switch(proc_op) {
    case PROC_OP_START:
      mtev_main(APPNAME, config_file, debug, foreground,
                MTEV_LOCK_OP_LOCK, NULL, droptouser, droptogroup,
                child_main);
      break;
    case PROC_OP_STOP:
      exit(mtev_main_terminate(APPNAME, config_file, debug));
      break;
    case PROC_OP_STATUS:
      if(mtev_main_status(APPNAME, config_file, debug, &pid, &pgid) != 0) exit(-1);
      mtevL(mtev_debug, "running pid: %d, pgid: %d\n", pid, pgid);
      break;
    case PROC_OP_ERROR:
      exit(usage(argv[0]));
      break;
   }
  return 0;
}
