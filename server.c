#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>

// Used to be able to configure the bash path at compile time
#ifndef BASH_PATH
#define BASH_PATH "/bin/bash"
#endif

// No need to support that many concurrent connections
#define LISTEN_BACKLOG 10
// Should be plenty of space to receive any commands, at least for the current use-case
#define COMMAND_BUFFER_MAX_SIZE 4096

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static int listen_fd = -1;
static volatile sig_atomic_t is_accepting_requests = 0;

static void signal_handler(int signal) 
{
    (void)signal;

    if (is_accepting_requests && listen_fd != -1) {
        shutdown(listen_fd, SHUT_RD);
    }

    is_accepting_requests = 0;
}

static void setup_signal_handler() 
{
    int signals[] = { SIGINT, SIGTERM, SIGHUP };
    int signals_len = ARRAY_SIZE(signals);

    // Block all other signals while we are cleaning up
    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    for (int i = 0; i < signals_len; i++) {
        sigaddset(&sig_mask, signals[i]);
    }

    struct sigaction action = {
        .sa_handler = signal_handler,
        .sa_mask = sig_mask,
    };

    for (int i = 0; i < signals_len; i++) {
        if (sigaction(signals[i], &action, NULL) < 0) {
            perror("sigaction");
        }
    }
}

static void handle_client(int client_fd) 
{
    char buffer[COMMAND_BUFFER_MAX_SIZE] = {0};
    int pos = 0;
    char *new_line_pos = NULL;

    while (pos < ARRAY_SIZE(buffer) - 1) {
        // The last byte in the buffer is not allowed to be read as it must be a NULL-terminated string
        ssize_t bytesRead = recv(client_fd, buffer + pos, ARRAY_SIZE(buffer) - pos - 1, 0);
        if (bytesRead < 0) {
            perror("recv");
            close(client_fd);
            return;
        }

        new_line_pos = strchr(buffer + pos, '\n');
        if (new_line_pos != NULL) {
            break;
        }

        pos += bytesRead;
    }

    if (new_line_pos == NULL) {
        // We did not find a new line, so the command is not valid. Bail.
        close(client_fd);
        return;
    }

    // NULL-terminated the buffer at the new line position
    *new_line_pos = 0;
    printf("Running: %s", buffer);

    pid_t pid = fork();
    if (pid < 0)  {
        perror("fork");
        close(client_fd);
        return;
    }

    if (pid == 0) {
        close(client_fd);

        char *argv[] = { BASH_PATH, "-c", buffer, NULL};
        extern char **environ;
        execve(argv[0], argv, environ);

        // The child was not able to replace the process. Bail.
        exit(EXIT_FAILURE);
    }

    close(client_fd);
}

struct arguments {
    struct in_addr host;
    in_port_t port;
};

static struct arguments parse_arguments(int argc, char *const argv[]) 
{
    struct in_addr host = { .s_addr = htonl(INADDR_LOOPBACK) };
    in_port_t port = htons(1337);

    struct option long_options[] = {
        { .name = "host", .has_arg = 1, .flag = NULL, .val = 'H' },
        { .name = "port", .has_arg = 1, .flag = NULL, .val = 'p' },
        // { .name = "--help", .has_arg = 0, .flag = NULL, .val = 'h' },
        {0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, ":H:p:", long_options, NULL)) != -1) {
        unsigned long parsed_port;
        char *port_endptr;

        switch (opt) {
        case 'H':
            if (inet_aton(optarg, &host) == 0) {
                fprintf(stderr, "error: Invalid host: %s\n", optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case 'p':
            parsed_port = strtoul(optarg, &port_endptr, 10);
            if(*port_endptr != 0) {
                fprintf(stderr, "error: Invalid port: %s\n", optarg);
                exit(EXIT_FAILURE);
            }

            if (parsed_port < 0 || parsed_port > UINT16_MAX) {
                fprintf(stderr, "error: Port argument must be between %d and %d\n", 0, UINT16_MAX);
                exit(EXIT_FAILURE);
            }

            port = htons((uint16_t)parsed_port);
            break;
        case ':':
            fprintf(stderr, "error: Missing argument for option -%c\n", optopt);
            exit(EXIT_FAILURE);
        case '?':
        default:
            // Allow extra arguments without warnings
            break;
        }
    }

    return (struct arguments){ .host = host, .port = port};
}

int main(int argc, char *const argv[])
{
    struct arguments args = parse_arguments(argc, argv);

    setup_signal_handler();

    // SOCK_CLOEXEC is used to automatically close the socket if any exec* is successful
    listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_address = {
        .sin_family = AF_INET,
        .sin_addr = args.host,
        .sin_port = args.port,
    };

    int bind_result = bind(listen_fd, (const struct sockaddr *)&server_address, sizeof(server_address));
    if (bind_result < 0) {
        perror("bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    int listen_result = listen(listen_fd, LISTEN_BACKLOG);
    if (listen_result < 0) {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    char host_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &args.host.s_addr, host_str, sizeof(host_str));

    printf("Running bash command from %s\n", BASH_PATH);
    printf("Listening on %s:%d PID: %d\n", host_str, ntohs(args.port), getpid());

    is_accepting_requests = 1;
    while(is_accepting_requests) {
        fflush(stdout);

        struct sockaddr_in client_address;
        socklen_t client_address_len = sizeof(client_address);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_address, &client_address_len);

        if (client_fd < 0) {
            perror("client");
            continue;
        }

        handle_client(client_fd);
    }

    printf("Closing...\n");

    close(listen_fd);

    return EXIT_SUCCESS;
}
