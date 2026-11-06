import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());

function convert(args) {
  return run(['convert', ...args]);
}

function rejected(result, label) {
  checks.assert(result.status === 1 && result.json?.ok === false && result.json?.code === 'bad-frame', label);
}

const identity = convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '2', '--dst-h', '2',
  '--stride', '6', '--bytes', '12', '--fill', 'ramp',
]);
checks.assert(identity.status === 0 && identity.json?.bytes === 12, 'packed scale keeps 12 bytes');
checks.assert(identity.json?.b0 === 0 && identity.json?.g0 === 0 && identity.json?.r0 === 200, 'first pixel');
checks.assert(identity.json?.bLast === 1 && identity.json?.gLast === 1 && identity.json?.rLast === 200, 'last pixel');

const down = convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '1', '--dst-h', '1',
  '--stride', '6', '--bytes', '12', '--fill', 'ramp',
]);
checks.assert(down.status === 0 && down.json?.bytes === 3 && down.json?.r0 === 200 && down.json?.b0 === 0, 'nearest sample is the origin');

const padded = convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '2', '--dst-h', '2',
  '--stride', '8', '--bytes', '16', '--fill', 'ramp',
]);
checks.assert(padded.status === 0 && padded.json?.bytes === 12, 'padding is dropped');
checks.assert(padded.json?.bLast === 1 && padded.json?.gLast === 1 && padded.json?.rLast === 200, 'padded last pixel');

const grown = convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '4', '--dst-h', '4',
  '--stride', '6', '--bytes', '12', '--fill', 'ramp',
]);
checks.assert(grown.status === 0 && grown.json?.bytes === 48, '4x4 scale');
checks.assert(grown.json?.b0 === 0 && grown.json?.r0 === 200, 'upscale origin');
checks.assert(grown.json?.bLast === 1 && grown.json?.gLast === 1 && grown.json?.rLast === 200, 'upscale far corner');

rejected(convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '0', '--dst-h', '2',
  '--stride', '6', '--bytes', '12', '--fill', 'ramp',
]), 'zero destination');
rejected(convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '-1', '--dst-h', '1',
  '--stride', '6', '--bytes', '12', '--fill', 'ramp',
]), 'negative destination');
rejected(convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '2', '--dst-h', '2',
  '--stride', '6', '--bytes', '11', '--fill', 'ramp',
]), 'short source buffer');
rejected(convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '2', '--dst-h', '2',
  '--stride', '4', '--bytes', '12', '--fill', 'ramp',
]), 'stride shorter than a row');
rejected(convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '2', '--dst-h', '2',
  '--stride', '-1', '--bytes', '12', '--fill', 'ramp',
]), 'negative stride');
rejected(convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '2', '--dst-h', '2',
  '--stride', '6', '--bytes', '12', '--format', 'rgb24', '--fill', 'ramp',
]), 'rgb24 is not a bgr scale source');
rejected(convert([
  '--op', 'scale', '--src-w', '2', '--src-h', '2', '--dst-w', '800000000', '--dst-h', '2',
  '--stride', '6', '--bytes', '12', '--fill', 'ramp',
]), 'destination row does not fit');

const red = convert([
  '--op', 'nv12', '--src-w', '2', '--src-h', '2', '--stride', '6', '--bytes', '12', '--fill', 'red',
]);
checks.assert(red.status === 0 && red.json?.bytes === 6, 'red nv12 size');
checks.assert(JSON.stringify(red.json?.y) === JSON.stringify([82, 82, 82, 82]), 'red luma');
checks.assert(red.json?.u === 90 && red.json?.v === 240, 'red chroma');

const quad = convert([
  '--op', 'nv12', '--src-w', '2', '--src-h', '2', '--stride', '6', '--bytes', '12', '--fill', 'quad',
]);
checks.assert(quad.status === 0 && JSON.stringify(quad.json?.y) === JSON.stringify([82, 144, 41, 235]), 'quad luma');
checks.assert(quad.json?.u === 128 && quad.json?.v === 128, 'averaged chroma is neutral');

const paddedNv = convert([
  '--op', 'nv12', '--src-w', '2', '--src-h', '2', '--stride', '8', '--bytes', '16', '--fill', 'red',
]);
checks.assert(paddedNv.status === 0 && paddedNv.json?.u === 90 && paddedNv.json?.y?.[2] === 82, 'padded red row');

rejected(convert([
  '--op', 'nv12', '--src-w', '3', '--src-h', '2', '--stride', '9', '--bytes', '18',
  '--nv12-bytes', '16', '--fill', 'red',
]), 'odd width');
rejected(convert([
  '--op', 'nv12', '--src-w', '2', '--src-h', '3', '--stride', '6', '--bytes', '18',
  '--nv12-bytes', '16', '--fill', 'red',
]), 'odd height');
rejected(convert([
  '--op', 'nv12', '--src-w', '2', '--src-h', '2', '--stride', '4', '--bytes', '12', '--fill', 'red',
]), 'nv12 stride shorter than a row');
rejected(convert([
  '--op', 'nv12', '--src-w', '2', '--src-h', '2', '--stride', '6', '--bytes', '11', '--fill', 'red',
]), 'nv12 source shorter than the frame');
rejected(convert([
  '--op', 'nv12', '--src-w', '2', '--src-h', '2', '--stride', '6', '--bytes', '12',
  '--nv12-bytes', '5', '--fill', 'red',
]), 'nv12 destination shorter than the frame');
rejected(convert([
  '--op', 'nv12', '--src-w', '2', '--src-h', '2', '--stride', '-8', '--bytes', '16', '--fill', 'red',
]), 'nv12 negative stride');

finish(checks);
