#!/bin/bash

clear

# elevate to root privileges
[ "$UID" -eq 0 ] || exec sudo bash "$0" "$@"

# write udev rule to allow yocto3d accelerometer USB access to all users
sudo echo '# udev rules to allow write access to all users for Yoctopuce USB devices' > /etc/udev/rules.d/90-yocto3d.rules
sudo echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="24e0", MODE="0666"
' >> /etc/udev/rules.d/90-yocto3d.rules

# restart udev rules (allow usb input to all users)
sudo udevadm control --reload-rules
sudo udevadm trigger


