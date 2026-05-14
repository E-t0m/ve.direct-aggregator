#!/usr/bin/python3
"""
vedirect_simulator.py — upstream aggregator simulator

Simulates a Teensy/Mega aggregator sending N MPPT blocks on one serial port.
Each second all N blocks are sent back-to-back, like a real upstream aggregator.

Usage:
    python3 vedirect_simulator.py /dev/ttyUSB1 3   # 3 MPPTs
    python3 vedirect_simulator.py /dev/ttyUSB1 1   # 1 MPPT

Connect TX of USB-UART adapter to RX of target Mega/Teensy port.
Set port_baud[] for that port to match BAUD in this file.
"""

import sys
import time
import serial

PORT  = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB1'
N     = int(sys.argv[2]) if len(sys.argv) > 2 else 3
BAUD  = 19200

DRIFT_PERIOD = 60.0   # seconds per full overtake cycle
# interval = 1.0 - 1/DRIFT_PERIOD so the burst overtakes once per DRIFT_PERIOD seconds

def sim_serial(i):
	return ('-' * (10 - i) + str(i + 1) + '-' * i)[:11]

DEVICES = [
	{'PID': '0xA060', 'SER#': sim_serial(i), 'FW': '174'}
	for i in range(N)
]

def make_block(dev, tick):
	v_mv = 52000 + (tick % 100) * 10
	i_ma = -50   + (tick % 20) * 5
	vpv  = 18000 + (tick % 50) * 100
	ppv  = max(0, (tick % 50) * 20)
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
		f'OR\t0x00000001',
		f'ERR\t0',
		f'LOAD\tOFF',
		f'IL\t0',
		f'H19\t0',
		f'H20\t0',
		f'H21\t99',
		f'H22\t0',
		f'H23\t0',
		f'HSDS\t0',
	]

	body       = '\r\n'.join(fields) + '\r\n'
	body_bytes = body.encode('ascii')
	prefix     = body_bytes + b'Checksum\t'
	suffix     = b'\r\n'
	cs_byte    = (256 - sum(prefix + suffix) % 256) % 256
	block      = prefix + bytes([cs_byte]) + suffix
	assert sum(block) % 256 == 0
	return block

print(f'Upstream simulator — {N} MPPT(s) on {PORT} at {BAUD} baud')
print(f'Devices: {[d["SER#"] for d in DEVICES]}')
print('Ctrl-C to stop\n')

with serial.Serial(PORT, BAUD) as ser:
	tick     = 0
	interval = 1.0 - 1.0 / DRIFT_PERIOD

	while True:
		t_start = time.time()

		for i, dev in enumerate(DEVICES):
			block = make_block(dev, tick + i * 7)
			ser.write(block)
			ser.flush()
			print(f'{i+1}', end=' ', flush=True)

		print(f'  t={int((time.time()-t_start)*1000)}ms')
		tick += 1

		elapsed = time.time() - t_start
		time.sleep(max(0, interval - elapsed))
