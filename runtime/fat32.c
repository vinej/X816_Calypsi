/* Read-only FAT32 for X816. See fat32.h for why this is a library and not
 * kernel code.
 *
 * BUILD THIS AT -O0. Calypsi 5.18 ELIMINATES VOLATILE READS at -O1 and above,
 * and every byte here arrives through one volatile register -- the block
 * device's data window. Out of line at -O1, buf_u32() emits its four reads;
 * inlined at -O1, it emits none, and the caller uses whatever happened to be
 * in the accumulator. The symptom was a reader that walked the root directory
 * perfectly and failed on the first subdirectory, because the cluster number
 * it "read" was never fetched.
 *
 * The same defect elides a read that follows a write to the same address,
 * which is why the device has separate CMD and STATUS registers; see
 * x816_sd.h. The durable fix is to move these four accessors into assembly,
 * where the C optimiser cannot see them, and build the rest at -O2.
 *
 * The logic here is verified independently: compiling this same file for the
 * host against a file-backed stub reads all the test files correctly, which
 * is how the bug was localised to codegen rather than to the parser. */

#include <string.h>
#include "fat32.h"
#include "x816_sd.h"

static uint32_t part_lba;        /* first sector of the filesystem            */
static uint16_t bytes_per_sec;
static uint8_t  sec_per_clus;
static uint32_t first_data_sec;  /* absolute, includes part_lba               */
static uint32_t fat_begin;       /* absolute LBA of FAT #1                    */
static uint32_t root_clus;
static uint8_t  num_fats;        /* FAT copies; ALL of them get updated      */
static uint32_t fat_sectors;     /* sectors per FAT copy                      */
static uint32_t max_cluster;     /* highest valid cluster number              */
static bool     mounted;

/* The block device's window is the only way to reach a fetched sector, and it
 * auto-increments, so every read here is sequential by construction. These
 * helpers exist to make that explicit rather than scattering SD_DATA about. */
static uint8_t
buf_u8(void)
{
    return SD_DATA;
}

static uint16_t
buf_u16(void)
{
    uint16_t lo = SD_DATA;
    return (uint16_t)(lo | ((uint16_t)SD_DATA << 8));
}

static uint32_t
buf_u32(void)
{
    uint32_t v = SD_DATA;
    v |= (uint32_t)SD_DATA << 8;
    v |= (uint32_t)SD_DATA << 16;
    v |= (uint32_t)SD_DATA << 24;
    return v;
}

/* Reposition the window. The device has no seek, so rewind and re-consume --
 * cheap, because it is all inside the FPGA's block buffer. */
static void
buf_seek(uint16_t off)
{
    SD_CMD = SD_CMD_RESET;
    while (off--)
        (void)SD_DATA;
}

static bool
parse_bpb(uint32_t lba)
{
    if (!sd_read_buf(lba))
        return false;

    buf_seek(11);
    bytes_per_sec = buf_u16();
    sec_per_clus  = buf_u8();
    uint16_t reserved = buf_u16();
    num_fats = buf_u8();

    /* A FAT32 BPB has a zero 16-bit FAT size and a zero root-entry count; if
     * either is non-zero this is FAT12/16 and none of the offsets below mean
     * what we would read into them. */
    buf_seek(17);
    uint16_t root_ents = buf_u16();
    buf_seek(22);
    uint16_t fatsz16 = buf_u16();

    buf_seek(36);
    uint32_t fatsz32 = buf_u32();
    buf_seek(44);
    root_clus = buf_u32();

    if (bytes_per_sec != 512 || sec_per_clus == 0 || num_fats == 0)
        return false;
    if (root_ents != 0 || fatsz16 != 0 || fatsz32 == 0)
        return false;
    if (root_clus < 2)
        return false;

    part_lba       = lba;
    fat_begin      = lba + reserved;
    first_data_sec = fat_begin + (uint32_t)num_fats * fatsz32;
    fat_sectors    = fatsz32;

    /* Bound for the allocator. A FAT sector holds 128 32-bit entries, so the
       table itself caps how many clusters can exist -- searching past that
       would read whatever follows the FAT and hand back a cluster number the
       volume does not have. */
    max_cluster    = fatsz32 * 128u;
    if (max_cluster < 2)
        return false;
    return true;
}

bool
fat32_mount(void)
{
    mounted = false;
    if (!sd_present())
        return false;

    /* Bare filesystem first: a card straight out of mkfs has its BPB in
     * sector 0, one formatted by a desktop OS has an MBR there instead. */
    if (parse_bpb(0)) {
        mounted = true;
        return true;
    }

    /* MBR: four 16-byte partition entries at offset 446; the start LBA is at
     * +8 within an entry. Take the first entry with a non-zero type. */
    if (!sd_read_buf(0))
        return false;
    for (uint8_t i = 0; i < 4; i++) {
        buf_seek((uint16_t)(446 + i * 16 + 4));
        uint8_t type = buf_u8();
        (void)buf_u8(); (void)buf_u8(); (void)buf_u8();
        uint32_t start = buf_u32();
        if (type != 0 && start != 0 && parse_bpb(start)) {
            mounted = true;
            return true;
        }
        if (!sd_read_buf(0))            /* parse_bpb clobbered the window */
            return false;
    }
    return false;
}

uint32_t fat32_root_cluster(void)     { return root_clus; }
uint16_t fat32_bytes_per_cluster(void){ return (uint16_t)(512u * sec_per_clus); }

static uint32_t
cluster_lba(uint32_t clus)
{
    return first_data_sec + (clus - 2) * sec_per_clus;
}

/* Next cluster in the chain, or >= 0x0FFFFFF8 for end-of-chain. */
static uint32_t
fat_next(uint32_t clus)
{
    uint32_t byte = clus * 4u;
    uint32_t sec  = fat_begin + (byte >> 9);
    if (!sd_read_buf(sec))
        return 0x0FFFFFFFu;
    buf_seek((uint16_t)(byte & 511u));
    return buf_u32() & 0x0FFFFFFFu;
}

/* Turn "HELLO.TXT" into the padded 11-byte on-disk form. Returns false for
 * anything that will not fit 8.3 -- better than silently truncating and
 * opening a different file. */
static bool
to_83(const char *name, char *out)
{
    uint8_t i = 0, j = 0;
    memset(out, ' ', 11);
    while (name[i] && name[i] != '.' && name[i] != '/') {
        if (j >= 8) return false;
        out[j++] = name[i++];
    }
    if (name[i] == '.') {
        i++;
        j = 8;
        while (name[i] && name[i] != '/') {
            if (j >= 11) return false;
            out[j++] = name[i++];
        }
    }
    return true;
}

/* Scan the directory chain starting at `dir_clus` for `name83`.
   On success fills first_cluster/size and reports whether it is a directory. */
static bool
dir_find(uint32_t dir_clus, const char *name83,
         uint32_t *out_clus, uint32_t *out_size, bool *out_isdir)
{
    while (dir_clus < 0x0FFFFFF8u && dir_clus >= 2) {
        for (uint8_t s = 0; s < sec_per_clus; s++) {
            if (!sd_read_buf(cluster_lba(dir_clus) + s))
                return false;
            for (uint16_t e = 0; e < 512; e += 32) {
                buf_seek(e);
                char nm[11];
                for (uint8_t k = 0; k < 11; k++)
                    nm[k] = (char)SD_DATA;
                uint8_t attr = SD_DATA;

                if (nm[0] == 0x00)
                    return false;                 /* end of directory */
                if ((uint8_t)nm[0] == 0xE5)
                    continue;                     /* deleted */
                if ((attr & 0x0F) == 0x0F)
                    continue;                     /* long-filename fragment */

                if (memcmp(nm, name83, 11) != 0)
                    continue;

                buf_seek((uint16_t)(e + 20));
                uint16_t hi = buf_u16();
                buf_seek((uint16_t)(e + 26));
                uint16_t lo = buf_u16();
                uint32_t sz = buf_u32();

                *out_clus  = ((uint32_t)hi << 16) | lo;
                *out_size  = sz;
                *out_isdir = (attr & 0x10) != 0;
                return true;
            }
        }
        dir_clus = fat_next(dir_clus);
    }
    return false;
}

/* Walk an absolute path to its entry. Shared by fat32_open and fat32_stat so
   the two can never disagree about what a path means. */
static bool
path_walk(const char *path, uint32_t *out_clus, uint32_t *out_size,
          bool *out_isdir)
{
    if (!mounted || path == NULL || *path != '/')
        return false;

    uint32_t clus  = root_clus;
    uint32_t size  = 0;
    bool     isdir = true;
    const char *p  = path + 1;

    while (*p) {
        char comp[11];
        /* A trailing slash ("/SUB/") is not an empty component. */
        if (*p == '/') { p++; continue; }
        if (!to_83(p, comp))
            return false;
        if (!isdir)
            return false;                 /* a path component under a file */
        if (!dir_find(clus, comp, &clus, &size, &isdir))
            return false;
        while (*p && *p != '/')
            p++;
        if (*p == '/')
            p++;
    }

    /* The root has no directory entry of its own; dir_find never ran, and the
       initial values already describe it. */
    *out_clus  = clus;
    *out_size  = size;
    *out_isdir = isdir;
    return true;
}

bool
fat32_stat(const char *path, uint32_t *cluster, uint32_t *size, bool *is_dir)
{
    uint32_t c, sz;
    bool     d;

    if (!path_walk(path, &c, &sz, &d))
        return false;
    if (cluster) *cluster = c;
    if (size)    *size    = sz;
    if (is_dir)  *is_dir  = d;
    return true;
}

bool
fat32_opendir(const char *path, fat32_dir *dir)
{
    uint32_t clus, size;
    bool     isdir;

    if (!path_walk(path, &clus, &size, &isdir))
        return false;
    if (!isdir)
        return false;

    /* A subdirectory entry stores cluster 0 to mean "the root", which is how
       ".." at the top of the tree is encoded. Normalise it here so callers
       never see a cluster number that is not a cluster. */
    dir->clus = (clus == 0) ? root_clus : clus;
    dir->sec  = 0;
    dir->off  = 0;
    dir->done = false;
    return true;
}

bool
fat32_readdir(fat32_dir *dir, fat32_dirent *e)
{
    if (!mounted || dir->done)
        return false;

    while (dir->clus < 0x0FFFFFF8u && dir->clus >= 2) {
        while (dir->sec < sec_per_clus) {
            if (!sd_read_buf(cluster_lba(dir->clus) + dir->sec)) {
                dir->done = true;
                return false;
            }
            while (dir->off < 512) {
                uint16_t at = dir->off;
                dir->off = (uint16_t)(at + 32);

                buf_seek(at);
                char nm[11];
                for (uint8_t k = 0; k < 11; k++)
                    nm[k] = (char)SD_DATA;
                uint8_t attr = SD_DATA;

                if (nm[0] == 0x00) {          /* end of directory */
                    dir->done = true;
                    return false;
                }
                if ((uint8_t)nm[0] == 0xE5)
                    continue;                 /* deleted */
                if ((attr & 0x0F) == 0x0F)
                    continue;                 /* long-filename fragment */
                if (attr & 0x08)
                    continue;                 /* volume label */

                buf_seek((uint16_t)(at + 20));
                uint16_t hi = buf_u16();
                buf_seek((uint16_t)(at + 26));
                uint16_t lo = buf_u16();
                uint32_t sz = buf_u32();

                /* 8.3 back to "NAME.EXT": trailing spaces are padding, and the
                   dot is not stored. */
                uint8_t n = 0, k;
                for (k = 0; k < 8 && nm[k] != ' '; k++)
                    e->name[n++] = nm[k];
                if (nm[8] != ' ') {
                    e->name[n++] = '.';
                    for (k = 8; k < 11 && nm[k] != ' '; k++)
                        e->name[n++] = nm[k];
                }
                e->name[n]  = '\0';
                e->cluster  = ((uint32_t)hi << 16) | lo;
                e->is_dir   = (attr & 0x10) != 0;
                e->size     = e->is_dir ? 0 : sz;
                return true;
            }
            dir->off = 0;
            dir->sec++;
        }
        dir->sec  = 0;
        dir->clus = fat_next(dir->clus);
    }

    dir->done = true;
    return false;
}

bool
fat32_open(const char *path, fat32_file *f)
{
    uint32_t clus, size;
    bool     isdir;

    if (!path_walk(path, &clus, &size, &isdir))
        return false;
    if (isdir)
        return false;                     /* fat32_open opens files */

    f->first_cluster = clus;
    f->size          = size;
    f->pos           = 0;
    f->cluster       = clus;
    f->cluster_off   = 0;
    /* Read-only handle. dir_lba = 0 is what makes fat32_write refuse and
       fat32_close a no-op; leaving these uninitialised would let a stray
       close() write a directory entry at whatever address was on the stack. */
    f->dir_lba       = 0;
    f->dir_off       = 0;
    f->dirty         = false;
    return true;
}

uint16_t
fat32_read(fat32_file *f, uint8_t *dst, uint16_t len)
{
    uint16_t done = 0;
    uint16_t cbytes = fat32_bytes_per_cluster();

    while (len > 0 && f->pos < f->size) {
        if (f->cluster < 2 || f->cluster >= 0x0FFFFFF8u)
            break;

        uint16_t sec_in_clus = (uint16_t)(f->cluster_off >> 9);
        uint16_t off_in_sec  = (uint16_t)(f->cluster_off & 511u);

        if (!sd_read_buf(cluster_lba(f->cluster) + sec_in_clus))
            break;
        buf_seek(off_in_sec);

        uint16_t n = (uint16_t)(512u - off_in_sec);
        if (n > len)                     n = len;
        if (f->pos + n > f->size)        n = (uint16_t)(f->size - f->pos);

        for (uint16_t i = 0; i < n; i++)
            dst[done + i] = SD_DATA;

        done            += n;
        len             -= n;
        f->pos          += n;
        f->cluster_off  += n;

        if (f->cluster_off >= cbytes) {
            f->cluster_off = 0;
            f->cluster     = fat_next(f->cluster);
        }
    }
    return done;
}

uint32_t
fat32_read_far(fat32_file *f, uint32_t dest, uint32_t len)
{
    uint32_t done = 0;
    uint16_t cbytes = fat32_bytes_per_cluster();

    /* Whole clusters only, by DMA. A partial cluster at the head or tail is
     * left to fat32_read -- mixing the two here would trade the DMA's whole
     * advantage for a byte loop anyway. */
    while (len >= cbytes && f->pos + cbytes <= f->size) {
        if (f->cluster < 2 || f->cluster >= 0x0FFFFFF8u)
            break;
        if (f->cluster_off != 0)
            break;
        if (!sd_read_dma(cluster_lba(f->cluster), dest, sec_per_clus))
            break;

        dest += cbytes;
        done += cbytes;
        len  -= cbytes;
        f->pos += cbytes;
        f->cluster = fat_next(f->cluster);
    }
    return done;
}

/* ==========================================================================
 * Writing
 *
 * Everything here is READ-MODIFY-WRITE of a whole 512-byte sector. The device
 * sends its entire buffer on a write, so touching one byte means fetching the
 * sector first; skipping that fetch would write 511 bytes of whatever the
 * buffer last held, which is how a filesystem gets quietly shredded rather
 * than loudly broken.
 *
 * Every FAT copy is updated, not just the first. Most volumes carry two, and
 * tools disagree about which they trust -- chkdsk compares them, some drivers
 * read the mirror. Updating one and not the other produces a card that reads
 * correctly here and is reported as corrupt everywhere else.
 * ========================================================================== */

static void
put_u8(uint8_t v)
{
    sd_buf_put(v);
}

static void
put_u16(uint16_t v)
{
    sd_buf_put((uint8_t)v);
    sd_buf_put((uint8_t)(v >> 8));
}

static void
put_u32(uint32_t v)
{
    sd_buf_put((uint8_t)v);
    sd_buf_put((uint8_t)(v >> 8));
    sd_buf_put((uint8_t)(v >> 16));
    sd_buf_put((uint8_t)(v >> 24));
}

/* Write one FAT entry, in every copy of the FAT.
 *
 * The top four bits of a FAT32 entry are reserved and must be preserved, so
 * the old value is read back rather than assumed to be zero. */
static bool
fat_set(uint32_t clus, uint32_t val)
{
    uint32_t byte = clus * 4u;
    uint16_t off  = (uint16_t)(byte & 511u);
    uint32_t rel  = byte >> 9;
    uint8_t  i;

    for (i = 0; i < num_fats; i++) {
        uint32_t sec = fat_begin + (uint32_t)i * fat_sectors + rel;
        uint32_t old;

        if (!sd_read_buf(sec))
            return false;
        buf_seek(off);
        old = buf_u32();

        buf_seek(off);
        put_u32((old & 0xF0000000u) | (val & 0x0FFFFFFFu));
        if (!sd_write_buf(sec))
            return false;
    }
    return true;
}

/* Claim a free cluster and mark it end-of-chain. Returns 0 when the volume is
   full, which the callers must treat as an error rather than as cluster 0. */
static uint32_t
fat_alloc(void)
{
    uint32_t clus;

    for (clus = 2; clus < max_cluster; clus++) {
        if (fat_next(clus) == 0) {
            if (!fat_set(clus, 0x0FFFFFFFu))
                return 0;
            return clus;
        }
    }
    return 0;
}

/* Release a whole chain. Walks first, frees after: reading the next link out
   of an entry that has already been zeroed would strand the rest. */
static bool
fat_free_chain(uint32_t clus)
{
    while (clus >= 2 && clus < 0x0FFFFFF8u) {
        uint32_t next = fat_next(clus);
        if (!fat_set(clus, 0))
            return false;
        clus = next;
    }
    return true;
}

/* Zero a freshly allocated cluster.
 *
 * Not optional for a DIRECTORY cluster -- an unzeroed one is full of whatever
 * the card held before, and the scanner reads that as directory entries. For a
 * file it costs a little time and buys the guarantee that a partially written
 * file trails zeroes rather than someone else's deleted data. */
static bool
cluster_zero(uint32_t clus)
{
    uint32_t lba = cluster_lba(clus);
    uint8_t  s;
    uint16_t i;

    for (s = 0; s < sec_per_clus; s++) {
        SD_CMD = SD_CMD_RESET;
        for (i = 0; i < 512; i++)
            sd_buf_put(0);
        if (!sd_write_buf(lba + s))
            return false;
    }
    return true;
}

/* Find a usable slot in a directory: a free entry, or the end marker.
 * Extends the directory by a cluster when it is full.
 *
 * `out_last` reports whether the slot was the 0x00 end marker, because in that
 * case the NEXT entry has to stay 0x00 -- a scanner stops there, and writing
 * over the marker without replacing it makes every later entry unreachable. */
static bool
dir_alloc_slot(uint32_t dir_clus, uint32_t *out_lba, uint16_t *out_off,
               bool *out_last)
{
    uint32_t clus = dir_clus;
    uint32_t prev = dir_clus;

    while (clus >= 2 && clus < 0x0FFFFFF8u) {
        uint8_t s;
        for (s = 0; s < sec_per_clus; s++) {
            uint32_t lba = cluster_lba(clus) + s;
            uint16_t e;
            if (!sd_read_buf(lba))
                return false;
            for (e = 0; e < 512; e += 32) {
                uint8_t first;
                buf_seek(e);
                first = buf_u8();
                if (first == 0x00 || first == 0xE5) {
                    *out_lba  = lba;
                    *out_off  = e;
                    *out_last = (first == 0x00);
                    return true;
                }
            }
        }
        prev = clus;
        clus = fat_next(clus);
    }

    /* Full: hang another cluster off the end and use its first slot. */
    clus = fat_alloc();
    if (clus == 0)
        return false;
    if (!cluster_zero(clus))
        return false;
    if (!fat_set(prev, clus))
        return false;
    *out_lba  = cluster_lba(clus);
    *out_off  = 0;
    *out_last = true;
    return true;
}

/* Write a directory entry. Only the fields that matter here are set; the
   timestamps are left zero, which is legal and which every tool tolerates. */
static bool
dir_put_entry(uint32_t lba, uint16_t off, const char *name83, uint8_t attr,
              uint32_t clus, uint32_t size)
{
    uint8_t k;

    if (!sd_read_buf(lba))
        return false;
    buf_seek(off);
    for (k = 0; k < 11; k++)
        put_u8((uint8_t)name83[k]);
    put_u8(attr);
    put_u8(0);                          /* reserved / lowercase flags */
    put_u8(0);                          /* creation time, tenths */
    put_u16(0);                         /* creation time */
    put_u16(0);                         /* creation date */
    put_u16(0);                         /* last access date */
    put_u16((uint16_t)(clus >> 16));    /* cluster high */
    put_u16(0);                         /* write time */
    put_u16(0);                         /* write date */
    put_u16((uint16_t)(clus & 0xFFFFu));/* cluster low */
    put_u32(size);
    return sd_write_buf(lba);
}

/* Split "/A/B/NAME.EXT" into the parent directory's cluster and the leaf's
   8.3 form. The parent must exist; nothing is created implicitly. */
static bool
split_parent(const char *path, uint32_t *parent_clus, char *leaf83)
{
    const char *p, *leaf;
    uint32_t clus  = root_clus;
    uint32_t size;
    bool     isdir = true;

    if (!mounted || path == NULL || *path != '/')
        return false;

    /* Find the last component. */
    leaf = NULL;
    for (p = path; *p; p++)
        if (*p == '/')
            leaf = p + 1;
    if (leaf == NULL || *leaf == '\0')
        return false;                   /* no name, or a trailing slash */

    /* Walk everything before it. */
    p = path + 1;
    while (p < leaf - 1) {
        char comp[11];
        if (!to_83(p, comp))
            return false;
        if (!isdir)
            return false;
        if (!dir_find(clus, comp, &clus, &size, &isdir))
            return false;
        while (*p && *p != '/')
            p++;
        if (*p == '/')
            p++;
    }
    if (!isdir)
        return false;

    if (!to_83(leaf, leaf83))
        return false;
    *parent_clus = (clus == 0) ? root_clus : clus;
    return true;
}

bool
fat32_create(const char *path, fat32_file *f)
{
    char     name83[11];
    uint32_t dir_clus, lba = 0, old_clus, old_size;
    uint16_t off = 0;
    bool     last = false, isdir, found = false;

    if (!split_parent(path, &dir_clus, name83))
        return false;

    /* Existing file? Truncate it: release the chain and REUSE the entry, so
       the file keeps its place in the directory rather than leaving a hole and
       growing the directory on every rewrite. */
    if (dir_find(dir_clus, name83, &old_clus, &old_size, &isdir)) {
        if (isdir)
            return false;               /* never truncate a directory */
        if (old_clus >= 2 && !fat_free_chain(old_clus))
            return false;
    }

    /* Find the entry by name -- it is either the one just truncated, or
       absent. dir_find deliberately does not report WHERE it looked, so this
       is the one place that needs to know. */
    {
        uint32_t c = dir_clus;
        while (!found && c >= 2 && c < 0x0FFFFFF8u) {
            uint8_t s2;
            for (s2 = 0; s2 < sec_per_clus && !found; s2++) {
                uint32_t l = cluster_lba(c) + s2;
                uint16_t e;
                if (!sd_read_buf(l))
                    return false;
                for (e = 0; e < 512; e += 32) {
                    char nm[11];
                    uint8_t k;
                    buf_seek(e);
                    for (k = 0; k < 11; k++)
                        nm[k] = (char)buf_u8();
                    if (memcmp(nm, name83, 11) == 0) {
                        lba = l; off = e; found = true;
                        break;
                    }
                    if ((uint8_t)nm[0] == 0x00)
                        break;          /* end of directory: not here */
                }
            }
            if (!found)
                c = fat_next(c);
        }
    }

    if (!found) {
        if (!dir_alloc_slot(dir_clus, &lba, &off, &last))
            return false;
        /* Keep the end marker: the slot after a 0x00 must stay 0x00, or every
           entry beyond this one becomes unreachable to a scanner. */
        if (last && (uint16_t)(off + 32) < 512) {
            if (!sd_read_buf(lba))
                return false;
            buf_seek((uint16_t)(off + 32));
            put_u8(0x00);
            if (!sd_write_buf(lba))
                return false;
        }
    }

    /* Zero clusters until something is written -- a zero first-cluster is how
       FAT32 spells "no data", and an empty file costs nothing on the card. */
    if (!dir_put_entry(lba, off, name83, 0x20, 0, 0))
        return false;

    f->first_cluster = 0;
    f->size          = 0;
    f->pos           = 0;
    f->cluster       = 0;
    f->cluster_off   = 0;
    f->dir_lba       = lba;
    f->dir_off       = off;
    f->dirty         = true;
    return true;
}

uint16_t
fat32_write(fat32_file *f, const uint8_t *src, uint16_t len)
{
    uint16_t done = 0;
    uint16_t cbytes = fat32_bytes_per_cluster();

    if (!mounted || f->dir_lba == 0)
        return 0;

    while (done < len) {
        uint32_t lba;
        uint16_t off, room, n, i;

        /* Need a cluster? Either the first one, or the next in the chain. */
        if (f->cluster < 2 || f->cluster_off >= cbytes) {
            uint32_t next;

            if (f->cluster < 2) {
                next = fat_alloc();
                if (next == 0)
                    break;
                if (!cluster_zero(next))
                    break;
                f->first_cluster = next;
            } else {
                next = fat_next(f->cluster);
                if (next >= 0x0FFFFFF8u) {
                    next = fat_alloc();
                    if (next == 0)
                        break;
                    if (!cluster_zero(next))
                        break;
                    if (!fat_set(f->cluster, next))
                        break;
                }
            }
            f->cluster     = next;
            f->cluster_off = 0;
        }

        lba  = cluster_lba(f->cluster) + (f->cluster_off >> 9);
        off  = (uint16_t)(f->cluster_off & 511u);
        room = (uint16_t)(512u - off);
        n    = (uint16_t)(len - done);
        if (n > room)
            n = room;

        /* Fetch before modifying: the device sends the whole buffer back. */
        if (!sd_read_buf(lba))
            break;
        buf_seek(off);
        for (i = 0; i < n; i++)
            sd_buf_put(src[done + i]);
        if (!sd_write_buf(lba))
            break;

        done           += n;
        f->cluster_off += n;
        f->pos         += n;
        if (f->pos > f->size)
            f->size = f->pos;
    }

    return done;
}

bool
fat32_close(fat32_file *f)
{
    char    name83[11];
    uint8_t k;
    bool    ok;

    if (!mounted || f->dir_lba == 0 || !f->dirty)
        return true;                    /* read-only handle: nothing to flush */

    /* Re-read the name out of the entry rather than carrying it in the handle:
       it is already on the card, and a copy is one more thing to get wrong. */
    if (!sd_read_buf(f->dir_lba))
        return false;
    buf_seek(f->dir_off);
    for (k = 0; k < 11; k++)
        name83[k] = (char)buf_u8();

    ok = dir_put_entry(f->dir_lba, f->dir_off, name83, 0x20,
                       f->first_cluster, f->size);
    if (ok)
        f->dirty = false;
    return ok;
}

bool
fat32_unlink(const char *path)
{
    char     name83[11];
    uint32_t dir_clus, clus, size, lba;
    uint16_t off;
    bool     isdir, found = false;

    if (!split_parent(path, &dir_clus, name83))
        return false;
    if (!dir_find(dir_clus, name83, &clus, &size, &isdir))
        return false;
    if (isdir)
        return false;                   /* rmdir's job, and it must check empty */

    /* Locate the entry so it can be marked deleted. */
    {
        uint32_t c = dir_clus;
        while (!found && c >= 2 && c < 0x0FFFFFF8u) {
            uint8_t s;
            for (s = 0; s < sec_per_clus && !found; s++) {
                uint32_t l = cluster_lba(c) + s;
                uint16_t e;
                if (!sd_read_buf(l))
                    return false;
                for (e = 0; e < 512; e += 32) {
                    char nm[11];
                    uint8_t k;
                    buf_seek(e);
                    for (k = 0; k < 11; k++)
                        nm[k] = (char)buf_u8();
                    if (memcmp(nm, name83, 11) == 0) {
                        lba = l; off = e; found = true;
                        break;
                    }
                }
            }
            if (!found)
                c = fat_next(c);
        }
    }
    if (!found)
        return false;

    /* Free the data FIRST. If the power goes between the two, lost clusters
       are a chkdsk warning; an entry pointing at freed clusters is corruption
       that another file can be allocated into. */
    if (clus >= 2 && !fat_free_chain(clus))
        return false;

    if (!sd_read_buf(lba))
        return false;
    buf_seek(off);
    put_u8(0xE5);
    return sd_write_buf(lba);
}
