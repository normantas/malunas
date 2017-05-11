#define _XOPEN_SOURCE 700
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <getopt.h>
#include <poll.h>

#include "exec.h"
#include "proxy.h"

#define program_name "malunas"

static struct option const longopts[] = {
    {"workers", required_argument, NULL, 'w'},
    {NULL, 0, NULL, 0}
};

typedef struct {
    char *name;
    void (*handle_func) (int, int, int, char **);
} t_modulecfg;

t_modulecfg modules[] = {
    {"exec", mlns_exec_handle},
    {"proxy", mlns_proxy_handle}
};

void usage(int status)
{
    printf
        ("usage: %s [-w|--workers] <port> [<handler> [<args>]]\n\n",
         program_name);

    fputs("\
Listens for incomming connections on a TCP port. All the data that is\n\
received and sent from an accepted connection socket is mapped to one of the\n\
supported handlers.\n\
\n", stdout);


    fputs("\
OPTIONS:\n\
Mandatory arguments to long options are mandatory for short options too.\n\
  -w, --workers=NUMBER adjust number of preforked workers that accept connections\n\
\n", stdout);


    fputs("\
HANDLERS:\n\
  exec     maps to input and output of a locally executed command\n\
  proxy    forward socket data to a new TCP connection\n\
\n", stdout);

    printf("Run '%s HANDLER --help' for more information on a handler.\n",
           program_name);

    exit(status);
}

void sigchld_handler(int s)
{
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *) sa)->sin_addr);
    }
    return &(((struct sockaddr_in6 *) sa)->sin6_addr);
}

in_port_t *get_in_port(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *) sa)->sin_port);
    }
    return &(((struct sockaddr_in6 *) sa)->sin6_port);
}

int main(int argc, char *argv[])
{
    char *port_to;
    int rc;
    struct addrinfo hints;
    struct addrinfo *res;
    int sockfd;
    int conn_fd;
    struct sockaddr_storage their_addr;
    struct sigaction sa;
    char s[INET6_ADDRSTRLEN];
    int c;
    int workers;
    int tty;

    tty = 0;
    workers = 2;
    while ((c = getopt_long(argc, argv, "+w:", longopts, NULL)) != -1) {
        switch (c) {
        case 'w':
            workers = atoi(optarg);
            break;
        case '?':
            if (optopt == 'f') {
                fprintf(stderr, "Option -%c requires an arg.\n", optopt);
            }
        default:
            usage(1);
        }
    }

    int i;
    int ac;

    char **av;
    av = argv + optind;
    ac = argc - optind;

    port_to = av[0];
    av++;
    ac--;

    if (port_to == 0) {
        fprintf(stderr, "Port is not specified.\n");
        usage(-1);
    }

    char *handler = av[0];

    t_modulecfg *module = &modules[0];
    int len = sizeof(modules) / sizeof(modules[0]);

    while (strcmp(handler, module->name) != 0) {
        // is it last module?
        if (module == &modules[len - 1]) {
            fprintf(stderr, "Module '%s' is not a valid module\n", handler);
            exit(1);
        }
        module++;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(NULL, port_to, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo for port %s: %s\n", port_to,
                gai_strerror(rc));
        exit(EXIT_FAILURE);
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (!sockfd) {
        perror("socket");
        exit(1);
    }

    rc = bind(sockfd, res->ai_addr, res->ai_addrlen);
    if (rc != 0) {
        close(sockfd);
        perror("bind");
        exit(1);
    }

    rc = getsockname(sockfd, res->ai_addr, &res->ai_addrlen);
    if (rc != 0) {
        fprintf(stderr, "getsockname for port %s: %s\n", port_to,
                gai_strerror(rc));
        exit(EXIT_FAILURE);
    }

    rc = listen(sockfd, 5);
    if (rc != 0) {
        close(sockfd);
        perror("listen");
        exit(1);
    }

    freeaddrinfo(res);

    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    struct sockaddr *sa1 = res->ai_addr;
    inet_ntop(sa1->sa_family, get_in_addr(sa1), s, sizeof s);
    fprintf(stderr, "%s: Listening on %s:%d\n",
            program_name, s, ntohs(*get_in_port(sa1)));

    int logs[workers][2];
    char *worker_names[workers];

    struct pollfd poll_fds[workers];

    for (i = 0; i < workers; i++) {
        pid_t pid;
        pipe(logs[i]);

        poll_fds[i].fd = logs[i][0];
        poll_fds[i].events = POLLIN;

        if ((pid = fork()) == -1) {
            exit(1);
        } else if (pid == 0) {

            close(logs[i][0]);
            int log = logs[i][1];

            // Accept loop
            do {
                socklen_t addr_size;
                addr_size = sizeof their_addr;

                dprintf(log, "waiting for a connection...");

                conn_fd =
                    accept(sockfd, (struct sockaddr *) &their_addr, &addr_size);

                inet_ntop(their_addr.ss_family,
                          get_in_addr((struct sockaddr *) &their_addr), s,
                          sizeof s);

                dprintf(log,
                        "accepted connection from %s (socket FD: %d)",
                        s, conn_fd);

                module->handle_func(conn_fd, log, ac, av);

                close(conn_fd);
            } while (1);
        }

        close(logs[i][1]);

        char *worker_name = malloc(10);
        sprintf(worker_name, "PID:%d", pid);
        worker_names[i] = worker_name;
        worker_names[i][9] = 0;
    }

    do {
        int ret = poll((struct pollfd *) &poll_fds, workers, 1000);
        if (ret == -1) {
            perror("poll");
            break;
        } else if (ret == 0) {
            // Poll timeout
        } else {

            for (i = 0; i < workers; i++) {
                if (poll_fds[i].revents & POLLIN) {
                    poll_fds[i].revents -= POLLIN;

                    int n;
                    char buf[0x100 + 1] = { 0 };
                    n = read(poll_fds[i].fd, buf, 0x100);
                    buf[n] = 0;
                    dprintf(2, "%s >>> %s\n", worker_names[i], buf);
                }
            }
        }
    } while (1);

    while (wait(NULL) > 0);
}
