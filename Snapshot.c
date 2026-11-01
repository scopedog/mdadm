// SPDX-License-Identifier: GPL-2.0-only
/*
 * mdadm --snapshot: native snapshots of a raidkm array created with
 * --snapshots=<reserve %>, over the kernel's md/rk_snap.
 *
 *   mdadm --snapshot /dev/mdX --create <name> [--attr key=value]... [--tag <t>]
 *   mdadm --snapshot /dev/mdX --destroy <name>
 *   mdadm --snapshot /dev/mdX --rename <old> <new>
 *   mdadm --snapshot /dev/mdX --set <name> key=value...
 *   mdadm --snapshot /dev/mdX --list [<name>] [--export]
 *   mdadm --snapshot /dev/mdX --exists <name>
 *   mdadm --snapshot /dev/mdX --checkpoint
 *
 * A snapshot is a read-only block device /dev/mdXs<id>.  Listing lines:
 * "name=<n> id=<i> ctime=<s> dev=<d> exc=<rows> [key=value]...", in id
 * order, read from each snapshot disk's /sys/block/mdXs<id>/rk_snap/ (the
 * array's md/rk_snap listing is one page, which a few snapshots with
 * attributes fill).  --tag t stores magic=t-ss (what a snapshot tool
 * looks for).
 *
 * Exit: 0 ok, 1 no such snapshot, 16 busy (mounted), 17 exists,
 * 22 bad argument, 5 anything else.
 */
#include "mdadm.h"
#include <limits.h>
#include <dirent.h>

#define SNAP_NAME_MAX	63		/* the kernel's: [A-Za-z0-9._-]{1,63} */

static char snap_sys[PATH_MAX + 32];	/* /sys/block/<name>/md/rk_snap */
static char snap_md[NAME_MAX + 1];	/* <name> */

static int snap_exit(int err)
{
	switch (err) {
	case 0:		return 0;
	case ENOENT:	return 1;
	case EBUSY:	return 16;
	case EEXIST:	return 17;
	case EINVAL:	return 22;
	}
	return 5;
}

static int snap_write(const char *op, const char *cmd)
{
	int fd = open(snap_sys, O_WRONLY), err = 0;
	ssize_t n;

	if (fd < 0) {
		err = errno;	/* pr_err() may change it */
		pr_err("%s: %s: %s\n", op, snap_sys, strerror(err));
		return snap_exit(err);
	}
	n = write(fd, cmd, strlen(cmd));
	if (n < 0)
		err = errno;
	close(fd);
	if (err)
		pr_err("%s: %s\n", op, strerror(err));
	return snap_exit(err);
}

/* one value of a snapshot disk's rk_snap/ (into v, no newline); 0 = ok */
static int snap_attr(const char *disk, const char *key, char *v, size_t len)
{
	char path[PATH_MAX];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "/sys/block/%s/rk_snap/%s", disk, key);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, v, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n && v[n - 1] == '\n')
		n--;
	v[n] = 0;
	return 0;
}

struct snap_ent {
	int	id;
	char	name[SNAP_NAME_MAX + 1];
	char	line[2048];
};

static int snap_ent_cmp(const void *a, const void *b)
{
	return ((const struct snap_ent *)a)->id - ((const struct snap_ent *)b)->id;
}

/* every snapshot with a disk: /sys/block/<md>s<id>/rk_snap/; *n entries
 * (malloc'ed, sorted by id), NULL on error */
static struct snap_ent *snap_scan(int *n)
{
	struct snap_ent *e = NULL, *t;
	size_t ml = strlen(snap_md);
	struct dirent *de;
	DIR *d = opendir("/sys/block");
	int cap = 0;

	*n = 0;
	if (!d)
		return NULL;
	while ((de = readdir(d))) {
		char name[SNAP_NAME_MAX + 1], id[32], ct[32], ex[32], at[1100];

		/* <md>s<digits> only: md7s* is not md70's */
		if (strncmp(de->d_name, snap_md, ml) || de->d_name[ml] != 's' ||
		    !de->d_name[ml + 1] ||
		    strspn(de->d_name + ml + 1, "0123456789") !=
		    strlen(de->d_name + ml + 1))
			continue;
		if (snap_attr(de->d_name, "name", name, sizeof(name)) ||
		    snap_attr(de->d_name, "id", id, sizeof(id)) ||
		    snap_attr(de->d_name, "ctime", ct, sizeof(ct)) ||
		    snap_attr(de->d_name, "exc", ex, sizeof(ex)) ||
		    snap_attr(de->d_name, "attrs", at, sizeof(at)))
			continue;	/* going away meanwhile */
		if (*n == cap) {
			cap = cap ? cap * 2 : 16;
			t = realloc(e, cap * sizeof(*e));
			if (!t) {
				free(e);
				closedir(d);
				return NULL;
			}
			e = t;
		}
		e[*n].id = atoi(id);
		snprintf(e[*n].name, sizeof(e[*n].name), "%s", name);
		snprintf(e[*n].line, sizeof(e[*n].line),
			 "name=%s id=%s ctime=%s dev=/dev/%s exc=%s%s%s",
			 name, id, ct, de->d_name, ex, *at ? " " : "", at);
		(*n)++;
	}
	closedir(d);
	if (*n)
		qsort(e, *n, sizeof(*e), snap_ent_cmp);
	return e ? e : calloc(1, sizeof(*e));
}

/*
 * The listing line of snapshot name (into out), or 0 when there is none.
 * From the snapshot disks; a snapshot whose disk could not be made (dev=-)
 * only shows in md/rk_snap, read as a fallback.
 */
static int snap_line(const char *name, char *out, size_t len)
{
	static char buf[65536];
	struct snap_ent *e;
	char *l, *save = NULL;
	size_t nl = strlen(name);
	ssize_t r;
	int n, i, fd, found = 0;

	e = snap_scan(&n);
	for (i = 0; e && i < n && !found; i++)
		if (!strcmp(e[i].name, name)) {
			if (out)
				snprintf(out, len, "%s", e[i].line);
			found = 1;
		}
	free(e);
	if (found)
		return 1;
	fd = open(snap_sys, O_RDONLY);
	if (fd < 0)
		return 0;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return 0;
	buf[r] = 0;
	for (l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		/* a whole line only: the page may end in the middle of one */
		if (strncmp(l, "snap name=", 10) || strncmp(l + 10, name, nl) ||
		    l[10 + nl] != ' ' || !strstr(l, " exc="))
			continue;
		if (out)
			snprintf(out, len, "%s", l + 5);
		return 1;
	}
	return 0;
}

static int snap_ok_word(const char *w)
{
	return w && *w && strlen(w) < 2048 && !strchr(w, ' ') &&
	       !strchr(w, '\n');
}

static int snap_ok_name(const char *w)
{
	return snap_ok_word(w) && strlen(w) <= SNAP_NAME_MAX;
}

static void snap_usage(void)
{
	fprintf(stderr,
		"Usage: mdadm --snapshot /dev/mdX --create <name> [--attr k=v]... [--tag t]\n"
		"       mdadm --snapshot /dev/mdX --destroy <name>\n"
		"       mdadm --snapshot /dev/mdX --rename <old> <new>\n"
		"       mdadm --snapshot /dev/mdX --set <name> k=v...\n"
		"       mdadm --snapshot /dev/mdX --list [<name>] [--export]\n"
		"       mdadm --snapshot /dev/mdX --exists <name>\n"
		"       mdadm --snapshot /dev/mdX --checkpoint\n");
}

/* argv[0] = the array, argv[1] = the operation, then its arguments */
int Snapshot(int argc, char *argv[])
{
	char real[PATH_MAX], cmd[8192], line[8192];
	const char *op;
	char *base;
	int i;
	size_t n;

	if (argc < 2) {
		snap_usage();
		return 22;
	}
	if (!realpath(argv[0], real)) {
		pr_err("%s: %s\n", argv[0], strerror(errno));
		return 22;
	}
	base = strrchr(real, '/');
	base = base ? base + 1 : real;
	if (strlen(base) >= sizeof(snap_md))
		return 22;
	strcpy(snap_md, base);
	snprintf(snap_sys, sizeof(snap_sys), "/sys/block/%s/md/rk_snap", base);
	if (access(snap_sys, R_OK)) {
		pr_err("%s is not a raidkm array (no md/rk_snap)\n", argv[0]);
		return 22;
	}
	op = argv[1];
	argc -= 2;
	argv += 2;

	if (!strcmp(op, "--create")) {
		if (argc < 1 || !snap_ok_name(argv[0]))
			return 22;
		n = snprintf(cmd, sizeof(cmd), "create %s", argv[0]);
		for (i = 1; i < argc; i += 2) {
			if (i + 1 >= argc || !snap_ok_word(argv[i + 1]))
				return 22;
			if (!strcmp(argv[i], "--attr"))
				n += snprintf(cmd + n, sizeof(cmd) - n, " %s",
					      argv[i + 1]);
			else if (!strcmp(argv[i], "--tag"))
				n += snprintf(cmd + n, sizeof(cmd) - n,
					      " magic=%s-ss", argv[i + 1]);
			else
				return 22;
			if (n >= sizeof(cmd))
				return 22;
		}
		return snap_write("--create", cmd);
	}
	if (!strcmp(op, "--destroy")) {
		if (argc != 1 || !snap_ok_name(argv[0]))
			return 22;
		/* the kernel says whether it exists (ENOENT: exit 1) */
		snprintf(cmd, sizeof(cmd), "destroy %s", argv[0]);
		return snap_write("--destroy", cmd);
	}
	if (!strcmp(op, "--rename")) {
		if (argc != 2 || !snap_ok_name(argv[0]) || !snap_ok_name(argv[1]))
			return 22;
		snprintf(cmd, sizeof(cmd), "rename %s %s", argv[0], argv[1]);
		return snap_write("--rename", cmd);
	}
	if (!strcmp(op, "--set")) {
		if (argc < 2 || !snap_ok_name(argv[0]))
			return 22;
		n = snprintf(cmd, sizeof(cmd), "set %s", argv[0]);
		for (i = 1; i < argc; i++) {
			if (!snap_ok_word(argv[i]))
				return 22;
			n += snprintf(cmd + n, sizeof(cmd) - n, " %s", argv[i]);
			if (n >= sizeof(cmd))
				return 22;
		}
		return snap_write("--set", cmd);
	}
	if (!strcmp(op, "--exists")) {
		if (argc != 1)
			return 22;
		return snap_line(argv[0], NULL, 0) ? 0 : 1;
	}
	if (!strcmp(op, "--checkpoint")) {
		if (argc)
			return 22;
		return snap_write("--checkpoint", "checkpoint");
	}
	if (!strcmp(op, "--list")) {
		const char *filter = NULL;
		struct snap_ent *e;
		int n;

		for (i = 0; i < argc; i++) {
			if (!strcmp(argv[i], "--export"))
				continue;	/* the listing is the export form */
			if (filter)
				return 22;
			filter = argv[i];
		}
		if (filter) {
			if (!snap_line(filter, line, sizeof(line)))
				return 1;
			printf("%s\n", line);
			return 0;
		}
		e = snap_scan(&n);
		if (!e)
			return 5;
		for (i = 0; i < n; i++)
			printf("%s\n", e[i].line);
		free(e);
		return 0;
	}
	snap_usage();
	return 22;
}
