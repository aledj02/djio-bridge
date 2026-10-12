// Prueba: reproduce un tono en un dispositivo de salida elegido por nombre (por ejemplo
// BlackHole), sin cambiar la salida predeterminada de la Mac.
// Uso: ./tone_to_device [nombre] [segundos] [hz_canales_1_2] [hz_canales_3_4] [amplitud] [canales]
//   amplitud: 0-1 (por defecto 0.25 = -12 dBFS); canales: máscara como "1010" (por defecto "1111")
//   Con una frecuencia menor a 2 Hz se genera una onda CUADRADA muy lenta (por ejemplo 0.1 Hz =
//   +amplitud durante 5 s y -amplitud durante 5 s). Sirve para medir con un tester en continua
//   la excursión de la salida de un DAC.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE 44100.0
static UInt32 chans;
static double ph1 = 0, ph2 = 0, hz1 = 440, hz2 = 660;
static float amp = 0.25f;
static char mask[5] = "1111";

static CFStringRef findOutputUID(const char *needle) {
    AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, NULL, &size);
    AudioDeviceID *ids = malloc(size);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &size, ids);
    for (UInt32 i = 0; i < size / sizeof(AudioDeviceID); i++) {
        CFStringRef name = NULL; UInt32 sz = sizeof name;
        AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &np, 0, NULL, &sz, &name) || !name) continue;
        char buf[256]; CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(name);
        if (!strcasestr(buf, needle)) continue;
        AudioObjectPropertyAddress cp = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
        AudioObjectGetPropertyDataSize(ids[i], &cp, 0, NULL, &sz);
        AudioBufferList *bl = malloc(sz);
        AudioObjectGetPropertyData(ids[i], &cp, 0, NULL, &sz, bl);
        chans = 0;
        for (UInt32 b = 0; b < bl->mNumberBuffers; b++) chans += bl->mBuffers[b].mNumberChannels;
        free(bl);
        if (!chans) continue;
        CFStringRef uid = NULL; sz = sizeof uid;
        AudioObjectPropertyAddress up = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectGetPropertyData(ids[i], &up, 0, NULL, &sz, &uid);
        printf("Salida: %s (%u canales)\n", buf, (unsigned)chans);
        return uid;
    }
    return NULL;
}

static void cb(void *ud, AudioQueueRef q, AudioQueueBufferRef b) {
    float *p = b->mAudioData;
    UInt32 frames = b->mAudioDataBytesCapacity / (chans * sizeof(float));
    for (UInt32 f = 0; f < frames; f++) {
        float a = (hz1 < 2 ? (sinf(ph1) >= 0 ? 1.0f : -1.0f) : sinf(ph1)) * amp;
        float c = (hz2 < 2 ? (sinf(ph2) >= 0 ? 1.0f : -1.0f) : sinf(ph2)) * amp;
        ph1 += 2 * M_PI * hz1 / RATE; if (ph1 > 2 * M_PI) ph1 -= 2 * M_PI;
        ph2 += 2 * M_PI * hz2 / RATE; if (ph2 > 2 * M_PI) ph2 -= 2 * M_PI;
        for (UInt32 ch = 0; ch < chans; ch++)
            p[f * chans + ch] = (ch < 4 && mask[ch] == '1') ? (ch < 2 ? a : c) : 0;
    }
    b->mAudioDataByteSize = frames * chans * sizeof(float);
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

int main(int argc, char **argv) {
    const char *needle = argc > 1 ? argv[1] : "BlackHole";
    double secs = argc > 2 ? atof(argv[2]) : 5;
    if (argc > 3) hz1 = atof(argv[3]);
    if (argc > 4) hz2 = atof(argv[4]);
    if (argc > 5) amp = atof(argv[5]);
    if (argc > 6) { strncpy(mask, argv[6], 4); for (int i = strlen(mask); i < 4; i++) mask[i] = '0'; }
    CFStringRef uid = findOutputUID(needle);
    if (!uid) { printf("No encuentro la salida \"%s\"\n", needle); return 1; }
    AudioStreamBasicDescription fmt = {
        .mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = chans * 4, .mFramesPerPacket = 1, .mBytesPerFrame = chans * 4,
        .mChannelsPerFrame = chans, .mBitsPerChannel = 32 };
    AudioQueueRef q;
    if (AudioQueueNewOutput(&fmt, cb, NULL, NULL, NULL, 0, &q)) { printf("AudioQueueNewOutput falló\n"); return 1; }
    AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, &uid, sizeof uid);
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b;
        AudioQueueAllocateBuffer(q, 1024 * chans * sizeof(float), &b);
        cb(NULL, q, b);
    }
    AudioQueueStart(q, NULL);
    printf("Tono %.0f Hz en canales 1-2 y %.0f Hz en 3-4 durante %.0f s\n", hz1, hz2, secs);
    usleep((useconds_t)(secs * 1e6));
    AudioQueueStop(q, true);
    AudioQueueDispose(q, true);
    return 0;
}
