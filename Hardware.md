# Numark DJ|iO: diagnóstico y reparación del hardware

Registro de cómo se encontró y se arregló la salida 2 baja de esta DJ|iO (2026-10-11). Para el uso del puente de software, ver `LEEME.md`.

## Resumen

- **Síntoma:** la salida 2 (RCA J7 y jack frontal de auriculares) sonaba unos **24 dB más baja** que la salida 1, en los dos canales, y con música se escuchaba algo más aguda (menos graves).
- **Causa:** **capacitores electrolíticos de acople de 10 µF degradados** en el camino de la salida 2. No tenían hinchazón ni pérdida de líquido: no se veía nada a simple vista.
- **Capacitores afectados** (todos de la salida 2): **C22 y C74** en el canal izquierdo (RCA blanca de J7), y **C48 y C68** en el canal derecho (RCA roja). De la salida 1 no se cambió nada.
- **Reparación:** se reemplazaron los capacitores dañados por electrolíticos nuevos de **22 µF** (los que había a mano, en vez de los 10 µF originales) respetando la polaridad de la placa.
- **Resultado:** la salida 2 quedó **pareja con la salida 1**, a oído, con un tono de 1 kHz por las cuatro salidas. Falta probarla un buen rato con música real.
- **Compensación en el puente:** mientras la placa estuvo así, el puente bajaba la salida 1 −24 dB. Ya no hace falta: `GANANCIA_SALIDA1_DB` y `GANANCIA_SALIDA2_DB` en `djio_bridge.c` quedaron en `0.0`.

## La placa

**NUMARK NIO2 MAIN PCB 9-40-0430-C, APR-24-2007.** Salida 1 = par de RCA **J6**. Salida 2 = par de RCA **J7** (la blanca es el canal izquierdo). Es la DJ|iO original (USB 15e4:0070), con electrónica de Ploytec.

**Chips** (según el usuario, son todos los de la placa):

| Ref. | Marca | Qué es |
|------|-------|--------|
| U1, U2 | `345C 0733` | **Probablemente CS4345**, de Cirrus Logic: DAC estéreo de 24 bits, 10 patas. Uno por salida. *Es una inferencia:* el marcado "345C" no figura en ninguna tabla que se encontró. Se apoya en las 10 patas, los 4 electrolíticos de acople al lado y que las mediciones coincidieron con la hoja de datos de ese chip. |
| U3 | JRC 4580 | NJM4580, dos operacionales de audio (probablemente el preamplificador de micrófono) |
| U4 | ISP1583BS | Controlador USB 2.0 de alta velocidad de NXP |
| U6, U9 | JRC 2112 | NJM2112, cuatro operacionales de bajo voltaje cada uno. Los buffers de salida |
| U7 | ATMEGA162 16AU | Microcontrolador AVR: el firmware que contesta los comandos `V` e `I` |
| U8 | LC4032V | CPLD Lattice ispMACH 4032V: reparte las señales digitales (audio I2S, relojes) |
| Regulador | `25 330 H5 2C` | Regulador lineal de 3,3 V (marcado "330"). No se identificó el modelo exacto ni su tensión máxima de entrada |

**Alimentación:** 5 V desde el USB (el descriptor declara alimentación por el bus, máximo 480 mA). Los DACs trabajan con 3,3 V. Hay una fuente negativa generada en la placa (se midió −3,7 V en la pata 11 de los operacionales, medida contra el punto de masa que se usó, que podría no ser exactamente la masa de los chips). Tiene un conector de fuente externa; según vendedores de la placa (no se encontró el manual oficial) es de **6 V DC, positivo al centro (2,1 × 5,5 mm)**, pero un listado dice 9 V. **No se probó ninguna fuente externa** y no se verificó el voltaje: confirmarlo en la placa antes de conectar nada.

**Hoja de datos del CS4344/5/8** (Cirrus, `CS4344-45-48_F2.pdf`). Pinout de 10 patas:

| 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|
| SDIN | DEM/SCLK | LRCK | MCLK | VQ | FILT+ | AOUTL | GND | VA | AOUTR |

- Las patas 1 a 5 están de un lado y la 6 a 10 del opuesto (la 6 queda enfrente de la 5). Contar mal ese lado da lecturas que parecen inconsistentes.
- Salida a fondo de escala: 0,60 a 0,70 × VA pico a pico. VQ (reposo) = 0,5 × VA.
- El formato de datos y el reloj se detectan solos; no hay registro de volumen.
- Los electrolíticos de 10 µF C26 a C29 son los de acople de las salidas del DAC, como en el circuito típico del fabricante.

**Operacionales U6 y U9** (14 patas, distribución típica de un cuádruple): las salidas son las cuatro esquinas del chip, **patas 1, 7, 8 y 14**. La pata 1 es la del punto. La alimentación está en las patas 4 y 11.

**Componentes cerca de las RCA** (de las fotos; las funciones exactas no se verificaron con un plano):

| | Salida 2 (J7), canal A | Salida 2 (J7), canal B | Salida 1 (J6) |
|---|---|---|---|
| Transistor | Q7 | Q6 | Q5 (y Q4) |
| 1 kΩ (`102`) | R71 | R68 | R65 |
| 100 Ω (`1000`) | R70 | R67 | R64 |
| 47 kΩ (`473`) | R69 | R66 | R63 |
| Cerámico | C76 | C75 | C73 |

Hay una red de cuatro resistencias de 47 kΩ en un solo encapsulado, marcada `473` (R55), junto a U9. No tiene un gemelo cerca de U6.

## Cómo se llegó a la causa

Todas las comparaciones de nivel se hicieron con el puente de software con las ganancias en 0 (`./djio_bridge --ganancia1 0 --ganancia2 0`), así las dos salidas reciben el mismo nivel digital. Los niveles se juzgaron **a oído o con los medidores de un mixer**, no con instrumentos calibrados.

**1. ¿Software o hardware?**

- Con el mismo tono digital por las dos salidas, la 2 sonaba más baja: hardware.
- Tonos por un solo canal a la vez: el mapeo es el correcto (canales 1-2 → salida 1, canales 3-4 → salida 2).
- A oído, la salida 1 hubo que bajarla **24 dB** para igualar a la 2. Esa fue la compensación que usó el puente hasta la reparación.
- Se confirmó que la salida 2 sigue el nivel digital: tonos cada vez más fuertes suben parejo.

**2. Descartado**

- Traktor y el puente (la pérdida estaba con un tono directo a BlackHole).
- El mixer y los cables (el problema sigue a la salida de la placa).
- El interruptor del jack frontal (no se usa, y la pérdida es igual por RCA y por el jack).
- Alimentación del puerto USB: la placa estuvo alimentada por un hub con fuente propia y no cambió.
- Los transistores del tipo "mute" (Q4 a Q7): resistencia RCA–masa, sin alimentación, igual en las 4 salidas, y la misma tensión en su pata de base en los cuatro.
- Capacitores hinchados o con pérdida de líquido: no había ninguno visible.

**3. Los DACs están bien**

Con el DAC a 3,3 V, en reposo: VQ y las dos salidas ≈ 1,66 V, y FILT+ ≈ 3,3 V, igual en los dos chips. Luego se mandó una onda cuadrada muy lenta (`tools/tone_to_device BlackHole 60 0.1 0.1 0.9`, cambia cada 5 s) y se midió en continua la salida AOUTR (pata 10) de cada DAC: **los dos oscilan entre 0,67 V y 2,72 V** (≈ 2,05 Vpp), dentro de la hoja de datos. Conclusión: USB, firmware, CPLD, datos, alimentación y los dos DACs funcionan. **La pérdida ocurre después del DAC.**

**4. Seguir la señal con un mixer**

Con un tono de 1 kHz y la entrada de línea de un mixer conectada a través de un capacitor cerámico de 100 nF en serie (masa a la malla de una RCA de la placa):

- Con el tono solo por la salida 2, aparece señal en **U6, patas 7 y 8** (sus dos canales). La pata 7 es el canal 3 (izquierdo).
- U9 (patas 7 y 8) y U6 pata 7 tienen el mismo nivel. **U6 pata 8 sale baja.**
- J6: los dos canales al mismo nivel. J7: baja respecto a J6, y la RCA **blanca (izquierda) la más baja**, aproximadamente la mitad que la roja.
- Es decir: el canal de la pata 7 sale bien del operacional y pierde nivel después, y el de la pata 8 ya sale bajo del chip. Dos tramos distintos, ambos con el mismo tipo de causa.

**5. El método que encontró los capacitores**

Un electrolítico de acople de 10 µF sano deja pasar un tono de 1 kHz casi sin atenuarlo (su impedancia es unos 16 Ω). Entonces, **con el tono sonando por un canal, se toca con la punta (a través del capacitor de 100 nF) cada pata de cada electrolítico de la zona y se compara el nivel de un lado contra el otro**. En un capacitor sano el nivel es igual a los dos lados. En **C22 y C74** el nivel era **muy bajo de un lado respecto del otro**, y después se encontraron otros dos capacitores en el mismo estado, del canal derecho: **C48 y C68**.

Prueba de confirmación (sin desoldar): poner el cerámico de 100 nF en paralelo con el capacitor sospechoso y ver si el nivel sube. A 1 kHz, 100 nF dejan pasar casi toda la señal, así que alcanza para detectar un capacitor abierto.

**6. Reparación**

Se reemplazaron los cuatro capacitores dañados: C22 y C74 (canal izquierdo) y C48 y C68 (canal derecho). No se cambió ningún componente de la salida 1. Los cuatro se reemplazaron por electrolíticos de **22 µF** (los que había a mano) en vez de 10 µF. En un capacitor de acople un valor mayor solo deja pasar más graves; lo único que cambia es que al encender puede haber un golpecito más fuerte. Se respetó la polaridad: el "+" de la placa hacia el lado con más tensión continua (en las salidas de los DACs, hacia el DAC). Tras el cambio, el tono de 1 kHz por las cuatro salidas se escuchó parejo.

## Dudas y límites de este diagnóstico

- Los niveles se compararon a oído y con medidores de un mixer, no con instrumentos; "24 dB" y "la mitad" son estimaciones.
- La identificación de U1 y U2 como CS4345 es una inferencia (ver arriba).
- Nunca se confirmó cuál capacitor corresponde a cuál tramo del circuito, porque no se tiene el esquema; solo se sabe que los dañados (C22, C74, C48 y C68) eran los que mostraban caída de un lado al otro. Lo que se sabe: C22 y C74 son del canal izquierdo y C48 y C68 del derecho; en las fotos, C74 está cerca de J7 y C22 del lado de los jacks frontales. De la posición de C48 y C68 en la placa no se anotó nada.
- La pata 8 de U6 salía baja desde el propio chip (antes de su capacitor de salida). Se asume que corresponde a un capacitor de acople de entrada degradado, pero no se verificó cuál.
- No se probó la fuente externa ni la respuesta en graves con tonos de 100 Hz.

## Si vuelve a pasar

1. Abrir el puente con las ganancias en 0: `./djio_bridge --ganancia1 0 --ganancia2 0`.
2. Mandar 1 kHz por la salida con el problema (`tools/tone_to_device BlackHole 300 1000 1000 0.25 0011` para la salida 2, o `1100` para la 1).
3. Con un mixer y un capacitor de 100 nF en serie, comparar el nivel a los dos lados de cada electrolítico de 10 µF de la zona. El que cae mucho de un lado al otro es el dañado.
4. Como los demás electrolíticos de la placa tienen la misma edad, conviene cambiar también los vecinos del mismo grupo.
5. Si fuera necesario compensar mientras tanto, bajar la otra salida con `--ganancia1` o `--ganancia2` (en dB), documentándolo en el código.
