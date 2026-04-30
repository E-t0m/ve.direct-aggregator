#!/usr/bin/python3
# -*- coding: utf-8 -*-
#
# VE.Direct Aggregator — powerset interface examples
#
# Demonstrates SET and HEX command usage via ve_aggregator module.
# Requires: readtext_sendhex firmware on the MCU.

from ve_aggregator import VEDirect
from time import sleep, time

PORT = '/dev/ttyUSB0'
BAUD = 19200

# ── basic set/hex usage ───────────────────────────────────────────────────────

print('── basic usage ──')

with VEDirect(PORT, BAUD) as vd:

	sleep(2)   # wait for first data

	# limit all MPPT chargers to 1500W
	vd.set_watts('ALL', 1500)
	for r in vd.get_replies(timeout=3):
		print(r)
	# OK 0xA053 1500W 58.5A
	# OK 0xA060 1500W 58.5A

	# limit a single charger
	vd.set_watts('0xA053', 500)
	for r in vd.get_replies(timeout=3):
		print(r)
	# OK 0xA053 500W 19.5A

	# stop all charging
	vd.set_watts('ALL', 0)
	for r in vd.get_replies(timeout=3):
		print(r)

	# arbitrary HEX command
	vd.hex_cmd('0xA053', ':70015200A3')
	for r in vd.get_replies(timeout=2):
		print(r)
	# HEX_REPLY 0xA053 :50015200641F

	# restore text mode manually after HEX
	vd.restore_text_mode('ALL')

# ── voltage ramp ──────────────────────────────────────────────────────────────

V_START   = 54.0   # V — full power below this
V_STOP    = 57.0   # V — zero power at or above this
MAX_WATTS = 3000   # W — total installed PV power

def charge_limit(vbat):
	"""Linear ramp MAX_WATTS → 0W between V_START and V_STOP."""
	if vbat <= 0 or vbat < V_START:  return None   # no limiting needed
	if vbat >= V_STOP:               return 0
	frac = (vbat - V_START) / (V_STOP - V_START)
	return int(MAX_WATTS * (1.0 - frac))

print('\n── voltage ramp ──')
for vbat in [53.0, 54.0, 55.5, 56.0, 57.0]:
	limit = charge_limit(vbat)
	print(f'Vbat={vbat:.1f}V  →  {"no limit" if limit is None else f"{limit}W"}')

# ── control loop example ──────────────────────────────────────────────────────

print('\n── control loop (10 cycles) ──')

with VEDirect(PORT, BAUD, hysteresis_w=50) as vd:
	sleep(2)
	for _ in range(10):
		sleep(1)
		c    = vd.combined()
		vbat = c.get('Vbat', 0)
		ppv  = c.get('PPV',  0)
		ibat = c.get('I',    0)
		print(f'Vbat={vbat:.2f}V  PPV={ppv}W  I={ibat:.1f}A', end='')

		limit = charge_limit(vbat)
		if limit is not None:
			vd.set_watts('ALL', limit)
			print(f'  → SET ALL {limit}W', end='')
			for r in vd.drain_replies():
				print(f'  {r}', end='')
		print()
