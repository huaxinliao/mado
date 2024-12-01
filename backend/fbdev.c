/*
 * Twin - A Tiny Window System
 * Copyright (c) 2024 National Cheng Kung University, Taiwan
 * All rights reserved.
 */

#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <twin.h>
#include <unistd.h>

#include "linux_input.h"
#include "linux_vt.h"
#include "twin_backend.h"
#include "twin_private.h"

#define FBDEV_NAME "FRAMEBUFFER"
#define FBDEV_DEFAULT "/dev/fb0"
#define SCREEN(x) ((twin_context_t *) x)->screen
#define PRIV(x) ((twin_fbdev_t *) ((twin_context_t *) x)->priv)

typedef struct {
    twin_screen_t *screen;

    /* Linux input system */
    void *input;

    /* Linux virtual terminal (VT) */
    int vt_fd;
    int vx_fd;
    int vt_swsig;
    int old_kbmode;
    struct termios old_tio;
    bool vt_active;

    /* Linux framebuffer */
    int fb_fd;
    struct fb_var_screeninfo fb_var;
    struct fb_fix_screeninfo fb_fix;
    uint16_t cmap[3][256];
    uint8_t *fb_base;
    size_t fb_len;
} twin_fbdev_t;

/* color conversion */
#define ARGB32_TO_RGB565_PERLINE(dest, pixels, width)   \
    do {                                                \
        for (int i = 0; i < width; i++)                 \
            dest[i] = ((pixels[i] & 0x00f80000) >> 8) | \
                      ((pixels[i] & 0x0000fc00) >> 5) | \
                      ((pixels[i] & 0x000000f8) >> 3);  \
    } while (0)

/* Requires validation in 24-bit per pixel environments */
#define ARGB32_TO_RGB888_PERLINE(dest, pixels, width) \
    do {                                              \
        for (int i = 0; i < width; i++)               \
            dest[i] = 0xff000000 | pixels[i];         \
    } while (0)

#define ARGB32_TO_ARGB32_PERLINE(dest, pixels, width) \
    memcpy(dest, pixels, width * sizeof(*dest))

#define FBDEV_PUT_SPAN_IMPL(bpp, op)                                     \
    static void _twin_fbdev_put_span##bpp(                               \
        twin_coord_t left, twin_coord_t top, twin_coord_t right,         \
        twin_argb32_t *pixels, void *closure)                            \
    {                                                                    \
        uint32_t *dest;                                                  \
        twin_fbdev_t *tx = PRIV(closure);                                \
        off_t off = sizeof(*dest) * left + top * tx->fb_fix.line_length; \
        dest = (uint32_t *) ((uintptr_t) tx->fb_base + off);             \
        twin_coord_t width = right - left;                               \
        op(dest, pixels, width);                                         \
    }

FBDEV_PUT_SPAN_IMPL(16, ARGB32_TO_RGB565_PERLINE)
FBDEV_PUT_SPAN_IMPL(24, ARGB32_TO_RGB888_PERLINE)
FBDEV_PUT_SPAN_IMPL(32, ARGB32_TO_ARGB32_PERLINE)

static void twin_fbdev_get_screen_size(twin_fbdev_t *tx,
                                       int *width,
                                       int *height)
{
    struct fb_var_screeninfo info;
    ioctl(tx->fb_fd, FBIOGET_VSCREENINFO, &info);
    *width = info.xres;
    *height = info.yres;
}

static void twin_fbdev_damage(twin_screen_t *screen, twin_fbdev_t *tx)
{
    int width, height;
    twin_fbdev_get_screen_size(tx, &width, &height);
    twin_screen_damage(tx->screen, 0, 0, width, height);
}

static void twin_fbdev_damaged(void *closure)
{
    twin_screen_t *screen = SCREEN(closure);
    twin_fbdev_t *tx = PRIV(closure);

    /* VT switch is ready */
    if (tx->vt_active && screen && twin_screen_damaged(screen))
        twin_screen_update(screen);
}

static inline bool twin_fbdev_is_rgb565(twin_fbdev_t *tx)
{
    return tx->fb_var.red.offset == 11 && tx->fb_var.red.length == 5 &&
           tx->fb_var.green.offset == 5 && tx->fb_var.green.length == 6 &&
           tx->fb_var.blue.offset == 0 && tx->fb_var.blue.length == 5;
}

static inline bool twin_fbdev_is_rgb888(twin_fbdev_t *tx)
{
    return tx->fb_var.red.offset == 16 && tx->fb_var.red.length == 8 &&
           tx->fb_var.green.offset == 8 && tx->fb_var.green.length == 8 &&
           tx->fb_var.blue.offset == 0 && tx->fb_var.blue.length == 8;
}

static inline bool twin_fbdev_is_argb32(twin_fbdev_t *tx)
{
    return tx->fb_var.red.offset == 16 && tx->fb_var.red.length == 8 &&
           tx->fb_var.green.offset == 8 && tx->fb_var.green.length == 8 &&
           tx->fb_var.blue.offset == 0 && tx->fb_var.blue.length == 8;
}

static bool twin_fbdev_apply_config(twin_fbdev_t *tx)
{
    /* Read changable information of the framebuffer */
    if (ioctl(tx->fb_fd, FBIOGET_VSCREENINFO, &tx->fb_var) == -1) {
        log_error("Failed to get framebuffer information");
        return false;
    }

    /* Set the virtual screen size to be the same as the physical screen */
    tx->fb_var.xres_virtual = tx->fb_var.xres;
    tx->fb_var.yres_virtual = tx->fb_var.yres;
    if (ioctl(tx->fb_fd, FBIOPUT_VSCREENINFO, &tx->fb_var) < 0) {
        log_error("Failed to set framebuffer mode");
        return false;
    }

    /* Read changable information of the framebuffer again */
    if (ioctl(tx->fb_fd, FBIOGET_VSCREENINFO, &tx->fb_var) < 0) {
        log_error("Failed to get framebuffer information");
        return false;
    }

    /* Examine the framebuffer format */
    switch (tx->fb_var.bits_per_pixel) {
    case 16: /* RGB565 */
        if (!twin_fbdev_is_rgb565(tx)) {
            log_error("Invalid framebuffer format for 16 bpp");
            return false;
        }
        break;
    case 24: /* RGB888 */
        if (!twin_fbdev_is_rgb888(tx)) {
            log_error("Invalid framebuffer format for 24 bpp");
            return false;
        }
        break;
    case 32: /* ARGB32 */
        if (!twin_fbdev_is_argb32(tx)) {
            log_error("Invalid framebuffer format for 32 bpp");
            return false;
        }
        break;
    default:
        log_error("Unsupported bits per pixel: %d", tx->fb_var.bits_per_pixel);
        break;
    }

    /* Read unchangable information of the framebuffer */
    ioctl(tx->fb_fd, FBIOGET_FSCREENINFO, &tx->fb_fix);

    /* Align the framebuffer memory address with the page size */
    off_t pgsize = getpagesize();
    off_t start = (off_t) tx->fb_fix.smem_start & (pgsize - 1);

    /* Round up the framebuffer memory size to match the page size */
    tx->fb_len = start + (size_t) tx->fb_fix.smem_len + (pgsize - 1);
    tx->fb_len &= ~(pgsize - 1);

    /* Map framebuffer device to the virtual memory */
    tx->fb_base = mmap(NULL, tx->fb_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                       tx->fb_fd, 0);
    if (tx->fb_base == MAP_FAILED) {
        log_error("Failed to mmap framebuffer");
        return false;
    }

    return true;
}

static void twin_fbdev_switch(twin_fbdev_t *tx, int activate)
{
    tx->vt_active = activate;

    /* Upon activation */
    if (activate) {
        /* Switch complete */
        ioctl(tx->vt_fd, VT_RELDISP, VT_ACKACQ);

        /* Restore fbdev settings */
        if (twin_fbdev_apply_config(tx)) {
            tx->vt_active = true;

            /* Mark entire screen for refresh */
            if (tx->screen)
                twin_screen_damage(tx->screen, 0, 0, tx->screen->width,
                                   tx->screen->height);
        }
    } else {
        /* FIXME: should expose some option to disallow them */
        ioctl(tx->vt_fd, VT_RELDISP, 1);

        tx->vt_active = false;

        if (tx->fb_base != MAP_FAILED) {
            munmap(tx->fb_base, tx->fb_len);
            tx->fb_base = MAP_FAILED;
        }
    }
}

static bool vt_switch_pending;

static bool twin_fbdev_work(void *closure)
{
    twin_screen_t *screen = SCREEN(closure);
    twin_fbdev_t *tx = PRIV(closure);

    if (twin_screen_damaged(screen))
        twin_screen_update(screen);

    if (vt_switch_pending) {
        twin_fbdev_switch(tx, !tx->vt_active);
        vt_switch_pending = false;
    }

    return true;
}

static void twin_fbdev_vtswitch(int sig)
{
    signal(sig, twin_fbdev_vtswitch);
    vt_switch_pending = true;
}

static bool twin_fbdev_setup_vt(twin_fbdev_t *tx, int switch_sig)
{
    struct vt_mode vtm;
    struct termios tio;

    /* Retrieve current VT mode */
    if (ioctl(tx->vt_fd, VT_GETMODE, &vtm) < 0) {
        log_info("can't get VT mode");
        return 0;
    }
    /* Set VT mode to process mode, with specified signal for switching */
    vtm.mode = VT_PROCESS;
    vtm.relsig = switch_sig;
    vtm.acqsig = switch_sig;

    signal(switch_sig, twin_fbdev_vtswitch);
    tx->vt_swsig = switch_sig;

    /* Apply VT mode settings */
    if (ioctl(tx->vt_fd, VT_SETMODE, &vtm) < 0) {
        log_info("can't set VT mode");
        signal(switch_sig, SIG_IGN);
        return 0;
    }

    /* Save and configure terminal settings */
    tcgetattr(tx->vt_fd, &tx->old_tio);

    ioctl(tx->vt_fd, KDGKBMODE, &tx->old_kbmode);
    ioctl(tx->vt_fd, KDSKBMODE, K_MEDIUMRAW);

    tio = tx->old_tio;
    tio.c_iflag = (IGNPAR | IGNBRK) & (~PARMRK) & (~ISTRIP);
    tio.c_oflag = 0;
    tio.c_cflag = CREAD | CS8;
    tio.c_lflag = 0;
    tio.c_cc[VTIME] = 0;
    tio.c_cc[VMIN] = 1;
    cfsetispeed(&tio, 9600);
    cfsetospeed(&tio, 9600);
    tcsetattr(tx->vt_fd, TCSANOW, &tio);

    /* Set virtual console to graphics mode */
    ioctl(tx->vx_fd, KDSETMODE, KD_GRAPHICS);

    return true;
}

twin_context_t *twin_fbdev_init(int width, int height)
{
    char *fbdev_path = getenv(FBDEV_NAME);
    if (!fbdev_path) {
        log_info("Environment variable $FRAMEBUFFER not set, use %s by default",
                 FBDEV_DEFAULT);
        fbdev_path = FBDEV_DEFAULT;
    }

    twin_context_t *ctx = calloc(1, sizeof(twin_context_t));
    if (!ctx)
        return NULL;
    ctx->priv = calloc(1, sizeof(twin_fbdev_t));
    if (!ctx->priv)
        return NULL;

    twin_fbdev_t *tx = ctx->priv;

    /* Open the framebuffer device */
    tx->fb_fd = open(fbdev_path, O_RDWR);
    if (tx->fb_fd == -1) {
        log_error("Failed to open %s", fbdev_path);
        goto bail;
    }

    /* Set up virtual terminal environment */
    if (!twin_vt_setup(&tx->vt_fd)) {
        goto bail_fb_fd;
    }

    if (!twin_fbdev_setup_vt(tx, SIGUSR1)) {
        goto bail_vt_fd;
    }

    /* Apply configurations to the framebuffer device */
    if (!twin_fbdev_apply_config(tx)) {
        log_error("Failed to apply configurations to the framebuffer device");
        goto bail_vt_fd;
    }

    /* Examine if framebuffer mapping is valid */
    if (tx->fb_base == MAP_FAILED) {
        log_error("Failed to map framebuffer memory");
        return;
    }

    const twin_put_span_t fbdev_put_spans[] = {
        _twin_fbdev_put_span16,
        _twin_fbdev_put_span24,
        _twin_fbdev_put_span32,
    };
    /* Create TWIN screen */
    ctx->screen = twin_screen_create(
        width, height, NULL, fbdev_put_spans[tx->fb_var.bits_per_pixel / 8 - 2],
        ctx);

    /* Create Linux input system object */
    tx->input = twin_linux_input_create(ctx->screen);
    if (!tx->input) {
        log_error("Failed to create Linux input system object");
        goto bail_screen;
    }

    /* Setup file handler and work functions */
    twin_set_work(twin_fbdev_work, TWIN_WORK_REDISPLAY, ctx);

    /* Enable immediate refresh */
    twin_screen_register_damaged(ctx->screen, twin_fbdev_damaged, ctx);
    return ctx;

bail_screen:
    twin_screen_destroy(ctx->screen);
bail_vt_fd:
    close(tx->vt_fd);
bail_fb_fd:
    close(tx->fb_fd);
bail:
    free(ctx->priv);
    free(ctx);
    return NULL;
}

static void twin_fbdev_configure(twin_context_t *ctx)
{
    int width, height;
    twin_fbdev_t *tx = ctx->priv;
    twin_fbdev_get_screen_size(tx, &width, &height);
    twin_screen_resize(ctx->screen, width, height);
}

static void twin_fbdev_exit(twin_context_t *ctx)
{
    if (!ctx)
        return;

    twin_fbdev_t *tx = PRIV(ctx);
    twin_vt_mode(tx->vt_fd, KD_TEXT);
    munmap(tx->fb_base, tx->fb_len);
    twin_linux_input_destroy(tx->input);
    close(tx->vt_fd);
    close(tx->fb_fd);
    free(ctx->priv);
    free(ctx);
}

/* Register the Linux framebuffer backend */

const twin_backend_t g_twin_backend = {
    .init = twin_fbdev_init,
    .configure = twin_fbdev_configure,
    .exit = twin_fbdev_exit,
};
