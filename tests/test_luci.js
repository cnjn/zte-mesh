// Runs the LuCI view of openwrt/luci-app-zte-mesh against a minimal mock of LuCI's form API and checks that the form offers
// every UCI option the init script reads (tests/test_init.sh "all-options" case, the slot and ACL sections).
// usage: node tests/test_luci.js [zte-mesh.js] [test_init.sh]
const fs = require('fs'), path = require('path');
const viewFile = process.argv[2] || path.join(__dirname, '../openwrt/luci-app-zte-mesh/htdocs/luci-static/resources/view/network/zte-mesh.js');
const initTest = process.argv[3] || path.join(__dirname, 'test_init.sh');
String.prototype.format = function() { let i = 0, a = arguments; return this.replace(/%[sd]/g, () => a[i++]); };
const registered = {}; // section type -> {option: {datatype, depends, ...}}
let uciRole = 'master'; // what uci.get('zte_mesh', 'main', 'mesh_role') answers
let uciLocal = null;   // what uci.get('zte_mesh', 'main', 'apply_local') answers; null = option absent (legacy)
class Opt { constructor(n, c) { this.name = n; this.cls = c; this.deps = []; this.values = []; }
	depends(k, v) { this.deps.push([k, v]); } value(v) { this.values.push(v); } }
class Sec { constructor(type) { this.type = type; registered[type] = registered[type] || {}; }
	tab() {} taboption(t, c, n) { return registered[this.type][n] = new Opt(n, c.name); }
	option(c, n) { return registered[this.type][n] = new Opt(n, c.name); } }
class Map { constructor() {} section(c, name, type) { return new Sec(c === GridSection ? name : type); }
	render() { return Promise.resolve({}); } }
class NamedSection {} class GridSection {}
class Flag {} class Value {} class ListValue {} class MultiValue {}
// The view derives ReportingGridSection from form.GridSection. Here the derived class is the same one, which keeps
// the mock's m.section(Class, NAME, ...) keyed on NAME (the section type the init script and this test agree on).
GridSection.extend = () => GridSection;
let viewDef;
const mocks = {
	view: { extend: d => (viewDef = d) },
	dom: { content() {} }, poll: { add() {} }, ui: { createHandlerFn: (t, f) => f, addNotification() {} },
	uci: { load: () => Promise.resolve(), sections: (c, t) => (t === 'zte_mesh_node' ? [{ mac: 'AA:BB:CC:DD:EE:01', name: 'Living room' }] : []) }, network: { getHostHints: () => Promise.resolve({}) },
	rpc: { declare: () => () => Promise.resolve({}) },
	form: { Map, NamedSection, GridSection, Flag, Value, ListValue, MultiValue },
};
// GridSection options are keyed by the section TYPE argument of m.section(form.GridSection, TYPE, ...)
const src = fs.readFileSync(viewFile, 'utf8').replace(/^'use strict';/, '').replace(/^'require [^']*';$/gm, '');
mocks.uci.get = (pkg, sec, opt) => (pkg !== 'zte_mesh' || sec !== 'main' ? null
	: opt === 'mesh_role' ? uciRole
		: opt === 'apply_local' ? uciLocal : null);
const names = Object.keys(mocks);
const fn = new Function(...names, '_', 'E', src);
// dom.create() semantics, as far as the view can trip over them: the first argument is used directly
// (html.charCodeAt(0)), so E(null) throws "Cannot read properties of null (reading 'charCodeAt')". An array is
// unpacked by calling create() on every element, so E([ ok, null ]) throws the same way, while
// E('div', attrs, [ ok, null ]) does not: that path goes through append(), which skips nulls. The stub used to
// accept anything, which is why the child role could break in a browser while this test stayed green.
const newNode = (t, a, c) => ({ t, a, c, kids: [], appendChild(x) { this.kids.push(x); return x; } });
const E = (t, a, c) => {
	if (Array.isArray(t)) {
		const frag = newNode('#document-fragment');
		for (const kid of t)
			frag.appendChild(E(kid));
		return frag;
	}
	if (t === null || t === undefined)
		throw new TypeError("Cannot read properties of null (reading 'charCodeAt')");
	return newNode(t, a, c);
};
const result = fn(...names.map(n => mocks[n]), s => s, E);
(async () => {
	const data = [{ installed: true, running: true, state: { running: true, nodes: [] }, age: 1 }, {}, undefined];
	/* The sweep below compares every option the init test exercises against this form, so it has to be the role that
	 * can offer all of them: a controller that also broadcasts its own Wi-Fi. The controller-only default shows fewer
	 * fields on purpose, and the role blocks further down check exactly that. */
	uciLocal = '1';
	await viewDef.render(data);
	await viewDef.load();
	// node table: ZTE routers that are online get the Restart / Light buttons, others a dash
	{
		const table = viewDef.renderNodes({ state: { nodes: [
			{ mac: 'aa:bb:cc:dd:ee:01', name: 'child', model: 'M', software: 'v', zte: true, online: true, light: 1, stations: 8, bss: 2, hops: 1, metrics_age: 5,
			  bss_detail: [ { bssid: '02:00:00:00:19:e3', stations: 3, age: 5 }, { bssid: '02:00:00:00:19:f3', stations: 5, age: 2 } ] },
			{ mac: 'aa:bb:cc:dd:ee:02', name: 'std', model: '', software: '', zte: false, online: true },
			{ mac: 'aa:bb:cc:dd:ee:03', name: 'off', model: '', software: '', zte: true, online: false } ] } }, {});
		const rows = table.kids;
		const buttons = r => JSON.stringify(r).split('"t":"button"').length - 1;
		const text = r => JSON.stringify(r);
		if (!text(rows[0]).includes('Living room') || text(rows[1]).includes('Living room') || !text(rows[0]).includes('"On"') || !text(rows[0]).includes('"8"')) {
			console.log('alias / light / clients cells wrong:', text(rows[0]).slice(0, 400));
			process.exit(1);
		}
		/* The link cell shows what the 1905 header said (direct or relayed) and the BSS count sits in its tooltip; a
		 * node that never reported metrics shows '-' rather than a zero that looks measured. */
		if (!text(rows[0]).includes('"Direct"') || !text(rows[0]).includes('2 BSS reported by the router') || !text(rows[0]).includes('5 s ago')) {
			console.log('link / BSS cells wrong:', text(rows[0]).slice(-500));
			process.exit(1);
		}
		/* Where the frames split the count per BSS, those rows are a column of their own: one line per BSS, the
		 * total stays in Clients, and a radio with nobody associated is a real `0` line, not a gap. */
		{
			const cells = rows[0].c;
			if (cells.length !== 12) { console.log('node row has', cells.length, 'cells, want 12'); process.exit(1); }
			const column = JSON.stringify(cells[8]);
			for (const w of ['"02:00:00:00:19:e3 3"', '"02:00:00:00:19:f3 5"', '"5 s ago"', '"2 s ago"']) {
				if (!column.includes(w)) { console.log('BSS column lacks ' + w + ':', column); process.exit(1); }
			}
			if (text(cells[7]).includes('02:00:00:00:19:e3')) { console.log('the total tooltip still repeats the rows'); process.exit(1); }
			if (!text(cells[7]).includes('2 BSS reported by the router')) { console.log('Clients lost the reported BSS count'); process.exit(1); }
		}
		/* A BSS that reports zero, and a BSS whose age the controller could not work out, both still belong in the
		 * column: neither may turn into a dash, because a dash is what "never reported" looks like. */
		{
			const zero = viewDef.renderNodes({ state: { nodes: [{ mac: 'aa:bb:cc:dd:ee:04', name: 'zero', zte: true, online: true,
				stations: 0, bss: 1, hops: 1, metrics_age: 0, bss_detail: [ { bssid: '02:00:00:00:19:01', stations: 0, age: -1 } ] }] } }, {});
			const column = JSON.stringify(zero.kids[0].c[8]);
			if (!column.includes('"02:00:00:00:19:01 0"')) { console.log('a zero-client BSS is not a line:', column); process.exit(1); }
			if (column.includes('s ago')) { console.log('an unknown age got a stale tooltip:', column); process.exit(1); }
			if (!JSON.stringify(rows[1].c[8]).includes('"-"')) { console.log('a node without per-BSS rows shows something else:', JSON.stringify(rows[1].c[8])); process.exit(1); }
		}
		if (!text(rows[1]).includes('"-"') || text(rows[1]).includes('"Direct"')) {
			console.log('a node without metrics should show no link:', text(rows[1]).slice(-400));
			process.exit(1);
		}
		if (rows.length !== 3 || buttons(rows[0]) !== 3 || buttons(rows[1]) !== 0 || buttons(rows[2]) !== 0) {
			console.log('node action buttons wrong:', rows.map(buttons));
			process.exit(1);
		}
	}
	/* The device list is read-only and local: one row per device, the tables that saw it named one by one, the
	 * network it is associated to as the position (the interface name is the diagnosis and lives in the tooltip),
	 * and a router of the topology marked as one so a child is not read as a device. */
	let bad0 = 0;
	{
		/* The fourth device is what a child router reported: this router's own tables only know the address, so the
		 * report is what makes it wireless, and the Node column names the child with the same title the topology
		 * table uses -- the model here, since the child has no name and no alias. */
		const info = { state: { nodes: [ { mac: '02:00:00:00:19:e3', name: '', model: 'ZXHN E2631' } ] } };
		const table = viewDef.renderDevices(info, [
			{ mac: '00:01:f3:96:43:cf', ip: '192.168.1.162', name: 'DESKTOP-JSJGOJL', ssid: '', iface: '', lease_end: 0, sources: 'lease neigh', node: false, via: '' },
			{ mac: '0a:95:b7:16:e2:39', ip: '192.168.1.222', name: 'Xiaomi-15', ssid: 'FIX2G', iface: 'phy1-ap0', lease_end: Math.round(Date.now() / 1000) + 600, sources: 'lease neigh wlan', node: false, via: '' },
			{ mac: 'd4:b7:09:0d:77:30', ip: '192.168.1.171', name: '', ssid: '', iface: '', lease_end: 0, sources: 'neigh', node: true, via: '' },
			{ mac: 'aa:95:b7:16:e2:40', ip: '192.168.1.223', name: '', ssid: '', iface: '', lease_end: 0, sources: 'neigh assoc', node: false, via: '02:00:00:00:19:e3' }
		]);
		const t = JSON.stringify(table);
		if (table.kids.length !== 4) { console.log('device table has', table.kids.length, 'rows, want 4'); bad0++; }
		for (const want of ['DESKTOP-JSJGOJL', 'DHCP lease', 'Neighbour table', 'Wired', 'FIX2G', 'phy1-ap0',
		                    'Wireless association', '10 min left', 'Child router', 'Node', 'Child router report']) {
			if (!t.includes(want)) { console.log('device table lacks ' + JSON.stringify(want)); bad0++; }
		}
		/* The reported device must not fall through to 'Wired', which is what the page used to say about a phone on
		 * a child's radio: its Position is 'Wireless', and the Node cell carries the child's title. */
		const reported = JSON.stringify(table.kids[3]) || '';
		if (!reported.includes('Wireless') || !reported.includes('ZXHN E2631') || reported.includes('Wired')) {
			console.log('reported device row is not attributed to its node:', reported);
			bad0++;
		}
		/* A device with no lease is not a device with an expired one: the lease cell is a dash, and nobody is told
		 * that a static address "ran out". */
		if (t.includes('Expired')) { console.log('a device without a lease shows Expired'); bad0++; }
		const empty = JSON.stringify(viewDef.renderDevices(null, []));
		if (!empty.includes('Nothing here yet')) { console.log('an empty device list has no explanation'); bad0++; }
	}
	/* Device actions: the status box is the only place a ban or a kick can be seen to have worked, so it has to
	 * spell out the address, the verb, the state and the time left. */
	{
		const box = viewDef.renderStatus({ installed: true, running: true, age: 1, role: 'master', state: { running: true, nodes: [],
			al_mac: '02:00:00:00:19:e3', config_applied: 1,
			clients: [ { mac: 'aa:bb:cc:dd:ee:ff', action: 'ban', state: 'sent', left: 42, attempts: 1 },
			           { mac: '11:22:33:44:55:66', action: 'kick', state: 'failed', left: 0, attempts: 3 } ] } });
		const t = JSON.stringify(box);
		for (const want of ['aa:bb:cc:dd:ee:ff: Blocked, sent · 1 min left', '11:22:33:44:55:66: Kicked, the router refused or did not answer']) {
			if (!t.includes(want)) { console.log('status box lacks ' + JSON.stringify(want) + ':', t.slice(0, 600)); bad0++; }
		}
		if (t.includes('Blocked, sent · 1 min left') && JSON.stringify(viewDef.renderStatus({ installed: true, running: true, age: 1, state: { running: true, nodes: [], clients: [] } })).includes('Device actions')) {
			console.log('status box shows an empty device action row'); bad0++;
		}
	}
	/* The title row reads the two axes the page writes: a controller, a controller that also broadcasts (the main
	 * router), or a child. Folding the legacy `main`/`auto` into that pair is rpcd's job, not the view's. */
	{
		const base = { installed: true, running: true, age: 1, state: { running: true, nodes: [] } };
		for (const [role, local, want] of [['master', false, 'Controller'], ['master', true, 'Main router'],
			['slave', false, 'Child router']]) {
			const t = JSON.stringify(viewDef.renderStatus(Object.assign({}, base, { role: role, local: local })));
			if (!t.includes(want)) { console.log('status title for role=' + role + ' local=' + local + ' lacks ' + want); bad0++; }
		}
		if (JSON.stringify(viewDef.renderStatus(Object.assign({}, base, { role: 'master', local: true }))).includes('own radios are not managed here')) {
			console.log('a broadcasting controller is still told it manages no radios'); bad0++;
		}
	}
	const text = fs.readFileSync(initTest, 'utf8');
	const wanted = { zte_mesh: new Set(['enabled', 'bridge', 'al_last']), /* read by start_service, not by write_config */ zte_mesh_bss: new Set(['band', 'slot', 'enabled', 'ssid', 'key', 'hidden', 'security', 'acl_policy', 'access_minutes', 'force_wifi5']),
	                 zte_mesh_acl: new Set(['mac', 'name', 'band', 'bss']), zte_mesh_timer: new Set(['start', 'end', 'days', 'scope']),
	                 zte_mesh_client: new Set(['mac', 'action']), zte_mesh_node: new Set(['mac', 'name']) };
	const block = text.slice(text.indexOf('# 8. every optional main option'), text.indexOf('run all-options'));
	for (const m of block.matchAll(/^\toption (\w+) '/gm)) wanted.zte_mesh.add(m[1]);
	// 'mode' is the legacy option of older files: the init script accepts and ignores it, so the form must not offer it.
	// 'mesh_v2' is the retired name of mesh_compatible, kept in the init test to prove the reader ignores it.
	wanted.zte_mesh.delete('mode');
	wanted.zte_mesh.delete('mesh_v2');
	for (const m of text.matchAll(/option (wifi_5g_ssid|wifi_5g_key|wifi_5g_suffix|wifi_dual_band_sync|wifi_24g_ssid|wifi_24g_key|wifi_5g_enabled|wifi_24g_enabled|enabled|bridge) /g)) wanted.zte_mesh.add(m[1]);
	let bad = bad0;
	for (const [type, set] of Object.entries(wanted)) {
		for (const name of set) {
			if (!registered[type] || !registered[type][name]) { console.log('LuCI form lacks', type, name); bad++; }
		}
	}
	const extra = Object.keys(registered.zte_mesh || {}).filter(n => !wanted.zte_mesh.has(n));
	if (extra.length) { console.log('form options the init test does not exercise:', extra.join(' ')); bad++; }
	// Child role: the settings this router would push to its own child routers do not apply there, so they must not be
	// offered at all. The backhaul choice is on both sides that touch this router's radios -- different end, same link.
	/* The action list writes what the init script reads: 'ban' first (it is the default), then 'kick'. A grid option that
	 * no page can set to a value the init accepts is the failure this checks for. */
	{
		const act = (registered.zte_mesh_client || {}).action;
		const vals = act ? act.values.map(v => Array.isArray(v) ? v[0] : v).join(',') : '';
		if (vals !== 'ban,kick') { console.log('client action list is "' + vals + '", want ban,kick'); bad++; }
	}
	const counts = { main: Object.keys(registered.zte_mesh || {}).length,
		bss: Object.keys(registered.zte_mesh_bss || {}).length,
		acl: Object.keys(registered.zte_mesh_acl || {}).length,
		client: Object.keys(registered.zte_mesh_client || {}).length, child: '' };
	// Device role: two names, because the page now writes the two axes and nothing else -- `main`/`auto` stay readable
	// on input but are not offered, so the form cannot produce a value the init script would have to translate.
	{
		const ro = (registered.zte_mesh || {}).mesh_role;
		const vals = ro ? ro.values.map(v => Array.isArray(v) ? v[0] : v).join(',') : '';
		if (vals !== 'master,slave') { console.log('device role list is "' + vals + '", want master,slave'); bad++; }
	}
	for (const k of Object.keys(registered)) delete registered[k];
	uciRole = 'slave';
	await viewDef.render([{ installed: true, running: true, state: { running: true, nodes: [] }, age: 1 }, {}]);
	{
		const m = registered.zte_mesh || {};
		/* extra_slots is not a controller setting: on a child it decides whether the slots the controller sends are put
		 * on this router's own air, which is exactly the child's business. The two radio pins are the same: a child
		 * with two cards on one band has to name its own. */
		const forbidden = Object.keys(m).filter(n => n !== 'enabled' && n !== 'bridge' && n !== 'mesh_role' && n !== 'backhaul' && n !== 'extra_slots' && n !== 'wifi_2g' && n !== 'wifi_5g');
		if (forbidden.length) { console.log('child form still offers controller settings:', forbidden.join(' ')); bad++; }
		if (!m.backhaul) { console.log('child form lacks the backhaul choice'); bad++; }
		if (!m.wifi_2g || !m.wifi_5g) { console.log('child form lacks the radio pins'); bad++; }
			if (registered.zte_mesh_bss || registered.zte_mesh_acl || registered.zte_mesh_timer || registered.zte_mesh_client) { console.log('child form still offers the controller grids'); bad++; }
		counts.child = Object.keys(registered.zte_mesh || {}).length + ' options';
	}
	// Main router that broadcasts (the legacy `main` = master + apply_local=1): the controller half is still there -- it
	// is what hands the profile out -- and the local half owns this router's own radios, so the two radios-side
	// switches (extra SSIDs and the backhaul) belong here too. The backhaul is the far end of the same link: on this
	// side it puts the backhaul BSS on the air instead of joining one.
	{
		for (const k of Object.keys(registered)) delete registered[k];
		uciRole = 'main';
		await viewDef.render([{ installed: true, running: true, state: { running: true, nodes: [] }, age: 1 }, {}]);
		const m = registered.zte_mesh || {};
		const missing = ['wifi_24g_ssid', 'wifi_24g_key', 'wifi_dual_band_sync', 'mesh_role', 'mesh_compatible', 'extra_slots', 'apply_local', 'backhaul', 'wifi_2g', 'wifi_5g'].filter(n => !m[n]);
		if (missing.length) { console.log('main router form lacks the controller options:', missing.join(' ')); bad++; }
		/* A pin is a wifi-device section name, and a name wlan-sync cannot match is ignored with a log line -- which is
		 * the same silent no-op the field exists to avoid, so the shape is refused here, before it is saved. */
		{
			const pin = m.wifi_5g;
			for (const [ok, v] of [[true, ''], [true, 'radio1'], [true, 'radio_hwmode11g'], [false, 'radio-1'], [false, 'radio 1'], [false, 'radio.1'], [false, '"radio1"']]) {
				const got = pin && pin.validate('main', v);
				if ((got === true) !== ok) { console.log('pin validate(' + JSON.stringify(v) + ') = ' + JSON.stringify(got) + ', accept=' + ok); bad++; }
			}
		}
		/* The second axis defaults to what the legacy name means, so a page opened on an old `main` configuration shows
		 * the checkbox ticked and saving it keeps the behaviour instead of quietly dropping the local broadcast. */
		if (m.apply_local && m.apply_local.default !== '1') { console.log('apply_local default on a legacy main is', m.apply_local.default); bad++; }
	}
	// A controller that leaves its own radios alone applies no profile here, so it builds neither end of the backhaul,
	// no extra SSID and names no radio: offering those would invite exactly the "I set it and nothing happened" report.
	{
		for (const k of Object.keys(registered)) delete registered[k];
		uciRole = 'master';
		uciLocal = '0';
		await viewDef.render([{ installed: true, running: true, state: { running: true, nodes: [] }, age: 1 }, {}]);
		const m = registered.zte_mesh || {};
		const unwanted = ['backhaul', 'extra_slots', 'wifi_2g', 'wifi_5g'].filter(n => m[n]);
		if (unwanted.length) { console.log('controller-only form offers settings that apply to radios it does not touch:', unwanted.join(' ')); bad++; }
		if (!m.apply_local) { console.log('controller-only form lost the apply_local switch'); bad++; }
		uciLocal = null;
	}
	console.log(bad ? 'luci test: FAIL' : 'luci test: ok (' + counts.main + ' main options, ' + counts.bss + ' slot options, ' + counts.acl + ' ACL options, ' + counts.client + ' client options, child form ' + counts.child + ')');
	process.exit(bad ? 1 : 0);
})().catch(e => { console.log('luci test: exception', e.stack); process.exit(1); });
