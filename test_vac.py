import sounddevice as sd
import numpy as np
import time
import argparse
def generate_square_wave(frequency, sample_rate, duration, channels):
    # Generate time array
    t = np.linspace(0, duration, int(sample_rate * duration), endpoint=False)
    # Generate square wave (1.0 or -1.0)
    wave = np.sign(np.sin(2 * np.pi * frequency * t))
    # Scale amplitude
    wave = wave * 0.1  # 10% volume so we don't blow out eardrums
    # Duplicate across required number of channels
    multi_channel_wave = np.tile(wave, (channels, 1)).T
    return multi_channel_wave.astype(np.float32)

def main():
    parser = argparse.ArgumentParser(description="Generate a square wave and play it to an audio device.")
    parser.add_argument("-d", "--device", type=int, help="The device ID to output to.")
    args = parser.parse_args()

    print("Available Audio Devices:")
    print(sd.query_devices())
    
    devices = sd.query_devices()
    target_index = args.device

    # If no device was specified, try to auto-find Virtual Audio Cable
    if target_index is None:
        for i, dev in enumerate(devices):
            name = dev['name'].lower()
            if ('virtual' in name or 'cable' in name or 'vbc' in name or 'vba' in name) and dev['max_output_channels'] > 0:
                target_index = i
                break
                
        if target_index is not None:
            print(f"\n[+] Auto-selected output device: {devices[target_index]['name']} (ID: {target_index})")
        else:
            print("\n[!] Could not automatically find Virtual Audio Cable.")
            target_index = int(input("Enter the device ID of your output device: "))
    else:
        print(f"\n[+] Using specified output device: {devices[target_index]['name']} (ID: {target_index})")
        
    dev_info = devices[target_index]
    sample_rate = int(dev_info['default_samplerate'])
    channels = int(dev_info['max_output_channels'])
    
    if sample_rate <= 0:
        sample_rate = 48000 # Fallback
        
    frequency = 440.0  # 440 Hz (A4)
    duration = 10.0    # 10 seconds per block
    
    print(f"\nGenerating a {frequency}Hz square wave ({channels} channels @ {sample_rate}Hz)...")
    wave = generate_square_wave(frequency, sample_rate, duration, channels)
    
    print("Playing to Virtual Audio Cable... (Press Ctrl+C to stop)")
    try:
        # Use loop=True and a Python sleep loop to cleanly catch Ctrl+C
        sd.play(wave, samplerate=sample_rate, device=target_index, loop=True)
        while True:
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("\nStopped.")
        sd.stop()

if __name__ == "__main__":
    main()
