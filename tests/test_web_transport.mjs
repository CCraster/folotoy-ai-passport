import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const html = readFileSync(new URL('../firmware/main/settings.html', import.meta.url), 'utf8');
const translate = zh => zh;
const source = html.split('// BEGIN REQUEST QUEUE')[1].split('// END REQUEST QUEUE')[0];
const createQueue = vm.runInNewContext(source + '\ncreateRequestQueue', {
  translate, Promise, AbortController, Response, TextDecoder, TypeError, setTimeout, clearTimeout,
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

const photoEncoder = html.slice(html.indexOf('const PHOTO_JPEG_LIMIT'), html.indexOf('async function drawPhoto('));
const {encodePhoto, PHOTO_JPEG_LIMIT} = vm.runInNewContext(photoEncoder + '\n({encodePhoto, PHOTO_JPEG_LIMIT})', {
  translate, Uint8Array,
});

function mockPhotoCanvas(sizes, mime = 'image/jpeg') {
  const qualities = [];
  let bodyReads = 0;
  return {
    qualities,
    get bodyReads() { return bodyReads; },
    toBlob(callback, type, quality) {
      assert.equal(type, 'image/jpeg');
      qualities.push(quality);
      const size = sizes[Math.min(qualities.length - 1, sizes.length - 1)];
      callback({type: mime, size, arrayBuffer: async () => {
        bodyReads++;
        return new ArrayBuffer(size);
      }});
    },
  };
}

const fits = mockPhotoCanvas([PHOTO_JPEG_LIMIT]);
assert.equal((await encodePhoto(fits)).length, PHOTO_JPEG_LIMIT);
assert.deepEqual(fits.qualities, [0.90]);
const detailed = mockPhotoCanvas([PHOTO_JPEG_LIMIT + 1, PHOTO_JPEG_LIMIT + 1, 60 * 1024]);
assert.equal((await encodePhoto(detailed)).length, 60 * 1024);
assert.deepEqual(detailed.qualities, [0.90, 0.82, 0.74]);
assert.equal(detailed.bodyReads, 1);
const oversize = mockPhotoCanvas([PHOTO_JPEG_LIMIT + 1]);
await assert.rejects(encodePhoto(oversize), /超过 80 KiB/);
assert.equal(oversize.qualities.at(-1), 0.40);
assert.equal(oversize.bodyReads, 0);
await assert.rejects(encodePhoto(mockPhotoCanvas([32], 'image/png')), /无法生成 JPEG/);
console.log('Photo encoding tests passed: original quality, compression fallback, and size and MIME limits.');

// 同一份校验函数用于手机浏览器与 Mac 备份窗口。
const checksum = html.slice(html.indexOf('function crc32('), html.indexOf('async function sendImage('));
const format = html.slice(html.indexOf('function encodeImage('), html.indexOf('async function backupAction('));
const deviceKeys = ['brightness', 'volume', 'sleepSeconds', 'powerMode', 'deepSleepSeconds'];
const {encodeImage, validateBackup} = vm.runInNewContext(checksum + format + '\n({encodeImage, validateBackup})', {
  translate, btoa, atob, Uint8Array, encoder: new TextEncoder(), deviceKeys, PHOTO_JPEG_LIMIT,
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
assert.throws(() => validateBackup({...saved, device: {...saved.device, deepSleepSeconds: 301}}), /显示与待机设置/);
assert.equal(validateBackup(saved).photos.length, 6);
assert.equal(validateBackup(saved).language, undefined);
assert.equal(validateBackup({...saved, language:'en'}).language, 'en');
assert.throws(() => validateBackup({...saved, language:'fr'}), /语言设置/);
assert.throws(() => validateBackup({...saved, identity:{name:5}}), /身份卡文字/);
const jpeg = new Uint8Array(32);
jpeg.set([255,216], 0);
jpeg.set([255,217], jpeg.length - 2);
const newer = {...saved, version:2, photos:[null, null, null, null, null, encodeImage(jpeg)], selectedPhoto:5};
assert.deepEqual(validateBackup(newer).photos[5], jpeg);
const tooBig = new Uint8Array(PHOTO_JPEG_LIMIT + 1);
tooBig.set([255,216]);
tooBig.set([255,217], tooBig.length - 2);
assert.throws(() => validateBackup({...newer, photos:[null,null,null,null,null,encodeImage(tooBig)]}), /尺寸/);
console.log('Backup format checks passed: byte-for-byte photo, checksum, version and selected-slot validation.');

const between = (start, end) => html.slice(html.indexOf(start), html.indexOf(end, html.indexOf(start)));
const fields = new Map();
const field = id => {
  if (!fields.has(id)) fields.set(id, {disabled:true, hidden:true, dataset:{}, setAttribute() {}});
  return fields.get(id);
};
const writes = [];
let reloaded = false;
const languageSandbox = {
  byId:field, deviceLanguage:'zh-CN', language:'zh-CN', languageOverride:null, translate,
  request:async (path, options) => {
    writes.push([path, options.method, options.body]);
    return new Response(options.body);
  },
  reloadDevicePanel() { reloaded = true; },
};
vm.runInNewContext(between('async function saveDeviceLanguage', 'const panels ='), languageSandbox);
field('language').value = 'zh-CN';
await field('saveLanguage').onclick();
assert.deepEqual(writes.at(-1), ['/language', 'POST', 'zh-CN']);
assert.equal(reloaded, false);
languageSandbox.languageOverride = 'en';
await vm.runInNewContext("saveDeviceLanguage('en')", languageSandbox);
assert.deepEqual(writes.at(-1), ['/language', 'POST', 'en']);

const network = vm.runInNewContext(between('async function loadNetworkPanel', 'let profileRequest;') + '\ninitializeDevice', {
  byId:field, translate,
  request:async () => new Response(JSON.stringify({brightness:60,volume:40,sleepSeconds:60,powerMode:1,deepSleepSeconds:900,
    storage:{total:8*1048576,firmware:2.25*1048576,firmwareCapacity:3*1048576,assetsCapacity:1048576,badge:0,photo:0}})),
  readNetwork:async () => { throw new Error('Network state unavailable'); },
  updateDevicePreview() {},
});
await network();
assert.equal(field('saveDevice').disabled, false);
assert.equal(field('storageBar').value, 0.75);
assert.equal(field('networkRetry').hidden, false);
assert.equal(field('networkState').textContent, 'Network state unavailable');
console.log('Settings checks passed: explicit language writes, independent network errors, and app partition usage.');
