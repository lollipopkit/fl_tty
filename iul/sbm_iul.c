#include "sbm_iul.h"

#if !SBM_IUL_ENABLED

bool sbm_iul_available(void) { return false; }

int sbm_iul_boot(const char *rootfs, const char *dev_store, const char *profile, const char *hostname) {
    (void)rootfs; (void)dev_store; (void)profile; (void)hostname;
    return -1;
}
int sbm_iul_attach(const char *profile) { (void)profile; return -1; }
int sbm_iul_detach(const char *profile) { (void)profile; return -1; }
int sbm_iul_sessions(const char *profile) { (void)profile; return 0; }
int sbm_iul_open(const char *profile, const char *shell, const char *command,
                 const char *environment, int columns, int rows) {
    (void)profile; (void)shell; (void)command; (void)environment;
    (void)columns; (void)rows;
    return -1;
}
int sbm_iul_read(int session, char *buffer, int length, int timeout_ms) {
    (void)session; (void)buffer; (void)length; (void)timeout_ms;
    return -1;
}
int sbm_iul_write(int session, const char *buffer, int length) {
    (void)session; (void)buffer; (void)length;
    return -1;
}
int sbm_iul_try_write(int session, const char *buffer, int length) {
    (void)session; (void)buffer; (void)length;
    return -1;
}
void sbm_iul_resize(int session, int columns, int rows) {
    (void)session; (void)columns; (void)rows;
}
int sbm_iul_exit_code(int session) { (void)session; return -1; }
void sbm_iul_close(int session) { (void)session; }

#else

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

#include "iul.h"

/// What `die()` does when the host app must not abort with the guest.
static void park_on_die(const char *message) {
    syslog(LOG_ERR, "sbm_iul: die(%s)", message == NULL ? "" : message);
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    select(0, NULL, NULL, NULL, NULL);
}

// — Sessions ————————————————————————————————————————————————————————

#define SBM_MAX_SESSIONS 8
#define SBM_OUTPUT_CAPACITY (256 * 1024)
#define SBM_SESSION_SLOT_MASK 0xff
#define SBM_SESSION_GEN_MAX   0x7fffffu

struct session {
    bool used;
    bool opening;
    bool closing;
    uint32_t gen;
    struct tty *tty;
    pid_t_ pid;
    char profile[64];
    int exit_code;
    int tty_users;
    char data[SBM_OUTPUT_CAPACITY];
    size_t head, tail;
};

struct profile_sid {
    char profile[64];
    pid_t_ sid;
};

#define SBM_MAX_PROFILES 8
static struct session sessions[SBM_MAX_SESSIONS];
static struct profile_sid *profile_sids;
static size_t profile_sids_len;
static size_t profile_sids_cap;
static char detaching[SBM_MAX_PROFILES][64]; // Guarded by sessions_lock.
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sessions_wrote = PTHREAD_COND_INITIALIZER;
static pthread_cond_t sessions_tty_idle = PTHREAD_COND_INITIALIZER;

static pid_t_ close_slot(int index, uint32_t expected_gen, const char *profile);

static int make_session_id(int index, uint32_t gen) {
    return (int)((gen << 8) | (uint32_t)index);
}

static bool split_session_id(int session_id, int *index_out, uint32_t *gen_out) {
    if (session_id < 0) return false;
    int index = session_id & SBM_SESSION_SLOT_MASK;
    uint32_t gen = (uint32_t)session_id >> 8;
    if (index >= SBM_MAX_SESSIONS || gen == 0) return false;
    *index_out = index;
    *gen_out = gen;
    return true;
}

static struct session *lookup_session_locked(int session_id) {
    int index;
    uint32_t gen;
    if (!split_session_id(session_id, &index, &gen)) return NULL;
    struct session *session = &sessions[index];
    if (!session->used || session->opening || session->gen != gen) return NULL;
    return session;
}

static int reserve_profile_sids_locked(size_t extra) {
    if (extra > SIZE_MAX - profile_sids_len) return -ENOMEM;
    size_t need = profile_sids_len + extra;
    if (need <= profile_sids_cap) return 0;
    size_t next_cap = profile_sids_cap == 0 ? 16 : profile_sids_cap;
    while (next_cap < need) {
        if (next_cap > SIZE_MAX / 2) {
            next_cap = need;
            break;
        }
        next_cap *= 2;
    }
    struct profile_sid *grown = realloc(profile_sids, next_cap * sizeof(*grown));
    if (grown == NULL) return -ENOMEM;
    profile_sids = grown;
    profile_sids_cap = next_cap;
    return 0;
}

static int record_profile_sid_locked(const char *profile, pid_t_ sid) {
    if (sid <= 0) return 0;
    for (size_t i = 0; i < profile_sids_len; i++) {
        if (profile_sids[i].sid == sid &&
            strcmp(profile_sids[i].profile, profile) == 0) {
            return 0;
        }
    }
    int err = reserve_profile_sids_locked(1);
    if (err < 0) return err;
    snprintf(profile_sids[profile_sids_len].profile,
             sizeof(profile_sids[profile_sids_len].profile), "%s", profile);
    profile_sids[profile_sids_len].sid = sid;
    profile_sids_len++;
    return 0;
}

static void forget_profile_sids_locked(const char *profile) {
    size_t keep = 0;
    for (size_t i = 0; i < profile_sids_len; i++) {
        if (strcmp(profile_sids[i].profile, profile) == 0) continue;
        if (keep != i) profile_sids[keep] = profile_sids[i];
        keep++;
    }
    profile_sids_len = keep;
}

static int session_of(struct tty *tty) {
    for (int i = 0; i < SBM_MAX_SESSIONS; i++)
        if (sessions[i].used && sessions[i].tty == tty) return i;
    return -1;
}

// — What a session prints ————————————————————————————————————————————

static int console_init(struct tty *tty) { (void)tty; return 0; }
static int console_open(struct tty *tty) { (void)tty; return 0; }
static int console_close(struct tty *tty) { (void)tty; return 0; }
static void console_cleanup(struct tty *tty) { (void)tty; }

static int console_write(struct tty *tty, const void *buffer, size_t length, bool blocking) {
    (void)blocking;
    pthread_mutex_lock(&sessions_lock);
    int index = session_of(tty);
    if (index < 0) {
        pthread_mutex_unlock(&sessions_lock);
        return (int)length;
    }
    struct session *session = &sessions[index];
    const char *bytes = buffer;
    for (size_t i = 0; i < length; i++) {
        size_t next = (session->head + 1) % SBM_OUTPUT_CAPACITY;
        if (next == session->tail)
            session->tail = (session->tail + 1) % SBM_OUTPUT_CAPACITY;
        session->data[session->head] = bytes[i];
        session->head = next;
    }
    pthread_cond_broadcast(&sessions_wrote);
    pthread_mutex_unlock(&sessions_lock);
    return (int)length;
}

static const struct tty_driver_ops console_ops = {
    .init = console_init,
    .open = console_open,
    .close = console_close,
    .write = console_write,
    .cleanup = console_cleanup,
};

static struct tty_driver sbm_pty_driver = {
    .ops = &console_ops,
    .major = TTY_PSEUDO_SLAVE_MAJOR,
    .limit = SBM_MAX_SESSIONS,
};

static int attach_stdio(struct tty *tty) {
    char slave[64];
    snprintf(slave, sizeof(slave), "/dev/pts/%d", tty_number(tty));

    struct fd *fd = generic_open(slave, O_RDWR_, 0);
    if (!IS_ERR(fd) && !S_ISCHR(fd_type(fd))) {
        fd_close(fd);
        fd = ERR_PTR(_ENOTTY);
    }
    if (IS_ERR(fd)) {
        syslog(LOG_ERR, "sbm_iul: %s did not open (%d); stdio is adhoc, so "
               "isatty and /dev/stdout will not work in this session",
               slave, (int)PTR_ERR(fd));
        return create_stdio(slave, TTY_PSEUDO_SLAVE_MAJOR, tty_number(tty));
    }

    struct fd *displaced[3] = {0};
    int err = fdtable_replace_range(current_files(), fd, 0, 3, 0,
            rlimit(RLIMIT_NOFILE_), (void **)displaced);
    if (err < 0) {
        fd_close(fd);
        return err;
    }
    for (size_t i = 0; i < 3; i++) {
        if (displaced[i] != NULL) fd_close(displaced[i]);
    }
    return 0;
}

// — Booting ————————————————————————————————————————————————————————

static _Atomic bool booted = false;
static bool boot_poisoned = false;
static pthread_mutex_t boot_lock = PTHREAD_MUTEX_INITIALIZER;
static char *rootfs_path;
static char *dev_store_path;

static void guest_exited(int pid, int code) {
    int status = (code & 0x7f) ? 128 + (code & 0x7f) : code >> 8;
    pthread_mutex_lock(&sessions_lock);
    for (int i = 0; i < SBM_MAX_SESSIONS; i++) {
        if (sessions[i].used && sessions[i].pid == pid) {
            sessions[i].exit_code = status;
            break;
        }
    }
    pthread_cond_broadcast(&sessions_wrote);
    pthread_mutex_unlock(&sessions_lock);
}

static int make_dev_db(const char *profile, char *data_out, size_t data_len) {
    char dir[MAX_PATH];
    mkdir(dev_store_path, 0755);
    int n = snprintf(dir, sizeof(dir), "%s/%s", dev_store_path, profile);
    if (n < 0 || (size_t)n >= sizeof(dir)) return -ENAMETOOLONG;
    mkdir(dir, 0755);
    n = snprintf(data_out, data_len, "%s/data", dir);
    if (n < 0 || (size_t)n >= data_len) return -ENAMETOOLONG;
    mkdir(data_out, 0755);

    char db_path[MAX_PATH];
    n = snprintf(db_path, sizeof(db_path), "%s/meta.db", dir);
    if (n < 0 || (size_t)n >= sizeof(db_path)) return -ENAMETOOLONG;
    if (access(db_path, F_OK) == 0) return 0;

    return fake_db_create(dir);
}

static int make_dev(const char *profile) {
    char path[MAX_PATH];
#define GUEST(...) (snprintf(path, sizeof(path), __VA_ARGS__), path)

    generic_mkdirat(AT_PWD, GUEST("/%s/dev", profile), 0755);

    char data_path[MAX_PATH];
    int err = make_dev_db(profile, data_path, sizeof(data_path));
    if (err < 0) return err;
    err = do_mount(&fakefs, data_path, GUEST("/%s/dev", profile), "", 0);
    if (err < 0) {
        syslog(LOG_ERR, "sbm_iul: %s has no /dev (%d)", profile, err);
        return err;
    }

    generic_mknodat(AT_PWD, GUEST("/%s/dev/null", profile), S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_NULL_MINOR));
    generic_mknodat(AT_PWD, GUEST("/%s/dev/zero", profile), S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_ZERO_MINOR));
    generic_mknodat(AT_PWD, GUEST("/%s/dev/full", profile), S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_FULL_MINOR));
    generic_mknodat(AT_PWD, GUEST("/%s/dev/random", profile), S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_RANDOM_MINOR));
    generic_mknodat(AT_PWD, GUEST("/%s/dev/urandom", profile), S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_URANDOM_MINOR));
    generic_mknodat(AT_PWD, GUEST("/%s/dev/tty", profile), S_IFCHR | 0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR));
    generic_mknodat(AT_PWD, GUEST("/%s/dev/console", profile), S_IFCHR | 0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR));
    generic_mknodat(AT_PWD, GUEST("/%s/dev/ptmx", profile), S_IFCHR | 0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR));

    generic_mkdirat(AT_PWD, GUEST("/%s/dev/pts", profile), 0755);
    err = do_mount(&devptsfs, "devpts", GUEST("/%s/dev/pts", profile), "", 0);
    if (err < 0) {
        syslog(LOG_ERR, "sbm_iul: %s has no /dev/pts (%d)", profile, err);
        return err;
    }

    generic_symlinkat("/proc/self/fd", AT_PWD, GUEST("/%s/dev/fd", profile));
    generic_symlinkat("/proc/self/fd/0", AT_PWD, GUEST("/%s/dev/stdin", profile));
    generic_symlinkat("/proc/self/fd/1", AT_PWD, GUEST("/%s/dev/stdout", profile));
    generic_symlinkat("/proc/self/fd/2", AT_PWD, GUEST("/%s/dev/stderr", profile));
    generic_mkdirat(AT_PWD, GUEST("/%s/dev/shm", profile), 01777);
#undef GUEST
    return 0;
}

static char attached[SBM_MAX_PROFILES][64];
static pthread_mutex_t attached_lock = PTHREAD_MUTEX_INITIALIZER;

static int check_profile(const char *profile) {
    if (profile == NULL || profile[0] == '\0') return -EINVAL;
    if (strchr(profile, '/') != NULL) return -EINVAL;
    if (strcmp(profile, ".") == 0 || strcmp(profile, "..") == 0) return -EINVAL;
    if (strlen(profile) >= sizeof(attached[0])) return -ENAMETOOLONG;
    return 0;
}

static bool is_attached(const char *profile) {
    for (int i = 0; i < SBM_MAX_PROFILES; i++)
        if (strcmp(attached[i], profile) == 0) return true;
    return false;
}

static bool is_detaching_locked(const char *profile) {
    for (int i = 0; i < SBM_MAX_PROFILES; i++)
        if (strcmp(detaching[i], profile) == 0) return true;
    return false;
}

static int mark_detaching_locked(const char *profile) {
    if (is_detaching_locked(profile)) return -EBUSY;
    for (int i = 0; i < SBM_MAX_PROFILES; i++) {
        if (detaching[i][0] != '\0') continue;
        snprintf(detaching[i], sizeof(detaching[i]), "%s", profile);
        return 0;
    }
    return -EMFILE;
}

static void clear_detaching_locked(const char *profile) {
    for (int i = 0; i < SBM_MAX_PROFILES; i++) {
        if (strcmp(detaching[i], profile) != 0) continue;
        detaching[i][0] = '\0';
        return;
    }
}

static const char *const mount_points[] = { "dev/pts", "dev", "proc" };

static int unmount_profile(const char *profile) {
    char path[MAX_PATH];
    int err = 0;
    for (size_t i = 0; i < sizeof(mount_points) / sizeof(mount_points[0]); i++) {
        snprintf(path, sizeof(path), "/%s/%s", profile, mount_points[i]);
        int one = do_umount(path);
        if (one < 0 && one != _EINVAL) {
            syslog(LOG_ERR, "sbm_iul: %s will not unmount (%d)", path, one);
            if (err == 0) err = one;
        }
    }
    return err;
}

static int enter_profile(const char *profile) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "/%s", profile);
    struct fd *dir = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(dir)) return (int)PTR_ERR(dir);
    fs_chroot(current_fs(), dir);

    struct fd *pwd = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(pwd)) return (int)PTR_ERR(pwd);
    fs_chdir(current_fs(), pwd);
    return 0;
}

bool sbm_iul_available(void) { return true; }

static int attach_profile(const char *profile) {
    pthread_mutex_lock(&sessions_lock);
    if (is_detaching_locked(profile)) {
        pthread_mutex_unlock(&sessions_lock);
        return -EBUSY;
    }
    pthread_mutex_lock(&attached_lock);
    pthread_mutex_unlock(&sessions_lock);
    if (is_attached(profile)) {
        pthread_mutex_unlock(&attached_lock);
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < SBM_MAX_PROFILES; i++)
        if (attached[i][0] == '\0') { slot = i; break; }
    if (slot < 0) {
        pthread_mutex_unlock(&attached_lock);
        return -EMFILE;
    }

    int stale = unmount_profile(profile);
    if (stale < 0) {
        pthread_mutex_unlock(&attached_lock);
        syslog(LOG_ERR, "sbm_iul: %s stale mounts could not be cleared (%d)",
               profile, stale);
        return stale;
    }

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "/%s/proc", profile);
    generic_mkdirat(AT_PWD, path, 0755);
    int err = do_mount(&procfs, "proc", path, "", 0);
    if (err < 0) {
        syslog(LOG_ERR, "sbm_iul: %s has no /proc (%d)", profile, err);
        unmount_profile(profile);
        pthread_mutex_unlock(&attached_lock);
        return err;
    }
    err = make_dev(profile);
    if (err < 0) {
        int undone = unmount_profile(profile);
        if (undone < 0) {
            syslog(LOG_ERR, "sbm_iul: %s did not roll back (%d); its mounts stay "
                   "until an attach can take them down", profile, undone);
        }
        pthread_mutex_unlock(&attached_lock);
        return err;
    }

    snprintf(attached[slot], sizeof(attached[slot]), "%s", profile);
    pthread_mutex_unlock(&attached_lock);
    return 0;
}

int sbm_iul_attach(const char *profile) {
    if (!atomic_load_explicit(&booted, memory_order_acquire)) return -ENOTCONN;
    int checked = check_profile(profile);
    if (checked < 0) return checked;
    return attach_profile(profile);
}

int sbm_iul_sessions(const char *profile) {
    if (!atomic_load_explicit(&booted, memory_order_acquire)) return 0;
    if (check_profile(profile) < 0) return 0;
    int count = 0;
    pthread_mutex_lock(&sessions_lock);
    for (int i = 0; i < SBM_MAX_SESSIONS; i++) {
        if (sessions[i].used && strcmp(sessions[i].profile, profile) == 0)
            count++;
    }
    pthread_mutex_unlock(&sessions_lock);
    return count;
}

static int detach_profile(const char *profile) {
    pid_t_ stack_pids[SBM_MAX_SESSIONS];
    pid_t_ *kill_pids = stack_pids;
    size_t kill_cap = SBM_MAX_SESSIONS;
    size_t kill_count = 0;

    pthread_mutex_lock(&sessions_lock);
    for (int i = 0; i < SBM_MAX_SESSIONS; i++) {
        if (sessions[i].used && (sessions[i].opening || sessions[i].pid == 0) &&
            strcmp(sessions[i].profile, profile) == 0) {
            pthread_mutex_unlock(&sessions_lock);
            return -EBUSY;
        }
    }
    size_t need = profile_sids_len + SBM_MAX_SESSIONS;
    if (need > kill_cap) {
        pid_t_ *heap_pids = malloc(need * sizeof(*heap_pids));
        if (heap_pids == NULL) {
            pthread_mutex_unlock(&sessions_lock);
            return -ENOMEM;
        }
        kill_pids = heap_pids;
        kill_cap = need;
    }
    int marking = mark_detaching_locked(profile);
    if (marking < 0) {
        pthread_mutex_unlock(&sessions_lock);
        if (kill_pids != stack_pids) free(kill_pids);
        return marking;
    }
    for (size_t i = 0; i < profile_sids_len && kill_count < kill_cap; i++) {
        if (strcmp(profile_sids[i].profile, profile) == 0) {
            kill_pids[kill_count++] = profile_sids[i].sid;
        }
    }
    for (int i = 0; i < SBM_MAX_SESSIONS && kill_count < kill_cap; i++) {
        pid_t_ sid = sessions[i].pid;
        if (!sessions[i].used || sid <= 0 || strcmp(sessions[i].profile, profile) != 0) continue;
        bool seen = false;
        for (size_t j = 0; j < kill_count; j++) {
            if (kill_pids[j] == sid) { seen = true; break; }
        }
        if (!seen) kill_pids[kill_count++] = sid;
    }
    pthread_mutex_unlock(&sessions_lock);

    for (int i = 0; i < SBM_MAX_SESSIONS; i++) close_slot(i, 0, profile);

    // Every session this profile had, closed now or earlier: a session whose
    // terminal closed before may still have members that ignored the hangup,
    // and they hold `/dev` as surely as an open one.
    for (size_t i = 0; i < kill_count; i++) {
        iul_kill_session(kill_pids[i], SIGKILL_);
        iul_leave_terminal(kill_pids[i]);
    }

    int err = 0;
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
    for (int attempt = 0; attempt < 100; attempt++) {
        pthread_mutex_lock(&attached_lock);
        err = unmount_profile(profile);
        if (err == 0) {
            for (int i = 0; i < SBM_MAX_PROFILES; i++) {
                if (strcmp(attached[i], profile) == 0) { attached[i][0] = '\0'; break; }
            }
            pthread_mutex_unlock(&attached_lock);
            break;
        }
        pthread_mutex_unlock(&attached_lock);
        if (err != _EBUSY) break;
        for (size_t i = 0; i < kill_count; i++) {
            iul_kill_session(kill_pids[i], SIGKILL_);
        }
        nanosleep(&pause, NULL);
    }

    if (err < 0) {
        syslog(LOG_ERR, "sbm_iul: %s did not detach (%d); it stays attached, "
               "because what would not unmount is still in use", profile, err);
        pthread_mutex_lock(&sessions_lock);
        for (int i = 0; i < SBM_MAX_SESSIONS; i++) {
            if (sessions[i].pid == 0 || strcmp(sessions[i].profile, profile) != 0) continue;
            syslog(LOG_ERR, "sbm_iul:   session %d pid %d used=%d",
                   i, sessions[i].pid, sessions[i].used);
        }
    } else {
        pthread_mutex_lock(&sessions_lock);
        forget_profile_sids_locked(profile);
    }
    clear_detaching_locked(profile);
    pthread_mutex_unlock(&sessions_lock);
    if (kill_pids != stack_pids) free(kill_pids);
    return err;
}

int sbm_iul_detach(const char *profile) {
    if (!atomic_load_explicit(&booted, memory_order_acquire)) return 0;
    int checked = check_profile(profile);
    if (checked < 0) return checked;
    return detach_profile(profile);
}

int sbm_iul_boot(const char *rootfs, const char *dev_store, const char *profile, const char *hostname) {
    if (rootfs == NULL || dev_store == NULL) return -EINVAL;
    int checked = check_profile(profile);
    if (checked < 0) return checked;

    pthread_mutex_lock(&boot_lock);
    if (atomic_load_explicit(&booted, memory_order_acquire)) {
        pthread_mutex_unlock(&boot_lock);
        return -EEXIST;
    }
    if (boot_poisoned) {
        pthread_mutex_unlock(&boot_lock);
        return -EIO;
    }

    char *rootfs_dup = strdup(rootfs);
    char *dev_store_dup = strdup(dev_store);
    if (rootfs_dup == NULL || dev_store_dup == NULL) {
        free(rootfs_dup);
        free(dev_store_dup);
        pthread_mutex_unlock(&boot_lock);
        return -ENOMEM;
    }

    iul_set_die_handler(park_on_die);
    iul_set_hostname(hostname);
    set_exit_hook(guest_exited);

    rootfs_path = rootfs_dup;
    dev_store_path = dev_store_dup;
    int err = mount_root(&realfs, rootfs_path);
    if (err < 0) {
        free(rootfs_path);
        rootfs_path = NULL;
        free(dev_store_path);
        dev_store_path = NULL;
        pthread_mutex_unlock(&boot_lock);
        return err;
    }

    err = become_first_process();
    if (err < 0) {
        do_umount("");
        free(rootfs_path);
        rootfs_path = NULL;
        free(dev_store_path);
        dev_store_path = NULL;
        pthread_mutex_unlock(&boot_lock);
        return err;
    }

    // pid 1 now exists; any failure from this point leaves process-wide init
    // state partially initialized, so later boot attempts must fail cleanly.
    struct fd *root_fd = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(root_fd)) {
        boot_poisoned = true;
        pthread_mutex_unlock(&boot_lock);
        return (int)PTR_ERR(root_fd);
    }
    fs_chdir(current_fs(), root_fd);

    // Give init adhoc /dev/null stdio before attaching/entering profile so
    // child tools do not open /<profile>/dev/null and pin /dev forever.
    err = create_stdio("", MEM_MAJOR, DEV_NULL_MINOR);
    if (err < 0) {
        boot_poisoned = true;
        pthread_mutex_unlock(&boot_lock);
        return err;
    }

    err = attach_profile(profile);
    if (err < 0) {
        boot_poisoned = true;
        pthread_mutex_unlock(&boot_lock);
        return err;
    }

    struct fd *outer_root = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(outer_root)) {
        detach_profile(profile);
        boot_poisoned = true;
        pthread_mutex_unlock(&boot_lock);
        return (int)PTR_ERR(outer_root);
    }
    struct fd *outer_pwd = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(outer_pwd)) {
        fd_close(outer_root);
        detach_profile(profile);
        boot_poisoned = true;
        pthread_mutex_unlock(&boot_lock);
        return (int)PTR_ERR(outer_pwd);
    }

    err = enter_profile(profile);
    if (err < 0) {
        fs_chroot(current_fs(), outer_root);
        fs_chdir(current_fs(), outer_pwd);
        detach_profile(profile);
        boot_poisoned = true;
        pthread_mutex_unlock(&boot_lock);
        return err;
    }

    char argv[512];
    const char *parts[] = {
        "/bin/sh",
        "-c",
        "while :; do sleep 2147483647; done",
    };
    size_t position = 0;
    for (size_t i = 0; i < 3; i++) {
        size_t length = strlen(parts[i]) + 1;
        memcpy(argv + position, parts[i], length);
        position += length;
    }
    argv[position] = '\0';

    char environment[256] = {0};
    size_t written = 0;
    written += snprintf(environment + written, sizeof(environment) - written, "HOME=/root") + 1;
    written += snprintf(environment + written, sizeof(environment) - written,
                        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin") + 1;

    err = do_execve("/bin/sh", 3, argv, environment);
    if (err < 0) {
        fs_chroot(current_fs(), outer_root);
        fs_chdir(current_fs(), outer_pwd);
        detach_profile(profile);
        boot_poisoned = true;
        pthread_mutex_unlock(&boot_lock);
        return err;
    }

    fd_close(outer_root);
    fd_close(outer_pwd);
    task_start(current);
    atomic_store_explicit(&booted, true, memory_order_release);
    pthread_mutex_unlock(&boot_lock);
    return 0;
}

int sbm_iul_open(const char *profile, const char *shell, const char *command,
                 const char *environment, int columns, int rows) {
    if (!atomic_load_explicit(&booted, memory_order_acquire)) return -ENOTCONN;
    int checked = check_profile(profile);
    if (checked < 0) return checked;

    bool interactive = command == NULL || command[0] == '\0';
    if (shell == NULL || shell[0] == '\0') shell = "/bin/sh";

    pthread_mutex_lock(&sessions_lock);
    if (is_detaching_locked(profile)) {
        pthread_mutex_unlock(&sessions_lock);
        return -EBUSY;
    }
    int index = -1;
    for (int i = 0; i < SBM_MAX_SESSIONS; i++) {
        if (!sessions[i].used && !sessions[i].closing && sessions[i].tty_users == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        pthread_mutex_unlock(&sessions_lock);
        return -EMFILE;
    }
    if (reserve_profile_sids_locked(SBM_MAX_SESSIONS) < 0) {
        pthread_mutex_unlock(&sessions_lock);
        return -ENOMEM;
    }
    struct session *session = &sessions[index];
    uint32_t gen = (session->gen + 1) & SBM_SESSION_GEN_MAX;
    if (gen == 0) gen = 1;
    memset(session, 0, sizeof(*session));
    session->gen = gen;
    session->used = true;
    session->opening = true;
    session->exit_code = -1;
    snprintf(session->profile, sizeof(session->profile), "%s", profile);
    pthread_mutex_unlock(&sessions_lock);

    pid_t_ child_pid = 0;
    struct tty *tty = NULL;
    int err = become_new_init_child();
    if (err < 0) goto fail;
    child_pid = current_pid();

    err = attach_profile(profile);
    if (err < 0) goto fail;
    err = enter_profile(profile);
    if (err < 0) goto fail;

    tty = pty_open_fake(&sbm_pty_driver);
    if (IS_ERR(tty)) {
        err = (int)PTR_ERR(tty);
        tty = NULL;
        goto fail;
    }
    struct winsize_ size = {
        .col = (uint16_t)(columns > 0 ? columns : 80),
        .row = (uint16_t)(rows > 0 ? rows : 25),
    };
    tty_set_winsize(tty, size);

    if (!interactive) {
        struct termios_ termios;
        tty_get_termios(tty, &termios);
        termios.oflags &= ~OPOST_;
        termios.lflags &= ~ECHO_;
        tty_set_termios(tty, &termios);
    }

    pthread_mutex_lock(&sessions_lock);
    if (is_detaching_locked(profile)) {
        pthread_mutex_unlock(&sessions_lock);
        tty_hangup(tty);
        tty_release(tty);
        tty = NULL;
        err = -EBUSY;
        goto fail;
    }
    if (record_profile_sid_locked(profile, child_pid) < 0) {
        pthread_mutex_unlock(&sessions_lock);
        tty_hangup(tty);
        tty_release(tty);
        tty = NULL;
        err = -ENOMEM;
        goto fail;
    }
    struct tty *session_tty = tty;
    session->tty = session_tty;
    session->pid = child_pid;
    tty = NULL;
    pthread_mutex_unlock(&sessions_lock);

    err = attach_stdio(session_tty);
    if (err < 0) goto fail;

    static const char default_environment[] =
        "TERM=xterm-256color\0"
        "LANG=C.UTF-8\0"
        "HOME=/root\0"
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0";
    if (environment == NULL || environment[0] == '\0') {
        environment = default_environment;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        char argv[4096];
        size_t position = 0;
        const char *parts[3] = { shell, "-c", command };
        int count = interactive ? 1 : 3;
        bool too_long = false;
        for (int i = 0; i < count; i++) {
            size_t length = strlen(parts[i]) + 1;
            if (position + length >= sizeof(argv) - 1) { too_long = true; break; }
            memcpy(argv + position, parts[i], length);
            position += length;
        }
        if (too_long) { err = -E2BIG; goto fail; }
        argv[position] = '\0';

        err = do_execve(shell, count, argv, environment);
        if (err >= 0) break;
        if (attempt == 1 || strcmp(shell, "/bin/sh") == 0) goto fail;
        syslog(LOG_ERR, "sbm_iul: %s did not exec (%d); falling back to /bin/sh",
               shell, err);
        shell = "/bin/sh";
    }
    if (err < 0) goto fail;

    pthread_mutex_lock(&sessions_lock);
    if (!session->used || session->gen != gen || is_detaching_locked(profile)) {
        pthread_mutex_unlock(&sessions_lock);
        err = -EBUSY;
        goto fail;
    }
    pthread_mutex_unlock(&sessions_lock);

    task_start(current);

    pthread_mutex_lock(&sessions_lock);
    if (session->used && session->gen == gen) {
        session->opening = false;
        pthread_cond_broadcast(&sessions_wrote);
    }
    pthread_mutex_unlock(&sessions_lock);
    return make_session_id(index, gen);

fail:
    ;
    struct tty *cleanup_tty = NULL;
    pthread_mutex_lock(&sessions_lock);
    bool owns_slot = session->used && session->gen == gen;
    if (owns_slot) {
        cleanup_tty = session->tty;
        session->tty = NULL;
        session->pid = 0;
    }
    if (child_pid > 0) {
        (void)record_profile_sid_locked(profile, child_pid);
    }
    pthread_mutex_unlock(&sessions_lock);
    if (cleanup_tty != NULL) {
        tty_hangup(cleanup_tty);
        tty_release(cleanup_tty);
    } else if (tty != NULL) {
        tty_hangup(tty);
        tty_release(tty);
    }
    if (child_pid > 0) {
        iul_kill_session(child_pid, SIGKILL_);
        iul_leave_terminal(child_pid);
        task_start(current);
    }
    if (owns_slot) {
        pthread_mutex_lock(&sessions_lock);
        if (session->used && session->gen == gen) {
            session->used = false;
            session->opening = false;
            pthread_cond_broadcast(&sessions_wrote);
        }
        pthread_mutex_unlock(&sessions_lock);
    }
    return err;
}

int sbm_iul_read(int session_id, char *buffer, int length, int timeout_ms) {
    if (buffer == NULL || length <= 0) return -EINVAL;
    int index;
    uint32_t gen;
    if (!split_session_id(session_id, &index, &gen)) return -EINVAL;

    if (pthread_mutex_trylock(&sessions_lock) != 0) {
        struct timespec retry = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        int attempts = 0;
        while (pthread_mutex_trylock(&sessions_lock) != 0) {
            if (++attempts > 50) return -EBUSY;
            nanosleep(&retry, NULL);
        }
    }

    struct session *session = &sessions[index];
    if (!session->used || session->opening || session->gen != gen) {
        pthread_mutex_unlock(&sessions_lock);
        return -1;
    }
    if (session->head == session->tail && session->exit_code < 0 && timeout_ms > 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        struct timespec until = {
            .tv_sec = now.tv_sec + timeout_ms / 1000,
            .tv_nsec = now.tv_usec * 1000 + (long)(timeout_ms % 1000) * 1000000,
        };
        if (until.tv_nsec >= 1000000000) { until.tv_sec += 1; until.tv_nsec -= 1000000000; }
        while (session->used && !session->opening && session->gen == gen &&
               session->head == session->tail && session->exit_code < 0) {
            if (pthread_cond_timedwait(&sessions_wrote, &sessions_lock, &until) == ETIMEDOUT) break;
        }
    }
    if (!session->used || session->opening || session->gen != gen) {
        pthread_mutex_unlock(&sessions_lock);
        return -1;
    }

    int count = 0;
    while (count < length && session->tail != session->head) {
        buffer[count++] = session->data[session->tail];
        session->tail = (session->tail + 1) % SBM_OUTPUT_CAPACITY;
    }
    bool ended = count == 0 && session->exit_code >= 0;
    pthread_mutex_unlock(&sessions_lock);
    return ended ? -1 : count;
}

int sbm_iul_write(int session_id, const char *buffer, int length) {
    if (buffer == NULL || length <= 0) return -EINVAL;
    int index;
    uint32_t gen;
    if (!split_session_id(session_id, &index, &gen)) return -EINVAL;
    pthread_mutex_lock(&sessions_lock);
    struct session *session = lookup_session_locked(session_id);
    struct tty *tty = session != NULL ? session->tty : NULL;
    if (tty != NULL) session->tty_users++;
    pthread_mutex_unlock(&sessions_lock);
    if (tty == NULL) return -ENOTTY;

    int result = (int)tty_input(tty, buffer, (size_t)length, true);

    pthread_mutex_lock(&sessions_lock);
    if (--sessions[index].tty_users == 0) pthread_cond_broadcast(&sessions_tty_idle);
    pthread_mutex_unlock(&sessions_lock);
    return result;
}

int sbm_iul_try_write(int session_id, const char *buffer, int length) {
    if (buffer == NULL || length <= 0) return -EINVAL;
    int index;
    uint32_t gen;
    if (!split_session_id(session_id, &index, &gen)) return -EINVAL;
    pthread_mutex_lock(&sessions_lock);
    struct session *session = lookup_session_locked(session_id);
    struct tty *tty = session != NULL ? session->tty : NULL;
    if (tty != NULL) session->tty_users++;
    pthread_mutex_unlock(&sessions_lock);
    if (tty == NULL) return -ENOTTY;

    ssize_t written = tty_input(tty, buffer, (size_t)length, false);

    pthread_mutex_lock(&sessions_lock);
    if (--sessions[index].tty_users == 0) pthread_cond_broadcast(&sessions_tty_idle);
    pthread_mutex_unlock(&sessions_lock);
    if (written == _EAGAIN) return 0;
    return (int)written;
}

void sbm_iul_resize(int session_id, int columns, int rows) {
    int index;
    uint32_t gen;
    if (!split_session_id(session_id, &index, &gen)) return;
    pthread_mutex_lock(&sessions_lock);
    struct session *session = lookup_session_locked(session_id);
    struct tty *tty = session != NULL ? session->tty : NULL;
    if (tty != NULL) session->tty_users++;
    pthread_mutex_unlock(&sessions_lock);
    if (tty == NULL) return;

    struct winsize_ size = {
        .col = (uint16_t)(columns > 0 ? columns : 80),
        .row = (uint16_t)(rows > 0 ? rows : 25),
    };
    tty_set_winsize(tty, size);

    pthread_mutex_lock(&sessions_lock);
    if (--sessions[index].tty_users == 0) pthread_cond_broadcast(&sessions_tty_idle);
    pthread_mutex_unlock(&sessions_lock);
}

int sbm_iul_exit_code(int session_id) {
    pthread_mutex_lock(&sessions_lock);
    struct session *session = lookup_session_locked(session_id);
    int code = session != NULL ? session->exit_code : -1;
    pthread_mutex_unlock(&sessions_lock);
    return code;
}

static pid_t_ close_slot(int index, uint32_t expected_gen, const char *profile) {
    pthread_mutex_lock(&sessions_lock);
    struct session *session = &sessions[index];
    bool mine = session->used && !session->opening &&
                (expected_gen == 0 || session->gen == expected_gen) &&
                (profile == NULL || strcmp(session->profile, profile) == 0);
    pid_t_ pid = mine ? session->pid : 0;
    struct tty *tty = mine ? session->tty : NULL;
    if (mine) {
        session->used = false;
        session->closing = true;
        session->tty = NULL;
        pthread_cond_broadcast(&sessions_wrote);
    }
    pthread_mutex_unlock(&sessions_lock);

    if (!mine) return 0;

    if (tty != NULL) {
        tty_hangup(tty);
    }

    if (pid > 0) {
        iul_kill_session(pid, SIGHUP_);
        iul_leave_terminal(pid);
    }

    pthread_mutex_lock(&sessions_lock);
    while (session->tty_users > 0) {
        pthread_cond_wait(&sessions_tty_idle, &sessions_lock);
    }
    session->closing = false;
    pthread_mutex_unlock(&sessions_lock);

    if (tty != NULL) {
        tty_release(tty);
    }
    return pid;
}

void sbm_iul_close(int session_id) {
    int index;
    uint32_t gen;
    if (!split_session_id(session_id, &index, &gen)) return;
    close_slot(index, gen, NULL);
}

#endif // SBM_IUL_ENABLED
