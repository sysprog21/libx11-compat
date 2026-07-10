/* Minimal macOS CoreAudio output plugin for XMMS 1.2.x. */

#include "config.h"

#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "xmms/plugin.h"

#define BUFFER_COUNT 4
#define BUFFER_SIZE 32768

static AudioQueueRef queue;
static AudioQueueBufferRef buffers[BUFFER_COUNT];
static int busy[BUFFER_COUNT];
static int current_buffer;
static int bytes_per_second;
static guint64 bytes_written;
static guint64 bytes_played;
static float volume = 1.0f;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static void ca_callback(void *data, AudioQueueRef q, AudioQueueBufferRef buffer)
{
    int i;
    (void) data;
    (void) q;

    pthread_mutex_lock(&lock);
    bytes_played += buffer->mAudioDataByteSize;
    for (i = 0; i < BUFFER_COUNT; i++) {
        if (buffers[i] == buffer) {
            busy[i] = 0;
            break;
        }
    }
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
}

static int ca_open(AFormat fmt, int rate, int nch)
{
    AudioStreamBasicDescription asbd;
    UInt32 bits;
    int i;

    if (fmt != FMT_S16_NE && fmt != FMT_U8)
        return 0;

    bits = (fmt == FMT_U8) ? 8 : 16;
    bytes_per_second = rate * nch * (bits / 8);
    bytes_written = 0;
    bytes_played = 0;
    current_buffer = 0;
    memset(busy, 0, sizeof(busy));
    memset(&asbd, 0, sizeof(asbd));
    asbd.mSampleRate = rate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsPacked;
    if (fmt == FMT_S16_NE)
        asbd.mFormatFlags |=
            kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian;
    asbd.mBitsPerChannel = bits;
    asbd.mChannelsPerFrame = nch;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = nch * (bits / 8);
    asbd.mBytesPerPacket = asbd.mBytesPerFrame;

    if (AudioQueueNewOutput(&asbd, ca_callback, NULL, NULL, NULL, 0, &queue)) {
        /* The out param is undefined on failure; keep queue NULL so the other
         * entry points do not act on a garbage handle.
         */
        queue = NULL;
        return 0;
    }
    for (i = 0; i < BUFFER_COUNT; i++) {
        if (AudioQueueAllocateBuffer(queue, BUFFER_SIZE, &buffers[i])) {
            /* Tear the queue down before bailing: leaving it non-NULL with some
             * buffers[] still NULL would leak it and let a later ca_flush
             * dereference the NULL slots.
             */
            AudioQueueDispose(queue, true);
            queue = NULL;
            return 0;
        }
    }
    AudioQueueSetParameter(queue, kAudioQueueParam_Volume, volume);
    if (AudioQueueStart(queue, NULL) != noErr) {
        /* Started nothing: tear the queue and its buffers down so the plugin
         * does not report a live device on a failed open. XMMS may not call
         * close after an unsuccessful open, so this also avoids leaking them.
         */
        AudioQueueDispose(queue, true);
        queue = NULL;
        return 0;
    }
    return 1;
}

static void ca_write(void *ptr, int length)
{
    char *src = ptr;

    while (length > 0) {
        AudioQueueBufferRef buffer;
        int n;

        pthread_mutex_lock(&lock);
        while (queue && busy[current_buffer])
            pthread_cond_wait(&cond, &lock);
        if (!queue) {
            pthread_mutex_unlock(&lock);
            return;
        }
        int slot = current_buffer;
        buffer = buffers[slot];
        busy[slot] = 1;
        current_buffer = (current_buffer + 1) % BUFFER_COUNT;
        pthread_mutex_unlock(&lock);

        n = length > BUFFER_SIZE ? BUFFER_SIZE : length;
        memcpy(buffer->mAudioData, src, n);
        buffer->mAudioDataByteSize = n;
        /* bytes_written is written here unlocked and bytes_played under the
         * lock in ca_callback; the *_playing/*_time readers load both without
         * the lock. That is safe only because these are naturally aligned
         * 64-bit loads/stores on the arm64/x86-64 targets this plugin builds
         * for. Take the lock here if it is ever ported to a 32-bit ABI.
         */
        if (!AudioQueueEnqueueBuffer(queue, buffer, 0, NULL)) {
            bytes_written += n;
        } else {
            /* The enqueue failed, so ca_callback never fires for this buffer
             * and never clears its busy flag. Release the slot here or ca_write
             * blocks forever once every slot is stuck busy.
             */
            pthread_mutex_lock(&lock);
            busy[slot] = 0;
            pthread_cond_signal(&cond);
            pthread_mutex_unlock(&lock);
        }
        src += n;
        length -= n;
    }
}

static void ca_close(void)
{
    pthread_mutex_lock(&lock);
    if (queue) {
        AudioQueueRef old = queue;
        queue = NULL;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
        AudioQueueStop(old, true);
        AudioQueueDispose(old, true);
    } else
        pthread_mutex_unlock(&lock);
}

static void ca_pause(short paused)
{
    /* Capture the handle under the lock so a concurrent ca_close cannot dispose
     * it between the null check and the AudioQueue call.
     */
    pthread_mutex_lock(&lock);
    AudioQueueRef q = queue;
    pthread_mutex_unlock(&lock);
    if (!q)
        return;
    if (paused)
        AudioQueuePause(q);
    else
        AudioQueueStart(q, NULL);
}

static void ca_flush(int time)
{
    int i;
    guint64 bytes =
        bytes_per_second ? ((guint64) time * bytes_per_second) / 1000 : 0;

    pthread_mutex_lock(&lock);
    AudioQueueRef q = queue;
    bytes_written = bytes;
    bytes_played = bytes;
    memset(busy, 0, sizeof(busy));
    current_buffer = 0;
    /* Clear the byte sizes under the lock, before releasing it: a ca_write
     * racing this reset must not observe a half-cleared slot, and ca_callback
     * reads mAudioDataByteSize under the same lock. The buffers stay valid
     * while queue (captured as q) is non-NULL.
     */
    if (q) {
        for (i = 0; i < BUFFER_COUNT; i++)
            buffers[i]->mAudioDataByteSize = 0;
    }
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    /* Reset outside the lock: AudioQueueReset can invoke the completion
     * callback, which also takes the lock, so holding it here would deadlock.
     * Operate on the captured handle so a concurrent ca_close cannot dispose it
     * mid-call.
     */
    if (q)
        AudioQueueReset(q);
}

static int ca_free(void)
{
    int i, free_bytes = 0;

    pthread_mutex_lock(&lock);
    for (i = 0; i < BUFFER_COUNT; i++)
        if (!busy[i])
            free_bytes += BUFFER_SIZE;
    pthread_mutex_unlock(&lock);
    return free_bytes;
}

static int ca_playing(void)
{
    return queue && bytes_played < bytes_written;
}

static int ca_output_time(void)
{
    return bytes_per_second ? (int) ((bytes_played * 1000) / bytes_per_second)
                            : 0;
}

static int ca_written_time(void)
{
    return bytes_per_second ? (int) ((bytes_written * 1000) / bytes_per_second)
                            : 0;
}

static void ca_get_volume(int *l, int *r)
{
    *l = *r = (int) (volume * 100.0f);
}

static void ca_set_volume(int l, int r)
{
    volume = ((l + r) / 2) / 100.0f;
    if (queue)
        AudioQueueSetParameter(queue, kAudioQueueParam_Volume, volume);
}

static OutputPlugin ca_op = {
    NULL,
    NULL,
    "CoreAudio Output Plugin",
    NULL,
    NULL,
    NULL,
    ca_get_volume,
    ca_set_volume,
    ca_open,
    ca_write,
    ca_close,
    ca_flush,
    ca_pause,
    ca_free,
    ca_playing,
    ca_output_time,
    ca_written_time,
};

OutputPlugin *get_oplugin_info(void)
{
    return &ca_op;
}
