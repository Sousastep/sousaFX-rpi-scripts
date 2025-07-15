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

# Global variables to store the current values
current_env = 0
current_palette = 0
current_vfxtype = 0

def handle_env(path, args):
    global current_env, current_palette, current_vfxtype
    i = args[0]
    
    # Ensure i is a valid byte, and not a start or end marker, by clipping
    current_env = max(0, min(253, int(i)))
    
    # Create a byte sequence with start marker (254), env byte, palette byte, and end marker (255)
    message = bytes([254, current_env, current_palette, current_vfxtype, 255])
    ser.write(message)

def handle_palette(path, args):
    global current_env, current_palette, current_vfxtype
    i = args[0]
    
    # Ensure p is a valid byte, and not a start or end marker, by clipping
    current_palette = max(0, min(253, int(i)))
    
    # Create a byte sequence with start marker (254), env byte, palette byte, and end marker (255)
    message = bytes([254, current_env, current_palette, current_vfxtype, 255])
    ser.write(message)

def handle_vfxtype(path, args):
    global current_env, current_palette, current_vfxtype
    i = args[0]
    
    # Ensure p is a valid byte, and not a start or end marker, by clipping
    current_vfxtype = max(0, min(253, int(i)))
    
    # Create a byte sequence with start marker (254), env byte, palette byte, and end marker (255)
    message = bytes([254, current_env, current_palette, current_vfxtype, 255])
    ser.write(message)

def fallback(path, args, types, src):
    pass

# register callback methods for server routes
server.add_method("/rnbo/inst/0/messages/out/vfx_env", 'i', handle_env)
server.add_method("/rnbo/inst/0/messages/out/vfx_palette_number", 'i', handle_palette)
server.add_method("/rnbo/inst/0/messages/out/wobble_or_solo", 'i', handle_vfxtype)

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