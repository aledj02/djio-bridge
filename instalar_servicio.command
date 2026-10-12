#!/bin/bash
# Instala el puente de la DJ|iO como servicio: arranca solo al iniciar sesión y
# cada vez que se enchufa la placa. No pide contraseña (es un servicio del usuario).
set -e
cd "$(dirname "$0")"
DIR="$(pwd)"
LABEL=com.djio.bridge
DEST="$HOME/Library/LaunchAgents/$LABEL.plist"

if [ ! -x "$DIR/djio_bridge" ]; then
    echo "Falta el programa djio_bridge: hay que compilarlo primero (ver LEEME.md, sección Compilar)."
    exit 1
fi

mkdir -p "$HOME/Library/LaunchAgents"
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
# El plist del proyecto es una plantilla: @DIR@ se reemplaza por esta carpeta.
sed "s|@DIR@|${DIR//&/\\&}|g" "$LABEL.plist.template" > "$DEST"
launchctl bootstrap "gui/$(id -u)" "$DEST"
echo "Servicio instalado ($LABEL). Registro: $DIR/djio.log"
