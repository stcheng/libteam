/*
 * teamd.c - Network team device daemon
 * Copyright (c) 2011 Jiri Pirko <jpirko@redhat.com>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 of the License.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <linux/netdevice.h>
#include <sys/syslog.h>
#include <libdaemon/dfork.h>
#include <libdaemon/dsignal.h>
#include <libdaemon/dlog.h>
#include <libdaemon/dpid.h>
#include <json/json.h>
#include <team.h>

#include "teamd.h"

/* For purpose of immediate use, e.g. print */
char *dev_name(const struct teamd_context *ctx, uint32_t ifindex)
{
	static char ifname[IFNAMSIZ];

	return team_ifindex2ifname(ctx->th, ifindex, ifname, sizeof(ifname));
}

char *dev_name_dup(const struct teamd_context *ctx, uint32_t ifindex)
{
	char *ifname = dev_name(ctx, ifindex);

	if (!ifname)
		return NULL;
	return strdup(ifname);
}

static const struct teamd_runner *teamd_runner_list[] = {
	&teamd_runner_dummy,
	&teamd_runner_roundrobin,
	&teamd_runner_activebackup,
};

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define TEAMD_RUNNER_LIST_SIZE ARRAY_SIZE(teamd_runner_list)

static const struct teamd_runner *teamd_find_runner(const char *runner_name)
{
	int i;

	for (i = 0; i < TEAMD_RUNNER_LIST_SIZE; i++) {
		if (strcmp(teamd_runner_list[i]->name, runner_name) == 0)
			return teamd_runner_list[i];
	}
	return NULL;
}


static void libteam_log_daemon(struct team_handle *th, int priority,
			       const char *file, int line, const char *fn,
			       const char *format, va_list args)
{
	daemon_logv(priority, format, args);
}

static char **__g_pid_file;

static int teamd_cfg_get_str(const struct teamd_context *ctx, const char **dst,
			     const char *query, ...)
{
	json_object *jso;
	va_list arglist;
	char *qbuffer;
	int err;

	va_start(arglist, query);
	err = vasprintf(&qbuffer, query, arglist);
	va_end(arglist);
	if (err == -1) {
		free(qbuffer);
		return -errno;
	}
	teamd_log_dbg("Query: \"%s\".", qbuffer);
	jso = json_object_simple_query(ctx->config_jso, qbuffer);
	free(qbuffer);
	if (!jso) {
		teamd_log_dbg("Config string get failed. No such object.");
		return -ENOENT;
	}
	if (json_object_get_type(jso) != json_type_string) {
		teamd_log_dbg("Config string get failed. Object has different type.");
		return -EINVAL;
	}

	*dst = json_object_get_string(jso);
	return 0;
}

static void print_help(const struct teamd_context *ctx) {
	int i;

	printf(
            "%s [options]\n"
            "    -h --help                Show this help\n"
            "    -d --daemonize           Daemonize after startup (implies -s)\n"
            "    -k --kill                Kill a running daemon\n"
            "    -e --check               Return 0 if a daemon is already running\n"
            "    -V --version             Show version\n"
            "    -f --config-file=FILE    Load the specified configuration file\n"
            "    -c --config=TEXT         Use given config string (This causes configuration\n"
	    "                             file will be ignored)\n"
            "    -p --pid-file=FILE       Use the specified PID file\n"
            "    -g --debug               Increase verbosity\n"
            "    -r --force-recreate      Force team device recreation in case it\n"
            "                             already exists\n",
            ctx->argv0);
	printf("Available runners: ");
	for (i = 0; i < TEAMD_RUNNER_LIST_SIZE; i++) {
		if (i != 0)
			printf(", ");
		printf("%s", teamd_runner_list[i]->name);
	}
	printf("\n");
}

static int parse_command_line(struct teamd_context *ctx,
			      int argc, char *argv[]) {
	int opt;
	static const struct option long_options[] = {
		{ "help",		no_argument,		NULL, 'h' },
		{ "daemonize",		no_argument,		NULL, 'd' },
		{ "kill",		no_argument,		NULL, 'k' },
		{ "check",		no_argument,		NULL, 'e' },
		{ "version",		no_argument,		NULL, 'v' },
		{ "config-file",	required_argument,	NULL, 'f' },
		{ "config",		required_argument,	NULL, 'c' },
		{ "pid-file",		required_argument,	NULL, 'p' },
		{ "debug",		no_argument,		NULL, 'g' },
		{ "force-recreate",	no_argument,		NULL, 'r' },
		{ NULL, 0, NULL, 0 }
	};

	while ((opt = getopt_long(argc, argv, "hdkevf:c:p:gr",
				  long_options, NULL)) >= 0) {

		switch(opt) {
		case 'h':
			ctx->cmd = DAEMON_CMD_HELP;
			break;
		case 'd':
			ctx->daemonize = true;
			break;
		case 'k':
			ctx->cmd = DAEMON_CMD_KILL;
			break;
		case 'e':
			ctx->cmd = DAEMON_CMD_CHECK;
			break;
		case 'v':
			ctx->cmd = DAEMON_CMD_VERSION;
			break;
		case 'f':
			free(ctx->config_file);
			ctx->config_file = realpath(optarg, NULL);
			if (!ctx->config_file)
				fprintf(stderr, "Failed to get absolute path of \"%s\": %s\n",
					optarg, strerror(errno));
			break;
		case 'c':
			free(ctx->config_text);
			ctx->config_text = strdup(optarg);
			break;
		case 'p':
			free(ctx->pid_file);
			ctx->pid_file = strdup(optarg);
			break;
		case 'g':
			ctx->debug = true;
			break;
		case 'r':
			ctx->force_recreate = true;
			break;
		default:
			return -1;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Too many arguments\n");
		return -1;
	}

	return 0;
}

static const char *pid_file_proc(void) {
	return *__g_pid_file;
}

static int teamd_run(struct teamd_context *ctx)
{
	bool quit = false;
	int sig_fd;
	int team_event_fd;
	fd_set fds;
	int fdmax;

	FD_ZERO(&fds);
	sig_fd = daemon_signal_fd();
	FD_SET(sig_fd, &fds);
	team_event_fd = team_get_event_fd(ctx->th);
	FD_SET(team_event_fd, &fds);
	fdmax = (sig_fd > team_event_fd ? sig_fd : team_event_fd) + 1;

	while (!quit) {
		fd_set fds_tmp = fds;

		if (select(fdmax, &fds_tmp, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;

			teamd_log_err("select() failed.");
			return -errno;
		}

		if (FD_ISSET(sig_fd, &fds_tmp)) {
			int sig;

			/* Get signal */
			if ((sig = daemon_signal_next()) <= 0) {
				teamd_log_err("daemon_signal_next() failed.");
				return -errno;
			}

			/* Dispatch signal */
			switch (sig) {
			case SIGINT:
			case SIGQUIT:
			case SIGTERM:
				teamd_log_warn("Got SIGINT, SIGQUIT or SIGTERM.");
				quit = true;
				break;

			}
		}
		if (FD_ISSET(team_event_fd, &fds_tmp))
			team_process_event(ctx->th);
	}
	return 0;
}

static int load_file(char *filename, char **pstr)
{
	int err;
	FILE *f;
	char *str;
	long size;

	f = fopen(filename, "r");
	if (!f)
		return -errno;
	err = fseek(f, 0, SEEK_END);
	if (err) {
		err = -errno;
		goto fclose;
	}
	size = ftell(f);
	if (errno) {
		err = -errno;
		goto fclose;
	}
	rewind(f);
	str = malloc(sizeof(char) * (size + 1));
	if (!str) {
		err = -ENOMEM;
		goto fclose;
	}
	if (size != fread(str, sizeof(char), size, f)) {
		err = -errno;
		goto free_str;
	}
	fclose(f);
	*pstr = str;
	return 0;
free_str:
	free(str);
fclose:
	fclose(f);
	return err;
}

static int load_config(struct teamd_context *ctx)
{
	int err;

	if (ctx->config_file) {
		if (ctx->config_text) {
			teamd_log_warn("Command line configuration is present, ignoring given config file.");
		} else {
			err = load_file(ctx->config_file, &ctx->config_text);
			if (err) {
				teamd_log_err("Failed to read file \"%s\".", ctx->config_file);
				return err;
			}
		}
	}
	if (ctx->config_text) {
		ctx->config_jso = json_tokener_parse(ctx->config_text);
		if (!ctx->config_jso) {
			teamd_log_err("Failed to parse configuration.");
			return -EIO;
		}
	} else {
		teamd_log_err("Either configuration file or command line configuration string must be present.");
		return -ENOENT;
	}
	return 0;
}

static int parse_hwaddr(const char *hwaddr_str, char **phwaddr,
			unsigned int *plen)
{
	const char *pos = hwaddr_str;
	unsigned int byte_count = 0;
	unsigned int tmp;
	int err;
	char *hwaddr = NULL;
	char *new_hwaddr;
	char *endptr;

	while (true) {
		errno = 0;
		tmp = strtoul(pos, &endptr, 16);
		if (errno != 0 || tmp > 0xFF) {
			err = -EINVAL;
			goto err_out;
		}
		byte_count++;
		new_hwaddr = realloc(hwaddr, sizeof(char) * byte_count);
		if (!new_hwaddr) {
			err = -ENOMEM;
			goto err_out;
		}
		hwaddr = new_hwaddr;
		hwaddr[byte_count - 1] = (char) tmp;
		while (isspace(endptr[0]) && endptr[0] != '\0')
			endptr++;
		if (endptr[0] == ':') {
			pos = endptr + 1;
		} else if (endptr[0] == '\0') {
			break;
		} else {
			err = -EINVAL;
			goto err_out;
		}
	}
	*phwaddr = hwaddr;
	*plen = byte_count;
	return 0;
err_out:
	free(hwaddr);
	return err;
}

static int teamd_check_change_hwaddr(struct teamd_context *ctx)
{
	int err;
	const char *hwaddr_str;
	char *hwaddr;
	unsigned int hwaddr_len;

	err = teamd_cfg_get_str(ctx, &hwaddr_str, "['hwaddr']");
	if (err)
		return 0; /* addr is not defined in config, no change needed */

	teamd_log_dbg("Hwaddr string: \"%s\".", hwaddr_str);
	err = parse_hwaddr(hwaddr_str, &hwaddr, &hwaddr_len);
	if (err) {
		teamd_log_err("Failed to parse hardware address.");
		return err;
	}

	if (hwaddr_len != ctx->hwaddr_len) {
		teamd_log_err("Passed hardware address has different length (%d) than team device has (%d).",
			      hwaddr_len, ctx->hwaddr_len);
		return -EINVAL;
	}
	err = team_hwaddr_set(ctx->th, ctx->ifindex, hwaddr, hwaddr_len);
	free(hwaddr);
	return err;
}

static int teamd_add_ports(struct teamd_context *ctx)
{
	int i;
	const char *port_name;
	int err;
	uint32_t ifindex;

	teamd_for_each_port(i, port_name, ctx) {
		ifindex = team_ifname2ifindex(ctx->th, port_name);
		teamd_log_dbg("Adding port \"%s\" (found ifindex \"%d\").",
			      port_name, ifindex);
		err = team_port_add(ctx->th, ifindex);
		if (err) {
			teamd_log_err("Failed to add port \"%s\".", port_name);
			return err;
		}
	}

	return 0;
}

static int teamd_runner_init(struct teamd_context *ctx)
{
	int err;
	const char *runner_name;

	err = teamd_cfg_get_str(ctx, &runner_name, "['runner']");
	if (err) {
		teamd_log_err("Failed to get team runner name from config.");
		return err;
	}
	teamd_log_dbg("Using team runner \"%s\".", runner_name);
	ctx->runner = teamd_find_runner(runner_name);
	if (!ctx->runner) {
		teamd_log_err("No runner named \"%s\" available.", runner_name);
		return -ENOENT;
	}

	if (ctx->runner->team_mode_name) {
		err = team_set_mode_name(ctx->th, ctx->runner->team_mode_name);
		if (err) {
			teamd_log_err("Failed to set team mode \"%s\".",
				      ctx->runner->team_mode_name);
			return err;
		}
	} else {
		teamd_log_warn("Note \"%s\" runner does not select team mode resulting in no functionality!",
			       runner_name);
	}

	if (ctx->runner->priv_size) {
		ctx->runner_priv = malloc(ctx->runner->priv_size);
		if (!ctx->runner_priv)
			return -ENOMEM;
		memset(ctx->runner_priv, 0, ctx->runner->priv_size);
	}

	if (ctx->runner->init) {
		err = ctx->runner->init(ctx);
		if (err) {
			free(ctx->runner_priv);
			return err;
		}
	}
	return 0;
}

static void teamd_runner_fini(struct teamd_context *ctx)
{
	if (ctx->runner->fini)
		ctx->runner->fini(ctx);
	free(ctx->runner_priv);
}

static int teamd_init(struct teamd_context *ctx)
{
	int err;
	const char *team_name;

	err = load_config(ctx);
	if (err) {
		teamd_log_err("Failed to load config.");
		return err;
	}
	err = teamd_cfg_get_str(ctx, &team_name, "['device']");
	if (err) {
		teamd_log_err("Failed to get team device name.");
		return err;
	}
	teamd_log_dbg("Using team device \"%s\".", team_name);

	ctx->th = team_alloc();
	if (!ctx->th) {
		teamd_log_err("Team alloc failed.");
		return -ENOMEM;
	}
	if (ctx->debug)
		team_set_log_priority(ctx->th, LOG_DEBUG);

	team_set_log_fn(ctx->th, libteam_log_daemon);

	if (ctx->force_recreate)
		err = team_recreate(ctx->th, team_name);
	else
		err = team_create(ctx->th, team_name);
	if (err) {
		teamd_log_err("Failed to create team device.");
		goto team_free;
	}

	ctx->ifindex = team_ifname2ifindex(ctx->th, team_name);
	if (!ctx->ifindex) {
		teamd_log_err("Netdevice \"%s\" not found.", team_name);
		err = -ENOENT;
		goto team_destroy;
	}

	err = team_init(ctx->th, ctx->ifindex);
	if (err) {
		teamd_log_err("Team init failed.");
		goto team_destroy;
	}

	ctx->hwaddr_len = team_hwaddr_len_get(ctx->th, ctx->ifindex);
	if (ctx->hwaddr_len < 0) {
		teamd_log_err("Failed to get hardware address length.");
		err = ctx->hwaddr_len;
		goto team_destroy;
	}

	err = teamd_check_change_hwaddr(ctx);
	if (err) {
		teamd_log_err("Hardware address change failed.");
		goto team_destroy;
	}

	err = teamd_runner_init(ctx);
	if (err) {
		teamd_log_err("Failed to init runner.");
		goto team_destroy;
	}

	err = teamd_add_ports(ctx);
	if (err) {
		teamd_log_err("Failed to add ports.");
		goto runner_fini;
	}

	return 0;

runner_fini:
	teamd_runner_fini(ctx);
team_destroy:
	team_destroy(ctx->th);
team_free:
	team_free(ctx->th);
	return err;
}

static void teamd_fini(struct teamd_context *ctx)
{
	teamd_runner_fini(ctx);
	team_destroy(ctx->th);
	team_free(ctx->th);
}

static int teamd_start(struct teamd_context *ctx)
{
	pid_t pid;
	int err = 0;

	if (getuid() != 0) {
		teamd_log_err("This program is intended to be run as root.");
		return -EPERM;
	}

	if (daemon_reset_sigs(-1) < 0) {
		teamd_log_err("Failed to reset all signal handlers.");
		return -errno;
	}

	if (daemon_unblock_sigs(-1) < 0) {
		teamd_log_err("Failed to unblock all signals.");
		return -errno;
	}

	pid = daemon_pid_file_is_running();
	if (pid >= 0) {
		teamd_log_err("Daemon already running on PID %u.", pid);
		return -EEXIST;
	}

	if (ctx->daemonize) {
		daemon_retval_init();

		pid = daemon_fork();
		if (pid < 0) {
			teamd_log_err("Daemon fork failed.");
			daemon_retval_done();
			return -errno;
		}
		else if (pid != 0) {
			int ret;

			/* Parent */
			ret = daemon_retval_wait(20);
			if (ret < 0) {
				teamd_log_err("Could not receive return value from daemon process.");
				return -errno;
			}
			if (ret > 0)
				teamd_log_err("Daemon process failed.");
			return -ret;
		}

	/* Child */
	}

	if (daemon_close_all(-1) < 0) {
		teamd_log_err("Failed to close all file descriptors.");
		daemon_retval_send(errno);
		return -errno;
	}

	if (daemon_pid_file_create() < 0) {
		teamd_log_err("Could not create PID file.");
		daemon_retval_send(errno);
		return -errno;
	}

	if (daemon_signal_init(SIGINT, SIGTERM, SIGQUIT, SIGHUP, 0) < 0) {
		teamd_log_err("Could not register signal handlers.");
		daemon_retval_send(errno);
		err = -errno;
		goto pid_file_remove;
	}

	err = teamd_init(ctx);
	if (err) {
		teamd_log_err("teamd_init() failed.");
		daemon_retval_send(-err);
		goto signal_done;
	}

	daemon_retval_send(0);

	teamd_log_info(PACKAGE_VERSION" sucessfully started.");

	err = teamd_run(ctx);

	teamd_log_info("Exiting...");

	teamd_fini(ctx);

signal_done:
	daemon_signal_done();

pid_file_remove:
	daemon_pid_file_remove();

	return err;
}

static int teamd_context_init(struct teamd_context **pctx)
{
	struct teamd_context *ctx;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	memset(ctx, 0, sizeof(*ctx));
	*pctx = ctx;

	__g_pid_file = &ctx->pid_file;

	return 0;
}

static void teamd_context_fini(struct teamd_context *ctx)
{
	free(ctx->config_text);
	free(ctx->config_file);
	free(ctx->pid_file);
	free(ctx);
}

int main(int argc, char **argv)
{
	int ret = EXIT_FAILURE;
	int err;
	struct teamd_context *ctx;

	err = teamd_context_init(&ctx);
	if (err) {
		fprintf(stderr, "Failed to init daemon context\n");
		return ret;
	}

	err = parse_command_line(ctx, argc, argv);
	if (err)
		goto finish;

	if (ctx->debug)
		daemon_set_verbosity(LOG_DEBUG);

	ctx->argv0 = daemon_ident_from_argv0(argv[0]);
	daemon_log_ident = ctx->argv0;
	daemon_pid_file_ident = ctx->argv0;

	if (ctx->pid_file)
		daemon_pid_file_proc = pid_file_proc;

	teamd_log_dbg("Using PID file \"%s\"", daemon_pid_file_proc());
	if (ctx->config_file)
		teamd_log_dbg("Using config file \"%s\"", ctx->config_file);

	switch (ctx->cmd) {
	case DAEMON_CMD_HELP:
		print_help(ctx);
		ret = EXIT_SUCCESS;
		break;
	case DAEMON_CMD_VERSION:
		printf("%s "PACKAGE_VERSION"\n", ctx->argv0);
		ret = 0;
		break;
	case DAEMON_CMD_KILL:
		err = daemon_pid_file_kill_wait(SIGTERM, 5);
		if (err)
			teamd_log_warn("Failed to kill daemon: %s", strerror(errno));
		else
			ret = EXIT_SUCCESS;
		break;
	case DAEMON_CMD_CHECK:
		ret = (daemon_pid_file_is_running() >= 0) ? 0 : 1;
		break;
	case DAEMON_CMD_RUN:
		err = teamd_start(ctx);
		if (err)
			teamd_log_err("Failed to start daemon: %s", strerror(-err));
		else
			ret = EXIT_SUCCESS;
		break;
	}

finish:

	teamd_context_fini(ctx);
	return ret;
}
