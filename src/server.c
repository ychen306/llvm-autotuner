#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "common.h"

#define ERROR '\0'
#define SUCCESS '\1'

#define KILL '\0'

#define CANT_LOAD_LIB "unable to open library"
#define CANT_LOAD_FUNC "unable to load function from library"

#define OUT_FILENAME "worker-data.txt"
#define LIBPATH_MAX_LEN 100
#define MAXFD 256
#define DEFAULT_MAX_CLIENT 4
#define MAX_CLIENT
// one control byte + path + description
#define RESP_SIZE (LIBPATH_MAX_LEN + 1 + 100)

void _server_init() __attribute__ ((constructor));
void _server_shutdown() __attribute__ ((destructor)); 

FILE *out_file = NULL;
int max_client; 

int is_parent = 1;

// make an error response
static inline void *make_error(char *msg)
{ 
    char *resp = calloc(1, RESP_SIZE);
    resp[0] = ERROR;
    strcpy(resp+1, msg);
    return resp;
}

// make a success response
static inline void *make_report(float time_spent)
{ 
    char *resp = calloc(1, RESP_SIZE);
    resp[0] = SUCCESS;
    float *time_ptr = (float *)(resp + 1);
    *time_ptr = time_spent;
    return resp;
}

static inline void *respond(int fd, void *resp)
{ 
    write(fd, resp, RESP_SIZE);
    free(resp);
    exit(0);
}

static inline void dump_worker_data(const char *funcname, const char *sock_path)
{ 
    fprintf(out_file, "%s\t%s\n", funcname, sock_path);
    // in case buffer gets written multiple times when
    // forked processes exits
    fflush(out_file);
} 

void _server_spawn_worker(
        void (*orig_func)(void *), char *funcname, void *args, uint32_t *workers_to_spawn)
{ 
    // a new worker can only be spawned if
    // 1) current process if the parent process
    // 2) doing so won't spawn more workers than asked for
#define CAN_SPAWN (is_parent && *workers_to_spawn > 0)

    // use the first byte as control byte
    // and the rest as lib_path
    char msg[LIBPATH_MAX_LEN+1]; 
    memset(msg, 0, sizeof msg);

    char sock_path[100] = "/tmp/tuning-XXXXXX";
    if (CAN_SPAWN) {
        mkdtemp(sock_path);
        strcat(sock_path, "/socket");
    }
    
    if (CAN_SPAWN && fork()) { // body of worker process
        is_parent = 0;

        daemon(1, 0);
        struct sockaddr_un addr;
        int sockfd;

        if ((sockfd=socket(AF_UNIX, SOCK_STREAM, 0)) == - 1) {
            exit(-1);
        }

        memset(&addr, 0, sizeof (addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path, (sizeof (addr.sun_path))-1);

        if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            exit(-1);
        }

        if (listen(sockfd, max_client) == -1) {
            exit(-1);
        }

        for (;;) {
            int cli_fd;
            if ((cli_fd=accept(sockfd, NULL, NULL)) == -1) {
                continue;
            }

            read(cli_fd, msg, LIBPATH_MAX_LEN);

            // read control byte and see if needs to kill current worker
            if (msg[0] == KILL) {
                close(cli_fd);
                break;
            }

            if (fork()) { 
                char *lib_path = &msg[1];

                // lookup the function from shared library
                void *lib = dlopen(lib_path, RTLD_NOW);
                if (!lib) respond(cli_fd, make_error(CANT_LOAD_LIB));

                void *(*func)(void *) = dlsym(lib, funcname); 
                if (!func) respond(cli_fd, make_error(CANT_LOAD_FUNC));

                // run the function
                struct timespec begin, end;
                clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &begin);
                func(args); 
                clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
                float time_spent = (float) ((end.tv_sec-begin.tv_sec) * 1e9 +
                        (end.tv_nsec - begin.tv_nsec));

                respond(cli_fd, make_report(time_spent));
            }

            memset(msg, 0, sizeof msg);
            close(cli_fd);
        }
        unlink(sock_path);
        exit(0);
    } else { // body of parent process 
        if (CAN_SPAWN) {
            *workers_to_spawn -= 1;
            dump_worker_data(funcname, sock_path);
        }
        orig_func(args); 
    }
#undef CAN_SPAWN
}

void _server_init()
{
    const char *max_client_str = getenv("MAX_CLIENT");
    if (!max_client_str) {
        max_client = DEFAULT_MAX_CLIENT;
    } else { 
        max_client = atoi(max_client_str);
        if (!max_client) {
            fprintf(stderr, "invalid $MAX_CLIENT\n");
            exit(-1);
        }
    }

    out_file = fopen(OUT_FILENAME, "wb");
}

void _server_shutdown()
{
    fclose(out_file);
}
