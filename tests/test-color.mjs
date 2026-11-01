import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());

const ok = run(['color', '--hex', '#C45C6A']);
checks.assert(ok.status === 0 && ok.json?.ok === true, 'hex exits 0');
checks.assert(ok.json?.r === 196 && ok.json?.g === 92 && ok.json?.b === 106, 'C45C6A channels');

const bare = run(['color', '--hex', '00ff00']);
checks.assert(bare.status === 0 && bare.json?.g === 255 && bare.json?.r === 0, 'RRGGBB without hash');

const bad = run(['color', '--hex', 'zz']);
checks.assert(bad.status === 1 && bad.json?.ok === false && bad.json?.code === 'bad-hex', 'bad hex is a failure object');

const usage = run(['color']);
checks.assert(usage.status === 2, 'missing --hex is usage');

finish(checks);
