import serial, sys, time, os

PORT = "/dev/cu.usbmodem143301"
BAUD = 115200
SECS = 60

def ts():
    return time.strftime("%H:%M:%S")

print(f"监控 {PORT} 共 {SECS} 秒，自动重连，捕捉复位/断开 ...\n")
ser = None
start = time.time()
last_seen = None
while time.time() - start < SECS:
    if ser is None or not getattr(ser, "is_open", False):
        if not os.path.exists(PORT):
            time.sleep(0.15)
            continue
        try:
            ser = serial.Serial(PORT, BAUD, timeout=0.5)
            ser.setDTR(False)
            ser.setRTS(False)
            print(f"[{ts()}] >>> 串口已连接（设备出现，可能刚复位）")
            last_seen = time.time()
        except Exception as e:
            print(f"[{ts()}] 打开失败: {e}")
            time.sleep(0.2)
            continue
    try:
        d = ser.read(8192)
        if d:
            last_seen = time.time()
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
        else:
            # 读超时：检查设备是否仍在
            if not os.path.exists(PORT):
                print(f"[{ts()}] !!! 设备消失（疑似复位/断电），累计在线 {int(time.time()-last_seen) if last_seen else '?'}s")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
    except Exception as e:
        print(f"[{ts()}] 读异常: {e}")
        try:
            ser.close()
        except Exception:
            pass
        ser = None
        time.sleep(0.2)

print(f"\n[{ts()}] === 监控结束 ===")
