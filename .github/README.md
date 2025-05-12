about
=====

these scripts connect a gamepad to rnbo via bluetooth, and rnbo to the teensy via usb serial, for use with [SousaFX-rnbo](https://github.com/Sousastep/SousaFX-rnbo)

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

sudo apt-get upgrade
sudo apt-get update
sudo apt upgrade
sudo apt update

sudo apt install python3-venv python3-full python3-dev build-essential
sudo apt-get install liblo-dev
sudo apt install python3-evdev python3-liblo python3-serial


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

shutdown
--------

`sudo shutdown -h now`

