#!/usr/bin/env python3
"""Verify the self-contained factory bank using the Python standard library."""
from pathlib import Path
import collections
import hashlib
import json
import math
import wave


def verify(root):
    sample_root = root / 'Samples'
    manifest = json.loads((sample_root / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1:
        raise ValueError('Unsupported sample manifest schema')
    regions = manifest.get('regions')
    if not isinstance(regions, list) or not 1 <= len(regions) <= 2048:
        raise ValueError('No factory regions found')
    counts, names, keys, total = collections.Counter(), set(), set(), 0
    for region in regions:
        instrument = region['instrument']
        if instrument not in ('clarinet', 'flute', 'trombone', 'trumpet'):
            raise ValueError(f'Unknown instrument: {instrument}')
        path = (sample_root / region['file']).resolve()
        if not path.is_relative_to(sample_root.resolve()):
            raise ValueError('Sample path escapes Samples directory')
        if path.name in names:
            raise ValueError(f'Duplicate embedded basename: {path.name}')
        names.add(path.name)
        key = (instrument, region['rootNote'], region['velocityLayer'], region['roundRobin'])
        if key in keys:
            raise ValueError(f'Duplicate sample region: {key}')
        keys.add(key)
        if any(type(region[field]) is not int for field in
               ('rootNote', 'velocityLayer', 'roundRobin', 'loopStart', 'loopEnd', 'sampleRate')):
            raise ValueError(f'Non-integer mapping field: {key}')
        if not 0 <= region['rootNote'] <= 127:
            raise ValueError(f'Invalid root note: {key}')
        if not 0 <= region['velocityLayer'] <= 127 or not 0 <= region['roundRobin'] <= 7:
            raise ValueError(f'Invalid layer/RR: {key}')
        if not 0 <= float(region['velocityCentre']) <= 1:
            raise ValueError(f'Invalid velocity centre: {key}')
        if not math.isfinite(float(region['gainDb'])) or not -36 <= float(region['gainDb']) <= 12:
            raise ValueError(f'Invalid gain: {key}')
        with wave.open(str(path), 'rb') as wav:
            if wav.getnchannels() != 1 or wav.getsampwidth() not in (2, 3):
                raise ValueError(f'Expected mono PCM16/24: {path.name}')
            if wav.getframerate() != region['sampleRate'] or wav.getframerate() != 48000:
                raise ValueError(f'Unexpected sample rate: {path.name}')
            if not 32 <= wav.getnframes() <= 576000:
                raise ValueError(f'Invalid WAV frame count: {path.name}')
            if not 0 <= region['loopStart'] < region['loopEnd'] <= wav.getnframes():
                raise ValueError(f'Invalid sustain loop: {path.name}')
            if region['loopEnd'] - region['loopStart'] < 32:
                raise ValueError(f'Sustain loop too short: {path.name}')
        if 'sha256' in region and hashlib.sha256(path.read_bytes()).hexdigest() != region['sha256']:
            raise ValueError(f'WAV checksum mismatch: {path.name}')
        counts[instrument] += 1
        total += path.stat().st_size
    if set(counts) != {'clarinet', 'flute', 'trombone', 'trumpet'}:
        raise ValueError('The bank must contain all four acoustic instruments')
    extras = sorted(p.name for p in sample_root.rglob('*.wav') if p.name not in names)
    if extras:
        raise ValueError(f'Unreferenced WAV files: {extras}')
    return {'regions': len(regions), 'instruments': dict(counts), 'wavBytes': total}


if __name__ == '__main__':
    print(json.dumps(verify(Path(__file__).resolve().parents[1]), indent=2))
