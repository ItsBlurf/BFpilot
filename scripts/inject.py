import socket
import time

try:
    with open("C:/Users/Blurf/Documents/PS5 dev/projects/GemBfpilot/bfpilot.elf", "rb") as f:
        payload = f.read()
    print(f"Loaded {len(payload)} bytes")
    s = socket.socket()
    s.connect(("192.168.1.204", 9021))
    s.sendall(payload)
    s.close()
    print("Injected successfully!")
except Exception as e:
    print(f"Error: {e}")
