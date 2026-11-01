import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());

const live = run(['pause', '--reasons', '0']);
checks.assert(live.status === 0 && live.json?.active === true, 'no bits means recording');

const held = run(['pause', '--reasons', '3']);
checks.assert(held.json?.active === false && held.json?.user === true && held.json?.idle === true, 'user and idle bits');

const rec = run(['status', '--state', 'recording']);
checks.assert(rec.status === 0 && rec.json?.state === 'recording' && rec.json?.active === true, 'recording is active');

const bad = run(['status', '--state', 'nope']);
checks.assert(bad.status === 1 && bad.json?.code === 'bad-state', 'unknown state fails');

const px = run(['landmark', '--x', '0.5', '--y', '0.25', '--width', '100', '--height', '80']);
checks.assert(px.status === 0 && px.json?.x === 50 && px.json?.y === 20, 'landmark pixel');
checks.assert(px.json?.lip === 61 && px.json?.wantsBeauty === true && px.json?.needsSeg === false, 'lip index and default beauty');

const nv = run(['nv12']);
checks.assert(nv.status === 0 && nv.json?.bytes === 6 && nv.json?.y0 === 82, '2x2 red NV12 luma');

const vcam = run(['vcam']);
checks.assert(vcam.status === 0 && vcam.json?.magic === 0x56434D32, 'VCM2 magic');
checks.assert(vcam.json?.width === 1280 && vcam.json?.height === 720 && vcam.json?.slots === 2, '1280x720 double buffer');
checks.assert(vcam.json?.nv12Bytes === 1382400, 'NV12 byte count');

finish(checks);
