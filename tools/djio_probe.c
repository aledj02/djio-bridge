// Sonda de comandos para Numark DJ|iO (15e4:0070): no manda audio.
// Prueba, de a uno y con límite de tiempo, los pedidos de control del driver Ploytec.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef IOUSBDeviceInterface320 **Dev;

static Dev openDev(void) {
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    int vid = 0x15e4, pid = 0x0070;
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), CFNumberCreate(NULL, kCFNumberIntType, &vid));
    CFDictionarySetValue(m, CFSTR(kUSBProductID), CFNumberCreate(NULL, kCFNumberIntType, &pid));
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!s) { printf("No encuentro la DJ|iO\n"); exit(1); }
    IOCFPlugInInterface **pi; SInt32 sc; Dev d;
    IOCreatePlugInInterfaceForService(s, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &pi, &sc);
    (*pi)->QueryInterface(pi, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320), (LPVOID *)&d);
    return d;
}

static int req(Dev d, const char *name, UInt8 type, UInt8 rq, UInt16 val, UInt16 idx, UInt8 *buf, UInt16 len) {
    IOUSBDevRequestTO r = { type, rq, val, idx, len, buf, 0, 300, 600 };
    IOReturn e = (*d)->DeviceRequestTO(d, &r);
    printf("%-34s -> 0x%08x", name, e);
    if (!e && (type & 0x80)) {
        printf("  [%u bytes]", (unsigned)r.wLenDone);
        for (UInt32 k = 0; k < r.wLenDone; k++) printf(" %02x", buf[k]);
    }
    printf("\n");
    return e == kIOReturnSuccess;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    Dev d = openDev();
    UInt8 b[64];
    UInt8 rate[3] = { 0x80, 0xBB, 0x00 };   // 48000 Hz, little-endian
    // Secuencia de arranque Ploytec, sin audio. Sin GET_CUR: la DJ|iO se traba con ese pedido.
    // Corta en el primer error para no mandar nada a una placa que no responde.
    if (!req(d, "1 V  version firmware", 0xC0, 'V', 0, 0, b, 15)) return 1;
    if (argc > 1 && strcmp(argv[1], "v") == 0) return 0;   // solo comprobar que contesta
    if (!req(d, "2 I  leer estado (reg 0)", 0xC0, 'I', 0, 0, b, 1)) return 1;
    if (!req(d, "3 SET_CUR 48000 -> ep 0x02", 0x22, 0x01, 0x0100, 0x02, rate, 3)) return 1;
    if (!req(d, "4 V  sigue respondiendo?", 0xC0, 'V', 0, 0, b, 15)) return 1;
    if (!req(d, "5 I  leer estado (reg 0)", 0xC0, 'I', 0, 0, b, 1)) return 1;
    UInt8 st = b[0];
    UInt16 wv = (UInt16)(SInt16)(SInt8)(st | 0x20);
    printf("   confirmando estado: 0x%02x -> wValue 0x%04x\n", st, wv);
    if (!req(d, "6 I  confirmar estado (reg 0)", 0x40, 'I', wv, 0, NULL, 0)) return 1;
    if (!req(d, "7 I  leer estado (reg 0)", 0xC0, 'I', 0, 0, b, 1)) return 1;
    return 0;
}
