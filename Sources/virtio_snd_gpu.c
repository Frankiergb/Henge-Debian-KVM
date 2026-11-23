#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <AudioToolbox/AudioToolbox.h>

#define FB_W 800
#define FB_H 600
static uint8_t *fb_mem = NULL;
static AudioQueueRef audio_q = NULL;

void virtio_snd_gpu_init(void) {
    if (!fb_mem) {
        fb_mem = calloc(FB_W * FB_H * 4, 1);
    }
    if (!audio_q) {
        AudioStreamBasicDescription fmt = {
            .mSampleRate = 48000.0, .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
            .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4,
            .mChannelsPerFrame = 2, .mBitsPerChannel = 16
        };
        AudioQueueNewOutput(&fmt, NULL, NULL, NULL, NULL, 0, &audio_q);
        if (audio_q) AudioQueueStart(audio_q, NULL);
    }
}

void virtio_snd_play_pcm(const uint8_t *pcm, uint32_t len) {
    if (!audio_q || len == 0) return;
    AudioQueueBufferRef buf;
    if (AudioQueueAllocateBuffer(audio_q, len, &buf) == noErr) {
        memcpy(buf->mAudioData, pcm, len);
        buf->mAudioDataByteSize = len;
        AudioQueueEnqueueBuffer(audio_q, buf, 0, NULL);
    }
}

const uint8_t *virtio_get_framebuffer(int *width, int *height) {
    if (width) *width = FB_W;
    if (height) *height = FB_H;
    return fb_mem;
}
