// SPDX-License-Identifier: GPL-3.0-or-later
//
// Silent Hill memory card converter core (browser + Node). Mirrors ShSaveCard.cs;
// keep the two in step. Format notes: pc_port/docs/Save_Format_And_Converter.md.
(function (root) {
  'use strict';

  const FRAME = 128, BLOCK = 8192, CARD = 16 * BLOCK, DIRS = 15;
  const GME_HDR = 0xF40, VMP_HDR = 0x80;
  const SLOTS = 11, FILES_MAX = 15;
  const HDR_OFS = 0x200, CFG_OFS = 0x300, SLOT_OFS = 0x380, SLOT_SIZE = 640;

  const REGIONS = {
    usa: { prefix: 'BASLUS-00707SILENT', label: 'USA', game: 'Silent Hill (USA)' },
    eur: { prefix: 'BESLES-01514SILENT', label: 'Europe', game: 'Silent Hill (Europe) (En,Fr,De,Es,It)' },
    jpn: { prefix: 'BISLPM-86192SILENT', label: 'Japan', game: 'Silent Hill (Japan)' },
  };

  const LOCATIONS = ['Anywhere', 'Cafe', 'Bus', 'Store', 'Infirmary', 'Doghouse', 'Gordon', 'Church', 'Garage',
    'Police', 'Reception', 'Room 302', "Director's Office", 'Jewelry Shop', 'Pool Hall', 'Antique Shop',
    'Theme Park', 'Boat', 'Bridge', 'Motel', 'Lighthouse', 'Sewer', 'Nowhere', "Child's Room", 'Next Fear'];

  // PC port cards: 0..3 = port 1 slots A-D (multitap), 8..11 = port 2 slots A-D.
  const PC_CARD_NUMBERS = [0, 1, 2, 3, 8, 9, 10, 11];
  const PC_OUTPUT_ORDER = [0, 8, 1, 2, 3, 9, 10, 11];

  // Save icon from the retail disc (16-colour CLUT + 16x16 4bpp bitmap).
  const ICON = hex(
    '00804384628c6588a498a78cc89407a1ea9427a92c996ca18fa5f3a916ae37ae' +
    '0033556536110000103311011131030010c6ac581121130000fceeff1a112200' +
    '00eddefe3d10220010fdefee6d11220010a53a638a15220000a51c00a55c7400' +
    '00ecdecbde7cbb0000ecdddebc794b0000a8c8ac789904000085ca8b76990400' +
    '008155657777090000a0aa5844940b0000506a2422b46c000000300131cadc06');

  function hex(s) {
    const b = new Uint8Array(s.length / 2);
    for (let i = 0; i < b.length; i++) b[i] = parseInt(s.substr(i * 2, 2), 16);
    return b;
  }
  const u16 = (b, o) => b[o] | (b[o + 1] << 8);
  const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
  function w16(b, o, v) { b[o] = v & 255; b[o + 1] = (v >> 8) & 255; }
  function w32(b, o, v) { b[o] = v & 255; b[o + 1] = (v >>> 8) & 255; b[o + 2] = (v >>> 16) & 255; b[o + 3] = (v >>> 24) & 255; }

  function extractImage(raw) {
    if (raw.length === CARD) return raw.slice();
    if (raw.length === CARD + GME_HDR && raw[0] === 0x31 && raw[1] === 0x32 && raw[2] === 0x33) return raw.slice(GME_HDR);
    if (raw.length === CARD + VMP_HDR && raw[1] === 0x50 && raw[2] === 0x4D && raw[3] === 0x56) return raw.slice(VMP_HDR);
    return null;
  }

  function readName(b, o) {
    let s = '';
    for (let i = 0; i < 20 && b[o + i]; i++) s += String.fromCharCode(b[o + i]);
    return s;
  }

  /** Parses a card image into {files:[{name,data}], warnings}. Throws on non-card input. */
  function loadCard(raw) {
    const img = extractImage(raw);
    if (!img) throw new Error('Not a PlayStation memory card image. Expected a 128 KB raw card (.mcd, .mcr, .mc), a DexDrive .gme or a PSP .vmp.');
    if (img[0] !== 0x4D || img[1] !== 0x43) throw new Error('The memory card header is missing, so the image is unformatted or not a memory card.');
    const card = { files: [], warnings: [] };
    for (let i = 1; i <= DIRS; i++) {
      const o = i * FRAME;
      if (img[o] !== 0x51) continue;
      const name = readName(img, o + 10);
      if (!name) continue;
      // PC-port cards write 0 as the link of single-block files, so the size
      // field, not the link, decides where a chain ends.
      const blocks = Math.max(1, Math.ceil(u32(img, o + 4) / BLOCK));
      const chain = [];
      let cur = i;
      while (chain.length < blocks && cur >= 1 && cur <= DIRS && !chain.includes(cur)) {
        chain.push(cur);
        const next = u16(img, cur * FRAME + 8);
        cur = next === 0xFFFF ? -1 : next + 1;
      }
      if (chain.length < blocks) {
        card.warnings.push(`File ${name}: block chain is broken (${chain.length} of ${blocks} blocks), skipped.`);
        continue;
      }
      const data = new Uint8Array(blocks * BLOCK);
      chain.forEach((blk, k) => data.set(img.subarray(blk * BLOCK, (blk + 1) * BLOCK), k * BLOCK));
      card.files.push({ name, data });
    }
    return card;
  }

  const blocksOf = f => f.data.length / BLOCK;
  const freeBlocks = card => DIRS - card.files.reduce((n, f) => n + blocksOf(f), 0);

  function put(card, name, data) {
    const existing = card.files.find(f => f.name === name);
    const avail = freeBlocks(card) + (existing ? blocksOf(existing) : 0);
    if (data.length / BLOCK > avail) return false;
    if (existing) existing.data = data; else card.files.push({ name, data });
    return true;
  }

  function sealFrame(img, fr) {
    const o = fr * FRAME;
    let x = 0;
    for (let i = 0; i < FRAME - 1; i++) x ^= img[o + i];
    img[o + FRAME - 1] = x;
  }

  /** Standard, fully checksummed card image, files contiguous from block 1
   *  (the PC port reads a file's data at the block matching its directory slot). */
  function toImage(card) {
    const img = new Uint8Array(CARD);
    img[0] = 0x4D; img[1] = 0x43;
    sealFrame(img, 0);
    let blk = 1;
    for (const f of card.files) {
      const n = blocksOf(f);
      for (let b = 0; b < n; b++, blk++) {
        const o = blk * FRAME;
        img[o] = (n === 1 || b === 0) ? 0x51 : (b === n - 1 ? 0x53 : 0x52);
        if (b === 0) {
          w32(img, o + 4, n * BLOCK);
          for (let i = 0; i < Math.min(20, f.name.length); i++) img[o + 10 + i] = f.name.charCodeAt(i);
        }
        w16(img, o + 8, b === n - 1 ? 0xFFFF : blk);
        sealFrame(img, blk);
        img.set(f.data.subarray(b * BLOCK, (b + 1) * BLOCK), blk * BLOCK);
      }
    }
    for (; blk <= DIRS; blk++) {
      img[blk * FRAME] = 0xA0;
      w16(img, blk * FRAME + 8, 0xFFFF);
      sealFrame(img, blk);
    }
    for (let fr = 16; fr < 36; fr++) {
      w32(img, fr * FRAME, 0xFFFFFFFF);
      w16(img, fr * FRAME + 8, 0xFFFF);
      sealFrame(img, fr);
    }
    img.fill(0xFF, 36 * FRAME, 63 * FRAME);
    img.copyWithin(63 * FRAME, 0, FRAME);
    return img;
  }

  /** {region, index} for Silent Hill save names, else null. */
  function parseShName(name) {
    for (const key of Object.keys(REGIONS)) {
      const p = REGIONS[key].prefix;
      if (name.length === p.length + 2 && name.startsWith(p) && /^\d\d$/.test(name.slice(p.length))) {
        const index = +name.slice(p.length);
        return index < FILES_MAX ? { region: key, index } : null;
      }
    }
    return null;
  }
  const makeName = (region, index) => REGIONS[region].prefix + String(index).padStart(2, '0');

  function recordValid(b, o, size) {
    const f = o + size - 4;
    if (b[f + 2] !== 0xDC || b[f + 3] !== 0xDC) return false;
    let x = 0;
    for (let i = o; i < o + size; i++) if (i !== f && i !== f + 1) x ^= b[i];
    return x === b[f];
  }

  function readSlots(blk) {
    const out = [];
    for (let s = 0; s < SLOTS; s++) {
      const m = HDR_OFS + 4 + s * 12;
      const total = u32(blk, m) | 0;
      if (!total) continue;
      const flags = blk[m + 11];
      const secs = Math.floor(u32(blk, m + 4) / 4096) + ((flags >> 1) & 3) * 290 * 3600;
      const loc = blk[m + 10];
      out.push({
        index: s,
        totalSaveCount: total,
        saveCount: u16(blk, m + 8),
        locationId: loc,
        location: LOCATIONS[loc] || `Location ${loc}`,
        nextFear: !!(flags & 1),
        seconds: secs,
        valid: recordValid(blk, SLOT_OFS + s * SLOT_SIZE, SLOT_SIZE),
      });
    }
    return out;
  }
  const headerValid = blk => recordValid(blk, HDR_OFS, 256);

  // ---- Save editing ------------------------------------------------------
  // s_Savegame (636 bytes at SLOT_OFS + slot * 640). Offsets from
  // include/bodyprog/savegame.h. Values are little-endian; q12 = fixed point / 4096.
  const SAVEGAME = {
    items: 0x000, itemCount: 40,
    field_A0: 0x0A0, mapIdx: 0x0A4, mapRoomIdx: 0x0A5, savegameCount: 0x0A6, locationId: 0x0A8,
    paperMapIdx: 0x0A9, equippedWeapon: 0x0AA, inventorySlotCount: 0x0AB, itemToggleFlags: 0x0AC,
    ovlEnemyStates: 0x0B0, paperMapFlags: 0x164, eventFlags: 0x168, eventFlagWords: 52,
    healthSaturation: 0x238, pickedUpItemCount: 0x23C, inventoryItemFlags: 0x23F, playerHealth: 0x240,
    playerPositionX: 0x244, playerRotationY: 0x248, clearGameCount: 0x24A, clearGameEndings: 0x24B,
    playerPositionZ: 0x24C, gameplayTimer: 0x250, runDistance: 0x254, walkDistance: 0x258, bits25C: 0x25C,
    meleeKillCount: 0x25D, meleeKillCountB: 0x25E, rangedKillCount: 0x25F, word260: 0x260,
    firedShotCount: 0x264, closeRangeShotCount: 0x266, midRangeShotCount: 0x268, longRangeShotCount: 0x26A,
    field_26C: 0x26C, field_26E: 0x26E, field_270: 0x270, field_272: 0x272, field_274: 0x274,
    field_276: 0x276, field_278: 0x278, field_27A: 0x27A, continueCount: 0x27B,
  };
  // s_OptionsConfig (at CFG_OFS). Offsets from include/bodyprog/savegame.h.
  const OPTIONS = {
    controllerConfig: 0x00, screenPositionX: 0x1C, screenPositionY: 0x1D, soundType: 0x1E, volumeBgm: 0x1F,
    volumeSe: 0x20, vibrationEnabled: 0x21, brightness: 0x22, extraWeaponCtrl: 0x23, extraBloodColor: 0x24,
    autoLoad: 0x25, extraOptionsEnabled: 0x27, extraViewCtrl: 0x28, extraViewMode: 0x29,
    extraRetreatTurn: 0x2A, extraWalkRunCtrl: 0x2B, extraAutoAiming: 0x2C, extraBulletAdjust: 0x2D,
    seenGameOverTips: 0x2E, palLanguageId: 0x34,
  };

  const slotOffset = s => SLOT_OFS + s * SLOT_SIZE;
  const metaOffset = s => HDR_OFS + 4 + s * 12;
  const slotUsed = (blk, s) => (u32(blk, metaOffset(s)) | 0) !== 0;

  function sealRecord(blk, o, size) {
    const f = o + size - 4;
    blk[f] = blk[f + 1] = 0;
    blk[f + 2] = blk[f + 3] = 0xDC;
    let x = 0;
    for (let i = o; i < o + size; i++) x ^= blk[i];
    blk[f] = blk[f + 1] = x;
  }
  const sealHeader = blk => sealRecord(blk, HDR_OFS, 256);
  const sealOptions = blk => sealRecord(blk, CFG_OFS, 128);

  /** After editing a slot: copy the fields the save screen shows into the slot's
   *  header entry (as the game does when saving) and redo both checksums. */
  function sealSlot(blk, s) {
    const o = slotOffset(s), m = metaOffset(s);
    w32(blk, m + 4, u32(blk, o + SAVEGAME.gameplayTimer));
    w16(blk, m + 8, u16(blk, o + SAVEGAME.savegameCount));
    blk[m + 10] = blk[o + SAVEGAME.locationId];
    blk[m + 11] = blk[o + SAVEGAME.bits25C];
    sealRecord(blk, o, SLOT_SIZE);
    sealHeader(blk);
  }

  function clearSlot(blk, s) {
    blk.fill(0, metaOffset(s), metaOffset(s) + 12);
    blk.fill(0, slotOffset(s), slotOffset(s) + SLOT_SIZE);
    sealHeader(blk);
  }

  /** Copies a slot (data + header entry) over another. Same block allowed. */
  function copySlot(srcBlk, srcS, dstBlk, dstS) {
    const data = srcBlk.slice(slotOffset(srcS), slotOffset(srcS) + SLOT_SIZE);
    const meta = srcBlk.slice(metaOffset(srcS), metaOffset(srcS) + 12);
    dstBlk.set(data, slotOffset(dstS));
    dstBlk.set(meta, metaOffset(dstS));
    sealHeader(dstBlk);
  }

  function swapSlots(aBlk, aS, bBlk, bS) {
    const aData = aBlk.slice(slotOffset(aS), slotOffset(aS) + SLOT_SIZE);
    const aMeta = aBlk.slice(metaOffset(aS), metaOffset(aS) + 12);
    copySlot(bBlk, bS, aBlk, aS);
    bBlk.set(aData, slotOffset(bS));
    bBlk.set(aMeta, metaOffset(bS));
    sealHeader(aBlk);
    sealHeader(bBlk);
  }

  /** Highest total-save counter on the card: the game puts its cursor on the
   *  save holding it, so a duplicated save takes max + 1 to become "newest". */
  function maxTotalSaveCount(card) {
    let max = 0;
    for (const f of card.files) {
      if (!parseShName(f.name)) continue;
      for (let s = 0; s < SLOTS; s++) max = Math.max(max, u32(f.data, metaOffset(s)) | 0);
    }
    return max;
  }

  function setTotalSaveCount(blk, s, v) {
    w32(blk, metaOffset(s), v);
    sealHeader(blk);
  }

  /** A new, empty Silent Hill file. Options are copied from another file's block:
   *  an all-zero options record would load as muted, unbound controls. */
  function newShFile(region, index, optionsFrom) {
    const blk = new Uint8Array(BLOCK);
    normaliseTitleBlock(blk, region, index);
    sealHeader(blk);
    blk.set(optionsFrom.subarray(CFG_OFS, CFG_OFS + 128), CFG_OFS);
    return blk;
  }

  function getFlag(blk, s, idx) {
    const o = slotOffset(s) + SAVEGAME.eventFlags + (idx >> 5) * 4;
    return (u32(blk, o) >>> (idx & 31)) & 1;
  }
  function setFlag(blk, s, idx, on) {
    const o = slotOffset(s) + SAVEGAME.eventFlags + (idx >> 5) * 4;
    let v = u32(blk, o);
    v = on ? (v | (1 << (idx & 31))) : (v & ~(1 << (idx & 31)));
    w32(blk, o, v >>> 0);
  }

  function titleBytes(region, index) {
    const b = [];
    if (region === 'jpn') {
      b.push(0x83, 0x54, 0x83, 0x43, 0x83, 0x8C, 0x83, 0x93, 0x83, 0x67, 0x83, 0x71, 0x83, 0x8B,
        0x81, 0x40, 0x83, 0x74, 0x83, 0x40, 0x83, 0x43, 0x83, 0x8B);
    } else {
      for (const c of 'SILENT HILL  FILE') {
        if (c === ' ') b.push(0x81, 0x40); else b.push(0x82, 0x60 + c.charCodeAt(0) - 65);
      }
    }
    const n = index + 1;
    b.push(0x82, 0x4F + Math.floor(n / 10), 0x82, 0x4F + n % 10);
    return b;
  }

  const iconBlank = blk => blk.subarray(0x60, 0x100).every(v => v === 0);

  /** Console-correct title (Shift-JIS) for region/index; restores a blank icon.
   *  Older PC builds wrote the title as UTF-8 and skipped the icon. */
  function normaliseTitleBlock(blk, region, index) {
    blk[0] = 0x53; blk[1] = 0x43; blk[2] = 0x11; blk[3] = 1;
    blk.fill(0, 4, 0x60);
    blk.set(titleBytes(region, index), 4);
    if (iconBlank(blk)) blk.set(ICON, 0x60);
  }

  /** Title as text, or null when it is not valid Shift-JIS (PC-port files: the
   *  port writes the title as UTF-8, then patches the digits byte-wise). */
  function decodeTitle(blk) {
    if (blk[0] !== 0x53 || blk[1] !== 0x43) return null;
    let end = 4;
    while (end < 0x44 && blk[end]) end++;
    try {
      return new TextDecoder('shift_jis', { fatal: true }).decode(blk.subarray(4, end)).normalize('NFKC');
    } catch (e) {
      return null;
    }
  }

  /** 16x16 RGBA pixels of a save's first icon frame (blank PC icons use the disc icon). */
  function iconRgba(blk) {
    const src = iconBlank(blk) ? ICON : blk.subarray(0x60, 0x100);
    const pal = [];
    for (let i = 0; i < 16; i++) {
      const c = u16(src, i * 2);
      pal.push([(c & 31) * 255 / 31, ((c >> 5) & 31) * 255 / 31, ((c >> 10) & 31) * 255 / 31, c === 0 ? 0 : 255]);
    }
    const px = new Uint8ClampedArray(16 * 16 * 4);
    for (let i = 0; i < 128; i++) {
      const v = src[32 + i];
      px.set(pal[v & 15], i * 8);
      px.set(pal[v >> 4], i * 8 + 4);
    }
    return px;
  }

  /** Collects the Silent Hill files of `sources` into a fresh card for `target`
   *  region. Other games are left out; a FILE number that is already taken moves
   *  to the next free one. Returns {card, log}. */
  function gather(sources, labels, target, normalise) {
    const log = [];
    const taken = new Array(FILES_MAX).fill(false);
    const picked = [];
    sources.forEach((src, c) => {
      src.warnings.forEach(w => log.push({ level: 'warn', text: `${labels[c]}: ${w}` }));
      let other = 0;
      for (const f of src.files) {
        const sh = parseShName(f.name);
        if (!sh || blocksOf(f) !== 1) { other++; continue; }
        let dst = sh.index;
        if (taken[dst]) {
          dst = taken.indexOf(false);
          if (dst < 0) { log.push({ level: 'warn', text: `${f.name} skipped: all 15 Silent Hill file numbers are already used on the output card.` }); continue; }
        }
        taken[dst] = true;
        const blk = f.data.slice();
        if (normalise || dst !== sh.index || sh.region !== target) normaliseTitleBlock(blk, target, dst);
        picked.push({ dst, blk });
        let note = '';
        if (sh.region !== target) note += ` (${REGIONS[sh.region].label} save renamed for ${REGIONS[target].label})`;
        if (dst !== sh.index) note += ` (FILE${pad(sh.index + 1)} renumbered to FILE${pad(dst + 1)})`;
        log.push({ level: 'info', text: `${labels[c]}: ${f.name} → FILE${pad(dst + 1)}${note}` });
        if (!headerValid(blk)) log.push({ level: 'warn', text: `${f.name}: save header checksum is bad; the game will call this file damaged.` });
      }
      if (other) log.push({ level: 'info', text: `${labels[c]}: ${other} file(s) from other games left out` });
    });
    picked.sort((a, b) => a.dst - b.dst);
    const card = { files: [], warnings: [] };
    for (const p of picked) put(card, makeName(target, p.dst), p.blk);
    return { card, log };
  }

  /** Adds the Silent Hill files of `adds` to a copy of `base`, keeping every save
   *  already on it; colliding FILE numbers are renumbered, never overwritten. */
  function merge(base, baseLabel, adds, addLabels, target, normalise) {
    const sh = gather([base].concat(adds), [baseLabel].concat(addLabels), target, normalise);
    const card = { files: [], warnings: [] };
    const log = sh.log.slice();
    let kept = 0;
    for (const f of base.files) {
      if (parseShName(f.name) && blocksOf(f) === 1) continue;
      card.files.push({ name: f.name, data: f.data });
      kept++;
    }
    if (kept) log.push({ level: 'info', text: `${baseLabel}: kept ${kept} file(s) from other games` });
    for (const f of sh.card.files)
      if (!put(card, f.name, f.data)) log.push({ level: 'warn', text: `No room for ${f.name}: the card is full (15 blocks).` });
    return { card, log };
  }

  /** Region of the first Silent Hill file on the card, or null when it has none. */
  function cardRegion(card) {
    for (const f of card.files) { const sh = parseShName(f.name); if (sh) return sh.region; }
    return null;
  }

  function isPcCardFileName(fileName) {
    const m = /^(\d+)\.mcd$/i.exec(fileName.split(/[\\/]/).pop());
    return !!m && String(+m[1]) === m[1] && PC_CARD_NUMBERS.includes(+m[1]);
  }
  const pcNumber = fileName => parseInt(fileName.split(/[\\/]/).pop(), 10);

  function pcCardLabel(n) {
    const port = n >= 8 ? 2 : 1;
    return (n & 3) === 0 ? `Memory Card ${port}` : `Memory Card ${port}-${'ABCD'[n & 3]} (multitap)`;
  }

  function psxFileNameFor(pcNum, region) {
    let slot = pcNum >= 8 ? '2' : '1';
    if (pcNum & 3) slot += 'ABCD'[pcNum & 3];
    return `${REGIONS[region].game}_${slot}.mcd`;
  }

  function pad(n) { return String(n).padStart(2, '0'); }
  function formatTime(secs) {
    return `${Math.floor(secs / 3600)}:${pad(Math.floor(secs / 60) % 60)}:${pad(secs % 60)}`;
  }

  // Minimal store-only ZIP writer.
  const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    return t;
  })();
  function crc32(d) { let c = 0xFFFFFFFF; for (let i = 0; i < d.length; i++) c = CRC_TABLE[(c ^ d[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }

  function zip(entries) {
    const enc = new TextEncoder();
    const parts = [], central = [];
    let ofs = 0;
    for (const e of entries) {
      const name = enc.encode(e.name), crc = crc32(e.data), size = e.data.length;
      const lh = new Uint8Array(30 + name.length);
      w32(lh, 0, 0x04034B50); w16(lh, 4, 20); w16(lh, 6, 0x0800); w16(lh, 8, 0);
      w16(lh, 10, 0); w16(lh, 12, 0x21); w32(lh, 14, crc); w32(lh, 18, size); w32(lh, 22, size);
      w16(lh, 26, name.length); lh.set(name, 30);
      const ch = new Uint8Array(46 + name.length);
      w32(ch, 0, 0x02014B50); w16(ch, 4, 20); w16(ch, 6, 20); w16(ch, 8, 0x0800); w16(ch, 10, 0);
      w16(ch, 12, 0); w16(ch, 14, 0x21); w32(ch, 16, crc); w32(ch, 20, size); w32(ch, 24, size);
      w16(ch, 28, name.length); w32(ch, 42, ofs); ch.set(name, 46);
      parts.push(lh, e.data); central.push(ch);
      ofs += lh.length + size;
    }
    const cdSize = central.reduce((n, c) => n + c.length, 0);
    const end = new Uint8Array(22);
    w32(end, 0, 0x06054B50); w16(end, 8, entries.length); w16(end, 10, entries.length);
    w32(end, 12, cdSize); w32(end, 16, ofs);
    const out = new Uint8Array(ofs + cdSize + 22);
    let p = 0;
    for (const x of parts.concat(central, [end])) { out.set(x, p); p += x.length; }
    return out;
  }

  const api = {
    BLOCK, CARD, DIRS, REGIONS, PC_CARD_NUMBERS, PC_OUTPUT_ORDER,
    loadCard, toImage, put, freeBlocks, blocksOf, parseShName, makeName, readSlots, headerValid,
    normaliseTitleBlock, decodeTitle, iconRgba, iconBlank, gather, merge, cardRegion,
    isPcCardFileName, pcNumber, pcCardLabel, psxFileNameFor, formatTime, zip, crc32,
    SLOTS, FILES_MAX, SAVEGAME, OPTIONS, HDR_OFS, CFG_OFS, SLOT_OFS, SLOT_SIZE, LOCATIONS,
    slotOffset, metaOffset, slotUsed, sealSlot, sealHeader, sealOptions, clearSlot, copySlot, swapSlots,
    maxTotalSaveCount, setTotalSaveCount, newShFile, getFlag, setFlag, u16, u32, w16, w32,
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.SHCard = api;
})(typeof self !== 'undefined' ? self : this);
