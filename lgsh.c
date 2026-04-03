/* SPDX-License-Identifier: LGPL-2.1+ */
/* lgsh - SUID program for isolated shell environment with idmapped mounts */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/sched.h>
#include <linux/mount.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

/* ========== mount_setattr() definitions ========== */
#ifndef MOUNT_ATTR_IDMAP
#define MOUNT_ATTR_IDMAP 0x00100000
#endif

#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif

#ifndef __NR_mount_setattr
#define __NR_mount_setattr 442
struct mount_attr {
	__u64 attr_set;
	__u64 attr_clr;
	__u64 propagation;
	__u64 userns_fd;
};
#endif

/* ========== open_tree() definitions ========== */
#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif

#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC O_CLOEXEC
#endif

#ifndef __NR_open_tree
#define __NR_open_tree 428
#endif

/* ========== move_mount() definitions ========== */
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif

#ifndef MOVE_MOUNT_T_EMPTY_PATH
#define MOVE_MOUNT_T_EMPTY_PATH 0x00000040
#endif

/* ========== Helper macros ========== */
#define LGSH_BASE "/home/lgsh"
#define DEFAULT_PATH "/usr/local/cuda/bin:/usr/local/bin:/usr/bin:/bin:/snap/bin"

#define IDMAPLEN 4096
#define STRLITERALLEN(x) (sizeof(""x"") - 1)
#define INTTYPE_TO_STRLEN(type) \
	(2 + (sizeof(type) <= 1 ? 3 : sizeof(type) <= 2 ? 5 : sizeof(type) <= 4 ? 10 : sizeof(type) <= 8 ? 20 : sizeof(int[-2 * (sizeof(type) > 8)])))

#define syserror(format, ...) \
	({ \
		fprintf(stderr, format "\n", ##__VA_ARGS__); \
		(-errno); \
	})

/* ========== Syscall wrappers ========== */
static inline int sys_mount_setattr(int dfd, const char *path, unsigned int flags,
				    struct mount_attr *attr, size_t size)
{
	return syscall(__NR_mount_setattr, dfd, path, flags, attr, size);
}

static inline int sys_open_tree(int dfd, const char *filename, unsigned int flags)
{
	return syscall(__NR_open_tree, dfd, filename, flags);
}

static inline int sys_move_mount(int from_dfd, const char *from_pathname, int to_dfd,
				 const char *to_pathname, unsigned int flags)
{
	return syscall(__NR_move_mount, from_dfd, from_pathname, to_dfd, to_pathname, flags);
}

/* ========== ID Mapping structures ========== */
typedef enum idmap_type_t {
	ID_TYPE_UID,
	ID_TYPE_GID
} idmap_type_t;

struct id_map {
	idmap_type_t map_type;
	__u32 nsid;
	__u32 hostid;
	__u32 range;
};

struct list {
	void *elem;
	struct list *next;
	struct list *prev;
};

static struct list active_map;

static inline void list_init(struct list *list)
{
	list->elem = NULL;
	list->next = list->prev = list;
}

static inline int list_empty(const struct list *list)
{
	return list == list->next;
}

static inline void __list_add(struct list *new, struct list *prev, struct list *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void list_add_tail(struct list *head, struct list *list)
{
	__list_add(list, head->prev, head);
}

#define list_for_each(__iterator, __list) \
	for (__iterator = (__list)->next; __iterator != __list; __iterator = __iterator->next)

/* ========== Cleanup macros ========== */
#define call_cleaner(cleaner) __attribute__((__cleanup__(cleaner##_function)))

#define free_disarm(ptr) \
	({ \
		free(ptr); \
		ptr = NULL; \
	})

static inline void free_disarm_function(void *ptr)
{
	free_disarm(*(void **)ptr);
}
#define __do_free call_cleaner(free_disarm)

#define move_ptr(ptr) \
	({ \
		typeof(ptr) __internal_ptr__ = (ptr); \
		(ptr) = NULL; \
		__internal_ptr__; \
	})

/* Add ID map entry
 * For idmapped mount: files with UID=nsid in source will appear as UID=hostid in target
 * uid_map format: "nsid hostid range"
 */
static inline int add_map_entry(__u32 id_nsid, __u32 id_hostid, __u32 range, idmap_type_t map_type)
{
	__do_free struct list *new_list = NULL;
	__do_free struct id_map *newmap = NULL;

	newmap = malloc(sizeof(*newmap));
	if (!newmap)
		return -ENOMEM;

	new_list = malloc(sizeof(struct list));
	if (!new_list)
		return -ENOMEM;

	*newmap = (struct id_map){
		.nsid = id_nsid,      /* source UID to map */
		.hostid = id_hostid,  /* target UID to show */
		.range = range,
		.map_type = map_type,
	};

	new_list->elem = move_ptr(newmap);
	list_add_tail(&active_map, move_ptr(new_list));
	return 0;
}

/* ========== Clone helper for user namespace ========== */
#define __STACK_SIZE (8 * 1024 * 1024)

static int clone_cb(void *data)
{
	(void)data;
	return kill(getpid(), SIGSTOP);
}

static pid_t do_clone(int (*fn)(void *), void *arg, int flags)
{
	void *stack;

	stack = malloc(__STACK_SIZE);
	if (!stack)
		return -ENOMEM;

#ifdef __ia64__
	return __clone2(fn, stack, __STACK_SIZE, flags | SIGCHLD, arg, NULL);
#else
	return clone(fn, stack + __STACK_SIZE, flags | SIGCHLD, arg, NULL);
#endif
}

/* ========== ID Mapping functions ========== */
static ssize_t write_nointr(int fd, const void *buf, size_t count)
{
	ssize_t ret;
	do {
		ret = write(fd, buf, count);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static int write_id_mapping(idmap_type_t map_type, pid_t pid, const char *buf, size_t buf_size)
{
	int fd = -EBADF;
	int ret;
	char path[PATH_MAX];

	/* Always write "deny" to setgroups for GID mapping to ensure kernel accepts it */
	if (map_type == ID_TYPE_GID) {
		int setgroups_fd = -EBADF;
		ret = snprintf(path, PATH_MAX, "/proc/%d/setgroups", pid);
		if (ret < 0 || ret >= PATH_MAX)
			return -E2BIG;

		setgroups_fd = open(path, O_WRONLY | O_CLOEXEC);
		if (setgroups_fd < 0 && errno != ENOENT)
			return syserror("Failed to open %s", path);

		if (setgroups_fd >= 0) {
			ret = write_nointr(setgroups_fd, "deny\n", 5);
			close(setgroups_fd);
			if (ret != 5)
				return syserror("Failed to write 'deny' to /proc/%d/setgroups", pid);
		}
	}

	ret = snprintf(path, PATH_MAX, "/proc/%d/%cid_map", pid, map_type == ID_TYPE_UID ? 'u' : 'g');
	if (ret < 0 || ret >= PATH_MAX)
		return -E2BIG;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return syserror("Failed to open %s", path);

	ret = write_nointr(fd, buf, buf_size);
	close(fd);
	if (ret != (ssize_t)buf_size)
		return syserror("Failed to write %cid mapping to %s", map_type == ID_TYPE_UID ? 'u' : 'g', path);

	return 0;
}

static int map_ids(struct list *idmap, pid_t pid)
{
	char mapbuf[IDMAPLEN];
	bool had_entry;

	for (idmap_type_t map_type = ID_TYPE_UID; map_type <= ID_TYPE_GID; map_type++) {
		char *pos = mapbuf;
		int ret;
		struct list *iterator;
		char u_or_g = (map_type == ID_TYPE_UID) ? 'u' : 'g';
		int left, fill;

		had_entry = false;
		list_for_each(iterator, idmap) {
			struct id_map *map = iterator->elem;
			if (map->map_type != map_type)
				continue;

			had_entry = true;
			left = IDMAPLEN - (pos - mapbuf);
			fill = snprintf(pos, left, "%u %u %u\n", map->nsid, map->hostid, map->range);
			if (fill <= 0 || fill >= left)
				return syserror("Too many %cid mappings defined", u_or_g);
			pos += fill;
		}

		if (!had_entry)
			continue;

		ret = write_id_mapping(map_type, pid, mapbuf, pos - mapbuf);
		if (ret < 0)
			return syserror("Failed to write mapping: %s", mapbuf);

		memset(mapbuf, 0, sizeof(mapbuf));
	}

	return 0;
}

static int wait_for_pid(pid_t pid)
{
	int status, ret;
again:
	ret = waitpid(pid, &status, 0);
	if (ret < 0) {
		if (errno == EINTR)
			goto again;
		return -1;
	}
	if (ret != pid)
		goto again;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

/* Wait for child to stop (not exit) */
static int wait_for_stop(pid_t pid)
{
	int status, ret;
again:
	ret = waitpid(pid, &status, WUNTRACED);
	if (ret < 0) {
		if (errno == EINTR)
			goto again;
		return -1;
	}
	if (ret != pid)
		goto again;
	if (!WIFSTOPPED(status))
		return -1;
	return 0;
}

static int get_userns_fd(struct list *idmap)
{
	int ret;
	pid_t pid;
	char path_ns[PATH_MAX];

	/* Use clone with CLONE_NEWUSER similar to original mount-idmapped */
	pid = do_clone(clone_cb, NULL, CLONE_NEWUSER);
	if (pid < 0)
		return syserror("cannot clone child in new userns");


	/* Parent: wait for child to stop, then write mappings */
	ret = wait_for_stop(pid);
	if (ret < 0) {
		kill(pid, SIGKILL);
		wait_for_pid(pid);
		return syserror("child did not stop properly");
	}

	ret = map_ids(idmap, pid);
	if (ret < 0) {
		kill(pid, SIGKILL);
		wait_for_pid(pid);
		return ret;
	}

	/* Open the user namespace fd */
	ret = snprintf(path_ns, sizeof(path_ns), "/proc/%d/ns/user", pid);
	if (ret < 0 || (size_t)ret >= sizeof(path_ns))
		ret = -EIO;
	else
		ret = open(path_ns, O_RDONLY | O_CLOEXEC | O_NOCTTY);

	kill(pid, SIGKILL);
	wait_for_pid(pid);
	return ret;
}

/* ========== Security functions (from security.go) ========== */

/* Check if program is running as SUID root */
static int check_suid_root(void)
{
	uid_t euid = geteuid();
	uid_t ruid = getuid();

	if (euid != 0)
		return syserror("program must be run as SUID root (effective UID is %d, expected 0)", euid);

	if (ruid == 0)
		return syserror("program cannot be run directly as root");

	return 0;
}

/* Check if caller is in the executable's group */
static int check_group_membership(void)
{
	struct stat file_stat;
	char exec_path[PATH_MAX];
	gid_t caller_gid = getgid();
	int ret;

	ret = readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1);
	if (ret < 0)
		return syserror("cannot get executable path");
	exec_path[ret] = '\0';

	if (stat(exec_path, &file_stat) < 0)
		return syserror("cannot stat executable");

	if (caller_gid == file_stat.st_gid)
		return 0;

	/* Check supplementary groups */
	int ngroups = getgroups(0, NULL);
	if (ngroups < 0)
		return syserror("cannot get groups");

	gid_t *groups = malloc(ngroups * sizeof(gid_t));
	if (!groups)
		return -ENOMEM;

	if (getgroups(ngroups, groups) < 0) {
		free(groups);
		return syserror("cannot get groups");
	}

	for (int i = 0; i < ngroups; i++) {
		if (groups[i] == file_stat.st_gid) {
			free(groups);
			return 0;
		}
	}

	free(groups);
	return syserror("user is not in the required group (GID %d)", file_stat.st_gid);
}

/* Verify working directory is under HOME */
static int verify_working_directory(void)
{
	char cwd[PATH_MAX], real_cwd[PATH_MAX];
	char real_home[PATH_MAX];
	char *home_env;

	if (!getcwd(cwd, sizeof(cwd)))
		return syserror("cannot get working directory");

	if (realpath(cwd, real_cwd) == NULL)
		return syserror("cannot resolve symlinks in working directory");

	home_env = getenv("HOME");
	if (!home_env)
		return syserror("HOME environment variable is not set");

	if (realpath(home_env, real_home) == NULL)
		return syserror("cannot resolve symlinks in HOME");

	/* Check if cwd is a subdirectory of HOME (not HOME itself) */
	size_t home_len = strlen(real_home);
	if (strncmp(real_cwd, real_home, home_len) != 0 || real_cwd[home_len] != '/')
		return syserror("working directory '%s' is not a subdirectory of HOME '%s'", cwd, home_env);

	return 0;
}

/* Get username from UID */
static char *get_username(void)
{
	uid_t uid = getuid();
	struct passwd *pw = getpwuid(uid);
	if (pw)
		return pw->pw_name;

	/* Fallback: use uid number */
	static char uidbuf[32];
	snprintf(uidbuf, sizeof(uidbuf), "uid%d", uid);
	return uidbuf;
}

/* ========== Path and mount functions (from mount.go) ========== */

/* Get target UID/GID from directory ownership */
static int get_target_uid_gid(const char *path, uid_t *uid, gid_t *gid)
{
	struct stat statbuf;
	if (stat(path, &statbuf) < 0)
		return syserror("cannot stat %s", path);
	*uid = statbuf.st_uid;
	*gid = statbuf.st_gid;
	return 0;
}

/* Build target path: /home/lgsh/<username>_<dirname> */
static void build_target_path(char *target, size_t target_size,
			      const char *home, const char *cwd,
			      const char *lgsh_base, const char *username)
{
	const char *rel;
	size_t home_len = strlen(home);

	/* Get relative path from HOME */
	if (strncmp(cwd, home, home_len) == 0 && cwd[home_len] == '/')
		rel = cwd + home_len + 1;
	else
		rel = cwd;

	/* Get the directory name */
	const char *dirname = strrchr(rel, '/');
	if (dirname)
		dirname++;
	else
		dirname = rel;

	if (!dirname || *dirname == '\0')
		dirname = "home";

	/* Build path */
	snprintf(target, target_size, "%s/%s_%s", lgsh_base, username, dirname);
}

/* Create mount namespace */
static int create_mount_namespace(void)
{
	if (unshare(CLONE_NEWNS) < 0)
		return syserror("cannot create mount namespace");

	/* Set root mount to private propagation to prevent mounts from
	 * propagating to the parent namespace */
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		return syserror("cannot set root mount to private");

	return 0;
}

/* Bind mount file (optional, skip if source doesn't exist) */
static int bind_mount_file(const char *source, const char *target)
{
	struct stat statbuf;
	int fd;

	if (stat(source, &statbuf) < 0)
		return 0; /* Source doesn't exist, skip */

	/* Create target file if needed */
	fd = open(target, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0)
		close(fd);

	if (mount(source, target, "none", MS_BIND, NULL) < 0)
		return syserror("cannot bind mount: %s -> %s", source, target);

	return 0;
}

/* Setup idmapped mount */
static int setup_idmapped_mount(const char *source, const char *target,
				uid_t caller_uid, gid_t caller_gid,
				uid_t target_uid, gid_t target_gid)
{
	int fd_tree, fd_userns, ret;
	struct mount_attr attr = {};

	/* Create target directory */
	if (mkdir(target, 0755) < 0 && errno != EEXIST)
		return syserror("cannot create target directory %s", target);

	/* Initialize ID map */
	list_init(&active_map);

	/* Add UID mapping: nsid=caller_uid, hostid=target_uid, range=1
	 * uid_map format: "nsid hostid range" -> writes "caller_uid target_uid 1"
	 * Effect: files owned by caller_uid in source appear as target_uid in target */
	ret = add_map_entry(caller_uid, target_uid, 1, ID_TYPE_UID);
	if (ret < 0)
		return ret;

	/* Add GID mapping */
	ret = add_map_entry(caller_gid, target_gid, 1, ID_TYPE_GID);
	if (ret < 0)
		return ret;

	/* Get user namespace fd */
	fd_userns = get_userns_fd(&active_map);
	if (fd_userns < 0)
		return syserror("cannot create user namespace for idmapping");

	/* Clone the source mount tree */
	fd_tree = sys_open_tree(-EBADF, source,
				OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
	if (fd_tree < 0) {
		close(fd_userns);
		return syserror("cannot open tree %s", source);
	}

	/* Apply idmap to the cloned tree */
	attr.attr_set = MOUNT_ATTR_IDMAP;
	attr.userns_fd = fd_userns;

	ret = sys_mount_setattr(fd_tree, "", AT_EMPTY_PATH | AT_RECURSIVE, &attr, sizeof(attr));
	if (ret < 0) {
		close(fd_tree);
		close(fd_userns);
		return syserror("cannot set mount attributes");
	}

	close(fd_userns);

	/* Move the idmapped mount to target */
	ret = sys_move_mount(fd_tree, "", -EBADF, target, MOVE_MOUNT_F_EMPTY_PATH);
	close(fd_tree);
	if (ret < 0)
		return syserror("cannot move mount to %s", target);

	return 0;
}

/* ========== Environment setup ========== */

/* Get latest version from directory (sorted alphabetically, last entry) */
static int get_latest_version(const char *base_path, char *version_out, size_t max_len)
{
	DIR *dir;
	struct dirent *entry;
	char latest[256] = "";

	dir = opendir(base_path);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		if (strcmp(entry->d_name, latest) > 0)
			strncpy(latest, entry->d_name, sizeof(latest) - 1);
	}
	closedir(dir);

	if (latest[0] == '\0')
		return -1;

	strncpy(version_out, latest, max_len - 1);
	version_out[max_len - 1] = '\0';
	return 0;
}

static void setup_environment(void)
{
	char path_buf[PATH_MAX * 8];
	char nvm_version[64];
	char ndk_version[64];
	char build_tools_version[64];
	char cmake_version[64];
	char nvm_bin[PATH_MAX];
	char nvm_inc[PATH_MAX];
	char ndk_path[PATH_MAX];

	/* USER: set to 'lg' (the lgsh user) */
	setenv("USER", "lg", 1);
	setenv("TZ", "America/Los_Angeles", 1);

	/* NVM environment - detect version */
	if (get_latest_version(LGSH_BASE "/.nvm/versions/node", nvm_version, sizeof(nvm_version)) == 0) {
		setenv("NVM_DIR", LGSH_BASE "/.nvm", 1);
		snprintf(nvm_bin, sizeof(nvm_bin), LGSH_BASE "/.nvm/versions/node/%s/bin", nvm_version);
		snprintf(nvm_inc, sizeof(nvm_inc), LGSH_BASE "/.nvm/versions/node/%s/include", nvm_version);
		setenv("NVM_BIN", nvm_bin, 1);
		setenv("NVM_INC", nvm_inc, 1);
	}

	/* Miniconda environment */
	setenv("VIRTUAL_ENV", LGSH_BASE "/miniconda3", 1);

	/* NDK environment - detect version */
	if (get_latest_version(LGSH_BASE "/tools/ndk", ndk_version, sizeof(ndk_version)) == 0) {
		snprintf(ndk_path, sizeof(ndk_path), LGSH_BASE "/tools/ndk/%s", ndk_version);
		setenv("ANDROID_NDK_HOME", ndk_path, 1);
		setenv("NDK_ROOT", ndk_path, 1);
		setenv("NDK_PROJECT_PATH", ndk_path, 1);
	}

	/* Build PATH with detected versions */
	path_buf[0] = '\0';

	/* Miniconda */
	snprintf(path_buf + strlen(path_buf), sizeof(path_buf) - strlen(path_buf),
		LGSH_BASE "/miniconda3/bin:");

	/* NVM */
	if (nvm_version[0])
		snprintf(path_buf + strlen(path_buf), sizeof(path_buf) - strlen(path_buf),
			LGSH_BASE "/.nvm/versions/node/%s/bin:", nvm_version);

	/* NDK */
	if (ndk_version[0])
		snprintf(path_buf + strlen(path_buf), sizeof(path_buf) - strlen(path_buf),
			LGSH_BASE "/tools/ndk/%s:"
			LGSH_BASE "/tools/ndk/%s/toolchains/llvm-prebuilt/linux-x86_64/bin:",
			ndk_version, ndk_version);

	/* Build tools */
	if (get_latest_version(LGSH_BASE "/tools/build-tools", build_tools_version, sizeof(build_tools_version)) == 0)
		snprintf(path_buf + strlen(path_buf), sizeof(path_buf) - strlen(path_buf),
			LGSH_BASE "/tools/build-tools/%s:", build_tools_version);

	/* Platform tools */
	snprintf(path_buf + strlen(path_buf), sizeof(path_buf) - strlen(path_buf),
		LGSH_BASE "/tools/platform-tools:");

	/* Cmdline tools */
	snprintf(path_buf + strlen(path_buf), sizeof(path_buf) - strlen(path_buf),
		LGSH_BASE "/tools/cmdline-tools/latest/bin:");

	/* CMake */
	if (get_latest_version(LGSH_BASE "/tools/cmake", cmake_version, sizeof(cmake_version)) == 0)
		snprintf(path_buf + strlen(path_buf), sizeof(path_buf) - strlen(path_buf),
			LGSH_BASE "/tools/cmake/%s/bin:", cmake_version);

	/* User local bin */
	snprintf(path_buf + strlen(path_buf), sizeof(path_buf) - strlen(path_buf),
		LGSH_BASE "/.local/bin:");

	/* Default path */
	snprintf(path_buf + strlen(path_buf), sizeof(path_buf) - strlen(path_buf),
		DEFAULT_PATH);

	setenv("PATH", path_buf, 1);
}

/* ========== Main ========== */
int main(int argc, char *argv[])
{
	char cwd[PATH_MAX], home[PATH_MAX], target_path[PATH_MAX];
	uid_t caller_uid, target_uid;
	gid_t caller_gid, target_gid;
	char *username;
	int ret;

	/* 1. Security checks */
	if (check_suid_root() < 0)
		return EXIT_FAILURE;

	caller_uid = getuid();
	caller_gid = getgid();

	if (check_group_membership() < 0)
		return EXIT_FAILURE;

	if (verify_working_directory() < 0)
		return EXIT_FAILURE;

	/* 2. Get paths */
	if (!getcwd(cwd, sizeof(cwd))) {
		fprintf(stderr, "Error: cannot get working directory\n");
		return EXIT_FAILURE;
	}

	char *home_env = getenv("HOME");
	if (!home_env) {
		fprintf(stderr, "Error: HOME is not set\n");
		return EXIT_FAILURE;
	}
	strncpy(home, home_env, sizeof(home) - 1);

	username = get_username();
	build_target_path(target_path, sizeof(target_path), home, cwd, LGSH_BASE, username);

	/* 3. Get target UID/GID */
	if (get_target_uid_gid(LGSH_BASE, &target_uid, &target_gid) < 0)
		return EXIT_FAILURE;

	/* 4. Create mount namespace */
	if (create_mount_namespace() < 0)
		return EXIT_FAILURE;

	/* 5. Setup idmapped mount */
	ret = setup_idmapped_mount(cwd, target_path, caller_uid, caller_gid, target_uid, target_gid);
	if (ret < 0)
		return EXIT_FAILURE;

	/* 6. Bind mount optional config files */
	if (bind_mount_file(LGSH_BASE "/.config/hosts", "/etc/hosts") < 0)
		return EXIT_FAILURE;

	if (bind_mount_file(LGSH_BASE "/.config/resolv.conf", "/etc/resolv.conf") < 0)
		return EXIT_FAILURE;

	/* 7. Change to target directory (before setuid) */
	if (chdir(target_path) < 0) {
		fprintf(stderr, "Error: cannot change directory to %s\n", target_path);
		return EXIT_FAILURE;
	}

	/* Update PWD environment variable for shells like bash */
	setenv("PWD", target_path, 1);

	/* 8. Set UID/GID */
	if (setgid(target_gid) < 0) {
		fprintf(stderr, "Error: cannot set GID to %d\n", target_gid);
		return EXIT_FAILURE;
	}

	if (setuid(target_uid) < 0) {
		fprintf(stderr, "Error: cannot set UID to %d\n", target_uid);
		return EXIT_FAILURE;
	}

	/* 9. Setup environment */
	setenv("HOME", LGSH_BASE, 1);
	setup_environment();

	/* 10. Exec program */
	if (argc > 1) {
		/* Use argv[1] and remaining args as the program to run */
		execvp(argv[1], &argv[1]);
		fprintf(stderr, "Error: cannot exec %s\n", argv[1]);
	} else {
		/* Default: exec tmux with session management */
		char *session_name;
		char check_cmd[PATH_MAX];
		int session_exists;

		/* Extract session name from target_path (last component) */
		session_name = strrchr(target_path, '/');
		if (session_name)
			session_name++;
		else
			session_name = target_path;

		/* Check if session already exists */
		snprintf(check_cmd, sizeof(check_cmd),
			"tmux has-session -t %s 2>/dev/null", session_name);
		session_exists = (system(check_cmd) == 0);

		if (session_exists) {
			/* Attach to existing session */
			char *const tmux_argv[] = {"tmux", "attach", "-t", session_name, NULL};
			execvp("tmux", tmux_argv);
		} else {
			/* Create new session with name */
			char *const tmux_argv[] = {"tmux", "new", "-s", session_name, NULL};
			execvp("tmux", tmux_argv);
		}
		fprintf(stderr, "Error: cannot exec tmux\n");
	}
	return EXIT_FAILURE;
}