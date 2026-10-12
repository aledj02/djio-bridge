// djio_bridge: usa una Numark DJ|iO original (USB 15e4:0070) en macOS sin driver.
//
// Toma los canales 1-4 de una placa virtual (BlackHole) y los manda a la DJ|iO:
//   canales 1-2 -> salida 1 (máster), canales 3-4 -> salida 2 (auriculares).
//
// Protocolo USB (de github.com/djpallax/numark-djio-linux, captura del driver de Mac):
//   IF0 alt 1: EP 0x02 iso OUT, audio S24_3LE, 4 canales, 44,1 kHz únicamente
//   IF1 alt 1: EP 0x81 iso IN, ritmo de la placa (bytes ~44/45 = muestras por ms)
//              EP 0x86 bulk IN, hay que leerlo siempre (el contenido se descarta)
//
// Uso: ./djio_bridge [--esperar] [--ganancia1 dB] [--ganancia2 dB] [nombre_entrada] [latencia_ms]
//   --esperar:      si la placa no está, esperar a que se enchufe (modo servicio)
//   --ganancia1 dB: corrección de nivel de la salida 1 (por defecto GANANCIA_SALIDA1_DB)
//   --ganancia2 dB: corrección de nivel de la salida 2 (por defecto GANANCIA_SALIDA2_DB)
//   nombre_entrada: parte del nombre del dispositivo de entrada (por defecto "BlackHole")
//   latencia_ms:    colchón de audio entre la Mac y la placa (por defecto 25)
// Ctrl+C para salir.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <pthread/qos.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#define VID 0x15e4
#define PID 0x0070
#define RATE 44100.0
#define CH 4
#define BPF (CH * 3)            // bytes por frame en USB
#define UF_PER_REQ 32           // microframes por pedido de salida (4 ms)
#define OUT_INFLIGHT 5          // pedidos de 4 ms en vuelo (20 ms de margen ante demoras del sistema)
#define FB_ENTRIES 4            // un paquete de ritmo por ms
#define FB_INFLIGHT 2
#define BULK_SIZE 32768
#define BULK_INFLIGHT 3
#define RING_FRAMES 16384       // potencia de 2

// ===================== AJUSTES DE NIVEL POR SALIDA ============================
// Valen 0.0 (sin ajuste) desde el 2026-10-11. Antes, la salida 2 de esta DJ|iO sonaba
// unos 24 dB más baja que la 1 y se compensaba con GANANCIA_SALIDA1_DB = -24.
// Causa: capacitores electrolíticos de acople de 10 µF degradados en el camino de la
// salida 2 (entre el DAC y el operacional, y entre el operacional y la RCA). Se
// reemplazaron y los niveles quedaron parejos. Ver Hardware.md.
// Los parámetros se dejan por si hiciera falta compensar algo en el futuro
// (también por línea de comandos: --ganancia1 dB --ganancia2 dB).
#define GANANCIA_SALIDA1_DB 0.0
#define GANANCIA_SALIDA2_DB 0.0
// ==============================================================================

typedef IOUSBInterfaceInterface300 **Intf;

static int waitMode = 0;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
    if (waitMode) {   // en el registro del servicio, con fecha y hora
        time_t now = time(NULL); char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S ", localtime(&now));
        fputs(ts, stdout);
    }
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}

static io_service_t findDJiO(void) {
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    int vid = VID, pid = PID;
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), CFNumberCreate(NULL, kCFNumberIntType, &vid));
    CFDictionarySetValue(m, CFSTR(kUSBProductID), CFNumberCreate(NULL, kCFNumberIntType, &pid));
    return IOServiceGetMatchingService(kIOMainPortDefault, m);
}

// Si la placa quedó trabada, reintentar no sirve: en modo servicio se espera a que la
// desenchufen antes de salir (launchd vuelve a lanzar el puente y espera la reconexión).
static void fail(void) __attribute__((noreturn));
static void fail(void) {
    if (waitMode) {
        logmsg("Esperando a que desenchufes y vuelvas a enchufar la DJ|iO...\n");
        io_service_t d;
        while ((d = findDJiO())) { IOObjectRelease(d); sleep(1); }
    }
    exit(1);
}

// ---------- buffer circular (productor: CoreAudio, consumidor: USB) ----------

static float ring[RING_FRAMES * CH];
static _Atomic uint64_t ringWrite = 0, ringRead = 0;
static _Atomic long overflows = 0;

static void ringPush(const float *in, UInt32 frames, UInt32 stride) {
    uint64_t w = atomic_load_explicit(&ringWrite, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&ringRead, memory_order_acquire);
    if (w - r + frames > RING_FRAMES) { atomic_fetch_add(&overflows, 1); return; }
    for (UInt32 i = 0; i < frames; i++) {
        float *dst = &ring[((w + i) & (RING_FRAMES - 1)) * CH];
        for (int c = 0; c < CH; c++) dst[c] = c < (int)stride ? in[i * stride + c] : 0.0f;
    }
    atomic_store_explicit(&ringWrite, w + frames, memory_order_release);
}

// ---------- USB ----------

static Intf if0, if1;
static UInt8 pOut, pFb, pBulk;
static UInt64 outFrame, fbFrame;
static int outInflight = 0;
static volatile int stopping = 0;
static double fbSum = 0; static long fbCount = 0;

// ---- Lazo cerrado con el reloj de la placa (como el driver oficial) ----
// Cada milisegundo la placa informa cuántas muestras consumió (byte 0 del paquete de ritmo; los
// bytes 1 y 2 repiten los dos milisegundos anteriores). Se le manda EXACTAMENTE esa cantidad por
// milisegundo, con un retraso fijo: así su memoria interna nunca se desfasa. (Con un promedio
// fijo, la fase entre los dos patrones 44/45 y 5/6 se corre de a poco y la memoria de la placa
// se acerca a vaciarse o desbordarse: la distorsión crece y se corrige sola.)
static int closedLoop = 1;                       // --promedio vuelve al modo anterior
#define FBQ 512
#define FBQ_MAX 64                               // si se acumulan más, se descartan los más viejos
#define FBQ_LAG 8                                // ms de retraso entre el ritmo de la placa y lo que se manda
static uint8_t fbq[FBQ];
static unsigned fbqHead = 0, fbqTail = 0;
static long fbqEmpty = 0, fbqDrop = 0;
static unsigned fbqMin = 1000, fbqMax = 0;       // nivel de la cola desde el último informe
static UInt64 fbLastFrame = 0; static int fbHave = 0;
static double nomAccum = 0;
static unsigned fbqCount(void) { return fbqTail - fbqHead; }
static void fbqPush(int v) {
    if (fbqCount() >= FBQ_MAX) { fbqHead++; fbqDrop++; }
    fbq[fbqTail++ % FBQ] = (uint8_t)v;
}
static double deviceRate(void);
static int nominalMs(void) {                    // conteo de reserva con el ritmo promedio
    nomAccum += deviceRate() / 1000.0;
    int n = (int)nomAccum; nomAccum -= n; return n;
}
static int fbValid(int v) { return v >= 42 && v <= 46; }
static int fbFix(int v) { return fbValid(v) ? v : nominalMs(); }
static long usbErrors = 0, outFrameErrors = 0;
static IOReturn errBulk = 0, errFb = 0, errFrame = 0;   // último código de error de cada tipo
static int bulkConsec = 0, fbConsec = 0;                // errores seguidos (para rendirse si no se recupera)

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
    if (!s) { logmsg("No encuentro la interfaz %d de la DJ|iO\n", num); fail(); }
    IOCFPlugInInterface **pi; SInt32 sc; Intf i;
    IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &pi, &sc);
    (*pi)->QueryInterface(pi, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&i);
    IOReturn r = (*i)->USBInterfaceOpen(i);
    if (r) {
        logmsg("No pude abrir la interfaz %d (0x%08x). ¿Hay otro programa usando la placa?\n", num, r);
        if (waitMode) sleep(10);   // ocupada: reintentar más tarde, no esperar la reconexión
        exit(1);
    }
    r = (*i)->SetAlternateInterface(i, 1);
    if (r) { logmsg("La placa no responde (0x%08x): desenchufala y volvé a enchufarla.\n", r); fail(); }
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
    logmsg("No encuentro el endpoint 0x%02x\n", epAddr); fail();
}

static IOReturn ctrl(UInt8 type, UInt8 req, UInt16 val, UInt16 idx, void *buf, UInt16 len, UInt32 *got) {
    IOUSBDevRequestTO r = { type, req, val, idx, len, buf, 0, 100, 500 };
    IOReturn e = (*if0)->ControlRequestTO(if0, 0, &r);
    if (got) *got = r.wLenDone;
    return e;
}

// EP 0x86: se lee y se descarta.
typedef struct { UInt8 buf[BULK_SIZE]; } BulkBuf;
static BulkBuf bulkBufs[BULK_INFLIGHT];
static void bulkSubmit(BulkBuf *b);
static void bulkDone(void *refcon, IOReturn r, void *arg) {
    if (stopping || r == kIOReturnAborted) return;
    if (r == kIOReturnSuccess) { bulkConsec = 0; bulkSubmit(refcon); return; }
    // Un error NO puede dejar el canal sin leer: la placa se porta mal si no se lo drena.
    usbErrors++; errBulk = r;
    if (++bulkConsec > 50) { stopping = 1; return; }         // no se recupera: cerrar
    (*if1)->ClearPipeStallBothEnds(if1, pBulk);
    bulkSubmit(refcon);
}
static void bulkSubmit(BulkBuf *b) {
    IOReturn r = (*if1)->ReadPipeAsync(if1, pBulk, b->buf, BULK_SIZE, bulkDone, b);
    if (r) {
        usbErrors++; errBulk = r;
        if (!stopping && ++bulkConsec <= 50)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ bulkSubmit(b); });
    }
}

// EP 0x81: ritmo de la placa.
typedef struct { UInt8 buf[FB_ENTRIES * 64]; IOUSBIsocFrame fl[FB_ENTRIES]; UInt64 frame0; } FbBuf;
static FbBuf fbBufs[FB_INFLIGHT];
static void fbSubmit(FbBuf *f);
static void fbDone(void *refcon, IOReturn r, void *arg) {
    FbBuf *f = refcon;
    for (int k = 0; k < FB_ENTRIES; k++) {
        if (f->fl[k].frActCount != 3) continue;
        int b0 = f->buf[k * 64], b1 = f->buf[k * 64 + 1], b2 = f->buf[k * 64 + 2];
        for (int j = 0; j < 3; j++) {
            int v = f->buf[k * 64 + j];
            if (v >= 42 && v <= 46) { fbSum += v; fbCount++; }
        }
        UInt64 fr = f->frame0 + k;
        if (!fbHave) { fbqPush(fbFix(b0)); }
        else if (fr > fbLastFrame) {
            UInt64 gap = fr - fbLastFrame;           // normalmente 1; si faltaron paquetes se recuperan de b1 y b2
            if (gap > 3) for (UInt64 g = 3; g < gap; g++) fbqPush(nominalMs());
            if (gap >= 3) fbqPush(fbFix(b2));
            if (gap >= 2) fbqPush(fbFix(b1));
            fbqPush(fbFix(b0));
        }
        if (!fbHave || fr > fbLastFrame) { fbLastFrame = fr; fbHave = 1; }
    }
    if (stopping || r == kIOReturnAborted) return;
    if (r == kIOReturnSuccess || r == kIOReturnUnderrun) { fbConsec = 0; fbSubmit(f); return; }
    usbErrors++; errFb = r;
    if (++fbConsec > 50) { stopping = 1; return; }
    AbsoluteTime at; UInt64 now;
    (*if1)->GetBusFrameNumber(if1, &now, &at);
    if (fbFrame < now + 2) fbFrame = now + 10;               // reprogramar hacia adelante
    fbSubmit(f);
}
static void fbSubmit(FbBuf *f) {
    for (int k = 0; k < FB_ENTRIES; k++) { f->fl[k].frReqCount = 64; f->fl[k].frActCount = 0; f->fl[k].frStatus = 0; }
    f->frame0 = fbFrame;
    IOReturn r = (*if1)->ReadIsochPipeAsync(if1, pFb, f->buf, fbFrame, FB_ENTRIES, f->fl, fbDone, f);
    if (r) {
        usbErrors++; errFb = r;
        if (!stopping && ++fbConsec <= 50)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                AbsoluteTime at; UInt64 now; (*if1)->GetBusFrameNumber(if1, &now, &at);
                if (fbFrame < now + 2) fbFrame = now + 10;
                fbSubmit(f); });
        return;
    }
    fbFrame += FB_ENTRIES;
}

// Ritmo real de la placa: promedio de todas las lecturas (≈44102 Hz), como numark-djio-linux.
static double ppmBias = 0;   // ajuste fino del ritmo hacia la placa, en partes por millón (--ppm)
static double deviceRate(void) {
    if (fbCount < 3000) return RATE;
    double r = fbSum / fbCount * 1000.0;
    return ((r > RATE * 0.995 && r < RATE * 1.005) ? r : RATE) * (1.0 + ppmBias * 1e-6);
}

// ---------- de buffer circular a USB, con ajuste fino de velocidad ----------
// La Mac y la placa tienen relojes distintos: el colchón se mantiene cerca del objetivo
// remuestreando muy levemente (±0,1 %, inaudible) con interpolación lineal.

static float gain1 = 1.0f;   // ganancia lineal de los canales 1-2 (ver GANANCIA_SALIDA1_DB)
static float gain2 = 1.0f;   // ganancia lineal de los canales 3-4 (ver GANANCIA_SALIDA2_DB)

// Limitador suave: transparente hasta 0,8 y curva hacia 1,0 sin recortar de golpe.
static inline float softLimit(float x) {
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.2f * tanhf((a - 0.8f) / 0.2f);
    return x < 0 ? -y : y;
}

static double targetFill, fillAvg = 0, ratio = 1.0, frac = 0, accum = 0;
static int playing = 0;
static long underruns = 0;

static inline void putSample(UInt8 **p, float x) {
    if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
    int32_t s = (int32_t)lrintf(x * 8388607.0f);
    (*p)[0] = s; (*p)[1] = s >> 8; (*p)[2] = s >> 16; *p += 3;
}

// "Caja negra": los últimos DUMP_SECONDS segundos de audio EXACTO que se le manda a la placa
// (después de remuestreo y ganancias, 24 bits). Con `pkill -USR1 djio_bridge` se vuelca a
// ~/djio/djio-volcado-FECHA.wav para analizar si una distorsión ya sale del puente.
#define DUMP_SECONDS 12
#define DUMP_FRAMES ((size_t)(44100 * DUMP_SECONDS))
static int32_t *dumpBuf;
static uint64_t dumpPos = 0;
static inline void dumpFrame(const UInt8 *q) {
    if (!dumpBuf) return;
    int32_t *d = &dumpBuf[(dumpPos % DUMP_FRAMES) * CH];
    for (int c = 0; c < CH; c++) d[c] = (int32_t)((uint32_t)q[3*c] | ((uint32_t)q[3*c+1] << 8) | ((uint32_t)q[3*c+2] << 16)) << 8 >> 8;
    dumpPos++;
}
static long resyncs = 0, lateResched = 0;
static double maxGap = 0;   // hueco más largo entre dos pedidos USB completados (debería ser ≈ 4 ms)
static double monoNow(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static double fillMinSeen = 1e9, fillMaxSeen = 0;   // colchón mínimo y máximo desde el último informe

static void renderFrames(UInt8 **p, int n) {
    uint64_t r = atomic_load_explicit(&ringRead, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&ringWrite, memory_order_acquire);
    double fill = (double)(w - r);
    if (!playing && fill >= targetFill) {
        // Arranque: empezar justo con el colchón objetivo, descartando lo que sobre.
        r = w - (uint64_t)targetFill; fill = targetFill; fillAvg = fill; frac = 0;
        playing = 1;
    } else if (playing && fillAvg > targetFill * 2 + 512) {
        // Se juntó demasiado audio (el sistema estuvo ocupado): volver al objetivo de golpe.
        r = w - (uint64_t)targetFill; fill = targetFill; fillAvg = fill; frac = 0;
        resyncs++;
    }
    if (playing) { if (fill < fillMinSeen) fillMinSeen = fill; if (fill > fillMaxSeen) fillMaxSeen = fill; }
    fillAvg += (fill - fillAvg) * 0.002;
    // Corrección proporcional, hasta ±0,5 % (unos 8 cents: no se nota).
    ratio = 1.0 + 0.005 * fmax(-1.0, fmin(1.0, (fillAvg - targetFill) / targetFill));
    for (int f = 0; f < n; f++) {
        if (!playing || w - r < 2) {
            if (playing) { underruns++; playing = 0; }
            for (int c = 0; c < CH; c++) putSample(p, 0);
            dumpFrame(*p - BPF);
            continue;
        }
        const float *a = &ring[(r & (RING_FRAMES - 1)) * CH];
        const float *b = &ring[((r + 1) & (RING_FRAMES - 1)) * CH];
        for (int c = 0; c < CH; c++) {
            float v = a[c] + (b[c] - a[c]) * (float)frac;
            float g = c < 2 ? gain1 : gain2;                       // salida 1 o 2
            if (g != 1.0f) v = g > 1.0f ? softLimit(v * g) : v * g;
            putSample(p, v);
        }
        dumpFrame(*p - BPF);
        frac += ratio;
        while (frac >= 1.0) { frac -= 1.0; r++; }
    }
    atomic_store_explicit(&ringRead, r, memory_order_release);
}

typedef struct { UInt8 buf[UF_PER_REQ * 13 * BPF]; IOUSBIsocFrame fl[UF_PER_REQ]; } OutBuf;
static OutBuf outBufs[OUT_INFLIGHT];

static void fill(OutBuf *o) {
    UInt8 *p = o->buf;
    if (!closedLoop) {                           // modo anterior: ritmo promedio fijo
        double dr = deviceRate();
        for (int i = 0; i < UF_PER_REQ; i++) {
            accum += dr / 8000.0;
            int n = (int)accum; accum -= n;
            renderFrames(&p, n);
            o->fl[i].frReqCount = n * BPF; o->fl[i].frActCount = 0; o->fl[i].frStatus = 0;
        }
        return;
    }
    for (int ms = 0; ms < UF_PER_REQ / 8; ms++) {
        unsigned q = fbqCount();
        if (q < fbqMin) fbqMin = q;
        if (q > fbqMax) fbqMax = q;
        int F;
        if (q > 0) F = fbq[fbqHead++ % FBQ]; else { F = nominalMs(); fbqEmpty++; }
        int done = 0;
        for (int u = 0; u < 8; u++) {            // repartir F muestras parejo entre los 8 microtramas
            int target = ((u + 1) * F) / 8, n = target - done; done = target;
            renderFrames(&p, n);
            int i = ms * 8 + u;
            o->fl[i].frReqCount = n * BPF; o->fl[i].frActCount = 0; o->fl[i].frStatus = 0;
        }
    }
}

static void outSubmit(OutBuf *o);
static void outDone(void *refcon, IOReturn r, void *arg) {
    { static double lastDone = 0; double t = monoNow(); if (lastDone && t - lastDone > maxGap) maxGap = t - lastDone; lastDone = t; }
    {   // ¿alguna trama de este pedido salió con error?
        OutBuf *ob = refcon;
        for (int k = 0; k < UF_PER_REQ; k++)
            if (ob->fl[k].frStatus != kIOReturnSuccess) { outFrameErrors++; errFrame = ob->fl[k].frStatus; }
    }
    outInflight--;
    if (r != kIOReturnSuccess && r != kIOReturnAborted) {
        usbErrors++;
        if (!stopping) logmsg("\nSe cortó la comunicación con la DJ|iO (0x%08x). ¿Se desconectó?\n", r);
        stopping = 1;
    }
    if (!stopping) outSubmit(refcon);
    if (outInflight == 0) CFRunLoopStop(CFRunLoopGetMain());
}
static void outSubmit(OutBuf *o) {
    fill(o);
    IOReturn r = (*if0)->WriteIsochPipeAsync(if0, pOut, o->buf, outFrame, UF_PER_REQ, o->fl, outDone, o);
    if (r == kIOReturnIsoTooOld) {
        // Nos atrasamos (el sistema estuvo ocupado): reprogramar unos ms adelante.
        lateResched++;
        AbsoluteTime at; UInt64 now;
        (*if0)->GetBusFrameNumber(if0, &now, &at);
        outFrame = now + 8;
        r = (*if0)->WriteIsochPipeAsync(if0, pOut, o->buf, outFrame, UF_PER_REQ, o->fl, outDone, o);
    }
    if (r) {
        usbErrors++;
        if (!stopping) logmsg("\nNo pude mandar audio a la DJ|iO (0x%08x).\n", r);
        stopping = 1;
        if (outInflight == 0) CFRunLoopStop(CFRunLoopGetMain());
        return;
    }
    outFrame += UF_PER_REQ / 8;
    outInflight++;
}

static int startDJiO(void) {
    io_service_t dev = findDJiO();
    if (!dev) { logmsg("No encuentro la DJ|iO. ¿Está enchufada?\n"); return 0; }

    if0 = openIntf(dev, 0);
    if1 = openIntf(dev, 1);
    pOut = findPipe(if0, 0x02); pFb = findPipe(if1, 0x81); pBulk = findPipe(if1, 0x86);
    (*if0)->ClearPipeStallBothEnds(if0, pOut);
    (*if1)->ClearPipeStallBothEnds(if1, pBulk);

    // Secuencia de arranque (orden de la captura del driver de Mac). El GET_CUR es necesario:
    // sin él la placa acepta el audio pero nunca empieza a reproducir.
    UInt8 b[16]; UInt32 got = 0;
    UInt8 rl[3] = { 0x44, 0xAC, 0x00 };   // 44100 Hz
    if (ctrl(0xC0, 'V', 0, 0, b, 8, &got)) { logmsg("La placa no responde: desenchufala y volvé a enchufarla.\n"); return 0; }
    logmsg("DJ|iO encontrada (firmware %02x %02x %02x).\n", b[0], b[1], b[2]);
    // Cada paso se informa solo si falla, para poder diagnosticar.
    struct { const char *name; UInt8 type, req; UInt16 val, idx; void *buf; UInt16 len; } steps[] = {
        { "V",              0xC0, 'V',  0,      0,    b,  8 },
        { "I",              0xC0, 'I',  0,      0,    b,  4 },
        { "GET_CUR(0)",     0xA2, 0x81, 0x0100, 0,    b,  3 },
        { "SET_CUR(0x86)",  0x22, 0x01, 0x0100, 0x86, rl, 3 },
        { "SET_CUR(0x02)",  0x22, 0x01, 0x0100, 0x02, rl, 3 },
        { "GET_CUR(0x86)",  0xA2, 0x81, 0x0100, 0x86, b,  3 },
        { "I",              0xC0, 'I',  0,      0,    b,  4 },
        { "I <- 0x32",      0x40, 'I',  0x0032, 0,    NULL, 0 },
    };
    for (size_t k = 0; k < sizeof steps / sizeof steps[0]; k++) {
        IOReturn e = ctrl(steps[k].type, steps[k].req, steps[k].val, steps[k].idx, steps[k].buf, steps[k].len, &got);
        if (e) { logmsg("La placa no aceptó el arranque (paso %zu, %s: 0x%08x).\n"
                        "Desenchufala y volvé a enchufarla.\n", k + 2, steps[k].name, e); return 0; }
    }
    usleep(1000);

    AbsoluteTime at; UInt64 now;
    (*if0)->GetBusFrameNumber(if0, &now, &at);
    fbFrame = outFrame = now + 50;
    if (closedLoop)    // 5 pedidos de 4 ms ya armados antes del primer ritmo + el retraso fijo
        for (int k = 0; k < OUT_INFLIGHT * (UF_PER_REQ / 8) + FBQ_LAG; k++) fbqPush(nominalMs());
    for (int k = 0; k < FB_INFLIGHT; k++) fbSubmit(&fbBufs[k]);
    for (int k = 0; k < OUT_INFLIGHT; k++) outSubmit(&outBufs[k]);
    for (int k = 0; k < BULK_INFLIGHT; k++) bulkSubmit(&bulkBufs[k]);
    return 1;
}

static void stopDJiO(void) {
    stopping = 1;
    (*if0)->AbortPipe(if0, pOut);
    (*if1)->AbortPipe(if1, pFb);
    (*if1)->AbortPipe(if1, pBulk);
    usleep(100000);
    (*if0)->SetAlternateInterface(if0, 0); (*if1)->SetAlternateInterface(if1, 0);
    (*if0)->USBInterfaceClose(if0); (*if1)->USBInterfaceClose(if1);
}

// ---------- CoreAudio: captura desde BlackHole ----------

static AudioUnit au;
static _Atomic long peakBits = 0;   // pico de los canales 1-4 desde el último informe (×1e6)
static AudioBufferList *abl;
static UInt32 inChannels, ablFrames;

static AudioDeviceID findInputDevice(const char *needle, char *nameOut, size_t nameLen) {
    AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, NULL, &size);
    int n = size / sizeof(AudioDeviceID);
    AudioDeviceID *ids = malloc(size), found = kAudioObjectUnknown;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &size, ids);
    for (int i = 0; i < n && found == kAudioObjectUnknown; i++) {
        CFStringRef name = NULL; UInt32 sz = sizeof(name);
        AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &np, 0, NULL, &sz, &name) || !name) continue;
        char buf[256]; CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(name);
        AudioObjectPropertyAddress sp = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain };
        UInt32 ssz = 0;
        AudioObjectGetPropertyDataSize(ids[i], &sp, 0, NULL, &ssz);
        if (ssz && strcasestr(buf, needle)) { found = ids[i]; snprintf(nameOut, nameLen, "%s", buf); }
    }
    free(ids);
    return found;
}

static OSStatus inputCallback(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                              UInt32 bus, UInt32 frames, AudioBufferList *unused) {
    if (frames > ablFrames) return kAudio_ParamError;
    abl->mBuffers[0].mDataByteSize = frames * inChannels * sizeof(float);
    OSStatus e = AudioUnitRender(au, flags, ts, bus, frames, abl);
    if (!e) {
        const float *x = abl->mBuffers[0].mData;
        float pk = atomic_load_explicit(&peakBits, memory_order_relaxed) / 1e6f;
        UInt32 nch = inChannels < CH ? inChannels : CH;
        for (UInt32 i = 0; i < frames; i++)
            for (UInt32 c = 0; c < nch; c++) { float v = fabsf(x[i * inChannels + c]); if (v > pk) pk = v; }
        atomic_store_explicit(&peakBits, (long)(pk * 1e6f), memory_order_relaxed);
        ringPush(x, frames, inChannels);
    }
    return e;
}

static int startCapture(AudioDeviceID dev) {
    // La DJ|iO solo trabaja a 44,1 kHz: se pide lo mismo a la placa virtual.
    Float64 sr = RATE;
    AudioObjectPropertyAddress rp = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(dev, &rp, 0, NULL, sizeof sr, &sr);
    UInt32 bufFrames = 128;
    AudioObjectPropertyAddress bp = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(dev, &bp, 0, NULL, sizeof bufFrames, &bufFrames);

    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &au)) return 0;
    UInt32 on = 1, off = 0;
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof on);
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof off);
    if (AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev)) return 0;

    AudioStreamBasicDescription hw; UInt32 sz = sizeof hw;
    AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &hw, &sz);
    inChannels = hw.mChannelsPerFrame;
    if (inChannels < 1) return 0;
    AudioStreamBasicDescription fmt = {
        .mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = inChannels * 4, .mFramesPerPacket = 1, .mBytesPerFrame = inChannels * 4,
        .mChannelsPerFrame = inChannels, .mBitsPerChannel = 32 };
    if (AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &fmt, sizeof fmt)) return 0;

    ablFrames = 4096;
    abl = calloc(1, sizeof(AudioBufferList));
    abl->mNumberBuffers = 1;
    abl->mBuffers[0].mNumberChannels = inChannels;
    abl->mBuffers[0].mData = calloc(ablFrames * inChannels, sizeof(float));

    AURenderCallbackStruct cb = { inputCallback, NULL };
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(au) || AudioOutputUnitStart(au)) return 0;
    return 1;
}

// ---------- main ----------

// ---------- registro de eventos (djio-eventos.log) ----------
// Anota, con hora, cada vez que pasa algo que puede oírse mal: cortes de audio, saltos de
// resincronización, desbordes, paquetes USB que llegaron tarde y errores USB.

static FILE *evlog;
static void evmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void evmsg(const char *fmt, ...) {
    if (!evlog) return;
    time_t now = time(NULL); char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(evlog, "%s ", ts);
    va_list ap; va_start(ap, fmt); vfprintf(evlog, fmt, ap); va_end(ap);
    fputc('\n', evlog); fflush(evlog);
}

static void monitor(CFRunLoopTimerRef t, void *info) {
    static long lu = 0, lr = 0, lo = 0, ll = 0, le = 0, lf = 0, lq = 0, ld = 0; static int beat = 0;
    long o = atomic_load(&overflows);
    {   // ¿el hilo principal se demoró? (el temporizador debería sonar cada 1000 ms)
        static double lastTick = 0; double t = monoNow();
        if (lastTick && t - lastTick > 1.05) evmsg("DEMORA hilo principal: tardó %.0f ms en volver a correr (debía ser 1000)", (t - lastTick) * 1000);
        lastTick = t;
        if (maxGap > 0.012) evmsg("DEMORA USB: hueco máximo entre pedidos %.0f ms (normal ≈ 4)", maxGap * 1000);
        maxGap = 0;
    }
    double mn = fillMinSeen > 1e8 ? 0 : fillMinSeen, mx = fillMaxSeen;
    if (fbqEmpty != lq || fbqDrop != ld)
        evmsg("COLA DE RITMO: vacía +%ld veces, descartes +%ld (cola %u..%u ms)", fbqEmpty - lq, fbqDrop - ld, fbqMin > 999 ? 0 : fbqMin, fbqMax);
    lq = fbqEmpty; ld = fbqDrop;
    if (underruns != lu || resyncs != lr || o != lo || lateResched != ll || usbErrors != le || outFrameErrors != lf)
        evmsg("EVENTO cortes +%ld, resincros +%ld, desbordes +%ld, usb-tarde +%ld, errores-usb +%ld, errores-trama +%ld | códigos: bulk 0x%08x, ritmo 0x%08x, trama 0x%08x | colchón %.1f..%.1f ms | ajuste %+.3f %% | placa %.1f Hz",
              underruns - lu, resyncs - lr, o - lo, lateResched - ll, usbErrors - le, outFrameErrors - lf,
              errBulk, errFb, errFrame, mn / RATE * 1000, mx / RATE * 1000, (ratio - 1) * 100, deviceRate());
    lu = underruns; lr = resyncs; lo = o; ll = lateResched; le = usbErrors; lf = outFrameErrors;
    if (++beat >= 60) {
        beat = 0;
        evmsg("estado: cola de ritmo %u..%u ms | colchón %.1f..%.1f ms | ajuste %+.3f %% | placa %.1f Hz | totales: cortes %ld, resincros %ld, desbordes %ld, usb-tarde %ld, errores-usb %ld",
              fbqMin > 999 ? 0 : fbqMin, fbqMax, mn / RATE * 1000, mx / RATE * 1000, (ratio - 1) * 100, deviceRate(), underruns, resyncs, o, lateResched, usbErrors);
    }
    fillMinSeen = 1e9; fillMaxSeen = 0; fbqMin = 1000; fbqMax = 0;
}

// Imprimir en la Terminal NUNCA debe frenar el audio: si la Terminal tarda en leer, el buffer
// de la consola se llena y write() se bloquea. Por eso se imprime desde otro hilo, y si la
// impresión anterior sigue trabada, se descarta la nueva. (-DPRINT_SYNC vuelve al modo viejo
// para poder comparar.)
static _Atomic int printBusy = 0;
static void printAsync(const char *line) {
#ifdef PRINT_SYNC
    fputs(line, stdout);
#else
    if (atomic_exchange(&printBusy, 1)) return;
    char *copy = strdup(line);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        fputs(copy, stdout); free(copy); atomic_store(&printBusy, 0);
    });
#endif
}

// Prioridad de tiempo real para el hilo que maneja el USB (la misma que usan las aplicaciones de
// audio): evita que el sistema lo deje esperando decenas de ms detrás de otras tareas.
static void makeRealtime(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double msToAbs = 1e6 * (double)tb.denom / (double)tb.numer;
    thread_time_constraint_policy_data_t pol;
    pol.period = (uint32_t)(4 * msToAbs);          // un pedido USB cada 4 ms
    pol.computation = (uint32_t)(0.5 * msToAbs);   // trabajo por activación (de sobra)
    pol.constraint = (uint32_t)(2 * msToAbs);
    pol.preemptible = 1;
    kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                                         (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    evmsg("prioridad de tiempo real: %s", kr == KERN_SUCCESS ? "activada" : "NO se pudo activar");
}

static void doDump(void) {
    if (!dumpBuf || dumpPos == 0) { evmsg("volcado pedido, pero todavía no hay audio"); return; }
    size_t n = dumpPos < DUMP_FRAMES ? (size_t)dumpPos : DUMP_FRAMES;
    int32_t *copy = malloc(n * CH * sizeof(int32_t));
    uint64_t start = dumpPos - n;
    for (size_t i = 0; i < n; i++) memcpy(&copy[i * CH], &dumpBuf[((start + i) % DUMP_FRAMES) * CH], CH * sizeof(int32_t));
    time_t now = time(NULL); char ts[32], path[1100];
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&now));
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/djio/djio-volcado-%s.wav", home ? home : ".", ts);
    char *pathc = strdup(path);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        FILE *o = fopen(pathc, "wb");
        free(pathc);
        if (!o) { free(copy); return; }
        uint32_t bytes = (uint32_t)(n * CH * 4), v; uint16_t h;
        fwrite("RIFF", 1, 4, o); v = 36 + bytes; fwrite(&v, 4, 1, o); fwrite("WAVEfmt ", 1, 8, o);
        v = 16; fwrite(&v, 4, 1, o); h = 1; fwrite(&h, 2, 1, o); h = CH; fwrite(&h, 2, 1, o);
        v = 44100; fwrite(&v, 4, 1, o); v = 44100 * CH * 4; fwrite(&v, 4, 1, o);
        h = CH * 4; fwrite(&h, 2, 1, o); h = 32; fwrite(&h, 2, 1, o);
        fwrite("data", 1, 4, o); fwrite(&bytes, 4, 1, o);
        for (size_t i = 0; i < n * CH; i++) { int32_t x = copy[i] << 8; fwrite(&x, 4, 1, o); }   // 24 bits en 32
        fclose(o); free(copy);
    });
    evmsg("volcado de los últimos %.1f s guardado en %s", (double)n / 44100.0, path);
}

static void status(CFRunLoopTimerRef t, void *info) {
    uint64_t fillNow = atomic_load(&ringWrite) - atomic_load(&ringRead);
    double pk = atomic_exchange(&peakBits, 0) / 1e6;
    char lvl[16];
    if (pk > 0) snprintf(lvl, sizeof lvl, "%5.1f dB", 20 * log10(pk)); else snprintf(lvl, sizeof lvl, " silen.");
    if (waitMode)
        logmsg("nivel %s | colchón %4.1f ms | placa %.1f Hz | cortes %ld | resincros %ld | desbordes %ld\n",
               lvl, fillNow / RATE * 1000, deviceRate(), underruns, resyncs, (long)atomic_load(&overflows));
    else {
        char line[256];
        snprintf(line, sizeof line, "\r  nivel %s | colchón %4.1f ms | ajuste %+.3f %% | placa %.1f Hz | cortes %ld | resincros %ld | tarde %ld   ",
                 lvl, fillNow / RATE * 1000, (ratio - 1) * 100, deviceRate(), underruns, resyncs, lateResched);
        printAsync(line);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // Prioridad alta y sin suspensión por inactividad: el audio USB no puede esperar.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    IOPMAssertionID pmAssertion = 0;   // se pide recién cuando hay audio transmitiéndose (ver más abajo)
    {
        const char *home = getenv("HOME");
        char path[1024];
        snprintf(path, sizeof path, "%s/djio/djio-eventos.log", home ? home : ".");
        evlog = fopen(path, "a");
    }
    const char *pos[2] = { "BlackHole", "25" }; int np = 0;
    double gain1Db = GANANCIA_SALIDA1_DB, gain2Db = GANANCIA_SALIDA2_DB;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--esperar")) waitMode = 1;
        else if (!strcmp(argv[i], "--promedio")) closedLoop = 0;
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppmBias = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ganancia1") && i + 1 < argc) gain1Db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ganancia2") && i + 1 < argc) gain2Db = atof(argv[++i]);
        else if (np < 2) pos[np++] = argv[i];
    }
    const char *needle = pos[0];
    double latencyMs = atof(pos[1]);
    targetFill = fmax(64, latencyMs / 1000.0 * RATE);
    gain1 = (float)pow(10.0, gain1Db / 20.0);
    gain2 = (float)pow(10.0, gain2Db / 20.0);

    if (waitMode) {
        io_service_t d = findDJiO();
        if (!d) {
            logmsg("Esperando a que se enchufe la DJ|iO...\n");
            while (!(d = findDJiO())) sleep(1);
            sleep(2);   // dejar que termine de iniciarse
        }
        IOObjectRelease(d);
    }

    char devName[256] = "";
    AudioDeviceID in = findInputDevice(needle, devName, sizeof devName);
    if (in == kAudioObjectUnknown) {
        logmsg("No encuentro un dispositivo de entrada que se llame \"%s\".\n"
               "Instalá BlackHole (16ch) o pasá otro nombre: ./djio_bridge \"nombre\"\n", needle);
        if (waitMode) sleep(10);   // no reintentar en bucle si falta BlackHole
        return 1;
    }
    // Primero la captura (tarda en arrancar) y después la placa, para que el audio USB
    // no quede programado en un momento que ya pasó.
    if (!startCapture(in)) { logmsg("No pude capturar audio de \"%s\".\n", devName); if (waitMode) sleep(10); return 1; }
    if (!startDJiO()) { AudioOutputUnitStop(au); fail(); }
    // Sin suspensión por inactividad SOLO mientras se transmite audio a la placa. (Así, con el servicio
    // instalado y la placa desenchufada, la Mac puede dormirse con normalidad.)
    IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep, kIOPMAssertionLevelOn,
                                CFSTR("djio_bridge: audio en curso"), &pmAssertion);
    logmsg("Puente activo: \"%s\" (%u canales) -> DJ|iO. Colchón %.0f ms. Salida 1: %+.1f dB, salida 2: %+.1f dB.\n",
           devName, (unsigned)inChannels, latencyMs, gain1Db, gain2Db);
    if (!waitMode)
        printf("  Canales 1-2 -> salida 1 (máster), canales 3-4 -> salida 2 (auriculares).\n"
               "  Ctrl+C para salir.\n");

    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN);
    dispatch_source_t sigs[2];
    int nums[2] = { SIGINT, SIGTERM };
    for (int i = 0; i < 2; i++) {
        sigs[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, nums[i], 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(sigs[i], ^{ stopping = 1; CFRunLoopStop(CFRunLoopGetMain()); });
        dispatch_resume(sigs[i]);
    }
    evmsg("---- puente iniciado: %s, salida 1 %+.1f dB, salida 2 %+.1f dB, colchón %.0f ms, %d pedidos USB en vuelo, ajuste de ritmo %+.0f ppm, modo de ritmo: %s",
          devName, gain1Db, gain2Db, latencyMs, OUT_INFLIGHT, ppmBias,
          closedLoop ? "lazo cerrado (cuenta exacta de la placa)" : "promedio fijo");
    CFRunLoopTimerRef tm = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 1, 0, 0, monitor, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), tm, kCFRunLoopDefaultMode);
    dumpBuf = calloc(DUMP_FRAMES * CH, sizeof(int32_t));
    signal(SIGUSR1, SIG_IGN);
    dispatch_source_t sigDump = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(sigDump, ^{ doDump(); });
    dispatch_resume(sigDump);
    makeRealtime();
    double every = waitMode ? 300 : 2;   // en el registro del servicio, cada 5 minutos
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + (waitMode ? 10 : 2), every, 0, 0, status, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopDefaultMode);

    while (!stopping) CFRunLoopRun();

    logmsg("%sCerrando...\n", waitMode ? "" : "\n");
    AudioOutputUnitStop(au);
    AudioComponentInstanceDispose(au);
    stopDJiO();
    if (pmAssertion) IOPMAssertionRelease(pmAssertion);
    evmsg("---- puente cerrado: cortes %ld, resincros %ld, usb-tarde %ld, errores-usb %ld", underruns, resyncs, lateResched, usbErrors);
    logmsg("Listo. Cortes de audio: %ld.\n", underruns);
    return 0;
}
