// Prueba de salida para Numark DJ|iO (15e4:0070) en macOS, sin driver.
// Activa la interfaz 0 / alt 1 y manda un tono por el endpoint isócrono 0x02.
// Uso: ./djio_test [rate] [canal_inicial] [segundos] [usar_if1]
//   rate: 44100 o 48000 (por defecto 44100)
//   canal_inicial: 0 = salidas 1-2, 2 = salidas 3-4 (por defecto 0)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VID 0x15e4
#define PID 0x0070
#define CH 4
#define BPS 3                  // bytes por muestra (24 bits)
#define MICROFRAMES 64         // por transferencia (8 ms)
#define INFLIGHT 4
#define MAXPKT 156

typedef IOUSBInterfaceInterface300 **Intf;

static double rate = 44100, phase = 0, accum = 0;
static int startCh = 0, totalUf = 0, doneUf = 0, errors = 0, inflight = 0;
static Intf intf0;
static UInt8 pipeOut;
static UInt64 nextFrame;

typedef struct { UInt8 buf[MICROFRAMES * MAXPKT]; IOUSBIsocFrame fl[MICROFRAMES]; } Xfer;
static Xfer xfers[INFLIGHT];

static void fill(Xfer *x) {
    UInt8 *p = x->buf;
    for (int i = 0; i < MICROFRAMES; i++) {
        accum += rate / 8000.0;
        int n = (int)accum; accum -= n;
        for (int f = 0; f < n; f++) {
            int32_t s = (int32_t)(sin(phase) * 0.03 * 8388607); // ~ -30 dBFS
            phase += 2 * M_PI * 440 / rate;
            if (phase > 2 * M_PI) phase -= 2 * M_PI;
            static const int order[CH] = { 0, 2, 1, 3 };
            for (int k = 0; k < CH; k++) {
                int c = order[k];
                int32_t v = (startCh < 0 || c == startCh || c == startCh + 1) ? s : 0;
                *p++ = v & 0xff; *p++ = (v >> 8) & 0xff; *p++ = (v >> 16) & 0xff;
            }
        }
        x->fl[i].frReqCount = n * CH * BPS;
        x->fl[i].frActCount = 0;
        x->fl[i].frStatus = 0;
    }
}


// --- Monitoreo de lo que manda la placa (canal de sincronización y comandos) ---
#define IN_ENTRIES 8           // una entrada por intervalo de 1 ms
static Intf intf1;
static UInt64 inFrame;
static UInt8 inBuf[IN_ENTRIES * 64];
static IOUSBIsocFrame inFl[IN_ENTRIES];
static int inPrinted = 0, inPackets = 0, monitoring = 1;
typedef struct { Intf i; UInt8 pipe; UInt8 ep; UInt8 buf[512]; } BulkMon;
static BulkMon bulk[2];

static void dump(const char *tag, UInt8 *b, int n) {
    printf("%s (%d bytes):", tag, n);
    for (int k = 0; k < n && k < 32; k++) printf(" %02x", b[k]);
    printf("%s\n", n > 32 ? " ..." : "");
}

static void inSubmit(void);
static void inDone(void *refcon, IOReturn r, void *arg) {
    if (r != kIOReturnSuccess && inPrinted < 3) { printf("lectura 0x81: 0x%08x\n", r); inPrinted++; }
    UInt8 *p = inBuf;
    for (int k = 0; k < IN_ENTRIES; k++) {
        int n = inFl[k].frActCount;
        if (n) { inPackets++; if (inPrinted < 12) { dump("ep 0x81", p, n); inPrinted++; } }
        p += 64;
    }
    if (monitoring && r == kIOReturnSuccess) inSubmit();
}
static void inSubmit(void) {
    for (int k = 0; k < IN_ENTRIES; k++) { inFl[k].frReqCount = 64; inFl[k].frActCount = 0; inFl[k].frStatus = 0; }
    IOReturn r = (*intf1)->ReadIsochPipeAsync(intf1, 1, inBuf, inFrame, IN_ENTRIES, inFl, inDone, NULL);
    if (r) { printf("ReadIsochPipeAsync 0x81: 0x%08x\n", r); return; }
    inFrame += IN_ENTRIES;
}

static void bulkSubmit(BulkMon *m);
static void bulkDone(void *refcon, IOReturn r, void *arg) {
    BulkMon *m = refcon;
    int n = (int)(uintptr_t)arg;
    if (r == kIOReturnSuccess) {
        char tag[16]; snprintf(tag, sizeof tag, "ep 0x%02x", m->ep);
        dump(tag, m->buf, n);
        if (monitoring) bulkSubmit(m);
    }
}
static void bulkSubmit(BulkMon *m) {
    IOReturn r = (*m->i)->ReadPipeAsync(m->i, m->pipe, m->buf, sizeof m->buf, bulkDone, m);
    if (r) printf("ReadPipeAsync ep 0x%02x: 0x%08x\n", m->ep, r);
}


// Pedido de control por el endpoint 0. Devuelve el código de IOKit.
static IOReturn ctrl(Intf i, UInt8 type, UInt8 req, UInt16 val, UInt16 idx, void *buf, UInt16 len, UInt32 *got) {
    IOUSBDevRequestTO r = { type, req, val, idx, len, buf, 0, 500, 1000 };
    IOReturn e = (*i)->ControlRequestTO(i, 0, &r);
    if (got) *got = r.wLenDone;
    return e;
}

// Comando de frecuencia del driver Ploytec: SET_CUR (clase, al endpoint), 3 bytes LE.
// Sin GET_CUR: la DJ|iO no lo contesta y se queda trabada.
static int setRate(Intf i, UInt8 ep, UInt32 hz) {
    UInt8 b[3] = { hz & 0xff, (hz >> 8) & 0xff, (hz >> 16) & 0xff };
    IOReturn e = ctrl(i, 0x22, 0x01, 0x0100, ep, b, 3, NULL);
    printf("  frecuencia %u Hz -> ep 0x%02x: 0x%08x\n", hz, ep, e);
    return e == kIOReturnSuccess;
}

// Confirmación de estado Ploytec: lee el registro 0 de 'I' y lo reescribe con el bit 0x20.
static int confirmStatus(Intf i) {
    UInt8 st = 0; UInt32 got = 0;
    IOReturn e = ctrl(i, 0xC0, 'I', 0, 0, &st, 1, &got);
    if (e || got != 1) { printf("  leer estado: 0x%08x\n", e); return 0; }
    UInt16 wv = (UInt16)(SInt16)(SInt8)(st | 0x20);
    e = ctrl(i, 0x40, 'I', wv, 0, NULL, 0, NULL);
    UInt8 after = 0;
    ctrl(i, 0xC0, 'I', 0, 0, &after, 1, &got);
    printf("  estado: 0x%02x -> 0x%02x (escritura 0x%08x)\n", st, after, e);
    return e == kIOReturnSuccess;
}

static void submit(Xfer *x);
static void rearm(void);
static int rearmAt = -1;

static void done(void *refcon, IOReturn r, void *arg) {
    Xfer *x = refcon;
    inflight--;
    if (r != kIOReturnSuccess) { fprintf(stderr, "transferencia: 0x%08x\n", r); errors++; }
    for (int i = 0; i < MICROFRAMES; i++)
        if (x->fl[i].frStatus != kIOReturnSuccess && errors < 5) {
            fprintf(stderr, "microframe %d estado 0x%08x\n", i, x->fl[i].frStatus); errors++;
        }
    doneUf += MICROFRAMES;
    if (rearmAt >= 0 && doneUf >= rearmAt) { rearmAt = -1; rearm(); }
    if (doneUf + inflight * MICROFRAMES < totalUf && errors < 20) submit(x);
    if (inflight == 0) CFRunLoopStop(CFRunLoopGetCurrent());
}

static void submit(Xfer *x) {
    fill(x);
    IOReturn r = (*intf0)->WriteIsochPipeAsync(intf0, pipeOut, x->buf, nextFrame,
                                               MICROFRAMES, x->fl, done, x);
    if (r) { fprintf(stderr, "WriteIsochPipeAsync: 0x%08x\n", r); errors++; return; }
    nextFrame += MICROFRAMES / 8;
    inflight++;
}

static Intf openIntf(int num) {
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    int vid = VID, pid = PID;
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), CFNumberCreate(NULL, kCFNumberIntType, &vid));
    CFDictionarySetValue(m, CFSTR(kUSBProductID), CFNumberCreate(NULL, kCFNumberIntType, &pid));
    io_service_t dev = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!dev) { fprintf(stderr, "No encuentro la DJ|iO\n"); exit(1); }
    io_iterator_t it; io_service_t s = 0, c;
    IORegistryEntryGetChildIterator(dev, kIOServicePlane, &it);
    while ((c = IOIteratorNext(it))) {
        CFNumberRef nr = IORegistryEntryCreateCFProperty(c, CFSTR(kUSBInterfaceNumber), NULL, 0);
        int v = -1;
        if (nr) { CFNumberGetValue(nr, kCFNumberIntType, &v); CFRelease(nr); }
        if (v == num) { s = c; break; }
        IOObjectRelease(c);
    }
    if (!s) { fprintf(stderr, "No encuentro la interfaz %d de la DJ|iO\n", num); exit(1); }
    IOCFPlugInInterface **pi; SInt32 sc; Intf i;
    if (IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &pi, &sc)) {
        fprintf(stderr, "No pude crear el plugin de la interfaz %d\n", num); exit(1);
    }
    (*pi)->QueryInterface(pi, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&i);
    IOReturn r = (*i)->USBInterfaceOpen(i);
    if (r) { fprintf(stderr, "USBInterfaceOpen(%d): 0x%08x\n", num, r); exit(1); }
    r = (*i)->SetAlternateInterface(i, 1);
    if (r) { fprintf(stderr, "SetAlternateInterface(%d,1): 0x%08x\n", num, r); exit(1); }
    UInt8 n; (*i)->GetNumEndpoints(i, &n);
    for (UInt8 p = 1; p <= n; p++) {
        UInt8 dir, en, tt, iv; UInt16 mps;
        (*i)->GetPipeProperties(i, p, &dir, &en, &tt, &mps, &iv);
        printf("  if%d pipe %d: ep 0x%02x tipo %d max %d intervalo %d\n", num, p,
               en | (dir == kUSBIn ? 0x80 : 0), tt, mps, iv);
        if (num == 0 && tt == kUSBIsoc && dir == kUSBOut) pipeOut = p;
    }
    CFRunLoopSourceRef src;
    (*i)->CreateInterfaceAsyncEventSource(i, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    return i;
}

// Vuelve el estado a su valor de encendido (sin el bit 0x20) y lo confirma de nuevo,
// con el audio ya en marcha.
static void rearm(void) {
    UInt8 st = 0; UInt32 got = 0;
    ctrl(intf0, 0xC0, 'I', 0, 0, &st, 1, &got);
    UInt16 wv = (UInt16)(SInt16)(SInt8)(st & ~0x20);
    IOReturn e = ctrl(intf0, 0x40, 'I', wv, 0, NULL, 0, NULL);
    UInt8 mid = 0; ctrl(intf0, 0xC0, 'I', 0, 0, &mid, 1, &got);
    printf("  [%.1f s] estado 0x%02x -> 0x%02x (0x%08x)\n", doneUf / 8000.0, st, mid, e);
    confirmStatus(intf0);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) rate = atof(argv[1]);
    if (argc > 2) startCh = atoi(argv[2]);
    double secs = argc > 3 ? atof(argv[3]) : 5;
    int useIf1 = argc > 4 ? atoi(argv[4]) : 0;
    if (argc > 5) rearmAt = (int)(atof(argv[5]) * 8000);   // segundos hasta re-armar
    totalUf = (int)(secs * 8000);

    if (startCh < 0) printf("DJ|iO: %.0f Hz, 4 canales 24 bits, tono en las 4 salidas, %.0f s\n", rate, secs);
    else printf("DJ|iO: %.0f Hz, 4 canales 24 bits, tono en salidas %d-%d, %.0f s\n",
                rate, startCh + 1, startCh + 2, secs);
    intf0 = openIntf(0);
    intf1 = useIf1 ? openIntf(1) : NULL;
    if (!pipeOut) { fprintf(stderr, "No hay endpoint isócrono de salida\n"); return 1; }

    UInt8 ver[15] = { 0 }; UInt32 got = 0;
    IOReturn e = ctrl(intf0, 0xC0, 'V', 0, 0, ver, sizeof ver, &got);
    printf("  firmware ('V'): 0x%08x,", e);
    for (UInt32 k = 0; k < got; k++) printf(" %02x", ver[k]);
    printf("\n");
    if (e) return 1;
    if (!setRate(intf0, 0x02, (UInt32)rate)) return 1;
    if (!confirmStatus(intf0)) return 1;

    AbsoluteTime t;
    (*intf0)->GetBusFrameNumber(intf0, &nextFrame, &t);
    nextFrame += 20;
    bulk[0] = (BulkMon){ intf0, 2, 0x83 };
    // useIf1 == 2: activa la interfaz 1 pero sin dejar lecturas pendientes.
    if (useIf1 != 2) bulkSubmit(&bulk[0]);
    if (intf1 && useIf1 != 2) {
        bulk[1] = (BulkMon){ intf1, 2, 0x86 };
        bulkSubmit(&bulk[1]);
        inFrame = nextFrame;
        inSubmit();
    }
    for (int k = 0; k < INFLIGHT; k++) submit(&xfers[k]);
    if (inflight) CFRunLoopRun();
    monitoring = 0;
    if (intf1) printf("Paquetes recibidos por ep 0x81: %d\n", inPackets);

    (*intf0)->SetAlternateInterface(intf0, 0);
    (*intf0)->USBInterfaceClose(intf0);
    if (intf1) { (*intf1)->SetAlternateInterface(intf1, 0); (*intf1)->USBInterfaceClose(intf1); }
    printf("Listo. Microframes enviados: %d, errores: %d\n", doneUf, errors);
    return errors ? 2 : 0;
}
