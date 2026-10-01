#!/usr/bin/env python3
"""Exercise the ordinary synth through SIO and capture its GPU output."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


def elf_symbols(path):
    # Read the normal ELF's static symbols so Release and Debug can use their
    # own layouts without an extra fixture or a host binutils dependency.
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x01\x01', 'Expected a little-endian ELF32'
    offset = struct.unpack_from('<I', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 46)
    sections = [struct.unpack_from('<10I', data, offset + index * stride)
                for index in range(count)]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for entry in range(section[4], section[4] + section[5], section[9]):
            name, value, _, _, _, index = struct.unpack_from('<IIIBBH', data, entry)
            if index:
                end = names.index(0, name)
                symbols[names[name:end].decode('ascii')] = value
    return symbols


root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('configuration', nargs='?', default='debug', choices=('debug', 'release'))
parser.add_argument('--port', type=int, default=8080)
args = parser.parse_args()
if not 1 <= args.port <= 65535:
    parser.error('port must be between 1 and 65535')
exe = root / 'build' / args.configuration / 'hello.exe'
elf = exe.with_suffix('.elf')
if not exe.is_file() or not elf.is_file():
    parser.error(f'Build the synth first: {exe}')
symbols = elf_symbols(elf)
app = root / '.local/PCSX-Redux.app'
emulator = os.environ.get('PCSX_REDUX', str(app / 'Contents/MacOS/PCSX-Redux'))
bios = os.environ.get('PCSX_REDUX_BIOS', str(
    app / 'Contents/Resources/share/pcsx-redux/resources/openbios.bin'))
if not Path(emulator).is_file() or not Path(bios).is_file():
    parser.error('Install PCSX-Redux and OpenBIOS with scripts/setup.sh first')
# Refuse an occupied port rather than accidentally driving another emulator.
with socket.socket() as probe:
    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    probe.bind(('0.0.0.0', args.port))
output = root / 'build/validation' / ('web-' + args.configuration)
output.mkdir(parents=True, exist_ok=True)
result_path = output / 'result.json'
result_path.unlink(missing_ok=True)
with tempfile.TemporaryDirectory(prefix='session-', dir=output) as directory:
    data = Path(directory)
    data.joinpath('pcsx.json').write_text(json.dumps({'emulator': {
        'AutoUpdate': False, 'ShownAutoUpdateConfig': True}}))
    command = [emulator, '-portable', str(data), '-no-ui', '-no-gui-log', '-stdout',
               '-lua_stdout', '-interpreter', '-webserver', '-webserver-port', str(args.port),
               '-bios', bios, '-exe', str(exe), '-dofile', str(root / 'tests/web_control.lua'), '-run']
    base = f'http://127.0.0.1:{args.port}/api/v1/'
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(path, params=None):
        if params:
            path += ('&' if '?' in path else '?') + urllib.parse.urlencode(params)
        req = urllib.request.Request(base + path, data=None if params is None else b'')
        with opener.open(req, timeout=5) as response:
            return response.read()

    def status():
        return json.loads(request('lua/synth/status'))

    def wait_for(predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f'Redux exited with {process.returncode}; see {output / "emulator.log"}')
            if predicate():
                return
            time.sleep(0.02)
        raise TimeoutError('Emulator operation timed out')

    records = []

    def step(buttons='', hold=4, settle=12):
        before = status()
        request('lua/synth/step', dict(buttons=buttons, hold=hold, settle=settle))
        wait_for(lambda: status()['completed'] == before['completed'] + 1)
        state = status()
        assert state['remaining'] == 0
        assert state['frame'] - before['frame'] == hold + settle
        assert not json.loads(request('execution-flow'))['running']
        records.append(dict(buttons=buttons, hold=hold, settle=settle, **state))

    def snapshot():
        # The GET endpoint returns emulated RAM. All input still goes through
        # SIO; observations neither patch settings nor invoke synth functions.
        ram = request('cpu/ram/raw')
        assert len(ram) == 2 * 1024 * 1024

        def read(name, format, delta=0):
            offset = (symbols[name] & 0x1fffffff) + delta
            assert offset + struct.calcsize(format) <= len(ram)
            return struct.unpack_from(format, ram, offset)

        settings = dict(zip(('note', 'ratio', 'depth', 'mod_shape', 'carrier_shape',
                             'carrier_attack_ms', 'carrier_release_ms', 'mod_attack_ms',
                             'mod_release_ms', 'selected'), read('synth_settings', '<10i')))
        active = read('active_envelope_program', '<i')[0]
        assert active in (0, 1)
        fields = ('duration_ms', 'carrier_pitch', 'mod_pitch', 'carrier_adsr1',
                  'carrier_adsr2', 'mod_adsr1', 'mod_adsr2', 'mod_wave', 'carrier_wave', 'frequency')
        program = dict(zip(fields, read('envelope_programs', '<i9H', active * 24)))
        assert program['mod_wave'] == settings['mod_shape'] * 11 + settings['depth']
        assert program['carrier_wave'] == settings['carrier_shape'] * 11 + 4
        playback = dict(zip(fields, read('playback_program', '<i9H')))
        levels = dict(zip(('tick', 'mod', 'carrier', 'frequency'), read('sequencer', '<4i')))
        state = dict(settings=settings, program=program, playback=playback,
                     playing=read('playback_active', '<i')[0], levels=levels)
        records.append(dict(snapshot=state))
        return state

    def capture(name):
        png = request('screen/still')
        assert png[:8] == b'\x89PNG\r\n\x1a\n'
        assert struct.unpack('>II', png[16:24]) == (320, 240)
        output.joinpath(name + '.png').write_bytes(png)
        records.append(dict(capture=name, sha256=hashlib.sha256(png).hexdigest()))
        return png

    def select(index):
        current = snapshot()['settings']['selected']
        for _ in range((index - current) % 9):
            step('DOWN')
        assert snapshot()['settings']['selected'] == index

    def trigger(name):
        step('CROSS', hold=4, settle=4)
        state = snapshot()
        assert state['playing'] == 1
        assert state['playback'] == state['program']
        assert state['levels']['carrier'] > 0, 'Carrier ADSR did not start'
        capture(name)
        step(hold=1, settle=90)
        state = snapshot()
        assert state['playing'] == 0
        assert state['levels']['mod'] == state['levels']['carrier'] == 0

    with output.joinpath('emulator.log').open('wb') as log:
        process = subprocess.Popen(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
        try:
            def ready():
                try:
                    return status()['frame'] >= 300
                except (urllib.error.URLError, OSError):
                    return False
            wait_for(ready)
            request('execution-flow?function=pause', {})
            assert not json.loads(request('execution-flow'))['running']
            paused = status()['frame']
            time.sleep(0.1)
            assert status()['frame'] == paused
            request('execution-flow?function=resume', {})
            wait_for(lambda: status()['frame'] > paused)
            request('execution-flow?function=pause', {})
            step()
            initial = snapshot()['settings']
            assert (initial['mod_shape'], initial['carrier_shape'], initial['depth']) == (0, 0, 6)
            capture('initial')
            trigger('initial-playing')
            for index, field, other, prefix in ((3, 'mod_shape', 'carrier_shape', 'mod'),
                                                (4, 'carrier_shape', 'mod_shape', 'out')):
                select(index)
                other_value = snapshot()['settings'][other]
                for shape in range(64):
                    if shape:
                        step('RIGHT')
                    state = snapshot()
                    assert state['settings'][field] == shape
                    assert state['settings'][other] == other_value
                    capture(f'{prefix}-{shape + 1:02d}')
                step('RIGHT')
                assert snapshot()['settings'][field] == 63
                step('L1')
                assert snapshot()['settings'][field] == 53
                step('R1')
                assert snapshot()['settings'][field] == 63
            select(2)
            step('R1')
            assert snapshot()['settings']['depth'] == 10
            select(0)
            for _ in range(3):
                step('R1')
            assert snapshot()['settings']['note'] == 72
            select(1)
            for _ in range(2):
                step('R1')
            assert snapshot()['settings']['ratio'] == 19
            state = snapshot()
            assert state['program']['mod_wave'] == 703
            assert state['program']['carrier_wave'] == 697
            assert 0 < state['program']['carrier_pitch'] <= state['program']['mod_pitch'] < 0x4000
            capture('maximum-settings')
            trigger('maximum-playing')
            capture('maximum-stopped')
            select(3)
            for _ in range(7):
                step('L1')
            step('LEFT')
            assert snapshot()['settings']['mod_shape'] == 0
            select(4)
            for _ in range(7):
                step('L1')
            step('LEFT')
            assert snapshot()['settings']['carrier_shape'] == 0
            try:
                request('lua/synth/step', {'buttons': 'INVALID'})
            except urllib.error.HTTPError as error:
                assert error.code == 500
            else:
                raise AssertionError('Invalid button was accepted')
            assert status()['remaining'] == 0
            step()
            request('lua/synth/cancel', {})
            result_path.write_text(json.dumps({
                'configuration': args.configuration,
                'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                'checks': 'pause/resume, all 64 waves on both operators, bounds, x10 adjustments, '
                          'triggered programs, ADSR activity and completion, PNG capture, invalid request',
                'records': records}, indent=2) + '\n')
            print(f'PASS: 64 waves on both operators, playback state, and GPU captures: {output}')
            print('Captured screen content requires visual review; audio quality requires listening.')
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
