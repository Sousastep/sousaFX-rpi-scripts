about
=====

these scripts connect a gamepad to rnbo via bluetooth, and rnbo to the [teensy](https://github.com/Sousastep/sousaVFX-teensy) via usb serial, for use with [sousaFX-rnbo](https://github.com/Sousastep/SousaFX-rnbo)

setup overview
==============

https://rnbo.cycling74.com/learn/raspberry-pi-setup

ssh
---

`ssh pi@c74rpi.local` or `ssh pi@192.168.1.xx`

update
------

"Processing triggers for man-db" takes ages. Try this before apt-get next time:
`echo "set man-db/auto-update false" | debconf-communicate; dpkg-reconfigure man-db`
from https://askubuntu.com/a/1437819

```
sudo apt-get update && sudo apt-get upgrade
sudo apt update && sudo apt upgrade
```

mount
-----

install macfuse and sshfs https://macfuse.github.io/

`sshfs pi@192.168.1.xx:/home/pi ~/Desktop/raspberry_pi -o defer_permissions,noappledouble,nolocalcaches,volname=RaspberryPi`

clone: `cd /Users/<user>/Desktop/raspberry_pi/home/pi/;
git clone https://github.com/Sousastep/sousaFX-rpi-scripts.git`

install
-------

```
sudo apt install python3-evdev python3-liblo python3-serial build-essential liblo-dev libevdev-dev
```

enable audio interface
----------------------

this step may not be necessary

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

put controller in pairing mode, then

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

if something like this happens

```
[CHG] Device XX:XX:XX:XX:XX:XX Connected: yes
[CHG] Device XX:XX:XX:XX:XX:XX Connected: no
[CHG] Device XX:XX:XX:XX:XX:XX Connected: yes
[CHG] Device XX:XX:XX:XX:XX:XX Connected: no
[CHG] Device XX:XX:XX:XX:XX:XX Connected: yes
```

try `remove XX:XX:XX:XX:XX:XX`


build
-----

```
cd ~/sousaFX-rpi-scripts;
g++ -O3 oscserial.cpp -o oscserial -llo -lpthread;
g++ -O3 gamepad.cpp -o gamepad -llo;
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

shift-G goes to end of file (may take a sec)


connecting android to rpi
-------------------------

turn on android hotspot

connect rpi to hotspot via `sudo nmtui`: https://rnbo.cycling74.com/learn/raspberry-pi-setup#current-image-1.3.0-and-greater

find IP with termux: https://cycling74.com/forums/how-to-connect-raspberry-pi-to-android-hotspot


reinstalling
------------

when sshing after reinstalling, you'll see

```
WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!
```

delete `~/.ssh/known_hosts`


shutdown
--------

`sudo shutdown -h now`

