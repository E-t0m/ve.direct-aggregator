#!/usr/bin/python3
"""
vedirect_simulator.py — upstream aggregator simulator v2.0

Simulates a Teensy/Mega aggregator sending N MPPT blocks on one serial port.
Each second all N blocks are sent back-to-back, like a real upstream aggregator.

HEX/SET commands on the RX line cycle through three response modes per type:
  req #1 → no reply    (timeout)
  req #2 → wrong value (ERR verify)
  req #3 → correct     (OK/ACK)

The verification GET after a SET inherits the SET's response mode.

Usage:
	python3 vedirect_simulator.py /dev/ttyUSB1 3
	python3 vedirect_simulator.py /dev/ttyUSB1 1
"""

import sys
import time
import serial
import threading

PORT = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB1'
N    = int(sys.argv[2]) if len(sys.argv) > 2 else 3
BAUD = 19200

# NOTE: SET/HEX command testing works reliably only with N=1.
# With N>1, all devices share one serial port and one reply counter --
# the firmware cannot distinguish which device a reply belongs to.
# Use N=1 for sendhex_test.py; use N>1 only for block-forwarding tests.


def sim_serial(i):
	return ('-' * (10 - i) + str(i + 1) + '-' * i)[:11]


DEVICES = [
	{'PID': '0xA060', 'SER#': sim_serial(i), 'FW': '174'}
	for i in range(N)
]


# ── HEX helpers ─────────────────────────────────────────

def hex_cs(data_bytes):
	"""Sum of all frame bytes must be 0x55."""
	return (0x55 - sum(data_bytes)) & 0xFF


def _frame(t, val):
	"""type(1 hex nibble) + val_lo(2) + val_hi(2) + cs(2) — matches real MPPT format."""
	data = bytes([t, val & 0xFF, (val >> 8) & 0xFF])
	cs   = hex_cs(data)
	return f':{t:X}{val & 0xFF:02X}{(val >> 8) & 0xFF:02X}{cs:02X}\n'


def make_get_reply(val=0xAAAA):
	"""GET reply type=4 — matches real :4AAAAFD."""
	return _frame(4, val)


def make_set_reply(val):
	"""SET reply type=6."""
	return _frame(6, val)


def make_ping_reply():
	"""Ping reply — matches real :574419B."""
	data = bytes([0x05, 0x74, 0x41])
	return f':5{0x74:02X}{0x41:02X}{hex_cs(data):02X}\n'


# ── command reader ──────────────────────────────────────

_req_count     = {}   # {type_char: count}
_last_set_mode = None # mode of last SET — inherited by verification GET
_last_set_val  = None # value of last SET — returned by verification GET on correct
_sending_reply = threading.Event()


def cmd_reader(ser):
	global _last_set_mode
	buf = b''

	while True:
		chunk = ser.read(64)
		if not chunk:
			continue
		buf += chunk

		while b'\n' in buf:
			line, buf = buf.split(b'\n', 1)
			try:
				text = line.decode('ascii').strip()
			except Exception:
				continue
			if not text.startswith(':') or len(text) < 3:
				continue

			if N > 1:
				print(f'\nWARNING: N={N} -- SET/HEX reply testing unreliable with multiple devices. Use N=1.', flush=True)
				continue

			# VE.Direct HEX type: ':08...' → type='8', ':07...' → type='7'
			# type is always the second hex char (text[2])
			# except ping ':154' where text[1]='1'
			if text[1] == '1':
				key = '1'   # ping
			else:
				key = text[2]   # SET='8', GET='7'

			# verification GET after SET inherits SET mode
			if key == '7' and _last_set_mode is not None:
				mode  = _last_set_mode
				_last_set_mode = None
				count = _req_count.get(key, 0)
			else:
				_req_count[key] = _req_count.get(key, 0) + 1
				count = _req_count[key]
				mode  = (count - 1) % 3
				if key == '8':
					_last_set_mode = mode

			labels = {0: '→ (no reply)', 1: '→ wrong value', 2: '→ correct'}
			print(f'\ncmd: {text}  req#{count}  {labels[mode]}', flush=True)

			if mode == 0:
				continue

			_sending_reply.set()
			time.sleep(0.1)

			if key == '1':
				ser.write(make_ping_reply().encode())

			elif key == '7':
				get_val = _last_set_val if (_last_set_val is not None and mode == 2) else 0xAAAA
				ser.write(make_get_reply(get_val).encode())
				_last_set_val = None

			elif key == '8':
				try:
					val = int(text[9:11], 16) | (int(text[11:13], 16) << 8)
					reply_val = (val + 1) if mode == 1 else val
					_last_set_val = val   # remember for verification GET
					ser.write(make_set_reply(reply_val).encode())
				except Exception as e:
					print(f'[SET parse error: {e}]', flush=True)

			ser.flush()
			time.sleep(0.05)
			_sending_reply.clear()


# ── block builder ───────────────────────────────────────

def make_block(dev, tick):
	v_mv = 52000 + (tick % 100) * 10
	i_ma = -50   + (tick % 20)  * 5
	vpv  = 18000 + (tick % 50)  * 100
	ppv  = max(0, (tick % 50)   * 20)
	cs   = 3 if ppv > 0 else 0

	fields = [
		f'PID\t{dev["PID"]}',
		f'FW\t{dev["FW"]}',
		f'SER#\t{dev["SER#"]}',
		f'V\t{v_mv}',
		f'I\t{i_ma}',
		f'VPV\t{vpv}',
		f'PPV\t{ppv}',
		f'CS\t{cs}',
		f'MPPT\t{"2" if ppv > 0 else "0"}',
		'OR\t0x00000001',
		'ERR\t0',
		'LOAD\tOFF',
		'IL\t0',
		'H19\t0',
		'H20\t0',
		'H21\t99',
		'H22\t0',
		'H23\t0',
		'HSDS\t0',
	]

	body      = '\r\n'.join(fields) + '\r\n'
	body_b    = body.encode('ascii')
	prefix    = body_b + b'Checksum\t'
	suffix    = b'\r\n'
	cs_byte   = (256 - sum(prefix + suffix) % 256) % 256
	block     = prefix + bytes([cs_byte]) + suffix
	assert sum(block) % 256 == 0
	return block


# ── main ────────────────────────────────────────────────

print(f'Upstream simulator — {N} MPPT(s) on {PORT} at {BAUD} baud')
print(f'Devices: {[d["SER#"] for d in DEVICES]}')
print('HEX replies: timeout → wrong value → correct (rotating per type)')
print('Ctrl-C to stop\n')

DRIFT_PERIOD = 120.0
interval     = 1.0 - 1.0 / DRIFT_PERIOD

with serial.Serial(PORT, BAUD, timeout=0.05) as ser:
	threading.Thread(target=cmd_reader, args=(ser,), daemon=True).start()

	tick = 0
	while True:
		t_start = time.time()

		while _sending_reply.is_set():
			time.sleep(0.005)

		for i, dev in enumerate(DEVICES):
			block = make_block(dev, tick + i * 7)
			ser.write(block)
			ser.flush()
			print(f'{i+1}', end=' ', flush=True)

		print(f' t={int((time.time()-t_start)*1000)}ms')
		tick += 1
		elapsed = time.time() - t_start
		time.sleep(max(0, interval - elapsed))
