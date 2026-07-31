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
    uint8_t  num_fats = buf_u8();

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

bool
fat32_open(const char *path, fat32_file *f)
{
    if (!mounted || path == NULL || *path != '/')
        return false;

    uint32_t clus = root_clus;
    uint32_t size = 0;
    bool     isdir = true;
    const char *p = path + 1;

    while (*p) {
        char comp[11];
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
    if (isdir)
        return false;                     /* fat32_open opens files */

    f->first_cluster = clus;
    f->size          = size;
    f->pos           = 0;
    f->cluster       = clus;
    f->cluster_off   = 0;
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
