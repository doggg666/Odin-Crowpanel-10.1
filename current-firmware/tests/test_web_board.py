#!/usr/bin/env python3
"""Live-board regression checks. Run with ODIN_WEB_PASSWORD set after flashing."""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import io
import json
import os
import struct
import time
import urllib.error
import urllib.parse
import urllib.request


class Board:
    def __init__(self, url, password):
        self.url = url.rstrip('/')
        self.auth = 'Basic ' + base64.b64encode(('admin:' + password).encode()).decode()

    def request(self, path, body=None, auth=True, headers=None, timeout=20):
        h = {'Authorization': self.auth} if auth else {}
        if body is not None:
            body = json.dumps(body).encode()
            h['Content-Type'] = 'application/json'
        h.update(headers or {})
        return urllib.request.urlopen(urllib.request.Request(self.url + path, data=body, headers=h), timeout=timeout)

    def api(self, path, body=None, **kwargs):
        with self.request(path, body, **kwargs) as r:
            return json.load(r)

    def expect_error(self, code, path, body=None, **kwargs):
        try:
            with self.request(path, body, **kwargs) as r:
                raise AssertionError(f'Expected HTTP {code}, received {r.status}')
        except urllib.error.HTTPError as e:
            assert e.code == code, (e.code, e.read(300))
            e.close()

    def wait(self, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.api('/api/status')
            if predicate(status):
                return status
            time.sleep(.3)
        raise AssertionError('Timed out waiting for board state')


def first_jpeg(response):
    data = bytearray()
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline and len(data) < 2 * 1024 * 1024:
        chunk = response.read(4096)
        if not chunk:
            break
        data.extend(chunk)
        start = data.find(b'\xff\xd8')
        end = data.find(b'\xff\xd9', start + 2) if start >= 0 else -1
        if end >= 0:
            return bytes(data[start:end + 2])
    raise AssertionError('No complete JPEG frame received')


def jpeg_dimensions(data):
    pos = 2
    while pos + 4 <= len(data):
        assert data[pos] == 0xff
        marker = data[pos + 1]
        pos += 2
        size = int.from_bytes(data[pos:pos + 2], 'big')
        if marker in (0xc0, 0xc1, 0xc2):
            return int.from_bytes(data[pos + 5:pos + 7], 'big'), int.from_bytes(data[pos + 3:pos + 5], 'big')
        pos += size
    raise AssertionError('JPEG frame has no image dimensions')


def run(board, streams=True, speech=True):
    initial = board.api('/api/settings')
    start = board.api('/api/status')
    assert start['connected']
    board.expect_error(401, '/api/settings', auth=False)
    board.expect_error(401, '/api/settings', {}, headers={'Origin': 'http://untrusted.invalid'})
    for patch in ({'brightness': 101}, {'provider': 9}, {'camera': {'resolution': 255}},
                  {'camera': {'fps': 0}}, {'tts': {'url': 'file:///etc/passwd'}},
                  {'weather': {'latitude': 999, 'longitude': 0}}, {'volume': 'loud'}):
        board.expect_error(400, '/api/settings', patch)
    unchanged = board.api('/api/settings')
    assert unchanged == initial, 'Rejected patches changed stored settings'
    assert all('key' not in p for p in initial['providers'])
    assert 'key' not in initial['tts'] and 'password' not in initial
    with board.request('/') as r:
        html = r.read().decode()
    assert 'Find your network' in html and 'Live camera' in html
    print('PASS authentication, secret redaction, transactional validation, full web-page transfer', flush=True)
    board.api('/api/wifi/scan', {})
    deadline = time.monotonic() + 20
    while True:
        scan = board.api('/api/wifi/scan')
        if scan['state'] != 1:
            break
        assert time.monotonic() < deadline
        time.sleep(.5)
    assert scan['state'] == 2 and scan['networks']
    print('PASS Wi-Fi scan:', len(scan['networks']), 'networks', flush=True)
    results = board.api('/api/weather/search?city=Laval&country=Canada')
    assert results.get('results') and all(r.get('country_code') == 'CA' for r in results['results'])
    board.api('/api/settings', {'weather': initial['weather']})
    print('PASS country/city lookup and weather settings save', flush=True)
    for payload in ({'password': 'short', 'confirm': 'short'},
                    {'password': 'valid-password', 'confirm': 'different-password'},
                    {'password': 'x' * 33, 'confirm': 'x' * 33},
                    {'password': 'line\nbreak', 'confirm': 'line\nbreak'},
                    {'password': 12345678, 'confirm': 12345678}):
        board.expect_error(400, '/api/password', payload)
    board.api('/api/settings')  # Invalid attempts must preserve existing access.
    board.expect_error(401, '/api/password', {'password': 'test-password', 'confirm': 'test-password'}, auth=False)
    print('PASS password validation and authentication; existing password preserved', flush=True)
    p = initial['providers'][initial['provider']]
    board.api('/api/settings', {'provider': initial['provider'], 'providers': [dict(p, key='')]})
    after = board.api('/api/settings')
    assert after['providers'][initial['provider']]['key_set'] == p['key_set']
    print('PASS provider configuration and blank-key preservation', flush=True)
    if speech:
        # Status must remain responsive while inference waits on the provider.
        with ThreadPoolExecutor(max_workers=1) as executor:
            inference = executor.submit(board.api, '/api/provider/test', {}, timeout=65)
            while not inference.done():
                board.api('/api/status', timeout=3)
                time.sleep(.1)
            result = inference.result()
        assert result['ok']
        print('PASS active provider inference:', result['message'], flush=True)
        board.api('/api/tts/test', {})
        result = board.wait(lambda s: s['tts_state'] != 0 and s['assistant'] == 0, 75)
        assert result['tts_state'] == 1, result['tts_detail']
        print('PASS real TTS synthesis/WAV validation/playback:', result['tts_detail'], flush=True)
    original_camera = {k: initial['camera'][k] for k in ('resolution', 'fps', 'quality', 'audio')}
    try:
        if streams and initial['camera']['available']:
            for mode in initial['camera']['resolutions']:
                board.api('/api/settings', {'camera': {'resolution': mode['id'], 'fps': 5, 'quality': 70}})
                with board.request('/stream', timeout=20) as r:
                    assert 'multipart' in r.headers['Content-Type']
                    jpeg = first_jpeg(r)
                    assert jpeg_dimensions(jpeg) == (mode['width'], mode['height'])
                    status = board.api('/api/status')
                    assert status['video_clients'] == 1
                board.wait(lambda s: s['video_clients'] == 0)
                print('PASS MJPEG', mode['name'], len(jpeg), 'bytes; settings API responsive during video', flush=True)
            board.api('/api/settings', {'camera': {'resolution': 1, 'audio': False}})
            board.expect_error(403, '/audio.wav')
            board.api('/api/settings', {'camera': {'audio': True}})
            with board.request('/stream', timeout=20) as video:
                first_jpeg(video)
                with board.request('/audio.wav', timeout=20) as audio:
                    header = audio.read(44)
                    assert header[:4] == b'RIFF' and header[8:12] == b'WAVE'
                    assert struct.unpack_from('<I', header, 24)[0] == 16000
                    assert len(audio.read(8192)) == 8192
                    board.wait(lambda s: s['microphone_owner'] == 2)
                    board.expect_error(409, '/audio.wav')
                    print('PASS concurrent video/audio; microphone listener admission control', flush=True)
            board.wait(lambda s: s['microphone_owner'] == 0 and s['video_clients'] == 0)
    finally:
        board.api('/api/settings', {'camera': original_camera})
    end = board.api('/api/status')
    if 'uptime_ms' in start:
        assert end['uptime_ms'] > start['uptime_ms'], 'Board rebooted during tests'
    assert end['internal_free'] > 20000, end
    print('PASS cleanup, camera settings restored, no restart; internal RAM free:', end['internal_free'], flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--url', default='http://192.168.1.153')
    p.add_argument('--no-streams', action='store_true')
    p.add_argument('--no-speech', action='store_true')
    args = p.parse_args()
    run(Board(args.url, os.environ['ODIN_WEB_PASSWORD']), not args.no_streams, not args.no_speech)
