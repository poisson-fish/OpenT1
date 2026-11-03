"""Compare deterministic benchmark PPM readbacks; export PNG previews and error counts."""
import argparse
import json
from pathlib import Path
import struct
import zlib


def png(path, pixels, width=800, height=600):
    def chunk(name, data):
        body = name + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    rows = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    args = parser.parse_args()
    sources = sorted(args.reference.glob('*.json.ppm'))
    if not sources:
        parser.error(f'No reference readbacks found in {args.reference}')
    missing = [source.name for source in sources if not (args.candidate / source.name).is_file()]
    if missing:
        parser.error('Missing candidate readbacks: ' + ', '.join(missing))
    results = {}
    for source in sources:
        candidate = args.candidate / source.name
        a = source.read_bytes().split(b'\n', 3)[3]
        b = candidate.read_bytes().split(b'\n', 3)[3]
        if len(a) != len(b) or len(a) != 800 * 600 * 3:
            raise ValueError('Readback size mismatch')
        errors = [abs(x-y) for x, y in zip(a, b)]
        result = {'changed_pixels': sum(any(errors[i:i+3]) for i in range(0, len(errors), 3)),
                  'pixels_over_2': sum(max(errors[i:i+3]) > 2 for i in range(0, len(errors), 3)),
                  'max_channel_error': max(errors), 'mean_channel_error': sum(errors)/len(errors)}
        results[source.name] = result
        png(source.with_suffix('.png'), a)
        png(candidate.with_suffix('.png'), b)
        png(candidate.with_suffix('.difference.png'), bytes(min(255, d*16) for d in errors))
        print(source.name, result)
    (args.candidate / 'readback-comparisons.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
