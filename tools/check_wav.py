"""Check whether a reply.wav is suitable for the firmware speaker path."""
import sys
import wave
with wave.open(sys.argv[1], 'rb') as wav:
    print('Channels:', wav.getnchannels())
    print('Sample width:', wav.getsampwidth(), 'bytes')
    print('Sample rate:', wav.getframerate(), 'Hz')
    print('Duration:', round(wav.getnframes() / wav.getframerate(), 2), 's')
    assert wav.getnchannels() == 2
    assert wav.getsampwidth() == 2
    assert wav.getframerate() == 16000
print('ARLO format OK')
