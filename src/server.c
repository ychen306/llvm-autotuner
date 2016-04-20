#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/un.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "common.h"

#define KILL '\0'

#define LIBPATH_MAX_LEN 100

#define CANT_LOAD_LIB "unable to open library"
#define CANT_LOAD_FUNC "unable to load function from library"

#define OUT_FILENAME "worker-data.txt"
#define MAXFD 256
#define MAX_CLIENT

extern uint32_t _server_invos[];
extern uint32_t _server_num_invos;

void _server_init() __attribute__((constructor));

int max_client;

int is_parent = 1;

struct response {
  int success;
  double elapsed;
  char msg[LIBPATH_MAX_LEN + 100];
};

static inline struct response *make_error(char *msg) {
  struct response *resp = malloc(sizeof(struct response));
  resp->success = 0;
  strcpy(resp->msg, msg);
  return resp;
}

static inline struct response *make_report(float time_spent) {
  struct response *resp = malloc(sizeof(struct response));
  resp->success = 1;
  resp->elapsed = time_spent;
  return resp;
}

// send response to the client and kill current process
static inline void respond(int fd, struct response *resp) {
  write(fd, resp, sizeof(struct response));
  free(resp);
  close(fd);
  _exit(0);
}

static inline void dump_worker_data(const char *sock_path) {
  FILE *out_file = fopen(OUT_FILENAME, "a");
  fprintf(out_file, "%s\n", sock_path);
  fclose(out_file);
}

void handle_sigchld(int sig) {
  while (waitpid((pid_t)(-1), 0, WNOHANG) > 0) {
  }
}

uint32_t _server_spawn_worker(uint32_t (*orig_func)(void *), char *funcname,
                              void *args) {
  static int invo = 0;

  int can_spawn = 0;
  invo++;
  if (is_parent) {
    uint32_t i;
    for (i = 0; i < _server_num_invos; i++)
      if (_server_invos[i] == invo) {
        can_spawn = 1;
        break;
      }
  }

  char msg[LIBPATH_MAX_LEN];
  memset(msg, 0, sizeof msg);

  char sock_path[100] = "/tmp/tuning-XXXXXX";
  if (can_spawn) {
    mkdtemp(sock_path);
    strcat(sock_path, "/socket");
  }

  if (can_spawn && fork() == 0) { // body of worker process
    is_parent = 0;

    daemon(1, 0);
    struct sockaddr_un addr;
    int sockfd;

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
      exit(-1);
    }

    struct sigaction sa;
    sa.sa_handler = &handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, 0) == -1) {
      perror(0);
      exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, (sizeof(addr.sun_path)) - 1);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
      exit(-1);
    }

    if (listen(sockfd, max_client) == -1) {
      exit(-1);
    }

    for (;;) {
      int cli_fd;
      if ((cli_fd = accept(sockfd, NULL, NULL)) == -1) {
        continue;
      }

      if (read(cli_fd, msg, LIBPATH_MAX_LEN) <= 0) {
        continue;
      }

      // read control byte and see if needs to kill current worker
      if (msg[0] == KILL) {
        close(cli_fd);
        break;
      }

      if (fork() == 0) {
        close(sockfd);

        // lookup the function from shared library
        void *lib = dlopen(msg, RTLD_NOW);
        if (!lib) {
          respond(cli_fd, make_error(dlerror()));
        }

        void *(*func)(void *) = dlsym(lib, funcname);
        if (!func) {
          respond(cli_fd, make_error(dlerror()));
        }

        // run the function
        struct timespec begin, end;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        func(args);
        clock_gettime(CLOCK_MONOTONIC, &end);
        float time_spent = (float)((end.tv_sec - begin.tv_sec) * 1e9 +
                                   (end.tv_nsec - begin.tv_nsec));

        respond(cli_fd, make_report(time_spent));
      }

      memset(msg, 0, sizeof msg);
      close(cli_fd);
    }
    unlink(sock_path);
    exit(0);
  } else { // body of parent process
    if (can_spawn) {
      dump_worker_data(sock_path);
    }
    return orig_func(args);
  }
}

void _server_init() {
  max_client = sysconf(_SC_NPROCESSORS_ONLN);
  remove(OUT_FILENAME);
}
