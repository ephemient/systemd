/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2012 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdio.h>
#include <mntent.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "util.h"
#include "unit-name.h"
#include "path-util.h"
#include "mount-setup.h"
#include "special.h"
#include "mkdir.h"
#include "fileio.h"

static const char *arg_dest = "/tmp";
static bool arg_enabled = true;

static int mount_find_pri(struct mntent *me, int *ret) {
        char *end, *pri;
        unsigned long r;

        assert(me);
        assert(ret);

        pri = hasmntopt(me, "pri");
        if (!pri)
                return 0;

        pri += 4;

        errno = 0;
        r = strtoul(pri, &end, 10);
        if (errno > 0)
                return -errno;

        if (end == pri || (*end != ',' && *end != 0))
                return -EINVAL;

        *ret = (int) r;
        return 1;
}

static int add_swap(const char *what, struct mntent *me) {
        _cleanup_free_ char *name = NULL, *unit = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        bool noauto;
        int r, pri = -1;

        assert(what);
        assert(me);

        r = mount_find_pri(me, &pri);
        if (r < 0) {
                log_error("Failed to parse priority");
                return pri;
        }

        noauto = !!hasmntopt(me, "noauto");

        name = unit_name_from_path(what, ".swap");
        if (!name)
                return log_oom();

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                if (errno == EEXIST)
                        log_error("Failed to create swap unit file %s, as it already exists. Duplicate entry in /etc/fstab?", unit);
                else
                        log_error("Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fprintf(f,
                "# Automatically generated by systemd-fstab-generator\n\n"
                "[Unit]\n"
                "SourcePath=/etc/fstab\n\n"
                "[Swap]\n"
                "What=%s\n",
                what);

        if (pri >= 0)
                fprintf(f,
                        "Priority=%i\n",
                        pri);

        fflush(f);
        if (ferror(f)) {
                log_error("Failed to write unit file %s: %m", unit);
                return -errno;
        }

        if (!noauto) {
                lnk = strjoin(arg_dest, "/" SPECIAL_SWAP_TARGET ".wants/", name, NULL);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(unit, lnk) < 0) {
                        log_error("Failed to create symlink %s: %m", lnk);
                        return -errno;
                }
        }

        return 0;
}

static bool mount_is_network(struct mntent *me) {
        assert(me);

        return
                hasmntopt(me, "_netdev") ||
                fstype_is_network(me->mnt_type);
}

static bool mount_in_initrd(struct mntent *me) {
        assert(me);

        return
                hasmntopt(me, "x-initrd.mount") ||
                streq(me->mnt_dir, "/usr");
}

static int add_mount(
                const char *what,
                const char *where,
                const char *type,
                const char *opts,
                int passno,
                bool noauto,
                bool nofail,
                bool automount,
                const char *post,
                const char *source) {
        _cleanup_free_ char
                *name = NULL, *unit = NULL, *lnk = NULL,
                *automount_name = NULL, *automount_unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;

        assert(what);
        assert(where);
        assert(type);
        assert(opts);
        assert(source);

        if (streq(type, "autofs"))
                return 0;

        if (!is_path(where)) {
                log_warning("Mount point %s is not a valid path, ignoring.", where);
                return 0;
        }

        if (mount_point_is_api(where) ||
            mount_point_ignore(where))
                return 0;

        name = unit_name_from_path(where, ".mount");
        if (!name)
                return log_oom();

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                if (errno == EEXIST)
                        log_error("Failed to create mount unit file %s, as it already exists. Duplicate entry in /etc/fstab?", unit);
                else
                        log_error("Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fprintf(f,
              "# Automatically generated by systemd-fstab-generator\n\n"
              "[Unit]\n"
              "SourcePath=%s\n",
              source);

        if (post && !noauto && !nofail && !automount)
                fprintf(f,
                        "Before=%s\n",
                        post);

        if (passno > 0) {
                if (streq(where, "/")) {
                        lnk = strjoin(arg_dest, "/", SPECIAL_LOCAL_FS_TARGET, ".wants/", "systemd-fsck-root.service", NULL);
                        if (!lnk)
                                return log_oom();

                        mkdir_parents_label(lnk, 0755);
                        if (symlink("systemd-fsck-root.service", lnk) < 0) {
                                log_error("Failed to create symlink %s: %m", lnk);
                                return -errno;
                        }
                } else {
                        _cleanup_free_ char *fsck = NULL;

                        fsck = unit_name_from_path_instance("systemd-fsck", what, ".service");
                        if (!fsck)
                                return log_oom();

                        fprintf(f,
                                "Requires=%s\n"
                                "After=%s\n",
                                fsck,
                                fsck);
                }
        }


        fprintf(f,
                "\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=%s\n"
                "Type=%s\n",
                what,
                where,
                type);

        if (!isempty(opts) &&
            !streq(opts, "defaults"))
                fprintf(f,
                        "Options=%s\n",
                        opts);

        fflush(f);
        if (ferror(f)) {
                log_error("Failed to write unit file %s: %m", unit);
                return -errno;
        }

        if (!noauto) {
                if (post) {
                        free(lnk);
                        lnk = strjoin(arg_dest, "/", post, nofail || automount ? ".wants/" : ".requires/", name, NULL);
                        if (!lnk)
                                return log_oom();

                        mkdir_parents_label(lnk, 0755);
                        if (symlink(unit, lnk) < 0) {
                                log_error("Failed to create symlink %s: %m", lnk);
                                return -errno;
                        }
                }
        }

        if (automount && !path_equal(where, "/")) {
                automount_name = unit_name_from_path(where, ".automount");
                if (!automount_name)
                        return log_oom();

                automount_unit = strjoin(arg_dest, "/", automount_name, NULL);
                if (!automount_unit)
                        return log_oom();

                fclose(f);
                f = fopen(automount_unit, "wxe");
                if (!f) {
                        log_error("Failed to create unit file %s: %m", automount_unit);
                        return -errno;
                }

                fprintf(f,
                        "# Automatically generated by systemd-fstab-generator\n\n"
                        "[Unit]\n"
                        "SourcePath=%s\n",
                        source);

                if (post)
                        fprintf(f,
                                "Before= %s\n",
                                post);

                fprintf(f,
                        "[Automount]\n"
                        "Where=%s\n",
                        where);

                fflush(f);
                if (ferror(f)) {
                        log_error("Failed to write unit file %s: %m", automount_unit);
                        return -errno;
                }

                free(lnk);
                lnk = strjoin(arg_dest, "/", post, nofail ? ".wants/" : ".requires/", automount_name, NULL);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(automount_unit, lnk) < 0) {
                        log_error("Failed to create symlink %s: %m", lnk);
                        return -errno;
                }
        }

        return 0;
}

static int parse_fstab(const char *prefix, bool initrd) {
        char *fstab_path;
        _cleanup_endmntent_ FILE *f;
        int r = 0;
        struct mntent *me;

        fstab_path = strappenda(strempty(prefix), "/etc/fstab");
        f = setmntent(fstab_path, "r");
        if (!f) {
                if (errno == ENOENT)
                        return 0;

                log_error("Failed to open %s/etc/fstab: %m", strempty(prefix));
                return -errno;
        }

        while ((me = getmntent(f))) {
                _cleanup_free_ char *where = NULL, *what = NULL;
                int k;

                if (initrd && !mount_in_initrd(me))
                        continue;

                what = fstab_node_to_udev_node(me->mnt_fsname);
                where = strjoin(strempty(prefix), me->mnt_dir, NULL);
                if (!what || !where)
                        return log_oom();

                if (is_path(where))
                        path_kill_slashes(where);

                log_debug("Found entry what=%s where=%s type=%s", what, where, me->mnt_type);

                if (streq(me->mnt_type, "swap"))
                        k = add_swap(what, me);
                else {
                        bool noauto, nofail, automount;
                        const char *post;

                        noauto = !!hasmntopt(me, "noauto");
                        nofail = !!hasmntopt(me, "nofail");
                        automount =
                                  hasmntopt(me, "comment=systemd.automount") ||
                                  hasmntopt(me, "x-systemd.automount");

                        if (initrd) {
                                post = SPECIAL_INITRD_FS_TARGET;
                        } else if (mount_in_initrd(me)) {
                                post = SPECIAL_INITRD_ROOT_FS_TARGET;
                        } else if (mount_is_network(me)) {
                                post = SPECIAL_REMOTE_FS_TARGET;
                        } else {
                                post = SPECIAL_LOCAL_FS_TARGET;
                        }

                        k = add_mount(what, where, me->mnt_type, me->mnt_opts,
                                      me->mnt_passno, noauto, nofail, automount,
                                      post, fstab_path);
                }

                if (k < 0)
                        r = k;
        }

        return r;
}

static int parse_new_root_from_proc_cmdline(void) {
        _cleanup_free_ char *what = NULL, *type = NULL, *opts = NULL, *line = NULL;
        bool noauto, nofail;
        char *w, *state;
        size_t l;
        int r;

        r = proc_cmdline(&line);
        if (r < 0)
                log_warning("Failed to read /proc/cmdline, ignoring: %s", strerror(-r));
        if (r <= 0)
                return 0;

        opts = strdup("ro");
        type = strdup("auto");
        if (!opts || !type)
                return log_oom();

        /* root= and roofstype= may occur more than once, the last instance should take precedence.
         * In the case of multiple rootflags= the arguments should be concatenated */
        FOREACH_WORD_QUOTED(w, l, line, state) {
                _cleanup_free_ char *word;

                word = strndup(w, l);
                if (!word)
                        return log_oom();

                else if (startswith(word, "root=")) {
                        free(what);
                        what = fstab_node_to_udev_node(word+5);
                        if (!what)
                                return log_oom();

                } else if (startswith(word, "rootfstype=")) {
                        free(type);
                        type = strdup(word + 11);
                        if (!type)
                                return log_oom();

                } else if (startswith(word, "rootflags=")) {
                        char *o;

                        o = strjoin(opts, ",", word + 10, NULL);
                        if (!o)
                                return log_oom();

                        free(opts);
                        opts = o;

                } else if (streq(word, "ro") || streq(word, "rw")) {
                        char *o;

                        o = strjoin(opts, ",", word, NULL);
                        if (!o)
                                return log_oom();

                        free(opts);
                        opts = o;
                }
        }

        noauto = !!strstr(opts, "noauto");
        nofail = !!strstr(opts, "nofail");

        if (!what) {
                log_debug("Could not find a root= entry on the kernel commandline.");
                return 0;
        }

        if (what[0] != '/') {
                log_debug("Skipping entry what=%s where=/sysroot type=%s", what, type);
                return 0;
        }

        log_debug("Found entry what=%s where=/sysroot type=%s", what, type);
        r = add_mount(what, "/sysroot", type, opts, 1, noauto, nofail, false,
                      SPECIAL_INITRD_ROOT_FS_TARGET, "/proc/cmdline");

        return (r < 0) ? r : 0;
}

static int parse_proc_cmdline(void) {
        _cleanup_free_ char *line = NULL;
        char *w, *state;
        size_t l;
        int r;

        r = proc_cmdline(&line);
        if (r < 0)
                log_warning("Failed to read /proc/cmdline, ignoring: %s", strerror(-r));
        if (r <= 0)
                return 0;

        FOREACH_WORD_QUOTED(w, l, line, state) {
                _cleanup_free_ char *word = NULL;

                word = strndup(w, l);
                if (!word)
                        return log_oom();

                if (startswith(word, "fstab=")) {
                        r = parse_boolean(word + 6);
                        if (r < 0)
                                log_warning("Failed to parse fstab switch %s. Ignoring.", word + 6);
                        else
                                arg_enabled = r;

                } else if (startswith(word, "rd.fstab=")) {

                        if (in_initrd()) {
                                r = parse_boolean(word + 9);
                                if (r < 0)
                                        log_warning("Failed to parse fstab switch %s. Ignoring.", word + 9);
                                else
                                        arg_enabled = r;
                        }

                } else if (startswith(word, "fstab.") ||
                           (in_initrd() && startswith(word, "rd.fstab."))) {

                        log_warning("Unknown kernel switch %s. Ignoring.", word);
                }
        }

        return 0;
}

int main(int argc, char *argv[]) {
        int r = 0, k, l = 0;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[1];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        if (parse_proc_cmdline() < 0)
                return EXIT_FAILURE;

        if (in_initrd())
                r = parse_new_root_from_proc_cmdline();

        if (!arg_enabled)
                return (r < 0) ? EXIT_FAILURE : EXIT_SUCCESS;

        k = parse_fstab(NULL, false);

        if (in_initrd())
                l = parse_fstab("/sysroot", true);

        return (r < 0) || (k < 0) || (l < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
