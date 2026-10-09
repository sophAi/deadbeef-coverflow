/*
 * DeaDBeeF Cover Flow - Native C Launcher
 *
 * Provides instant CLI window activation, single-instance control,
 * and background residency in MATE desktop panel / system tray.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define DEADBEEF_PATH "/opt/deadbeef-coverflow/deadbeef"
#define PLUGIN_DIR "/opt/deadbeef-coverflow/plugins"
#define LIB_DIR "/opt/deadbeef-coverflow/lib"

static void get_socket_path(char *path, size_t size) {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        snprintf(path, size, "%s/deadbeef/socket", runtime_dir);
    } else {
        snprintf(path, size, "/run/user/%d/deadbeef/socket", getuid());
    }
}

static int connect_to_deadbeef(const char *sock_path) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int send_activate(const char *sock_path) {
    int s = connect_to_deadbeef(sock_path);
    if (s < 0) {
        return -1;
    }
    char msg = 0;
    if (send(s, &msg, 1, 0) != 1) {
        close(s);
        return -1;
    }
    shutdown(s, SHUT_WR);
    char buf[16];
    recv(s, buf, sizeof(buf), 0);
    close(s);
    return 0;
}

static void setup_env(void) {
    setenv("DEADBEEF_PLUGIN_DIR", PLUGIN_DIR, 1);
    const char *cur_ld = getenv("LD_LIBRARY_PATH");
    char new_ld[4096];
    if (cur_ld && cur_ld[0]) {
        snprintf(new_ld, sizeof(new_ld), "%s:%s", LIB_DIR, cur_ld);
    } else {
        snprintf(new_ld, sizeof(new_ld), "%s", LIB_DIR);
    }
    setenv("LD_LIBRARY_PATH", new_ld, 1);
}

int main(int argc, char *argv[]) {
    char sock_path[512];
    get_socket_path(sock_path, sizeof(sock_path));

    int has_show = 0;
    int remaining_argc = 0;
    char **remaining_argv = malloc((argc + 4) * sizeof(char *));
    if (!remaining_argv) {
        perror("malloc");
        return 1;
    }

    remaining_argv[0] = DEADBEEF_PATH;
    remaining_argv[1] = "--gui";
    remaining_argv[2] = "GTK3";
    int r_idx = 3;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--show") || !strcmp(argv[i], "--show-ui")) {
            has_show = 1;
        } else {
            remaining_argv[r_idx++] = argv[i];
            remaining_argc++;
        }
    }
    remaining_argv[r_idx] = NULL;

    setup_env();

    // Check if DeaDBeeF is already running in background
    int test_s = connect_to_deadbeef(sock_path);
    int is_running = (test_s >= 0);
    if (test_s >= 0) {
        close(test_s);
    }

    if (has_show) {
        if (is_running) {
            // State 1: Resident in background -> Activate main window
            if (remaining_argc > 0) {
                // If extra arguments/files exist, pass them to DeaDBeeF first
                pid_t pid = fork();
                if (pid == 0) {
                    execv(DEADBEEF_PATH, remaining_argv);
                    _exit(1);
                }
                waitpid(pid, NULL, 0);
            }
            send_activate(sock_path);
            free(remaining_argv);
            return 0;
        } else {
            // State 2: Not running -> Launch DeaDBeeF, wait for socket, display window
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                execv(DEADBEEF_PATH, remaining_argv);
                perror("execv");
                _exit(1);
            }

            // Wait until DeaDBeeF socket is ready (poll up to 2 seconds)
            for (int i = 0; i < 100; i++) {
                usleep(20000); // 20ms
                if (send_activate(sock_path) == 0) {
                    break;
                }
            }
            free(remaining_argv);
            return 0;
        }
    }

    // State 3: Invoked with no arguments (e.g. system autostart)
    if (argc <= 1) {
        if (is_running) {
            // Already resident in background -> do nothing, stay quiet
            free(remaining_argv);
            return 0;
        } else {
            // Not running -> Start resident in mate-panel (gtkui.start_hidden 1)
            execv(DEADBEEF_PATH, remaining_argv);
            perror("execv");
            free(remaining_argv);
            return 1;
        }
    }

    // Normal command line execution without --show (e.g. --play, --next, files)
    execv(DEADBEEF_PATH, remaining_argv);
    perror("execv");
    free(remaining_argv);
    return 1;
}
