/*
 * osc-jack-play.c
 *
 * OSC -> JACK wav playback client.
 *
 * Listens for OSC messages on a UDP port and plays a wav file through the
 * jack-play(1) tool from the jack-tools package:
 *
 *   /play <index>   start playing wav file <index>
 *                   (0-based by default, see -1 for 1-based)
 *                   ignored if a file is already playing
 *   /stop           stop the current playback (SIGTERM to jack-play)
 *
 * The wav files are given as positional arguments; /play <index> selects
 * the index-th file.  <index> may arrive as an OSC int ('i') or float ('f').
 *
 * RNBO (rnbooscquery image): on startup the client registers itself as an
 * RNBO listener by sending '/rnbo/listeners/add host:port' to the RNBO
 * oscquery service (default 127.0.0.1:1234), because RNBO only streams OSC
 * to registered listeners.  See the --rnbo-* and --no-register options.
 *
 * Build (target needs liblo-dev and jack-tools installed):
 *   make
 *
 * Example:
 *   ./osc-jack-play -c 'system:playback_%d' sound1.wav sound2.wav \
 *       sound3.wav sound4.wav
 */

#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <lo/lo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* global state                                                       */

static volatile sig_atomic_t g_busy = 0;        /* a jack-play child is running */
static volatile sig_atomic_t g_child_exited = 0; /* child exit status pending  */
static volatile sig_atomic_t g_child_status = 0;
static pid_t g_child_pid = 0;
static volatile sig_atomic_t g_running = 1;

static char **g_files = NULL;
static int g_nfiles = 0;
static int g_one_based = 0;
static const char *g_connect = NULL;    /* JACK_PLAY_CONNECT_TO pattern */
static const char *g_client_name = NULL;
static const char *g_port = "7000";            /* our OSC UDP port */
static const char *g_rnbo_host = "127.0.0.1";   /* RNBO oscquery host */
static const char *g_rnbo_port = "1234";        /* RNBO oscquery port */
static const char *g_advertise_host = "127.0.0.1"; /* host in listener addr */
static int g_register = 1;                      /* register as RNBO listener */

/* ------------------------------------------------------------------ */
/* small helpers                                                      */

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [options] wavfile...\n"
        "\n"
        "Listens for OSC messages and plays wav files through jack-play(1).\n"
        "\n"
        "options:\n"
        "  -p, --port PORT        OSC UDP port to listen on (default 7000)\n"
        "  -c, --connect PORTS    JACK_PLAY_CONNECT_TO pattern for jack-play,\n"
        "                         e.g. 'system:playback_%%d' (%%d is replaced by\n"
        "                         the file's channel number, 1-based). Without\n"
        "                         this, jack-play makes no connections; use\n"
        "                         jack_connect(1) or jack-plumbing(1) instead.\n"
        "  -n, --client-name NAME set jack-play's JACK client name\n"
        "  -1, --one-based        /play indices are 1-based (default 0-based)\n"
        "  -r, --rnbo-port PORT   RNBO oscquery port to register with\n"
        "                         (default 1234)\n"
        "      --rnbo-host HOST   RNBO oscquery host (default 127.0.0.1)\n"
        "      --advertise-host HOST  host advertised in the listener address\n"
        "                         (default 127.0.0.1; set to this Pi's IP if\n"
        "                         RNBO runs on another machine)\n"
        "      --no-register      do not register as an RNBO listener\n"
        "  -h, --help             show this help\n"
        "\n"
        "OSC messages:\n"
        "  /play <i>   play wavfile[i] (ignored while another file is playing)\n"
        "  /stop       stop current playback\n"
        "\n"
        "RNBO: on startup the client sends '/rnbo/listeners/add <host:port>'\n"
        "to the RNBO oscquery service so RNBO streams its OSC output here.\n"
        "Disable with --no-register (e.g. for plain OSC senders).\n"
        "\n"
        "example:\n"
        "  %s -c 'system:playback_%%d' sound1.wav sound2.wav sound3.wav sound4.wav\n",
        prog, prog);
}

static void install_signal(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* RNBO listener registration                                         */

/* Extract the port the server actually bound to (handles port "0"). */
static void server_port(lo_server server, char *out, size_t outsz)
{
    const char *url = lo_server_get_url(server);
    const char *p = url ? strrchr(url, ':') : NULL;
    const char *q = p ? strchr(p, '/') : NULL;
    if (p != NULL && q != NULL && (size_t)(q - p - 1) < outsz) {
        size_t len = (size_t)(q - p - 1);
        memcpy(out, p + 1, len);
        out[len] = '\0';
        return;
    }
    snprintf(out, outsz, "%s", g_port);
}

/* Register (add=1) or unregister (add=0) as an RNBO listener. */
static void rnbo_listener_update(lo_server server, int add)
{
    char listener[128];
    char our_port[16];

    server_port(server, our_port, sizeof(our_port));
    int n = snprintf(listener, sizeof(listener), "%s:%s",
                     g_advertise_host, our_port);
    if (n < 0 || (size_t)n >= sizeof(listener)) {
        fprintf(stderr, "warning: listener address too long, aborting\n");
        return;
    }

    lo_address rnbo = lo_address_new(g_rnbo_host, g_rnbo_port);
    if (rnbo == NULL) {
        fprintf(stderr, "warning: could not create RNBO address %s:%s\n",
                g_rnbo_host, g_rnbo_port);
        return;
    }
    const char *path =
        add ? "/rnbo/listeners/add" : "/rnbo/listeners/remove";
    if (lo_send_from(rnbo, server, LO_TT_IMMEDIATE, path, "s", listener) < 0) {
        fprintf(stderr, "warning: could not %s listener with RNBO at %s:%s "
                "(is the RNBO oscquery service running?)\n",
                add ? "register" : "unregister", g_rnbo_host, g_rnbo_port);
    } else {
        printf("osc-jack-play: %s listener '%s' with RNBO at %s:%s\n",
               add ? "registered" : "unregistered", listener,
               g_rnbo_host, g_rnbo_port);
        fflush(stdout);
    }
    lo_address_free(rnbo);
}

/* ------------------------------------------------------------------ */
/* signal handlers                                                    */

static void sigchld_handler(int sig)
{
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
        g_child_status = status;
        g_child_exited = 1;
        g_busy = 0;
    }
    (void)sig;
}

static void stop_signal_handler(int sig)
{
    g_running = 0;
    (void)sig;
}

/* ------------------------------------------------------------------ */
/* OSC handlers                                                       */

static void osc_error(int num, const char *msg, const char *where)
{
    fprintf(stderr, "liblo error %d in %s: %s\n", num, where, msg);
}

static int play_handler(const char *path, const char *types, lo_arg **argv,
                        int argc, lo_message msg, void *user_data)
{
    int idx;

    (void)path;
    (void)msg;
    (void)user_data;

    if (argc < 1) {
        fprintf(stderr, "/play: missing index argument\n");
        return 0;
    }
    if (types[0] == 'i') {
        idx = argv[0]->i;
    } else if (types[0] == 'f') {
        idx = (int)argv[0]->f;
    } else {
        fprintf(stderr, "/play: expected an int or float argument, got '%c'\n",
                types[0]);
        return 0;
    }

    if (g_one_based) {
        if (idx < 1 || idx > g_nfiles) {
            fprintf(stderr, "/play: index %d out of range 1..%d\n", idx, g_nfiles);
            return 0;
        }
        idx -= 1;
    } else {
        if (idx < 0 || idx >= g_nfiles) {
            fprintf(stderr, "/play: index %d out of range 0..%d\n", idx,
                    g_nfiles - 1);
            return 0;
        }
    }

    if (g_busy) {
        fprintf(stderr, "/play %d: ignored, playback already in progress\n", idx);
        return 0;
    }

    /* fork + exec jack-play; the SIGCHLD handler reaps it and clears g_busy */
    g_busy = 1;
    pid_t pid = fork();
    if (pid < 0) {
        g_busy = 0;
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        /* child: exec jack-play */
        if (g_connect != NULL)
            setenv("JACK_PLAY_CONNECT_TO", g_connect, 1);
        char *args[8];
        int n = 0;
        args[n++] = "jack-play";
        if (g_client_name != NULL) {
            args[n++] = "-n";
            args[n++] = (char *)g_client_name;
        }
        args[n++] = g_files[idx];
        args[n] = NULL;
        execvp("jack-play", args);
        fprintf(stderr, "exec jack-play failed: %s\n", strerror(errno));
        _exit(127);
    }

    g_child_pid = pid;
    fprintf(stderr, "/play %d: starting jack-play with %s (pid %ld)\n",
            idx, g_files[idx], (long)pid);
    return 0;
}

static int stop_handler(const char *path, const char *types, lo_arg **argv,
                        int argc, lo_message msg, void *user_data)
{
    (void)path;
    (void)types;
    (void)argv;
    (void)argc;
    (void)msg;
    (void)user_data;

    if (g_busy && g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        fprintf(stderr, "/stop: sent SIGTERM to pid %ld\n", (long)g_child_pid);
    } else {
        fprintf(stderr, "/stop: nothing playing\n");
    }
    return 0;
}

/* RNBO echoes our listener add/remove requests back to us. */
static int rnbo_reply_handler(const char *path, const char *types,
                              lo_arg **argv, int argc, lo_message msg,
                              void *user_data)
{
    (void)types;
    (void)argv;
    (void)argc;
    (void)msg;
    (void)user_data;
    fprintf(stderr, "RNBO reply: %s\n", path);
    return 0;
}

static char g_seen_paths[16][64];

static int generic_handler(const char *path, const char *types, lo_arg **argv,
                           int argc, lo_message msg, void *user_data)
{
    (void)types;
    (void)argv;
    (void)argc;
    (void)msg;
    (void)user_data;
    /* Log each unknown path once, to avoid flooding on RNBO param streams. */
    for (int i = 0; i < 16; i++) {
        if (g_seen_paths[i][0] == '\0') {
            strncpy(g_seen_paths[i], path, sizeof(g_seen_paths[i]) - 1);
            g_seen_paths[i][sizeof(g_seen_paths[i]) - 1] = '\0';
            fprintf(stderr, "unhandled OSC message: %s (repeats not logged)\n",
                    path);
            return 0;
        }
        if (strcmp(g_seen_paths[i], path) == 0)
            return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */

int main(int argc, char **argv)
{
    int opt;

    static const struct option longopts[] = {
        { "port",           required_argument, NULL, 'p' },
        { "connect",        required_argument, NULL, 'c' },
        { "client-name",    required_argument, NULL, 'n' },
        { "one-based",      no_argument,       NULL, '1' },
        { "rnbo-port",      required_argument, NULL, 'r' },
        { "rnbo-host",      required_argument, NULL, 256 },
        { "advertise-host", required_argument, NULL, 257 },
        { "no-register",    no_argument,       NULL, 258 },
        { "help",           no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "p:c:n:1hr:", longopts, NULL)) != -1) {
        switch (opt) {
        case 'p': g_port = optarg; break;
        case 'c': g_connect = optarg; break;
        case 'n': g_client_name = optarg; break;
        case '1': g_one_based = 1; break;
        case 'r': g_rnbo_port = optarg; break;
        case 256: g_rnbo_host = optarg; break;
        case 257: g_advertise_host = optarg; break;
        case 258: g_register = 0; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    g_files = &argv[optind];
    g_nfiles = argc - optind;
    if (g_nfiles < 1) {
        fprintf(stderr, "error: at least one wav file is required\n");
        usage(argv[0]);
        return 1;
    }

    install_signal(SIGCHLD, sigchld_handler);
    install_signal(SIGINT, stop_signal_handler);
    install_signal(SIGTERM, stop_signal_handler);

    lo_server server = lo_server_new(g_port, osc_error);
    if (server == NULL) {
        fprintf(stderr, "error: could not open OSC server on UDP port %s\n",
                g_port);
        return 1;
    }
    lo_server_add_method(server, "/play", "i", play_handler, NULL);
    lo_server_add_method(server, "/play", "f", play_handler, NULL);
    lo_server_add_method(server, "/stop", "", stop_handler, NULL);
    lo_server_add_method(server, "/rnbo/listeners/add", NULL,
                         rnbo_reply_handler, NULL);
    lo_server_add_method(server, "/rnbo/listeners/remove", NULL,
                         rnbo_reply_handler, NULL);
    lo_server_add_method(server, NULL, NULL, generic_handler, NULL);

    printf("osc-jack-play: listening on %s\n", lo_server_get_url(server));
    if (g_register)
        rnbo_listener_update(server, 1);
    printf("osc-jack-play: %d wav file%s:\n", g_nfiles, g_nfiles == 1 ? "" : "s");
    for (int i = 0; i < g_nfiles; i++)
        printf("  [%d] %s\n", g_one_based ? i + 1 : i, g_files[i]);
    printf("osc-jack-play: send /play <index> to start playback "
           "(ignored while playing), /stop to stop\n");
    fflush(stdout);

    while (g_running) {
        lo_server_recv_noblock(server, 10);
        if (g_child_exited) {
            g_child_exited = 0;
            int status = g_child_status;
            if (WIFEXITED(status))
                fprintf(stderr, "playback finished (exit %d)\n",
                        WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                fprintf(stderr, "playback terminated (signal %d)\n",
                        WTERMSIG(status));
        }
    }

    /* shutdown: stop any running playback */
    if (g_busy && g_child_pid > 0) {
        fprintf(stderr, "shutting down, stopping playback...\n");
        kill(g_child_pid, SIGTERM);
        for (int i = 0; i < 50 && g_busy; i++)
            usleep(100000);
        if (g_busy && g_child_pid > 0)
            kill(g_child_pid, SIGKILL);
    }
    if (g_register)
        rnbo_listener_update(server, 0);
    lo_server_free(server);
    return 0;
}
