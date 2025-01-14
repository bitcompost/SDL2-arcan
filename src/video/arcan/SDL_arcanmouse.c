/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2016 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_ARCAN

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>

#include "../SDL_sysvideo.h"

#include "SDL_mouse.h"
#include "../../events/SDL_mouse_c.h"
#include "SDL_arcanvideo.h"

#include "SDL_assert.h"

typedef struct {
    int hot_x, hot_y;
    int w, h;
    char cursor_type[64];
    shmif_pixel buffer[0];
} Arcan_CursorData;

static SDL_Cursor* allocCursor(const char* system,
                               SDL_Surface* surf, int hot_x, int hot_y)
{
    SDL_Cursor *cursor = calloc(1, sizeof (*cursor));
    size_t buf_sz = surf ? surf->w * surf->h * sizeof(shmif_pixel) : 0;
    Arcan_CursorData *data = calloc (1, sizeof (Arcan_CursorData) + buf_sz);
    SDL_VideoDevice *vd = SDL_GetVideoDevice();
    Arcan_SDL_Meta *wd = (Arcan_SDL_Meta *) vd->driverdata;

    if (!cursor){
        SDL_OutOfMemory();
        return NULL;
    }

    if (!data){
        SDL_OutOfMemory();
        free(cursor);
        return NULL;
    }

/* don't know if we can handle native cursor or not, try and get one */
    if (surf){
        if (!wd->cursor.addr && !wd->cursor_reject){
            struct arcan_event acqev;
            arcan_shmif_enqueue(&wd->mcont, &(struct arcan_event){
                .ext.kind = ARCAN_EVENT(SEGREQ),
                .ext.segreq.width = surf->w,
                .ext.segreq.height = surf->h,
                .ext.segreq.kind = SEGID_CURSOR,
                .ext.segreq.id = 0xbad1dea
            });
            if (arcan_shmif_acquireloop(&wd->mcont,
                                       &acqev, &wd->pqueue, &wd->pqueue_sz)){
                wd->cursor = arcan_shmif_acquire(&wd->mcont,
                                                 NULL, SEGID_CURSOR, 0);
            }
            else{
                wd->cursor_reject = true;
            }

/* pqueue is flushed as part of the normal event queue */
        }

        if (wd->cursor_reject){
            free(cursor);
            free(data);
            return NULL;
        }

        data->hot_x = hot_x;
        data->hot_y = hot_y;
        data->w = surf->w;
        data->h = surf->h;
    }

    cursor->driverdata = (void *) data;

    if (surf){
        if (0 != SDL_ConvertPixels(surf->w, surf->h,
                                   surf->format->format, surf->pixels,
                                   surf->pitch, wd->format, data->buffer,
                                   surf->w * sizeof(shmif_pixel))){
            free(cursor);
            free(data);
            return NULL;
       }
    }
    else {
        snprintf(data->cursor_type, 64, "%s", system);
    }

    return cursor;
}

static SDL_Cursor *
Arcan_CreateCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
    return allocCursor(NULL, surface, hot_x, hot_y);
}

static SDL_Cursor *
Arcan_CreateSystemCursor(SDL_SystemCursor id)
{
    const char* label = "default";
    switch(id){
    case SDL_SYSTEM_CURSOR_ARROW:
        label = "default";
    break;
    case SDL_SYSTEM_CURSOR_IBEAM:
        label = "typefield";
    break;
    case SDL_SYSTEM_CURSOR_WAIT:
    case SDL_SYSTEM_CURSOR_WAITARROW:
        label = "wait";
    break;
    case SDL_SYSTEM_CURSOR_CROSSHAIR:
        label = "cross";
    break;
    case SDL_SYSTEM_CURSOR_SIZENWSE:
        label = "diag-ll";
    break;
    case SDL_SYSTEM_CURSOR_SIZENESW:
        label = "diag-ur";
    break;
    case SDL_SYSTEM_CURSOR_SIZEWE:
        label = "left-right";
    break;
    case SDL_SYSTEM_CURSOR_SIZENS:
        label = "up-down";
    break;
    case SDL_SYSTEM_CURSOR_SIZEALL:
        label = "move";
    break;
    case SDL_SYSTEM_CURSOR_NO:
        label = "forbidden";
    break;
    case SDL_SYSTEM_CURSOR_HAND:
        label = "hand";
    break;
    default:
    break;
    }

    return allocCursor(label, NULL, 0, 0);
}

static void
Arcan_FreeCursor(SDL_Cursor *cursor)
{
    if (!cursor || !cursor->driverdata)
        return;

    free (cursor->driverdata);
    SDL_free(cursor);
}

static void synchCursor(struct arcan_shmif_cont *dst, Arcan_CursorData *cd)
{
/* anti tearing precaution, being limited to cursor should only happen in
 * rare cases of bugs / abuse */
    while (dst->addr->vready){}

/* FIXME:
 * only synch if the cursor is different from the last one */

    if (cd->w > dst->w || cd->h > dst->h){
        if (!arcan_shmif_resize(dst, cd->w, cd->h))
            return;
        memset(dst->vidb, '\0', dst->h * dst->stride);
    }

/* assumes no padding/alignment for buffer */
    for (size_t y = 0; y < cd->h; y++){
        memcpy(
               &dst->vidp[dst->pitch * y],
               &cd->buffer[y * cd->w],
               sizeof(shmif_pixel) * cd->w
              );
    }

/* FIXME: set hotspot to match */
    arcan_shmif_signal(dst, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
}

static int
Arcan_ShowCursor(SDL_Cursor *cursor)
{
    SDL_VideoDevice *vd = SDL_GetVideoDevice();
    Arcan_SDL_Meta *d = vd->driverdata;

/* need to handle both system-labels and those that require buffer synch */
    if (cursor)
    {
        Arcan_CursorData *cd = cursor->driverdata;
        struct arcan_shmif_cont *dst = &d->mcont;
        struct arcan_event outev = {
            .ext.kind = ARCAN_EVENT(CURSORHINT)
        };
        if (d->cursor.addr){
            if (cd->cursor_type[0] == '\0'){
                synchCursor(&d->cursor, cd);
                return 0;
            }
            dst = &d->cursor;
        }
/* system cursor */
        snprintf((char*)outev.ext.message.data,
            sizeof(outev.ext.message.data)/sizeof(outev.ext.message.data[0]),
            "%s", cd->cursor_type);
        arcan_shmif_enqueue(dst, &outev);
    }
    else
    {
        if (d->cursor.addr){
            arcan_shmif_enqueue(&d->mcont, &(struct arcan_event){
                .ext.kind = ARCAN_EVENT(CURSORHINT),
                .ext.message.data = "hidden"
            });
        }
    }

    return 0;
}

static void hintRelative(struct arcan_shmif_cont* con, SDL_bool rel)
{
    struct arcan_event ev = {
        .ext.kind = ARCAN_EVENT(CURSORHINT)
    };
    sprintf((char*)ev.ext.message.data, rel ? "hidden-rel" : "hidden-abs");
    arcan_shmif_enqueue(con, &ev);
}

static int
Arcan_SetRelativeMouseMode(SDL_bool enabled)
{
    SDL_VideoDevice *vd = SDL_GetVideoDevice();
    Arcan_SDL_Meta *arcan_data;
    if (!vd || !vd->driverdata)
        return SDL_Unsupported();

    arcan_data = vd->driverdata;
    for (size_t i = 0; i < sizeof(arcan_data->windows)/
                           sizeof(arcan_data->windows)[0]; i++){
        if (arcan_data->windows[i].addr)
            hintRelative(&arcan_data->windows[i], enabled);
    }

    hintRelative(&arcan_data->mcont, enabled);
    return 0;
}

static Uint32
Arcan_GetGlobalMouseState(int *x, int *y)
{
    SDL_Mouse *mouse = SDL_GetMouse();
    *x = mouse->last_x;
    *y = mouse->last_y;
    return 0;//mouse->buttonstate;
}

void
Arcan_InitMouse(void)
{
    SDL_Mouse *mouse = SDL_GetMouse();

    mouse->CreateCursor = Arcan_CreateCursor;
    mouse->CreateSystemCursor = Arcan_CreateSystemCursor;
    mouse->ShowCursor = Arcan_ShowCursor;
    mouse->FreeCursor = Arcan_FreeCursor;
    mouse->GetGlobalMouseState = Arcan_GetGlobalMouseState;
}

void
Arcan_FiniMouse(void)
{
    SDL_Mouse *mouse = SDL_GetMouse();

    if (mouse->def_cursor != mouse->cur_cursor)
        Arcan_FreeCursor(mouse->cur_cursor);

    Arcan_FreeCursor (mouse->def_cursor);
    mouse->def_cursor = mouse->cur_cursor = NULL;
    mouse->CreateCursor =  NULL;
    mouse->CreateSystemCursor = NULL;
    mouse->ShowCursor = NULL;
    mouse->FreeCursor = NULL;
    mouse->WarpMouse = NULL;
    mouse->SetRelativeMouseMode = NULL;
}
#endif
/* vi: set ts=4 sw=4 expandtab: */
