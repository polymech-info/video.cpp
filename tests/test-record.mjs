import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());

const modes = run(['modes']);
const order = modes.json?.order || [];
checks.assert(modes.status === 0 && order.length === 4, 'four modes');
checks.assert(order[0]?.format === 'NV12' && order[0]?.fps === 60, '1080p60 first');
checks.assert(order[1]?.format === 'MJPEG' && order[2]?.format === 'NV12', 'same rate sorts by format');
checks.assert(order[3]?.w === 640, '480p last');
checks.assert(modes.json?.kept === true && modes.json?.emptyNeedle === true, 'negative mode keeps the size');

const mp4 = run(['file', '--path', 'clip.MP4']);
checks.assert(mp4.json?.native === true && mp4.json?.still === false, 'mp4 is a native container');
const jpg = run(['file', '--path', 'shot.JPEG']);
checks.assert(jpg.json?.still === true && jpg.json?.native === false, 'jpeg is a still');

const tick = run(['tick', '--ts', '50', '--index', '0', '--fps', '0', '--prev', '100']);
checks.assert(tick.json?.ts === 101 && tick.json?.prev === 101, 'timestamp moves forward');
const paced = run(['tick', '--ts', '0', '--index', '2', '--fps', '10', '--prev', '-1']);
checks.assert(paced.json?.ts === 200, 'fps fills an earlier timestamp');

const missing = run(['nvenc', '--message', 'NVENC plugin is not installed']);
checks.assert(missing.json?.absent === true, 'missing plugin');
const other = run(['nvenc', '--message', 'encoder failed']);
checks.assert(other.json?.absent === false, 'other encoder error');

const url = run(['url', '--api', 'http://127.0.0.1:9/', '--token', 'tok', '--dev', 'http://dev', '--bind', '']);
checks.assert(url.json?.host === '127.0.0.1', 'empty bind is loopback');
checks.assert(url.json?.open === 'http://dev/?server=http%3A%2F%2F127.0.0.1%3A9%2F&token=tok', 'dev client url');
const plain = run(['url', '--api', 'http://cam/', '--token', 'tok', '--dev', '']);
checks.assert(plain.json?.open === 'http://cam/', 'no dev url keeps the api');

const token = run(['token']);
checks.assert(token.status === 0 && /^[A-Za-z0-9]{12}$/.test(token.json?.token || ''), '12 character token');

const dir = mkdtempSync(join(tmpdir(), 'pm-video-avi-'));
const out = join(dir, 'one.avi');
try {
  const avi = run(['avi', '--out', out]);
  const bytes = avi.status === 0 ? readFileSync(out) : Buffer.alloc(0);
  checks.assert(avi.status === 0 && bytes.subarray(0, 4).toString('latin1') === 'RIFF', 'AVI starts with RIFF');
  checks.assert(bytes.subarray(8, 12).toString('latin1') === 'AVI ', 'AVI form');
  checks.assert(bytes.includes(Buffer.from('idx1')), 'frame index');
  checks.assert(bytes.includes(Buffer.from([0xff, 0xd8, 0xff, 0xd9])), 'jpeg payload');
} finally {
  rmSync(dir, { recursive: true, force: true });
}

finish(checks);
