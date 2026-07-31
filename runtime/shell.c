/* The X816 boot prompt. See shell.h and X816_Core doc/SHELL.md. BUILD AT -O0. */

#include "shell.h"
#include "console.h"
#include "fat32.h"

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
            con_putc((c >= 0x20 && c <= 0x5F) ? (char)c : '.');
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

/* Absolute, always starts with '/', and never ends with one except at the
   root. Holding it normalised means every consumer can just concatenate. */
static char cwd[SH_MAX_LINE] = { '/', '\0' };
static bool fs_mounted;

static bool
fs_ready(void)
{
    static char nocard[] = "NO CARD\n";
    if (fs_mounted)
        return true;
    if (fat32_mount()) {
        fs_mounted = true;
        return true;
    }
    con_puts(nocard);
    return false;
}

static uint8_t
str_len(const char *s)
{
    uint8_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Resolve `arg` against the working directory into `out` (SH_MAX_LINE bytes).
 *
 * Handles absolute paths, "." and "..", and repeated or trailing slashes. The
 * result is always normalised, so ".." at the root stays at the root rather
 * than escaping above it -- a path that walks off the top of the tree is the
 * classic way a shell ends up reading something it should not. */
static bool
sh_abspath(const char *arg, char *out)
{
    uint8_t n = 0;
    const char *p = arg;

    if (*p == '/') {
        out[n++] = '/';
        p++;
    } else {
        uint8_t i = 0;
        while (cwd[i] && n < SH_MAX_LINE - 1)
            out[n++] = cwd[i++];
    }

    while (*p) {
        const char *seg;
        uint8_t seglen = 0;

        while (*p == '/')
            p++;
        if (!*p)
            break;

        seg = p;
        while (*p && *p != '/') {
            p++;
            seglen++;
        }

        if (seglen == 1 && seg[0] == '.')
            continue;                             /* "." changes nothing */

        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n > 1 && out[n - 1] != '/')    /* drop the last component */
                n--;
            if (n > 1)
                n--;                              /* and its separator */
            continue;
        }

        if (n > 1 && n < SH_MAX_LINE - 1)
            out[n++] = '/';
        while (seglen--) {
            if (n >= SH_MAX_LINE - 1)
                return false;                     /* too long: refuse */
            out[n++] = *seg++;
        }
    }

    if (n == 0)
        out[n++] = '/';
    out[n] = '\0';
    return true;
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

static uint8_t
cmd_cd(uint8_t argc, char **argv)
{
    static char notdir[] = " NOT A DIRECTORY\n";
    char     path[SH_MAX_LINE];
    uint32_t clus;
    bool     isdir;
    uint8_t  i;

    (void)argc;
    if (!fs_ready())
        return 1;
    if (!sh_abspath(argv[1], path))
        return 1;

    /* The root always exists and has no entry to stat. */
    if (!(path[0] == '/' && path[1] == '\0')) {
        if (!fat32_stat(path, &clus, 0, &isdir) || !isdir) {
            con_puts(argv[1]);
            con_puts(notdir);
            return 1;
        }
    }

    for (i = 0; path[i] && i < SH_MAX_LINE - 1; i++)
        cwd[i] = path[i];
    cwd[i] = '\0';
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
    return 0;
}

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
    { "cd",   "change directory",    1, 1, cmd_cd   },
    { "pwd",  "print directory",     0, 0, cmd_pwd  },
    { "type", "show a text file",    1, 1, cmd_type },
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
        con_puts(sh_commands[i].name);
        con_gotoxy(8, con_gety());
        con_puts(sh_commands[i].help);
        con_putc('\n');
    }
    return 0;
}

/* ---- dispatch ---------------------------------------------------------- */

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
        if (!str_eq(argv[0], sh_commands[i].name))
            continue;
        if (argc - 1 < sh_commands[i].min_args
            || argc - 1 > sh_commands[i].max_args) {
            con_puts(e_args);
            con_puts(sh_commands[i].help);
            con_putc('\n');
            return;
        }
        if (sh_commands[i].fn(argc, argv))
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
        char c = con_getc();

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
        if (n + 1 >= size)                  /* full: drop it rather than
                                               overrun the buffer */
            continue;
        buf[n++] = c;
        con_putc(c);
    }
}

void
sh_run(void)
{
    static char banner[] = "X816\n";
    static char prompt[] = "> ";
    char line[SH_MAX_LINE];

    con_puts(banner);
    for (;;) {
        con_puts(prompt);
        sh_readline(line, sizeof line);
        sh_exec(line);
    }
}
