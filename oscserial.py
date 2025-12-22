############################################################################
# https://www.lysdexic.com/light-up-some-leds-with-rnbo-on-the-raspberry-pi/
# https://cycling74.com/forums/sending-osc-from-rnbo-to-madmapper#reply-65f041d91371430013ba8bd8
################################################################################################

import liblo as OSC
import sys
import serial
import time

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
    'divisions':        {'index': 3, 'route': '/rnbo/inst/1/messages/out/divisions'},
    'width':            {'index': 4, 'route': '/rnbo/inst/1/messages/out/width'},
    'curve':            {'index': 5, 'route': '/rnbo/inst/1/messages/out/curve'},
    'rotation':         {'index': 6, 'route': '/rnbo/inst/1/messages/out/rotation'},
    'fadeIn':           {'index': 7, 'route': '/rnbo/inst/1/messages/out/fadeIn'},
    'fadeOut':          {'index': 8, 'route': '/rnbo/inst/1/messages/out/fadeOut'},
    'peakPosition':     {'index': 9, 'route': '/rnbo/inst/1/messages/out/peakPosition'},
    'pattern':          {'index': 10, 'route': '/rnbo/inst/1/messages/out/pattern'},
    'gradientOffset':   {'index': 11, 'route': '/rnbo/inst/1/messages/out/gradientOffset'},
}

# Store current values in a list (order matches the message format)
current_values = [90, 253, 0, 2, 201, 126, 231, 59, 0, 128, 0, 0]

def clamp_value(value, min_val=0, max_val=253):
    """Clamp value to valid range."""
    return max(min_val, min(max_val, int(value)))

def send_message():
    """Send current state as a serial message."""
    message = bytes([254] + current_values + [255])
    ser.write(message)

def make_handler(param_name):
    """Factory function to create handlers for each parameter."""
    param_index = PARAMS[param_name]['index']
    
    def handler(path, args):
        current_values[param_index] = clamp_value(args[0])
        send_message()
    
    return handler

# Register handlers for all parameters
for param_name, config in PARAMS.items():
    handler = make_handler(param_name)
    server.add_method(config['route'], 'i', handler)

# Finally add fallback method for unhandled OSC addrs
server.add_method(None, None, fallback)

OSC.send(target, "/rnbo/listeners/add", f"127.0.0.1:4321")

try:
    while True:
        server.recv(100)  # Timeout in milliseconds

except KeyboardInterrupt:
    print("Exiting cleanly...")
finally:
    ser.close()
    print("Serial port closed")