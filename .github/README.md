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
sudo apt install python3-evdev python3-liblo python3-serial build-essential liblo-dev libevdev-dev evtest
```

enable audio interface
----------------------

this step is generally not necessary

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
g++ -O3 -std=c++17 -march=native -mtune=native -ffast-math oscserial.cpp -o oscserial -llo -lpthread;
g++ -O3 -std=c++17 -march=native -mtune=native -flto gamepad.cpp -o gamepad -llo;
```



new build? restart service:
```
sudo systemctl restart oscserial.service
```


auto-run scripts on startup
---------------------------

```
cd sousaFX-rpi-scripts/;
sudo cp gamepad.service /etc/systemd/system/;
sudo cp oscserial.service /etc/systemd/system/;
sudo cp flucoma_pitch_osc.service /etc/systemd/system/;
sudo systemctl enable gamepad.service;
sudo systemctl enable oscserial.service;
sudo systemctl enable flucoma_pitch_osc.service;
```

run
```
sudo systemctl start gamepad.service;
sudo systemctl start oscserial.service;
sudo systemctl start flucoma_pitch_osc.service;
```

status
```
sudo systemctl status gamepad.service;
sudo systemctl status oscserial.service;
sudo systemctl status flucoma_pitch_osc.service;
```

logs
```
sudo journalctl -u gamepad.service
sudo journalctl -u oscserial.service
sudo journalctl -u flucoma_pitch_osc.service;
```

shift-G goes to end of file (may take a sec)


connecting android to rpi
-------------------------

turn on android hotspot

connect rpi to hotspot via `sudo nmtui`: https://rnbo.cycling74.com/learn/raspberry-pi-setup#current-image-1.3.0-and-greater

find IP with termux: https://cycling74.com/forums/how-to-connect-raspberry-pi-to-android-hotspot


log2ram
-------

write logs to ram instead of SD card

https://github.com/azlux/log2ram


recording
---------

`sudo service rnbooscquery stop` allows audio to be recorded via
`arecord -D hw:AMS24 -f S32_LE -r 48000 -c 2 -d 5 test.wav`


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


Exporting SousaFX & SousaVFX to rPI
===================================

delete old files
----------------

Turn on the rPI with its audio interface attached.

Open https://c74rpi.local:3000/

In Graph Editor, delete all devices.

In Manage Resources > Graphs, delete all graphs.

In Manage Resources > Patchers, delete all patchers.

In Graph Editor > Open Graph Presets Menu > three dots to the right of "initial" > Delete.

In Settings > Devices, uncheck all Auto Connect toggles.


export sousafx
--------------

Check `cd ~/Documents/Max\ 8/Projects/SousaFX-rnbo && git status` for uncomitted changes.

Delete any maxsnaps in `/SousaFX-rnbo/data/`.

Open `SousaFX-rnbo.maxproj`

Select preset number 2: "Sousa AMS-24"

Disable the custom menubar with `option m`, or `menubar > File > Max Menus`.

Exit presentation mode with `option command e`, or `menubar > View > Presentation`.

Click the "remove certain IO before exporting to rPI" button.

Click the "open subpatcher containing sousaFX-rnbo~" button.

Press `option command m`, or click the pencil icon in the bottom left, to modify read-only.

Press `command e`, or click the lock icon in the bottom left, to edit.

Select the `rnbo~ @patchername SousaFX-rnbo ...` object.

Click the "Show Snapshots" button on the right.

Click "New..." to create a new snapshot using the contents of the current preset.

Click the circle above "New..." to embed the snapshot so it can be exported to the rPI.

Press `command e`, or click the lock icon in the bottom left, to lock.

Double-click the `rnbo~ @patchername SousaFX-rnbo ...` object.

Click "Show Export Sidebar".

Click c74rpi.

Uncheck "Migrate Presets".

Click the "Export to selected target" button.

Optional: ssh into the rPI and run `htop` to watch the compilation run. 

Devices > Open Device Preset Menu > Click "SousaFX-rnbo" to load preset.

Devices > Parameters > Search for "noise", check that "noise gate thresh" param matches max preset.

If not, reload the browser tab, then reload the preset.


export sousavfx
---------------

Check `cd ~/Documents/Max\ 8/Projects/sousaVFX-teensy && git status` for uncomitted changes.

Open `SousaVFX-maxteensy.maxproj`

Exit presentation mode with `option command e`, or `menubar > View > Presentation`.

Click the "open" button.

Click "Show Export Sidebar".

Click c74rpi.

Uncheck "Migrate Presets" and "Include Presets".

Click the "Export to selected target" button.


setup correct initialization
----------------------------

Goto https://c74rpi.local:3000/ Graph Editor

Add Node > both patchers

Connect like this: [pic]

Devices > Open Device Preset Menu > Click "SousaFX-rnbo" to load preset.

Three dots to the right of "SousaFX-rnbo" (Preset Actions) > Load on Startup

Graph Editor > Open Graph Menu > Save Graph As > sousastep > Save Graph

Open Graph Menu > Manage Graphs > Configure Startup Settings > Load graph sousastep > save

test with `sudo reboot`


notes on htop
-------------

Check `htop`. Using ~90% of one single core?

`sudo reboot`

Now `htop` shows utilization spread across all 4 cores...

Note: sousafx will crackle while htop is running.


notes on Zoom AMS-24
--------------------

With an Audio-Technica Pro35 phantom-powered mic, a mic gain between 2.5 and 2.9 is best. Watch for the red clipping LED.

Best audio from Zoom to Minirig:

Zoom dual 1/4" outs > 
dual 1/4" TS to single 1/4" TRS cable > 
socket/socket 1/4" TRS adapter > 
1/4" to 1/8" adapter > 
1/8" TRS cable > 
Minirig aux input

Using the Zoom's headphone out to run the Minirig results in lackluster volume levels.

Zoom's Dual 1/4" outputs, at +4 dBu, are ~3 dB quieter than the digital output.

