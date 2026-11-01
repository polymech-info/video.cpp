import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());

const offer = run(['signal', '--json', JSON.stringify({ type: 'ice-candidate', candidate: 'a', sdp: '' })]);
checks.assert(offer.status === 0 && offer.json?.type === 'candidate', 'ice-candidate normalizes to candidate');
checks.assert(offer.json?.from === 'host' && offer.json?.id === 7, 'default from and id');

const bye = run(['signal', '--json', '{"type":"bye"}']);
checks.assert(bye.status === 0 && bye.json?.type === 'bye', 'bye');

const bad = run(['signal', '--json', '{']);
checks.assert(bad.status === 1 && bad.json?.ok === false && bad.json?.code === 'bad-json', 'broken JSON fails');

const caps = run(['caps']);
checks.assert(caps.status === 0 && caps.json?.compiled === false, 'standalone build has no WebRTC peer');
checks.assert(caps.json?.implementation === 'disabled', 'implementation is disabled');
checks.assert(caps.json?.signalEndpoint === '/signal', 'signal endpoint');

finish(checks);
