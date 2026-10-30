#include <3ds.h>
#include <string.h>

#include "loader.h"

static Thread thread;
static LightLock lock;
static LightEvent wake;
static volatile bool running;
static int box;

static char req_path[512];
static int req_index = -1;
static bool req_new;
static LoadResult result;
static bool result_ready;

static void load(const char *path, LoadResult *r)
{
    tags_read(path, &r->info);
    if (r->info.art)
        art_decode(&r->art, r->info.art, r->info.art_size, box);
    tags_free(&r->info);
}

static void loader_thread(void *arg)
{
    (void)arg;
    while (running) {
        LightEvent_Wait(&wake);

        for (;;) {
            char path[sizeof(req_path)];
            LightLock_Lock(&lock);
            if (!req_new || !running) {
                LightLock_Unlock(&lock);
                break;
            }
            memcpy(path, req_path, sizeof(path));
            int index = req_index;
            req_new = false;
            LightLock_Unlock(&lock);

            LoadResult r = { .index = index };
            load(path, &r);

            LightLock_Lock(&lock);
            if (req_new) {
                art_pixels_free(&r.art);
            } else {
                if (result_ready)
                    art_pixels_free(&result.art);
                result = r;
                result_ready = true;
            }
            LightLock_Unlock(&lock);
        }
    }
}

bool loader_init(int art_box)
{
    box = art_box;
    LightLock_Init(&lock);
    LightEvent_Init(&wake, RESET_ONESHOT);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    running = true;
    thread = threadCreate(loader_thread, NULL, 128 * 1024, prio + 1, -1, false);
    if (!thread)
        running = false;
    return thread != NULL;
}

void loader_exit(void)
{
    if (!thread)
        return;
    running = false;
    LightEvent_Signal(&wake);
    threadJoin(thread, U64_MAX);
    threadFree(thread);
    thread = NULL;

    if (result_ready)
        art_pixels_free(&result.art);
    result_ready = false;
}

void loader_request(int index, const char *path)
{
    LightLock_Lock(&lock);
    snprintf(req_path, sizeof(req_path), "%s", path);
    req_index = index;
    req_new = true;
    if (result_ready) {
        art_pixels_free(&result.art);
        result_ready = false;
    }
    LightLock_Unlock(&lock);
    LightEvent_Signal(&wake);
}

bool loader_poll(LoadResult *out)
{
    LightLock_Lock(&lock);
    bool ready = result_ready;
    if (ready) {
        *out = result;
        result_ready = false;
    }
    LightLock_Unlock(&lock);
    return ready;
}
