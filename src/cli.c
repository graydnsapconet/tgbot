#define _POSIX_C_SOURCE 200809L

#include "cli.h"
#include "cfg.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVICE_NAME "tgbot-service.service"

static void print_usage(void)
{
    printf("Usage: tgbot [command] [options]\n"
           "\n"
           "Commands:\n"
           "  run              Run the bot daemon (default if no command given)\n"
           "  start            Start the systemd service\n"
           "  stop             Stop the systemd service\n"
           "  stop -f          Force-stop the service (SIGKILL)\n"
           "  restart          Restart the systemd service\n"
           "  status           Show service status\n"
           "  logs [-n N] [-f] Show log output (last N lines, or follow)\n"
           "  help, --help     Show this help message\n");
}

// fork + exec a command.  Returns the child exit code, or -1 on error
static int run_cmd(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("tgbot: fork");
        return -1;
    }
    if (pid == 0) {
        // child - cast away const for execvp (POSIX signature)
        execvp(argv[0], (char *const *)argv);
        perror("tgbot: exec");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("tgbot: waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

// subcommand handlers
static int cmd_start(void)
{
    const char *args[] = {"systemctl", "start", SERVICE_NAME, NULL};
    return run_cmd(args);
}

static int cmd_stop(int force)
{
    if (force) {
        const char *args[] = {"systemctl", "kill", "--signal=SIGKILL", SERVICE_NAME, NULL};
        return run_cmd(args);
    }
    const char *args[] = {"systemctl", "stop", SERVICE_NAME, NULL};
    return run_cmd(args);
}

static int cmd_restart(void)
{
    const char *args[] = {"systemctl", "restart", SERVICE_NAME, NULL};
    return run_cmd(args);
}

static int cmd_status(void)
{
    const char *args[] = {"systemctl", "status", SERVICE_NAME, NULL};
    return run_cmd(args);
}

static int cmd_logs(int argc, char **argv)
{
    int n = 20;    // default lines
    int follow = 0;

    // parse sub-options: -n <N>, -f
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n = atoi(argv[++i]);
            if (n <= 0) {
                n = 20;
            }
        } else if (strcmp(argv[i], "-f") == 0) {
            follow = 1;
        }
    }

    // load config to discover log path
    Config cfg;
    if (config_load(&cfg, "tgbot.ini") != 0) {
        /* Fallback: try /etc/tgbot/tgbot.ini */
        if (config_load(&cfg, "/etc/tgbot/tgbot.ini") != 0) {
            fprintf(stderr, "tgbot: cannot determine log path from config\n");
            return 1;
        }
    }

    if (follow) {
        // print last n lines then follow
        log_read_last_n(cfg.log_path, n);
        return log_follow(cfg.log_path);
    }

    return log_read_last_n(cfg.log_path, n);
}

// public dispatch
int cli_dispatch(int argc, char **argv, int *exit_code)
{
    if (argc < 2) {
        return 1; // no subcommand - run daemon
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "run") == 0) {
        return 1; // explicit daemon mode
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage();
        *exit_code = 0;
        return 0;
    }

    if (strcmp(cmd, "start") == 0) {
        *exit_code = cmd_start();
        return 0;
    }

    if (strcmp(cmd, "stop") == 0) {
        int force = (argc >= 3 && strcmp(argv[2], "-f") == 0);
        *exit_code = cmd_stop(force);
        return 0;
    }

    if (strcmp(cmd, "restart") == 0) {
        *exit_code = cmd_restart();
        return 0;
    }

    if (strcmp(cmd, "status") == 0) {
        *exit_code = cmd_status();
        return 0;
    }

    if (strcmp(cmd, "logs") == 0) {
        *exit_code = cmd_logs(argc - 2, argv + 2);
        return 0;
    }

    fprintf(stderr, "tgbot: unknown command '%s'\n\n", cmd);
    print_usage();
    *exit_code = 1;
    return 0;
}
