// Numark DJ|iO (15e4:0070) en macOS sin driver: secuencia de streaming del driver oficial
// (Ploytec, "bulk in + isoc out"), reconstruida a partir de su desensamblado.
//
//   IF0 alt 1: EP 0x02 iso OUT (audio, 24 bits LE, 4 canales en orden natural)
//   IF1 alt 1: EP 0x81 iso IN (feedback: byte 0 = muestras por ms)
//              EP 0x86 bulk IN (hay que leerlo siempre, si no el firmware se cuelga)
//
// Uso: ./djio_stream [rate] [segundos_silencio] [segundos_tono] [modo]
//   modo: "full" (por defecto), "noout" (sin audio: solo arranque + lecturas),
//         "noarm" (como noout, sin escribir el estado 0x32)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VID 0x15e4
#define PID 0x0070
#define CH 4
#define BPF (CH * 3)            // bytes por frame
#define UF_PER_REQ 32           // microframes por pedido de salida (4 ms)
#define OUT_INFLIGHT 3
#define FB_ENTRIES 4            // un paquete de feedback por ms
#define FB_INFLIGHT 2
#define BULK_SIZE 32768
#define BULK_INFLIGHT 3

typedef IOUSBInterfaceInterface300 **Intf;

static Intf if0, if1;
static UInt8 pOut, pFb, pBulk;
static double rate = 44100, phase = 0, accum = 0;
static int silenceUf, totalUf, sentUf = 0, outInflight = 0, stopping = 0;
static UInt64 outFrame, fbFrame;
static int fbAdvance = FB_ENTRIES;
static double fbSum = 0; static long fbCount = 0;
static int feedback = 0, fbPackets = 0, fbBad = 0, fbLast[8], fbLastN = 0;
static long bulkBytes = 0; static int bulkReads = 0, bulkErr = 0;
static int outErr = 0;

// ---------- utilidades ----------

static Intf openIntf(io_service_t dev, int num) {
    io_iterator_t it; io_service_t c, s = 0;
    IORegistryEntryGetChildIterator(dev, kIOServicePlane, &it);
    while ((c = IOIteratorNext(it))) {
        CFNumberRef nr = IORegistryEntryCreateCFProperty(c, CFSTR(kUSBInterfaceNumber), NULL, 0);
        int v = -1;
        if (nr) { CFNumberGetValue(nr, kCFNumberIntType, &v); CFRelease(nr); }
        if (v == num) { s = c; break; }
        IOObjectRelease(c);
    }
    if (!s) { printf("No encuentro la interfaz %d\n", num); exit(1); }
    IOCFPlugInInterface **pi; SInt32 sc; Intf i;
    IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &pi, &sc);
    (*pi)->QueryInterface(pi, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&i);
    IOReturn r = (*i)->USBInterfaceOpen(i);
    if (r) { printf("abrir interfaz %d: 0x%08x\n", num, r); exit(1); }
    r = (*i)->SetAlternateInterface(i, 1);
    if (r) { printf("interfaz %d alt 1: 0x%08x\n", num, r); exit(1); }
    CFRunLoopSourceRef src;
    (*i)->CreateInterfaceAsyncEventSource(i, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    return i;
}

static UInt8 findPipe(Intf i, UInt8 epAddr) {
    UInt8 n; (*i)->GetNumEndpoints(i, &n);
    for (UInt8 p = 1; p <= n; p++) {
        UInt8 dir, en, tt, iv; UInt16 mps;
        (*i)->GetPipeProperties(i, p, &dir, &en, &tt, &mps, &iv);
        if ((en | (dir == kUSBIn ? 0x80 : 0)) == epAddr) return p;
    }
    printf("No encuentro el endpoint 0x%02x\n", epAddr); exit(1);
}

static IOReturn ctrl(UInt8 type, UInt8 req, UInt16 val, UInt16 idx, void *buf, UInt16 len, UInt32 *got) {
    IOUSBDevRequestTO r = { type, req, val, idx, len, buf, 0, 100, 500 };
    IOReturn e = (*if0)->ControlRequestTO(if0, 0, &r);
    if (got) *got = r.wLenDone;
    return e;
}

static int alive(void) {
    UInt8 v[8]; UInt32 got = 0;
    return ctrl(0xC0, 'V', 0, 0, v, 5, &got) == kIOReturnSuccess && got == 5;
}

// ---------- EP 0x86: leer siempre ----------

typedef struct { UInt8 buf[BULK_SIZE]; } BulkBuf;
static BulkBuf bulkBufs[BULK_INFLIGHT];
static void bulkSubmit(BulkBuf *b);
static void bulkDone(void *refcon, IOReturn r, void *arg) {
    if (r == kIOReturnSuccess) { bulkReads++; bulkBytes += (long)(uintptr_t)arg; }
    else if (r != kIOReturnAborted) { if (bulkErr++ < 3) printf("  lectura 0x86: 0x%08x\n", r); }
    if (!stopping && r == kIOReturnSuccess) bulkSubmit(refcon);
}
static void bulkSubmit(BulkBuf *b) {
    IOReturn r = (*if1)->ReadPipeAsync(if1, pBulk, b->buf, BULK_SIZE, bulkDone, b);
    if (r && bulkErr++ < 3) printf("  ReadPipeAsync 0x86: 0x%08x\n", r);
}

// ---------- EP 0x81: feedback ----------

typedef struct { UInt8 buf[FB_ENTRIES * 64]; IOUSBIsocFrame fl[FB_ENTRIES]; } FbBuf;
static FbBuf fbBufs[FB_INFLIGHT];
static void fbSubmit(FbBuf *f);
static int fbDoneCount = 0;

static void fbDone(void *refcon, IOReturn r, void *arg) {
    FbBuf *f = refcon;
    if (fbDoneCount++ < 4) {
        printf("  feedback #%d: 0x%08x, bytes por entrada:", fbDoneCount, r);
        for (int k = 0; k < FB_ENTRIES; k++) printf(" %d%s", f->fl[k].frActCount, f->fl[k].frStatus == kIOReturnSuccess ? "" : (f->fl[k].frStatus == kIOReturnUnderrun ? "u" : "!"));
        printf("\n");
    }
    for (int k = 0; k < FB_ENTRIES; k++) {
        int n = f->fl[k].frActCount;
        if (n == 3) {
            int F = f->buf[k * 64];
            fbPackets++;
            for (int j = 0; j < 3; j++) {
                int v = f->buf[k * 64 + j];
                if (fabs(v - rate / 1000) <= 2) { fbSum += v; fbCount++; }
            }
            if (fabs(F - rate / 1000) <= 2) feedback = F;
            if (fbLastN < 8) fbLast[fbLastN++] = F;
        } else if (n) fbBad++;
    }
    if (!stopping && (r == kIOReturnSuccess || r == kIOReturnUnderrun)) fbSubmit(f);
}
static void fbSubmit(FbBuf *f) {
    for (int k = 0; k < FB_ENTRIES; k++) { f->fl[k].frReqCount = 64; f->fl[k].frActCount = 0; f->fl[k].frStatus = 0; }
    IOReturn r = (*if1)->ReadIsochPipeAsync(if1, pFb, f->buf, fbFrame, FB_ENTRIES, f->fl, fbDone, f);
    if (r) { if (fbBad++ < 3) printf("  ReadIsochPipeAsync 0x81: 0x%08x\n", r); return; }
    fbFrame += fbAdvance;
}

// ---------- EP 0x02: audio ----------

typedef struct { UInt8 buf[UF_PER_REQ * 13 * BPF]; IOUSBIsocFrame fl[UF_PER_REQ]; } OutBuf;
static OutBuf outBufs[OUT_INFLIGHT];

// Ritmo de la placa: promedio de todos los bytes de feedback (muestras por ms), como
// numark-djio-linux. Hasta tener suficientes datos se usa la frecuencia nominal.
static double deviceRate(void) {
    if (fbCount < 3000) return rate;
    double r = fbSum / fbCount * 1000.0;
    if (r < rate * 0.995 || r > rate * 1.005) return rate;
    return r;
}

static void fill(OutBuf *o) {
    UInt8 *p = o->buf;
    double dr = deviceRate();
    for (int idx = 0; idx < UF_PER_REQ; idx++) {
        accum += dr / 8000.0;
        int n = (int)accum; accum -= n;
        for (int f = 0; f < n; f++) {
            int32_t s = 0;
            if (sentUf + idx >= silenceUf) {
                s = (int32_t)(sin(phase) * 0.25 * 8388607);   // ~ -12 dBFS
                phase += 2 * M_PI * 440 / rate;
                if (phase > 2 * M_PI) phase -= 2 * M_PI;
            }
            for (int c = 0; c < CH; c++) { *p++ = s; *p++ = s >> 8; *p++ = s >> 16; }
        }
        o->fl[idx].frReqCount = n * BPF; o->fl[idx].frActCount = 0; o->fl[idx].frStatus = 0;
    }
}

static void outSubmit(OutBuf *o);
static int outPktBad = 0, outPktOk = 0; static IOReturn outPktFirstBad = 0;
static void outDone(void *refcon, IOReturn r, void *arg) {
    OutBuf *ob = refcon;
    for (int k = 0; k < UF_PER_REQ; k++) {
        if (ob->fl[k].frStatus == kIOReturnSuccess && ob->fl[k].frActCount == ob->fl[k].frReqCount) outPktOk++;
        else { if (!outPktBad) outPktFirstBad = ob->fl[k].frStatus; outPktBad++; }
    }
    outInflight--;
    if (r != kIOReturnSuccess && outErr++ < 3) printf("  escritura 0x02: 0x%08x\n", r);
    if (!stopping && sentUf < totalUf && r == kIOReturnSuccess) outSubmit(refcon);
    if (outInflight == 0) CFRunLoopStop(CFRunLoopGetCurrent());
}
static void outSubmit(OutBuf *o) {
    fill(o);
    IOReturn r = (*if0)->WriteIsochPipeAsync(if0, pOut, o->buf, outFrame, UF_PER_REQ, o->fl, outDone, o);
    if (r) { if (outErr++ < 3) printf("  WriteIsochPipeAsync: 0x%08x\n", r); return; }
    outFrame += UF_PER_REQ / 8;
    sentUf += UF_PER_REQ;
    outInflight++;
}

// Chequeo periódico: ¿la placa sigue contestando mientras suena?
static void tick(CFRunLoopTimerRef t, void *info) {
    printf("  [%.1f s] ritmo estimado %.1f Hz (%ld muestras de feedback), 0x86: %ld bytes\n",
           sentUf / 8000.0, fbCount ? fbSum / fbCount * 1000.0 : 0, fbCount, bulkBytes);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) rate = atof(argv[1]);
    double sil = argc > 2 ? atof(argv[2]) : 1.5, tone = argc > 3 ? atof(argv[3]) : 3;
    const char *mode = argc > 4 ? argv[4] : "full";
    if (argc > 5) fbAdvance = atoi(argv[5]);   // frames (ms) que avanza cada lectura de feedback
    int noOut = strcmp(mode, "full") != 0, noArm = strcmp(mode, "noarm") == 0;
    silenceUf = (int)(sil * 8000); totalUf = (int)((sil + tone) * 8000);

    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    int vid = VID, pid = PID;
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), CFNumberCreate(NULL, kCFNumberIntType, &vid));
    CFDictionarySetValue(m, CFSTR(kUSBProductID), CFNumberCreate(NULL, kCFNumberIntType, &pid));
    io_service_t dev = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!dev) { printf("No encuentro la DJ|iO\n"); return 1; }

    printf("DJ|iO: %.0f Hz, %.1f s de silencio y %.1f s de tono\n", rate, sil, tone);
    if0 = openIntf(dev, 0);
    if1 = openIntf(dev, 1);
    pOut = findPipe(if0, 0x02); pFb = findPipe(if1, 0x81); pBulk = findPipe(if1, 0x86);

    // Clear halt en 0x02 y 0x86 ANTES de fijar la frecuencia (como numark-djio-linux).
    (*if0)->ClearPipeStallBothEnds(if0, pOut);
    (*if1)->ClearPipeStallBothEnds(if1, pBulk);

    // Secuencia de arranque de numark-djio-linux (clon de una captura del driver de Mac).
    UInt8 b[16]; UInt32 got = 0; IOReturn e;
    UInt32 hz = (UInt32)rate;
    UInt8 rl[3] = { hz & 0xff, (hz >> 8) & 0xff, (hz >> 16) & 0xff };
    e = ctrl(0xC0, 'V', 0, 0, b, 8, &got);
    printf("  1 V: 0x%08x [%u] %02x %02x %02x %02x\n", e, (unsigned)got, b[0], b[1], b[2], b[3]);
    if (e) { printf("La placa no contesta (reconectala)\n"); return 1; }
    e = ctrl(0xC0, 'V', 0, 0, b, 8, &got);           printf("  1 V: 0x%08x\n", e);
    e = ctrl(0xC0, 'I', 0, 0, b, 4, &got);           printf("  5 I: 0x%08x [%u] %02x\n", e, (unsigned)got, b[0]);
    e = ctrl(0xA2, 0x81, 0x0100, 0, b, 3, &got);     printf("  6 GET_CUR(0): 0x%08x [%u] %02x %02x %02x\n", e, (unsigned)got, b[0], b[1], b[2]);
    e = ctrl(0x22, 0x01, 0x0100, 0x86, rl, 3, NULL); printf("  7 SET_CUR 0x86: 0x%08x\n", e);
    e = ctrl(0x22, 0x01, 0x0100, 0x02, rl, 3, NULL); printf("  7 SET_CUR 0x02: 0x%08x\n", e);
    e = ctrl(0xA2, 0x81, 0x0100, 0x86, b, 3, &got); printf("  8 GET_CUR(0x86): 0x%08x [%u] %02x %02x %02x\n", e, (unsigned)got, b[0], b[1], b[2]);
    e = ctrl(0xC0, 'I', 0, 0, b, 4, &got);           printf("  9 I: 0x%08x [%u] %02x\n", e, (unsigned)got, b[0]);
    e = noArm ? kIOReturnSuccess : ctrl(0x40, 'I', 0x0032, 0, NULL, 0, NULL);
    printf("  9 I <- 0x32: 0x%08x\n", e);
    usleep(1000);

    // Feedback, audio y por último las lecturas de 0x86 (orden de numark-djio-linux).
    AbsoluteTime at; UInt64 now;
    (*if0)->GetBusFrameNumber(if0, &now, &at);
    fbFrame = outFrame = now + 50;
    for (int k = 0; k < FB_INFLIGHT; k++) fbSubmit(&fbBufs[k]);
    if (!noOut) for (int k = 0; k < OUT_INFLIGHT; k++) outSubmit(&outBufs[k]);
    for (int k = 0; k < BULK_INFLIGHT; k++) bulkSubmit(&bulkBufs[k]);

    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 1, 0, 0, tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
    if (noOut) CFRunLoopRunInMode(kCFRunLoopDefaultMode, sil + tone, false);
    else if (outInflight) CFRunLoopRun();
    CFRunLoopTimerInvalidate(t);

    // Apagado como el driver oficial: abortar pipes y volver a alt 0.
    stopping = 1;
    (*if0)->AbortPipe(if0, pOut);
    (*if1)->AbortPipe(if1, pFb);
    (*if1)->AbortPipe(if1, pBulk);
    usleep(100000);
    printf("Feedback (primeros):");
    for (int k = 0; k < fbLastN; k++) printf(" %d", fbLast[k]);
    printf("\nPaquetes de audio: %d OK, %d con problema (primer estado 0x%08x)", outPktOk, outPktBad, outPktFirstBad);
    printf("\nFin: %d microframes enviados, errores salida %d, feedback raros %d, control al final %s\n",
           sentUf, outErr, fbBad, alive() ? "OK" : "SIN RESPUESTA");
    (*if0)->SetAlternateInterface(if0, 0); (*if1)->SetAlternateInterface(if1, 0);
    (*if0)->USBInterfaceClose(if0); (*if1)->USBInterfaceClose(if1);
    return 0;
}
