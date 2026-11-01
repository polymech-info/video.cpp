import { writeFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { PKG, createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());

const iou = run(['iou']);
checks.assert(iou.status === 0 && iou.json?.same === 1, 'identical boxes have IoU 1');
checks.assert(iou.json?.none === 0, 'disjoint boxes have IoU 0');
checks.assert(typeof iou.json?.probiou === 'number' && iou.json.probiou > 0.99, 'identical oriented boxes');

const nms = run(['nms']);
checks.assert(nms.status === 0 && nms.json?.kept === 2, 'NMS keeps the other class');

const labels = run(['labels']);
checks.assert(labels.status === 0 && labels.json?.dota === 15, 'DOTA has 15 classes');
checks.assert(labels.json?.skeleton === 19, 'COCO skeleton has 19 edges');
checks.assert(labels.json?.task === 'pose', 'pose task name');

const file = join(PKG, 'build', 'labels-fixture.txt');
writeFileSync(file, ' cat \n\n dog\n');
const loaded = run(['labels', '--file', file]);
checks.assert(loaded.status === 0 && loaded.json?.loaded === 2, 'label file skips blanks');
unlinkSync(file);

finish(checks);
