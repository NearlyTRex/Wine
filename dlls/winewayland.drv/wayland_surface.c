/*
 * Wayland surfaces
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdlib.h>
#include <unistd.h>

#include "waylanddrv.h"
#include "wine/debug.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static void xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface,
                                         uint32_t serial)
{
    struct wayland_surface *surface;
    BOOL initial_configure = FALSE;
    HWND hwnd = data;

    TRACE("serial=%u\n", serial);

    if (!(surface = wayland_surface_lock_hwnd(hwnd))) return;

    /* Handle this event only if wayland_surface is still associated with
     * the target xdg_surface. */
    if (surface->xdg_surface == xdg_surface)
    {
        /* If we have a previously requested config, we have already sent a
         * WM_WAYLAND_CONFIGURE which hasn't been handled yet. In that case,
         * avoid sending another message to reduce message queue traffic. */
        BOOL should_post = surface->requested.serial == 0;
        initial_configure = surface->current.serial == 0;
        surface->pending.serial = serial;
        surface->requested = surface->pending;
        memset(&surface->pending, 0, sizeof(surface->pending));
        if (should_post) NtUserPostMessage(hwnd, WM_WAYLAND_CONFIGURE, 0, 0);
    }

    pthread_mutex_unlock(&surface->mutex);

    /* Flush the window surface in case there is content that we weren't
     * able to flush before due to the lack of the initial configure. */
    if (initial_configure) wayland_window_flush(hwnd);
}

static const struct xdg_surface_listener xdg_surface_listener =
{
    xdg_surface_handle_configure
};

static void xdg_toplevel_handle_configure(void *data,
                                          struct xdg_toplevel *xdg_toplevel,
                                          int32_t width, int32_t height,
                                          struct wl_array *states)
{
    struct wayland_surface *surface;
    HWND hwnd = data;
    uint32_t *state;
    enum wayland_surface_config_state config_state = 0;

    wl_array_for_each(state, states)
    {
        switch(*state)
        {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_RESIZING;
            break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_TILED;
            break;
        default:
            break;
        }
    }

    TRACE("hwnd=%p %dx%d,%#x\n", hwnd, width, height, config_state);

    if (!(surface = wayland_surface_lock_hwnd(hwnd))) return;

    if (surface->xdg_toplevel == xdg_toplevel)
    {
        surface->pending.width = width;
        surface->pending.height = height;
        surface->pending.state = config_state;
    }

    pthread_mutex_unlock(&surface->mutex);
}

static void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    NtUserPostMessage((HWND)data, WM_SYSCOMMAND, SC_CLOSE, 0);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener =
{
    xdg_toplevel_handle_configure,
    xdg_toplevel_handle_close
};

/**********************************************************************
 *          wayland_surface_create
 *
 * Creates a role-less wayland surface.
 */
struct wayland_surface *wayland_surface_create(HWND hwnd)
{
    struct wayland_surface *surface;

    surface = calloc(1, sizeof(*surface));
    if (!surface)
    {
        ERR("Failed to allocate space for Wayland surface\n");
        goto err;
    }

    TRACE("surface=%p\n", surface);

    pthread_mutex_init(&surface->mutex, NULL);

    surface->hwnd = hwnd;
    surface->wl_surface = wl_compositor_create_surface(process_wayland.wl_compositor);
    if (!surface->wl_surface)
    {
        ERR("Failed to create wl_surface Wayland surface\n");
        goto err;
    }
    wl_surface_set_user_data(surface->wl_surface, hwnd);

    return surface;

err:
    if (surface) wayland_surface_destroy(surface);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_destroy
 *
 * Destroys a wayland surface.
 */
void wayland_surface_destroy(struct wayland_surface *surface)
{
    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.focused_hwnd == surface->hwnd)
    {
        process_wayland.pointer.focused_hwnd = NULL;
        process_wayland.pointer.enter_serial = 0;
    }
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    pthread_mutex_lock(&surface->mutex);

    if (surface->xdg_toplevel)
    {
        xdg_toplevel_destroy(surface->xdg_toplevel);
        surface->xdg_toplevel = NULL;
    }

    if (surface->xdg_surface)
    {
        xdg_surface_destroy(surface->xdg_surface);
        surface->xdg_surface = NULL;
    }

    if (surface->wl_surface)
    {
        wl_surface_destroy(surface->wl_surface);
        surface->wl_surface = NULL;
    }

    pthread_mutex_unlock(&surface->mutex);

    if (surface->latest_window_buffer)
        wayland_shm_buffer_unref(surface->latest_window_buffer);

    wl_display_flush(process_wayland.wl_display);

    pthread_mutex_destroy(&surface->mutex);

    free(surface);
}

/**********************************************************************
 *          wayland_surface_make_toplevel
 *
 * Gives the toplevel role to a plain wayland surface.
 */
void wayland_surface_make_toplevel(struct wayland_surface *surface)
{
    TRACE("surface=%p\n", surface);

    surface->xdg_surface =
        xdg_wm_base_get_xdg_surface(process_wayland.xdg_wm_base, surface->wl_surface);
    if (!surface->xdg_surface) goto err;
    xdg_surface_add_listener(surface->xdg_surface, &xdg_surface_listener, surface->hwnd);

    surface->xdg_toplevel = xdg_surface_get_toplevel(surface->xdg_surface);
    if (!surface->xdg_toplevel) goto err;
    xdg_toplevel_add_listener(surface->xdg_toplevel, &xdg_toplevel_listener, surface->hwnd);

    wl_surface_commit(surface->wl_surface);
    wl_display_flush(process_wayland.wl_display);

    return;

err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign toplevel role to wayland surface\n");
}

/**********************************************************************
 *          wayland_surface_clear_role
 *
 * Clears the role related Wayland objects of a Wayland surface, making it a
 * plain surface again. We can later assign the same role (but not a
 * different one!) to the surface.
 */
void wayland_surface_clear_role(struct wayland_surface *surface)
{
    TRACE("surface=%p\n", surface);

    if (surface->xdg_toplevel)
    {
        xdg_toplevel_destroy(surface->xdg_toplevel);
        surface->xdg_toplevel = NULL;
    }

    if (surface->xdg_surface)
    {
        xdg_surface_destroy(surface->xdg_surface);
        surface->xdg_surface = NULL;
    }

    memset(&surface->pending, 0, sizeof(surface->pending));
    memset(&surface->requested, 0, sizeof(surface->requested));
    memset(&surface->processing, 0, sizeof(surface->processing));
    memset(&surface->current, 0, sizeof(surface->current));

    /* Ensure no buffer is attached, otherwise future role assignments may fail. */
    wl_surface_attach(surface->wl_surface, NULL, 0, 0);
    wl_surface_commit(surface->wl_surface);

    wl_display_flush(process_wayland.wl_display);
}

/**********************************************************************
 *          wayland_surface_attach_shm
 *
 * Attaches a SHM buffer to a wayland surface.
 *
 * The buffer is marked as unavailable until committed and subsequently
 * released by the compositor.
 */
void wayland_surface_attach_shm(struct wayland_surface *surface,
                                struct wayland_shm_buffer *shm_buffer,
                                HRGN surface_damage_region)
{
    RGNDATA *surface_damage;

    TRACE("surface=%p shm_buffer=%p (%dx%d)\n",
          surface, shm_buffer, shm_buffer->width, shm_buffer->height);

    shm_buffer->busy = TRUE;
    wayland_shm_buffer_ref(shm_buffer);

    wl_surface_attach(surface->wl_surface, shm_buffer->wl_buffer, 0, 0);

    /* Add surface damage, i.e., which parts of the surface have changed since
     * the last surface commit. Note that this is different from the buffer
     * damage region. */
    surface_damage = get_region_data(surface_damage_region);
    if (surface_damage)
    {
        RECT *rgn_rect = (RECT *)surface_damage->Buffer;
        RECT *rgn_rect_end = rgn_rect + surface_damage->rdh.nCount;

        for (;rgn_rect < rgn_rect_end; rgn_rect++)
        {
            wl_surface_damage_buffer(surface->wl_surface,
                                     rgn_rect->left, rgn_rect->top,
                                     rgn_rect->right - rgn_rect->left,
                                     rgn_rect->bottom - rgn_rect->top);
        }
        free(surface_damage);
    }
}

/**********************************************************************
 *          wayland_surface_configure_is_compatible
 *
 * Checks whether a wayland_surface_configure object is compatible with the
 * the provided arguments.
 */
static BOOL wayland_surface_configure_is_compatible(struct wayland_surface_config *conf,
                                                    int width, int height,
                                                    enum wayland_surface_config_state state)
{
    static enum wayland_surface_config_state mask =
        WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;

    /* We require the same state. */
    if ((state & mask) != (conf->state & mask)) return FALSE;

    /* The maximized state requires the configured size. */
    if ((conf->state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
        (width != conf->width || height != conf->height))
    {
        return FALSE;
    }

    return TRUE;
}

/**********************************************************************
 *          wayland_surface_reconfigure
 *
 * Reconfigures the wayland surface as needed to match the latest requested
 * state.
 */
BOOL wayland_surface_reconfigure(struct wayland_surface *surface)
{
    struct wayland_window_config *window = &surface->window;
    int width, height;

    if (!surface->xdg_toplevel) return TRUE;

    width = surface->window.rect.right - surface->window.rect.left;
    height = surface->window.rect.bottom - surface->window.rect.top;

    TRACE("hwnd=%p window=%dx%d,%#x processing=%dx%d,%#x current=%dx%d,%#x\n",
          surface->hwnd, width, height, window->state,
          surface->processing.width, surface->processing.height,
          surface->processing.state, surface->current.width,
          surface->current.height, surface->current.state);

    /* Acknowledge any compatible processed config. */
    if (surface->processing.serial && surface->processing.processed &&
        wayland_surface_configure_is_compatible(&surface->processing,
                                                width, height,
                                                window->state))
    {
        surface->current = surface->processing;
        memset(&surface->processing, 0, sizeof(surface->processing));
        xdg_surface_ack_configure(surface->xdg_surface, surface->current.serial);
    }
    /* If this is the initial configure, and we have a compatible requested
     * config, use that, in order to draw windows that don't go through the
     * message loop (e.g., some splash screens). */
    else if (!surface->current.serial && surface->requested.serial &&
             wayland_surface_configure_is_compatible(&surface->requested,
                                                     width, height,
                                                     window->state))
    {
        surface->current = surface->requested;
        memset(&surface->requested, 0, sizeof(surface->requested));
        xdg_surface_ack_configure(surface->xdg_surface, surface->current.serial);
    }
    else if (!surface->current.serial ||
             !wayland_surface_configure_is_compatible(&surface->current,
                                                      width, height,
                                                      window->state))
    {
        return FALSE;
    }

    return TRUE;
}

/**********************************************************************
 *          wayland_shm_buffer_ref
 *
 * Increases the reference count of a SHM buffer.
 */
void wayland_shm_buffer_ref(struct wayland_shm_buffer *shm_buffer)
{
    InterlockedIncrement(&shm_buffer->ref);
}

/**********************************************************************
 *          wayland_shm_buffer_unref
 *
 * Decreases the reference count of a SHM buffer (and may destroy it).
 */
void wayland_shm_buffer_unref(struct wayland_shm_buffer *shm_buffer)
{
    if (InterlockedDecrement(&shm_buffer->ref) > 0) return;

    TRACE("destroying %p map=%p\n", shm_buffer, shm_buffer->map_data);

    if (shm_buffer->wl_buffer)
        wl_buffer_destroy(shm_buffer->wl_buffer);
    if (shm_buffer->map_data)
        NtUnmapViewOfSection(GetCurrentProcess(), shm_buffer->map_data);
    if (shm_buffer->damage_region)
        NtGdiDeleteObjectApp(shm_buffer->damage_region);

    free(shm_buffer);
}

/**********************************************************************
 *          wayland_shm_buffer_create
 *
 * Creates a SHM buffer with the specified width, height and format.
 */
struct wayland_shm_buffer *wayland_shm_buffer_create(int width, int height,
                                                     enum wl_shm_format format)
{
    struct wayland_shm_buffer *shm_buffer = NULL;
    HANDLE handle = 0;
    int fd = -1;
    SIZE_T view_size = 0;
    LARGE_INTEGER section_size;
    NTSTATUS status;
    struct wl_shm_pool *pool;
    int stride, size;

    stride = width * WINEWAYLAND_BYTES_PER_PIXEL;
    size = stride * height;
    if (size == 0)
    {
        ERR("Invalid shm_buffer size %dx%d\n", width, height);
        goto err;
    }

    shm_buffer = calloc(1, sizeof(*shm_buffer));
    if (!shm_buffer)
    {
        ERR("Failed to allocate space for SHM buffer\n");
        goto err;
    }

    TRACE("%p %dx%d format=%d size=%d\n", shm_buffer, width, height, format, size);

    shm_buffer->ref = 1;
    shm_buffer->width = width;
    shm_buffer->height = height;
    shm_buffer->map_size = size;

    shm_buffer->damage_region = NtGdiCreateRectRgn(0, 0, width, height);
    if (!shm_buffer->damage_region)
    {
        ERR("Failed to create buffer damage region\n");
        goto err;
    }

    section_size.QuadPart = size;
    status = NtCreateSection(&handle,
                             GENERIC_READ | SECTION_MAP_READ | SECTION_MAP_WRITE,
                             NULL, &section_size, PAGE_READWRITE, SEC_COMMIT, 0);
    if (status)
    {
        ERR("Failed to create SHM section status=0x%lx\n", (long)status);
        goto err;
    }

    status = NtMapViewOfSection(handle, GetCurrentProcess(),
                                (PVOID)&shm_buffer->map_data, 0, 0, NULL,
                                &view_size, ViewUnmap, 0, PAGE_READWRITE);
    if (status)
    {
        shm_buffer->map_data = NULL;
        ERR("Failed to create map SHM handle status=0x%lx\n", (long)status);
        goto err;
    }

    status = wine_server_handle_to_fd(handle, FILE_READ_DATA, &fd, NULL);
    if (status)
    {
        ERR("Failed to get fd from SHM handle status=0x%lx\n", (long)status);
        goto err;
    }

    pool = wl_shm_create_pool(process_wayland.wl_shm, fd, size);
    if (!pool)
    {
        ERR("Failed to create SHM pool fd=%d size=%d\n", fd, size);
        goto err;
    }
    shm_buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
                                                      stride, format);
    wl_shm_pool_destroy(pool);
    if (!shm_buffer->wl_buffer)
    {
        ERR("Failed to create SHM buffer %dx%d\n", width, height);
        goto err;
    }

    close(fd);
    NtClose(handle);

    TRACE("=> map=%p\n", shm_buffer->map_data);

    return shm_buffer;

err:
    if (fd >= 0) close(fd);
    if (handle) NtClose(handle);
    if (shm_buffer) wayland_shm_buffer_unref(shm_buffer);
    return NULL;
}
