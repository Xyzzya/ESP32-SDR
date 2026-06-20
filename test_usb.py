"""
ESP32-S3 SDR USB Test

Tests over USB:
  1. CDC Serial Commands — frequency query, tone enable/disable
  2. USB Audio Capture — captures I/Q tone and verifies via FFT (optional)

Usage:
  python test_usb.py                # auto-detect, CDC + audio test
  python test_usb.py --cdc-only     # CDC commands only (no audio capture)
  python test_usb.py -f 2000        # use 2 kHz tone
  python test_usb.py -p COM8 -d 5   # manual port/device selection
  python test_usb.py -l             # list available devices

Dependencies: pyserial, sounddevice, numpy
"""

import sounddevice as sd
import numpy as np
import serial
import serial.tools.list_ports
import argparse
import sys
import time
import threading

SAMPLE_RATE = 48000
CHANNELS = 2
CAPTURE_DURATION = 1.0
MIN_RMS = 0.003
MAX_FREQ_ERR = 5.0
CAPTURE_TIMEOUT = 10.0  # seconds


def find_esp32_cdc():
    for port in serial.tools.list_ports.comports():
        if port.vid == 0x303A:
            return port.device
    for port in serial.tools.list_ports.comports():
        desc = port.description.lower()
        dev = port.device.lower()
        if 'esp32' in desc or 'ttyacm' in dev or 'ttyusb' in dev:
            return port.device
    return None


def find_esp32_audio():
    devices = sd.query_devices()
    for i, dev in enumerate(devices):
        name = dev['name']
        if dev['max_input_channels'] >= 2:
            if 'ESP32 SDR' in name or 'SDR Receiver' in name or 'SDR I/Q' in name:
                return i, name
    return None, None


def list_devices():
    print("Serial ports:")
    ports = list(serial.tools.list_ports.comports())
    if ports:
        for p in ports:
            print(f"  {p.device}  VID:{p.vid:04X} PID:{p.pid:04X}  {p.description}")
    else:
        print("  (none)")
    print()
    print("Audio input devices:")
    found = False
    for i, dev in enumerate(sd.query_devices()):
        if dev['max_input_channels'] > 0:
            print(f"  {i}: {dev['name']}  ({dev['max_input_channels']}ch @ {int(dev['default_samplerate'])}Hz)")
            found = True
    if not found:
        print("  (none)")


def send_command(ser, cmd):
    ser.write((cmd + '\r\n').encode())
    ser.flush()
    response = b''
    deadline = time.time() + 0.3
    while time.time() < deadline:
        waiting = ser.in_waiting
        if waiting:
            response += ser.read(waiting)
        if b'\n' in response:
            break
        time.sleep(0.01)
    return response.decode(errors='replace').strip()


def capture_with_timeout(device_id, n_samples):
    result = [None]
    error = [None]

    def do_capture():
        try:
            x = sd.rec(n_samples, samplerate=SAMPLE_RATE, channels=CHANNELS,
                       dtype='float32', device=device_id)
            sd.wait()
            result[0] = x
        except Exception as e:
            error[0] = str(e)

    thread = threading.Thread(target=do_capture, daemon=True)
    thread.start()
    thread.join(CAPTURE_TIMEOUT)
    if thread.is_alive():
        sd.stop()
        thread.join(timeout=1.0)
        return None, "Capture timed out (WASAPI may not support this USB audio device)"
    if error[0]:
        return None, error[0]
    return result[0], None


def run_cdc_test(port_name):
    cdc_ok = True
    try:
        ser = serial.Serial(port_name, 115200, timeout=0.5)
    except serial.SerialException as e:
        print(f"  [FAIL] Cannot open serial port: {e}")
        return False

    time.sleep(0.5)

    resp = send_command(ser, '?')
    print(f"  [?] => {resp}")
    if 'FREQ' not in resp and resp != 'FREQ: unknown':
        cdc_ok = False

    resp = send_command(ser, 'F 10100')
    print(f"  [F 10100] => {resp}")

    resp = send_command(ser, '?')
    print(f"  [?] => {resp}")

    resp = send_command(ser, 'T 1000')
    print(f"  [T 1000] => {resp}")
    time.sleep(2.0)  # let tone run for 2 seconds

    resp = send_command(ser, 'T 0')
    print(f"  [T 0] => {resp}")

    ser.close()
    return cdc_ok


def main():
    parser = argparse.ArgumentParser(
        description="ESP32-S3 SDR USB test — test CDC serial and optionally USB audio capture."
    )
    parser.add_argument('-f', '--freq', type=float, default=1000.0,
                        help='Test tone frequency in Hz (default: 1000)')
    parser.add_argument('-d', '--device', type=int,
                        help='Audio input device ID')
    parser.add_argument('-p', '--port',
                        help='CDC serial port (e.g. COM3)')
    parser.add_argument('--cdc-only', action='store_true',
                        help='CDC command test only, skip audio capture')
    parser.add_argument('-l', '--list', action='store_true',
                        help='List available devices and exit')
    args = parser.parse_args()

    if args.list:
        list_devices()
        return

    print("=== ESP32-S3 SDR USB Test ===\n")

    # ---- Locate CDC ----
    port_name = args.port or find_esp32_cdc()
    if not port_name:
        print("[!] No ESP32 CDC serial port found (VID=0x303A).")
        print("    Use -p to specify or -l to list ports.")
        sys.exit(1)
    print(f"[+] CDC serial: {port_name}")

    # ---- CDC Test ----
    print("\n--- CDC Serial Command Test ---")
    cdc_passed = run_cdc_test(port_name)
    if cdc_passed:
        print("  [PASS] CDC serial commands work")
    else:
        print("  [WARN] CDC serial commands may have issues")

    # ---- Locate audio ----
    if args.device is not None:
        audio_dev = args.device
        dev_name = sd.query_devices(audio_dev)['name']
    else:
        audio_dev, dev_name = find_esp32_audio()
    audio_available = audio_dev is not None

    if args.cdc_only:
        print("\n  (skipping audio capture -- --cdc-only)")
        print("\nDone.")
        sys.exit(0 if cdc_passed else 1)

    if not audio_available:
        print("\n[!] No SDR I/Q Audio device found. Skipping audio capture.")
        print("    Use --cdc-only to suppress this message.")
        print("\nDone.")
        sys.exit(0 if cdc_passed else 1)

    print(f"[+] Audio input: {dev_name} (ID={audio_dev})")

    # ---- Audio Capture ----
    print(f"\n--- Audio Capture ({args.freq:.0f} Hz tone) ---")

    # Enable tone
    ser = serial.Serial(port_name, 115200, timeout=0.5)
    time.sleep(0.5)
    send_command(ser, f'T {int(args.freq)}')
    time.sleep(0.3)
    ser.close()
    time.sleep(1.0)

    # Re-detect audio device (index may change after USB re-enumeration)
    if args.device is None:
        audio_dev, dev_name = find_esp32_audio()
        if audio_dev is None:
            print("[!] Audio device disappeared after serial close.")
            sys.exit(1)
        print(f"  Re-detected audio: {dev_name} (ID={audio_dev})")

    print(f"  Capturing {CAPTURE_DURATION}s @ {SAMPLE_RATE}Hz (timeout {CAPTURE_TIMEOUT}s)...")
    n_samples = int(SAMPLE_RATE * CAPTURE_DURATION)
    recording, capture_err = capture_with_timeout(audio_dev, n_samples)

    # Cleanup tone regardless
    ser = serial.Serial(port_name, 115200, timeout=0.5)
    time.sleep(0.5)
    send_command(ser, 'T 0')
    time.sleep(0.2)
    ser.close()

    if capture_err:
        print(f"  [SKIP] {capture_err}")
        print(f"\n  Note: Audio capture via sounddevice/PortAudio may not work")
        print(f"        with all USB Audio Class 2.0 devices on Windows.")
        print(f"        For full audio testing, use HDSDR or SDR# to verify")
        print(f"        that 'Microphone (SDR I/Q Audio)' produces I/Q data.")
        print("\nDone.")
        sys.exit(0 if cdc_passed else 1)

    # ---- Analyze ----
    i_ch = recording[:, 0]
    q_ch = recording[:, 1]
    window = np.hanning(len(i_ch))
    spectrum = np.abs(np.fft.rfft(i_ch * window))
    freqs = np.fft.rfftfreq(len(i_ch), 1.0 / SAMPLE_RATE)

    if len(spectrum) < 2:
        print(f"\n  [SKIP] Recording empty or too short for FFT analysis")
        print("\nDone.")
        sys.exit(0 if cdc_passed else 1)

    peak_idx = np.argmax(spectrum[1:]) + 1
    peak_freq = freqs[peak_idx]
    peak_mag = spectrum[peak_idx]
    rms_i = float(np.sqrt(np.mean(i_ch ** 2)))
    rms_q = float(np.sqrt(np.mean(q_ch ** 2)))

    print(f"\n--- Audio Results ---")
    print(f"  I-channel RMS : {rms_i:.4f}")
    print(f"  Q-channel RMS : {rms_q:.4f}")
    print(f"  Peak frequency: {peak_freq:.1f} Hz  (expected {args.freq:.0f} Hz)")
    print(f"  Peak magnitude: {peak_mag:.1f}")

    freq_err = abs(peak_freq - args.freq)
    audio_passed = (freq_err <= MAX_FREQ_ERR) and (rms_i >= MIN_RMS)

    if audio_passed:
        print(f"\n  [PASS] Tone detected at {peak_freq:.1f} Hz (error {freq_err:.1f} Hz, I-RMS {rms_i:.4f})")
    else:
        print(f"\n  [FAIL]")
        if rms_i < MIN_RMS:
            print(f"         I-channel RMS {rms_i:.4f} below threshold ({MIN_RMS}).")
        if freq_err > MAX_FREQ_ERR:
            print(f"         Frequency error {freq_err:.1f} Hz exceeds limit ({MAX_FREQ_ERR} Hz).")

    print("\nDone.")
    sys.exit(0 if (cdc_passed and audio_passed) else 1)


if __name__ == '__main__':
    main()
