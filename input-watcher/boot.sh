#!/bin/sh
# selfroot boot module: (re)create and start tizgb-input-access.service.
# Modelled on modules/tiznes-input/boot.sh; independent unit, config, state.
set -eu
MODULE_DIR=${MODULE_DIR:-/opt/usr/share/selfroot/modules/tizgb-input}
if [ -f "$MODULE_DIR/disabled" ] || [ -s /home/owner/share/tmp/sdk_tools/selfroot-ui/modules/disabled/tizgb-input ]; then
    systemctl stop tizgb-input-access.service >/dev/null 2>&1 || true
    exit 0
fi
cat > /run/systemd/system/tizgb-input-access.service <<UNIT
[Unit]
Description=Grant the selected physical USB controller to TizGB
DefaultDependencies=false
After=local-fs.target
[Service]
Type=simple
ExecStart=/bin/sh -c "/bin/cat $MODULE_DIR/watch.sh | /bin/sh"
Restart=on-failure
RestartSec=2
KillMode=control-group
TimeoutStopSec=5
UNIT
systemctl daemon-reload
systemctl restart tizgb-input-access.service
systemctl is-active tizgb-input-access.service
