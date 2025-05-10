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
BAUD_RATE = 9600  # Match the baud rate with what's on your Teensy

# Try to open serial connection
try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    # Give the Teensy time to reset after serial connect
    time.sleep(2)
    print(f"Successfully connected to {SERIAL_PORT}")
except serial.SerialException as e:
    print(f"Error opening serial port {SERIAL_PORT}: {e}")
    sys.exit(1)  # Exit with error code

try:
    server = OSC.Server(4321)
except OSC.ServerError as err:
    print(err)

# Global variables to store the current values
current_env = 0
current_palette = 0

def handle_env(path, args):
    global current_env, current_palette
    i = args[0]
    
    # Ensure i is a valid byte, and not a start or end marker, by clipping
    current_env = max(0, min(253, int(i)))
    
    # Create a byte sequence with start marker (254), env byte, palette byte, and end marker (255)
    message = bytes([254, current_env, current_palette, 255])
    ser.write(message)
    # time.sleep(0.001)  # 1ms delay to allow processing

def handle_palette(path, args):
    global current_env, current_palette
    p = args[0]
    
    # Ensure p is a valid byte, and not a start or end marker, by clipping
    current_palette = max(0, min(253, int(p)))
    
    # Create a byte sequence with start marker (254), env byte, palette byte, and end marker (255)
    message = bytes([254, current_env, current_palette, 255])
    ser.write(message)

def fallback(path, args, types, src):
    pass
    # print("got unknown message '%s' from '%s'" % (path, src.url))
    # print("don't panic - probably just the runner echoing back your changes :)")
    # for a, t in zip(args, types):
    #     print("argument of type '%s': %s" % (t, a))

# register callback methods for server routes
server.add_method("/rnbo/inst/0/messages/out/vfx_env", 'i', handle_env)
server.add_method("/rnbo/inst/0/messages/out/vfx_palette_number", 'p', handle_palette)

# Finally add fallback method for unhandled OSC addrs
server.add_method(None, None, fallback)

# Set up RNBO OSC listener
OSC.send(target, "/rnbo/listeners/add", f"127.0.0.1:4321")

try:
    while True:
        server.recv(8)        
        
except KeyboardInterrupt:
    print("exiting cleanly...")
finally:
    ser.close()
