############################################################################
# https://www.lysdexic.com/light-up-some-leds-with-rnbo-on-the-raspberry-pi/
# https://cycling74.com/forums/sending-osc-from-rnbo-to-madmapper#reply-65f041d91371430013ba8bd8
################################################################################################

import liblo as OSC     # https://dsacre.github.io/pyliblo/doc/
import sys              # https://docs.python.org/3/library/sys.html
import serial           # https://www.pyserial.com/docs
import time             # https://docs.python.org/3/library/time.html

# Replace with the correct serial port, often /dev/ttyACM0 or /dev/ttyUSB0 for Teensy
SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200  # Match the baud rate with what's on your Teensy

# Try to open serial connection
def connect_serial(max_retries=5, retry_delay=10):
    for attempt in range(max_retries):
        try:
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
            time.sleep(2) # Give the Teensy time to reset after serial connect
            print(f"Successfully connected to {SERIAL_PORT}")
            return ser
        except serial.SerialException as e:
            print(f"Connection attempt {attempt + 1}/{max_retries} failed: {e}")
            if attempt < max_retries - 1:
                print(f"Retrying in {retry_delay} seconds...")
                time.sleep(retry_delay)

    print(f"Failed to connect to {SERIAL_PORT} after {max_retries} attempts")
    return None

ser = connect_serial()
if ser is None:
    sys.exit(1)

try:
    target = OSC.Address(1234)
except OSC.AddressError as err:
    print(err)
    time.sleep(8)
    sys.exit(1)

try:
    server = OSC.Server(4321)
    print(f"OSC server started and listening on port 4321")
except OSC.ServerError as err:
    print(err)
    time.sleep(8)
    sys.exit(1)

# Configuration for parameters
PARAMS = {
    'brightness':       {'index': 0, 'route': '/rnbo/inst/1/messages/out/brightness'},
    'radius':           {'index': 1, 'route': '/rnbo/inst/1/messages/out/radius'},
    'palette':          {'index': 2, 'route': '/rnbo/inst/1/messages/out/palette'},
    'divisionsHi':      {'index': 3, 'route': '/rnbo/inst/1/messages/out/divisionsHi'},
    'divisionsLo':      {'index': 4, 'route': '/rnbo/inst/1/messages/out/divisionsLo'},
    'width':            {'index': 5, 'route': '/rnbo/inst/1/messages/out/width'},
    'curve':            {'index': 6, 'route': '/rnbo/inst/1/messages/out/curve'},
    'rotation':         {'index': 7, 'route': '/rnbo/inst/1/messages/out/rotation'},
    'fadeIn':           {'index': 8, 'route': '/rnbo/inst/1/messages/out/fadeIn'},
    'fadeOut':          {'index': 9, 'route': '/rnbo/inst/1/messages/out/fadeOut'},
    'peakPosition':     {'index': 10, 'route': '/rnbo/inst/1/messages/out/peakPosition'},
    'pattern':          {'index': 11, 'route': '/rnbo/inst/1/messages/out/pattern'},
    'gradientOffset':   {'index': 12, 'route': '/rnbo/inst/1/messages/out/gradientOffset'},
}

tx_buffer = bytearray([254] + [0]*13 + [255])

def make_handler(index):
    def handler(path, args):
        # Update the buffer directly (index + 1 because of the 254 start byte)
        tx_buffer[index + 1] = int(args[0])
    return handler

# Register handlers
for config in PARAMS.values():
    server.add_method(config['route'], 'i', make_handler(config['index']))

FPS = 320
ns_per_frame = 1000000000 / FPS
next_frame = time.time_ns()

try:
    while True:
        # Handle all pending OSC messages immediately
        while server.recv(0):
            pass

        # Precise timing for Serial Write
        current_time = time.time_ns()
        if current_time >= next_frame:
            ser.write(tx_buffer)
            # Schedule next frame relative to the last one to prevent drift
            next_frame += ns_per_frame

def fallback(path, args):
    pass

# Finally add fallback method for unhandled OSC addrs
server.add_method(None, None, fallback)

OSC.send(target, "/rnbo/listeners/add", f"127.0.0.1:4321")

except KeyboardInterrupt:
    print("Exiting cleanly...")
finally:
    ser.close()
    print("Serial port closed")