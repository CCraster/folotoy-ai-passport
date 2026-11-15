// Rebuild the flash artwork with: node tools/pixel_city_art.mjs
// Coordinates below use the 240x248 street in the approved preview.
import { writeFileSync } from 'node:fs';

const width = 240, height = 320, top = 40;
const state = { mode: 'day' };
let raster;
let tx = 0, ty = top, scale = 1;
const ctx = {
  save() {}, restore() { tx=0; ty=top; scale=1; },
  translate(x,y) { tx=x; ty=top+y; },
  scale(x) { scale=x; },
  fillStyle: '#000000',
  fillRect(x,y,w,h) {
    const x0 = Math.round(tx + x*scale), y0 = Math.round(ty + y*scale);
    const x1 = Math.round(tx + (x+w)*scale), y1 = Math.round(ty + (y+h)*scale);
    paint(x0,y0,x1,y1,this.fillStyle);
  }
};

function paint(x0,y0,x1,y1,hex) {
  const rgb = Number.parseInt(hex.slice(1,7),16);
  const alpha = hex.length===9 ? Number.parseInt(hex.slice(7),16) : 255;
  for (let y=Math.max(0,y0);y<Math.min(height,y1);++y) {
    for (let x=Math.max(0,x0);x<Math.min(width,x1);++x) {
      const i=(y*width+x)*3;
      for (let c=0;c<3;++c) {
        const v=(rgb >> (16-c*8)) & 255;
        raster[i+c]=Math.round((v*alpha+raster[i+c]*(255-alpha))/255);
      }
    }
  }
}
const palettes = {
  night:{sky:'#0c1827',horizon:'#1d3541',far:'#203c46',farEdge:'#2e4b50',wall:'#34494d',side:'#23373d',roof:'#526367',brick:'#40565a',shadow:'#1c3036',window:'#142e35',lit:'#e4c789',light:'#fff2c4',road:'#14232b',stripe:'#65746c',ground:'#394e50',leaf:'#28594c',leafLight:'#3c7860',trunk:'#657069',cloud:'#203441'},
  dusk:{sky:'#514357',horizon:'#d68d76',far:'#655967',farEdge:'#7c6b75',wall:'#777173',side:'#595765',roof:'#a18e88',brick:'#93817b',shadow:'#424658',window:'#394552',lit:'#f5c588',light:'#ffe3ae',road:'#343645',stripe:'#a49c89',ground:'#736e6d',leaf:'#426a5a',leafLight:'#63816a',trunk:'#8c8172',cloud:'#aa7d7c'},
  day:{sky:'#92bbc2',horizon:'#c7d4bf',far:'#87a4a3',farEdge:'#9eb7b2',wall:'#b7bca8',side:'#8a9e95',roof:'#d3d0b5',brick:'#c2c4af',shadow:'#658b87',window:'#4c8187',lit:'#81b2b0',light:'#dfe5c6',road:'#526f74',stripe:'#b8c2aa',ground:'#a1ad9b',leaf:'#42826b',leafLight:'#65a077',trunk:'#8d9580',cloud:'#d7e5d3'}
};
const font = {'A':['010','101','111','101','101'],'B':['110','101','110','101','110'],'C':['011','100','100','100','011'],'E':['111','100','110','100','111'],'F':['111','100','110','100','100'],'I':['111','010','010','010','111'],'N':['101','111','111','111','101'],'O':['010','101','101','101','010'],'P':['110','101','110','100','100'],'R':['110','101','110','101','101'],'T':['111','010','010','010','010'],'Y':['101','101','010','010','010'],'2':['110','001','010','100','111'],'4':['101','101','111','001','001']};
function rect(x,y,w,h,c){ctx.fillStyle=c;ctx.fillRect(Math.round(x),Math.round(y),Math.round(w),Math.round(h));}
function text(word,x,y,c){for(const ch of word){const grid=font[ch];if(grid)grid.forEach((row,j)=>[...row].forEach((v,i)=>{if(v==='1')rect(x+i,y+j,1,1,c);}));x+=4;}}
function poly(points, c) {
  const ys=points.map(p=>p[1]);
  for(let y=Math.min(...ys);y<Math.max(...ys);++y) {
    const intersections=[];
    for (let i=0;i<points.length;++i) {
      const [x1,y1]=points[i], [x2,y2]=points[(i+1)%points.length];
      if ((y1<=y && y<y2)||(y2<=y && y<y1)) intersections.push(x1+(x2-x1)*(y-y1)/(y2-y1));
    }
    if(intersections.length===2) rect(Math.min(...intersections),y,Math.abs(intersections[0]-intersections[1]),1,c);
  }
}
function cloud(x,y,w,p){rect(x,y,w,4,p.cloud);rect(x+5,y-3,w-13,4,p.cloud);rect(x+12,y-6,w-27,4,p.cloud);}
function windowAt(x,y,w,h,on,p){rect(x-1,y-1,w+2,h+2,p.shadow);rect(x,y,w,h,on?p.lit:p.window);if(on){rect(x+1,y+1,w-2,2,p.light);rect(x+1,y+3,2,h-4,p.lit);}else rect(x+1,y+1,w-2,2,p.farEdge);rect(x+Math.floor(w/2),y,1,h,p.side);rect(x-2,y+h,w+4,2,p.roof);}
function planter(x,y,p){rect(x,y,8,5,p.roof);rect(x+1,y+4,6,2,p.side);rect(x+1,y-4,2,5,p.leafLight);rect(x+4,y-7,2,8,p.leaf);rect(x+6,y-3,2,4,p.leafLight);}
function tree(x,y,p,t,large=true){const sway=Math.sin(t*.75+x)>.4?1:0;rect(x,y-23,3,26,p.trunk);rect(x-5,y-5,8,2,p.trunk);let s=large?1:.7;ctx.save();ctx.translate(x+sway,y-23);ctx.scale(s,s);[[ -13,-7,23,13],[-10,-15,17,11],[-6,-20,12,6],[-17,-4,6,8],[4,-10,10,13]].forEach(a=>rect(...a,p.leaf));rect(-9,-14,10,4,p.leafLight);rect(-13,-5,5,4,p.leafLight);rect(3,-8,5,3,p.leafLight);rect(-4,-18,6,2,p.leafLight);ctx.restore();rect(x-7,y+3,16,2,p.shadow);}
function lamp(x,p){const lit=state.mode!=='day';if(lit){poly([[x-3,158],[x+4,158],[x+19,211],[x-18,211]],state.mode==='night'?'#f3dc9b14':'#f9d99812');rect(x-12,211,26,2,p.ground);}rect(x,159,2,54,p.shadow);rect(x-3,212,8,3,p.side);rect(x-4,156,10,3,p.roof);rect(x-3,159,8,4,lit?p.light:p.side);rect(x-2,153,6,3,p.side);}
function scene(t){const p=palettes[state.mode];const night=state.mode==='night';
  rect(0,-top,240,248+top,p.sky);rect(0,55,240,44,p.horizon);
  if(night){for(let i=0;i<18;i++){const x=(i*47+23)%237,y=(i*19+8)%65;rect(x,y,1,1,(Math.sin(t*.6+i*4)>.6)?'#a4c6bb':'#466577');}rect(183,18,13,13,'#d7e5c1');rect(181,21,17,7,'#d7e5c1');rect(180,16,11,11,p.sky);}
  else{rect(181,13,17,17,state.mode==='day'?'#f4e6b0':'#f2c79d');rect(178,17,23,9,state.mode==='day'?'#f4e6b0':'#f2c79d');}
    cloud(10,38,53,p);cloud(132,65,43,p);
  const farBuildings=[[0,89,22,55],[26,75,20,65],[49,98,19,43],[70,64,25,84],[99,89,22,63],[127,79,28,73],[160,92,21,53],[186,72,23,80],[214,97,26,45]];
  farBuildings.forEach(([x,y,w,h],j)=>{rect(x,y,w,h,p.far);rect(x,y,w,2,p.farEdge);if(j===3){rect(x+8,y-11,8,11,p.far);rect(x+12,y-19,1,8,p.farEdge);}for(let a=0;a<Math.floor(w/6);a++)for(let b=0;b<4;b++)if((a+b+j)%3)rect(x+4+a*6,y+7+b*10,2,3,night&&((j+a+b)%4===0)?'#718876':p.farEdge);});
  rect(0,144,240,73,p.shadow);
  // 三座建筑采用不同轮廓，近景细节始终按设备原生像素绘制。
  rect(8,127,67,77,p.wall);rect(5,123,72,5,p.roof);rect(9,128,3,76,p.brick);rect(70,127,5,77,p.side);
  for(let i=0;i<3;i++)windowAt(18+i*17,136,9,12,night&&i!==1,p);
  rect(12,161,58,6,p.shadow);text('CAFE',34,161,'#32f08c');
  rect(11,169,61,7,'#2d916c');for(let i=0;i<6;i++)rect(11+i*10,169,5,7,'#81b994');rect(11,176,61,2,p.side);
  rect(15,179,30,22,p.window);rect(17,181,26,17,night?'#d4b97e':'#739c8b');rect(28,181,2,20,p.side);rect(16,194,28,2,p.side);
  rect(50,179,15,25,p.side);rect(52,181,11,19,night?'#b6aa75':'#70988c');rect(59,190,1,3,p.light);rect(12,202,59,3,p.roof);
  rect(19,197,3,3,p.shadow);rect(23,198,8,2,p.shadow);rect(36,196,5,4,p.leaf);
  rect(81,92,77,114,p.wall);rect(155,97,6,109,p.side);rect(78,88,81,5,p.roof);rect(81,93,74,3,p.brick);
  rect(89,80,21,8,p.side);rect(90,79,20,2,p.roof);rect(94,75,11,4,p.roof);rect(141,69,2,18,p.side);rect(134,74,16,1,p.roof);rect(138,70,8,1,p.roof);
  planter(119,83,p);planter(132,83,p);
  for(let row=0;row<3;row++){for(let col=0;col<3;col++){const i=row*3+col;let on=state.mode!=='day'&&[0,2,3,7,8].includes(i);if(i===5)on=state.mode!=='day'&&Math.floor(t/13)%2===1;windowAt(90+col*22,103+row*25,12,15,on,p);}rect(84,123+row*25,69,2,p.brick);}
  rect(84,178,68,27,p.side);rect(87,181,28,22,p.window);rect(90,184,22,15,night?'#3c7862':'#719c90');rect(98,181,2,22,p.roof);rect(120,180,27,7,p.shadow);text('24',129,181,'#32f08c');rect(123,191,20,14,p.window);rect(131,193,1,12,p.roof);
  rect(173,132,61,74,p.wall);rect(169,126,67,6,p.roof);rect(229,132,5,74,p.side);
  for(let j=0;j<2;j++)windowAt(181+j*24,141,14,17,state.mode!=='day'&&j===0,p);
  rect(175,166,53,3,p.roof);rect(176,172,50,28,p.window);rect(178,174,46,25,state.mode==='day'?'#779c90':'#6c8f75');
  for(let j=0;j<3;j++){rect(181+j*14,179,10,15,p.side);rect(183+j*14,182,6,8,p.roof);rect(185+j*14,184,3,4,p.window);}
  rect(170,204,64,3,p.roof);rect(227,178,9,25,p.shadow);rect(228,180,7,10,'#32a978');rect(229,181,5,2,'#a3dcac');rect(230,194,3,3,'#9ac3a5');
  for(let i=0;i<15;i++){const x=12+(i*31)%216,y=133+(i*19)%67;if((x<72||x>174)&&y<166)rect(x,y,4,1,p.brick);}
  // 电线、街灯、树木形成前后层次，暖光仅用在生活细节。
  rect(2,121,1,86,p.side);for(let x=2;x<240;x++)rect(x,113+Math.round(8*Math.sin(x/240*Math.PI)),1,1,p.side);
  rect(0,207,240,13,p.ground);rect(0,207,240,2,p.roof);for(let x=0;x<240;x+=17)rect(x,209,1,9,p.brick);rect(0,218,240,3,p.shadow);
  tree(15,207,p,t,true);tree(165,207,p,t,false);planter(76,202,p);
  rect(46,207,16,2,p.roof);rect(48,210,2,7,p.side);rect(59,210,2,7,p.side);rect(46,211,16,2,p.roof);
  lamp(155,p);lamp(235,p);
  rect(0,221,240,height-top-221,p.road);rect(0,221,240,1,p.roof);for(let x=2;x<240;x+=35)rect(x,241,16,2,p.stripe);
  if(night){rect(20,227,23,1,'#4c6254');rect(26,229,13,1,'#334d48');rect(119,230,17,1,'#2d5b4e');rect(125,232,21,1,'#25453e');rect(207,224,15,1,'#405b4f');}
  rect(0,height-top-15,240,1,p.shadow);
  rect(0,height-top-14,240,14,p.ground);
  rect(0,height-top-14,240,2,p.roof);
}

// Frame order stays in sync with pixel_city.c. RGB565, little endian, no padding.
function finishSky() {
  for (let y = 0; y < 42; ++y) {
    const alpha = Math.round(170 * (1 - y / 42)).toString(16).padStart(2, '0');
    paint(0, y, width, y + 1, '#080d16' + alpha);
  }
}

const output = Buffer.alloc(width * height * 2 * 3);
for (const [i, lighting] of ['day', 'dusk', 'night'].entries()) {
  state.mode = lighting;
  raster = new Uint8Array(width * height * 3);
  scene(7);
  finishSky();
  for(let pixel=0;pixel<width*height;++pixel) {
    const [r,g,b]=raster.subarray(pixel*3,pixel*3+3);
    output.writeUInt16LE((r>>3)<<11 | (g>>2)<<5 | b>>3, (i*width*height+pixel)*2);
  }
}
writeFileSync(new URL('../firmware/main/art/pixel_city.rgb565', import.meta.url), output);

// Additional scenes use a 2x pixel grid. The screen can draw them straight from
// flash at their display size without keeping a full-size copy in RAM.
function landscape(p) {
  rect(0, -top, 240, height, p.sky);
  rect(0, 44, 240, 57, p.horizon);
  cloud(28, 39, 64, p);
  cloud(116, 22, 52, p);
  if (state.mode === 'night') {
    for (let i = 0; i < 19; ++i)
      rect((i * 47 + 23) % 235, (i * 19) % 78 - 5, 2, 2, i % 3 ? p.farEdge : p.light);
    rect(181, 38, 17, 17, p.light);
    rect(176, 35, 15, 14, p.sky);
  } else {
    rect(172, 35, 32, 18, p.light);
    rect(178, 28, 20, 31, p.light);
  }
}

function park(p) {
  landscape(p);
  poly([[0,94],[52,55],[100,88],[151,58],[240,102],[240,180],[0,180]], p.far);
  poly([[0,100],[44,81],[99,112],[180,87],[240,102],[240,176],[0,176]], p.leaf);
  rect(0,122,240,158,p.ground);
  poly([[0,154],[72,131],[182,135],[240,169],[240,198],[155,211],[58,206],[0,187]], p.leaf);
  poly([[17,164],[71,143],[180,150],[223,170],[213,193],[147,203],[53,193]], p.window);
  for (let i = 0; i < 32; ++i) {
    const x = 44 + (i * 37) % 148;
    const y = 153 + (i * 13) % 32;
    rect(x, y, i % 2 ? 12 : 6, 2, p.farEdge);
  }
  // Pavilion, footbridge and reed beds give the activity in front of the pond a frame.
  rect(143,113,54,13,p.shadow);
  poly([[138,113],[153,99],[186,99],[201,113]],p.roof);
  for (const x of [145,169,192]) rect(x,113,4,26,p.side);
  rect(141,137,60,4,p.roof);
  poly([[52,166],[80,155],[109,157],[129,170],[129,177],[52,172]],p.trunk);
  for (const x of [59,74,89,104,119]) rect(x,155+(x>100?5:0),3,17,p.side);
  rect(0,224,240,35,p.road);
  rect(0,224,240,3,p.roof);
  rect(0,258,240,2,p.side);
  for (let x = 8; x < 240; x += 31) rect(x, 253, 13, 2, p.stripe);
  for (const [x,y] of [[14,156],[33,207],[206,213],[228,149]]) tree(x,y,p,8,true);
  rect(136,212,30,5,p.trunk);
  rect(136,219,30,3,p.trunk);
  rect(139,219,3,7,p.side);
  rect(161,219,3,7,p.side);
  for (let i = 0; i < 62; ++i) {
    const x = (i * 67) % 237;
    const y = i % 2 ? 268 + i % 10 : 207 + i % 14;
    rect(x,y,2,4,p.leaf);
    rect(x-1,y,4,2,i % 3 ? p.light : '#e3a0a1');
  }
}

function harbor(p) {
  landscape(p);
  const sea = state.mode === 'day' ? '#508f9c' : state.mode === 'dusk' ? '#647d90' : '#203f59';
  poly([[0,106],[49,61],[86,74],[130,113],[184,76],[240,103],[240,127],[0,127]], p.far);
  rect(0,110,240,170,sea);
  for (let i = 0; i < 90; ++i) {
    const x = (i * 37 + 11) % 239, y = 113 + (i * 53) % 163;
    rect(x,y,3+(i % 6)*4,2,i % 5 ? p.farEdge : p.cloud);
  }
  for (let i = 0; i < 14; ++i) rect(173+(i*13)%28,115+5*i,4+i%3*5,2,p.light);
  poly([[0,188],[44,171],[109,177],[135,191],[70,212],[0,208]], p.shadow);
  rect(0,190,85,10,p.roof);
  // Lighthouse on the breakwater.
  poly([[28,185],[34,111],[49,111],[56,185]],p.wall);
  rect(33,135,17,12,'#b97069');
  rect(31,112,21,7,p.side);
  rect(35,100,14,12,p.light);
  rect(39,100,3,12,p.side);
  poly([[30,100],[42,88],[54,100]],'#b97069');
  rect(40,162,6,22,p.window);
  poly([[0,242],[71,218],[240,222],[240,280],[0,280]],p.ground);
  for (let y = 241; y < 280; y += 8) rect(0,y,240,2,p.trunk);
  for (let x = 0; x < 240; x += 20) rect(x,232,3,17,p.side);
  rect(0,233,240,3,p.roof);
  rect(190,244,18,5,p.trunk);
  rect(191,249,2,8,p.side);
  rect(206,249,2,8,p.side);
}

// Ordered as park day/dusk/night, then harbor day/dusk/night.
const small = Buffer.alloc(width * height / 4 * 2 * 6);
let offset = 0;
for (const draw of [park, harbor]) {
  for (const light of ['day', 'dusk', 'night']) {
    state.mode = light;
    raster = new Uint8Array(width * height * 3);
    draw(palettes[light]);
    finishSky();
    for (let y = 0; y < height; y += 2) {
      for (let x = 0; x < width; x += 2) {
        const index = (y * width + x) * 3;
        const [r, g, b] = raster.subarray(index, index + 3);
        small.writeUInt16LE((r >> 3) << 11 | (g >> 2) << 5 | b >> 3, offset);
        offset += 2;
      }
    }
  }
}
writeFileSync(new URL('../firmware/main/art/pixel_scenes.rgb565', import.meta.url), small);
