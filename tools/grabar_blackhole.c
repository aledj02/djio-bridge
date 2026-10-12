// Graba lo que llega a un dispositivo de entrada (por defecto BlackHole) a un WAV float32,
// sin cambiar su configuración. Sirve para saber si la distorsión ya viene en el audio de
// origen. Uso: ./grabar_blackhole [segundos] [archivo.wav] [nombre]
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OUTCH 4
static AudioUnit au; static AudioBufferList *abl;
static UInt32 inCh; static float *rec; static size_t cap, nframes;

static OSStatus cb(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *u) {
    abl->mBuffers[0].mDataByteSize = n * inCh * sizeof(float);
    OSStatus e = AudioUnitRender(au, fl, ts, bus, n, abl);
    if (e) return e;
    const float *x = abl->mBuffers[0].mData;
    for (UInt32 i = 0; i < n && nframes < cap; i++, nframes++)
        for (int c = 0; c < OUTCH; c++) rec[nframes * OUTCH + c] = c < (int)inCh ? x[i * inCh + c] : 0;
    return 0;
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 20;
    const char *file = argc > 2 ? argv[2] : "grabacion.wav";
    const char *needle = argc > 3 ? argv[3] : "BlackHole";
    AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, NULL, &size);
    AudioDeviceID *ids = malloc(size), dev = kAudioObjectUnknown;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &size, ids);
    for (UInt32 i = 0; i < size / sizeof(AudioDeviceID) && dev == kAudioObjectUnknown; i++) {
        CFStringRef name = NULL; UInt32 sz = sizeof name;
        AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &np, 0, NULL, &sz, &name) || !name) continue;
        char b[256]; CFStringGetCString(name, b, sizeof b, kCFStringEncodingUTF8); CFRelease(name);
        if (strcasestr(b, needle)) dev = ids[i];
    }
    if (dev == kAudioObjectUnknown) { printf("No encuentro \"%s\"\n", needle); return 1; }

    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    AudioComponentInstanceNew(comp, &au);
    UInt32 on = 1, off = 0;
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof on);
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof off);
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev);
    AudioStreamBasicDescription hw; UInt32 sz = sizeof hw;
    AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &hw, &sz);
    inCh = hw.mChannelsPerFrame; double rate = hw.mSampleRate;
    AudioStreamBasicDescription f = { .mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, .mBytesPerPacket = inCh * 4,
        .mFramesPerPacket = 1, .mBytesPerFrame = inCh * 4, .mChannelsPerFrame = inCh, .mBitsPerChannel = 32 };
    AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f);
    abl = calloc(1, sizeof(AudioBufferList)); abl->mNumberBuffers = 1;
    abl->mBuffers[0].mNumberChannels = inCh; abl->mBuffers[0].mData = calloc(4096 * inCh, sizeof(float));
    cap = (size_t)(secs * rate) + 8192; rec = calloc(cap * OUTCH, sizeof(float));
    AURenderCallbackStruct c = { cb, NULL };
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &c, sizeof c);
    AudioUnitInitialize(au); AudioOutputUnitStart(au);
    printf("Grabando %.0f s de BlackHole (%u canales, %.0f Hz)...\n", secs, (unsigned)inCh, rate);
    usleep((useconds_t)(secs * 1e6));
    AudioOutputUnitStop(au);

    FILE *o = fopen(file, "wb"); uint32_t dataBytes = (uint32_t)(nframes * OUTCH * 4), v;
    uint16_t h;
    fwrite("RIFF", 1, 4, o); v = 36 + dataBytes; fwrite(&v, 4, 1, o); fwrite("WAVEfmt ", 1, 8, o);
    v = 16; fwrite(&v, 4, 1, o); h = 3; fwrite(&h, 2, 1, o); h = OUTCH; fwrite(&h, 2, 1, o);
    v = (uint32_t)rate; fwrite(&v, 4, 1, o); v = (uint32_t)rate * OUTCH * 4; fwrite(&v, 4, 1, o);
    h = OUTCH * 4; fwrite(&h, 2, 1, o); h = 32; fwrite(&h, 2, 1, o);
    fwrite("data", 1, 4, o); fwrite(&dataBytes, 4, 1, o); fwrite(rec, 4, nframes * OUTCH, o); fclose(o);
    printf("Listo: %zu cuadros en %s\n", nframes, file);
    return 0;
}
