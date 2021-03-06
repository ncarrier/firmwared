/**
 * @file instances.c
 * @brief
 *
 * @date Apr 21, 2015
 * @author nicolas.carrier@parrot.com
 * @copyright Copyright (C) 2015 Parrot S.A.
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif /* _XOPEN_SOURCE */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif /* _GNU_SOURCE */
#include <linux/limits.h>
#include <sched.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>

#include <grp.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <signal.h>

#include <argz.h>
#include <errno.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dlfcn.h>
#include <libgen.h>

#include <openssl/sha.h>

#define ULOG_TAG firmwared_instances
#include <ulog.h>
ULOG_DECLARE_TAG(firmwared_instances);

#include <io_mon.h>
#include <io_process.h>
#include <io_utils.h>

#include <ut_utils.h>
#include <ut_string.h>
#include <ut_file.h>
#include <ut_process.h>
#include <ut_bits.h>

#include <ptspair.h>

#include <fwd.h>

#include "log.h"
#include "process.h"
#include "folders.h"
#include "instances.h"
#include "preparation.h"
#include "utils.h"
#include "config.h"
#include "firmwared.h"
#include "apparmor.h"
#include "instances-private.h"
#include "properties/instance_properties.h"

#ifndef POST_PREPARATION_TIMEOUT
#define POST_PREPARATION_TIMEOUT 10000
#endif /* POST_PREPARATION_TIMEOUT */

/*
 * this hardcoded value could be a modifiable parameter, but it really adds to*
 * much complexity to the code and so isn't worth the effort
 */
#define NET_BITS "24"

static ut_bit_field indices;

static struct folder instances_folder;

struct instance_preparation {
	struct preparation preparation;
};

static int sha1(struct instance *instance,
		unsigned char hash[SHA_DIGEST_LENGTH])
{
	SHA_CTX ctx;

	if (ut_string_is_invalid(instance->firmware_path))
		return -EINVAL;

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, instance->firmware_path,
			strlen(instance->firmware_path));
	SHA1_Update(&ctx, &instance->time, sizeof(instance->time));
	SHA1_Update(&ctx, &instance->id, sizeof(instance->id));
	SHA1_Final(hash, &ctx);

	return 0;
}

static const char *compute_sha1(struct instance *instance)
{
	int ret;
	unsigned char hash[SHA_DIGEST_LENGTH];

	if (instance->sha1[0] == '\0') {
		ret = sha1(instance, hash);
		if (ret < 0) {
			errno = -ret;
			return NULL;
		}

		buffer_to_string(hash, SHA_DIGEST_LENGTH, instance->sha1);
	}

	return instance->sha1;
}

static const char *instance_sha1(struct folder_entity *entity)
{
	return instance_get_sha1(to_instance(entity));
}

static void clean_paths(struct instance *instance)
{
	ut_string_free(&instance->ro_mount_point);
	ut_string_free(&instance->rw_dir);
	ut_string_free(&instance->union_mount_point);
	ut_string_free(&instance->x11_mount_point);
	ut_string_free(&instance->nvidia_mount_point);
	ut_string_free(&instance->nvidia_path);
}

static int invoke_mount_helper(struct instance *instance, const char *action,
		bool only_unregister)
{
	int ret;
	struct io_process process;

	ret = io_process_init_prepare_launch_and_wait(&process,
			&process_default_parameters,
			NULL,
			config_get(CONFIG_MOUNT_HOOK),
			action,
			folder_entity_get_base_workspace(&instance->entity),
			instance->ro_mount_point,
			instance->rw_dir,
			instance->union_mount_point,
			config_get(CONFIG_X11_PATH),
			instance->x11_mount_point,
			instance->nvidia_path ? instance->nvidia_path : "",
			instance->nvidia_mount_point,
			instance->firmware_path,
			instance_get_sha1(instance),
			only_unregister ? "true" : "false",
			config_get(CONFIG_PREVENT_REMOVAL),
			config_get(CONFIG_VERBOSE_HOOK_SCRIPTS),
			NULL /* NULL guard */);
	if (ret < 0)
		return ret;

	return process.status == 0 ? 0 : -ECANCELED;
}

static int invoke_net_helper(struct instance *i, const char *action)
{
	char pid[10]; /* max is 1 << 22 -> 7 digits in base 10 */
	char id[10]; /* max is 255 */
	int ret;
	struct io_process process;

	snprintf(pid, 10, "%jd", (intmax_t)i->pid);
	snprintf(id, 10, "%"PRIu8, i->id);
	ret = io_process_init_prepare_launch_and_wait(&process,
			&process_default_parameters,
			NULL,
			config_get(CONFIG_NET_HOOK),
			action,
			i->interface,
			i->stolen_interface == NULL ? "" : i->stolen_interface,
			i->stolen_btusb_id,
			config_get(CONFIG_HOST_INTERFACE_PREFIX),
			id,
			config_get(CONFIG_NET_FIRST_TWO_BYTES),
			NET_BITS,
			pid,
			config_get(CONFIG_VERBOSE_HOOK_SCRIPTS),
			NULL /* NULL guard */);
	if (ret < 0)
		return ret;

	return process.status == 0 ? 0 : -ECANCELED;
}

static int invoke_post_prepare_instance_helper(struct instance *instance)
{
	int ret;
	struct io_process process;
	struct io_process_parameters parameters = process_default_parameters;

	parameters.timeout = POST_PREPARATION_TIMEOUT;

	/* TODO this blocks too much time */
	ret = io_process_init_prepare_launch_and_wait(&process,
			&parameters,
			NULL,
			config_get(CONFIG_POST_PREPARE_INSTANCE_HOOK),
			instance->union_mount_point,
			instance->firmware_path,
			config_get(CONFIG_VERBOSE_HOOK_SCRIPTS),
			NULL /* NULL guard */);
	if (ret < 0)
		return ret;

	return process.status == 0 ? 0 : -ECANCELED;
}

static void clean_mount_points(struct instance *instance, bool only_unregister)
{
	int ret;

	ret = invoke_mount_helper(instance, "clean", only_unregister);
	if (ret != 0)
		ULOGE("invoke_mount_helper clean returned %d", ret);
	clean_paths(instance);
}

static void clean_command_line(struct instance *instance)
{
	if (instance->command_line == NULL)
		return;

	memset(instance->command_line, 0, instance->command_line_len);
	free(instance->command_line);
	instance->command_line = NULL;
	instance->command_line_len = 0;
}

static void clean_instance(struct instance *i, bool only_unregister)
{
	clean_command_line(i);
	if (!config_get_bool(CONFIG_DISABLE_APPARMOR))
		apparmor_remove_profile(instance_get_sha1(i));
	ut_process_sync_clean(&i->sync);
	io_mon_remove_sources(firmwared_get_mon(),
			io_src_evt_get_source(&i->monitor_evt),
			&i->ptspair_src,
			NULL /* guard */);
	io_src_evt_clean(&i->monitor_evt);
	io_src_clean(&i->ptspair_src);

	ptspair_clean(&i->ptspair);
	clean_mount_points(i, only_unregister);

	ut_string_free(&i->stolen_interface);
	ut_string_free(&i->interface);
	ut_string_free(&i->firmware_path);
	ut_bit_field_release_index(&indices, i->id);
	memset(i, 0, sizeof(*i));
}

static bool instance_is_running(struct instance *instance)
{
	return instance == NULL ? false : instance->state != INSTANCE_READY;
}

static bool instance_can_drop(struct folder_entity *entity)
{
	struct instance *instance = to_instance(entity);

	return !instance_is_running(instance);
}

static int instance_drop(struct folder_entity *entity, bool only_unregister)
{
	struct instance *instance = to_instance(entity);

	ULOGD("%s", __func__);

	if (instance_is_running(instance)) {
		ULOGW("instance %s still running, try to kill and wait for it",
				instance_get_name(instance));
		instance_kill(instance, (uint32_t)-1);
		sleep(1);
	}
	instance_delete(&instance, only_unregister);

	return 0;
}

static int get_nvidia_path(char **nvidia_path)
{
	int ret = 0;
	const char *filename = "libGL.so.1";
	const char *fnname = "glHint";
	void *dl_handle;
	void *sym;
	Dl_info dl_info;
	const char *config_nvidia_path;
	char *tmp;
	char *dir;

	*nvidia_path = NULL;

	/* give priority to user-specified path */
	config_nvidia_path = config_get(CONFIG_NVIDIA_PATH);
	if (!ut_string_is_invalid(config_nvidia_path)) {
		*nvidia_path = strdup(config_nvidia_path);
		if (*nvidia_path == NULL) {
			ret = -errno;
			ULOGE("calloc: %s", strerror(-ret));
		}
		return ret;
	}

	/* load the dynamic library file */
	dl_handle = dlopen(filename, RTLD_LAZY|RTLD_LOCAL);
	if (dl_handle == NULL) {
		ULOGW("Failed to load library %s: %s", filename, dlerror());
		return ret;
	}

	/* find the address where the symbol is loaded into memory */
	sym = dlsym(dl_handle, fnname);
	if (sym == NULL) {
		ULOGW("Failed to resolve %s: %s", fnname, dlerror());
		goto out;
	}

	/*
	 * resolve file where the function pointer is located. If the substring
	 * "nvidia" is located in the pathname of the shared object that contains
	 * the address, return the duplicate of the directory.
	 */
	if (dladdr(sym, &dl_info) == 0) {
		ULOGW("Failed to resolve %p: %s", sym, dlerror());
		goto out;
	}
	if (strstr(dl_info.dli_fname, "nvidia") != NULL) {
		tmp = strdup(dl_info.dli_fname);
		if (tmp == NULL) {
			ret = -errno;
			ULOGE("calloc: %s", strerror(-ret));
			goto out;
		}
		dir = dirname(tmp);
		*nvidia_path = memmove(tmp, dir, strlen(dir) + 1);
	}

out:
	dlclose(dl_handle);
	return ret;
}

static int init_paths(struct instance *instance)
{
	int ret;
	const char *base_workspace = folder_entity_get_base_workspace(
			&instance->entity);

	ret = asprintf(&instance->ro_mount_point, "%s/ro", base_workspace);
	if (ret < 0) {
		instance->ro_mount_point = NULL;
		ULOGE("asprintf ro_mount_point error");
		ret = -ENOMEM;
		goto err;
	}
	ret = asprintf(&instance->rw_dir, "%s/rw", base_workspace);
	if (ret < 0) {
		instance->rw_dir = NULL;
		ULOGE("asprintf rw_dir error");
		ret = -ENOMEM;
		goto err;
	}
	ret = asprintf(&instance->union_mount_point, "%s/union",
			base_workspace);
	if (ret < 0) {
		instance->union_mount_point = NULL;
		ULOGE("asprintf union_mount_point error");
		ret = -ENOMEM;
		goto err;
	}
	ret = asprintf(&instance->x11_mount_point, "%s/simulator/x11",
			instance->union_mount_point);
	if (ret < 0) {
		instance->x11_mount_point = NULL;
		ULOGE("asprintf x11_mount_point error");
		ret = -ENOMEM;
		goto err;
	}
	ret = asprintf(&instance->nvidia_mount_point, "%s/simulator/nvidia",
			instance->union_mount_point);
	if (ret < 0) {
		instance->nvidia_mount_point = NULL;
		ULOGE("asprintf nvidia_mount_point error");
		ret = -ENOMEM;
		goto err;
	}
	ret = get_nvidia_path(&instance->nvidia_path);
	if (ret < 0) {
		ULOGE("nvidia_get_path nvidia_path error");
		goto err;
	}

	return 0;
err:
	clean_paths(instance);

	return ret;
}

static int init_mount_points(struct instance *instance)
{
	int ret;

	ret = init_paths(instance);
	if (ret < 0) {
		ULOGE("init_paths: %s", strerror(-ret));
		goto err;
	}
	ret = invoke_mount_helper(instance, "init", false);
	if (ret != 0) {
		ULOGE("invoke_mount_helper init returned %d", ret);
		ret = -ENOTRECOVERABLE;
		goto err;
	}

	return 0;
err:
	clean_mount_points(instance, false);

	return ret;
}

static void monitor_evt_cb(struct io_src_evt *evt, uint64_t ignored)
{
	int ret;
	int program_status;
	struct instance *i = ut_container_of(evt, typeof(*i), monitor_evt);

	ret = waitpid(i->pid, &program_status, 0);
	if (ret < 0) {
		ULOGE("waitpid : err=%d(%s)", ret, strerror(-ret));
		return;
	}
	ULOGD("waitpid said %d", program_status);

	ret = invoke_net_helper(i, "clean");
	if (ret != 0)
		ULOGE("invoke_net_helper assign returned %d", ret);

	i->state = INSTANCE_READY;

	ret = firmwared_notify(FWD_ANSWER_DEAD, FWD_FORMAT_ANSWER_DEAD,
			i->killer_seqnum, instance_get_sha1(i),
			instance_get_name(i));
	i->killer_seqnum = (uint32_t)-1;
	if (ret < 0)
		ULOGE("firmwared_notify : err=%d(%s)", ret, strerror(-ret));
}

static void ptspair_src_cb(struct io_src *src)
{
	int ret;
	struct instance *i = ut_container_of(src, typeof(*i), ptspair_src);
	struct ptspair *ptspair = &i->ptspair;

	ret = ptspair_process_events(ptspair);
	if (ret < 0) {
		ULOGE("ptspair_process_events : err=%d(%s)", ret,
				strerror(-ret));
		return;
	}
}

static int setup_container(struct instance *instance)
{
	int ret;
	int flags;

	/*
	 * use our own namespace for IPC, networking, mount points and uts
	 * (hostname and domain name), the pid namespace will be set up only
	 * after we have no more fork() (read: system()) calls to do for the
	 * setup
	 */
	flags = CLONE_FILES | CLONE_NEWIPC | CLONE_NEWNET |
			CLONE_NEWNS | CLONE_NEWUTS | CLONE_SYSVSEM;
	ret = unshare(flags);
	if (ret < 0) {
		ret = -errno;
		ULOGE("unshare: %m");
		return ret;
	}

	/*
	 * although we have a new mount points namespace, it is still necessary
	 * to make them private, recursively, so that changes aren't propagated
	 * to the parent namespace
	 */
	ret = mount(NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL);
	if (ret < 0) {
		ret = -errno;
		ULOGE("cannot make \"/\" private: %m");
		return ret;
	}

	return 0;
}

static int setup_chroot(struct instance *instance)
{
	int chroot_ret;
	int chdir_ret;

	/*
	 * coverity groans about chdir not being called before we read the errno
	 * value, which triggers implicitly a call to __errno_location()
	 * hence, we don't retrieve this value before calling chdir, but because
	 * of this, the corresponding errno is lost
	 */
	chroot_ret = chroot(instance->union_mount_point);
	chdir_ret = chdir("/");
	if (chroot_ret == -1) {
		/* dummy value, since errno is overwritten by chdir_ret */
		chroot_ret = -ECANCELED;
		ULOGE("chroot in %s failed, unknown error",
				instance->union_mount_point);
		return chroot_ret;
	}
	if (chdir_ret < 0) {
		chdir_ret = -errno;
		ULOGE("chdir(/): %m");
		return chdir_ret;
	}

	return 0;
}

static void launch_pid_1(struct instance *instance, int fd)
{
	int ret;
	int i;
	const char *hostname;
	const char *sha1;
	char **argv = NULL;
	size_t argc;
	sigset_t mask;

	sha1 = instance_get_sha1(instance);
	ret = ut_process_change_name("pid-1-%s", sha1);
	if (ret < 0)
		ULOGE("ut_process_change_name(pid-1-%s): %s", sha1,
				strerror(-ret));

	ULOGI("%s", __func__);

	/* prepare the command-line */
	argc = argz_count(instance->command_line, instance->command_line_len);
	argv = calloc(argc + 1,  sizeof(*argv));
	if (argv == NULL) {
		ULOGE("calloc: %m");
		_exit(EXIT_FAILURE);
	}

	argz_extract(instance->command_line, instance->command_line_len, argv);

	/* we need to be able to differentiate instances by hostnames */
	hostname = instance_get_name(instance);
	ret = sethostname(hostname, strlen(hostname));
	if (ret < 0)
		ULOGE("sethostname(%s): %m", hostname);

	/* be sure we die if our parent does */
	ret = prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (ret < 0)
		ULOGE("prctl(PR_SET_PDEATHSIG, SIGKILL): %m");

	if (fd >= 0) {
		ret = dup2(fd, STDIN_FILENO);
		if (ret < 0)
			ULOGE("dup2(fd, STDIN_FILENO): %m");
		ret = dup2(fd, STDOUT_FILENO);
		if (ret < 0)
			ULOGE("dup2(fd, STDOUT_FILENO): %m");
		ret = dup2(fd, STDERR_FILENO);
		if (ret < 0)
			ULOGE("dup2(fd, STDERR_FILENO): %m");
	}
	/* re-enable the signals previously blocked */
	sigemptyset(&mask);
	ret = sigprocmask(SIG_SETMASK, &mask, NULL);
	if (ret == -1) {
		ULOGE("sigprocmask: %m");
		_exit(EXIT_FAILURE);
	}
	for (i = sysconf(_SC_OPEN_MAX) - 1; i > 2; i--)
		close(i);
	/* from here, ulog doesn't work anymore */
	ret = execv(argv[0], argv);
	if (ret < 0)
		/*
		 * if one want to search for potential failure of the execve
		 * call, he has to read in the instances pts
		 */
		fprintf(stderr, "execv: %m");

	_exit(EXIT_FAILURE);
}

__attribute__((noreturn))
static void launch_instance(struct instance *instance)
{
	int ret;
	pid_t pid;
	int fd;
	const char *sha1;
	char buf[0x200];
	const char *instance_name;
	int sfd;
	sigset_t mask;
	struct signalfd_siginfo si;
	int status;

	ret = prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (ret < 0)
		ULOGE("prctl(PR_SET_PDEATHSIG, SIGKILL): %m");

	instance_name = instance_get_name(instance);
	sha1 = instance_get_sha1(instance);
	ret = ut_process_change_name("monitor-%s", sha1);
	if (ret < 0)
		ULOGE("ut_process_change__name(monitor-%s): %s", sha1,
				strerror(-ret));

	ULOGI("%s \"%s\"", __func__, instance->firmware_path);

	fd = open(ptspair_get_path(&instance->ptspair, PTSPAIR_BAR), O_RDWR);
	if (fd < 0) {
		ULOGE("open: %m");
		_exit(EXIT_FAILURE);
	}
	ret = setup_container(instance);
	if (ret < 0) {
		ULOGE("setup_container: %m");
		_exit(EXIT_FAILURE);
	}
	/*
	 * notify firmwared that we have just created our net namespace in which
	 * it can migrate our network interfaces.
	 */
	ret = ut_process_sync_child_unlock(&instance->sync);
	if (ret < 0)
		ULOGE("ut_process_sync_child_unlock: parent/child "
				"synchronisation failed: %s", strerror(-ret));
	/*
	 * wait until firmwared has assigned our network interfaces to our
	 * namespace, so that we can configure them
	 */
	ret = ut_process_sync_child_lock(&instance->sync);
	if (ret < 0)
		ULOGE("ut_process_sync_child_lock: parent/child "
				"synchronisation failed: %s", strerror(-ret));
	ret = invoke_net_helper(instance, "config");
	if (ret != 0) {
		ULOGE("invoke_net_helper config returned %d", ret);
		_exit(EXIT_FAILURE);
	}
	if (!config_get_bool(CONFIG_DISABLE_APPARMOR)) {
		ret = apparmor_change_profile(sha1);
		if (ret < 0) {
			ULOGE("apparmor_change_profile: %s", strerror(-ret));
			_exit(EXIT_FAILURE);
		}
	}
	ret = setup_chroot(instance);
	if (ret < 0) {
		ULOGE("setup_chroot: %m");
		_exit(EXIT_FAILURE);
	}

	/*
	 * at last, setup the pid namespace, no more fork allowed apart from
	 * pid 1
	 */
	ret = unshare(CLONE_NEWPID);
	if (ret < 0) {
		ULOGE("unshare: %m");
		_exit(EXIT_FAILURE);
	}

	sigemptyset(&mask);
	ret = sigaddset(&mask, SIGUSR1);
	if (ret == -1) {
		ULOGE("sigaddset: %m");
		_exit(EXIT_FAILURE);
	}
	ret = sigaddset(&mask, SIGCHLD);
	if (ret == -1) {
		ULOGE("sigaddset: %m");
		_exit(EXIT_FAILURE);
	}
	ret = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (ret == -1) {
		ULOGE("sigprocmask: %m");
		_exit(EXIT_FAILURE);
	}
	sfd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (sfd == -1) {
		ULOGE("signalfd: %m");
		_exit(EXIT_FAILURE);
	}

	pid = fork();
	if (pid < 0) {
		ULOGE("fork: %m");
		_exit(EXIT_FAILURE);
	}
	if (pid == 0)
		launch_pid_1(instance, fd);
	ut_file_fd_close(&fd);

	ret = io_read(sfd, &si, sizeof(si));
	if (ret == -1)
		ULOGE("read: %m");
	ut_file_fd_close(&sfd);

	ret = kill(pid, SIGKILL);
	if (ret == -1)
		ULOGE("kill: %m");

	ret = waitpid(pid, &status, 0);
	if (ret < 0) {
		_exit(EXIT_FAILURE);
		ULOGE("waitpid: %m");
	}
	if (WIFEXITED(status))
		ULOGI("program exited with status %d", WEXITSTATUS(status));

	/*
	 * check that hostname hasn't changed, it could break some things like
	 * ulog's pseudo-namespacing
	 */
	ret = gethostname(buf, strlen(instance_name) + 1);
	if (ret < 0)
		ULOGW("gethostname: %m");
	else
		if (!ut_string_match(buf, instance_name))
			ULOGW("hostname has been changed during the life of the"
					" instance, this is bad as it can break"
					" some functionalities like ulog's "
					"pseudo name-spacing");

	ULOGI("instance %s terminated with status %d", instance_name, status);

	ret = io_src_evt_notify(&instance->monitor_evt, 1);
	if (ret < 0)
		ULOGE("io_src_evt_notify: %s", strerror(-ret));

	_exit(EXIT_SUCCESS);
}

static int setup_pts(struct ptspair *ptspair, enum pts_index index)
{
	int ret;
	struct group *g;
	const char *pts_path = ptspair_get_path(ptspair, index);

	g = getgrnam(FIRMWARED_GROUP);
	if (g == NULL) {
		ret = -errno;
		ULOGE("getgrnam(%s): %s", pts_path, strerror(-ret));
		return ret;
	}
	ret = chown(pts_path, -1, g->gr_gid);
	if (ret == -1) {
		ret = -errno;
		ULOGE("chown(%s, -1, %jd): %s", pts_path, (intmax_t)g->gr_gid,
				strerror(-ret));
		return ret;
	}
	ret = chmod(pts_path, 0660);
	if (ret == -1) {
		ret = -errno;
		ULOGE("chmod(%s, 0660): %s", pts_path, strerror(-ret));
		return ret;
	}

	return 0;
}

static int setup_ptspair(struct ptspair *ptspair)
{
	int ret;

	ret = ptspair_init(ptspair);
	if (ret < 0)
		return ret;
	ret = setup_pts(ptspair, PTSPAIR_FOO);
	if (ret < 0) {
		ULOGE("init_pts foo: %s", strerror(-ret));
		return ret;
	}

	return setup_pts(ptspair, PTSPAIR_BAR);
}

/* load sane defaults to launch a boxinit product */
static int init_command_line(struct instance *instance)
{
#define RO_BOOT_CONSOLE "ro.boot.console"
	int ret;
	const char *pts;
	char ro_boot_console[] = RO_BOOT_CONSOLE "=XXXXXXXXXXXXXXX";
	char *argv[] = {
		"/sbin/boxinit",
		ro_boot_console,
		NULL,
	};

	pts = ptspair_get_path(&instance->ptspair, PTSPAIR_BAR);
	snprintf(ro_boot_console, UT_ARRAY_SIZE(ro_boot_console),
			RO_BOOT_CONSOLE "=%s", pts + 4);

	ret = argz_create(argv, &instance->command_line,
			&instance->command_line_len);
	if (ret != 0) {
		ULOGE("argz_create: %s", strerror(ret));
		return -ret;
	}

	return 0;
#undef RO_BOOT_CONSOLE
}

static int init_instance(struct instance *instance,
		struct folder_entity *firmware_entity)
{
	int ret;
	struct firmware *firmware;

	firmware = firmware_from_entity(firmware_entity);

	instance->entity.folder = folder_find(INSTANCES_FOLDER_NAME);
	instance->id = ut_bit_field_claim_free_index(&indices);
	if (instance->id == UT_BIT_FIELD_INVALID_INDEX) {
		ULOGE("ut_bit_field_claim_free_index: No free index");
		return -ENOMEM;
	}
	instance->time = time(NULL);
	instance->state = INSTANCE_READY;
	instance->killer_seqnum = (uint32_t)-1;
	instance->firmware_path = strdup(firmware_get_path(firmware));
	instance->interface = strdup(config_get(CONFIG_CONTAINER_INTERFACE));
	if (instance->firmware_path == NULL || instance->interface == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	ret = init_mount_points(instance);
	if (ret < 0) {
		ULOGE("install_mount_points");
		errno = -ret;
		goto err;
	}

	ret = setup_ptspair(&instance->ptspair);
	if (ret < 0)
		ULOGE("init_ptspair: %s", strerror(-ret));
	ret = io_src_init(&instance->ptspair_src,
			ptspair_get_fd(&instance->ptspair), IO_IN,
			ptspair_src_cb);
	if (ret < 0) {
		ULOGE("io_src_init: %s", strerror(-ret));
		goto err;
	}

	ret = io_src_evt_init(&instance->monitor_evt, monitor_evt_cb, false, 0);
	if (ret < 0) {
		ULOGE("io_src_evt_init: %s", strerror(-ret));
		goto err;
	}

	ret = io_mon_add_sources(firmwared_get_mon(),
			&instance->ptspair_src,
			io_src_evt_get_source(&instance->monitor_evt),
			NULL /* guard */);
	if (ret < 0) {
		ULOGE("io_mon_add_sources: %s", strerror(-ret));
		goto err;
	}

	ret = ut_process_sync_init(&instance->sync, true);
	if (ret < 0) {
		ULOGE("ut_process_sync_init: %s", strerror(-ret));
		goto err;
	}
	if (!config_get_bool(CONFIG_DISABLE_APPARMOR)) {
		ret = apparmor_load_profile(folder_entity_get_base_workspace(
						&instance->entity),
				instance_get_sha1(instance));
		if (ret < 0) {
			ULOGE("apparmor_load_profile: %s", strerror(-ret));
			goto err;
		}
	}
	ret = init_command_line(instance);
	if (ret < 0) {
		ULOGE("init_command_line: %s", strerror(-ret));
		goto err;
	}

	ret = invoke_post_prepare_instance_helper(instance);
	if (ret < 0)
		ULOGW("invoke_post_prepare_instance_helper failed: %d", ret);

	return 0;
err:
	clean_instance(instance, false);

	return ret;
}

static struct instance *instance_new(const char *firmware_identifier)
{
	int ret;
	struct instance *instance;
	struct folder_entity *firmware_entity;

	instance = calloc(1, sizeof(*instance));
	if (instance == NULL)
		return NULL;

	firmware_entity = folder_find_entity(FIRMWARES_FOLDER_NAME,
			firmware_identifier);

	if (firmware_entity == NULL)
		return NULL;

	ret = init_instance(instance, firmware_entity);
	if (ret < 0) {
		ULOGE("init_instance: %s", strerror(-ret));
		goto err;
	}

	return instance;
err:
	instance_delete(&instance, false);

	errno = -ret;

	return NULL;
}

static int instance_preparation_start(struct preparation *preparation)
{
	int ret;
	struct instance *instance;

	instance = instance_new(preparation->identification_string);
	if (instance == NULL) {
		ret = -errno;
		ULOGE("instance_new(%s): %m",
				preparation->identification_string);
		return ret;
	}

	return preparation->completion(preparation,
			instance_to_entity(instance));
}

static struct preparation *instance_get_preparation(void)
{
	struct instance_preparation *instance_preparation;

	instance_preparation = calloc(1, sizeof(*instance_preparation));
	if (instance_preparation == NULL)
		return NULL;

	instance_preparation->preparation.start = instance_preparation_start;
	instance_preparation->preparation.folder = INSTANCES_FOLDER_NAME;

	return &instance_preparation->preparation;
}

static void instance_destroy_preparation(struct preparation **preparation)
{
	struct instance_preparation *instance_preparation;

	instance_preparation = ut_container_of(*preparation,
			struct instance_preparation, preparation);

	memset(instance_preparation, 0, sizeof(*instance_preparation));
	free(instance_preparation);
	*preparation = NULL;
}

struct folder_entity_ops instance_ops = {
		.sha1 = instance_sha1,
		.can_drop = instance_can_drop,
		.drop = instance_drop,
		.get_preparation = instance_get_preparation,
		.destroy_preparation = instance_destroy_preparation,
};

char *instance_state_to_str(enum instance_state state)
{
	switch (state) {
	case INSTANCE_READY:
		return "ready";
	case INSTANCE_STARTED:
		return "started";
	case INSTANCE_STOPPING:
		return "stopping";
	default:
		return "(unknown)";
	}
}

int instances_init(void)
{
	int ret;

	ULOGD("%s", __func__);

	instances_folder.name = INSTANCES_FOLDER_NAME;
	memcpy(&instances_folder.ops, &instance_ops, sizeof(instance_ops));
	ret = folder_register(&instances_folder);
	if (ret < 0) {
		ULOGE("folder_register: %s", strerror(-ret));
		return ret;
	}
	folder_register_properties(INSTANCES_FOLDER_NAME, instance_properties);

	return 0;
}

struct instance *instance_from_entity(struct folder_entity *entity)
{
	errno = -EINVAL;
	if (entity == NULL)
		return NULL;

	return to_instance(entity);
}

struct folder_entity *instance_to_entity(struct instance *instance)
{
	errno = -EINVAL;
	if (instance == NULL)
		return NULL;

	return &instance->entity;
}

static int read_stolen_btusb_id(struct instance *instance)
{
	int ret;
	char __attribute__((cleanup(ut_string_free))) *device_path = NULL;
	const char *id;
	size_t id_len;

	if (ut_string_is_invalid(instance->stolen_btusb))
		return 0;

	ret = asprintf(&device_path, "/sys/class/bluetooth/%s/device",
			instance->stolen_btusb);
	if (ret == -1) {
		device_path = NULL;
		ULOGE("asprintf error");
		return -ENOMEM;
	}
	ret = readlink(device_path, instance->stolen_btusb_id, PATH_MAX - 1);
	if (ret < 0) {
		if (errno == ENOENT) /* device isn't bound to btusb */
			return 0;

		ret = -errno;
		ULOGE("readlink: %m");
		return ret;
	}
	id = basename(instance->stolen_btusb_id);
	id_len = strlen(id);
	memmove(instance->stolen_btusb_id, id, id_len);
	instance->stolen_btusb_id[id_len] = '\0';
	ULOGI("stolen bluetooth device id is %s", instance->stolen_btusb_id);

	return 0;
}

int instance_start(struct instance *instance)
{
	int ret;
	pid_t pid;

	if (instance == NULL)
		return -EINVAL;

	if (instance->state != INSTANCE_READY) {
		ULOGW("wrong state %s for instance %s",
				instance_state_to_str(instance->state),
				instance_get_sha1(instance));
		return -EBUSY;
	}

	/*
	 * the veth pair must be re-created at each instance startup, because it
	 * is automatically deleted at the namespace's destruction
	 */
	ret = invoke_net_helper(instance, "create");
	if (ret != 0) {
		ULOGE("invoke_net_helper create returned %d", ret);
		return -EBUSY;
	}

	instance->state = INSTANCE_STARTED;
	ptspair_cooked(&instance->ptspair, PTSPAIR_BAR);
	pid = fork();
	if (pid == -1) {
		ret = -errno;
		ULOGE("fork: %m");
		return ret;
	}
	if (pid == 0)
		launch_instance(instance); /* in child */
	/* in parent */
	/* the pid must be set before being used, e.g. in invoke_net_helper() */
	instance->pid = pid;

	/*
	 * wait until the child as setup it's network container, so that we can
	 * assign it's network interfaces to it
	 */
	ret = ut_process_sync_parent_lock(&instance->sync);
	if (ret < 0)
		ULOGE("ut_process_sync_parent_lock: parent/child "
				"synchronisation failed: %s", strerror(-ret));

	ret = read_stolen_btusb_id(instance);
	if (ret != 0)
		ULOGE("read_stolen_btusb_id: %s", strerror(-ret));
	ret = invoke_net_helper(instance, "assign");
	if (ret != 0)
		ULOGE("invoke_net_helper assign returned %d", ret);
	/*
	 * now the interface has been assigned to the child's name space, the
	 * child can be unlocked and can now configure the interface to fit it's
	 * needs
	 */
	ret = ut_process_sync_parent_unlock(&instance->sync);
	if (ret < 0)
		ULOGE("ut_process_sync_parent_unlock: parent/child "
				"synchronisation failed: %s", strerror(-ret));


	return 0;
}

int instance_kill(struct instance *instance, uint32_t killer_seqnum)
{
	int ret;

	if (instance == 0)
		return -EINVAL;

	if (instance->state != INSTANCE_STARTED)
		return -ECHILD;

	instance->state = INSTANCE_STOPPING;
	instance->killer_seqnum = killer_seqnum;
	ret = kill(instance->pid, SIGUSR1);
	if (ret < 0) {
		ret = -errno;
		ULOGE("kill: %m");
	}

	return ret;
}

int instance_remount(struct instance *instance)
{
	if (instance == 0)
		return -EINVAL;

	return invoke_mount_helper(instance, "remount", false);
}

const char *instance_get_sha1(struct instance *instance)
{
	errno = EINVAL;
	if (instance == NULL)
		return NULL;

	return compute_sha1(instance);
}

const char *instance_get_name(const struct instance *instance)
{
	errno = EINVAL;
	if (instance == NULL)
		return NULL;

	return instance->entity.name;
}

/*
 * normally, an instance doesn't have to be destroyed manually, except when it
 * has been created successfully, but it's storage in the folder has failed
 */
void instance_delete(struct instance **instance, bool only_unregister)
{
	struct instance *i;

	if (instance == NULL || *instance == NULL)
		return;
	i = *instance;

	clean_instance(i, only_unregister);

	free(i);
	*instance = NULL;
}

void instances_cleanup(void)
{
	ULOGD("%s", __func__);

	/*
	 * instances destruction is managed by instance_drop, called on each
	 * instance by folder_unregister
	 */
	folder_unregister(INSTANCES_FOLDER_NAME);
}
