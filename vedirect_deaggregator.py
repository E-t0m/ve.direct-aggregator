#!/usr/bin/python3
"""
vedirect_deaggregator.py — VE.Direct De-Aggregator  v2.0

Reads the aggregated VE.Direct stream from a readtext/readtext_sendhex
firmware (Mega 2560 or Teensy 4.1) and splits it into one virtual serial
port per device, identified by SER#.

Each virtual port is a socat pty pair — one end is managed here, the other
end (/dev/pts/N) can be registered as a VE.Direct device in Venus OS or
any other software that expects a plain serial VE.Direct stream.

Requirements:
	pip install pyserial --break-system-packages
	apt install socat

Usage:
	python3 vedirect_deaggregator.py [options]
	python3 vedirect_deaggregator.py --port /dev/ttyACM3 --baud 19200

Venus OS integration (on a Raspberry Pi running Venus OS):
	# run de-aggregator as a service, then register each /dev/pts/N:
	# edit /etc/venus/dbus-fronius.conf or use VRM → Settings → I/O →
	# VE.Direct devices → add /dev/pts/N for each MPPT
"""

import argparse
import os
import pty
import sys
import threading
import time
from collections import defaultdict
from datetime import datetime

import serial


VERSION = '2.0'

# ── helpers ───────────────────────────────────────────────────────────

def fmt():
	return datetime.now().strftime('%H:%M:%S.%f')[:-3]

def log(msg):
	print(f'{fmt()}  {msg}', flush=True)


# ── virtual port ──────────────────────────────────────────────────────

class VirtualPort:
	"""
	One socat pty pair — master fd stays here, slave path is exposed
	to Venus OS or other consumers.
	"""

	def __init__(self, ser, label):
		self.ser        = ser       # SER# of the device
		self.label      = label     # human-readable label
		self.master_fd  = None
		self.slave_path = None
		self.lock       = threading.Lock()
		self._open()

	def _open(self):
		master_fd, slave_fd = pty.openpty()
		self.master_fd  = master_fd
		self.slave_path = os.ttyname(slave_fd)
		os.close(slave_fd)   # consumer opens the slave; we keep master
		log(f'virtual port: {self.label} ({self.ser}) → {self.slave_path}')

	def write(self, data: bytes):
		with self.lock:
			try:
				os.write(self.master_fd, data)
			except OSError:
				pass   # consumer disconnected — ignore

	def close(self):
		try:
			os.close(self.master_fd)
		except OSError:
			pass


# ── block parser ──────────────────────────────────────────────────────

class BlockParser:
	"""
	Parses a VE.Direct text stream and emits complete blocks.
	A block starts with PID and ends with Checksum.
	"""

	def __init__(self):
		self._buf  = b''
		self._block = b''

	def feed(self, data: bytes):
		"""Feed bytes; yields (fields_dict, raw_block_bytes) per complete block."""
		self._buf += data
		while b'\n' in self._buf:
			line_b, self._buf = self._buf.split(b'\n', 1)
			line_b = line_b.rstrip(b'\r')
			line   = line_b.decode('latin1', errors='replace')

			# discard HEX frames, ALIVE, firmware replies
			if line.startswith(':') or line in ('ALIVE', '') or \
			   line.startswith('OK ') or line.startswith('ERR ') or \
			   line.startswith('HEX_REPLY ') or line.startswith('READTEXT ') or \
			   line.startswith('SENDHEX '):
				if self._block:
					self._block = b''   # discard partial block on HEX injection
				continue

			self._block += line_b + b'\r\n'

			if line.startswith('Checksum\t'):
				raw   = self._block
				block = self._block
				self._block = b''
				fields = self._parse_fields(raw)
				yield fields, raw

	@staticmethod
	def _parse_fields(raw: bytes) -> dict:
		fields = {}
		for line in raw.split(b'\n'):
			line = line.rstrip(b'\r')
			if b'\t' in line:
				k, _, v = line.partition(b'\t')
				try:
					fields[k.decode('ascii')] = v.decode('latin1')
				except Exception:
					pass
		return fields


# ── de-aggregator ─────────────────────────────────────────────────────

class DeAggregator:
	"""
	Reads the upstream aggregated stream, routes each block to the
	correct VirtualPort by SER#.
	"""

	def __init__(self, port, baud, label_map=None, max_devices=16):
		self.port        = port
		self.baud        = baud
		self.label_map   = label_map or {}
		self.max_devices = max_devices

		self._vports  = {}          # ser → VirtualPort
		self._order   = []          # insertion order (SER#)
		self._stats   = defaultdict(int)
		self._parser  = BlockParser()
		self._stop    = threading.Event()
		self._lock    = threading.Lock()

	def _get_or_create(self, ser):
		key = ser
		if not key:
			return None
		with self._lock:
			if key not in self._vports:
				if len(self._vports) >= self.max_devices:
					log(f'max devices ({self.max_devices}) reached — ignoring {key}')
					return None
				label = self.label_map.get(key, key)
				vp = VirtualPort(key, label)
				self._vports[key] = vp
				self._order.append(key)
				self._print_map()
			return self._vports[key]

	def _print_map(self):
		print('\n─── virtual port map ───────────────────────────────────')
		for key in self._order:
			vp = self._vports[key]
			print(f'  {vp.slave_path}  ←→  {vp.label}  ({vp.ser})')
		print('────────────────────────────────────────────────────────\n',
		      flush=True)

	def print_stats(self):
		print(f'\n─── block counts ───')
		for key in self._order:
			print(f'  {key}: {self._stats[key]} blocks')
		print()

	def run(self):
		log(f've_deaggregator v{VERSION} — opening {self.port} at {self.baud} baud')
		while not self._stop.is_set():
			try:
				with serial.Serial(self.port, self.baud, timeout=0.1) as ser:
					log(f'connected — waiting for blocks...')
					while not self._stop.is_set():
						data = ser.read(512)
						if not data:
							continue
						for fields, raw in self._parser.feed(data):
							device_ser = fields.get('SER#', '').strip()
							device_pid = fields.get('PID', '').strip()
							vp = self._get_or_create(device_ser)
							if vp:
								vp.write(raw)
								self._stats[vp.ser] += 1
			except serial.SerialException as e:
				log(f'serial error: {e} — retrying in 3s')
				time.sleep(3)

	def stop(self):
		self._stop.set()
		with self._lock:
			for vp in self._vports.values():
				vp.close()


# ── Venus OS udev rule generator ──────────────────────────────────────

def print_venus_hints(vports: dict):
	print('\n─── Venus OS integration ───────────────────────────────────')
	print('Add each virtual port as a VE.Direct device in Venus OS:')
	print()
	print('Option A — VRM GUI:')
	print('  Settings → I/O → VE.Direct ports → Add → enter path')
	print()
	print('Option B — command line on Venus OS RPi:')
	for key, vp in vports.items():
		print(f'  ln -sf {vp.slave_path} /dev/ttyVE_{key[:8]}')
	print()
	print('Option C — /etc/venus/dbus-fronius.conf')
	print('  (add each slave_path as an additional VE.Direct port)')
	print('────────────────────────────────────────────────────────────\n')


# ── main ──────────────────────────────────────────────────────────────

def main():
	p = argparse.ArgumentParser(description='VE.Direct De-Aggregator v2.0')
	p.add_argument('--port',   '-p', default='/dev/ttyACM3',
	               help='Aggregator serial port (default: /dev/ttyACM3)')
	p.add_argument('--baud',   '-b', type=int, default=19200,
	               help='Baud rate (default: 19200)')
	p.add_argument('--max',    '-n', type=int, default=16,
	               help='Maximum number of virtual ports (default: 16)')
	p.add_argument('--label',  '-l', action='append', default=[],
	               metavar='SER=LABEL',
	               help='Custom label for a device (e.g. HQ2529K6QK4=Roof)')
	args = p.parse_args()

	label_map = {}
	for entry in args.label:
		if '=' in entry:
			k, _, v = entry.partition('=')
			label_map[k.strip()] = v.strip()

	deagg = DeAggregator(
		port        = args.port,
		baud        = args.baud,
		label_map   = label_map,
		max_devices = args.max,
	)

	t = threading.Thread(target=deagg.run, daemon=True)
	t.start()

	try:
		while True:
			time.sleep(30)
			deagg.print_stats()
	except KeyboardInterrupt:
		print('\nshutting down...')
		deagg.stop()
		deagg.print_stats()


if __name__ == '__main__':
	main()
