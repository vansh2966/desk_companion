import socket
import subprocess
import os
import sys
import time

import psutil


ESP_IP = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("ESP_IP", "255.255.255.255")
PORT = int(sys.argv[2] if len(sys.argv) > 2 else os.environ.get("ESP_PORT", "4210"))


def gpu_temp():
    try:
        output = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=temperature.gpu", "--format=csv,noheader,nounits"],
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=1,
        )
        return float(output.splitlines()[0])
    except Exception:
        return -1.0


sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
print(f"Sending telemetry to {ESP_IP}:{PORT}")
last_bytes = psutil.net_io_counters().bytes_sent + psutil.net_io_counters().bytes_recv
last_time = time.time()

while True:
    cpu = psutil.cpu_percent(interval=0.5)
    ram = psutil.virtual_memory().percent
    now = time.time()
    total_bytes = psutil.net_io_counters().bytes_sent + psutil.net_io_counters().bytes_recv
    wifi_mbps = ((total_bytes - last_bytes) * 8) / max(now - last_time, 0.1) / 1_000_000
    last_bytes = total_bytes
    last_time = now

    message = f"cpu={cpu:.1f},gpu={gpu_temp():.1f},ram={ram:.1f},wifi={wifi_mbps:.2f}"
    sock.sendto(message.encode("ascii"), (ESP_IP, PORT))
    print(message)
    time.sleep(1)
