// SPDX-License-Identifier: GPL-3.0-or-later
//
// Save editor panel for sh_savecard.html. Edits one save slot in place in the
// loaded card's file data; every change re-seals the slot (header entry +
// checksums) exactly as the game does when it saves.
(function (root) {
  'use strict';
  const S = root.SHCard, D = root.SHCardData;
  const SG = S.SAVEGAME, OP = S.OPTIONS;
  const FLAG_COUNT = SG.eventFlagWords * 32;

  let panel, hooks, ctx = null, tab = 'overview';
  const flagView = { mode: 'map', q: '', show: 'all', kind: '', map: '', named: false, compare: '' };

  // ---- small DOM helpers ----
  function h(tag, attrs, ...kids) {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === undefined || v === null || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
      else if (k === 'value') el.value = v;
      else if (k === 'checked') el.checked = !!v;
      else el.setAttribute(k, v === true ? '' : v);
    }
    for (const kid of kids.flat()) if (kid !== null && kid !== undefined && kid !== false) el.append(kid instanceof Node ? kid : String(kid));
    return el;
  }
  const pad2 = n => String(n).padStart(2, '0');
  let uid = 0;
  const nextId = () => 'ed' + (++uid);

  // ---- data access for the open slot ----
  const blk = () => ctx.entry.card.files[ctx.fi].data;
  const dv = () => new DataView(blk().buffer, blk().byteOffset + S.slotOffset(ctx.s), S.SLOT_SIZE);
  const rd = {
    u8: o => dv().getUint8(o), s8: o => dv().getInt8(o), u16: o => dv().getUint16(o, true),
    s16: o => dv().getInt16(o, true), u32: o => dv().getUint32(o, true), s32: o => dv().getInt32(o, true),
  };
  const wr = {
    u8: (o, v) => dv().setUint8(o, v), s8: (o, v) => dv().setInt8(o, v), u16: (o, v) => dv().setUint16(o, v, true),
    s16: (o, v) => dv().setInt16(o, v, true), u32: (o, v) => dv().setUint32(o, v >>> 0, true), s32: (o, v) => dv().setInt32(o, v, true),
  };
  const RANGE = { u8: [0, 255], s8: [-128, 127], u16: [0, 65535], s16: [-32768, 32767], u32: [0, 4294967295], s32: [-2147483648, 2147483647] };

  function commit() {
    S.sealSlot(blk(), ctx.s);
    hooks.onChange(ctx.entry);
    renderHead();
  }

  // ---- field builders ----
  function field(label, hint, control, wide) {
    const id = control.id || (control.id = nextId());
    return h('div', { class: 'fld' + (wide ? ' wide' : ''), title: hint || null },
      h('label', { for: id }, label, hint ? h('span', { class: 'hint-dot', 'aria-hidden': 'true' }, '?') : null), control);
  }

  /** Numeric input. `scale` divides the stored integer for display (4096 for q12). */
  function num(label, hint, type, ofs, opts = {}) {
    const scale = opts.scale || 1;
    const [lo, hi] = RANGE[type];
    const input = h('input', {
      type: 'number', step: opts.step || (scale === 1 ? 1 : 0.001),
      min: opts.min !== undefined ? opts.min : lo / scale, max: opts.max !== undefined ? opts.max : hi / scale,
      value: +(rd[type](ofs) / scale).toFixed(scale === 1 ? 0 : 3),
      onchange: e => {
        let v = Math.round(parseFloat(e.target.value) * scale);
        if (!Number.isFinite(v)) return;
        v = Math.min(hi, Math.max(lo, v));
        wr[type](ofs, v);
        commit();
      },
    });
    return field(label, hint, input);
  }

  function select(label, hint, options, get, set) {
    const cur = get();
    const sel = h('select', { onchange: e => { set(parseInt(e.target.value, 10)); commit(); } });
    let found = false;
    for (const [v, text] of options) {
      sel.append(h('option', { value: v }, text));
      if (v === cur) found = true;
    }
    if (!found) sel.append(h('option', { value: cur }, `Unknown (${cur})`));
    sel.value = cur;
    return field(label, hint, sel);
  }

  function check(label, hint, get, set) {
    const id = nextId();
    return h('label', { class: 'chk', for: id, title: hint || null },
      h('input', { type: 'checkbox', id, checked: get(), onchange: e => { set(e.target.checked); commit(); } }), label);
  }

  function bitChecks(type, ofs, bits) {
    return h('div', { class: 'chks' }, bits.map(([name, mask, hint]) =>
      check(name, hint, () => (rd[type](ofs) & mask) !== 0, on => wr[type](ofs, on ? (rd[type](ofs) | mask) : (rd[type](ofs) & ~mask)))));
  }

  const mapOptions = () => D.maps.map(m => [m.idx, `${m.id}${m.desc ? ' · ' + m.desc : ''}`]);
  const itemOptions = (withNone) => [[withNone ? 0 : 255, withNone ? 'None' : 'Empty'], ...Object.entries(D.items).map(([id, n]) => [+id, `${n} (${id})`])];

  // ---- panel frame ----
  function slotSummary(entry, fi, s) {
    const f = entry.card.files[fi];
    const sh = S.parseShName(f.name);
    const slot = S.readSlots(f.data).find(x => x.index === s);
    return { f, sh, slot, label: `FILE${pad2(sh.index + 1)} · slot ${s + 1}` + (slot ? ` · save #${slot.saveCount} · ${slot.location}` : ' · empty') };
  }

  function renderHead() {
    const head = panel.querySelector('.ed-head');
    const { slot, label } = slotSummary(ctx.entry, ctx.fi, ctx.s);
    head.innerHTML = '';
    head.append(
      h('div', null, h('h3', null, label), h('div', { class: 'hint' }, `${ctx.entry.fileName}${slot ? ' · ' + S.formatTime(slot.seconds) + (slot.valid ? '' : ' · checksum bad') : ''}`)),
      h('div', { class: 'row' },
        moveControls(),
        ctx.entry.modified ? h('button', { type: 'button', class: 'primary', onclick: e => hooks.download(ctx.entry, e.target) }, 'Download edited card') : null,
        h('button', { type: 'button', onclick: close }, 'Close')));
  }

  function moveControls() {
    // Copy or move this save to an empty slot, on this card's files or a new file.
    const card = ctx.entry.card;
    const opts = [];
    card.files.forEach((f, fi) => {
      const sh = S.parseShName(f.name);
      if (!sh) return;
      for (let s = 0; s < S.SLOTS; s++)
        if (!S.slotUsed(f.data, s)) opts.push([`${fi}:${s}`, `FILE${pad2(sh.index + 1)} slot ${s + 1}`]);
    });
    const used = new Set(card.files.map(f => S.parseShName(f.name)).filter(Boolean).map(sh => sh.index));
    const region = S.parseShName(card.files[ctx.fi].name).region;
    if (S.freeBlocks(card) > 0) {
      for (let i = 0; i < S.FILES_MAX; i++) if (!used.has(i)) { opts.push([`new:${i}`, `New FILE${pad2(i + 1)} (uses 1 free block)`]); break; }
    }
    const sel = h('select', { 'aria-label': 'Target slot' }, opts.length ? opts.map(([v, t]) => h('option', { value: v }, t)) : h('option', { value: '' }, 'No empty slot'));
    const go = (move) => {
      const v = sel.value;
      if (!v) return;
      let fi, s;
      if (v.startsWith('new:')) {
        const idx = +v.slice(4);
        const data = S.newShFile(region, idx, card.files[ctx.fi].data);
        S.put(card, S.makeName(region, idx), data);
        fi = card.files.length - 1; s = 0;
      } else {
        [fi, s] = v.split(':').map(Number);
      }
      const src = blk();
      S.copySlot(src, ctx.s, card.files[fi].data, s);
      if (move) S.clearSlot(src, ctx.s);
      else S.setTotalSaveCount(card.files[fi].data, s, S.maxTotalSaveCount(card) + 1);
      ctx.fi = fi; ctx.s = s;
      hooks.onChange(ctx.entry);
      render();
    };
    return h('div', { class: 'row', title: 'Copy gives the copy the newest-save marker, so the save screen opens on it.' },
      sel,
      h('button', { type: 'button', onclick: () => go(false), disabled: !opts.length }, 'Copy to'),
      h('button', { type: 'button', onclick: () => go(true), disabled: !opts.length }, 'Move to'));
  }

  const TABS = [['overview', 'Overview'], ['inventory', 'Inventory'], ['flags', 'Event flags'], ['world', 'Maps & enemies'], ['stats', 'Stats'], ['options', 'File options']];

  function render() {
    panel.innerHTML = '';
    panel.hidden = false;
    const tabs = h('div', { class: 'tabs', role: 'tablist' }, TABS.map(([k, t]) =>
      h('button', { type: 'button', role: 'tab', 'aria-selected': String(tab === k), class: tab === k ? 'on' : null, onclick: () => { tab = k; render(); } }, t)));
    const body = h('div', { class: 'ed-body' });
    panel.append(h('div', { class: 'ed-head' }), tabs, body);
    renderHead();
    ({ overview, inventory, flags, world, stats, options })[tab](body);
  }

  // ---- tabs ----
  function overview(body) {
    const locs = Object.entries(D.locations).map(([v, n]) => [+v, n]);
    const bits = () => rd.u8(SG.bits25C);
    const setBits = (mask, shift, v) => wr.u8(SG.bits25C, (bits() & ~mask) | ((v << shift) & mask));
    const secs = () => Math.floor(rd.u32(SG.gameplayTimer) / 4096) + ((bits() >> 1) & 3) * 290 * 3600;
    const timeInput = h('input', {
      type: 'text', value: S.formatTime(secs()), pattern: '\\d+:\\d{1,2}:\\d{1,2}', inputmode: 'numeric',
      onchange: e => {
        const m = /^(\d+):(\d{1,2}):(\d{1,2})$/.exec(e.target.value.trim());
        if (!m) { e.target.value = S.formatTime(secs()); return; }
        let total = +m[1] * 3600 + +m[2] * 60 + +m[3];
        // The game counts 290-hour wraps in 2 bits and caps the last stretch at 130 h.
        const add = Math.min(3, Math.floor(total / (290 * 3600)));
        let rest = total - add * 290 * 3600;
        if (add === 3) rest = Math.min(rest, 130 * 3600);
        setBits(0x06, 1, add);
        wr.u32(SG.gameplayTimer, rest * 4096);
        commit();
      },
    });
    const diffGet = () => { const v = rd.u32(SG.word260) >>> 28; return v >= 8 ? v - 16 : v; };
    const meta = () => S.u32(blk(), S.metaOffset(ctx.s)) | 0;
    body.append(
      h('p', { class: 'note' }, 'Changing the map without a matching room and position can drop Harry outside the level. Copy the three values from a save made in the place you want.'),
      h('div', { class: 'grid' },
        select('Save list label', 'The place name the save screen shows. The game takes it from a fixed list, so it cannot be free text.', locs, () => rd.s8(SG.locationId), v => wr.s8(SG.locationId, v)),
        select('Map', 'Map overlay loaded on continue (MapIdx).', mapOptions(), () => rd.s8(SG.mapIdx), v => wr.s8(SG.mapIdx, v)),
        num('Room', 'Room index inside the map (mapRoomIdx).', 's8', SG.mapRoomIdx),
        num('Position X', 'World units (1.0 = 4096 raw).', 's32', SG.playerPositionX, { scale: 4096 }),
        num('Position Z', 'World units (1.0 = 4096 raw).', 's32', SG.playerPositionZ, { scale: 4096 }),
        field('Facing (degrees)', 'Clockwise from +Z.', h('input', {
          type: 'number', step: 0.1, min: 0, max: 360, value: +((rd.u16(SG.playerRotationY) & 0xFFF) / 4096 * 360).toFixed(1),
          onchange: e => { const d = parseFloat(e.target.value); if (!Number.isFinite(d)) return; wr.u16(SG.playerRotationY, Math.round(((d % 360) + 360) % 360 / 360 * 4096) & 0xFFF); commit(); },
        })),
        num('Health', 'Default 100.', 's32', SG.playerHealth, { scale: 4096, min: 0, max: 100 }),
        num('Health reserve', 'Ampoules store up to 300; it slowly refills health.', 's32', SG.healthSaturation, { scale: 4096, min: 0, max: 300 }),
        select('Difficulty', null, Object.entries(D.difficulty).map(([v, n]) => [+v, n]), diffGet,
          v => wr.u32(SG.word260, ((rd.u32(SG.word260) & 0x0FFFFFFF) | ((v & 0xF) << 28)) >>> 0)),
        field('Play time (h:mm:ss)', 'Shown on the save screen and used for the ending rank.', timeInput),
        num('Save count', 'The "save #" on the save screen; counts saves in this playthrough.', 's16', SG.savegameCount),
        field('Newest-save counter', 'Counts every save across playthroughs. The save screen opens on the highest one. 0 would delete the slot, so the minimum is 1.', h('input', {
          type: 'number', min: 1, max: 2147483647, value: meta(),
          onchange: e => { const v = parseInt(e.target.value, 10); if (v >= 1) { S.setTotalSaveCount(blk(), ctx.s, v); hooks.onChange(ctx.entry); renderHead(); } },
        })),
        num('Continues', null, 'u8', SG.continueCount),
        num('Games cleared', 'Range 0 to 99.', 'u8', SG.clearGameCount, { max: 99 }),
        field('Special items picked up', 'Rock Drill, Chainsaw, Katana, Hyper Blaster, Gasoline Tank and Channeling Stone each add one. Counts toward the ending rank. Range 0 to 7.', h('input', {
          type: 'number', min: 0, max: 7, value: (bits() >> 3) & 7,
          onchange: e => { const v = parseInt(e.target.value, 10); if (v >= 0 && v <= 7) { setBits(0x38, 3, v); commit(); } },
        })),
        select('Hyper Blaster colour', 'Beam colour and damage. The results screen awards yellow for an 80+ rank and green for 100. On USA and European discs the results screen also miscounts yellow as one extra special item (a game bug fixed in later Japanese releases).',
          [[0, 'Red (default)'], [1, 'Yellow (3\u00d7 damage)'], [2, 'Green (about 5\u00d7 damage)'], [3, 'Unused value 3 (no colour or damage coded)']],
          () => bits() >> 6, v => setBits(0xC0, 6, v)),
      ),
      h('h4', null, 'Flags'),
      h('div', { class: 'chks' },
        check('Next Fear', 'Makes the save gold and starts Next Fear mode.', () => (bits() & 1) !== 0, on => setBits(0x01, 0, on ? 1 : 0))),
      h('h4', null, 'Endings seen'),
      bitChecks('u8', SG.clearGameEndings, D.endings.map(([n, m]) => [n, m])),
    );
  }

  function inventory(body) {
    const toggles = D.itemToggles.map(([n, m]) => [n, m]);
    const invFlags = D.inventoryItemFlags.map(([n, m]) => [n, m]);
    const rows = [];
    for (let i = 0; i < SG.itemCount; i++) {
      const o = SG.items + i * 4;
      const itemSel = h('select', { 'aria-label': `Item ${i + 1}`, onchange: e => { wr.u8(o, +e.target.value); commit(); } },
        itemOptions(false).map(([v, t]) => h('option', { value: v }, t)));
      if (![...itemSel.options].some(op => +op.value === rd.u8(o))) itemSel.append(h('option', { value: rd.u8(o) }, `Unknown (${rd.u8(o)})`));
      itemSel.value = rd.u8(o);
      const cmdSel = h('select', { 'aria-label': `Command ${i + 1}`, onchange: e => { wr.u8(o + 2, +e.target.value); commit(); } },
        Object.entries(D.invCommands).map(([v, t]) => h('option', { value: v }, t)));
      if (![...cmdSel.options].some(op => +op.value === rd.u8(o + 2))) cmdSel.append(h('option', { value: rd.u8(o + 2) }, `Unknown (${rd.u8(o + 2)})`));
      cmdSel.value = rd.u8(o + 2);
      const numIn = (off, label) => h('input', { type: 'number', min: 0, max: 255, value: rd.u8(off), 'aria-label': label, class: 'narrow',
        onchange: e => { const v = parseInt(e.target.value, 10); if (v >= 0 && v <= 255) { wr.u8(off, v); commit(); } } });
      rows.push(h('tr', { class: rd.u8(o) === 255 ? 'dim' : null },
        h('td', null, i + 1), h('td', null, itemSel), h('td', null, numIn(o + 1, `Count ${i + 1}`)), h('td', null, cmdSel), h('td', null, numIn(o + 3, `Extra ${i + 1}`))));
    }
    body.append(
      h('div', { class: 'grid' },
        num('Inventory slots', 'How many slots the inventory screen shows. Items past this are hidden.', 'u8', SG.inventorySlotCount, { max: 40 }),
        select('Equipped weapon', 'Item ID shown in Harry\'s hand.', itemOptions(true), () => rd.u8(SG.equippedWeapon), v => wr.u8(SG.equippedWeapon, v)),
        num('Items picked up', 'Counter used by the ending rank.', 's16', SG.pickedUpItemCount)),
      h('h4', null, 'Toggles'), bitChecks('u32', SG.itemToggleFlags, toggles),
      h('h4', { title: 'Set once the item has been used where it belongs.' }, 'Key item states'), bitChecks('u8', SG.inventoryItemFlags, invFlags),
      h('h4', null, 'Items'),
      h('div', { class: 'scroll' }, h('table', { class: 'inv' },
        h('thead', null, h('tr', null, h('th', null, '#'), h('th', null, 'Item'), h('th', { title: 'Stack size, ammo count or charges.' }, 'Count'), h('th', { title: 'Menu command for this item.' }, 'Command'), h('th', { title: 'field_3 in the decomp; purpose unknown, keep as is.' }, 'Extra'))),
        h('tbody', null, rows))));
  }

  // ---- event flags ----
  function flagInfo(i) {
    const f = D.flags[i];
    if (!f) return { name: '', label: `Flag ${i}`, tip: `Flag ${i}\nNot listed in the decomp's flag table.`, maps: [], named: false };
    const [name, group, note, maps, funcs] = f;
    const named = !/^\d+$/.test(name);
    const label = named ? name.replace(/_/g, ' ') : 'unnamed';
    const mapText = maps.map(id => { const m = D.maps.find(x => x.id === id); return m && m.desc ? `${id} (${m.desc})` : id; });
    const tip = [`Flag ${i}${named ? ': ' + name : ''}`, group, note,
      maps.length ? 'Used in: ' + mapText.join(', ') : 'Not referenced by decompiled code yet.',
      funcs.length ? 'Functions: ' + funcs.join(', ') : ''].filter(Boolean).join('\n');
    return { name, label, tip, maps, named };
  }

  function compareTargets() {
    const out = [];
    for (const entry of hooks.entries()) {
      entry.card.files.forEach((f, fi) => {
        const sh = S.parseShName(f.name);
        if (!sh) return;
        for (const sl of S.readSlots(f.data)) {
          if (entry === ctx.entry && fi === ctx.fi && sl.index === ctx.s) continue;
          out.push({ key: `${entry.id}:${fi}:${sl.index}`, entry, fi, s: sl.index,
            label: `${entry.fileName} · FILE${pad2(sh.index + 1)} slot ${sl.index + 1} · #${sl.saveCount} ${sl.location}` });
        }
      });
    }
    return out;
  }

  const PICKUP_RE = /Pickup|Bullets|Shells|HealthDrink|FirstAid|Ampoule|Map\d*$/;
  function flagKind(f) {
    if (!f.named) return 'unnamed';
    if (f.name.startsWith('MapMark_')) return 'marking';
    if (PICKUP_RE.test(f.name)) return 'pickup';
    return 'event';
  }
  function prettyFlag(f) {
    if (!f.named) return `Unnamed flag ${f.i}`;
    return f.name.replace(/^MapMark_/, '').replace(/^M\dS\d\d_/, '').replace(/^Pickup/, '')
      .split('_').map(part => part.replace(/(?<=[a-z])(?=[A-Z])|(?<=[0-9])(?=[A-Z][a-z])/g, ' ').replace(/(?<=[a-z])(?=\d)/g, ' ')).join(' \u00b7 ');
  }
  /** Map a flag belongs to: its name prefix (M1S01_...) or else the maps whose code uses it. */
  function flagMaps(f) {
    const m = /^M(\d)S(\d\d)_/.exec(f.name);
    const out = new Set(f.maps);
    if (m) out.add(`MAP${m[1]}_S${m[2]}`);
    return [...out];
  }

  let allInfos = null;
  function infosAll() {
    if (!allInfos) {
      allInfos = [];
      for (let i = 1; i < FLAG_COUNT; i++) {
        const f = Object.assign({ i }, flagInfo(i));
        f.kind = flagKind(f);
        f.pretty = prettyFlag(f);
        f.mapIds = flagMaps(f);
        allInfos.push(f);
      }
    }
    return allInfos;
  }

  /** One flag as an on/off switch. `after` runs once the flag has changed. */
  function flagSwitch(f, cmpValue, after) {
    const on = !!S.getFlag(blk(), ctx.s, f.i);
    const diff = cmpValue !== null && !!cmpValue !== on;
    const btn = h('button', {
      type: 'button', role: 'switch', 'aria-checked': String(on),
      class: 'fsw' + (on ? ' on' : '') + (diff ? ' diff' : '') + (f.named ? '' : ' unnamed'),
      title: f.tip + (cmpValue !== null ? `\nCompared save: ${cmpValue ? 'on' : 'off'}` : '') + '\nClick to turn ' + (on ? 'off.' : 'on.'),
      onclick: () => {
        const now = !S.getFlag(blk(), ctx.s, f.i);
        S.setFlag(blk(), ctx.s, f.i, now);
        commit();
        btn.classList.toggle('on', now);
        btn.setAttribute('aria-checked', String(now));
        btn.querySelector('.pill').textContent = now ? 'ON' : 'OFF';
        after && after();
      },
    }, h('span', { class: 'pill' }, on ? 'ON' : 'OFF'), h('span', { class: 'fn' }, f.pretty), h('span', { class: 'fi' }, '#' + f.i));
    return btn;
  }

  function flags(body) {
    const infos = infosAll();
    const targets = compareTargets();
    const cmp = targets.find(t => t.key === flagView.compare);
    const other = i => cmp ? S.getFlag(cmp.entry.card.files[cmp.fi].data, cmp.s, i) : null;
    const isOn = f => !!S.getFlag(blk(), ctx.s, f.i);

    const cmpSel = h('select', { 'aria-label': 'Compare with', onchange: e => { flagView.compare = e.target.value; render(); } },
      h('option', { value: '' }, 'Compare with another save\u2026'), targets.map(t => h('option', { value: t.key }, t.label)));
    cmpSel.value = cmp ? flagView.compare : '';
    const modeBtn = (mode, text) => h('button', { type: 'button', class: flagView.mode === mode ? 'on' : null, 'aria-pressed': String(flagView.mode === mode),
      onclick: () => { flagView.mode = mode; render(); } }, text);

    body.append(
      h('div', { class: 'row' }, h('div', { class: 'seg' }, modeBtn('map', 'By map'), modeBtn('all', 'All flags')), cmpSel),
      h('p', { class: 'note' }, 'Every switch below is one flag stored in this save; click it to turn it on or off. The change is written to this slot straight away (the card is marked "edited"); download the card when you are done. Hover a flag for everything the decomp knows about it.'));

    const content = h('div', { class: 'ed-body' });
    body.append(content);
    if (flagView.mode === 'all') allFlagsView(content, infos, other, isOn);
    else mapFlagsView(content, infos, other, isOn);
  }

  function mapFlagsView(body, infos, other, isOn) {
    const withFlags = D.maps.filter(m => infos.some(f => f.mapIds.includes(m.id)));
    if (!flagView.map || !withFlags.some(m => m.id === flagView.map)) {
      const cur = D.maps.find(m => m.idx === rd.s8(SG.mapIdx));
      flagView.map = cur && withFlags.includes(cur) ? cur.id : withFlags[0].id;
    }
    const mapSel = h('select', { 'aria-label': 'Map', onchange: e => { flagView.map = e.target.value; render(); } },
      withFlags.map(m => h('option', { value: m.id }, `${m.id} \u00b7 ${m.desc}`)));
    mapSel.value = flagView.map;

    const progress = h('details', { class: 'progress' }, h('summary', null, 'Progress on every map'),
      h('div', { class: 'scroll mapsum' }, h('table', null,
        h('thead', null, h('tr', null, h('th', null, 'Map'), h('th', null, ''), h('th', null, 'Pickups taken'), h('th', null, 'Map notes'), h('th', null, 'Events'))),
        h('tbody', null, withFlags.map(m => {
          const mine = infos.filter(f => f.mapIds.includes(m.id));
          const frac = kind => { const k = mine.filter(f => f.kind === kind); return k.length ? `${k.filter(isOn).length} / ${k.length}` : ''; };
          return h('tr', { class: m.id === flagView.map ? 'sel' : null, onclick: () => { flagView.map = m.id; render(); } },
            h('td', { class: 'mono' }, m.id), h('td', null, m.desc), h('td', null, frac('pickup')), h('td', null, frac('marking')), h('td', null, frac('event')));
        })))));

    body.append(h('div', { class: 'row' }, h('label', { for: mapSel.id = nextId() }, 'Map'), mapSel), progress);

    const mine = infos.filter(f => f.mapIds.includes(flagView.map));
    const SECTIONS = [
      ['pickup', 'Item pickups', 'On = already picked up: the item is gone from the world. It does not add the item to the inventory; use the Inventory tab for that.', 'taken'],
      ['marking', 'Paper map notes', 'On = the note or arrow is drawn on the paper map.', 'drawn'],
      ['event', 'Story and events', 'Named event flags: doors opened, cutscenes seen, puzzle steps.', 'on'],
      ['unnamed', 'Unnamed flags used on this map', 'Real flags this map\'s code reads or writes, not yet named by the decomp. Hover for the functions that use them.', 'on'],
    ];
    for (const [kind, title, hint, word] of SECTIONS) {
      const list = mine.filter(f => f.kind === kind);
      if (!list.length) continue;
      const head = h('h4', { title: hint });
      const grid = h('div', { class: 'fgrid' });
      const update = () => { head.textContent = `${title} \u00b7 ${list.filter(isOn).length} of ${list.length} ${word}`; };
      const fill = () => { grid.innerHTML = ''; for (const f of list) grid.append(flagSwitch(f, other(f.i), update)); update(); };
      const setAll = on => { for (const f of list) S.setFlag(blk(), ctx.s, f.i, on); commit(); fill(); };
      body.append(h('div', { class: 'fsec' },
        h('div', { class: 'row' }, head,
          h('button', { type: 'button', class: 'small', onclick: () => setAll(true) }, 'All on'),
          h('button', { type: 'button', class: 'small', onclick: () => setAll(false) }, 'All off')),
        h('p', { class: 'hint' }, hint), grid));
      fill();
    }
  }

  function allFlagsView(body, infos, other, isOn) {
    const grid = h('div', { class: 'fgrid tall' });
    const count = h('span', { class: 'hint' });
    const bulkRow = h('div', { class: 'row' });
    let shown = [];
    let pending = null;
    function draw() {
      grid.innerHTML = '';
      const q = flagView.q.toLowerCase();
      shown = infos.filter(f => {
        const on = isOn(f);
        if (flagView.named && !f.named) return false;
        if (flagView.show === 'set' && !on) return false;
        if (flagView.show === 'unset' && on) return false;
        if (flagView.show === 'diff' && (other(f.i) === null || !!other(f.i) === on)) return false;
        if (flagView.kind && f.kind !== flagView.kind) return false;
        return !q || String(f.i) === q || f.pretty.toLowerCase().includes(q) || f.tip.toLowerCase().includes(q);
      });
      for (const f of shown) grid.append(flagSwitch(f, other(f.i), recount));
      recount();
      drawBulk();
    }
    function recount() { count.textContent = `${shown.length} listed \u00b7 ${infos.filter(isOn).length} of ${FLAG_COUNT - 1} on in this save`; }
    function drawBulk() {
      bulkRow.innerHTML = '';
      bulkRow.append(count);
      if (!shown.length) return;
      if (pending === null) {
        bulkRow.append(
          h('button', { type: 'button', class: 'small', onclick: () => { pending = true; drawBulk(); } }, `Turn all ${shown.length} listed on`),
          h('button', { type: 'button', class: 'small', onclick: () => { pending = false; drawBulk(); } }, `Turn all ${shown.length} listed off`));
      } else {
        bulkRow.append(
          h('button', { type: 'button', class: 'small danger', onclick: () => { for (const f of shown) S.setFlag(blk(), ctx.s, f.i, pending); pending = null; commit(); draw(); } },
            `Confirm: turn ${shown.length} flags ${pending ? 'on' : 'off'}`),
          h('button', { type: 'button', class: 'small', onclick: () => { pending = null; drawBulk(); } }, 'Cancel'));
      }
    }
    const search = h('input', { type: 'search', placeholder: 'Search name, number or map', value: flagView.q, 'aria-label': 'Search flags', oninput: e => { flagView.q = e.target.value; pending = null; draw(); } });
    const showSel = h('select', { 'aria-label': 'Show', onchange: e => { flagView.show = e.target.value; pending = null; draw(); } },
      [['all', 'On and off'], ['set', 'On only'], ['unset', 'Off only'], ['diff', 'Different from compared save']].map(([v, t]) => h('option', { value: v }, t)));
    showSel.value = flagView.show;
    const kindSel = h('select', { 'aria-label': 'Kind', onchange: e => { flagView.kind = e.target.value; pending = null; draw(); } },
      [['', 'Every kind'], ['pickup', 'Item pickups'], ['marking', 'Paper map notes'], ['event', 'Story and events'], ['unnamed', 'Unnamed']].map(([v, t]) => h('option', { value: v }, t)));
    kindSel.value = flagView.kind || '';
    body.append(
      h('div', { class: 'row filters' }, search, showSel, kindSel,
        h('label', { class: 'chk', for: 'flag-named' },
          h('input', { type: 'checkbox', id: 'flag-named', checked: flagView.named, onchange: e => { flagView.named = e.target.checked; pending = null; draw(); } }), 'Named only')),
      bulkRow, grid);
    draw();
  }

  function world(body) {
    const rows = D.maps.filter(m => m.idx < 45).map(m => {
      const o = SG.ovlEnemyStates + m.idx * 4;
      const input = h('input', { class: 'mono hex', value: rd.u32(o).toString(16).toUpperCase().padStart(8, '0'), 'aria-label': `${m.id} enemy mask`, maxlength: 8,
        onchange: e => { const v = parseInt(e.target.value, 16); if (Number.isFinite(v)) { wr.u32(o, v); commit(); } e.target.value = rd.u32(o).toString(16).toUpperCase().padStart(8, '0'); } });
      const dead = 32 - popcount(rd.u32(o));
      return h('tr', null, h('td', { class: 'mono' }, m.id), h('td', null, m.desc), h('td', null, input),
        h('td', { class: 'hint' }, dead ? `${dead} killed` : 'all alive'),
        h('td', null, h('button', { type: 'button', onclick: () => { wr.u32(o, 0xFFFFFFFF); commit(); render(); }, title: 'Respawns every enemy on this map.' }, 'Revive all')));
    });
    const paper = Object.entries(D.paperMaps).map(([v, n]) => [n, 1 << +v, `Paper map ${v}`]);
    body.append(
      h('div', { class: 'grid' },
        select('Map screen opens on', 'Paper map shown first when the map screen opens.', Object.entries(D.paperMaps).map(([v, n]) => [+v, n]), () => rd.u8(SG.paperMapIdx), v => wr.u8(SG.paperMapIdx, v))),
      h('h4', { title: 'One bit per paper map. Picking up a map sets every floor it covers.' }, 'Paper maps owned'),
      bitChecks('u32', SG.paperMapFlags, paper),
      h('h4', { title: 'One bit per enemy spawn on each map; a cleared bit means that enemy was killed and stays dead. New games start with every bit set.' }, 'Enemies alive, per map'),
      h('div', { class: 'scroll' }, h('table', null,
        h('thead', null, h('tr', null, h('th', null, 'Map'), h('th', null, ''), h('th', null, 'Mask (hex)'), h('th', null, ''), h('th', null, ''))),
        h('tbody', null, rows))));
  }
  function popcount(v) { let c = 0; while (v) { v &= v - 1; c++; } return c; }

  function stats(body) {
    body.append(
      h('div', { class: 'grid' },
        num('Walk distance (m)', 'Shown on the results screen.', 'u32', SG.walkDistance, { scale: 4096 }),
        num('Run distance (m)', null, 'u32', SG.runDistance, { scale: 4096 }),
        num('Melee kills', null, 'u8', SG.meleeKillCount),
        num('Melee kills B', 'Second melee counter; its exact use is unconfirmed.', 'u8', SG.meleeKillCountB),
        num('Ranged kills', null, 'u8', SG.rangedKillCount),
        num('Shots fired', 'Misses = fired minus the three hit counts.', 'u16', SG.firedShotCount),
        num('Close-range hits', null, 'u16', SG.closeRangeShotCount),
        num('Mid-range hits', null, 'u16', SG.midRangeShotCount),
        num('Long-range hits', null, 'u16', SG.longRangeShotCount)),
      h('h4', null, 'Unidentified fields'),
      h('p', { class: 'note' }, 'Names from the decomp. Their meaning is not known yet; change them only to experiment.'),
      h('div', { class: 'grid' },
        num('field_A0', null, 's8', SG.field_A0),
        num('field_26C', null, 'u16', SG.field_26C), num('field_26E', 'Related to enemy kills.', 'u16', SG.field_26E),
        num('field_270', null, 'u16', SG.field_270), num('field_272', null, 'u16', SG.field_272),
        num('field_274', null, 'u16', SG.field_274), num('field_276', null, 'u16', SG.field_276),
        num('field_278', null, 'u16', SG.field_278), num('field_27A', 'Flags.', 's8', SG.field_27A),
        field('field_260 (28 bits)', 'Low 28 bits of the word that also holds difficulty.', h('input', {
          type: 'number', min: 0, max: 0x0FFFFFFF, value: rd.u32(SG.word260) & 0x0FFFFFFF,
          onchange: e => { const v = parseInt(e.target.value, 10); if (v >= 0 && v <= 0x0FFFFFFF) { wr.u32(SG.word260, ((rd.u32(SG.word260) & 0xF0000000) | v) >>> 0); commit(); } },
        }))));
  }

  // Options belong to the whole file (all 11 slots), not the slot.
  function options(body) {
    const b = () => blk();
    const base = S.CFG_OFS;
    const commitOpt = () => { S.sealOptions(b()); hooks.onChange(ctx.entry); };
    const onum = (label, hint, key, lo, hi, signed) => field(label, hint, h('input', {
      type: 'number', min: lo, max: hi, value: signed ? (b()[base + OP[key]] << 24 >> 24) : b()[base + OP[key]],
      onchange: e => { const v = parseInt(e.target.value, 10); if (v >= lo && v <= hi) { b()[base + OP[key]] = v & 255; commitOpt(); } },
    }));
    const osel = (label, hint, key, opts) => {
      const sel = h('select', { onchange: e => { b()[base + OP[key]] = +e.target.value & 255; commitOpt(); } }, opts.map(([v, t]) => h('option', { value: v }, t)));
      const cur = b()[base + OP[key]];
      if (!opts.some(([v]) => v === cur)) sel.append(h('option', { value: cur }, `Unknown (${cur})`));
      sel.value = cur;
      return field(label, hint, sel);
    };
    const onOff = [[0, 'Off'], [1, 'On']];
    body.append(
      h('p', { class: 'note' }, 'These options are stored once per FILE and apply to all of its slots. The game loads them with the save; the PC port keeps its own settings on top.'),
      h('div', { class: 'grid' },
        onum('Brightness', 'Range 0 to 7, default 3.', 'brightness', 0, 7),
        onum('Music volume', 'Range 0 to 128 in steps of 8, default 16.', 'volumeBgm', 0, 128),
        onum('Sound volume', 'Range 0 to 128 in steps of 8, default 16.', 'volumeSe', 0, 128),
        osel('Sound', null, 'soundType', [[0, 'Stereo'], [1, 'Mono']]),
        osel('Vibration', null, 'vibrationEnabled', [[0, 'Off'], [128, 'On']]),
        osel('Auto load', null, 'autoLoad', onOff),
        onum('Screen X', 'Range -11 to 11.', 'screenPositionX', -11, 11, true),
        onum('Screen Y', 'Range -8 to 8.', 'screenPositionY', -8, 8, true),
        osel('Weapon control', null, 'extraWeaponCtrl', [[0, 'Switch'], [1, 'Press']]),
        osel('Blood colour', 'Extra option.', 'extraBloodColor', Object.entries(D.bloodColors).map(([v, n]) => [+v, n])),
        osel('View control', null, 'extraViewCtrl', [[0, 'Normal'], [1, 'Reverse']]),
        osel('View mode', null, 'extraViewMode', [[0, 'Normal'], [1, 'Self view']]),
        osel('Retreat turn', null, 'extraRetreatTurn', [[0, 'Normal'], [1, 'Reverse']]),
        osel('Walk/run control', null, 'extraWalkRunCtrl', [[0, 'Normal'], [1, 'Reverse']]),
        osel('Auto aiming', null, 'extraAutoAiming', [[0, 'On'], [1, 'Off']]),
        onum('Bullet adjust', 'x1 to x6, stored 0 to 5.', 'extraBulletAdjust', 0, 5),
        onum('Extra options unlocked', 'Bit flags for unlocked extra options.', 'extraOptionsEnabled', 0, 255)));
  }

  function open(entry, fi, s) {
    ctx = { entry, fi, s };
    render();
    panel.scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'start' });
  }
  function close() { ctx = null; panel.hidden = true; panel.innerHTML = ''; hooks.onClose && hooks.onClose(); }
  /** Keeps the panel pointing at the same save after slots were rearranged elsewhere. */
  function follow(entry, fromFi, fromS, toFi, toS) {
    if (!ctx || ctx.entry !== entry) return;
    if (ctx.fi === fromFi && ctx.s === fromS) { ctx.fi = toFi; ctx.s = toS; }
    else if (ctx.fi === toFi && ctx.s === toS) { ctx.fi = fromFi; ctx.s = fromS; }
    if (!S.slotUsed(blk(), ctx.s)) close(); else render();
  }

  root.SHEditor = {
    mount(el, h_) { panel = el; hooks = h_; },
    open, close, follow,
    current: () => ctx,
    refresh: () => { if (ctx) render(); },
  };
})(typeof self !== 'undefined' ? self : this);
