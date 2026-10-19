/*
 * lcvs.c - The harness's maps in an LCVS shared-memory region (FORMATS.md section 3).
 *
 * A side channel for suzzer's region readers (shm_bitmap.py, ring readers): the INIT
 * map and every iteration's map go into the ring as INIT and ITER snapshots, written
 * with the seqlock protocol of section 3.5, and the live bitmap holds the union of
 * everything published so far (saturating sums), so LIVE_EMPTY stays clear. Crashed,
 * hung and timed-out iterations are not published: libcov's process never gets to
 * publish those either. Every field is in this host's byte order, as the format says
 * for its writer.
 *
 * PATH follows LIBCOV_SHM_PATH's rule: with a '/' after the first character it is a
 * file, otherwise a POSIX shared-memory name (/dev/shm). The region is left in place
 * at exit for readers that come late.
 *
 * Ownership follows libcov's: the decision to take a region is made under an exclusive
 * flock, so two writers starting at once (ember or libcov) cannot both claim it, and an
 * owner counts as alive only if its pid exists with the recorded start time (a reused
 * pid has another one), or, without /proc, if the pid exists at all.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lcv_formats.h"
#include "suzzer.h"

struct sz_lcvs {
    uint8_t                     *base;
    size_t                       size;
    struct libcov_shm_header    *hdr;
    uint8_t                     *live;
    uint32_t                     map_size;
    uint16_t                     ring_size;
    uint64_t                     published;
};

static bool looks_like_file(const char *path)
{
    for (const char *p = path + (path[0] == '/'); *p; p++)
        if (*p == '/')
            return true;
    return false;
}

/* A process's start time in clock ticks since boot (field 22 of /proc/PID/stat), low
 * 32 bits; 0 where that is not available (no such process, no /proc). pid 0: this one. */
static uint32_t start_time(pid_t pid)
{
    char path[64];
    if (pid)
        snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    else
        snprintf(path, sizeof(path), "/proc/self/stat");
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    char *p = strrchr(buf, ')');   /* the command name may hold spaces */
    if (!p)
        return 0;
    p++;
    for (int field = 3; field < 22 && p; field++)
        p = strchr(p + 1, ' ');   /* to the space before field + 1 */
    return p ? (uint32_t)strtoull(p + 1, NULL, 10) : 0;
}

/* libcov's owner_alive: the recorded start time decides when /proc has one for the pid;
 * otherwise the pid's existence does (EPERM: it exists, as another user's). */
static bool owner_alive(uint32_t pid, uint32_t start)
{
    if (!pid)
        return false;
    uint32_t cur = start_time((pid_t)pid);
    if (cur)
        return cur == start;
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

/* An existing region whose owner is still alive and is not us belongs to someone else. */
static bool owned_by_other(int fd)
{
    struct libcov_shm_header h;
    if (pread(fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h) || memcmp(h.magic, LIBCOV_SHM_MAGIC, 4))
        return false;
    if (h.owner_pid == (uint32_t)getpid() && h.owner_start == start_time(0))
        return false;
    return owner_alive(h.owner_pid, h.owner_start);
}

/* Releases the decision lock before close: a flock belongs to the open file
 * description, which the region's mapping keeps alive after close(fd). */
static void unlock_close(int fd)
{
    flock(fd, LOCK_UN);
    close(fd);
}

sz_lcvs *sz_lcvs_create(const char *path, uint32_t map_size, unsigned ring_size, uint32_t scheme_flags,
                        char *err, size_t errlen)
{
    if (ring_size < 2 || ring_size > 0xffff) {
        snprintf(err, errlen, "the ring needs 2 to 65535 slots");
        return NULL;
    }
    int fd;
    if (looks_like_file(path)) {
        fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    } else {
        char name[256];
        snprintf(name, sizeof(name), "/%s", path + (path[0] == '/'));
        fd = shm_open(name, O_RDWR | O_CREAT, 0600);
    }
    if (fd < 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return NULL;
    }
    /* Check, truncate and initialise under the lock libcov takes for the same choice. */
    if (flock(fd, LOCK_EX) != 0)
        uam_log(UAM_LOG_WARN, "lcvs", "%s: flock: %s (continuing unlocked)", path, strerror(errno));
    if (owned_by_other(fd)) {
        snprintf(err, errlen, "%s: in use by another live process", path);
        unlock_close(fd);
        return NULL;
    }
    const size_t slot_stride = sizeof(struct libcov_ring_slot_hdr) + map_size;
    const size_t size = sizeof(struct libcov_shm_header) + map_size + (size_t)ring_size * slot_stride;
    /* Truncating to 0 first gives a zeroed region: readers wait for the magic. */
    if (ftruncate(fd, 0) != 0 || ftruncate(fd, (off_t)size) != 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        unlock_close(fd);
        return NULL;
    }
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    sz_lcvs *l = base == MAP_FAILED ? NULL : calloc(1, sizeof(*l));
    if (!l) {
        snprintf(err, errlen, "%s: %s", path, base == MAP_FAILED ? strerror(errno) : "out of memory");
        if (base != MAP_FAILED)
            munmap(base, size);
        unlock_close(fd);
        return NULL;
    }
    l->base = base;
    l->size = size;
    l->hdr = base;
    l->map_size = map_size;
    l->ring_size = (uint16_t)ring_size;
    l->live = l->base + sizeof(struct libcov_shm_header);

    struct libcov_shm_header *h = l->hdr;
    h->version = LIBCOV_SHM_VERSION;
    h->ring_size = (uint16_t)ring_size;
    h->map_size = map_size;
    h->slot_stride = (uint32_t)slot_stride;
    h->flags = LIBCOV_SHM_FLAG_ACTIVE | scheme_flags;
    h->bitmap_offset = sizeof(struct libcov_shm_header);
    h->ring_offset = sizeof(struct libcov_shm_header) + map_size;
    h->owner_pid = (uint32_t)getpid();
    h->owner_start = start_time(0);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(h->magic, LIBCOV_SHM_MAGIC, 4);   /* last: the region is ready */
    unlock_close(fd);
    return l;
}

void sz_lcvs_close(sz_lcvs *l)
{
    if (!l)
        return;
    munmap(l->base, l->size);
    free(l);
}

uint64_t sz_lcvs_published(const sz_lcvs *l) { return l->published; }

/* One snapshot (section 3.5, writer). */
static void publish(sz_lcvs *l, uint64_t slot_flags, const uint8_t *map)
{
    struct libcov_shm_header *h = l->hdr;
    const uint64_t g = __atomic_add_fetch(&h->gen_current, 1, __ATOMIC_ACQ_REL);
    uint8_t *slot = l->base + h->ring_offset + (g % l->ring_size) * (uint64_t)h->slot_stride;
    struct libcov_ring_slot_hdr *sh = (struct libcov_ring_slot_hdr *)slot;
    __atomic_store_n(&sh->slot_gen, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sh->tid = (uint64_t)getpid();   /* one hart, one thread */
    sh->ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    sh->flags = slot_flags;
    memcpy(slot + sizeof(*sh), map, l->map_size);
    __atomic_store_n(&sh->slot_gen, g, __ATOMIC_RELEASE);
    if (slot_flags == LIBCOV_SLOT_FLAG_INIT)
        h->gen_init = g;
    for (uint32_t i = 0; i < l->map_size; i++) {
        unsigned v = (unsigned)l->live[i] + map[i];
        l->live[i] = (uint8_t)(v > 0xff ? 0xff : v);
    }
    l->published++;
}

void sz_lcvs_observe(void *ctx, uint64_t id, int kind, uam_fuzz_verdict v, uint64_t detail,
                     const uint8_t *map, size_t map_size)
{
    (void)id;
    (void)detail;
    sz_lcvs *l = ctx;
    if (map_size != l->map_size)
        return;
    if (kind == UAM_FUZZ_KIND_INIT)
        publish(l, LIBCOV_SLOT_FLAG_INIT, map);
    else if (v == UAM_FUZZ_OK)
        publish(l, LIBCOV_SLOT_FLAG_ITER, map);
}
