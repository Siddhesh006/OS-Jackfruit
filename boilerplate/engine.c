#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
  CMD_SUPERVISOR = 0,
  CMD_START,
  CMD_RUN,
  CMD_PS,
  CMD_LOGS,
  CMD_STOP
} command_kind_t;

typedef enum {
  CONTAINER_STARTING = 0,
  CONTAINER_RUNNING,
  CONTAINER_STOPPED,
  CONTAINER_KILLED,
  CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
  char id[CONTAINER_ID_LEN];
  pid_t host_pid;
  time_t started_at;
  container_state_t state;
  unsigned long soft_limit_bytes;
  unsigned long hard_limit_bytes;
  int exit_code;
  int exit_signal;
  char log_path[PATH_MAX];
  int stop_requested;
  int log_read_fd;
  struct container_record *next;
} container_record_t;

typedef struct {
  char container_id[CONTAINER_ID_LEN];
  size_t length;
  char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
  log_item_t items[LOG_BUFFER_CAPACITY];
  size_t head;
  size_t tail;
  size_t count;
  int shutting_down;
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
  command_kind_t kind;
  char container_id[CONTAINER_ID_LEN];
  char rootfs[PATH_MAX];
  char command[CHILD_COMMAND_LEN];
  unsigned long soft_limit_bytes;
  unsigned long hard_limit_bytes;
  int nice_value;
} control_request_t;

typedef struct {
  int status;
  char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
  char id[CONTAINER_ID_LEN];
  char rootfs[PATH_MAX];
  char command[CHILD_COMMAND_LEN];
  int nice_value;
  int log_write_fd;
} child_config_t;

typedef struct {
  int server_fd;
  int monitor_fd;
  int should_stop;
  pthread_t logger_thread;
  bounded_buffer_t log_buffer;
  pthread_mutex_t metadata_lock;
  container_record_t *containers;
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;

typedef struct {
  int pipe_read_fd;
  char container_id[CONTAINER_ID_LEN];
  bounded_buffer_t *buffer;
} producer_arg_t;

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage:\n"
          "  %s supervisor <base-rootfs>\n"
          "  %s start <id> <container-rootfs> <command> [--soft-mib N] "
          "[--hard-mib N] [--nice N]\n"
          "  %s run <id> <container-rootfs> <command> [--soft-mib N] "
          "[--hard-mib N] [--nice N]\n"
          "  %s ps\n"
          "  %s logs <id>\n"
          "  %s stop <id>\n",
          prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value,
                          unsigned long *target_bytes) {
  char *end = NULL;
  unsigned long mib;

  errno = 0;
  mib = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
    return -1;
  }

  if (mib > ULONG_MAX / (1UL << 20)) {
    fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
    return -1;
  }

  *target_bytes = mib * (1UL << 20);
  return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[],
                                int start_index) {
  int i;

  for (i = start_index; i < argc; i += 2) {
    char *end = NULL;
    long nice_value;

    if (i + 1 >= argc) {
      fprintf(stderr, "Missing value for option: %s\n", argv[i]);
      return -1;
    }

    if (strcmp(argv[i], "--soft-mib") == 0) {
      if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) !=
          0)
        return -1;
      continue;
    }

    if (strcmp(argv[i], "--hard-mib") == 0) {
      if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) !=
          0)
        return -1;
      continue;
    }

    if (strcmp(argv[i], "--nice") == 0) {
      errno = 0;
      nice_value = strtol(argv[i + 1], &end, 10);
      if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
          nice_value < -20 || nice_value > 19) {
        fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n",
                argv[i + 1]);
        return -1;
      }
      req->nice_value = (int)nice_value;
      continue;
    }

    fprintf(stderr, "Unknown option: %s\n", argv[i]);
    return -1;
  }

  if (req->soft_limit_bytes > req->hard_limit_bytes) {
    fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
    return -1;
  }

  return 0;
}

static const char *state_to_string(container_state_t state) {
  switch (state) {
  case CONTAINER_STARTING:
    return "starting";
  case CONTAINER_RUNNING:
    return "running";
  case CONTAINER_STOPPED:
    return "stopped";
  case CONTAINER_KILLED:
    return "killed";
  case CONTAINER_EXITED:
    return "exited";
  default:
    return "unknown";
  }
}

static int bounded_buffer_init(bounded_buffer_t *buffer) {
  int rc;

  memset(buffer, 0, sizeof(*buffer));

  rc = pthread_mutex_init(&buffer->mutex, NULL);
  if (rc != 0)
    return rc;

  rc = pthread_cond_init(&buffer->not_empty, NULL);
  if (rc != 0) {
    pthread_mutex_destroy(&buffer->mutex);
    return rc;
  }

  rc = pthread_cond_init(&buffer->not_full, NULL);
  if (rc != 0) {
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
    return rc;
  }

  return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer) {
  pthread_cond_destroy(&buffer->not_full);
  pthread_cond_destroy(&buffer->not_empty);
  pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer) {
  pthread_mutex_lock(&buffer->mutex);
  buffer->shutting_down = 1;
  pthread_cond_broadcast(&buffer->not_empty);
  pthread_cond_broadcast(&buffer->not_full);
  pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item) {
  pthread_mutex_lock(&buffer->mutex);

  while (buffer->count >= LOG_BUFFER_CAPACITY && !buffer->shutting_down)
    pthread_cond_wait(&buffer->not_full, &buffer->mutex);

  if (buffer->shutting_down) {
    pthread_mutex_unlock(&buffer->mutex);
    return -1;
  }

  buffer->items[buffer->tail] = *item;
  buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
  buffer->count++;

  pthread_cond_signal(&buffer->not_empty);
  pthread_mutex_unlock(&buffer->mutex);
  return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item) {
  pthread_mutex_lock(&buffer->mutex);

  while (buffer->count == 0 && !buffer->shutting_down)
    pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

  if (buffer->count == 0) {
    pthread_mutex_unlock(&buffer->mutex);
    return -1;
  }

  *item = buffer->items[buffer->head];
  buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
  buffer->count--;

  pthread_cond_signal(&buffer->not_full);
  pthread_mutex_unlock(&buffer->mutex);
  return 0;
}

void *logging_thread(void *arg) {
  supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
  log_item_t item;
  int fd;
  char path[PATH_MAX];

  while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
      perror("logging_thread: open log file");
      continue;
    }
    if (write(fd, item.data, item.length) < 0)
      perror("logging_thread: write");
    close(fd);
  }

  return NULL;
}

static void *producer_thread(void *arg) {
  producer_arg_t *parg = (producer_arg_t *)arg;
  char buf[LOG_CHUNK_SIZE];
  ssize_t n;
  log_item_t item;

  while ((n = read(parg->pipe_read_fd, buf, sizeof(buf))) > 0) {
    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, parg->container_id, CONTAINER_ID_LEN - 1);
    item.length = (size_t)n;
    memcpy(item.data, buf, (size_t)n);
    bounded_buffer_push(parg->buffer, &item);
  }

  close(parg->pipe_read_fd);
  free(parg);
  return NULL;
}

int child_fn(void *arg) {
  child_config_t *cfg = (child_config_t *)arg;

  dup2(cfg->log_write_fd, STDOUT_FILENO);
  dup2(cfg->log_write_fd, STDERR_FILENO);
  close(cfg->log_write_fd);

  if (cfg->nice_value != 0)
    (void)nice(cfg->nice_value);

  if (chroot(cfg->rootfs) < 0) {
    perror("child_fn: chroot");
    return 1;
  }
  if (chdir("/") < 0) {
    perror("child_fn: chdir /");
    return 1;
  }

  if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
    perror("child_fn: mount /proc (warning)");
  }

  char hn[CONTAINER_ID_LEN + 4];
  snprintf(hn, sizeof(hn), "ct-%s", cfg->id);
  (void)sethostname(hn, strlen(hn));

  execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
  perror("child_fn: execl");
  return 1;
}

int register_with_monitor(int monitor_fd, const char *container_id,
                          pid_t host_pid, unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes) {
  struct monitor_request req;

  memset(&req, 0, sizeof(req));
  req.pid = host_pid;
  req.soft_limit_bytes = soft_limit_bytes;
  req.hard_limit_bytes = hard_limit_bytes;
  strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

  if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
    return -1;

  return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id,
                            pid_t host_pid) {
  struct monitor_request req;

  memset(&req, 0, sizeof(req));
  req.pid = host_pid;
  strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

  if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
    return -1;

  return 0;
}

static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id) {
  container_record_t *c = ctx->containers;
  while (c) {
    if (strcmp(c->id, id) == 0)
      return c;
    c = c->next;
  }
  return NULL;
}

static container_record_t *find_container_by_pid(supervisor_ctx_t *ctx,
                                                 pid_t pid) {
  container_record_t *c = ctx->containers;
  while (c) {
    if (c->host_pid == pid)
      return c;
    c = c->next;
  }
  return NULL;
}

static void sigchld_handler(int sig) {
  (void)sig;
  int status;
  pid_t pid;

  if (!g_ctx)
    return;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    container_record_t *rec;

    pthread_mutex_lock(&g_ctx->metadata_lock);
    rec = find_container_by_pid(g_ctx, pid);
    if (rec) {
      if (WIFEXITED(status)) {
        rec->exit_code = WEXITSTATUS(status);
        rec->exit_signal = 0;
        rec->state = rec->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
      } else if (WIFSIGNALED(status)) {
        rec->exit_signal = WTERMSIG(status);
        rec->exit_code = 128 + rec->exit_signal;
        if (rec->stop_requested) {
          rec->state = CONTAINER_STOPPED;
        } else if (rec->exit_signal == SIGKILL) {
          rec->state = CONTAINER_KILLED;
        } else {
          rec->state = CONTAINER_EXITED;
        }
      }
      if (g_ctx->monitor_fd >= 0)
        unregister_from_monitor(g_ctx->monitor_fd, rec->id, rec->host_pid);
    }
    pthread_mutex_unlock(&g_ctx->metadata_lock);
  }
}

static void sigterm_handler(int sig) {
  (void)sig;
  if (g_ctx)
    g_ctx->should_stop = 1;
}

static container_record_t *spawn_container(supervisor_ctx_t *ctx,
                                           const control_request_t *req) {
  int pipefd[2];
  char *stack;
  pid_t pid;
  child_config_t *cfg;
  container_record_t *rec;
  pthread_t prod_tid;
  producer_arg_t *parg;

  if (pipe(pipefd) < 0) {
    perror("spawn_container: pipe");
    return NULL;
  }

  stack = malloc(STACK_SIZE);
  if (!stack) {
    perror("spawn_container: malloc stack");
    close(pipefd[0]);
    close(pipefd[1]);
    return NULL;
  }

  cfg = malloc(sizeof(*cfg));
  if (!cfg) {
    perror("spawn_container: malloc cfg");
    free(stack);
    close(pipefd[0]);
    close(pipefd[1]);
    return NULL;
  }
  memset(cfg, 0, sizeof(*cfg));
  strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
  strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
  strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
  cfg->nice_value = req->nice_value;
  cfg->log_write_fd = pipefd[1];

  pid = clone(child_fn, stack + STACK_SIZE,
              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, cfg);

  close(pipefd[1]);
  free(stack);
  free(cfg);

  if (pid < 0) {
    perror("spawn_container: clone");
    close(pipefd[0]);
    return NULL;
  }

  mkdir(LOG_DIR, 0755);

  rec = calloc(1, sizeof(*rec));
  if (!rec) {
    perror("spawn_container: calloc record");
    close(pipefd[0]);
    kill(pid, SIGKILL);
    return NULL;
  }
  strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
  rec->host_pid = pid;
  rec->started_at = time(NULL);
  rec->state = CONTAINER_RUNNING;
  rec->soft_limit_bytes = req->soft_limit_bytes;
  rec->hard_limit_bytes = req->hard_limit_bytes;
  rec->exit_code = 0;
  rec->exit_signal = 0;
  rec->stop_requested = 0;
  rec->log_read_fd = pipefd[0];
  snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

  if (ctx->monitor_fd >= 0) {
    if (register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes) < 0)
      perror("spawn_container: register_with_monitor (continuing)");
  }

  parg = malloc(sizeof(*parg));
  if (!parg) {
    perror("spawn_container: malloc parg");
    close(pipefd[0]);
  } else {
    parg->pipe_read_fd = pipefd[0];
    strncpy(parg->container_id, req->container_id, CONTAINER_ID_LEN - 1);
    parg->buffer = &ctx->log_buffer;
    if (pthread_create(&prod_tid, NULL, producer_thread, parg) != 0) {
      perror("spawn_container: pthread_create producer");
      free(parg);
      close(pipefd[0]);
    } else {
      pthread_detach(prod_tid);
      rec->log_read_fd = -1;
    }
  }

  pthread_mutex_lock(&ctx->metadata_lock);
  rec->next = ctx->containers;
  ctx->containers = rec;
  pthread_mutex_unlock(&ctx->metadata_lock);

  return rec;
}

static void handle_ps(supervisor_ctx_t *ctx, int client_fd) {
  container_record_t *c;
  char line[512];
  int n;

  pthread_mutex_lock(&ctx->metadata_lock);

  n = snprintf(line, sizeof(line), "%-12s %-8s %-12s %-10s %-12s %-12s %-6s\n",
               "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)", "STARTED",
               "EXIT");
  (void)write(client_fd, line, n);

  for (c = ctx->containers; c; c = c->next) {
    char started[32];
    struct tm *tm_info = localtime(&c->started_at);
    strftime(started, sizeof(started), "%H:%M:%S", tm_info);

    n = snprintf(line, sizeof(line),
                 "%-12s %-8d %-12s %-10lu %-12lu %-12s %-6d\n", c->id,
                 c->host_pid, state_to_string(c->state),
                 c->soft_limit_bytes >> 20, c->hard_limit_bytes >> 20, started,
                 c->exit_code);
    (void)write(client_fd, line, n);
  }

  pthread_mutex_unlock(&ctx->metadata_lock);
}

static void handle_logs(supervisor_ctx_t *ctx, int client_fd, const char *id) {
  container_record_t *rec;
  char path[PATH_MAX];
  int fd;
  char buf[4096];
  ssize_t n;

  pthread_mutex_lock(&ctx->metadata_lock);
  rec = find_container(ctx, id);
  if (!rec) {
    pthread_mutex_unlock(&ctx->metadata_lock);
    const char *msg = "Error: container not found\n";
    (void)write(client_fd, msg, strlen(msg));
    return;
  }
  strncpy(path, rec->log_path, PATH_MAX - 1);
  pthread_mutex_unlock(&ctx->metadata_lock);

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    const char *msg = "Error: log file not found\n";
    (void)write(client_fd, msg, strlen(msg));
    return;
  }
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    (void)write(client_fd, buf, (size_t)n);
  close(fd);
}

static void handle_stop(supervisor_ctx_t *ctx, int client_fd, const char *id) {
  container_record_t *rec;

  pthread_mutex_lock(&ctx->metadata_lock);
  rec = find_container(ctx, id);
  if (!rec) {
    pthread_mutex_unlock(&ctx->metadata_lock);
    const char *msg = "Error: container not found\n";
    (void)write(client_fd, msg, strlen(msg));
    return;
  }

  if (rec->state != CONTAINER_RUNNING && rec->state != CONTAINER_STARTING) {
    pthread_mutex_unlock(&ctx->metadata_lock);
    const char *msg = "Error: container is not running\n";
    (void)write(client_fd, msg, strlen(msg));
    return;
  }

  rec->stop_requested = 1;
  kill(rec->host_pid, SIGTERM);
  pthread_mutex_unlock(&ctx->metadata_lock);

  const char *msg = "OK: stop signal sent\n";
  (void)write(client_fd, msg, strlen(msg));
}

static void handle_request(supervisor_ctx_t *ctx, int client_fd,
                           const control_request_t *req) {
  container_record_t *rec;
  char msg[CONTROL_MESSAGE_LEN];

  switch (req->kind) {
  case CMD_START:
    rec = spawn_container(ctx, req);
    if (!rec) {
      snprintf(msg, sizeof(msg), "Error: failed to start container %s\n",
               req->container_id);
      (void)write(client_fd, msg, strlen(msg));
    } else {
      snprintf(msg, sizeof(msg), "OK: container %s started (pid=%d)\n", rec->id,
               rec->host_pid);
      (void)write(client_fd, msg, strlen(msg));
    }
    break;

  case CMD_RUN:
    rec = spawn_container(ctx, req);
    if (!rec) {
      snprintf(msg, sizeof(msg), "Error: failed to run container %s\n",
               req->container_id);
      (void)write(client_fd, msg, strlen(msg));
      break;
    }
    snprintf(msg, sizeof(msg), "RUNNING: container %s pid=%d\n", rec->id,
             rec->host_pid);
    (void)write(client_fd, msg, strlen(msg));

    {
      pid_t target_pid = rec->host_pid;
      int wstatus;
      waitpid(target_pid, &wstatus, 0);

      pthread_mutex_lock(&ctx->metadata_lock);
      rec = find_container_by_pid(ctx, target_pid);
      if (rec) {
        if (WIFEXITED(wstatus)) {
          rec->exit_code = WEXITSTATUS(wstatus);
          rec->state =
              rec->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
        } else if (WIFSIGNALED(wstatus)) {
          rec->exit_signal = WTERMSIG(wstatus);
          rec->exit_code = 128 + rec->exit_signal;
          rec->state = (rec->stop_requested)           ? CONTAINER_STOPPED
                       : (rec->exit_signal == SIGKILL) ? CONTAINER_KILLED
                                                       : CONTAINER_EXITED;
        }
        snprintf(msg, sizeof(msg),
                 "DONE: container %s exited state=%s code=%d\n", rec->id,
                 state_to_string(rec->state), rec->exit_code);
        if (ctx->monitor_fd >= 0)
          unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
      } else {
        snprintf(msg, sizeof(msg), "DONE: container exited\n");
      }
      pthread_mutex_unlock(&ctx->metadata_lock);
      (void)write(client_fd, msg, strlen(msg));
    }
    break;

  case CMD_PS:
    handle_ps(ctx, client_fd);
    break;

  case CMD_LOGS:
    handle_logs(ctx, client_fd, req->container_id);
    break;

  case CMD_STOP:
    handle_stop(ctx, client_fd, req->container_id);
    break;

  default:
    snprintf(msg, sizeof(msg), "Error: unknown command kind %d\n", req->kind);
    (void)write(client_fd, msg, strlen(msg));
    break;
  }
}

static int run_supervisor(const char *rootfs) {
  supervisor_ctx_t ctx;
  int rc;

  memset(&ctx, 0, sizeof(ctx));
  ctx.server_fd = -1;
  ctx.monitor_fd = -1;

  rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
  if (rc != 0) {
    errno = rc;
    perror("pthread_mutex_init");
    return 1;
  }

  rc = bounded_buffer_init(&ctx.log_buffer);
  if (rc != 0) {
    errno = rc;
    perror("bounded_buffer_init");
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 1;
  }

  ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
  if (ctx.monitor_fd < 0)
    fprintf(stderr,
            "Warning: cannot open /dev/container_monitor (%s). "
            "Memory limits will not be enforced.\n",
            strerror(errno));

  struct sockaddr_un addr;
  ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ctx.server_fd < 0) {
    perror("socket");
    goto cleanup;
  }
  unlink(CONTROL_PATH);
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

  if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    goto cleanup;
  }
  if (listen(ctx.server_fd, 8) < 0) {
    perror("listen");
    goto cleanup;
  }

  g_ctx = &ctx;

  struct sigaction sa_chld, sa_term;
  memset(&sa_chld, 0, sizeof(sa_chld));
  sa_chld.sa_handler = sigchld_handler;
  sigemptyset(&sa_chld.sa_mask);
  sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa_chld, NULL);

  memset(&sa_term, 0, sizeof(sa_term));
  sa_term.sa_handler = sigterm_handler;
  sigemptyset(&sa_term.sa_mask);
  sa_term.sa_flags = 0;
  sigaction(SIGINT, &sa_term, NULL);
  sigaction(SIGTERM, &sa_term, NULL);

  if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
    perror("pthread_create logger");
    goto cleanup;
  }

  fprintf(stderr, "Supervisor started. Control socket: %s  rootfs base: %s\n",
          CONTROL_PATH, rootfs);

  while (!ctx.should_stop) {
    int client_fd;
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);
    control_request_t req;
    ssize_t n;

    client_fd =
        accept(ctx.server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (errno == EINTR)
        continue;
      perror("accept");
      break;
    }

    n = read(client_fd, &req, sizeof(req));
    if (n == (ssize_t)sizeof(req))
      handle_request(&ctx, client_fd, &req);
    else
      fprintf(stderr, "Supervisor: short read from client (%zd)\n", n);

    close(client_fd);
  }

  fprintf(stderr, "Supervisor shutting down...\n");

  {
    container_record_t *c;
    pthread_mutex_lock(&ctx.metadata_lock);
    for (c = ctx.containers; c; c = c->next) {
      if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) {
        c->stop_requested = 1;
        kill(c->host_pid, SIGTERM);
      }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    sleep(1);
  }

cleanup:
  bounded_buffer_begin_shutdown(&ctx.log_buffer);
  if (ctx.logger_thread)
    pthread_join(ctx.logger_thread, NULL);

  {
    container_record_t *c = ctx.containers;
    while (c) {
      container_record_t *next = c->next;
      if (c->log_read_fd >= 0)
        close(c->log_read_fd);
      free(c);
      c = next;
    }
  }

  bounded_buffer_destroy(&ctx.log_buffer);
  pthread_mutex_destroy(&ctx.metadata_lock);

  if (ctx.server_fd >= 0)
    close(ctx.server_fd);
  if (ctx.monitor_fd >= 0)
    close(ctx.monitor_fd);
  unlink(CONTROL_PATH);

  fprintf(stderr, "Supervisor exited cleanly.\n");
  return 0;
}

static int send_control_request(const control_request_t *req) {
  int sock_fd;
  struct sockaddr_un addr;
  ssize_t n;
  char buf[4096];
  ssize_t total;

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("socket");
    return 1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

  if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect: is the supervisor running?");
    close(sock_fd);
    return 1;
  }

  n = write(sock_fd, req, sizeof(*req));
  if (n != (ssize_t)sizeof(*req)) {
    perror("write request");
    close(sock_fd);
    return 1;
  }

  shutdown(sock_fd, SHUT_WR);

  total = 0;
  while ((n = read(sock_fd, buf, sizeof(buf))) > 0) {
    fwrite(buf, 1, (size_t)n, stdout);
    total += n;
  }
  (void)total;

  close(sock_fd);
  return 0;
}

static int cmd_start(int argc, char *argv[]) {
  control_request_t req;

  if (argc < 5) {
    fprintf(stderr,
            "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] "
            "[--hard-mib N] [--nice N]\n",
            argv[0]);
    return 1;
  }

  memset(&req, 0, sizeof(req));
  req.kind = CMD_START;
  strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
  strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
  strncpy(req.command, argv[4], sizeof(req.command) - 1);
  req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
  req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

  if (parse_optional_flags(&req, argc, argv, 5) != 0)
    return 1;

  return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[]) {
  control_request_t req;

  if (argc < 5) {
    fprintf(stderr,
            "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] "
            "[--hard-mib N] [--nice N]\n",
            argv[0]);
    return 1;
  }

  memset(&req, 0, sizeof(req));
  req.kind = CMD_RUN;
  strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
  strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
  strncpy(req.command, argv[4], sizeof(req.command) - 1);
  req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
  req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

  if (parse_optional_flags(&req, argc, argv, 5) != 0)
    return 1;

  return send_control_request(&req);
}

static int cmd_ps(void) {
  control_request_t req;

  memset(&req, 0, sizeof(req));
  req.kind = CMD_PS;

  printf("Expected states include: %s, %s, %s, %s, %s\n",
         state_to_string(CONTAINER_STARTING),
         state_to_string(CONTAINER_RUNNING), state_to_string(CONTAINER_STOPPED),
         state_to_string(CONTAINER_KILLED), state_to_string(CONTAINER_EXITED));
  return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[]) {
  control_request_t req;

  if (argc < 3) {
    fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
    return 1;
  }

  memset(&req, 0, sizeof(req));
  req.kind = CMD_LOGS;
  strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

  return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[]) {
  control_request_t req;

  if (argc < 3) {
    fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
    return 1;
  }

  memset(&req, 0, sizeof(req));
  req.kind = CMD_STOP;
  strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

  return send_control_request(&req);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  if (strcmp(argv[1], "supervisor") == 0) {
    if (argc < 3) {
      fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
      return 1;
    }
    return run_supervisor(argv[2]);
  }

  if (strcmp(argv[1], "start") == 0)
    return cmd_start(argc, argv);

  if (strcmp(argv[1], "run") == 0)
    return cmd_run(argc, argv);

  if (strcmp(argv[1], "ps") == 0)
    return cmd_ps();

  if (strcmp(argv[1], "logs") == 0)
    return cmd_logs(argc, argv);

  if (strcmp(argv[1], "stop") == 0)
    return cmd_stop(argc, argv);

  usage(argv[0]);
  return 1;
}
