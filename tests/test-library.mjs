import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());

const bgr = run(['convert', '--op', 'bgr', '--src-w', '2', '--src-h', '2', '--stride', '6', '--bytes', '12', '--format', 'rgb24', '--fill', 'ramp']);
checks.assert(bgr.status === 0 && bgr.json?.b0 === 200 && bgr.json?.r0 === 0, 'rgb swaps to bgr');
const short = run(['convert', '--op', 'bgr', '--src-w', '2', '--src-h', '2', '--stride', '4', '--bytes', '12', '--fill', 'ramp']);
checks.assert(short.status === 1 && short.json?.code === 'bad-frame', 'short bgr stride is rejected');

const oldest = run(['queue', '--cap', '2', '--count', '3', '--policy', 'oldest']);
checks.assert(oldest.status === 0 && oldest.json?.dropped === 1 && oldest.json?.first === 1, 'drop oldest keeps the later frames');
checks.assert(oldest.json?.closedPush === false, 'closed queue rejects a push');
const newest = run(['queue', '--cap', '2', '--count', '3', '--policy', 'newest']);
checks.assert(newest.json?.dropped === 1 && newest.json?.first === 0, 'drop newest keeps the earlier frames');
const reject = run(['queue', '--cap', '2', '--count', '3', '--policy', 'reject']);
checks.assert(reject.json?.accepted === 2 && reject.json?.dropped === 0 && reject.json?.first === 0, 'reject leaves the queue unchanged');

const yuv = run(['yuv']);
checks.assert(yuv.status === 0 && JSON.stringify(yuv.json?.y) === JSON.stringify([10, 11, 12, 13]), 'planar luma');
checks.assert(yuv.json?.u0 === 20 && yuv.json?.v0 === 30 && yuv.json?.odd === false, 'pixel stride is honored and odd size is rejected');

const hw = run(['hw']);
checks.assert(hw.status === 0 && hw.json?.bytes === 12 && hw.json?.b0 === 7 && hw.json?.unmapped === false, 'cpu map fills a frame');

const backends = run(['backends']);
checks.assert(backends.status === 0 && backends.json?.capture?.mediaFoundation === 'compiled', 'media foundation is compiled');
checks.assert(backends.json?.webrtc?.transport === 'unsupported', 'webrtc transport is not in the core');
checks.assert(backends.json?.encode?.mjpegAvi === 'compiled', 'mjpeg avi is compiled');

const small = run(['riff', '--bytes', '100']);
const huge = run(['riff', '--bytes', '4294967304']);
const tiny = run(['riff', '--bytes', '7']);
checks.assert(small.json?.fits === true && huge.json?.fits === false && tiny.json?.fits === false, 'riff size field limit');

const dir = mkdtempSync(join(tmpdir(), 'pm-video-lib-'));
try {
  const avi = run(['avi', '--out', join(dir, 'one.avi')]);
  const bytes = avi.status === 0 ? readFileSync(join(dir, 'one.avi')) : Buffer.alloc(0);
  checks.assert(avi.status === 0 && bytes.subarray(0, 4).toString('latin1') === 'RIFF', 'explicit finalize');
  const again = run(['avi', '--out', join(dir, 'dtor.avi'), '--destructor']);
  const dtor = again.status === 0 ? readFileSync(join(dir, 'dtor.avi')) : Buffer.alloc(0);
  checks.assert(again.status === 0 && dtor.includes(Buffer.from([0xff, 0xd8, 0xff, 0xd9])), 'destructor finalizes');
  const empty = run(['avi', '--out', join(dir, 'empty.avi'), '--empty']);
  checks.assert(empty.status === 0 && empty.json?.rejected === true, 'empty jpeg is rejected');
  const bad = run(['avi', '--out', join(dir, 'bad.avi'), '--width', '0']);
  checks.assert(bad.status === 1 && bad.json?.code === 'avi', 'zero width is rejected');
} finally {
  rmSync(dir, { recursive: true, force: true });
}

const bench = run(['bench', '--width', '64', '--height', '64', '--frames', '2']);
checks.assert(bench.status === 0 && bench.json?.y0 === 82 && bench.json?.scaleUs >= 0 && bench.json?.nv12Us >= 0, '64x64 bench keeps red luma');

finish(checks);
