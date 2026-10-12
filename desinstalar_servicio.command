#!/bin/bash
# Quita el servicio del puente de la DJ|iO. El programa queda en esta carpeta.
LABEL=com.djio.bridge
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/$LABEL.plist"
echo "Servicio quitado."
