'use strict';
'require view';
'require dom';
'require form';
'require rpc';
'require poll';
'require ui';
'require uci';
'require network';

var callStatus = rpc.declare({
	object: 'luci.zte-mesh',
	method: 'status',
	expect: { '': {} }
});

var callRestart = rpc.declare({
	object: 'luci.zte-mesh',
	method: 'restart',
	expect: { '': {} }
});

/* Read-only: the rpcd plugin relays what the host-side collector saw -- DHCP leases, the neighbour table and the
 * stations on this router's own access points. Which device sits on which child router is not in here yet: the
 * controller learns a station count per BSS and nothing about who those stations are. */
var callDevices = rpc.declare({
	object: 'luci.zte-mesh',
	method: 'devices',
	expect: { '': {} }
});

var callRebootNode = rpc.declare({
	object: 'luci.zte-mesh',
	method: 'reboot_node',
	params: [ 'mac' ],
	expect: { '': {} }
});

var callLightNode = rpc.declare({
	object: 'luci.zte-mesh',
	method: 'light_node',
	params: [ 'mac', 'on' ],
	expect: { '': {} }
});

/* The controller answers a node action with a status code (see the plugin): null = the message for "sent", otherwise the reason. */
function actionProblem(res) {
	var r = res ? res.result : undefined;
	if (r === 0)
		return null;
	if (r === null || r === undefined)
		return _('No answer from the controller yet.');
	return {
		'-2': _('The request was malformed.'),
		'-3': _('That router is not online or is not a ZTE router.'),
		'-4': _('The controller is restarting; try again in a moment.'),
		'-5': _('The controller is not running.')
	}[String(r)] || _('The controller could not send the request (%s).').format(r);
}

function normMac(mac) {
	return (mac || '').toLowerCase();
}

/* Node 1905 AL MAC -> IPv4 address, using whatever the router has learned (luci-rpc keys its hosts by upper-case MAC). */
function lookupAddress(hints, mac) {
	var host = hints && hints.hosts ? hints.hosts[(mac || '').toUpperCase()] : null;
	if (host && host.ipaddrs && host.ipaddrs.length)
		return host.ipaddrs[0];
	return null;
}

function badge(text, cls) {
	return E('span', { 'class': 'ifacebadge', 'style': cls ? 'background:' + cls : null }, text);
}

/*
 * A table section whose edit dialog says why it does not close. LuCI saves the
 * dialog silently and drops the error, so a missing required field only shows
 * as a red frame, often on a field that is out of sight.
 */
var ReportingGridSection = form.GridSection.extend({
	handleModalSave: function(modalMap, ev) {
		var self = this, args = arguments;
		var mapNode = this.getActiveModalMap();
		var activeMap = dom.findClassInstance(mapNode);

		return activeMap.parse().then(function() {
			return self.super('handleModalSave', args);
		}).catch(function(e) {
			var box = mapNode.querySelector('.zte-mesh-modal-error');

			if (!box) {
				box = E('div', { 'class': 'alert-message warning zte-mesh-modal-error' });
				mapNode.insertBefore(box, mapNode.firstChild);
			}
			dom.content(box, E('p', [ _('Not saved: '), e.message ]));
		});
	}
});

return view.extend({
	load: function() {
		return Promise.all([
			callStatus(),
			network.getHostHints(),
			uci.load('zte_mesh'),
			callDevices()
		]);
	},

	renderStatus: function(info) {
		var state = info.state;
		var rows = [];
		var stale = info.age < 0 || info.age > 10;

		function row(label, value) {
			rows.push(E('div', { 'class': 'tr' }, [
				E('div', { 'class': 'td left', 'width': '33%' }, label),
				E('div', { 'class': 'td left' }, value)
			]));
		}

		if (!info.installed) {
			return E('div', { 'class': 'alert-message warning' },
				_('The ZTE Mesh runtime is not installed on this router.'));
		}

		var child = info.role === 'slave';
		var local = info.local === true;   /* the controller that also puts the profile on the air; rpcd reads both UCI options */
		var running = info.running && state && state.running && !stale;
		row(child ? _('Child router') : local ? _('Main router') : _('Controller'), running
			? badge(_('Running'), '#46a546')
			: badge(info.running ? _('Starting or stalled') : _('Stopped'), '#b94a48'));
		if (child)
			row(_('Device role'), _('Child router, managed by the controller'));
		else if (!local)
			row(_('Device role'), _('Controller only: this router\'s own radios are not managed here'));

		if (state && state.running) {
			row(_('Controller address (AL MAC)'), state.al_mac || '-');
			/* This router's own two names. The model is the one the daemon broadcasts in the mesh, so it reads
			 * the same here and in the controller's child list; when neither the broadcast nor the board name
			 * is known the cell says "-" instead of a made-up "router". */
			if (state.self) {
				row(_('Model'), state.self.model || '-');
				row(_('Host name'), state.self.name || 'router');
			}
			if (!child) {
				row(_('MESH compatible'), uci.get('zte_mesh', 'main', 'mesh_compatible') === '0'
					? _('Off (ZTE enhanced)') : _('On (ZTE compatible)'));
				row(_('Wi-Fi updates sent'), String(state.config_applied));
			}
			row(_('Wired port'), info.attached ? badge(_('Attached to the LAN bridge'), '#46a546')
			                                   : badge(_('Not attached'), '#b94a48'));
			/* What the controller has been asked to do to individual devices, as it stands now. A kick the child
			 * router answered reads "done", one that ran out of tries reads "did not work", and a block counts
			 * down. It is the only feedback there is: the child routers do not report a client list. */
			if (state.clients && state.clients.length) {
				var text = [];
				for (var i = 0; i < state.clients.length; i++) {
					var c = state.clients[i];
					var how = c.state === 'done' ? _('done')
						: c.state === 'failed' ? _('the router refused or did not answer')
						: c.state === 'expired' ? _('ran out')      /* a timed block that is over */
						: c.state === 'pending' ? _('queued') : _('sent');
					var left = c.left > 0 ? ' · ' + _('%d min left').format(Math.ceil(c.left / 60)) : '';
					text.push(c.mac + ': ' + (c.action === 'ban' ? _('Blocked') : _('Kicked')) + ', ' + how + left);
				}
				row(_('Device actions'), text.join('; '));
			}
		}

		/* Both roles that put a profile into /etc/config/wireless report it: a child gets the profile from the
		 * controller, a main router from its own configuration. wlan-sync runs outside the sandbox, so its last run is
		 * reported from the file it leaves behind, not from the service state. */
		if ((child || local) && info.sync) {
			var sy = info.sync, syAge = sy.epoch > 0 ? Math.max(0, Math.round(Date.now() / 1000 - sy.epoch)) : -1;
			row(_('Wi-Fi synchronisation'), [
				sy.result === 'ok' ? badge(_('Applied'), '#46a546') : badge(_('Failed'), '#b94a48'),
				syAge >= 0 ? ' ' + _('%d s ago').format(syAge) : '',
				sy.bands ? ' · ' + sy.bands : '',
				sy.message ? ' · ' + sy.message : ''
			]);
		}

		if (state && state.config_error)
			row(_('Last configuration problem'), E('span', { 'style': 'color:#b94a48' }, state.config_error));
		if (state && state.restart_required)
			row(_('Pending'), _('A setting changed that the controller reads only at start; restart the controller to apply it.'));
		if (stale && info.running)
			row(_('Report'), _('No fresh status for %d s').format(info.age));

		return E('div', { 'class': 'table' }, rows);
	},

	/* Restart and LED buttons of one child router (ZTE routers only; the controller tunnels them to the router). */
	renderNodeActions: function(node, title) {
		var done = function(res, okText) {
			var problem = actionProblem(res);
			ui.addNotification(null, E('p', problem || okText), problem ? 'warning' : 'info');
		};
		var led = function(on) {
			return ui.createHandlerFn(this, function() {
				return callLightNode(node.mac, on).then(function(res) {
					done(res, on ? _('Indicator light switched on.') : _('Indicator light switched off.'));
				});
			});
		};
		return E('div', { 'class': 'cbi-section-actions' }, [
			E('button', {
				'class': 'btn cbi-button cbi-button-negative',
				'click': ui.createHandlerFn(this, function() {
					ui.showModal(_('Restart router'), [
						E('p', _('Restart %s? Its Wi-Fi and the devices connected through it are offline for a few minutes.').format(title)),
						E('div', { 'class': 'right' }, [
							E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
							E('button', {
								'class': 'btn cbi-button cbi-button-negative important',
								'click': ui.createHandlerFn(this, function() {
									ui.hideModal();
									return callRebootNode(node.mac).then(function(res) {
										done(res, _('The restart request was sent.'));
									});
								})
							}, _('Restart'))
						])
					]);
				})
			}, _('Restart')), ' ',
			E('button', { 'class': 'btn cbi-button', 'click': led(true) }, _('Light on')), ' ',
			E('button', { 'class': 'btn cbi-button', 'click': led(false) }, _('Light off'))
		]);
	},

	renderNodes: function(info, hints) {
		var state = info.state;
		var nodes = (state && state.nodes) || [];
		var table = E('table', { 'class': 'table cbi-section-table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Name')),
				E('th', { 'class': 'th' }, _('Model')),
				E('th', { 'class': 'th' }, _('Address')),
				E('th', { 'class': 'th' }, _('MAC')),
				E('th', { 'class': 'th' }, _('Software')),
				E('th', { 'class': 'th' }, _('Type')),
				E('th', { 'class': 'th' }, _('Light')),
				E('th', { 'class': 'th' }, _('Clients')),
				E('th', { 'class': 'th' }, _('BSS detail')),
				E('th', { 'class': 'th' }, _('Link')),
				E('th', { 'class': 'th' }, _('State')),
				E('th', { 'class': 'th' }, _('Actions'))
			])
		]);
		var self = this;

		/* names the administrator gave to child routers (the controller keeps them, like the factory topology page; the router itself is not renamed) */
		var aliases = {};
		uci.sections('zte_mesh', 'zte_mesh_node').forEach(function(sec) {
			if (sec.mac && sec.name)
				aliases[normMac(sec.mac)] = sec.name;
		});

		nodes.forEach(function(node) {
			var ip = lookupAddress(hints, node.mac);
			var title = aliases[normMac(node.mac)] || node.name || node.model || node.mac;
			/* The station count comes from the AP Metrics the child sends; the BSS count from its Topology Response.
			 * Both are -1 (shown as '-') when the child has not reported them, which is not the same as zero.
			 * Where the frames split that count per BSS, the rows get a column of their own - one total cannot say
			 * which radio is busy - and a BSS with nobody associated is a zero line there, not a missing one.
			 * The controller sorted and capped them; re-sorting here would disagree with the total next door. */
			var detail = node.bss_detail || [];
			var shown = node.bss >= 0 ? _('%d BSS reported by the router').format(node.bss) : '';
			table.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, title),
				E('td', { 'class': 'td' }, node.model || '-'),
				E('td', { 'class': 'td' }, ip
					? E('a', { 'href': 'http://' + ip + '/', 'target': '_blank', 'rel': 'noreferrer' }, ip)
					: '-'),
				E('td', { 'class': 'td' }, node.mac),
				E('td', { 'class': 'td' }, node.software || '-'),
				E('td', { 'class': 'td' }, node.zte ? 'ZTE' : _('Standard')),
				E('td', { 'class': 'td' }, node.light === 1 ? _('On') : (node.light === 0 ? _('Off') : '-')),
				E('td', { 'class': 'td', 'title': shown },
					(node.online && node.stations >= 0) ? String(node.stations) : '-'),
				/* One line per BSS the child reported, so a reader can see which radio holds the clients. A node whose
				 * frames never split the count has nothing here: '-' is "not reported", not "nobody associated". */
				E('td', { 'class': 'td' }, detail.length ? E('div', { 'class': 'small' },
					detail.map(function(b) {
						return E('div', { 'title': b.age >= 0 ? _('%d s ago').format(b.age) : '' },
							b.bssid + ' ' + b.stations);
					})) : '-'),
				/* Reached over its own link or through another node: the 1905 header's relay bit, nothing inferred. */
				E('td', { 'class': 'td', 'title': (node.metrics_age >= 0 ? _('%d s ago').format(node.metrics_age) : '') },
					node.hops === 1 ? _('Direct') : (node.hops === 2 ? _('Relayed') : '-')),
				E('td', { 'class': 'td' }, node.online ? badge(_('Online'), '#46a546') : badge(_('Offline'), '#999')),
				E('td', { 'class': 'td' }, (node.online && node.zte) ? self.renderNodeActions(node, title) : '-')
			]));
		});

		if (!nodes.length)
			table.appendChild(E('tr', { 'class': 'tr placeholder' }, [
				E('td', { 'class': 'td', 'colspan': 12 },
					E('em', _('No child routers have been discovered yet.')))
			]));

		return table;
	},

	/* What this router can see itself. Three local tables, merged host-side by MAC and marked with which tables saw the
	 * device, because they age differently: a lease outlives the device and a neighbour entry only lasts while the
	 * kernel keeps talking to the address. A wireless row carries the network it is associated to, so a device on a
	 * guest network is not confused with one on the main network; a wired row says so instead of guessing a port.
	 * A router of the topology is marked as one: the controller's own child routers are here, and they are not devices.
	 * A device a child router reported (`via`) is wireless by that report: the address tables of this router cannot see
	 * which radio another router's client is on, so the Position follows that report and the Node column names the
	 * child, with the same title the topology table uses. */
	renderDevices: function(info, list) {
		var count = 0;
		var state = info && info.state;
		var nodes = (state && state.nodes) || [];
		var aliases = {};
		uci.sections('zte_mesh', 'zte_mesh_node').forEach(function(sec) {
			if (sec.mac && sec.name)
				aliases[normMac(sec.mac)] = sec.name;
		});
		var titles = {};
		nodes.forEach(function(node) {
			titles[normMac(node.mac)] = aliases[normMac(node.mac)] || node.name || node.model || node.mac;
		});
		var table = E('table', { 'class': 'table cbi-section-table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Device')),
				E('th', { 'class': 'th' }, _('MAC')),
				E('th', { 'class': 'th' }, _('Address')),
				E('th', { 'class': 'th' }, _('Position')),
				E('th', { 'class': 'th' }, _('Node')),
				E('th', { 'class': 'th' }, _('Seen by')),
				E('th', { 'class': 'th' }, _('Lease')),
				E('th', { 'class': 'th' }, _('Note'))
			])
		]);
		var now = Math.round(Date.now() / 1000);

		(list || []).forEach(function(d) {
			var ip = d.ip && d.ip !== '-' ? d.ip : '';
			var sources = [];
			(d.sources || '').split(' ').forEach(function(s) {
				if (s === 'lease')
					sources.push(_('DHCP lease'));
				else if (s === 'neigh')
					sources.push(_('Neighbour table'));
				else if (s === 'wlan')
					sources.push(_('Wireless association'));
				else if (s === 'assoc')
					sources.push(_('Child router report'));
			});
			var lease = '-';
			if (d.lease_end > 0) {
				var left = d.lease_end - now;
				lease = left > 0 ? _('%d min left').format(Math.ceil(left / 60)) : _('Expired');
			}
			/* The SSID is what a reader recognises; the interface name is the diagnosis. One is shown, the other kept
			 * in the tooltip, so neither replaces the other. A row a child reported without an SSID is still a wireless
			 * association - it must not fall through to 'Wired', which is what this page used to say about a phone on a
			 * child's radio because no local table could place it. */
			var where = d.ssid ? badge(d.ssid) : (d.iface ? badge(d.iface) : (d.via ? _('Wireless') : _('Wired')));
			var via = d.via ? (titles[normMac(d.via)] || d.via) : '-';

			table.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, d.name || '-'),
				E('td', { 'class': 'td' }, d.mac),
				E('td', { 'class': 'td' }, ip
					? E('a', { 'href': 'http://' + ip + '/', 'target': '_blank', 'rel': 'noreferrer' }, ip)
					: '-'),
				E('td', { 'class': 'td', 'title': d.iface || '' }, where),
				E('td', { 'class': 'td', 'title': d.via || '' }, via),
				E('td', { 'class': 'td' }, sources.length ? sources.join(', ') : '-'),
				E('td', { 'class': 'td' }, lease),
				E('td', { 'class': 'td' }, d.node ? badge(_('Child router')) : '-')
			]));
			count++;
		});

		if (!count)
			table.appendChild(E('tr', { 'class': 'tr placeholder' }, [
				E('td', { 'class': 'td', 'colspan': 8 }, E('em', _('Nothing here yet: this router has no DHCP lease, no neighbour entry and no station on its access points.')))
			]));

		return table;
	},

	render: function(data) {
		var info = data[0], hints = data[1], devices = data[3];
		var statusBox = E('div', { 'id': 'zte-mesh-status' }, this.renderStatus(info));
		var nodesBox = E('div', { 'id': 'zte-mesh-nodes' }, this.renderNodes(info, hints));
		var devicesBox = E('div', { 'id': 'zte-mesh-devices' }, this.renderDevices(info, devices && devices.devices));
		var self = this;

		poll.add(function() {
			return Promise.all([ callStatus(), network.getHostHints(), callDevices() ]).then(function(res) {
				dom.content(statusBox, self.renderStatus(res[0]));
				dom.content(nodesBox, self.renderNodes(res[0], res[1]));
				dom.content(devicesBox, self.renderDevices(res[0], res[2] && res[2].devices));
			});
		}, 5);

		/* The role decides which half of this page applies. A child takes its Wi-Fi profile, roaming thresholds and extra
		 * networks from the controller, so those settings are not rendered there at all: an option that silently does
		 * nothing is worse than a missing one, and one typed by accident is worse still. Changing the role reloads the
		 * page (Save and Apply does), which is also when the service restarts, so the form is built once per role. */
		/* uci.load() resolves to the list of package names it loaded, not to a
		 * cursor: the data is in the module cache and is read back through
		 * uci.get(). data[2] is only awaited for that side effect. */
		var role = uci.get('zte_mesh', 'main', 'mesh_role') || 'master';
		var child = (role === 'slave');
		/* Two options, two axes: mesh_role says controller or child, apply_local says a controller also broadcasts.
		 * `main` and `auto` are the old spelling of the broadcasting controller and still mean apply_local=1, so a
		 * configuration written before the split is shown as what it does rather than as something else. */
		var legacy = (role === 'main' || role === 'auto');
		var apply = uci.get('zte_mesh', 'main', 'apply_local');
		var local = !child && (apply == null ? legacy : apply === '1');

		var m = new form.Map('zte_mesh', _('ZTE Mesh'), child
			? _('This router is a mesh child: the controller manages it and gives it the Wi-Fi name, password and security.')
			: local
			? _('The main router: this router is the mesh controller and also broadcasts the Wi-Fi profile below, so it and its child routers put the same network on the air.')
			: _('A controller without Wi-Fi of its own: it manages the child routers and gives them the Wi-Fi profile below. This router\'s own radios are left alone.'));
		var s = m.section(form.NamedSection, 'main', 'zte_mesh');
		s.addremove = false;
		var o;

		s.tab('general', _('General'));
		if (!child) {
			s.tab('wifi', _('Wi-Fi profile'));
			s.tab('policy', _('Roaming and steering'));
		}

		o = s.taboption('general', form.Flag, 'enabled', _('Enable'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('general', form.Value, 'bridge', _('LAN bridge'));
		o.default = 'br-lan';
		o.datatype = 'maxlength(15)';
		o.rmempty = false;
		o.description = _('The bridge the child routers are cabled to (up to 15 characters, default br-lan).');

		/* The controller identity a ZTE child matches on. Only the last byte is open: the first five bytes are the
		 * shape the child's admission check expects, so a free-form address would simply be refused. The byte exists
		 * for the one case a derivation cannot solve -- two boxes with the same LAN MAC (a cloned image). */
		if (!child) {
		var alNow = (info.state && info.state.al_mac) || '';
		o = s.taboption('general', form.Value, 'al_last', _('Controller address, last byte'));
		o.datatype = 'and(minlength(2),maxlength(2))';
		o.placeholder = alNow ? alNow.slice(-2) : '06';
		o.optional = true;
		o.validate = function(section_id, value) {
			if (value == null || value === '')
				return true;
			return /^[0-9a-fA-F]{2}$/.test(value) ? true : _('Two hex digits, as in e3.');
		};
		o.description = _('The address this router presents as a mesh controller is 02:00:00:00:19:<this byte>. Empty: the last byte of the LAN bridge MAC, which already differs between two boxes. Change it only when they still collide, for example because the same image was cloned onto both. Only this byte can be changed: the first five are the shape a ZTE child accepts. Currently %s.').format(alNow || '02:00:00:00:19:--');
		}

		o = s.taboption('general', form.ListValue, 'mesh_role', _('Device role'));
		o.value('master', _('Controller'));
		o.value('slave', _('Child router'));
		o.default = 'master';
		o.optional = true;
		o.description = _('Controller: this router runs the mesh -- it answers the child routers, hands out the Wi-Fi profile and decides about roaming and steering. Child router: this router is a mesh agent, it hands nothing out and the controller gives it the Wi-Fi name, key and security. A child starts from the wireless network this router already broadcasts until the controller sends its own, so nothing has to be filled in here: the Wi-Fi, roaming and extra-network settings are hidden while the child role is selected. Whether a controller also broadcasts this router\'s own Wi-Fi is the choice below. Changing the role restarts the service and reloads this page, and the child routers drop out of the topology for a short while.');

		/* The second axis, and the reason the old third role name is gone: being the controller and being the Wi-Fi of
		 * this box are two decisions, and a box can be either, both or one. Not offered on a child, whose radios are
		 * the controller's to configure. The default follows `main`/`auto`, the old spelling of on. */
		if (!child) {
		o = s.taboption('general', form.Flag, 'apply_local', _('Also broadcast here'));
		o.default = legacy ? '1' : '0';
		o.rmempty = false;
		o.description = _('On: this controller puts the Wi-Fi profile below on its own radios as well as handing it to the child routers, so it and they broadcast the same network -- the main router of the factory page. Off: the profile goes to the child routers only and this router\'s own radios are left exactly as they are, which is the shape of most wired OpenWrt boxes at the head of a mesh, so it is the default. On a router that does apply a profile here, a radio device that was switched off is switched on when the band is enabled, because a disabled radio puts nothing on the air; a radio switched off afterwards stays off until a different profile arrives.');
		}

		/* The backhaul is a link with two ends, and which end this router sits on is the role's business: a child
		 * reaches the controller as a client, a broadcasting controller puts the backhaul BSS on its own air.
		 * A controller that leaves its own radios alone (apply_local off) builds neither, so the field there would
		 * only invite the wrong expectation -- same rule as the extra SSIDs below. */
		if (child || local) {
		o = s.taboption('general', form.ListValue, 'backhaul',
			child ? _('Backhaul to the controller') : _('Backhaul BSS for the child routers'));
		o.value('off', _('Off (main SSIDs only)'));
		o.value('multi_ap', _('Multi-AP (4-address)'));
		o.default = 'off';
		o.optional = true;
		/* One description per end, and the restart warning once: it is the same consequence either way, and the signature
		 * only carries the mode where an applier actually runs (a controller that touches no radio restarts for nothing). */
		o.description = (child
			? _('How this router reaches the controller over the radio. Off: only the main SSIDs are applied, as before. Multi-AP: the backhaul network the controller sends becomes a client interface (mode sta, marked as a Multi-AP backhaul station, with 4-address mode on and the bridge of that band), so the mesh control traffic crosses the radio instead of the cable. Requires a hostapd built with Multi-AP support (the full wpad, not wpad-basic); nothing is created until the controller has sent a backhaul network.')
			: _('Whether this router puts the backhaul network of its profile on the air. Off: only the main SSIDs are applied, as before. Multi-AP: the hidden backhaul network becomes its own BSS (multi_ap=2) and the main SSIDs are marked as fronthaul, so a child that joins it reaches this router as 4-address Ethernet over the radio. Requires a hostapd built with Multi-AP support (the full wpad, not wpad-basic); nothing is created until the profile carries a backhaul network.'))
			+ ' ' + _('Switching this restarts the mesh service (the child routers drop out of the topology for up to a minute).');

		}

		/* Extra SSIDs, on the two cases that apply a profile to this router: on a child they are the slots the controller
		 * sends, on a broadcasting controller the ones configured below. A controller that leaves its own radios alone
		 * hands them out and never puts them on its own air, so the field would only invite the wrong expectation there. */
		if (child || local) {
		o = s.taboption('general', form.ListValue, 'extra_slots', _('Extra SSIDs on this router'));
		o.value('0', _('Off (main SSIDs only)'));
		o.value('1', _('On (guest, spare and hidden backhaul too)'));
		o.default = '0';
		o.optional = true;
		o.description = _('Whether the extra SSID slots are applied to this router as well. Off: only the main SSIDs are applied, as before. On: every enabled slot becomes its own network on the band it belongs to, and a slot that stops arriving is removed again. An extra slot shares the bridge of its band\'s main network: this image has no VLAN separation yet, so a guest slot is not isolated from the main one.');
		}

		/* Which device of a band the applier writes. A board can carry two on one band (an ax3600 has an unused PCIe
		 * 5 GHz card next to the SoC radio), and the only way wlan-sync tells the user so is a log line naming these
		 * two options -- an option the log points at with no field on the page is the same dead end as a switch that
		 * restarts nothing. */
		if (child || local) {
		var pinDesc = _('Which wireless device carries this band. Leave it alone unless the service log says it cannot tell the two apart: a band with one device needs no answer, and a band with several uses the one that is not switched off. The value is a wireless device name of the Wi-Fi settings, such as radio1, and it has to be a device of this band -- a name that is not one is ignored, and the mesh is left off that radio.')
			+ ' ' + _('Switching this restarts the mesh service (the child routers drop out of the topology for up to a minute).');
		var pinName = function(section_id, value) {
			if (value == null || value === '')
				return true;
			return /^[A-Za-z0-9_]+$/.test(value) ? true : _('A wireless device name: letters, digits and underscores, like radio1.');
		};
		o = s.taboption('general', form.Value, 'wifi_2g', _('2.4 GHz wireless device'));
		o.optional = true;
		o.placeholder = 'radio0';
		o.validate = pinName;
		o.description = pinDesc;

		o = s.taboption('general', form.Value, 'wifi_5g', _('5 GHz wireless device'));
		o.optional = true;
		o.placeholder = 'radio1';
		o.validate = pinName;   /* the same shape, and one message to translate */
		o.description = pinDesc;
		}

		/* Controller side: what this router pushes to its child routers. Not rendered on a child, where the service runs
		 * with these features off (a child has no agents of its own) and changing a value only invites misconfiguration. */
		if (!child) {
		o = s.taboption('general', form.Flag, 'mesh_compatible', _('MESH compatible'));
		o.default = '1';
		o.rmempty = false;
		o.description = _('The factory MESH switch. On (ZTE compatible, default): any child router is accepted. Off (ZTE enhanced): only ZTE child routers with the same market code are accepted. Switching restarts the mesh daemon (the child routers drop out of the topology for up to a minute).');

		o = s.taboption('general', form.Flag, 'mlo_enable', _('Wi-Fi 7 MLO'));
		o.optional = true;
		o.default = '1';
		o.description = _('Tell child routers whether multi-link operation is on (factory default: on). Only Wi-Fi 7 children act on it.');

		o = s.taboption('wifi', form.Flag, 'wifi_dual_band_sync', _('Same name and password on both bands'));
		o.default = '1';
		o.rmempty = false;
		o.description = _('On: the 5 GHz band uses the 2.4 GHz name, password, security, hidden setting and MAC filter policy. Off: the 5 GHz name is the 2.4 GHz name plus the suffix below unless a 5 GHz name is given.');

		function profile(band, label) {
			var id = 'wifi_' + band;
			var five = band === '5g';

			o = s.taboption('wifi', form.Flag, id + '_enabled', _('%s enabled').format(label));
			o.default = '1';
			o.rmempty = false;
			o.description = local
				? _('Off switches the main network of this band off here and on the child routers.')
				: _('Off switches the main network of this band off on the child routers.');

			o = s.taboption('wifi', form.Value, id + '_ssid', _('%s network name (SSID)').format(label));
			o.datatype = 'and(minlength(1),maxlength(31))';
			if (five) {
				o.rmempty = true;
				o.depends('wifi_dual_band_sync', '0');
				o.placeholder = _('2.4 GHz name + suffix');
				o.description = _('1 to 31 characters. Empty: the 2.4 GHz name plus the suffix below.');
			} else {
				o.rmempty = false;
				o.default = 'ImmortalWrt';
				o.description = _('1 to 31 characters. Required.');
			}

			o = s.taboption('wifi', form.Value, id + '_key', _('%s password').format(label));
			o.password = true;
			o.datatype = 'and(minlength(8),maxlength(63))';
			if (five) {
				o.rmempty = true;
				o.depends('wifi_dual_band_sync', '0');
				o.placeholder = _('Same as 2.4 GHz');
				o.description = _('8 to 63 characters. Empty: the 2.4 GHz password.');
			} else {
				o.rmempty = false;
				o.default = 'Ztemesh2631!';
				o.placeholder = _('8 to 63 characters');
				o.description = _('8 to 63 characters, default Ztemesh2631!. Required: the controller does not start without one.');
			}

			if (five) {
				o = s.taboption('wifi', form.Value, 'wifi_5g_suffix', _('5 GHz name suffix'));
				o.datatype = 'and(minlength(1),maxlength(8))';
				o.default = '-5G';
				o.optional = true;
				o.depends('wifi_dual_band_sync', '0');
				o.description = _('1 to 8 characters, appended to the 2.4 GHz name when no 5 GHz name is given (factory default -5G). The whole name must stay within 31 characters.');
			}

			o = s.taboption('wifi', form.Flag, id + '_hidden', _('%s hidden network name').format(label));
			o.optional = true;
			o.default = '0';
			if (five)
				o.depends('wifi_dual_band_sync', '0');

			o = s.taboption('wifi', form.ListValue, id + '_security', _('%s security').format(label));
			o.value('psk2', 'WPA2-PSK');
			o.value('psk-mixed', 'WPA/WPA2-PSK');
			o.value('sae-mixed', 'WPA2/WPA3');
			o.value('sae', 'WPA3');
			o.value('none', _('None (open network)'));
			o.default = five ? 'psk-mixed' : 'sae-mixed';
			o.optional = true;
			o.description = _('Child routers without WPA3 treat WPA2/WPA3 as WPA2 and ignore WPA3-only. "None" makes the main network open.');
			if (five)
				o.depends('wifi_dual_band_sync', '0');

			o = s.taboption('wifi', form.Value, id + '_beacon_interval', _('%s beacon interval (ms)').format(label));
			o.datatype = 'range(20,1000)';
			o.default = '100';
			o.optional = true;
			o.description = _('20 to 1000 ms, factory default 100. Changing it restarts the radio of the child routers for a few seconds.');

			o = s.taboption('wifi', form.Value, id + '_channel', _('%s channel').format(label));
			o.datatype = five ? 'or(range(0,0),range(36,165))' : 'range(0,13)';
			o.default = '0';
			o.optional = true;
			o.description = five
				? _('0 = automatic (factory default). Fixed: 36, 40, 44, 48, 52 to 64, 100 to 144 and 149 to 165 in steps of 4. Empty: keep the child routers\' setting.')
				: _('0 = automatic (factory default). Fixed: 1 to 13. Empty: keep the child routers\' setting.');

			o = s.taboption('wifi', form.Value, id + '_tx_power', _('%s transmit power (dBm, EIRP)').format(label));
			o.datatype = 'range(0,30)';
			o.default = five ? '23' : '20';
			o.optional = true;
			o.description = five
				? _('0 to 30 dBm. The child routers round it to one of five levels: above 21 is full power (factory default), 20 to 21, 16 to 19, 10 to 15, below 10. Empty: keep the child routers\' setting.')
				: _('0 to 30 dBm. The child routers round it to one of five levels: above 19 is full power (factory default), 16 to 19, 12 to 15, 8 to 11, below 8. Empty: keep the child routers\' setting.');

			o = s.taboption('wifi', form.ListValue, id + '_acl_policy', _('%s MAC filter policy').format(label));
			o.value('', _('Keep the child routers\' setting'));
			o.value('disabled', _('Off'));
			o.value('ban', _('Block listed devices'));
			o.value('allow', _('Allow only listed devices'));
			o.optional = true;
			if (five)
				o.depends('wifi_dual_band_sync', '0');
		}
		profile('24g', _('2.4 GHz'));
		profile('5g', _('5 GHz'));

		o = s.taboption('policy', form.Flag, 'band_steer', _('Band steering'));
		o.optional = true;
		o.default = '1';
		o.description = _('Move capable devices to the better band (factory default: on while both bands share a name). The factory turns this off together with the dual-band switch; here it is a separate choice.');

		o = s.taboption('policy', form.Value, 'band_steer_rssi_24g', _('Band steering threshold, 2.4 GHz (dBm)'));
		o.datatype = 'range(-110,-30)';
		o.default = '-60';
		o.optional = true;
		o.description = _('-110 to -30 dBm, factory default -60. The controller\'s own decision threshold; set both bands or neither.');
		o = s.taboption('policy', form.Value, 'band_steer_rssi_5g', _('Band steering threshold, 5 GHz (dBm)'));
		o.datatype = 'range(-110,-30)';
		o.default = '-110';
		o.optional = true;
		o.description = _('-110 to -30 dBm, factory default -110. Set both bands or neither.');

		o = s.taboption('policy', form.Value, 'roam_rssi_24g', _('Roaming threshold, 2.4 GHz (dBm)'));
		o.datatype = 'range(-110,-30)';
		o.default = '-60';
		o.optional = true;
		o.description = _('-110 to -30 dBm, factory default -60 (the factory page allows -110 to -40). A device weaker than this is moved to a better router. Sent to the child routers; some models, the Z506 among them, ignore it. Set both bands or neither.');
		o = s.taboption('policy', form.Value, 'roam_rssi_5g', _('Roaming threshold, 5 GHz (dBm)'));
		o.datatype = 'range(-110,-30)';
		o.default = '-70';
		o.optional = true;
		o.description = _('-110 to -30 dBm, factory default -70. Set both bands or neither.');

		o = s.taboption('policy', form.Flag, 'roam_force', _('Force roaming on weak signal'));
		o.optional = true;
		o.default = '0';
		o.description = _('Kick devices that stay on a weak signal (factory default: off).');

		o = s.taboption('policy', form.Value, 'ban_minutes', _('Blocked devices stay off for (minutes)'));
		o.datatype = 'range(0,1440)';
		o.optional = true;
		o.description = _('0 to 1440 minutes, 0 = until the entry is removed (the default). Applies to every entry of the list below, which is blocked for that long after the rule was first sent; the time is not extended by later saves.');

		var b = m.section(ReportingGridSection, 'zte_mesh_bss', _('Extra Wi-Fi networks'),
			_('Guest, spare and hidden backhaul networks of the child routers. The slots of one band must be 1, 2, 3 in order without a gap: removing slot 1 while slot 2 exists is refused. Adding or removing a slot makes the child routers drop out of the topology for up to a minute; switching one on or off does not.'));
		b.addremove = true;
		b.anonymous = true;
		b.nodescriptions = true;

		o = b.option(form.ListValue, 'band', _('Band'));
		o.value('24g', '2.4 GHz');
		o.value('5g', '5 GHz');
		o.default = '24g';
		o.rmempty = false;

		o = b.option(form.ListValue, 'slot', _('Slot'));
		o.value('1', _('1 (guest)'));
		o.value('2', _('2 (spare)'));
		o.value('3', _('3 (hidden backhaul)'));
		o.default = '1';
		o.rmempty = false;

		o = b.option(form.Flag, 'enabled', _('Enabled'));
		o.default = '1';
		o.rmempty = false;

		o = b.option(form.Value, 'ssid', _('Network name (SSID)'));
		o.datatype = 'and(minlength(1),maxlength(31))';
		o.rmempty = false;
		o.placeholder = _('1 to 31 characters');
		o.description = _('1 to 31 characters. Required.');

		o = b.option(form.Value, 'key', _('Password'));
		o.password = true;
		o.datatype = 'and(minlength(8),maxlength(63))';
		o.rmempty = false;
		o.modalonly = true;
		o.placeholder = _('8 to 63 characters');
		o.description = _('8 to 63 characters. Required, also for an open network (it is kept for when security is switched on).');

		o = b.option(form.Flag, 'hidden', _('Hidden network name'));
		o.optional = true;
		o.modalonly = true;

		o = b.option(form.ListValue, 'security', _('Security'));
		o.value('psk2', 'WPA2-PSK');
		o.value('psk-mixed', 'WPA/WPA2-PSK');
		o.value('sae-mixed', 'WPA2/WPA3');
		o.value('sae', 'WPA3');
		o.value('none', _('None (open network)'));
		o.default = 'psk2';
		o.optional = true;
		o.modalonly = true;

		o = b.option(form.Value, 'access_minutes', _('Switch off after (minutes)'));
		o.datatype = 'range(1,10080)';
		o.optional = true;
		o.modalonly = true;
		o.placeholder = _('Empty: no time limit');
		o.description = _('1 to 10080 minutes (7 days); empty = no time limit. Guest time limit: the network is switched off this long after it came on. Switching it off and on again, or changing this value, restarts the countdown. Not kept across a controller restart.');

		o = b.option(form.Flag, 'force_wifi5', _('Wi-Fi 5 compatible network'));
		o.optional = true;
		o.modalonly = true;
		o.description = _('Announces the network as Wi-Fi 5 only to child routers that understand it; many models ignore it.');

		o = b.option(form.ListValue, 'acl_policy', _('MAC filter policy'));
		o.value('', _('Keep the child routers\' setting'));
		o.value('disabled', _('Off'));
		o.value('ban', _('Block listed devices'));
		o.value('allow', _('Allow only listed devices'));
		o.optional = true;
		o.modalonly = true;

		var nm = m.section(ReportingGridSection, 'zte_mesh_node', _('Router names'),
			_('Names for child routers, shown in the list above instead of the name the router reports. They are kept on this router only; the child routers keep their own names.'));
		nm.addremove = true;
		nm.anonymous = true;
		nm.nodescriptions = true;

		o = nm.option(form.Value, 'mac', _('MAC address'));
		o.datatype = 'macaddr';
		o.rmempty = false;
		o.placeholder = 'aa:bb:cc:dd:ee:ff';
		o.description = _('The MAC address shown for the router in the list above.');

		o = nm.option(form.Value, 'name', _('Name'));
		o.datatype = 'and(minlength(1),maxlength(63))';
		o.rmempty = false;
		o.description = _('1 to 63 characters.');

		var t = m.section(ReportingGridSection, 'zte_mesh_timer', _('Wi-Fi schedule'),
			_('Switch Wi-Fi off during these windows on every child router. "All Wi-Fi" turns off the main networks and the guest and spare networks (the hidden backhaul network stays on); "Guest network" turns off guest network 1 only. A window that ends before it starts runs past midnight. Times are in the router\'s time zone (the zone in effect when the settings were saved).'));
		t.addremove = true;
		t.anonymous = true;
		t.nodescriptions = true;

		function timeOfDay(section_id, value) {
			return /^([01][0-9]|2[0-3]):[0-5][0-9]$/.test(value) || _('Use the form HH:MM, for example 23:00');
		}

		o = t.option(form.Value, 'start', _('Off from'));
		o.default = '23:00';
		o.rmempty = false;
		o.validate = timeOfDay;
		o.description = _('HH:MM, 00:00 to 23:59.');

		o = t.option(form.Value, 'end', _('Back on at'));
		o.default = '07:00';
		o.rmempty = false;
		o.validate = timeOfDay;
		o.description = _('HH:MM, 00:00 to 23:59. Earlier than the start: the window runs past midnight.');

		o = t.option(form.MultiValue, 'days', _('On the days that start on'));
		o.value('mon', _('Mon'));
		o.value('tue', _('Tue'));
		o.value('wed', _('Wed'));
		o.value('thu', _('Thu'));
		o.value('fri', _('Fri'));
		o.value('sat', _('Sat'));
		o.value('sun', _('Sun'));
		o.default = 'mon tue wed thu fri sat sun';
		o.rmempty = false;

		o = t.option(form.ListValue, 'scope', _('Switch off'));
		o.value('all', _('All Wi-Fi'));
		o.value('guest', _('Guest network'));
		o.default = 'all';

		var a = m.section(ReportingGridSection, 'zte_mesh_acl', _('MAC filter list'),
			_('Up to 16 devices. A rule applies to the main network of the chosen band or to one of its extra networks; the policy above decides whether listed devices are blocked or the only ones allowed.'));
		a.addremove = true;
		a.anonymous = true;
		a.nodescriptions = true;

		o = a.option(form.Value, 'mac', _('MAC address'));
		o.datatype = 'macaddr';
		o.rmempty = false;
		o.placeholder = 'aa:bb:cc:dd:ee:ff';
		o.description = _('The address the device uses now: phones often use a private MAC per network.');

		o = a.option(form.Value, 'name', _('Name'));
		o.datatype = 'maxlength(20)';
		o.optional = true;
		o.description = _('Up to 20 characters: letters, digits and _ . -. Empty: acl1, acl2 and so on.');
		o.validate = function(section_id, value) {
			if (value === '' || /^[A-Za-z0-9_.-]+$/.test(value))
				return true;
			return _('Only letters, digits and _ . - are allowed');
		};

		o = a.option(form.ListValue, 'band', _('Band'));
		o.value('', _('Both'));
		o.value('24g', '2.4 GHz');
		o.value('5g', '5 GHz');
		o.optional = true;

		o = a.option(form.ListValue, 'bss', _('Network'));
		o.value('', _('Main network'));
		o.value('1', _('Extra network 1'));
		o.value('2', _('Extra network 2'));
		o.value('3', _('Extra network 3'));
		o.optional = true;

		/* Client actions are sent as they are: the state machine in the controller decides how often, and it keeps one
		 * row per address, so the same device twice is refused by the init script rather than silently merged. */
		var c = m.section(ReportingGridSection, 'zte_mesh_client', _('Blocked and kicked devices'),
			_('Up to 8 devices. "Block" keeps a device off the Wi-Fi until the time below runs out or the entry is removed; the controller repeats the rule so a child router that reboots does not forget it. "Kick once" asks the device to move to the better access point and gives up after a few tries. Both need the child routers to support it; the factory default leaves it off, so a device that does not move is not necessarily an error.'));
		c.addremove = true;
		c.anonymous = true;
		c.nodescriptions = true;

		o = c.option(form.Value, 'mac', _('MAC address'));
		o.datatype = 'macaddr';
		o.rmempty = false;
		o.placeholder = 'aa:bb:cc:dd:ee:ff';
		o.description = _('The address the device uses now: phones often use a private MAC per network.');

		o = c.option(form.ListValue, 'action', _('Action'));
		o.value('ban', _('Block'));
		o.value('kick', _('Kick once'));
		o.default = 'ban';
		o.rmempty = false;

		}

		var restart = E('button', {
			'class': 'btn cbi-button cbi-button-action',
			'click': ui.createHandlerFn(this, function() {
				return callRestart().then(function() {
					ui.addNotification(null, E('p', _('The controller is restarting.')), 'info');
				});
			})
		}, _('Restart controller'));

		return m.render().then(function(formNode) {
			var sections = [
				E('div', { 'class': 'cbi-section' }, [
					E('h3', _('Status')), statusBox,
					E('div', { 'class': 'cbi-page-actions', 'style': 'text-align:left' }, restart)
				])
			];

			/* dom.create() calls itself on every element of an array and takes html.charCodeAt(0) on what it
			 * gets, so a null slot in this list is a TypeError ("Cannot read properties of null (reading
			 * 'charCodeAt')") and the whole page comes up empty: exactly what happened on a child, where this
			 * section does not exist. The section is left out of the list, not nulled out of it. */
			if (!child)
				sections.push(E('div', { 'class': 'cbi-section' }, [ E('h3', _('Child routers')), nodesBox ]));

			/* Read-only and local: no switch, nothing to save. It is a separate section rather than a column of the
			 * child router table because the two answer different questions -- that table says how many stations a
			 * router reports, this one says which devices this router itself can name -- and because the device list
			 * will grow a "which router it is attached to" column once the controller reports associations, without
			 * the child router table changing shape. */
			sections.push(E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Devices')),
				E('div', { 'class': 'small' }, _('What this router can see itself: its DHCP leases, the neighbour table and the stations on its own access points. A device attached to another router of the mesh is not here, because the controller learns a station count per network and not which stations those are.')),
				devicesBox
			]));

			sections.push(formNode);
			return E(sections);
		});
	}
});
