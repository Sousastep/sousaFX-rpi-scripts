about
=====

these scripts connect a gamepad to rnbo via bluetooth, and rnbo to the [teensy](https://github.com/Sousastep/sousaVFX-teensy) via usb serial, for use with [sousaFX-rnbo](https://github.com/Sousastep/SousaFX-rnbo)

setup overview
==============

https://rnbo.cycling74.com/learn/raspberry-pi-setup

mount
-----

install macfuse and sshfs https://macfuse.github.io/

`sshfs pi@192.168.1.xx:/home/pi ~/Desktop/raspberry_pi -o defer_permissions,noappledouble,nolocalcaches,volname=RaspberryPi`

clone
-----

`cd /Users/<user>/Desktop/raspberry_pi/home/pi/Documents/
git clone https://github.com/Sousastep/sousaFX-rpi-scripts.git`

ssh
---

`ssh pi@c74rpi.local` or `ssh pi@192.168.1.xx`

update packages
---------------

https://rnbo.cycling74.com/learn/working-with-the-raspberry-pi-target#after-exporting-to-my-pi-i-see-message-that-there-are-outdated-packages-on-my-system
```
sudo apt-get update
sudo apt-get upgrade
sudo apt update && sudo apt upgrade

sudo apt-get install liblo-dev
sudo apt install python3-evdev python3-liblo python3-serial
```

enable audio interface
----------------------

https://rnbo.cycling74.com/learn/working-with-the-raspberry-pi-target#help-my-usb-audio-interface-isnt-showing-upis-crashing-the-runner

```
pi@c74rpi:/home $ cat /proc/asound/cards
 0 [Dummy          ]: Dummy - Dummy
                      Dummy 1
 1 [vc4hdmi0       ]: vc4-hdmi - vc4-hdmi-0
                      vc4-hdmi-0
 2 [vc4hdmi1       ]: vc4-hdmi - vc4-hdmi-1
                      vc4-hdmi-1
 3 [AMS22          ]: USB-Audio - AMS-22
                      ZOOM Corporation AMS-22 at usb-xhci-hcd.0-1, high speed
```

~/.asoundrc
```
defaults.pcm.card 3
defaults.pcm.device 0
defaults.ctl.card 3
```

or
```
cd sousaFX-rpi-scripts/
cp .asoundrc ~/.asoundrc
```

enable bluetooth and pair controller
------------------------------------

```
sudo bluetoothctl
power on
agent on
default-agent
scan on
scan off
pair [XX:XX:XX:XX:XX:XX]
connect [XX:XX:XX:XX:XX:XX]
trust [XX:XX:XX:XX:XX:XX]
exit
```

auto-run scripts on startup
---------------------------

```
cd sousaFX-rpi-scripts/
sudo cp gamepad.service /etc/systemd/system/
sudo cp oscserial.service /etc/systemd/system/
sudo systemctl enable gamepad.service && sudo systemctl enable oscserial.service
```

run
```
sudo systemctl start gamepad.service
sudo systemctl start oscserial.service
```

status
```
sudo systemctl status gamepad.service && sudo systemctl status oscserial.service
```

logs
```
sudo journalctl -u gamepad.service
sudo journalctl -u oscserial.service
```

shutdown
--------

`sudo shutdown -h now`
