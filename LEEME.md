# Numark DJ|iO en macOS (sin driver)

Puente que manda el audio de **BlackHole 16ch** a una Numark DJ|iO original (USB 15e4:0070, chip Ploytec). Numark ya no tiene driver para macOS moderno. El original (3.2.0) no carga porque depende de IOUSBFamily, que Apple quitó. Este puente habla con la placa directamente por USB, sin instalar nada en el sistema.

- Canales 1-2 de BlackHole → salida 1 (máster)
- Canales 3-4 de BlackHole → salida 2 (auriculares / cue)
- Solo 44,1 kHz (la placa no acepta otra frecuencia; con 48 kHz se cuelga)

Probado en: MacBook Pro 16" 2019 (Intel), macOS 15.7.3, BlackHole 16ch, Traktor 3, mixer externo.

## Compilar

Hace falta una sola vez (o tras cambiar el código). Requiere las herramientas de línea de comandos de Xcode (`xcode-select --install`) y [BlackHole 16ch](https://existential.audio/blackhole/):

```bash
cd ~/djio   # o la carpeta donde esté el proyecto
clang -O2 -o djio_bridge djio_bridge.c -framework IOKit -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
```

## Uso

1. Enchufá la DJ|iO. Mejor en un hub con fuente propia: pide 480 mA y con poca corriente se cuelga.
2. En la Terminal:

   ```bash
   cd ~/djio && ./djio_bridge
   ```

3. En Traktor elegí **BlackHole 16ch** como salida: máster en 1-2, monitor/cue en 3-4, 44100 Hz.
4. Para cerrar: Ctrl+C en la Terminal.

Mientras corre, el puente muestra el nivel de entrada, el colchón de audio, el ritmo real de la placa (≈44102 Hz) y los cortes.

### Opciones

```
./djio_bridge [--esperar] [--ganancia1 dB] [--ganancia2 dB] [nombre_entrada] [latencia_ms]
```

- `nombre_entrada`: parte del nombre del dispositivo de entrada (por defecto `BlackHole`).
- `latencia_ms`: colchón entre la Mac y la placa (por defecto 25). Con menos ms baja el retraso, pero sube el riesgo de cortes. El retraso total ronda los 40 ms.
- `--promedio`: usa el ritmo promedio fijo en vez de copiar el conteo exacto de la placa (el modo anterior).
- `--ppm N`: ajuste fino del ritmo con que se le manda audio a la placa, en partes por millón (por defecto 0). Se probó con +300 y −300 para descartar una deriva de reloj: no hizo falta.
- `--ganancia1`, `--ganancia2`: corrección de nivel por salida, en dB. Valen 0 por defecto. Se usaron −24 en la salida 1 mientras la salida 2 estuvo dañada (ver `Hardware.md`).
- `--esperar`: modo servicio. Si la placa no está, espera a que la enchufes; si la desenchufás, termina (y el servicio lo vuelve a lanzar); si la placa arranca trabada, espera a que la reconectes.

### Arranque automático (opcional)

Para que el puente corra solo en segundo plano desde que iniciás sesión, y arranque al enchufar la placa:

- **Instalar:** doble clic en `instalar_servicio.command` (antes hay que tener compilado `djio_bridge`, ver "Compilar"). No pide contraseña: es un servicio de tu usuario. Genera `~/Library/LaunchAgents/com.djio.bridge.plist` a partir de la plantilla `com.djio.bridge.plist.template`, poniendo la ruta de la carpeta donde esté el proyecto.
- **Quitar:** `desinstalar_servicio.command`.
- **Registro:** `~/djio/djio.log`, con un estado cada 5 minutos.
- Consumo medido (2026-10-11): sin la placa, el servicio se despierta una vez por segundo (despreciable). Transmitiendo, usa 4-5 % de un núcleo, 12-16 MB de memoria y ~1.000 despertares por segundo (normal en audio USB). El puente solo le pide a macOS que no suspenda el sistema **mientras transmite**; sin placa, la Mac se duerme con normalidad. Con la placa enchufada y Traktor cerrado, igual transmite silencio: en una notebook con batería, conviene desenchufar la placa cuando no se usa.
- Verificado el 2026-10-11: con el servicio instalado y música sonando desde Traktor, al desenchufar y volver a enchufar la placa, el puente reanudó solo.
- Con el servicio instalado no abras el puente a mano: los dos se pelearían por la placa.
- La primera vez macOS puede pedir permiso de micrófono para `djio_bridge` (para leer BlackHole). Hay que aceptarlo. Si el registro muestra `nivel silen.` con música sonando, revisá ese permiso en Configuración del sistema → Privacidad y seguridad → Micrófono.

## Si algo falla

- **"La placa no responde"** o **"no aceptó el arranque"**: desenchufá la DJ|iO, volvé a enchufarla y arrancá el puente de nuevo. Es lo único que la destraba: la reconexión por software no alcanza.
- **"No pude abrir la interfaz … ¿Hay otro programa usando la placa?"**: ya hay otro puente corriendo (en otra Terminal o como servicio).
- **"No encuentro un dispositivo de entrada"**: falta BlackHole, o hay que reiniciar el audio con `sudo killall coreaudiod`.
- **Una salida suena más baja que la otra:** no es el software. Ver `Hardware.md` (diagnóstico y reparación de la placa, con el procedimiento para encontrar capacitores dañados).
- **Distorsión, agudos o "voz robótica" después de un rato** *(dos causas encontradas y corregidas el 2026-10-11)*:
  1. **La Terminal frenaba el audio.** El puente imprimía su línea de estado desde el mismo hilo que manda el audio. Si la Terminal tardaba en leer (por ejemplo, tapada por otra ventana), la consola se llenaba y el puente quedaba esperando sin mandar audio a la placa. Se reprodujo con una consola trabada: la versión vieja se frenó 103 segundos. Ahora la impresión va por otro hilo y se descarta si la consola no responde.
  2. **Demoras de 40 a 90 ms del propio puente cada ~2-3 minutos** (el sistema lo dejaba esperando detrás de otras tareas). Rompían las lecturas USB de ritmo, que solo tienen 8 ms de margen, y como el puente no las reintentaba, la placa quedaba sin que le leyeran sus canales de datos y seguía distorsionando hasta reiniciar. Ahora: (a) el puente corre con **prioridad de tiempo real**, y en la prueba pasó de un tropiezo cada 90-216 s a ninguno en 7 minutos; (b) si una lectura USB falla, **limpia el canal y la reintenta**, en vez de abandonarla; (c) manda 20 ms de audio por adelantado (5 pedidos de 4 ms) para tolerar demoras cortas.

  3. **Distorsión que crece de a poco y se corrige sola de golpe, sin ningún evento en el registro** (el 2026-10-11, tercera causa probable). La placa consume 44 o 45 muestras por milisegundo, en el patrón que ella decide; el puente se las mandaba con un promedio fijo, y la fase entre los dos patrones se corría lentamente, acercando la memoria interna de la placa a vaciarse o desbordarse. Ahora el puente **copia milisegundo a milisegundo el conteo exacto que informa la placa** (byte 0 del paquete de ritmo, con retraso fijo de 8 ms), como hace el driver oficial. La cola del ritmo se mantiene en 6-9 ms. Si el ritmo de la placa falla, vuelve solo al promedio. `./djio_bridge --promedio` usa el modo anterior, para comparar.

  Si vuelve a pasar, mirá `~/djio/djio-eventos.log`: anota con hora cada corte, resincronización, `DEMORA` del hilo principal o de USB, y los códigos de error exactos (por ejemplo, `ritmo 0xe00002ee` = lectura de ritmo programada en el pasado; `bulk 0xe00002ed` = la placa no respondió).
- **Pulsaciones o cortes:** subí el colchón, por ejemplo `./djio_bridge BlackHole 40`.
- Si cambiaste el código, recompilá:

  ```bash
  clang -O2 -o djio_bridge djio_bridge.c -framework IOKit -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
  ```

## Cómo funciona (protocolo USB)

Tomado de [numark-djio-linux](https://github.com/djpallax/numark-djio-linux), que reproduce una captura del driver oficial de Mac. Confirmado en esta placa.

- **Interfaz 0, alt 1:** EP 0x02 isócrono OUT, audio S24_3LE, 4 canales en orden natural, 32 microframes (4 ms) por pedido, 3 pedidos en vuelo.
- **Interfaz 1, alt 1:**
  - EP 0x81 isócrono IN: ritmo de la placa (bytes de 44/45 = muestras por ms).
  - EP 0x86 bulk IN: hay que leerlo siempre (≈2,7 MB/s, se descarta); si no, la placa no arranca.
- **Arranque**, en este orden; sin el GET_CUR la placa acepta audio pero no reproduce:
  1. Clear halt en 0x02 y 0x86.
  2. Leer la versión dos veces: `V` (0xC0, 0x56), con wLength 8.
  3. Leer el registro `I` (0xC0, 0x49).
  4. `GET_CUR(0)` (0xA2, 0x81, 0x0100).
  5. `SET_CUR 44100` a 0x86 y después a 0x02 (0x22, 0x01).
  6. `GET_CUR(0x86)`.
  7. Leer el registro `I`.
  8. Escribir `I = 0x32` (0x40, 0x49).
- **Ritmo (lazo cerrado, por defecto):** cada milisegundo la placa informa por EP 0x81 cuántas muestras consumió (byte 0; los bytes 1 y 2 repiten los dos milisegundos anteriores). El puente le manda exactamente esa cantidad por milisegundo, con un retraso fijo de 8 ms, repartida parejo en los 8 microtramas. Si falta el ritmo, usa el promedio (≈44102,7 Hz, acumulador fraccionario), que también es el modo `--promedio`. Con el valor instantáneo repetido en vez de la secuencia se oyen pulsaciones.
- **Relojes:** para absorber la diferencia entre el reloj de la Mac y el de la placa, el puente remuestrea levemente (hasta ±0,5 %) para mantener el colchón.

## Archivos

**El puente (raíz de `~/djio`)**

- `djio_bridge.c` / `djio_bridge`: el puente.
- `com.djio.bridge.plist.template`, `instalar_servicio.command`, `desinstalar_servicio.command`: arranque automático (la plantilla no tiene rutas fijas; el instalador la completa).
- `djio.log`: registro del servicio (lo que imprime el puente cuando corre solo).
- `djio-eventos.log`: registro de eventos del puente (se crea solo, crece lentamente: una línea por minuto más los eventos).
- `djio-volcado-FECHA.wav`: "caja negra" del puente. Con el puente corriendo, `pkill -USR1 djio_bridge` guarda los últimos 12 segundos exactos de audio que se le mandaron a la placa (4 canales, 24 bits en WAV de 32). Sirve para analizar si una distorsión ya sale del puente.
- `Hardware.md`: diagnóstico y reparación de la placa (salida 2 baja, capacitores, chips, mediciones).
- `driver/`: driver oficial Numark 3.2.0 (Ploytec), solo como referencia. No se instala.

**Herramientas (`tools/`)**

No hacen falta para usar el puente: se usaron para descifrar el protocolo y diagnosticar. Se ejecutan desde `~/djio`.

- `tools/tone_to_device`: tono de prueba hacia un dispositivo, sin cambiar la salida de la Mac:

  ```
  tools/tone_to_device BlackHole <segundos> <hz 1-2> <hz 3-4> [amplitud 0-1] [canales, p. ej. 0011]
  ```

  Con una frecuencia menor a 2 Hz genera una onda cuadrada lenta (para medir con un tester en continua).
- `tools/grabar_blackhole`: graba lo que llega a BlackHole a un WAV (`tools/grabar_blackhole 20 salida.wav`), sin cambiar su configuración. Sirve para saber si un problema ya viene en el audio de origen.
- `tools/djio_probe`: comprobar si la placa contesta. **Usar solo `tools/djio_probe v`.** Sin la `v` la "arma" sin mandarle audio, y después puede quedar trabada hasta reconectarla. No usarla con el puente corriendo.
- `tools/djio_stream`: prueba de streaming con tono, de cuando se descifró el protocolo (`tools/djio_stream 44100 1 5`). No usarla con el puente corriendo.
- `tools/djio_test`: primeras pruebas, obsoletas (48 kHz y arranque incompleto). No usar.

Cada herramienta tiene su fuente al lado (`.c`). Para recompilarlas:

```bash
cd ~/djio/tools
clang -O2 -o tone_to_device tone_to_device.c -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
clang -O2 -o grabar_blackhole grabar_blackhole.c -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
clang -O2 -o djio_probe djio_probe.c -framework IOKit -framework CoreFoundation
clang -O2 -o djio_stream djio_stream.c -framework IOKit -framework CoreFoundation
```

## Créditos

Basado en el protocolo documentado por [djpallax/numark-djio-linux](https://github.com/djpallax/numark-djio-linux) y en el trabajo de [mischa85/Ozzy](https://github.com/mischa85/Ozzy) sobre los chips Ploytec.
