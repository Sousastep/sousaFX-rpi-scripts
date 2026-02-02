############################################################################
# https://www.lysdexic.com/light-up-some-leds-with-rnbo-on-the-raspberry-pi/
# https://cycling74.com/forums/sending-osc-from-rnbo-to-madmapper#reply-65f041d91371430013ba8bd8
################################################################################################

import liblo as OSC
import sys
import serial
import time

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200

def connect_serial(max_retries=5, retry_delay=10):
    for attempt in range(max_retries):
        try:
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
            time.sleep(2) 
            print(f"Successfully connected to {SERIAL_PORT}")
            return ser
        except serial.SerialException as e:
            print(f"Connection attempt {attempt + 1}/{max_retries} failed: {e}")
            if attempt < max_retries - 1:
                time.sleep(retry_delay)
    return None

ser = connect_serial()
if ser is None:
    sys.exit(1)

# Initialize OSC Address and Server
try:
    target = OSC.Address(1234)
    server = OSC.Server(4321)
    print(f"OSC server started on port 4321")
except (OSC.AddressError, OSC.ServerError) as err:
    print(f"OSC Setup Error: {err}")
    ser.close()
    sys.exit(1)

PARAMS = {
    'brightness':     {'index': 0, 'route': '/rnbo/inst/1/messages/out/brightness'},
    'radius':         {'index': 1, 'route': '/rnbo/inst/1/messages/out/radius'},
    'palette':        {'index': 2, 'route': '/rnbo/inst/1/messages/out/palette'},
    'divisionsHi':    {'index': 3, 'route': '/rnbo/inst/1/messages/out/divisionsHi'},
    'divisionsLo':    {'index': 4, 'route': '/rnbo/inst/1/messages/out/divisionsLo'},
    'width':          {'index': 5, 'route': '/rnbo/inst/1/messages/out/width'},
    'curve':          {'index': 6, 'route': '/rnbo/inst/1/messages/out/curve'},
    'rotation':       {'index': 7, 'route': '/rnbo/inst/1/messages/out/rotation'},
    'fadeIn':         {'index': 8, 'route': '/rnbo/inst/1/messages/out/fadeIn'},
    'fadeOut':        {'index': 9, 'route': '/rnbo/inst/1/messages/out/fadeOut'},
    'peakPosition':   {'index': 10, 'route': '/rnbo/inst/1/messages/out/peakPosition'},
    'pattern':        {'index': 11, 'route': '/rnbo/inst/1/messages/out/pattern'},
    'gradientOffset': {'index': 12, 'route': '/rnbo/inst/1/messages/out/gradientOffset'},
}

tx_buffer = bytearray([254] + [0]*13 + [255])

def fallback(path, args):
    pass

def make_handler(index):
    def handler(path, args):
        tx_buffer[index + 1] = int(args[0])
    return handler

# Register handlers and fallback BEFORE the loop
for config in PARAMS.values():
    server.add_method(config['route'], 'i', make_handler(config['index']))

server.add_method(None, None, fallback)

# Tell RNBO to start sending messages to us
OSC.send(target, "/rnbo/listeners/add", "127.0.0.1:4321")

FPS = 260
ns_per_frame = 1000000000 // FPS  # Use integer division
next_frame = time.time_ns()

print("Entering main loop...")

try:
    while True:
        # 1. Handle OSC messages
        while server.recv(0):
            pass

        # 2. Precise timing for Serial Write
        current_time = time.time_ns()
        if current_time >= next_frame:
            ser.write(tx_buffer)
            next_frame += ns_per_frame

except KeyboardInterrupt:
    print("\nStopping script...")
except Exception as e:
    print(f"\nUnexpected error: {e}")
finally:
    ser.close()
    print("Serial port closed. Cleanup complete.")