const fs = require('fs');
const path = require('path');
// Optional resource regeneration only; normal C++ builds use committed PNG/ICO files.
const sharp = require(process.env.TOKENSCOPE_SHARP_PATH || 'sharp');
(async () => {
  const sources = {
    codex: 'codex.svg', 'claude-code': 'claude-code.png', trae: 'trae.png',
    workbuddy: 'workbuddy.svg', 'deepseek-harness': 'deepseek-harness.svg', opencode: 'opencode.png',
    cursor: 'cursor.svg', 'github-copilot': 'github-copilot.svg', codebuddy: 'codebuddy.svg',
    gemini: 'gemini-brand.png', 'qwen-code': 'qwen-code.png', kimi: 'kimi-brand.png',
    amp: 'client-amp.png', antigravity: 'client-antigravity.png', cline: 'client-cline.png',
    'roo-code': 'client-roocode.png', 'kilo-code': 'client-kilocode.png', 'mimo-code': 'client-micode.jpg',
    codebuff: 'client-codebuff.png', commandcode: 'client-commandcode.png', pi: 'client-pi.png',
    openclaw: 'client-openclaw.jpg', junie: 'client-junie.png', 'grok-build': 'client-grok.png',
    jcode: 'client-jcode.png', devin: 'client-devin.jpg', zcode: 'client-zcode.png',
    opencodereview: 'client-opencodereview.png', crush: 'client-crush.png', droid: 'client-droid.png',
    fx: 'client-fx.png', goose: 'client-goose.png', hermes: 'client-hermes.png', kiro: 'client-kiro.jpg',
    mux: 'client-mux.png', orca: 'client-orca.png', warp: 'client-warp.png', zed: 'client-zed.webp'
  };
  for (const [name, file] of Object.entries(sources)) {
    const source = path.join('design', 'assets', 'brands', file);
    await sharp(source, { density: 192 }).resize(64,64,{fit:'contain',background:{r:255,g:255,b:255,alpha:0}}).png().toFile(path.join('resources','brands',name+'.png'));
  }
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64"><rect width="64" height="64" rx="15" fill="#3565DF"/><path d="M14 43V32h8v11zm14 0V21h8v22zm14 0V13h8v30z" fill="white"/><path d="M12 49h40" stroke="#C7D7FF" stroke-width="3" stroke-linecap="round"/></svg>`;
  const png = await sharp(Buffer.from(svg)).png().toBuffer();
  const hdr = Buffer.alloc(22); hdr.writeUInt16LE(1,2);hdr.writeUInt16LE(1,4);hdr[6]=64;hdr[7]=64;hdr.writeUInt16LE(1,10);hdr.writeUInt16LE(32,12);hdr.writeUInt32LE(png.length,14);hdr.writeUInt32LE(22,18);
  fs.writeFileSync('resources/app.ico',Buffer.concat([hdr,png]));
  console.log('Brand PNG resources and app.ico generated.');
})();
