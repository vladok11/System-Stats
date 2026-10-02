#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <time.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>

#define MAX_CPUS 256
#define MAX_PROCS 1024
#define MAX_DISKS 32
#define MAX_NET_IFS 16
#define MAX_LINES 1500
#define LINE_LEN 256
#define OUT_BUF_SIZE (128 * 1024)
#define UID_CACHE_SZ 64


typedef struct {
    unsigned long long idle;
    unsigned long long total;
} CpuTicks;

typedef struct {
    unsigned long total, free, available, buffers, cached, sreclaimable, buff_cache, pure_used, used;
} RamStats;

typedef struct {
    unsigned long total, free, used;
} SwapStats;

typedef struct {
    char name[32];
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
} NetInterface;

typedef struct {
    char name[32];
    unsigned long long rd_sectors;
    unsigned long long wr_sectors;
    unsigned long long io_ticks;
} DiskStats;

typedef struct {
    int pid;
    char name[64];
    char state;
    int num_threads;
    char user[24];
    double cpu_pct;
    unsigned long rss_kb;
    double io_rate_kbs;
} ProcessInfo;

typedef struct {
    int pid;
    unsigned long long ticks;
    unsigned long long io_bytes;
} ProcSnap;

typedef struct {
    int running, sleeping, zombie, stopped, other;
} ProcStateCounts;

typedef struct {
    char lines[MAX_LINES][LINE_LEN];
    int count;
} RenderBuffer;

typedef struct {
    uid_t uid;
    char name[24];
} UidCacheEntry;


static int g_num_cores = 1;
static long g_page_size_kb = 4;
static int g_detected_cores = 0;

static CpuTicks g_cpu_prev[MAX_CPUS + 1];
static CpuTicks g_cpu_curr[MAX_CPUS + 1];

static DiskStats g_disks_prev[MAX_DISKS];
static DiskStats g_disks_curr[MAX_DISKS];
static int g_disk_count_prev = 0, g_disk_count_curr = 0;

static NetInterface g_net_prev[MAX_NET_IFS];
static NetInterface g_net_curr[MAX_NET_IFS];
static int g_net_count_prev = 0, g_net_count_curr = 0;

static ProcSnap g_snaps[MAX_PROCS];
static int g_snap_count = 0;

static ProcessInfo g_procs[MAX_PROCS];
static int g_proc_count = 0;
static ProcStateCounts g_state_counts;

static RenderBuffer g_buf;
static struct termios orig_termios;
static bool raw_mode_active = false;

static UidCacheEntry g_uid_cache[UID_CACHE_SZ];
static int g_uid_cache_count = 0;


static void bprintf(const char *fmt, ...) {
    if (g_buf.count >= MAX_LINES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_buf.lines[g_buf.count], LINE_LEN, fmt, args);
    va_end(args);
    g_buf.count++;
}

static void buf_print_bar(char *out, size_t out_sz, double percent, int width) {
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    int filled = (int)((percent / 100.0) * width);

    int pos = snprintf(out, out_sz, "[");
    for (int i = 0; i < width && pos < (int)out_sz - 2; i++) {
        out[pos++] = (i < filled) ? '#' : '-';
    }
    out[pos++] = ']';
    out[pos] = '\0';
}


void cleanup_terminal(int sig) {
    (void)sig;
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        const char *restore_str = "\033[?1000l\033[?25h\033[?1049l";
        (void)write(STDOUT_FILENO, restore_str, strlen(restore_str));
        raw_mode_active = false;
    }
    if (sig != 0) _exit(0);
}

void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = true;

    signal(SIGINT, cleanup_terminal);
    signal(SIGTERM, cleanup_terminal);

    const char *init_str = "\033[?1049h\033[?25l\033[?1000h";
    (void)write(STDOUT_FILENO, init_str, strlen(init_str));
}

void get_terminal_size(int *rows, int *cols) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
        *rows = w.ws_row;
        *cols = w.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

void render_frame(int scroll_offset, int rows, int cols) {
    static char frame[OUT_BUF_SIZE];
    int offset = 0;

    offset += snprintf(frame + offset, OUT_BUF_SIZE - offset, "\033[H");

    int visible_rows = rows - 1;
    for (int i = 0; i < visible_rows; i++) {
        int idx = scroll_offset + i;
        if (idx < g_buf.count) {
            offset += snprintf(frame + offset, OUT_BUF_SIZE - offset, 
                               "\033[K%.*s\n", cols, g_buf.lines[idx]);
        } else {
            offset += snprintf(frame + offset, OUT_BUF_SIZE - offset, "\033[K\n");
        }
    }

    int max_scroll = g_buf.count - visible_rows;
    if (max_scroll < 0) max_scroll = 0;

    offset += snprintf(frame + offset, OUT_BUF_SIZE - offset, 
                       "\033[K\033[7m[Lines: %d-%d/%d | Arrows/Mouse Wheel/PgUp/PgDn | 'q' Quit]\033[0m", 
                       scroll_offset + 1, 
                       (scroll_offset + visible_rows > g_buf.count) ? g_buf.count : scroll_offset + visible_rows,
                       g_buf.count);

    (void)write(STDOUT_FILENO, frame, offset);
}


static ssize_t fast_read_file(const char *path, char *buf, size_t max_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, max_len - 1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
    } else {
        buf[0] = '\0';
    }
    return n;
}

static void resolve_proc_user(int pid, char *user_buf, size_t sz) {
    char path[48];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    struct stat st;
    if (stat(path, &st) != 0) {
        strncpy(user_buf, "unknown", sz - 1);
        user_buf[sz - 1] = '\0';
        return;
    }

    for (int i = 0; i < g_uid_cache_count; i++) {
        if (g_uid_cache[i].uid == st.st_uid) {
            strncpy(user_buf, g_uid_cache[i].name, sz - 1);
            user_buf[sz - 1] = '\0';
            return;
        }
    }

    struct passwd *pw = getpwuid(st.st_uid);
    if (pw) {
        strncpy(user_buf, pw->pw_name, sz - 1);
    } else {
        snprintf(user_buf, sz, "%u", st.st_uid);
    }
    user_buf[sz - 1] = '\0';

    if (g_uid_cache_count < UID_CACHE_SZ) {
        g_uid_cache[g_uid_cache_count].uid = st.st_uid;
        strncpy(g_uid_cache[g_uid_cache_count].name, user_buf, sizeof(g_uid_cache[0].name) - 1);
        g_uid_cache_count++;
    }
}


static void read_all_cpu_ticks(CpuTicks *cpu_arr, int *detected_count) {
    int fd = open("/proc/stat", O_RDONLY);
    if (fd < 0) return;

    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char *p = buf;
    int max_core = 0;

    while (*p) {
        if (p[0] == 'c' && p[1] == 'p' && p[2] == 'u') {
            int idx = -1;
            p += 3;
            if (*p == ' ') {
                idx = 0;
            } else if (*p >= '0' && *p <= '9') {
                char *end;
                long core_num = strtol(p, &end, 10);
                p = end;
                if (core_num + 1 <= MAX_CPUS) {
                    idx = (int)core_num + 1;
                    if (idx > max_core) max_core = idx;
                }
            }

            if (idx >= 0) {
                while (*p == ' ') p++;
                unsigned long long fields[10] = {0};
                for (int i = 0; i < 10 && *p && *p != '\n'; i++) {
                    fields[i] = strtoull(p, &p, 10);
                    while (*p == ' ') p++;
                }
                cpu_arr[idx].idle = fields[3] + fields[4];
                unsigned long long total = 0;
                for (int i = 0; i < 8; i++) total += fields[i];
                cpu_arr[idx].total = total;
            }
        }
        char *next = strchr(p, '\n');
        if (!next) break;
        p = next + 1;
    }
    if (detected_count) *detected_count = max_core;
}

static void read_ram_stats(RamStats *ram, SwapStats *swap) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return;

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char *p = buf;
    while (*p) {
        switch (p[0]) {
            case 'M':
                if (strncmp(p, "MemTotal:", 9) == 0) ram->total = strtoul(p + 9, &p, 10);
                else if (strncmp(p, "MemFree:", 8) == 0) ram->free = strtoul(p + 8, &p, 10);
                else if (strncmp(p, "MemAvailable:", 13) == 0) ram->available = strtoul(p + 13, &p, 10);
                break;
            case 'B':
                if (strncmp(p, "Buffers:", 8) == 0) ram->buffers = strtoul(p + 8, &p, 10);
                break;
            case 'C':
                if (strncmp(p, "Cached:", 7) == 0) ram->cached = strtoul(p + 7, &p, 10);
                break;
            case 'S':
                if (strncmp(p, "SReclaimable:", 13) == 0) ram->sreclaimable = strtoul(p + 13, &p, 10);
                else if (strncmp(p, "SwapTotal:", 10) == 0) swap->total = strtoul(p + 10, &p, 10);
                else if (strncmp(p, "SwapFree:", 9) == 0) swap->free = strtoul(p + 9, &p, 10);
                break;
        }

        char *next = strchr(p, '\n');
        if (!next) break;
        p = next + 1;
    }

    ram->buff_cache = ram->buffers + ram->cached + ram->sreclaimable;
    
    unsigned long non_used = ram->free + ram->buff_cache;
    ram->pure_used = (ram->total > non_used) ? (ram->total - non_used) : 0;
    ram->used = ram->pure_used;

    swap->used = (swap->total >= swap->free) ? (swap->total - swap->free) : 0;
}

static int read_network_bytes(NetInterface *arr, int max_count) {
    int fd = open("/proc/net/dev", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    int count = 0;
    char *p = buf;
    for (int i = 0; i < 2 && p; i++) {
        p = strchr(p, '\n');
        if (p) p++;
    }

    while (p && *p && count < max_count) {
        while (*p == ' ') p++;
        char *colon = strchr(p, ':');
        if (!colon) break;

        size_t name_len = colon - p;
        if (name_len >= sizeof(arr[count].name)) name_len = sizeof(arr[count].name) - 1;
        memcpy(arr[count].name, p, name_len);
        arr[count].name[name_len] = '\0';

        p = colon + 1;
        arr[count].rx_bytes = strtoull(p, &p, 10);
        for (int i = 0; i < 7; i++) strtoull(p, &p, 10);
        arr[count].tx_bytes = strtoull(p, &p, 10);

        count++;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return count;
}

static int read_diskstats(DiskStats *arr, int max_count) {
    int fd = open("/proc/diskstats", O_RDONLY);
    if (fd < 0) return 0;
    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    int count = 0;
    char *p = buf;
    while (*p && count < max_count) {
        while (*p == ' ') p++;
        strtoul(p, &p, 10);
        strtoul(p, &p, 10);
        while (*p == ' ') p++;

        char dev_name[32];
        char *end_name = strchr(p, ' ');
        if (!end_name) break;
        size_t len = end_name - p;
        if (len >= sizeof(dev_name)) len = sizeof(dev_name) - 1;
        memcpy(dev_name, p, len);
        dev_name[len] = '\0';
        p = end_name;

        bool is_target = (dev_name[0] == 's' && dev_name[1] == 'd' && ((dev_name[2] >= 'a' && dev_name[2] <= 'z') || (dev_name[2] >= 'A' && dev_name[2] <= 'Z')) && dev_name[3] == '\0') ||
                         (strncmp(dev_name, "nvme", 4) == 0 && strchr(dev_name, 'n') && !strchr(dev_name, 'p'));

        if (is_target) {
            strncpy(arr[count].name, dev_name, sizeof(arr[count].name) - 1);
            strtoull(p, &p, 10);
            strtoull(p, &p, 10);
            arr[count].rd_sectors = strtoull(p, &p, 10);
            strtoull(p, &p, 10);
            strtoull(p, &p, 10);
            strtoull(p, &p, 10);
            arr[count].wr_sectors = strtoull(p, &p, 10);
            strtoull(p, &p, 10);
            strtoull(p, &p, 10);
            arr[count].io_ticks = strtoull(p, &p, 10);
            count++;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return count;
}

static bool read_proc_stat_fast(int pid, ProcessInfo *proc, unsigned long long *ticks) {
    char path[48];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';

    char *p_open = strchr(buf, '(');
    char *p_close = strrchr(buf, ')');
    if (!p_open || !p_close || p_close < p_open) return false;

    if (proc) {
        size_t len = p_close - (p_open + 1);
        if (len >= sizeof(proc->name)) len = sizeof(proc->name) - 1;
        memcpy(proc->name, p_open + 1, len);
        proc->name[len] = '\0';
    }

    char *p = p_close + 2;
    char state = *p;
    if (proc) proc->state = state;

    if (state == 'R') g_state_counts.running++;
    else if (state == 'S' || state == 'D') g_state_counts.sleeping++;
    else if (state == 'Z') g_state_counts.zombie++;
    else if (state == 'T') g_state_counts.stopped++;
    else g_state_counts.other++;

    p += 2;
    for (int i = 0; i < 10; i++) {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }

    unsigned long long utime = strtoull(p, &p, 10);
    while (*p == ' ') p++;
    unsigned long long stime = strtoull(p, &p, 10);
    *ticks = utime + stime;

    if (proc) {
        for (int i = 0; i < 4; i++) {
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
        }
        proc->num_threads = (int)strtol(p, &p, 10);

        char statm_path[48];
        snprintf(statm_path, sizeof(statm_path), "/proc/%d/statm", pid);
        int mfd = open(statm_path, O_RDONLY);
        if (mfd >= 0) {
            char mbuf[64];
            ssize_t mn = read(mfd, mbuf, sizeof(mbuf) - 1);
            close(mfd);
            if (mn > 0) {
                mbuf[mn] = '\0';
                char *mp = mbuf;
                strtoul(mp, &mp, 10); 
                unsigned long rss_pages = strtoul(mp, NULL, 10); 
                proc->rss_kb = rss_pages * g_page_size_kb;
            }
        }
    }
    return true;
}

static unsigned long long read_proc_io_bytes(int pid) {
    char path[48];
    snprintf(path, sizeof(path), "/proc/%d/io", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    unsigned long long rchar = 0, wchar = 0;
    char *p = strstr(buf, "read_bytes:");
    if (p) rchar = strtoull(p + 11, NULL, 10);
    p = strstr(buf, "write_bytes:");
    if (p) wchar = strtoull(p + 12, NULL, 10);

    return rchar + wchar;
}


static int cmp_proc_snap(const void *a, const void *b) {
    return ((const ProcSnap *)a)->pid - ((const ProcSnap *)b)->pid;
}

static int cmp_procs_by_cpu(const void *a, const void *b) {
    const ProcessInfo *p1 = (const ProcessInfo *)a;
    const ProcessInfo *p2 = (const ProcessInfo *)b;
    if (p2->cpu_pct > p1->cpu_pct) return 1;
    if (p2->cpu_pct < p1->cpu_pct) return -1;
    return (p2->rss_kb > p1->rss_kb) ? 1 : -1;
}

static int cmp_procs_by_mem(const void *a, const void *b) {
    const ProcessInfo *p1 = (const ProcessInfo *)a;
    const ProcessInfo *p2 = (const ProcessInfo *)b;
    if (p2->rss_kb > p1->rss_kb) return 1;
    if (p2->rss_kb < p1->rss_kb) return -1;
    return (p2->cpu_pct > p1->cpu_pct) ? 1 : -1;
}

static int cmp_procs_by_io(const void *a, const void *b) {
    const ProcessInfo *p1 = (const ProcessInfo *)a;
    const ProcessInfo *p2 = (const ProcessInfo *)b;
    if (p2->io_rate_kbs > p1->io_rate_kbs) return 1;
    if (p2->io_rate_kbs < p1->io_rate_kbs) return -1;
    return 0;
}


static void print_hardware_info(void) {
    bprintf("=================================================================");
    bprintf("                  SYSTEM & HARDWARE SPECIFICATIONS               ");
    bprintf("=================================================================");

    struct utsname un;
    if (uname(&un) == 0) {
        bprintf("Kernel       : %s %s (%s)", un.sysname, un.release, un.machine);
    }
    char os_name[128] = "Unknown Linux";
    int fd = open("/etc/os-release", O_RDONLY);
    if (fd >= 0) {
        char buf[2048];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *p = strstr(buf, "PRETTY_NAME=");
            if (p) {
                p += 12;
                if (*p == '"') p++;
                char *end = strpbrk(p, "\"\n");
                if (end) *end = '\0';
                strncpy(os_name, p, sizeof(os_name) - 1);
            }
        }
    }
    bprintf("Distribution : %s", os_name);

    char init_name[32] = "unknown";
    fast_read_file("/proc/1/comm", init_name, sizeof(init_name));
    bprintf("Init System  : %s", init_name);

    const char *de = getenv("XDG_CURRENT_DESKTOP");
    if (!de) de = getenv("DESKTOP_SESSION");
    bprintf("Desktop Env  : %s", de ? de : "Headless / CLI");

    char cpu_model[128] = "Unknown CPU";
    fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd >= 0) {
        char buf[2048];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *p = strstr(buf, "model name");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p++;
                    while (*p == ' ') p++;
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = '\0';
                    strncpy(cpu_model, p, sizeof(cpu_model) - 1);
                }
            }
        }
    }
    bprintf("Processor    : %s (%d logical threads)", cpu_model, g_num_cores);

    char max_freq_str[32] = {0};
    if (fast_read_file("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", max_freq_str, sizeof(max_freq_str)) > 0) {
        double ghz = strtod(max_freq_str, NULL) / 1000000.0;
        bprintf("Max CPU Freq : %.2f GHz", ghz);
    }

    DIR *pci_dir = opendir("/sys/bus/pci/devices");
    if (pci_dir) {
        struct dirent *d;
        while ((d = readdir(pci_dir)) != NULL) {
            if (d->d_name[0] == '.') continue;
            char class_path[256];
            snprintf(class_path, sizeof(class_path), "/sys/bus/pci/devices/%s/class", d->d_name);
            char class_buf[16] = {0};
            if (fast_read_file(class_path, class_buf, sizeof(class_buf)) > 0) {
                unsigned long pci_class = strtoul(class_buf, NULL, 16);
                if ((pci_class >> 16) == 0x03) {
                    char vendor_path[256], dev_path[256];
                    char v_buf[16] = {0}, d_buf[16] = {0};
                    snprintf(vendor_path, sizeof(vendor_path), "/sys/bus/pci/devices/%s/vendor", d->d_name);
                    snprintf(dev_path, sizeof(dev_path), "/sys/bus/pci/devices/%s/device", d->d_name);
                    fast_read_file(vendor_path, v_buf, sizeof(v_buf));
                    fast_read_file(dev_path, d_buf, sizeof(d_buf));
                    const char *type = (strstr(d->d_name, "00:02.0")) ? "Integrated (iGPU)" : "Discrete (dGPU)";
                    bprintf("GPU          : PCI %s: %s [%s:%s]", d->d_name, type, v_buf, d_buf);
                }
            }
        }
        closedir(pci_dir);
    }
    bprintf("=================================================================");
}

static void print_power_and_uptime(void) {
    int fd = open("/proc/uptime", O_RDONLY);
    if (fd >= 0) {
        char buf[64];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            double sec = strtod(buf, NULL);
            int d = (int)(sec / 86400);
            int h = (int)((sec - d * 86400) / 3600);
            int m = (int)((sec - d * 86400 - h * 3600) / 60);
            bprintf("Uptime       : %d days, %02d:%02d", d, h, m);
        }
    }

    DIR *dir = opendir("/sys/class/power_supply");
    if (dir) {
        struct dirent *d;
        while ((d = readdir(dir)) != NULL) {
            if (strncmp(d->d_name, "BAT", 3) == 0) {
                char path[256], cap_str[16] = {0}, pow_str[16] = {0}, stat_str[32] = {0};
                snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", d->d_name);
                fast_read_file(path, cap_str, sizeof(cap_str));
                snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", d->d_name);
                fast_read_file(path, stat_str, sizeof(stat_str));
                snprintf(path, sizeof(path), "/sys/class/power_supply/%s/power_now", d->d_name);
                if (fast_read_file(path, pow_str, sizeof(pow_str)) <= 0) {
                    snprintf(path, sizeof(path), "/sys/class/power_supply/%s/current_now", d->d_name);
                    fast_read_file(path, pow_str, sizeof(pow_str));
                }
                double watts = strtod(pow_str, NULL) / 1000000.0;
                bprintf("Battery (%s): %s%% [%s] %s%.2f W", d->d_name, cap_str, stat_str, 
                        (watts > 0.01 ? "Rate: " : ""), watts);
            }
        }
        closedir(dir);
    }
}

static void print_cpu_section(bool show_per_core) {
    unsigned long long d_total = (g_cpu_curr[0].total > g_cpu_prev[0].total) ? (g_cpu_curr[0].total - g_cpu_prev[0].total) : 1;
    unsigned long long d_idle  = (g_cpu_curr[0].idle > g_cpu_prev[0].idle)   ? (g_cpu_curr[0].idle - g_cpu_prev[0].idle)   : 0;
    double cpu_pct = (d_total > d_idle) ? ((double)(d_total - d_idle) * 100.0 / d_total) : 0.0;

    char bar[32];
    buf_print_bar(bar, sizeof(bar), cpu_pct, 18);
    bprintf("--- [ CPU Usage (%d Threads) ] --------------------------------", g_num_cores);
    bprintf("Total Load: %5.1f%% %s", cpu_pct, bar);

    if (show_per_core && g_detected_cores > 0) {
        for (int i = 1; i <= g_detected_cores; i += 2) {
            char line[128] = {0};
            int pos = 0;
            for (int k = i; k < i + 2 && k <= g_detected_cores; k++) {
                unsigned long long c_tot = (g_cpu_curr[k].total > g_cpu_prev[k].total) ? (g_cpu_curr[k].total - g_cpu_prev[k].total) : 1;
                unsigned long long c_idl = (g_cpu_curr[k].idle > g_cpu_prev[k].idle)   ? (g_cpu_curr[k].idle - g_cpu_prev[k].idle)   : 0;
                double pct = (c_tot > c_idl) ? ((double)(c_tot - c_idl) * 100.0 / c_tot) : 0.0;
                buf_print_bar(bar, sizeof(bar), pct, 10);
                pos += snprintf(line + pos, sizeof(line) - pos, " CPU%-2d: %5.1f%% %s%s", 
                                k - 1, pct, bar, (k == i && k + 1 <= g_detected_cores) ? "  | " : "");
            }
            bprintf("%s", line);
        }
    }
}

static void print_dynamic_frequencies(void) {
    bprintf("--- [ CPU Dynamic Frequencies ] ---------------------------------");
    char path[128], buf[32];
    char line[128] = {0};
    int pos = 0;

    for (int i = 0; i < g_num_cores; i++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        if (fast_read_file(path, buf, sizeof(buf)) > 0) {
            double mhz = strtod(buf, NULL) / 1000.0;
            pos += snprintf(line + pos, sizeof(line) - pos, " Core %-2d: %6.1f MHz%s", 
                            i, mhz, ((i + 1) % 3 == 0 || (i + 1) == g_num_cores) ? "" : " | ");
        }
        if ((i + 1) % 3 == 0 || (i + 1) == g_num_cores) {
            if (pos > 0) bprintf("%s", line);
            line[0] = '\0';
            pos = 0;
        }
    }
}

static void print_ram_section(void) {
    RamStats ram = {0};
    SwapStats swap = {0};
    read_ram_stats(&ram, &swap);

    unsigned long total_mb = ram.total >> 10;
    unsigned long used_mb = ram.used >> 10;
    unsigned long buff_cache_mb = ram.buff_cache >> 10;
    unsigned long free_mb = ram.free >> 10;

    int ram_pct = (ram.total > 0) ? (int)((ram.used * 100) / ram.total) : 0;

    char bar[32];
    buf_print_bar(bar, sizeof(bar), (double)ram_pct, 18);

    bprintf("--- [ Memory Usage ] --------------------------------------------");
    bprintf("RAM Used  : %8lu / %8lu MB (%3d%%) %s", used_mb, total_mb, ram_pct, bar);
    bprintf("Breakdown : Pure: %lu MB | Buff/Cache: %lu MB | Free: %lu MB",
            used_mb, buff_cache_mb, free_mb);

    if (swap.total > 0) {
        unsigned long swap_total_mb = swap.total >> 10;
        unsigned long swap_used_mb = swap.used >> 10;
        int swp_pct = (int)((swap.used * 100) / swap.total);
        bprintf("Swap Used : %8lu / %8lu MB (%3d%%)", swap_used_mb, swap_total_mb, swp_pct);
    }
}

static void print_network_section(double delta_sec) {
    bprintf("--- [ Network Traffic ] -----------------------------------------");
    for (int i = 0; i < g_net_count_curr; i++) {
        for (int j = 0; j < g_net_count_prev; j++) {
            if (strcmp(g_net_curr[i].name, g_net_prev[j].name) == 0) {
                unsigned long long rx_diff = g_net_curr[i].rx_bytes >= g_net_prev[j].rx_bytes ? 
                                             g_net_curr[i].rx_bytes - g_net_prev[j].rx_bytes : 0;
                unsigned long long tx_diff = g_net_curr[i].tx_bytes >= g_net_prev[j].tx_bytes ? 
                                             g_net_curr[i].tx_bytes - g_net_prev[j].tx_bytes : 0;

                double rx_mbs = (rx_diff / (1024.0 * 1024.0)) / delta_sec;
                double tx_mbs = (tx_diff / (1024.0 * 1024.0)) / delta_sec;

                bprintf(" %-10s : Down: %6.2f MB/s (%5.1f Mbps) | Up: %6.2f MB/s (%5.1f Mbps)",
                        g_net_curr[i].name, rx_mbs, rx_mbs * 8.0, tx_mbs, tx_mbs * 8.0);
                break;
            }
        }
    }
}

static void print_disk_section(double delta_sec) {
    bprintf("--- [ Disk I/O & Load ] -----------------------------------------");
    char bar[32];
    for (int i = 0; i < g_disk_count_curr; i++) {
        for (int j = 0; j < g_disk_count_prev; j++) {
            if (strcmp(g_disks_curr[i].name, g_disks_prev[j].name) == 0) {
                unsigned long long rd_diff = g_disks_curr[i].rd_sectors >= g_disks_prev[j].rd_sectors ? 
                                             g_disks_curr[i].rd_sectors - g_disks_prev[j].rd_sectors : 0;
                unsigned long long wr_diff = g_disks_curr[i].wr_sectors >= g_disks_prev[j].wr_sectors ? 
                                             g_disks_curr[i].wr_sectors - g_disks_prev[j].wr_sectors : 0;
                unsigned long long io_ms_diff = g_disks_curr[i].io_ticks >= g_disks_prev[j].io_ticks ? 
                                                g_disks_curr[i].io_ticks - g_disks_prev[j].io_ticks : 0;

                double rd_kbs = ((rd_diff * 512.0) / 1024.0) / delta_sec;
                double wr_kbs = ((wr_diff * 512.0) / 1024.0) / delta_sec;
                double util = (io_ms_diff / (delta_sec * 1000.0)) * 100.0;
                if (util > 100.0) util = 100.0;

                buf_print_bar(bar, sizeof(bar), util, 10);
                bprintf(" %-8s : Read: %7.1f KB/s | Write: %7.1f KB/s | Util: %5.1f%% %s",
                        g_disks_curr[i].name, rd_kbs, wr_kbs, util, bar);
                break;
            }
        }
    }
}

static void print_temperatures_section(void) {
    bprintf("--- [ Temperature Sensors ] -------------------------------------");
    DIR *hwmon_dir = opendir("/sys/class/hwmon");
    if (!hwmon_dir) return;

    struct dirent *d;
    while ((d = readdir(hwmon_dir)) != NULL) {
        if (d->d_name[0] == '.') continue;
        char base[256];
        snprintf(base, sizeof(base), "/sys/class/hwmon/%s", d->d_name);

        char name[64] = "Sensor";
        char name_path[300];
        snprintf(name_path, sizeof(name_path), "%s/name", base);
        fast_read_file(name_path, name, sizeof(name));

        DIR *sensor_dir = opendir(base);
        if (!sensor_dir) continue;
        struct dirent *sd;
        while ((sd = readdir(sensor_dir)) != NULL) {
            if (strncmp(sd->d_name, "temp", 4) == 0 && strstr(sd->d_name, "_input")) {
                int input_idx = atoi(sd->d_name + 4);
                char temp_path[320], label_path[320];
                snprintf(temp_path, sizeof(temp_path), "%s/%s", base, sd->d_name);
                snprintf(label_path, sizeof(label_path), "%s/temp%d_label", base, input_idx);

                char label[64] = {0};
                if (fast_read_file(label_path, label, sizeof(label)) <= 0) {
                    snprintf(label, sizeof(label), "Sensor %d", input_idx);
                }

                char val_str[16];
                if (fast_read_file(temp_path, val_str, sizeof(val_str)) > 0) {
                    double deg = strtod(val_str, NULL) / 1000.0;
                    if (deg > 0.0 && deg < 150.0) {
                        bprintf(" %-16s | %-16s : %5.1f °C", name, label, deg);
                    }
                }
            }
        }
        closedir(sensor_dir);
    }
    closedir(hwmon_dir);
}

static void print_process_table(int sort_type, int limit) {
    if (sort_type == 1) qsort(g_procs, g_proc_count, sizeof(ProcessInfo), cmp_procs_by_mem);
    else if (sort_type == 2) qsort(g_procs, g_proc_count, sizeof(ProcessInfo), cmp_procs_by_io);
    else qsort(g_procs, g_proc_count, sizeof(ProcessInfo), cmp_procs_by_cpu);

    bprintf("--- [ Processes: %d Running, %d Sleeping, %d Zombie, %d Stopped ] ---",
            g_state_counts.running, g_state_counts.sleeping, g_state_counts.zombie, g_state_counts.stopped);
    bprintf("%-7s %-10s %-2s %-4s %-8s %-12s %-12s %-20s",
            "PID", "USER", "S", "TH", "%CPU", "RAM (RSS)", "DISK I/O", "COMMAND");
    bprintf("--------------------------------------------------------------------------------");

    int print_limit = (limit <= 0 || limit > g_proc_count) ? g_proc_count : limit;
    for (int i = 0; i < print_limit; i++) {
        char io_str[24];
        if (g_procs[i].io_rate_kbs >= 1024.0) {
            snprintf(io_str, sizeof(io_str), "%.1f MB/s", g_procs[i].io_rate_kbs / 1024.0);
        } else {
            snprintf(io_str, sizeof(io_str), "%.1f KB/s", g_procs[i].io_rate_kbs);
        }

        bprintf("%-7d %-10s %-2c %-4d %7.1f%% %9.1f MB %12s %-20s",
                g_procs[i].pid,
                g_procs[i].user,
                g_procs[i].state,
                g_procs[i].num_threads,
                g_procs[i].cpu_pct,
                g_procs[i].rss_kb / 1024.0,
                io_str,
                g_procs[i].name);
    }
}


static void sample_telemetry(bool opt_cpu, bool opt_threads, bool opt_ram, bool opt_procs,
                             bool opt_info, bool opt_temps, bool opt_disk, bool opt_net,
                             int sort_type, int limit_procs, double delta_sec) {
    g_buf.count = 0;

    if (opt_cpu || opt_threads || opt_procs) {
        read_all_cpu_ticks(g_cpu_curr, &g_detected_cores);
    }
    if (opt_disk) {
        g_disk_count_curr = read_diskstats(g_disks_curr, MAX_DISKS);
    }
    if (opt_net) {
        g_net_count_curr = read_network_bytes(g_net_curr, MAX_NET_IFS);
    }

    if (opt_procs) {
        unsigned long long sys_delta = (g_cpu_curr[0].total > g_cpu_prev[0].total) ? 
                                       (g_cpu_curr[0].total - g_cpu_prev[0].total) : 1;
        DIR *dir = opendir("/proc");
        if (dir) {
            struct dirent *e;
            g_proc_count = 0;
            memset(&g_state_counts, 0, sizeof(g_state_counts));

            ProcSnap next_snaps[MAX_PROCS];
            int next_snap_count = 0;

            while ((e = readdir(dir)) != NULL && g_proc_count < MAX_PROCS) {
                if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
                int pid = atoi(e->d_name);

                ProcessInfo *p = &g_procs[g_proc_count];
                p->pid = pid;
                p->cpu_pct = 0.0;
                p->io_rate_kbs = 0.0;

                unsigned long long ticks = 0;
                if (!read_proc_stat_fast(pid, p, &ticks)) continue;

                unsigned long long io_bytes = (sort_type == 2) ? read_proc_io_bytes(pid) : 0;
                if (next_snap_count < MAX_PROCS) {
                    next_snaps[next_snap_count].pid = pid;
                    next_snaps[next_snap_count].ticks = ticks;
                    next_snaps[next_snap_count].io_bytes = io_bytes;
                    next_snap_count++;
                }

                ProcSnap key = {.pid = pid};
                ProcSnap *found = bsearch(&key, g_snaps, g_snap_count, sizeof(ProcSnap), cmp_proc_snap);
                if (found && ticks >= found->ticks) {
                    unsigned long long proc_delta = ticks - found->ticks;
                    p->cpu_pct = ((double)proc_delta / (double)sys_delta) * 100.0;

                    if (sort_type == 2 && io_bytes >= found->io_bytes) {
                        p->io_rate_kbs = ((io_bytes - found->io_bytes) / 1024.0) / delta_sec;
                    }
                }

                resolve_proc_user(pid, p->user, sizeof(p->user));
                g_proc_count++;
            }
            closedir(dir);

            memcpy(g_snaps, next_snaps, sizeof(ProcSnap) * next_snap_count);
            g_snap_count = next_snap_count;
            qsort(g_snaps, g_snap_count, sizeof(ProcSnap), cmp_proc_snap);
        }
    }

    if (opt_info) print_hardware_info();
    print_power_and_uptime();
    if (opt_cpu) print_cpu_section(opt_threads);
    if (opt_threads) print_dynamic_frequencies();
    if (opt_ram) print_ram_section();
    if (opt_disk) print_disk_section(delta_sec);
    if (opt_net) print_network_section(delta_sec);
    if (opt_temps) print_temperatures_section();
    if (opt_procs) print_process_table(sort_type, limit_procs);

    memcpy(g_cpu_prev, g_cpu_curr, sizeof(g_cpu_curr));
    memcpy(g_disks_prev, g_disks_curr, sizeof(g_disks_curr));
    g_disk_count_prev = g_disk_count_curr;
    memcpy(g_net_prev, g_net_curr, sizeof(g_net_curr));
    g_net_count_prev = g_net_count_curr;
}


static void print_help(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("  -w, --watch [SEC] Interactive mode with scrolling support (default 1s)\n");
    printf("  -c, --cpu         Basic CPU statistics\n");
    printf("  -t, --threads     Per-core output and dynamic frequencies\n");
    printf("  -r, --ram         RAM and Swap details\n");
    printf("  -p, --procs       Process table\n");
    printf("  -i, --info        Static information (CPU, GPU, OS)\n");
    printf("  -T, --temps       Temperature sensors\n");
    printf("  -d, --disk        Disk I/O operations\n");
    printf("  -n, --net         Network traffic\n");
    printf("  -a, --all         Print all metrics\n");
    printf("      --sort-cpu    Sort processes by CPU usage (default)\n");
    printf("      --sort-mem    Sort processes by RAM usage\n");
    printf("      --sort-io     Sort processes by I/O activity\n");
    printf("  -l, --limit <N>   Process list limit (default: 15)\n");
}


int main(int argc, char *argv[]) {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    g_num_cores = cores > 0 ? (int)cores : 1;
    if (g_num_cores > MAX_CPUS) g_num_cores = MAX_CPUS;

    long page_size = sysconf(_SC_PAGESIZE);
    g_page_size_kb = page_size > 0 ? page_size / 1024 : 4;

    bool opt_cpu = false, opt_threads = false, opt_ram = false;
    bool opt_procs = false, opt_info = false, opt_temps = false;
    bool opt_disk = false, opt_net = false, opt_all = false;
    bool loop_mode = false;
    int sort_type = 0;
    int limit_procs = 15;
    int delay_ms = 1000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--watch") == 0) {
            loop_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int val = atoi(argv[i + 1]);
                if (val > 0) {
                    delay_ms = val;
                    i++;
                }
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--cpu") == 0) {
            opt_cpu = true;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            opt_threads = true;
            opt_cpu = true;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--ram") == 0) {
            opt_ram = true;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--procs") == 0) {
            opt_procs = true;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--info") == 0) {
            opt_info = true;
        } else if (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--temps") == 0) {
            opt_temps = true;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--disk") == 0) {
            opt_disk = true;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--net") == 0) {
            opt_net = true;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            opt_all = true;
        } else if (strcmp(argv[i], "--sort-cpu") == 0) {
            sort_type = 0;
        } else if (strcmp(argv[i], "--sort-mem") == 0) {
            sort_type = 1;
        } else if (strcmp(argv[i], "--sort-io") == 0) {
            sort_type = 2;
        } else if ((strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--limit") == 0) && i + 1 < argc) {
            limit_procs = atoi(argv[++i]);
        }
    }

    if (opt_all) {
        opt_cpu = opt_threads = opt_ram = opt_procs = opt_info = opt_temps = opt_disk = opt_net = true;
    } else if (!opt_cpu && !opt_ram && !opt_procs && !opt_info && !opt_temps && !opt_disk && !opt_net) {
        opt_cpu = opt_ram = opt_procs = true;
    }

    read_all_cpu_ticks(g_cpu_prev, &g_detected_cores);
    g_disk_count_prev = read_diskstats(g_disks_prev, MAX_DISKS);
    g_net_count_prev = read_network_bytes(g_net_prev, MAX_NET_IFS);

    if (opt_procs) {
        DIR *dir = opendir("/proc");
        if (dir) {
            struct dirent *e;
            g_snap_count = 0;
            while ((e = readdir(dir)) != NULL && g_snap_count < MAX_PROCS) {
                if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
                int pid = atoi(e->d_name);
                unsigned long long ticks = 0;
                if (read_proc_stat_fast(pid, NULL, &ticks)) {
                    g_snaps[g_snap_count].pid = pid;
                    g_snaps[g_snap_count].ticks = ticks;
                    g_snaps[g_snap_count].io_bytes = (sort_type == 2) ? read_proc_io_bytes(pid) : 0;
                    g_snap_count++;
                }
            }
            closedir(dir);
            qsort(g_snaps, g_snap_count, sizeof(ProcSnap), cmp_proc_snap);
        }
    }

    if (!loop_mode) {
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 300000000L}, NULL);
        sample_telemetry(opt_cpu, opt_threads, opt_ram, opt_procs, opt_info, opt_temps, 
                         opt_disk, opt_net, sort_type, limit_procs, 0.3);
        for (int i = 0; i < g_buf.count; i++) {
            printf("%s\n", g_buf.lines[i]);
        }
        return 0;
    }

    enable_raw_mode();
    int scroll_offset = 0;
    struct timespec last_update;
    clock_gettime(CLOCK_MONOTONIC, &last_update);
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 200000000L}, NULL);
    sample_telemetry(opt_cpu, opt_threads, opt_ram, opt_procs, opt_info, opt_temps, 
                     opt_disk, opt_net, sort_type, limit_procs, 0.2);
    bool need_redraw = true;


    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        
        long elapsed_ms = (now.tv_sec - last_update.tv_sec) * 1000 + 
                          (now.tv_nsec - last_update.tv_nsec) / 1000000;

        if (elapsed_ms >= delay_ms) {
            double delta_sec = (double)elapsed_ms / 1000.0;
            sample_telemetry(opt_cpu, opt_threads, opt_ram, opt_procs, opt_info, opt_temps, 
                             opt_disk, opt_net, sort_type, limit_procs, delta_sec);
            last_update = now;
            need_redraw = true;
        }

        int rows, cols;
        get_terminal_size(&rows, &cols);
        int max_scroll = g_buf.count - (rows - 1);
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_offset > max_scroll) scroll_offset = max_scroll;

        if (need_redraw) {
            render_frame(scroll_offset, rows, cols);
            need_redraw = false;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        long select_timeout_us = (delay_ms * 1000) / 4;
        if (select_timeout_us > 50000) select_timeout_us = 50000;
        if (select_timeout_us < 5000)  select_timeout_us = 5000;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = select_timeout_us;

        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'q' || c == 'Q') break;

                if (c == '\033') {
                    char seq[8];
                    if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if (seq[0] == '[') {
                            if (seq[1] == 'A') { 
                                if (scroll_offset > 0) scroll_offset--;
                                need_redraw = true;
                            } else if (seq[1] == 'B') { 
                                if (scroll_offset < max_scroll) scroll_offset++;
                                need_redraw = true;
                            } else if (seq[1] == 'H') { 
                                scroll_offset = 0;
                                need_redraw = true;
                            } else if (seq[1] == 'F') { 
                                scroll_offset = max_scroll;
                                need_redraw = true;
                            } else if (seq[1] == '5' && read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == '~') { 
                                scroll_offset -= rows;
                                if (scroll_offset < 0) scroll_offset = 0;
                                need_redraw = true;
                            } else if (seq[1] == '6' && read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == '~') { 
                                scroll_offset += rows;
                                if (scroll_offset > max_scroll) scroll_offset = max_scroll;
                                need_redraw = true;
                            } else if (seq[1] == 'M') { 
                                char b, x, y;
                                (void)read(STDIN_FILENO, &b, 1);
                                (void)read(STDIN_FILENO, &x, 1);
                                (void)read(STDIN_FILENO, &y, 1);
                                if (b == 64) {
                                    scroll_offset -= 3;
                                    if (scroll_offset < 0) scroll_offset = 0;
                                    need_redraw = true;
                                } else if (b == 65) {
                                    scroll_offset += 3;
                                    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
                                    need_redraw = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    cleanup_terminal(0);
    return 0;
}