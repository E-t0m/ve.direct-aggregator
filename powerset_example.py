#!/usr/bin/python3
# -*- coding: utf-8 -*-
#
# VE.Direct Aggregator — Powerset Interface
# Python example code
#
# Demonstrates usage of the SET and HEX command interface
# of the multiple_powerset firmware.
#
# No dependencies beyond pyserial.

from serial import Serial
from threading import Thread, Event
from queue import Queue, Empty
from time import time, sleep


# ── connection ────────────────────────────────────────────────────────────────

PORT = '/dev/ttyUSB0'   # adjust to your device
BAUD = 19200            # must match BAUD_OUT in firmware


# ── simple blocking interface ─────────────────────────────────────────────────

class PowersetClient:
	"""
	Blocking interface to the multiple_powerset firmware.

	Opens the serial port, sends a command and waits for all replies
	within a timeout. Returns a dict of {pid: reply_line}.

	Shares the port with the VE.Direct text stream — callers should
	not hold the port open between commands if they also read the
	text stream on the same connection.
	"""

	def __init__(self, port=PORT, baud=BAUD, timeout=3.0):
		self.port    = port
		self.baud    = baud
		self.timeout = timeout
		self._ser    = None

	def open(self):
		self._ser = Serial(self.port, self.baud, timeout=1)

	def close(self):
		if self._ser:
			self._ser.close()
			self._ser = None

	def __enter__(self):
		self.open()
		return self

	def __exit__(self, *_):
		self.close()

	def _send(self, cmd):
		"""Send a command line and collect replies until idle."""
		if not cmd.endswith('\n'):
			cmd += '\n'
		self._ser.write(cmd.encode())
		self._ser.flush()

		replies = {}
		deadline = time() + self.timeout
		while time() < deadline:
			if self._ser.in_waiting:
				line = self._ser.readline().decode(errors='replace').strip()
				if not line:
					continue
				# collect OK / ERR / HEX_REPLY lines
				if line.startswith(('OK ', 'ERR ', 'HEX_REPLY ')):
					pid = line.split()[1]
					replies[pid] = line
				# stop when port is quiet
				if not self._ser.in_waiting:
					break
			else:
				sleep(0.02)
		return replies

	def set_watts(self, pid, watts):
		"""
		Limit charge power of one charger or all.

		pid   — PID string e.g. '0xA053', or 'ALL'
		watts — target power in watts (int, 0 = stop charging)

		Returns dict {pid: reply_line}
		"""
		return self._send(f'SET {pid} {int(watts)}')

	def hex_cmd(self, pid, hex_str):
		"""
		Send arbitrary VE.Direct HEX string to one charger or all.

		pid     — PID string or 'ALL'
		hex_str — complete HEX line e.g. ':154\n'

		Returns dict {pid: reply_line}
		"""
		hex_str = hex_str.strip()
		return self._send(f'HEX {pid} {hex_str}')

	def restore_text_mode(self, pid='ALL'):
		"""Switch charger(s) back to VE.Direct text mode."""
		return self.hex_cmd(pid, ':154')


# ── async interface ───────────────────────────────────────────────────────────

class PowersetAsync:
	"""
	Non-blocking interface — commands are queued and sent by a
	background thread. Replies are placed in reply_queue.

	Suitable for integration into a control loop that must not block.
	"""

	def __init__(self, port=PORT, baud=BAUD):
		self.reply_queue = Queue()
		self._cmd_queue  = Queue()
		self._stop       = Event()
		self._thread     = Thread(target=self._run, args=(port, baud), daemon=True)
		self._thread.start()

	def stop(self):
		self._stop.set()
		self._thread.join()

	def set_watts(self, pid, watts):
		"""Queue a SET command. Returns immediately."""
		self._cmd_queue.put(f'SET {pid} {int(watts)}')

	def hex_cmd(self, pid, hex_str):
		"""Queue a HEX command. Returns immediately."""
		self._cmd_queue.put(f'HEX {pid} {hex_str.strip()}')

	def restore_text_mode(self, pid='ALL'):
		self.hex_cmd(pid, ':154')

	def _run(self, port, baud):
		ser = None
		while not self._stop.is_set():
			# open / reopen port
			try:
				if ser is None:
					ser = Serial(port, baud, timeout=1)
			except Exception as e:
				print(f'[powerset] open error: {e}')
				sleep(2)
				continue

			# wait for next command
			try:
				cmd = self._cmd_queue.get(timeout=1)
			except Empty:
				continue

			# send and collect replies
			try:
				if not cmd.endswith('\n'):
					cmd += '\n'
				ser.write(cmd.encode())
				ser.flush()

				deadline = time() + 3.0
				while time() < deadline:
					if ser.in_waiting:
						line = ser.readline().decode(errors='replace').strip()
						if line.startswith(('OK ', 'ERR ', 'HEX_REPLY ')):
							self.reply_queue.put(line)
						if not ser.in_waiting and self._cmd_queue.empty():
							break
					else:
						sleep(0.02)

			except Exception as e:
				print(f'[powerset] send error: {e}')
				try: ser.close()
				except Exception: pass
				ser = None


# ── examples ──────────────────────────────────────────────────────────────────

if __name__ == '__main__':

	# ── blocking usage ────────────────────────────────────────────────────────

	print('── blocking interface ──')

	with PowersetClient(PORT, BAUD) as ps:

		# limit all chargers to 1500W
		replies = ps.set_watts('ALL', 1500)
		for pid, reply in replies.items():
			print(reply)
		# OK 0xA053 1500W 58.5A
		# OK 0xA060 1500W 58.5A

		# limit a single charger
		replies = ps.set_watts('0xA053', 500)
		print(replies.get('0xA053', 'no reply'))
		# OK 0xA053 500W 19.5A

		# stop all charging
		replies = ps.set_watts('ALL', 0)
		for pid, reply in replies.items():
			print(reply)
		# OK 0xA053 0W 0.0A
		# OK 0xA060 0W 0.0A

		# send arbitrary HEX — restore text mode manually
		replies = ps.restore_text_mode('0xA053')
		print(replies.get('0xA053', 'no reply'))
		# HEX_REPLY 0xA053 :90641F

		# arbitrary GET command
		replies = ps.hex_cmd('0xA053', ':70015200A3')
		print(replies.get('0xA053', 'no reply'))
		# HEX_REPLY 0xA053 :50015200641F

	# ── non-blocking usage ────────────────────────────────────────────────────

	print('\n── async interface ──')

	ps = PowersetAsync(PORT, BAUD)

	# queue commands — return immediately
	ps.set_watts('ALL', 2000)
	ps.set_watts('0xA053', 800)

	# collect replies as they arrive
	deadline = time() + 5.0
	while time() < deadline:
		try:
			reply = ps.reply_queue.get(timeout=0.1)
			print(reply)
		except Empty:
			pass

	ps.stop()

	# ── voltage ramp example ──────────────────────────────────────────────────

	print('\n── voltage ramp ──')

	V_START   = 54.0    # V — full power below this
	V_STOP    = 57.0    # V — zero power at or above this
	MAX_WATTS = 3000    # W — total installed PV power

	def charge_limit(vbat):
		"""
		Linear ramp from MAX_WATTS at V_START to 0W at V_STOP.
		Returns None if no limiting is needed.
		"""
		if vbat <= 0:        return None
		if vbat <  V_START:  return None
		if vbat >= V_STOP:   return 0
		frac = (vbat - V_START) / (V_STOP - V_START)
		return int(MAX_WATTS * (1.0 - frac))

	# example: Vbat = 55.5V → 50% → 1500W
	for vbat in [53.0, 54.0, 55.5, 56.0, 57.0, 58.0]:
		limit = charge_limit(vbat)
		print(f'Vbat={vbat:.1f}V  →  {limit}W' if limit is not None else f'Vbat={vbat:.1f}V  →  no limit')

	# apply in a control loop:
	# with PowersetClient(PORT, BAUD) as ps:
	#     while True:
	#         vbat = read_vbat_from_somewhere()
	#         limit = charge_limit(vbat)
	#         if limit is not None:
	#             ps.set_watts('ALL', limit)
	#         sleep(1)
