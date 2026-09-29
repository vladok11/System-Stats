#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <ctype.h>

#define MAX_PROCS 1024

typedef struct{
    unsigned long total, free, available, buffers, cached, sreclaimable, buff_cache, pure_used, used;
} RamStats;

typedef struct{
    unsigned long total, free, used;
} SwapStats;

typedef struct 
{
    int pid;
    char name[256];
    unsigned long rss_total, rss_anon, rss_file, rss_shem;
} ProcessInfo;

int compare_procs(const void *a, const void* b) {
    const ProcessInfo *p1 = (const ProcessInfo *)a;
    const ProcessInfo *p2 = (const ProcessInfo *)b;
    if(p2->rss_total > p1->rss_total) return 1;
    if(p2->rss_total < p1->rss_total) return -1;
    return 0;
}

int main(int argc, char *argv[]){
    
bool show_swap = false;
    bool show_cache = false;
    bool show_procs = false;
    int limit_procs = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--swap") == 0) {
            show_swap = true;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--cache") == 0) {
            show_cache = true;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--procs") == 0) {
            show_procs = true;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            show_swap = true;
            show_cache = true;
            show_procs = true;
            if (i + 1 < argc && isdigit(argv[i + 1][0])) {
                limit_procs = atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--num") == 0) {
            show_procs = true;
            if (i + 1 < argc && isdigit(argv[i + 1][0])) {
                limit_procs = atoi(argv[i + 1]);
                i++;
            }
        }
    }
    RamStats ram = {0};
    SwapStats swap = {0};

    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) {
        perror("File open error");
        return 1;
    }
    char buffer[256];
    while(fgets(buffer, sizeof(buffer), f)){
        //RAM
        if(sscanf(buffer, "MemTotal: %lu kB", &ram.total) == 1) continue;
        else if (sscanf(buffer, "MemFree: %lu kB", &ram.free) == 1) continue;
        else if(sscanf(buffer, "MemAvailable: %lu kB", &ram.available) == 1) continue;
        else if(sscanf(buffer, "Buffers: %lu kB", &ram.buffers) == 1) continue;
        else if(sscanf(buffer, "Cached: %lu kB", &ram.cached) == 1) continue;
        else if(sscanf(buffer, "SReclaimable: %lu kB", &ram.sreclaimable) == 1) continue;
        
        //SWAP
        else if (show_swap) {
            if (sscanf(buffer, "SwapTotal: %lu kB", &swap.total) == 1) continue;
            else if(sscanf(buffer, "SwapFree: %lu kB", &swap.free) == 1) continue;
        }

    }
    fclose(f);

    ram.used = ram.total - ram.available;
    ram.buff_cache = ram.buffers+ram.cached+ram.sreclaimable;
    ram.pure_used = (ram.total > (ram.free + ram.buff_cache)) ? (ram.total - ram.free - ram.buff_cache) : 0;

    double total_mb = ram.total / 1024.0;
    double used_mb = ram.used / 1024.0; 
    printf("RAM Used: %.2f MB / %.2f MB (%.1f%%) (free: %.2f MB)\n", used_mb, total_mb, ((double)ram.used / ram.total) * 100.0, total_mb - used_mb);

    if(show_cache) {
        double cache_mb = ram.buff_cache / 1024.0;
        double pure_used_mb = ram.pure_used / 1024.0;;
        double percent_cache = (cache_mb / total_mb) * 100.0;
        double percent_pure_used = (pure_used_mb / total_mb) * 100.0;
        printf("Pure App Usage: %.2f MB / %.2f MB (%.1f%%) (free: %.2f MB)\n", pure_used_mb, total_mb, percent_pure_used, total_mb - pure_used_mb);
        printf("Buff/Cache: %.2f MB / %.2f MB (%.1f%%) (free: %.2f MB)\n", cache_mb, total_mb, percent_cache, total_mb - cache_mb);
        printf("Pure + Cache Sum: %.2f MB / %.2f MB (%.1f%%) (free: %.2f MB)\n", pure_used_mb + cache_mb, total_mb, percent_pure_used + percent_cache, total_mb - (pure_used_mb + cache_mb));
    }

    if(show_swap) {
        swap.used = swap.total - swap.free;
        double swap_total_mb = swap.total / 1024.0;
        double swap_used_mb = swap.used / 1024.0;
        double precent_swap = swap.total > 0 ? ((double)swap.used / swap.total) * 100.0 : 0.0;
        printf("Swap Used: %.2f MB / %.2f MB (%.1f%%) (free: %.2f MB)\n", swap_used_mb, swap_total_mb, precent_swap, swap_total_mb - swap_used_mb);
    }   

    if(show_procs) {
        DIR *dir = opendir("/proc");
        if(!dir) {
            perror("Opendir error /proc");
            return 1;
        }

        ProcessInfo procs[MAX_PROCS];
        int proc_count = 0;
        struct dirent *entry;

        while ((entry =  readdir(dir)) != NULL && proc_count < MAX_PROCS) {
            if(!isdigit(entry->d_name[0])) continue;

            int pid = atoi(entry->d_name);
            char path[256];
            snprintf(path, sizeof(path), "/proc/%d/status", pid);

            FILE *f_stat = fopen(path, "r");
            if(!f_stat) continue;

            ProcessInfo *p = &procs[proc_count];
            p->pid = pid;
            p->rss_total = 0;
            p->rss_anon = 0;
            p->rss_shem = 0;
            p->rss_file = 0;
            strcpy(p->name, "unknown");

            char proc_buf[256];
            while(fgets(proc_buf, sizeof(proc_buf), f_stat)) {
                if (sscanf(proc_buf, "Name:\t%255s", p->name) == 1) continue;
                else if (sscanf(proc_buf, "VmRSS:\t%lu kB", &p->rss_total) == 1) continue;
                else if (sscanf(proc_buf, "RssAnon:\t%lu kB", &p->rss_anon) == 1) continue;
                else if (sscanf(proc_buf, "RssFile:\t%lu kB", &p->rss_file) == 1) continue;
                else if (sscanf(proc_buf, "RssShmem:\t%lu kB", &p->rss_shem) == 1) continue;
            }
            fclose(f_stat);

            if (p->rss_total > 0) proc_count++;
        }
        closedir(dir);

        qsort(procs, proc_count, sizeof(ProcessInfo), compare_procs);

        printf("\n%-8s %-20s %-12s %-12s %-12s %-12s\n", "PID", "NAME", "TOTAL RAM", "HEAP/STACK", "CODE/LIBS", "SHARED");
        printf("--------------------------------------------------------------------\n");

        int limit = (limit_procs <= 0 || limit_procs > proc_count) ? proc_count : limit_procs;
        for (int i = 0; i < limit; i++) {
            printf("%-8d %-20s %-12.2f %-12.2f %-12.2f %-12.2f MB\n",procs[i].pid, procs[i].name, procs[i].rss_total / 1024.0, procs[i].rss_anon / 1024.0, procs[i].rss_file / 1024.0, procs[i].rss_shem / 1024.0);
        }
    }
    return 0;
}