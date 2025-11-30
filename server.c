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
#include <sys/wait.h>
#include <linux/limits.h>

// Used to be able to configure the bash path at compile time
#ifndef BASH_PATH
#define BASH_PATH "/bin/bash"
#endif

// No need to support that many concurrent connections
#define LISTEN_BACKLOG 10
// Should be plenty of space to receive any commands, at least for the current use-case
#define COMMAND_BUFFER_MAX_SIZE 4096
#define MAX_SECRET_SIZE (128 + 1)

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

static int str_prefix(const char *restrict prefix, const char *restrict string)
{
    while(*prefix) {
        if (*prefix++ != *string++) {
            return 0;
        }
    }

    return 1;
}

static int handle_client(int client_fd, char secret[MAX_SECRET_SIZE])
{
    struct timeval recv_timeout = { .tv_sec = 2 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

    char buffer[COMMAND_BUFFER_MAX_SIZE] = {0};
    int pos = 0;
    char *new_line_pos = NULL;

    while (pos < ARRAY_SIZE(buffer) - 1) {
        // The last byte in the buffer is not allowed to be read as it must be a NULL-terminated string
        ssize_t bytesRead = recv(client_fd, buffer + pos, ARRAY_SIZE(buffer) - pos - 1, 0);
        if (bytesRead < 0) {
            perror("recv");
            close(client_fd);
            return EXIT_FAILURE;
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
        return EXIT_FAILURE;
    }

    // NULL-terminated the buffer at the new line position
    *new_line_pos = 0;

    if (!str_prefix(secret, buffer)) {
        fprintf(stderr, "Secret didn't match. Not running request");
        return EXIT_FAILURE;
    }

    char *const command_str =  buffer + strlen(secret);

    fprintf(stderr, "Running: %s\n", command_str);

    // We got the command to be executed, the connection should be closed now
    close(client_fd);

    // Replace the process with the bash process
    char *const argv[] = { BASH_PATH, "-c", command_str, NULL};
    extern char **environ;
    execve(argv[0], argv, environ);

    // only reachable if execve failed
    return EXIT_FAILURE;
}

struct arguments {
    struct in_addr host;
    in_port_t port;
    char secret_path[PATH_MAX];
};

static struct arguments parse_arguments(int argc, char *const argv[]) 
{
    struct arguments args = {
        .host = { .s_addr = htonl(INADDR_LOOPBACK) },
        .port = htons(1337),
    };

    struct option long_options[] = {
        { .name = "host", .has_arg = 1, .flag = NULL, .val = 'H' },
        { .name = "port", .has_arg = 1, .flag = NULL, .val = 'p' },
        { .name = "secret-path", .has_arg = 1, .flag = NULL, .val = 's' },
        // { .name = "--help", .has_arg = 0, .flag = NULL, .val = 'h' },
        {0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, ":H:p:s:", long_options, NULL)) != -1) {
        unsigned long parsed_port;
        char *port_endptr;

        switch (opt) {
        case 'H':
            if (inet_aton(optarg, &args.host) == 0) {
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

            args.port = htons((uint16_t)parsed_port);
            break;
        case 's':
            strncpy(args.secret_path, optarg, ARRAY_SIZE(args.secret_path));
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

    return args;
}

static size_t read_contents(const char *path, char *out, size_t out_size)
{
    FILE *file = fopen(path, "r");
    size_t read = fread(out, sizeof(out[0]), sizeof(out[0]) * out_size, file);

    fclose(file);
    return read;
}

int main(int argc, char *const argv[])
{
    struct arguments args = parse_arguments(argc, argv);
    char secret[MAX_SECRET_SIZE] = {};

    // secret path was specified, read it
    if (args.secret_path[0] != '\0') {
        size_t read = read_contents(args.secret_path, secret, ARRAY_SIZE(secret));
        if (read >= MAX_SECRET_SIZE) {
            fprintf(stderr, "The secret was larger than %d", MAX_SECRET_SIZE - 1);
            return EXIT_FAILURE;
        }
    }

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

    fprintf(stderr, "Running bash command from %s\n", BASH_PATH);
    fprintf(stderr, "Listening on %s:%d PID: %d\n", host_str, ntohs(args.port), getpid());

    is_accepting_requests = 1;
    while(is_accepting_requests) {
        struct sockaddr_in client_address;
        socklen_t client_address_len = sizeof(client_address);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_address, &client_address_len);

        if (client_fd < 0) {
            perror("client");
            continue;
        }

        // I learned about fork, I will use and abuse fork
        pid_t handler_pid = fork();
        if (handler_pid == 0) {
            return handle_client(client_fd, secret);
        } else {
            close(client_fd);
        }
    }

    fprintf(stderr, "Closing...\n");

    // TODO: wait for children to finish
    close(listen_fd);

    return EXIT_SUCCESS;
}
