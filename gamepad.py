from evdev import InputDevice, ecodes, list_devices, ff
import liblo as OSC
import sys
import time

TARGET_NAME = "Xbox Wireless Controller"
OSC_PORT = 1234
BASE_PATH = "/rnbo/inst/0/params/gpin/"
HEARTBEAT_INTERVAL = 60  # seconds between soft rumbles

EVENT_MAP = {
    # Analog (EV_ABS)
    ecodes.ABS_X: "ABS_X",
    ecodes.ABS_Y: "ABS_Y",
    ecodes.ABS_Z: "ABS_Z",
    ecodes.ABS_RZ: "ABS_RZ",
    ecodes.ABS_BRAKE: "ABS_BRAKE",
    ecodes.ABS_GAS: "ABS_GAS",
    ecodes.ABS_HAT0X: "ABS_HAT0X",
    ecodes.ABS_HAT0Y: "ABS_HAT0Y",
    # Buttons (EV_KEY)
    ecodes.BTN_TL: "BTN_TL",
    ecodes.BTN_TR: "BTN_TR",
    ecodes.BTN_SELECT: "BTN_SELECT",
    ecodes.BTN_START: "BTN_START",
    ecodes.BTN_EAST: "BTN_EAST",
    ecodes.BTN_SOUTH: "BTN_SOUTH",
    ecodes.BTN_THUMBL: "BTN_THUMBL",
    ecodes.BTN_THUMBR: "BTN_THUMBR",
    # Handled swaps
    ecodes.BTN_WEST: "BTN_NORTH",
    ecodes.BTN_NORTH: "BTN_WEST",
}

def find_controller(name):
    for path in list_devices():
        dev = InputDevice(path)
        if name in dev.name:
            return dev
    return None

def send_rumble(device):
    """Sends a very short, subtle rumble to keep the controller awake."""
    try:
        rumble = ff.Rumble(strong_magnitude=0x0000, weak_magnitude=0x2000)
        effect = ff.Effect(
            ecodes.FF_RUMBLE, 
            -1, 
            0, 
            ff.Trigger(0, 0),
            ff.Replay(200, 0),
            rumble=rumble
        )
        
        effect_id = device.upload_effect(effect)
        device.write(ecodes.EV_FF, effect_id, 1)
        time.sleep(0.2) 
        device.erase_effect(effect_id)
    except Exception as e:
        print(f"Rumble failed: {e}")

def run_bridge():
    target = OSC.Address(OSC_PORT)
    last_heartbeat = 0

    while True: # Outer loop for reconnection
        gamepad = find_controller(TARGET_NAME)
        
        if not gamepad:
            print(f"Searching for {TARGET_NAME}...")
            time.sleep(3)
            continue

        print(f"Connected to {gamepad.name} at {gamepad.path}")
        
        try:
            # Set up non-blocking read to allow for heartbeat timing
            for event in gamepad.read_loop():
                # Check if it's time for a heartbeat rumble
                if time.time() - last_heartbeat > HEARTBEAT_INTERVAL:
                    send_rumble(gamepad)
                    last_heartbeat = time.time()

                # Process OSC Messages
                if event.type in (ecodes.EV_ABS, ecodes.EV_KEY):
                    suffix = EVENT_MAP.get(event.code)
                    if suffix:
                        OSC.send(target, OSC.Message(f"{BASE_PATH}{suffix}", event.value))

        except (OSError, IOError):
            print("Controller disconnected. Retrying...")
        except Exception as e:
            print(f"Unexpected error: {e}")

if __name__ == "__main__":
    try:
        run_bridge()
    except KeyboardInterrupt:
        print("\nShutdown requested by user.")