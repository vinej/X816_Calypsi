/* desktop.c -- X816 text-mode desktop, launched from the prompt with X.
 *
 * This is the small, sturdy half of the X16 desktop idea: a launcher that is
 * pleasant from the console today, and that can later grow VERA2 wallpaper and
 * mouse handling without changing the launch/return contract.
 */

#include <stdint.h>
#include <stdbool.h>

#include "console.h"
#include "kernel.h"
#include "kfs.h"

#define SCREEN_W 80
#define SCREEN_H 60

#define TILE_W 17
#define TILE_H 7
#define TILE_COLS 4
#define TILE_COUNT 9
#define TILE_FILES 7
#define TILE_EXIT 8

#define BROWSE_MAX 36
#define BROWSE_ROWS 22

typedef struct {
    char    label[12];
    char    detail[16];
    char    path[32];
    uint8_t fg;
    uint8_t bg;
} tile_t;

typedef struct {
    char    name[13];
    uint8_t is_dir;
} entry_t;

static tile_t tiles[TILE_COUNT] = {
    { "KALK",    "SPREADSHEET", "/KALK/KALK.BIN",     1, 3 },
    { "FORTH",   "LANGUAGE",    "/FORTH/FORTH.BIN",   1, 2 },
    { "BASIC",   "LANGUAGE",    "/BASIC/BASIC.BIN",   1, 4 },
    { "CHARMAP", "FONT MAP",    "/DEMO/CHARMAP.BIN",  1, 5 },
    { "KEYSCAN", "KEY CODES",   "/DEMO/KEYSCAN.BIN",  1, 6 },
    { "KERNTEST","KERNEL",      "/DEMO/KERNTEST.BIN", 1, 7 },
    { "MEMTEST", "MEMORY",      "/DEMO/MEMTEST.BIN",  1, 8 },
    { "FILES",   "BROWSER",     "",                   1, 9 },
    { "EXIT",    "CONSOLE",     "",                   1, 2 },
};

static entry_t entries[BROWSE_MAX];
static uint8_t entry_count;
static char browse_path[KFS_PATH] = "/";
static char launch_path[KFS_PATH];
static char launch_dir[KFS_PATH];       /* launch()'s chdir target, see there */
static char dirent_buf[KFS_DIRENT_SIZE];
static char root_path[] = "/";

static uint8_t __far *
far8(uint32_t a)
{
    return (uint8_t __far *)a;
}

static void
resume_set(void)
{
    uint8_t __far *p = far8(KFS_CARRY_BASE + KFS_CARRY_RESUME);
    p[0] = 'X';
    p[1] = 'D';
    p[2] = 'S';
    p[3] = 'K';
}

static void
resume_clear(void)
{
    far8(KFS_CARRY_BASE + KFS_CARRY_RESUME)[0] = 0;
}

static void
k_color(uint8_t fg, uint8_t bg)
{
    kern_c = fg;
    kern_x = bg;
    kern_call(K_CON_COLOR);
}

static void
k_goto(uint8_t x, uint8_t y)
{
    kern_c = x;
    kern_x = y;
    kern_call(K_CON_GOTOXY);
}

static void
k_putc(uint8_t ch)
{
    kern_c = ch;
    kern_call(K_CON_PUTC);
}

static void
k_raw(uint8_t x, uint8_t y, uint8_t ch)
{
    kern_c = x;
    kern_x = y;
    kern_y = ch;
    kern_call(K_CON_PUTRAW);
}

static void
k_puts(char *s)
{
    kern_c = (uint16_t)(unsigned long)s;
    kern_x = 0;
    kern_call(K_CON_PUTS);
}

static void
put_at(uint8_t x, uint8_t y, char *s)
{
    k_goto(x, y);
    k_puts(s);
}

static uint8_t
slen(char *s)
{
    uint8_t n = 0;
    while (s[n])
        n++;
    return n;
}

static bool
streq(char *a, char *b)
{
    uint8_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return false;
        i++;
    }
    return a[i] == b[i];
}

static bool
ends_bin(char *s)
{
    uint8_t n = slen(s);
    if (n < 5)
        return false;
    return s[n - 4] == '.' && s[n - 3] == 'B' && s[n - 2] == 'I'
        && s[n - 1] == 'N';
}

static void
copy_str(char *dst, char *src, uint8_t cap)
{
    uint8_t i = 0;
    if (cap == 0)
        return;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool
make_child_path(char *out, char *base, char *name)
{
    uint8_t i = 0, j = 0;

    while (base[i]) {
        if (i + 1 >= KFS_PATH)
            return false;
        out[i] = base[i];
        i++;
    }
    if (!(i == 1 && out[0] == '/')) {
        if (i + 1 >= KFS_PATH)
            return false;
        out[i++] = '/';
    }
    while (name[j]) {
        if (i + 1 >= KFS_PATH)
            return false;
        out[i++] = name[j++];
    }
    out[i] = 0;
    return true;
}

static void
path_parent(char *path)
{
    uint8_t n = slen(path);

    if (n <= 1) {
        path[0] = '/';
        path[1] = 0;
        return;
    }
    while (n > 1 && path[n - 1] != '/')
        n--;
    if (n <= 1) {
        path[0] = '/';
        path[1] = 0;
    } else {
        path[n - 1] = 0;
    }
}

static void
clear_screen(uint8_t bg)
{
    k_color(1, bg);
    kern_call(K_CON_CLS);
}

static void
draw_box(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool selected)
{
    uint8_t i, j;
    /* THE DOUBLE RULE MARKS THE SELECTION, and nothing else uses it. Every
       tile drawn in double lines makes the screen loud and leaves the
       selected one with no shape of its own; single everywhere else keeps
       the heavy border meaning exactly one thing. */
    uint8_t tl = selected ? 0xC9 : 0xDA;
    uint8_t tr = selected ? 0xBB : 0xBF;
    uint8_t bl = selected ? 0xC8 : 0xC0;
    uint8_t br = selected ? 0xBC : 0xD9;
    uint8_t hz = selected ? 0xCD : 0xC4;
    uint8_t vt = selected ? 0xBA : 0xB3;

    k_raw(x, y, tl);
    k_raw((uint8_t)(x + w - 1), y, tr);
    k_raw(x, (uint8_t)(y + h - 1), bl);
    k_raw((uint8_t)(x + w - 1), (uint8_t)(y + h - 1), br);
    for (i = 1; i + 1 < w; i++) {
        k_raw((uint8_t)(x + i), y, hz);
        k_raw((uint8_t)(x + i), (uint8_t)(y + h - 1), hz);
    }
    for (j = 1; j + 1 < h; j++) {
        k_raw(x, (uint8_t)(y + j), vt);
        k_raw((uint8_t)(x + w - 1), (uint8_t)(y + j), vt);
        for (i = 1; i + 1 < w; i++)
            k_raw((uint8_t)(x + i), (uint8_t)(y + j), ' ');
    }
}

static void
center_text(uint8_t x, uint8_t y, uint8_t w, char *s)
{
    uint8_t n = slen(s);
    uint8_t at = (n >= w) ? x : (uint8_t)(x + ((w - n) >> 1));
    put_at(at, y, s);
}

static void
status(char *s)
{
    uint8_t i;
    k_color(1, 0);
    k_goto(0, 58);
    for (i = 0; i < SCREEN_W; i++)
        k_putc(' ');
    put_at(1, 58, s);
}

static void
chdir_to(char *dir)
{
    kern_c = (uint16_t)(unsigned long)dir;
    kern_x = 0;
    kern_call(K_FS_CHDIR);
}

static void
launch(char *path)
{
    static char msg[] = "LAUNCH FAILED";

    /* THE PROGRAM'S OWN DIRECTORY BECOMES THE WORKING DIRECTORY FIRST.
     *
     * Everything on the card loads its own files by BARE NAME -- Forth
     * includes BASE at boot and INCLUDE TEST/TEST pulls in forty more,
     * SuperBasic does LOAD "BM1.BAS", kalk's /SS writes where it was run --
     * and a bare name resolves against the working directory. That is why
     * the card's own README tells a person at the prompt to `cd /forth`
     * before `run forth.bin`.
     *
     * A tile is that same launch with nobody to type the cd. K_EXEC does park
     * a directory for the new program to adopt (kexec.c), but it parks the one
     * the KERNEL is sitting in -- which is wherever the desktop itself was
     * started from, normally the root. Started that way, Forth stops at its
     * first include and SuperBasic cannot find its programs: a working card
     * that reads as a broken one, which is exactly the failure the README
     * exists to prevent.
     *
     * So chdir to the containing directory before handing over. K_EXEC's
     * kfs_carry_save then parks THAT, and the program adopts it on the way in.
     * The path passed to K_EXEC stays absolute, so this cannot change which
     * file is run -- only where the program that is running thinks it is. */
    copy_str(launch_dir, path, sizeof launch_dir);
    path_parent(launch_dir);
    chdir_to(launch_dir);

    resume_set();
    kern_c = (uint16_t)(unsigned long)path;
    kern_x = 0;
    kern_call(K_EXEC);
    resume_clear();
    status(msg);
}

static void
draw_desktop(uint8_t selected)
{
    static char title[] = "X816 DESKTOP";
    static char keys[] = "ARROWS SELECT   ENTER OPEN   B BROWSE   E EXIT";
    uint8_t i;

    clear_screen(0);
    k_color(1, 0);
    put_at(2, 1, title);
    put_at(2, 3, keys);

    for (i = 0; i < TILE_COUNT; i++) {
        uint8_t col = (uint8_t)(i % TILE_COLS);
        uint8_t row = (uint8_t)(i / TILE_COLS);
        uint8_t x = (uint8_t)(4 + col * 19);
        uint8_t y = (uint8_t)(8 + row * 10);
        bool sel = i == selected;

        /* Selection is REVERSE VIDEO, so the foreground has to move too.
           Every tile's fg is white and the highlight bg is white, so
           highlighting by background alone painted the selected tile white on
           white -- the one tile you are looking at was the one that was not
           there. The box also changes from double to single rule (draw_box's
           `selected`), which is the cue that survives on a mono display. */
        k_color(sel ? 0 : tiles[i].fg, sel ? 1 : tiles[i].bg);
        draw_box(x, y, TILE_W, TILE_H, sel);
        center_text((uint8_t)(x + 1), (uint8_t)(y + 2), TILE_W - 2,
                    tiles[i].label);
        center_text((uint8_t)(x + 1), (uint8_t)(y + 4), TILE_W - 2,
                    tiles[i].detail);
    }
}

static uint16_t
dir_open(char *path)
{
    kern_c = (uint16_t)(unsigned long)path;
    kern_x = 0;
    return kern_call(K_DIR_OPEN);
}

static void
dir_close(uint16_t h)
{
    kern_c = h;
    kern_call(K_DIR_CLOSE);
}

static void
load_entries(void)
{
    uint16_t h;
    uint8_t i;

    entry_count = 0;
    h = dir_open(browse_path);
    if (kern_carry)
        return;

    while (entry_count < BROWSE_MAX) {
        kern_c = h;
        kern_x = (uint16_t)(unsigned long)dirent_buf;
        kern_y = 0;
        kern_call(K_DIR_NEXT);
        if (kern_carry)
            break;

        /* SKIP "." AND "..". FAT gives every subdirectory both, and they are
           directories, so the filter below took them and they landed at the
           TOP of the list -- above whatever you came here to run. Pressing
           Return on the first row then walked into "/KALK/." instead of
           starting anything, which is what "launching from the browser does
           not work" looks like from the outside. "." also grows the path by
           two characters every time, so a few of them overflow KFS_PATH and
           make_child_path starts refusing silently. Backspace is already the
           way up, so ".." was never needed either. */
        if (dirent_buf[0] == '.'
            && (dirent_buf[1] == 0
                || (dirent_buf[1] == '.' && dirent_buf[2] == 0)))
            continue;

        if (dirent_buf[13] || ends_bin(dirent_buf)) {
            for (i = 0; i < 13; i++)
                entries[entry_count].name[i] = dirent_buf[i];
            entries[entry_count].is_dir = (uint8_t)dirent_buf[13];
            entry_count++;
        }
    }
    dir_close(h);
}

static void
draw_browser(uint8_t selected, uint8_t top)
{
    static char title[] = "FILES";
    static char keys[] = "ENTER OPEN   BACKSPACE UP   ESC DESKTOP";
    static char empty[] = "NO PROGRAMS HERE";
    static char dir_tag[] = "[DIR] ";
    static char file_tag[] = "      ";
    uint8_t i;

    clear_screen(0);
    k_color(1, 0);
    put_at(2, 1, title);
    put_at(9, 1, browse_path);
    put_at(2, 3, keys);

    if (entry_count == 0) {
        put_at(4, 8, empty);
        return;
    }

    for (i = 0; i < BROWSE_ROWS && (uint8_t)(top + i) < entry_count; i++) {
        entry_t *e = &entries[top + i];
        uint8_t y = (uint8_t)(7 + i * 2);

        /* Reverse video, both halves -- see draw_desktop. White on white hid
           the selected row here too, and in a list there is no box rule to
           fall back on. */
        k_color((uint8_t)((top + i) == selected ? 0 : 1),
                (uint8_t)((top + i) == selected ? 1 : 0));
        put_at(4, y, e->is_dir ? dir_tag : file_tag);
        put_at(10, y, e->name);
    }
}

static void
browser(void)
{
    uint8_t selected = 0;
    uint8_t top = 0;
    bool dirty = true;

    copy_str(browse_path, root_path, sizeof browse_path);
    load_entries();

    for (;;) {
        uint16_t k;

        if (dirty) {
            draw_browser(selected, top);
            dirty = false;
        }
        k = kern_call(K_CON_GETC);

        if (k == 0x1B)
            return;
        if (k == 0x08) {
            path_parent(browse_path);
            selected = 0;
            top = 0;
            load_entries();
            dirty = true;
            continue;
        }
        if (k == KEY_UP) {
            if (selected > 0)
                selected--;
            if (selected < top)
                top = selected;
            dirty = true;
            continue;
        }
        if (k == KEY_DOWN) {
            if (selected + 1 < entry_count)
                selected++;
            if (selected >= top + BROWSE_ROWS)
                top = (uint8_t)(selected - BROWSE_ROWS + 1);
            dirty = true;
            continue;
        }
        if (k == 0x0D && selected < entry_count) {
            entry_t *e = &entries[selected];
            if (!make_child_path(launch_path, browse_path, e->name))
                continue;
            if (e->is_dir) {
                copy_str(browse_path, launch_path, sizeof browse_path);
                selected = 0;
                top = 0;
                load_entries();
                dirty = true;
            } else {
                launch(launch_path);
                dirty = true;
            }
        }
    }
}

static void
exit_prompt(void)
{
    resume_clear();
    kern_c = 0;
    kern_call(K_EXIT);
    for (;;)
        ;
}

/* Throw away whatever is still in the keyboard buffer.
 *
 * A program is left with a key or two of its own: the Return that chose "quit"
 * from its menu is typed while it is still running, and the SMC queues the
 * events rather than dropping them. The desktop starts fresh after every
 * launch (K_EXIT restarts the prompt, which re-runs X.BIN), so that leftover
 * Return arrives at the tile grid with the same tile still selected -- and
 * relaunches the program the user just left. It looks like the machine
 * refusing to let go.
 *
 * K_CON_GETKEY is the non-blocking read: it returns 0 when nothing is waiting,
 * which is the only way to drain without hanging on an empty buffer. */
static void
drain_keys(void)
{
    while (kern_call(K_CON_GETKEY) != 0)
        ;
}

int
main(void)
{
    uint8_t selected = 0;
    bool dirty = true;

    kern_c = 0;
    kern_call(K_CON_CURSOR);
    drain_keys();

    for (;;) {
        uint16_t k;

        if (dirty) {
            draw_desktop(selected);
            dirty = false;
        }
        k = kern_call(K_CON_GETC);

        if (k == 0x1B) {
            static char msg[] = "USE EXIT TO LEAVE THE DESKTOP";
            status(msg);
            continue;
        }
        if (k == 'e' || k == 'E')
            exit_prompt();
        if (k == 'b' || k == 'B'
            || (selected == TILE_FILES && k == 0x0D)) {
            browser();
            dirty = true;
            continue;
        }
        if (k == KEY_LEFT) {
            if (selected > 0)
                selected--;
            dirty = true;
            continue;
        }
        if (k == KEY_RIGHT) {
            if (selected + 1 < TILE_COUNT)
                selected++;
            dirty = true;
            continue;
        }
        if (k == KEY_UP) {
            if (selected >= TILE_COLS)
                selected = (uint8_t)(selected - TILE_COLS);
            dirty = true;
            continue;
        }
        if (k == KEY_DOWN) {
            if (selected + TILE_COLS < TILE_COUNT)
                selected = (uint8_t)(selected + TILE_COLS);
            dirty = true;
            continue;
        }
        if (k >= '1' && k <= '9') {
            selected = (uint8_t)(k - '1');
            dirty = true;
            continue;
        }
        if (k == 0x0D) {
            if (selected == TILE_EXIT)
                exit_prompt();
            else if (tiles[selected].path[0])
                launch(tiles[selected].path);
            else
                browser();
            dirty = true;
        }
    }
}
