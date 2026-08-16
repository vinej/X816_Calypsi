/* The X816 boot prompt. See shell.h and X816_Core doc/SHELL.md. BUILD AT -O0. */

#include "shell.h"
#include "console.h"
#include "fat32.h"
#include "kfs.h"
#include "kmem.h"
#include "x816_contract.h"

/* Flat 24-bit access to anywhere in the 16 MB.
 *
 * __far is what makes the memory commands able to reach the whole machine
 * from the small data model -- an ordinary pointer is 16 bits and would only
 * ever see bank $00. Verified to generate real long addressing
 * (`sta [.tiny (_Dp+4)]`, opcode $87); see the README. */
static uint8_t __far *far_ptr(uint32_t addr)
{
    return (uint8_t __far *)addr;
}

static bool
boot_desktop_selected(void)
{
    volatile uint8_t *sysctl = (volatile uint8_t *)X816_SYSCTL;
    return (*sysctl & X816_SYSCTL_DESKTOP) != 0;
}

/* ---- small helpers ----------------------------------------------------- */

static bool
is_space(char c)
{
    return c == ' ' || c == '\t';
}

static uint8_t
hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}

bool
sh_parse_hex(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    uint8_t  n = 0;

    if (*s == '\0')
        return false;
    /* A colon is allowed so bank:offset can be typed the way the memory map
       writes it -- `dump 01:0000` -- but it is only punctuation. */
    while (*s) {
        if (*s == ':') { s++; continue; }
        uint8_t d = hex_digit(*s);
        if (d == 0xFF)
            return false;
        if (n >= 6)                 /* more than 24 bits */
            return false;
        v = (v << 4) | d;
        n++;
        s++;
    }
    *out = v;
    return true;
}

static char hexchr(uint8_t n) { return (char)(n < 10 ? '0' + n : 'A' + n - 10); }

void sh_put_hex8(uint8_t v)
{
    con_putc(hexchr((uint8_t)(v >> 4)));
    con_putc(hexchr((uint8_t)(v & 15)));
}

void sh_put_hex16(uint16_t v)
{
    sh_put_hex8((uint8_t)(v >> 8));
    sh_put_hex8((uint8_t)v);
}

void sh_put_hex24(uint32_t v)
{
    sh_put_hex8((uint8_t)(v >> 16));
    con_putc(':');
    sh_put_hex16((uint16_t)v);
}

static int
str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);   /* commands are
                                                               case-insensitive */
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb)
            return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

#ifdef KERNEL_RESIDENT
static int
str_eq_far(const char *a, const char __far *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb)
            return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}
#define SH_PUT_HELP(s) con_puts_far(s)
#define SH_STR_EQ(a, b) str_eq_far((a), (b))
#else
#define SH_PUT_HELP(s) con_puts(s)
#define SH_STR_EQ(a, b) str_eq((a), (b))
#endif

/* ---- tokeniser --------------------------------------------------------- */

uint8_t
sh_tokenise(char *line, char **argv)
{
    uint8_t n = 0;

    while (*line) {
        while (is_space(*line))
            *line++ = '\0';
        if (*line == '\0')
            break;
        if (n >= SH_MAX_ARGS)
            return SH_TOO_MANY_ARGS;
        argv[n++] = line;
        while (*line && !is_space(*line))
            line++;
    }
    return n;
}

/* ---- commands ---------------------------------------------------------- */

static uint8_t cmd_help(uint8_t argc, char **argv);

static uint8_t
cmd_ver(uint8_t argc, char **argv)
{
    static char v[] = "X816 shell 0.1\n";
    (void)argc; (void)argv;
    con_puts(v);
    return 0;
}

static uint8_t
cmd_cls(uint8_t argc, char **argv)
{
    (void)argc; (void)argv;
    con_cls();
    return 0;
}

/* dump addr [len] -- hex and ASCII, 16 bytes to a line.
 *
 * This is the machine's only memory inspector: there is no monitor and no
 * debugger, so `dump 01:0000` is how you check that a program loaded where
 * you thought it did. */
static uint8_t
cmd_dump(uint8_t argc, char **argv)
{
    uint32_t addr, len = 64;
    uint8_t  i, j;

    if (!sh_parse_hex(argv[1], &addr))
        return 1;
    if (argc > 2 && !sh_parse_hex(argv[2], &len))
        return 1;
    if (len == 0)
        return 0;

    for (i = 0; i < 64 && len; i++) {           /* cap at 64 lines a time */
        uint8_t __far *p = far_ptr(addr);
        uint8_t n = (len < 16) ? (uint8_t)len : 16;

        sh_put_hex24(addr);
        con_putc(' ');
        for (j = 0; j < 16; j++) {
            if (j < n) {
                sh_put_hex8(p[j]);
                con_putc(' ');
            } else {
                { static char sp3[] = "   "; con_puts(sp3); }
            }
        }
        con_putc('|');
        for (j = 0; j < n; j++) {
            uint8_t c = p[j];
            /* All of printable ASCII: the CP437 font has real lower case, so
               folding $60-$7E to '.' would hide exactly the bytes most worth
               reading in a dump of text. */
            con_putc((c >= 0x20 && c <= 0x7E) ? (char)c : '.');
        }
        con_putc('\n');

        addr += n;
        len  -= n;
    }
    return 0;
}

static uint8_t
cmd_peek(uint8_t argc, char **argv)
{
    uint32_t addr;
    (void)argc;
    if (!sh_parse_hex(argv[1], &addr))
        return 1;
    sh_put_hex24(addr);
    { static char eq[] = " = "; con_puts(eq); }
    sh_put_hex8(*far_ptr(addr));
    con_putc('\n');
    return 0;
}

static uint8_t
cmd_poke(uint8_t argc, char **argv)
{
    uint32_t addr, val;
    (void)argc;
    if (!sh_parse_hex(argv[1], &addr) || !sh_parse_hex(argv[2], &val))
        return 1;
    if (val > 0xFF)
        return 1;
    *far_ptr(addr) = (uint8_t)val;
    return 0;
}

static uint8_t
cmd_fill(uint8_t argc, char **argv)
{
    uint32_t addr, len, val;
    (void)argc;
    if (!sh_parse_hex(argv[1], &addr) || !sh_parse_hex(argv[2], &len)
        || !sh_parse_hex(argv[3], &val))
        return 1;
    if (val > 0xFF)
        return 1;
    while (len--)
        *far_ptr(addr++) = (uint8_t)val;
    return 0;
}

/* move dst src len -- overlap-safe, because the obvious use is shuffling a
   loaded image around and the ranges will overlap sooner or later. */
static uint8_t
cmd_move(uint8_t argc, char **argv)
{
    uint32_t dst, src, len;
    (void)argc;
    if (!sh_parse_hex(argv[1], &dst) || !sh_parse_hex(argv[2], &src)
        || !sh_parse_hex(argv[3], &len))
        return 1;
    if (dst > src) {
        while (len--)
            *far_ptr(dst + len) = *far_ptr(src + len);
    } else {
        uint32_t i;
        for (i = 0; i < len; i++)
            *far_ptr(dst + i) = *far_ptr(src + i);
    }
    return 0;
}


/* ==========================================================================
 * Files
 *
 * The card is mounted LAZILY, on the first command that needs it, not from
 * con_init. A machine with no card in the slot must still reach a prompt and
 * still run dump/peek/poke -- those are exactly the commands you want when
 * the card is the thing that is broken.
 * ========================================================================== */

/* The working directory, the mount, and path resolution all belong to kfs.c
   now. They used to live here, and that was a latent bug rather than merely
   duplication: `cd` moved the prompt's copy while FS_CHDIR moved the kernel's,
   so a program launched from the prompt would have resolved a relative path
   against a different directory than the one on screen. One owner, per
   doc/KERNEL.md section 2.1 -- two programs each with their own copy corrupt
   something. */
#define cwd (kfs_cwd())

static bool
fs_ready(void)
{
    static char nocard[] = "NO CARD\n";
    if (kfs_ready())
        return true;
    con_puts(nocard);
    return false;
}

#define sh_abspath(arg, out) kfs_abspath((arg), (out))

static uint8_t
str_len(const char *s)
{
    uint8_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Decimal, because a file size in hex helps nobody. Digits are produced
   backwards into a small buffer -- the alternative is repeated division by
   descending powers of ten, which costs more of them. */
static void
put_dec32(uint32_t v)
{
    char     d[10];
    uint8_t  n = 0;

    if (v == 0) {
        con_putc('0');
        return;
    }
    while (v && n < 10) {
        d[n++] = (char)('0' + (uint8_t)(v % 10));
        v /= 10;
    }
    while (n)
        con_putc(d[--n]);
}

static void
put_pad(uint8_t have, uint8_t want)
{
    while (have < want) {
        con_putc(' ');
        have++;
    }
}

static uint8_t
cmd_pwd(uint8_t argc, char **argv)
{
    (void)argc; (void)argv;
    con_puts(cwd);
    con_putc('\n');
    return 0;
}

/* mem            -- the memory map, and where the user ceiling currently is
 * mem release    -- hand the kernel writable-data region to MEM_ALLOC
 *
 * WHY THIS COMMAND EXISTS AT ALL. The failure the releasable region is built to
 * avoid is a STALE BOUNDARY: an allocator that compiled the ceiling in and is
 * wrong on one side of a release, silently. The cheapest standing defence
 * against that is for the number to be visible -- so this prints what
 * K_MEM_TOP reports rather than what any constant says, and durexForth's boot
 * banner prints the value it queried. Two independent views of one number.
 *
 * The strings are `static char[]` and terse on purpose: in the KERNEL build
 * x816-kernel.scm puts `data` in KernRAM, which is the 4 KB bank-$00 claim, so
 * every byte of shell prose here is spent out of doc/KERNEL.md 3.1's budget.
 * A literal would go to cdata and cost nothing -- and cannot be addressed from
 * bank $00 in the LOADABLE build, which is the whole reason for this pattern
 * (see shell.h). */
static uint8_t
cmd_mem(uint8_t argc, char **argv)
{
    static char w_rel[]  = "release";
    static char l_top[]  = "top  ";
    static char s_res[]  = "  kdata reserved\n";
    static char s_rel[]  = "  kdata released\n";
    static char l_heap[] = "heap ";
    static char s_free[] = " free, ";
    static char s_live[] = " live\n";

    uint32_t top;
    uint16_t lo;

    /* The one subcommand is spelled in full, not prefix-matched: it cannot be
       undone before a reboot, so it should not be reachable by a typo. On
       anything else, and on a second release, fall through and just REPORT --
       the state line below says which it was, so a separate error string would
       be bank-$00 bytes spent saying what the next line already says. */
    if (argc > 1 && str_eq(argv[1], w_rel) && kmem_edit_reserved()) {
        kfs_c = KMEM_REGION_EDIT;
        kmem_release();
    }

    /* The ceiling comes from the entry every allocator is required to ask, not
       from X816_HEAP_END -- which is only the boot default. That is the point
       of printing it at all: a stale boundary is this design's failure mode,
       and two independent views of one number is the standing defence.
       durexForth's boot banner prints the value it queried, for the same
       reason.
       One call: kmem_top() returns the low half AND leaves the bank in kfs_x,
       so fetching the halves with two calls would work but would read as
       though they came from different places. */
    lo  = kmem_top();
    top = ((uint32_t)kfs_x << 16) | lo;

    con_puts(l_top);
    sh_put_hex24(top);
    con_puts(kmem_edit_reserved() ? s_res : s_rel);

    con_puts(l_heap);
    put_dec32(kmem_free_bytes());
    con_puts(s_free);
    put_dec32(kmem_live());
    con_puts(s_live);
    return 0;
}

static uint8_t
cmd_cd(uint8_t argc, char **argv)
{
    static char notdir[] = " NOT A DIRECTORY\n";

    (void)argc;
    if (!fs_ready())
        return 1;
    /* One refusal for "no such path" and "that is a file", because from the
       prompt they are the same mistake and the distinction only matters to a
       program, which gets it in the ABI's error code. */
    if (!kfs_chdir_path(argv[1])) {
        con_puts(argv[1]);
        con_puts(notdir);
        return 1;
    }
    return 0;
}

static uint8_t
cmd_ls(uint8_t argc, char **argv)
{
    static char nodir[] = " NOT FOUND\n";
    static char dirtag[] = "<DIR>";
    char         path[SH_MAX_LINE];
    fat32_dir    d;
    fat32_dirent e;
    uint16_t     files = 0;

    if (!fs_ready())
        return 1;
    if (!sh_abspath(argc > 1 ? argv[1] : cwd, path))
        return 1;
    if (!fat32_opendir(path, &d)) {
        con_puts(path);
        con_puts(nodir);
        return 1;
    }

    while (fat32_readdir(&d, &e)) {
        uint8_t n = str_len(e.name);
        con_puts(e.name);
        put_pad(n, 14);
        if (e.is_dir)
            con_puts(dirtag);
        else
            put_dec32(e.size);
        con_putc('\n');
        files++;
        /* A directory with hundreds of entries would scroll the useful part
           off the top; stop where a screen does. */
        if (files >= 200)
            break;
    }
    return 0;
}

static uint8_t
cmd_type(uint8_t argc, char **argv)
{
    static char nofile[] = " NOT FOUND\n";
    static char isdir[]  = " IS A DIRECTORY\n";
    static char ioerr[]  = " I/O ERROR (OUTPUT TRUNCATED)\n";
    char       path[SH_MAX_LINE];
    fat32_file f;
    bool       dir_flag;
    uint8_t    buf[64];
    uint16_t   got, i;

    (void)argc;
    if (!fs_ready())
        return 1;
    if (!sh_abspath(argv[1], path))
        return 1;
    if (!fat32_open(path, &f)) {
        /* Say WHICH failure this is. fat32_open refuses directories
           too, and reporting a directory as "not found" sends the
           reader hunting for a file that is sitting right there. */
        con_puts(path);
        if (fat32_stat(path, 0, 0, &dir_flag) && dir_flag)
            con_puts(isdir);
        else
            con_puts(nofile);
        return 1;
    }

    /* fat32_read returns a short count at EOF and on a device failure alike;
       only the volume's I/O flag tells them apart. Clear it going in, test it
       coming out -- otherwise a card yanked mid-file shows a truncated text
       with no hint that anything is missing. */
    fat32_clearerr();
    while ((got = fat32_read(&f, buf, sizeof buf)) != 0) {
        for (i = 0; i < got; i++) {
            uint8_t c = buf[i];
            /* CR is dropped rather than printed: a DOS file is CRLF, and
               con_putc treats CR as "column 0", which would overprint every
               line with the next one. */
            if (c == 0x0D)
                continue;
            con_putc((char)c);
        }
    }
    if (fat32_ioerr()) {
        con_putc('\n');
        con_puts(path);
        con_puts(ioerr);
        return 1;
    }
    return 0;
}


/* ==========================================================================
 * Programs
 *
 * The staging area. A loadable image is linked for $01:0000 and is NOT
 * relocatable -- Calypsi's absolute long addressing bakes the destination into
 * every jsl -- so `run` cannot simply load it somewhere else and jump. It has
 * to land exactly where the shell is currently executing.
 *
 * So the file is read here first, well clear of everything, and only then
 * moved down by the blob in exec.s, which runs from bank $00 for the one
 * reason that matters: the copy erases bank $01, including whatever code is
 * performing it. See exec.s.
 *
 * The staging address and the size cap are contract constants (x816_contract.h,
 * generated): exec.s copies FROM one and TO the other, goshell.c and kexec.c
 * stage at the same place, and x816-lib.scm has to leave the region alone.
 * Five files, one value -- it used to be five literals.
 * ========================================================================== */

extern void     x816_exec_init(void);
extern void     x816_exec(void);           /* does not return */
extern uint16_t x816_exec_len;

/* "X816" at the base of the image. boot/boot.s checks the same four bytes
   before jumping, and refusing here means a mistyped filename produces a
   message instead of a machine that wanders off into stale memory. */
static bool
has_magic(uint32_t at)
{
    uint8_t __far *p = far_ptr(at);
    return p[0] == 'X' && p[1] == '8' && p[2] == '1' && p[3] == '6';
}

static uint8_t
load_file(const char *arg, uint32_t dest, uint32_t *out_size)
{
    static char nofile[]  = " NOT FOUND\n";
    static char toobig[]  = " TOO BIG\n";
    static char isdir2[]  = " IS A DIRECTORY\n";
    static char shortrd[] = " SHORT READ\n";
    char       path[SH_MAX_LINE];
    fat32_file f;
    bool       dir_flag;
    uint32_t   got;

    if (!fs_ready())
        return 1;
    if (!sh_abspath(arg, path))
        return 1;
    if (!fat32_open(path, &f)) {
        con_puts(path);
        if (fat32_stat(path, 0, 0, &dir_flag) && dir_flag)
            con_puts(isdir2);
        else
            con_puts(nofile);
        return 1;
    }
    if (f.size == 0 || f.size > X816_EXEC_MAX) {
        con_puts(path);
        con_puts(toobig);
        return 1;
    }

    /* TWO passes, and the second one is not optional.
     *
     * fat32_read_far moves WHOLE CLUSTERS by DMA and returns how many bytes it
     * actually moved -- its contract, deliberately, since mixing a byte loop
     * into it would cost the DMA's whole advantage. So it stops at the last
     * cluster boundary, and for a file smaller than one cluster it copies
     * NOTHING and returns 0.
     *
     * Ignoring that return value is exactly the bug this replaces: `load`
     * reported the full size while leaving the tail of every image stale, and a
     * 164-byte program was reported as loaded without a single byte being
     * written. It looked fine because the bytes anyone thought to check were
     * inside the part that HAD transferred.
     *
     * So: clusters by DMA, then the remainder a byte at a time, and verify the
     * total. */
    got = fat32_read_far(&f, dest, f.size);

    while (got < f.size) {
        uint8_t  buf[64];
        uint16_t n = fat32_read(&f, buf, sizeof buf);
        uint8_t __far *p;
        uint16_t i;

        if (n == 0)
            break;                      /* short file: EOF before the size */
        p = far_ptr(dest + got);
        for (i = 0; i < n; i++)
            p[i] = buf[i];
        got += n;
    }

    if (got != f.size) {
        con_puts(path);
        con_puts(shortrd);
        return 1;
    }

    *out_size = got;
    return 0;
}

/* go -- enter an image already loaded at $01:0000 by the OSD's "Load Image".
 * With the resident kernel owning boot, an OSD-loaded image no longer starts
 * by itself (the firmware wins the magic race in boot.s); this is the
 * explicit hand-over. Does not return on success. */
extern void x816_go(void);              /* does not return */
static uint8_t
cmd_go(uint8_t argc, char **argv)
{
    static char nomagic[] = "NO X816 IMAGE AT $01:0000\n";
    (void)argc; (void)argv;
    if (!has_magic(X816_PROG_BASE)) {
        con_puts(nomagic);
        return 1;
    }
    kfs_carry_save();               /* so the prompt comes back here, not / */
    x816_go();
    return 0;                           /* unreachable */
}

/* Load `arg` over the shell and enter it. Returns only on failure, having
   said why.
 *
 * Split out of cmd_run because a bare `test` at the prompt runs TEST.BIN by
 * the same route, and two copies of "stage it, check the magic, remember
 * where we were, go" is two places for one of those steps to go missing. */
static uint8_t
run_image(const char *arg)
{
    static char nomagic[] = " IS NOT AN X816 IMAGE\n";
    uint32_t size;

    if (load_file(arg, X816_EXEC_STAGE, &size) != 0)
        return 1;

    /* Checked in the STAGING copy, before anything is overwritten. Once the
       relocation starts there is no shell left to report an error with. */
    if (!has_magic(X816_EXEC_STAGE)) {
        con_puts(arg);
        con_puts(nomagic);
        return 1;
    }

    /* Last thing before the hand-over, and after every refusal above: a
       command that did NOT launch anything must leave the block alone, or a
       later reset would restore a directory nothing was ever run from. */
    kfs_carry_save();

    x816_exec_len = (uint16_t)size;
    x816_exec();                    /* never comes back */
    return 0;                       /* unreachable, and the compiler wants it */
}

/* run file -- load it over the shell and go. Does not return. */
static uint8_t
cmd_run(uint8_t argc, char **argv)
{
    (void)argc;
    return run_image(argv[1]);
}

/* x -- enter the desktop. The binary ships in /DESKTOP with its support files,
   and the command is cwd-independent so `X` works from the prompt anywhere. */
static uint8_t
cmd_x(uint8_t argc, char **argv)
{
    static char desktop[] = "/DESKTOP/X.BIN";
    (void)argc; (void)argv;
    return run_image(desktop);
}

/* load file [addr] -- put it in memory and come back, for inspection with
   dump. Defaults to the staging area, which is out of everything's way.
 *
 * Deliberately UNGUARDED about where it writes. poke and fill will happily
 * demolish the machine too; this is a bare machine and pretending otherwise
 * would be a lie that costs more than it saves. Loading over the shell is a
 * legitimate thing to want to inspect -- it is exactly what run does. */
static uint8_t
cmd_load(uint8_t argc, char **argv)
{
    static char at[]   = " -> ";
    static char len[]  = ", ";
    static char tail[] = " BYTES\n";
    uint32_t dest = X816_EXEC_STAGE, size;

    if (argc > 2 && !sh_parse_hex(argv[2], &dest))
        return 1;
    if (load_file(argv[1], dest, &size) != 0)
        return 1;

    con_puts(argv[1]);
    con_puts(at);
    sh_put_hex24(dest);
    con_puts(len);
    put_dec32(size);
    con_puts(tail);
    return 0;
}


/* ---- writing ------------------------------------------------------------ */

/* save file addr len -- write a block of memory out as a file.
 *
 * The counterpart to `load`, and the reason the memory commands are worth
 * having: poke something together, save it, and it survives a power cycle.
 * Overwrites without asking, like every other command here. */
static uint8_t
cmd_save(uint8_t argc, char **argv)
{
    static char cantw[]  = " CANNOT WRITE\n";
    static char wrote[]  = " <- ";
    static char len2[]   = ", ";
    static char tail2[]  = " BYTES\n";
    char       path[SH_MAX_LINE];
    fat32_file f;
    uint32_t   addr, count, done = 0;
    uint8_t    buf[64];

    (void)argc;
    if (!fs_ready())
        return 1;
    if (!sh_parse_hex(argv[2], &addr) || !sh_parse_hex(argv[3], &count))
        return 1;
    if (!sh_abspath(argv[1], path))
        return 1;
    if (count == 0)
        return 1;

    if (!fat32_create(path, &f)) {
        con_puts(path);
        con_puts(cantw);
        return 1;
    }

    while (done < count) {
        uint16_t n = (uint16_t)((count - done > sizeof buf)
                                ? sizeof buf : (count - done));
        uint8_t __far *p = far_ptr(addr + done);
        uint16_t i;

        for (i = 0; i < n; i++)
            buf[i] = p[i];
        if (fat32_write(&f, buf, n) != n) {
            con_puts(path);
            con_puts(cantw);
            return 1;
        }
        done += n;
    }

    if (!fat32_close(&f)) {
        con_puts(path);
        con_puts(cantw);
        return 1;
    }

    con_puts(path);
    con_puts(wrote);
    sh_put_hex24(addr);
    con_puts(len2);
    put_dec32(done);
    con_puts(tail2);
    return 0;
}

/* del file -- delete it. No confirmation, matching poke and fill: this is a
   bare machine and a prompt that argues with you is worse than one that does
   what it is told. */
static uint8_t
cmd_del(uint8_t argc, char **argv)
{
    static char cantd[] = " CANNOT DELETE\n";
    char path[SH_MAX_LINE];

    (void)argc;
    if (!fs_ready())
        return 1;
    if (!sh_abspath(argv[1], path))
        return 1;
    if (!fat32_unlink(path)) {
        con_puts(path);
        con_puts(cantd);
        return 1;
    }
    return 0;
}


/* copy src dst -- through a small buffer, because the machine has no room to
   hold a whole file and no need to.
 *
 * The destination is created (and truncated) before anything is read, so
 * copying onto the source is caught by the filesystem rather than producing a
 * file that eats its own tail. */
static uint8_t
cmd_copy(uint8_t argc, char **argv)
{
    static char nosrc[]  = " NOT FOUND\n";
    static char nodst[]  = " CANNOT WRITE\n";
    static char ioerr2[] = " I/O ERROR (COPY INCOMPLETE)\n";
    static char arrow[]  = " -> ";
    static char tail3[]  = " BYTES\n";
    char       src[SH_MAX_LINE], dst[SH_MAX_LINE];
    fat32_file fin, fout;
    uint8_t    buf[64];
    uint16_t   n;
    uint32_t   total = 0;

    (void)argc;
    if (!fs_ready())
        return 1;
    if (!sh_abspath(argv[1], src) || !sh_abspath(argv[2], dst))
        return 1;

    if (!fat32_open(src, &fin)) {
        con_puts(src);
        con_puts(nosrc);
        return 1;
    }
    if (!fat32_create(dst, &fout)) {
        con_puts(dst);
        con_puts(nodst);
        return 1;
    }

    /* A read that stops short of the source's size is EOF only if the device
       did not fail on the way; without checking, a mid-file I/O error would
       "copy" a silently truncated file and report success. */
    fat32_clearerr();
    while ((n = fat32_read(&fin, buf, sizeof buf)) != 0) {
        if (fat32_write(&fout, buf, n) != n) {
            con_puts(dst);
            con_puts(nodst);
            return 1;
        }
        total += n;
    }
    if (fat32_ioerr()) {
        con_puts(src);
        con_puts(ioerr2);
        return 1;
    }

    if (!fat32_close(&fout)) {
        con_puts(dst);
        con_puts(nodst);
        return 1;
    }

    con_puts(src);
    con_puts(arrow);
    con_puts(dst);
    con_putc(' ');
    put_dec32(total);
    con_puts(tail3);
    return 0;
}

/* rename old new -- `new` is a bare name, not a path: this edits the entry in
   place, and moving between directories is a different operation. */
static uint8_t
cmd_rename(uint8_t argc, char **argv)
{
    static char cantr[] = " CANNOT RENAME\n";
    char path[SH_MAX_LINE];

    (void)argc;
    if (!fs_ready())
        return 1;
    if (!sh_abspath(argv[1], path))
        return 1;
    if (!fat32_rename(path, argv[2])) {
        con_puts(path);
        con_puts(cantr);
        return 1;
    }
    return 0;
}


/* mkdir path -- create a directory. */
static uint8_t
cmd_mkdir(uint8_t argc, char **argv)
{
    static char cantm[] = " CANNOT CREATE\n";
    char path[SH_MAX_LINE];

    (void)argc;
    if (!fs_ready())
        return 1;
    if (!sh_abspath(argv[1], path))
        return 1;
    if (!fat32_mkdir(path)) {
        con_puts(path);
        con_puts(cantm);
        return 1;
    }
    return 0;
}

/* rmdir path -- remove an EMPTY directory. The emptiness check lives in the
   filesystem, not here: it is the invariant that stops every file inside being
   stranded, and it should hold for any caller. */
static uint8_t
cmd_rmdir(uint8_t argc, char **argv)
{
    static char cantr2[] = " CANNOT REMOVE (NOT EMPTY?)\n";
    char path[SH_MAX_LINE];

    (void)argc;
    if (!fs_ready())
        return 1;
    if (!sh_abspath(argv[1], path))
        return 1;
    if (!fat32_rmdir(path)) {
        con_puts(path);
        con_puts(cantr2);
        return 1;
    }
    return 0;
}

#ifdef KERNEL_RESIDENT
extern void x816_edit_from_shell_empty(void);
extern void x816_edit_from_shell_path(void);
extern void x816_edit_smoke_empty(void);
extern void x816_edit_smoke_path(void);
extern void x816_edit_smoke_exit_on_x(void);

#define EDIT_SHELL_PATH X816_KDATA_FAR

static bool
edit_copy_arg(const char *arg)
{
    uint8_t i = 0;

    if (!arg)
        return false;
    while (arg[i] && i < SH_MAX_LINE - 1) {
        *far_ptr(EDIT_SHELL_PATH + i) = (uint8_t)arg[i];
        i++;
    }
    *far_ptr(EDIT_SHELL_PATH + i) = 0;
    return true;
}

static uint8_t
cmd_edit(uint8_t argc, char **argv)
{
    /* CLEAR THE SMOKE HAND-OFF BYTES. $0007FE selects a smoke path inside the
       editor (2, 3 and 4 each return instead of running) and $0007FF makes a
       typed 'x' mean Ctrl+X, i.e. quit. Nothing in bank $00 initialises them:
       they are fixed addresses no linker owns, deliberately, and every editsmk
       / edittp / editfl below WRITES one. On the emulator they read back zero
       because its RAM starts zeroed; on the board they are whatever the last
       run -- or power-up -- left there. An ordinary `edit` must state that it
       is not a smoke run rather than inherit the answer. */
    *far_ptr(0x0007FE) = 0;
    *far_ptr(0x0007FF) = 0;

    if (edit_copy_arg(argc > 1 ? argv[1] : 0))
        x816_edit_from_shell_path();
    else
        x816_edit_from_shell_empty();
    return 0;
}

static uint8_t
cmd_editsmk(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    *far_ptr(0x0007FE) = 2;
    x816_edit_smoke_empty();
    return 0;
}

static uint8_t
cmd_editmem(uint8_t argc, char **argv)
{
    uint8_t result;
    uint8_t detail;
    static char ok[] = "EDITMEM OK\n";
    static char fail[] = "EDITMEM FAIL ";

    (void)argc;
    (void)argv;

    *far_ptr(0x0007FE) = 1;
    *far_ptr(0x0007FD) = 0xFF;
    *far_ptr(0x0007FC) = 0;
    x816_edit_smoke_empty();

    result = *far_ptr(0x0007FD);
    detail = *far_ptr(0x0007FC);
    if (result == 0) {
        con_puts(ok);
        return 0;
    }

    con_puts(fail);
    sh_put_hex8(detail);
    con_putc('\n');
    return 1;
}

/* editfl <file> -- round-trip a file through the resident editor.
 *
 * The editor opens the named file into its buffer, holds it on screen long
 * enough for a captured frame, then saves it back out as /EDITOUT.TXT through
 * its own save path. Comparing the two files afterwards is the check that
 * matters: a page walk that reports the right byte count and moves the wrong
 * bytes passes every check made on the editor's own screen. */
static uint8_t
cmd_editfl(uint8_t argc, char **argv)
{
    uint8_t     result;
    static char ok[]   = "EDITFL OK\n";
    static char fail[] = "EDITFL FAIL ";

    *far_ptr(0x0007FE) = 4;
    *far_ptr(0x0007FD) = 0xFF;
    if (edit_copy_arg(argc > 1 ? argv[1] : 0))
        x816_edit_smoke_path();
    else
        x816_edit_smoke_empty();

    result = *far_ptr(0x0007FD);
    if (result == 0) {
        con_puts(ok);
        return 0;
    }
    con_puts(fail);
    sh_put_hex8(result);
    con_putc('\n');
    return 1;
}

static uint8_t
cmd_edittp(uint8_t argc, char **argv)
{
    *far_ptr(0x0007FE) = 3;
    if (edit_copy_arg(argc > 1 ? argv[1] : 0))
        x816_edit_smoke_path();
    else
        x816_edit_smoke_empty();
    if (argc > 1 && *far_ptr(0x0007FB) != 0) {
        static char ok[] = "EDITARG OK\n";
        con_puts(ok);
    }
    return 0;
}
#endif

sh_command sh_commands[] = {
    { "help", "this list",          0, 0, cmd_help },
    { "ver",  "version",            0, 0, cmd_ver  },
    { "cls",  "clear the screen",   0, 0, cmd_cls  },
    { "dump", "dump addr [len]",    1, 2, cmd_dump },
    { "peek", "peek addr",          1, 1, cmd_peek },
    { "poke", "poke addr val",      2, 2, cmd_poke },
    { "fill", "fill addr len val",  3, 3, cmd_fill },
    { "move", "move dst src len",   3, 3, cmd_move },
    { "ls",   "list a directory",    0, 1, cmd_ls   },
    { "dir",  "same as ls",          0, 1, cmd_ls   },
    { "cd",   "change directory",    1, 1, cmd_cd   },
    { "pwd",  "print directory",     0, 0, cmd_pwd  },
    { "mem",  "map; mem release",    0, 1, cmd_mem  },
    { "type", "show a text file",    1, 1, cmd_type },
    { "x",    "desktop",            0, 0, cmd_x    },
    { "run",  "load and run",       1, 1, cmd_run  },
    { "go",   "enter $01:0000",     0, 0, cmd_go },
    { "load", "load file [addr]",     1, 2, cmd_load },
    { "save", "save file addr len",   3, 3, cmd_save },
    { "copy", "copy src dst",         2, 2, cmd_copy },
    { "del",  "delete a file",        1, 1, cmd_del  },
    { "rename", "rename old new",     2, 2, cmd_rename },
    { "mkdir", "make a directory",    1, 1, cmd_mkdir },
    { "rmdir", "remove empty dir",    1, 1, cmd_rmdir },
#ifdef KERNEL_RESIDENT
    { "edit", "edit [file]",           0, 1, cmd_edit },
    { "editsmk", "editor smoke",        0, 0, cmd_editsmk },
    { "editmem", "editor mem smoke",    0, 0, cmd_editmem },
    { "edittp", "editor type smoke",   0, 1, cmd_edittp },
    { "editfl", "editor file smoke",   1, 1, cmd_editfl },
#endif
};

uint8_t
sh_command_count(void)
{
    return (uint8_t)(sizeof sh_commands / sizeof sh_commands[0]);
}

static uint8_t
cmd_help(uint8_t argc, char **argv)
{
    uint8_t i;
    (void)argc; (void)argv;
    for (i = 0; i < sh_command_count(); i++) {
        SH_PUT_HELP(sh_commands[i].name);
        con_gotoxy(8, con_gety());
        SH_PUT_HELP(sh_commands[i].help);
        con_putc('\n');
    }
    return 0;
}

/* ---- dispatch ---------------------------------------------------------- */

/* An unknown word is a PROGRAM NAME before it is a mistake: `test` runs
 * TEST.BIN out of the working directory, which is what everybody types first
 * and what `run test.bin` only ever stood in for.
 *
 * THE EXTENSION IS ADDED, NOT ASSUMED. A word with a dot in it is taken as
 * typed -- `test.bin` is a filename and appending .BIN to it would look for
 * TEST.BIN.BIN -- and a word without one gets ".BIN". That is the whole rule,
 * and it keeps `del` and `type` (which take real filenames) reading the same
 * way as this does.
 *
 * IT MUST STAY LAST, and it must stay quiet about the cases that are not
 * about programs at all:
 *
 *   - the command table wins. A file called LS.BIN does not shadow `ls`,
 *     because a built-in that could be replaced by dropping a file on the
 *     card is a built-in nobody can rely on.
 *   - no card, or no such file, is NOT reported here. It has to come back as
 *     the plain "?" the prompt has always given an unknown word: a typo
 *     answered with "NO CARD" sends the reader to the card slot.
 *
 * Returns 0 if the program ran (in which case this does not return at all),
 * 1 if there IS such a program and it could not be run -- already reported --
 * and SH_NO_PROGRAM if there is no such file, which is the only answer that
 * should print "?".
 */
#define SH_NO_PROGRAM 2
#define SH_EXT_LEN    4                 /* ".BIN", spelled out below */

static uint8_t
try_program(const char *name)
{
    char    cand[SH_MAX_LINE];
    char    path[SH_MAX_LINE];
    bool    isdir;
    bool    dotted = false;
    uint8_t n = 0;

    /* Silent about the card, per above: kfs_ready() mounts it if it can and
       says so without printing, which fs_ready() would not. */
    if (!kfs_ready())
        return SH_NO_PROGRAM;

    while (name[n]) {
        if (n + SH_EXT_LEN + 1 > sizeof cand)   /* room for ".BIN" and the NUL */
            return SH_NO_PROGRAM;
        if (name[n] == '.')
            dotted = true;
        cand[n] = name[n];
        n++;
    }
    /* Spelled out a character at a time rather than held in a `static char
       ext[]`. A string literal cannot be addressed from bank $00 here (see
       shell.h), so it would have to be an initialised array in `data` -- and
       `data` is in the kernel's $2000-$2FFF claim, which the map file says is
       down to single-digit spare bytes. Four stores in the firmware region
       cost nothing that is scarce. */
    if (!dotted) {
        cand[n++] = '.';
        cand[n++] = 'B';
        cand[n++] = 'I';
        cand[n++] = 'N';
    }
    cand[n] = '\0';

    /* Look before loading: load_file reports NOT FOUND, and a typo must not
       get that message when it is going to get "?" anyway. A directory named
       FOO.BIN is not a program either. */
    if (!sh_abspath(cand, path))
        return SH_NO_PROGRAM;
    if (!fat32_stat(path, 0, 0, &isdir) || isdir)
        return SH_NO_PROGRAM;

    return run_image(cand);         /* returns only if it could not */
}

void
sh_exec(char *line)
{
    static char e_many[] = "too many arguments\n";
    static char e_what[] = "?\n";
    static char e_args[] = "wrong number of arguments: ";
    static char e_fail[] = "error\n";
    char   *argv[SH_MAX_ARGS];
    uint8_t argc, i;

    argc = sh_tokenise(line, argv);
    if (argc == SH_TOO_MANY_ARGS) { con_puts(e_many); return; }
    if (argc == 0)                return;          /* a blank line is not an
                                                      error, it is a blank line */

    for (i = 0; i < sh_command_count(); i++) {
        if (!SH_STR_EQ(argv[0], sh_commands[i].name))
            continue;
        if (argc - 1 < sh_commands[i].min_args
            || argc - 1 > sh_commands[i].max_args) {
            con_puts(e_args);
            SH_PUT_HELP(sh_commands[i].help);
            con_putc('\n');
            return;
        }
        if (sh_commands[i].fn(argc, argv))
            con_puts(e_fail);
        return;
    }

    /* No built-in by that name. Extra words are accepted and DROPPED: there
       is no way to hand a program its arguments yet (K_EXEC takes a path and
       nothing else), and refusing `test foo` for a reason the user cannot act
       on is worse than running the program they asked for. */
    switch (try_program(argv[0])) {
    case 0:                             /* ran: this does not come back */
        return;
    case SH_NO_PROGRAM:
        break;
    default:
        con_puts(e_fail);
        return;
    }

    con_puts(argv[0]);
    con_puts(e_what);
}

/* ---- the prompt -------------------------------------------------------- */

void
sh_readline(char *buf, uint8_t size)
{
    uint8_t n = 0;

    for (;;) {
        /* 16-bit: the top byte marks a key with no character (F1, an arrow).
           Those are DISCARDED explicitly below. They used to fall through to
           the store, which inserted the LOW BYTE into the line -- F1 ($0170)
           typed 'p', KEY_LEFT ($014F) typed 'O' -- corrupting the prompt on
           every special key. Discarding is the right default until the line
           editor gives them meanings (that is a separate roadmap item). */
        uint16_t c = con_getc();

        if (c == 0x0D) {                    /* enter */
            buf[n] = '\0';
            con_putc('\n');
            return;
        }
        if (c == 0x08) {                    /* backspace */
            if (n > 0) {
                n--;
                con_putc(0x08);
                con_putc(' ');              /* rub the glyph out... */
                con_putc(0x08);             /* ...and step back over it */
            }
            continue;
        }
        if (c > 0xFF)                       /* KEY_SPECIAL | keynum: a key with
                                               no character. Nothing to insert,
                                               nothing to echo. */
            continue;
        if (n + 1 >= size)                  /* full: drop it rather than
                                               overrun the buffer */
            continue;
        buf[n++] = c;
        con_putc(c);
    }
}

/* The boot mark: the X16 butterfly's wings folded forward -- two chevrons in
 * the family stripes, full blocks in VERA's default palette, with the wordmark
 * centred underneath. X816_core doc/logo/ holds the full-resolution original;
 * this is its 8x8-cell cut, and it is laid out to the original's PROPORTIONS
 * rather than to whatever filled the screen:
 *
 *     mark 9 cells wide by 7 tall, wordmark 4 wide by 1 centred under it -- so
 *     the name is a little under half the mark's width and a seventh of its
 *     height, which is close to what the drawn logo does. An earlier cut was
 *     twelve rows of three-cell arms with the name set BESIDE it, and at 12:1
 *     the four letters read as a caption dropped next to a poster.
 *
 * The indent steps one cell per row, so the arms sit at 45 degrees in square
 * cells -- the angle the drawn chevrons use. Seven rows rather than six
 * because the shape needs travel to read as a chevron at all: six rows leave
 * only two cells of indent against a two-cell arm, and that is a staircase
 * rather than a >. Seven gives three, and an odd count puts the tip on a
 * single row so the point is sharp rather than a two-row flat.
 *
 * The pen is restored afterwards: the logo must not change what colour the
 * prompt prints in.
 *
 * NOT ONE BYTE OF STATIC DATA, and that is the whole shape of this function.
 * The obvious version -- a six-entry colour table, a six-entry indent table and
 * two strings -- is 21 bytes of `zdata`, and the RESIDENT kernel's zdata lives
 * in KernRAM, which was already full. Twenty-one bytes overflowed it and the
 * link failed outright ("Failed to place 1 section fragment"). So the palette
 * is packed into one 32-bit literal (a nibble per stripe, LSB first), the
 * indent is arithmetic, and the glyphs are placed one at a time by code rather
 * than copied from a string. Nothing here needs storing, so nothing is stored.
 *
 * con_putraw rather than con_putrun for the same reason -- a run needs a string
 * to run over. Thirty-two characters at ~90 us is 3 ms, once, at boot. */
static void
sh_banner(void)
{
    /* One nibble per ROW, LSB first, in VERA's default palette:
           red  orange  green  YELLOW  green  blue  purple
       Not the plain rainbow. The colours mirror about the tip -- green sits on
       both sides of the single yellow centre row -- which is a chosen
       arrangement rather than six colours divided into however many rows. The
       previous cut had eight rows and had to double two bands to make six
       colours fit, and an accidental doubling reads as exactly that. */
    uint32_t pal = 0x4657582UL;
    uint16_t r, c;

    for (r = 0; r < 7; r++) {
        /* 0,1,2,3,2,1,0 -- the indent that bends each bar into a chevron,
           turning once, on the single row that is its point */
        uint16_t step = (r < 4) ? r : (uint16_t)(6 - r);

        con_color((uint8_t)((pal >> (r << 2)) & 0x0F), 0);
        for (c = 0; c < 2; c++) {
            con_putraw((uint8_t)((1 + step + c) & 0xFF), (uint8_t)r, 0xDB);
            con_putraw((uint8_t)((5 + step + c) & 0xFF), (uint8_t)r, 0xDB);
        }
    }

    /* The name centred under the mark, which spans columns 1-9. It keeps the
       screen searchable, and the boot conformance scripts read exactly these
       four glyphs -- they look for them ABOVE THE PROMPT, not on a fixed row,
       so this block may be moved without touching them. */
    con_color(1, 0);
    con_putraw(3, 8, 'X');
    con_putraw(4, 8, '8');
    con_putraw(5, 8, '1');
    con_putraw(6, 8, '6');
    con_gotoxy(0, 10);
}

void
sh_run(void)
{
    /* CP437 $AF is the double chevron -- the boot mark's shape, one cell wide,
       so the prompt carries the logo rather than borrowing a '>' from every
       other machine. It is a glyph the font already has; con_putc casts to
       uint8_t before it reaches VERA, so the high bit survives a signed char.
       Note it is NOT '>' any more: anything matching the prompt on screen has
       to look for this. */
    static char prompt[] = "\xAF ";
    char line[SH_MAX_LINE];
    bool returning;

    sh_banner();
    /* Before the first prompt, and after the banner so a machine that comes
       up with nothing to restore looks exactly as it always did. This is the
       other half of run_image's kfs_carry_save: a program launched from
       /GAMES exits back to /GAMES rather than to the root. */
    returning = kfs_carry_pending();
    kfs_carry_restore();
    if (kfs_carry_desktop_resume()) {
        static char desktop[] = "/DESKTOP/X.BIN";
        run_image(desktop);
        con_putc('\n');                 /* launch failed; keep the prompt tidy */
    } else if (!returning && boot_desktop_selected()) {
        static char desktop[] = "/DESKTOP/X.BIN";
        run_image(desktop);
        con_putc('\n');                 /* launch failed; keep the prompt tidy */
    }
    for (;;) {
        con_puts(prompt);
        sh_readline(line, sizeof line);
        sh_exec(line);
    }
}
