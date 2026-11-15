import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const html = readFileSync(new URL('../firmware/main/settings.html', import.meta.url), 'utf8');
const source = html.split('// BEGIN REQUEST QUEUE')[1].split('// END REQUEST QUEUE')[0];
const createQueue = vm.runInNewContext(source + '\ncreateRequestQueue', {
  Promise, AbortController, Response, TextDecoder, TypeError, setTimeout, clearTimeout,
});
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let active = 0, maximum = 0, calls = [];
const request = createQueue(async (url, options) => {
  active++; maximum = Math.max(maximum, active); calls.push(url);
  return {ok:true, status:200, headers:{}, arrayBuffer:async () => {
    await delay(10); active--; return new TextEncoder().encode(url).buffer;
  }};
});
const values = await Promise.all(['/profile', '/avatar', '/photo'].map(path => request(path).then(r => r.text())));
assert.equal(maximum, 1); assert.deepEqual(values, calls);

let attempts = 0;
const recovering = createQueue(async () => {
  attempts++;
  if (attempts === 1) throw new TypeError('Load failed');
  return new Response('complete');
}, {retryDelay:1});
assert.equal(await (await recovering('/avatar')).text(), 'complete'); assert.equal(attempts, 2);
attempts = 0;
const writing = createQueue(async () => { attempts++; throw new TypeError('Load failed'); }, {retryDelay:1});
await assert.rejects(writing('/badge', {method:'POST'}), /保存结果未确认/); assert.equal(attempts, 1);

attempts = 0;
const denied = createQueue(async () => { attempts++; return new Response('closed', {status:403}); });
await assert.rejects(denied('/profile'), error => error.status === 403); assert.equal(attempts, 1);
// 超时覆盖读取响应体，失败后队列能继续完成下一项。
attempts = 0;
const timeout = createQueue(async (url, {signal}) => {
  attempts++;
  if (url === '/next') return new Response('ok');
  return {ok:true, status:200, headers:{}, arrayBuffer:() => new Promise((resolve, reject) => {
    signal.addEventListener('abort', () => reject(Object.assign(new Error('timeout'), {name:'AbortError'})));
  })};
}, {readTimeout:10, retryDelay:1});
const failed = timeout('/photo'); const next = timeout('/next');
await assert.rejects(failed, /连接中断/);
assert.equal(await (await next).text(), 'ok'); assert.equal(attempts, 3);
console.log('Web transport tests passed: body serialization, safe GET retry, POST once, 403, body timeout and queue recovery.');

// 同一份校验函数用于手机浏览器与 Mac 备份窗口。
const checksum = html.slice(html.indexOf('function crc32('), html.indexOf('async function sendImage('));
const format = html.slice(html.indexOf('function encodeImage('), html.indexOf('async function backupAction('));
const deviceKeys = ['brightness', 'volume', 'sleepSeconds', 'powerMode', 'deepSleepSeconds'];
const {encodeImage, validateBackup} = vm.runInNewContext(checksum + format + '\n({encodeImage, validateBackup})', {
  btoa, atob, Uint8Array, encoder: new TextEncoder(), deviceKeys,
});
const photo = new Uint8Array(153600);
photo[0] = 91;
const saved = {
  format: 'folotoy-passport', version: 1,
  identity: null, photos: [null, encodeImage(photo), null], selectedPhoto: 1,
  music: {enabled: true, selection: 0, custom: ''},
  device: {brightness: 80, volume: 40, sleepSeconds: 60, powerMode: 0, deepSleepSeconds: 300},
};
assert.deepEqual(validateBackup(saved).photos[1], photo);
assert.throws(() => validateBackup({...saved, selectedPhoto: 2}), /目录/);
assert.throws(() => validateBackup({...saved, version: 99}), /格式/);
assert.throws(() => validateBackup({...saved, photos: [null, {...saved.photos[1], crc32: '00000000'}, null]}), /校验/);
assert.throws(() => validateBackup({...saved, device: {...saved.device, deepSleepSeconds: 301}}), /显示设置/);
assert.equal(validateBackup(saved).photos.length, 6);
const jpeg = new Uint8Array(32);
jpeg.set([255,216], 0);
jpeg.set([255,217], jpeg.length - 2);
const newer = {...saved, version:2, photos:[null, null, null, null, null, encodeImage(jpeg)], selectedPhoto:5};
assert.deepEqual(validateBackup(newer).photos[5], jpeg);
const tooBig = new Uint8Array(80 * 1024 + 1);
tooBig.set([255,216]);
tooBig.set([255,217], tooBig.length - 2);
assert.throws(() => validateBackup({...newer, photos:[null,null,null,null,null,encodeImage(tooBig)]}), /尺寸/);
console.log('Backup format checks passed: byte-for-byte photo, checksum, version and selected-slot validation.');
