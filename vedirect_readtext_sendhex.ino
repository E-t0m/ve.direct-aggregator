// VE.Direct readtext_sendhex -- v2.0 -- Arduino Mega 2560
//
// Aggregates N VE.Direct text streams (Serial1..3) and exposes a
// command interface for SET and HEX operations.
//
// Serial1/2/3 RX/TX <-> MPPT devices (19200 baud)
// Serial0 TX -> aggregated output
// Serial0 RX <- commands from ve_aggregator.py
//
// Commands:
//   SET <SER#|PID|ALL> <watts>\n  -> OK/ERR per device on Serial0 TX
//   HEX <SER#|PID|ALL> <hexstr>\n -> HEX_REPLY/ERR per device on Serial0 TX
//   RESET\n                        -> propagate to downstream, then reset MCU
//   WHO\n                          -> READTEXT Mega2560 N=<N>\n
//
// Routing: SER# primary, PID fallback, cascade route table for upstream
// aggregators. Blocks dropped silently if TX queue full.
//
// HEX frames injected by devices into the text stream are stripped
// transparently (line start, mid-line, after Checksum byte).
// Devices return to text mode on their own after HEX commands.
//
// Optional: DS18B20 1-Wire temperature sensor(s) on TEMP_PIN.
// Each sensor appears as a pseudo VE.Direct device block in the stream.
// Install libraries: OneWire, DallasTemperature (Library Manager).
// Wiring: VCC -> 5V, GND -> GND, DATA -> TEMP_PIN, 4.7k pull-up 5V->DATA.
//
// Output consumed by ve_aggregator.py and/or vedirect_deaggregator.py.

#define SERIAL_RX_BUFFER_SIZE 512
#define ALIVE_TIMEOUT   10000UL
#define BAUD_VEDIRECT   19200
#define BAUD_UPSTREAM   115200
#define BAUD_OUT        19200
#define BUF_SIZE        200
#define N               3
#define Q_SIZE          12
#define CMD_BUF_SIZE    64
#define HEX_TIMEOUT     400
#define REG_MAX_CURRENT 0x2015
#define VBAT_FALLBACK   24.0f
#define MAX_ROUTES      12
#define PID_TIMEOUT     10000UL

// DS18B20 1-Wire temperature sensor
// Set TEMP_PIN to the digital pin the DATA line is connected to.
// Set TEMP_ENABLE to 0 to disable temperature sensing entirely.
#define TEMP_ENABLE     1
#define TEMP_PIN        2
#define TEMP_INTERVAL   5000UL   // read interval in ms (DS18B20 needs ~750ms per conversion)

#if TEMP_ENABLE
#include <avr/wdt.h>
#include <OneWire.h>
#include <DallasTemperature.h>
OneWire            temp_wire(TEMP_PIN);
DallasTemperature  temp_sensors(&temp_wire);
unsigned long      temp_last  = 0;
int                temp_count = 0;   // number of sensors found
#endif

#if BUF_SIZE > SERIAL_RX_BUFFER_SIZE
#error "BUF_SIZE exceeds SERIAL_RX_BUFFER_SIZE ? increase hardware RX buffer"
#endif

HardwareSerial* ports[N] = {&Serial1, &Serial2, &Serial3};

// per-port input baud ? change to BAUD_UPSTREAM for cascade inputs
uint32_t port_baud[N] = {BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_VEDIRECT};

char  rx_buf[N][BUF_SIZE];
int   rx_len[N]     = {0, 0, 0};
bool  hex_busy[N]   = {false, false, false};
bool  hex_skip[N]   = {false, false, false};

char  known_ser[N][16];
bool  dev_known[N]             = {false, false, false};
unsigned long dev_last_seen[N] = {0, 0, 0};
float known_vbat[N]            = {VBAT_FALLBACK, VBAT_FALLBACK, VBAT_FALLBACK};

char  route_ser[N][MAX_ROUTES][16];
unsigned long route_last_seen[N][MAX_ROUTES];
int   route_cnt[N] = {0, 0, 0};

char          cmd_buf[CMD_BUF_SIZE];
int           cmd_len  = 0;
unsigned long last_send = 0;

// -- circular TX queue -------------------------------------------------
char q_buf[Q_SIZE][BUF_SIZE];
int  q_len[Q_SIZE] = {0};
int  q_head = 0;
int  q_tail = 0;

inline bool q_full()  { return ((q_tail + 1) % Q_SIZE) == q_head; }
inline bool q_empty() { return q_head == q_tail; }

bool q_push(const char* data, int len) {
	if (q_full() || len > BUF_SIZE) return false;
	memcpy(q_buf[q_tail], data, len);
	q_len[q_tail] = len;
	q_tail = (q_tail + 1) % Q_SIZE;
	return true;
}

int out_pos = 0;

// =======================================================================
// HEX protocol helpers
// =======================================================================

uint8_t hex_checksum(const char* s) {
	uint8_t sum = 0;
	while (s[0] && s[1]) {
		uint8_t hi = (s[0] >= 'A') ? s[0]-'A'+10 : s[0]-'0';
		uint8_t lo = (s[1] >= 'A') ? s[1]-'A'+10 : s[1]-'0';
		sum += (hi << 4) | lo;
		s += 2;
	}
	return (0x55 - sum) & 0xFF;
}

void send_hex_set(HardwareSerial* port, uint16_t reg, uint16_t value) {
	char data[16];
	sprintf(data, "08%02X%02X00%02X%02X",
		(uint8_t)(reg & 0xFF), (uint8_t)(reg >> 8),
		(uint8_t)(value & 0xFF), (uint8_t)(value >> 8));
	char msg[24];
	sprintf(msg, ":%s%02X\n", data, hex_checksum(data));
	port->print(msg);
}

void send_hex_get(HardwareSerial* port, uint16_t reg) {
	char data[12];
	sprintf(data, "07%02X%02X00", (uint8_t)(reg & 0xFF), (uint8_t)(reg >> 8));
	char msg[16];
	sprintf(msg, ":%s%02X\n", data, hex_checksum(data));
	port->print(msg);
}

// forward declarations
void reset_rx(int idx);
void read_device(int idx);

void send_text_mode(HardwareSerial* port) {
		for (int i = 0; i < N; i++)
		if (ports[i] == port) { hex_skip[i] = false; reset_rx(i); }
}

bool wait_hex_reply(HardwareSerial* port, char* rbuf, int rsize) {
	unsigned long t0 = millis();
	int pos = 0;
	while (millis() - t0 < (unsigned long)HEX_TIMEOUT) {
		// service other ports to prevent HW buffer overflow
		// safe: read_device() uses line_start() ? no stored offset to corrupt
		for (int i = 0; i < N; i++)
			if (ports[i] != port) read_device(i);
		if (port->available()) {
			char c = port->read();
			if (c == '\r') continue;
			if (pos == 0 && c != ':') continue;
			if (pos < rsize - 1) rbuf[pos++] = c;
			if (c == '\n') {
				rbuf[pos] = '\0';
				return true;
			}
		}
	}
	rbuf[0] = '\0';
	return false;
}

uint16_t parse_hex_value(const char* reply) {
	// VE.Direct GET/SET reply: :<type><val_lo><val_hi><cs>\n  (8 chars + newline)
	// e.g. :4AAAAFD ? type=4, val=0xAAAA, cs=FD
	// minimum valid: 1(:) + 1(type) + 4(val) + 2(cs) = 8 chars before \n
	if (!reply || reply[0] != ':' || strlen(reply) < 8) return 0xFFFF;
	const char* p = reply + 2;   // skip ':' and type byte
	char slo[3] = {p[0], p[1], 0};
	char shi[3] = {p[2], p[3], 0};
	return (uint16_t)(((uint8_t)strtol(shi, NULL, 16) << 8) | (uint8_t)strtol(slo, NULL, 16));
}

// =======================================================================
// SET / HEX execution
// =======================================================================

void exec_set(int idx, uint32_t watts) {
	HardwareSerial* port = ports[idx];
	const char* id = known_ser[idx][0] ? known_ser[idx] : "?";
	char reply[64];
	hex_busy[idx] = true;

	float    vbat     = (known_vbat[idx] > 1.0f) ? known_vbat[idx] : VBAT_FALLBACK;
	float    amps     = (float)watts / vbat;
	uint16_t deciamps = (uint16_t)(amps * 10.0f + 0.5f);

	send_hex_set(port, REG_MAX_CURRENT, deciamps);

	if (!wait_hex_reply(port, reply, sizeof(reply))) {
		char msg[128]; sprintf(msg, "ERR %s timeout\n", id);
		Serial.print(msg); send_text_mode(port);
		hex_busy[idx] = false; return;
	}
	send_hex_get(port, REG_MAX_CURRENT);
	if (!wait_hex_reply(port, reply, sizeof(reply))) {
		char msg[128]; sprintf(msg, "ERR %s timeout\n", id);
		Serial.print(msg); send_text_mode(port);
		hex_busy[idx] = false; return;
	}

	uint16_t readback = parse_hex_value(reply);
	char msg[128];
	if (readback == deciamps) {
		char abuf[12]; dtostrf(deciamps / 10.0f, 4, 1, abuf);
		sprintf(msg, "OK %s %luW %sA\n", id, watts, abuf);
	} else {
		char as[12]; dtostrf(deciamps / 10.0f, 4, 1, as);
		char ar[12]; dtostrf(readback / 10.0f, 4, 1, ar);
		sprintf(msg, "ERR %s verify set=%sA rb=%sA\n", id, as, ar);
	}
	send_text_mode(port);
	hex_busy[idx] = false;
	Serial.print(msg);
}

void exec_hex(int idx, const char* hex_str) {
	if (strlen(hex_str) == 0) return;   // nothing to send
	HardwareSerial* port = ports[idx];
	const char* id = known_ser[idx][0] ? known_ser[idx] : "?";
	char reply[64];
	hex_busy[idx] = true;

	port->print(hex_str);
	if (hex_str[strlen(hex_str)-1] != '\n') port->print('\n');

	if (!wait_hex_reply(port, reply, sizeof(reply))) {
		char msg[128]; sprintf(msg, "ERR %s timeout\n", id);
		Serial.print(msg); send_text_mode(port);
		hex_busy[idx] = false; return;
	}
	char msg[128];
	sprintf(msg, "HEX_REPLY %s %s", id, reply);
	send_text_mode(port);   	hex_busy[idx] = false;
	Serial.print(msg);
}

// learn route: remember which port a SER# was seen on
void route_learn(int port_idx, const char* pid) {
	unsigned long now = millis();
	// search existing route
	for (int j = 0; j < route_cnt[port_idx]; j++) {
		if (strcmp(route_ser[port_idx][j], pid) == 0) {
			route_last_seen[port_idx][j] = now;
			return;
		}
	}
	// new SER# -- find free or oldest slot
	int slot = route_cnt[port_idx];
	if (slot >= MAX_ROUTES) {
		// evict oldest
		unsigned long oldest = route_last_seen[port_idx][0];
		slot = 0;
		for (int j = 1; j < MAX_ROUTES; j++) {
			if (route_last_seen[port_idx][j] < oldest) {
				oldest = route_last_seen[port_idx][j];
				slot = j;
			}
		}
	} else {
		route_cnt[port_idx]++;
	}
	strncpy(route_ser[port_idx][slot], pid, 15);
	route_ser[port_idx][slot][15] = '\0';
	route_last_seen[port_idx][slot] = now;
}

// find which port a SER# was last seen on (-1 if unknown or expired)
int route_find(const char* pid) {
	unsigned long now = millis();
	for (int i = 0; i < N; i++) {
		for (int j = 0; j < route_cnt[i]; j++) {
			if (strcmp(route_ser[i][j], pid) == 0) {
				if (now - route_last_seen[i][j] > PID_TIMEOUT) return -1;  // expired
				return i;
			}
		}
	}
	return -1;  // unknown
}

// forward command on all input ports ? used when route not yet learned
void forward_all(const char* fwd) {
	for (int i = 0; i < N; i++) {
		ports[i]->print(fwd);
		ports[i]->print('\n');
	}
}

void process_cmd(char* line) {
	if (strcmp(line, "RESET") == 0) {
		forward_all("RESET");   // propagate to all downstream devices
		delay(50);              // let transmission complete
		wdt_enable(WDTO_15MS);
		while(1);               // watchdog fires and resets MCU
	}
	if (strcmp(line, "WHO") == 0) {
		Serial.print("SENDHEX Mega2560 N=");
		Serial.print(N);
		Serial.print("\n");
		return;
	}
	bool is_set = (strncmp(line, "SET ", 4) == 0);
	bool is_hex = (strncmp(line, "HEX ", 4) == 0);
	if (!is_set && !is_hex) return;

	char* p = line + 4;
	char ser_str[20]; int ti = 0;
	while (*p && *p != ' ' && ti < 19) ser_str[ti++] = *p++;
	ser_str[ti] = '\0';
	if (*p == ' ') p++;

	// expire PIDs not seen for 10s ? allows device swap without restart
	unsigned long now = millis();
	for (int i = 0; i < N; i++) {
		if (dev_known[i] && (now - dev_last_seen[i] > PID_TIMEOUT)) {
			dev_known[i]    = false;
			known_ser[i][0] = '\0';
		}
	}

	bool is_all = (strcmp(ser_str, "ALL") == 0);

	// helper: does port i match the requested identifier (by SER#)?
	auto port_matches = [&](int i) -> bool {
		if (!dev_known[i]) return false;
		if (strcmp(known_ser[i], ser_str) == 0) return true;
			return false;
	};

	if (is_hex) {
		if (is_all) {
			for (int i = 0; i < N; i++)
				if (dev_known[i]) exec_hex(i, p);
			char fwd[CMD_BUF_SIZE + 32]; snprintf(fwd, sizeof(fwd), "HEX ALL %s", p);
			forward_all(fwd);
		} else {
			bool found = false;
			for (int i = 0; i < N; i++) {
				if (port_matches(i)) {
					exec_hex(i, p); found = true; break;
				}
			}
			if (!found) {
				int route = route_find(ser_str);
				char fwd[CMD_BUF_SIZE + 32]; snprintf(fwd, sizeof(fwd), "HEX %s %s", ser_str, p);
				if (route >= 0) { ports[route]->print(fwd); ports[route]->print('\n'); }
				else forward_all(fwd);
			}
		}
		return;
	}

	// SET
	uint32_t watts = (uint32_t)atol(p);
	if (is_all) {
		// sequential execution ? avoids mutual hex_busy blocking
		for (int i = 0; i < N; i++) {
			if (dev_known[i]) exec_set(i, watts);
		}
		char fwd[CMD_BUF_SIZE + 32]; snprintf(fwd, sizeof(fwd), "SET ALL %lu", watts);
		forward_all(fwd);
	} else {
		bool found = false;
		for (int i = 0; i < N; i++) {
			if (port_matches(i)) {
				exec_set(i, watts); found = true; break;
			}
		}
		if (!found) {
			char fwd[CMD_BUF_SIZE + 32]; snprintf(fwd, sizeof(fwd), "SET %s %lu", ser_str, watts);
			int route = route_find(ser_str);
			if (route >= 0) { ports[route]->print(fwd); ports[route]->print('\n'); }
			else forward_all(fwd);
		}
	}
}

// =======================================================================
// Command reader
// =======================================================================

void read_cmd() {
	while (Serial.available()) {
		char c = Serial.read();
		if (c == '\n' || c == '\r') {
			if (cmd_len > 0) {
				cmd_buf[cmd_len] = '\0';
				process_cmd(cmd_buf);
				cmd_len = 0;
			}
		} else if (cmd_len < CMD_BUF_SIZE - 1) {
			cmd_buf[cmd_len++] = c;
		}
	}
}

// =======================================================================
// SER# and Vbat learning
// =======================================================================

void learn_from_line(int idx, const char* line) {
	if (strncmp(line, "PID\t", 4) == 0) {
		dev_known[idx] = true;
	}
	if (strncmp(line, "SER#\t", 5) == 0) {
		strncpy(known_ser[idx], line + 5, 15);
		known_ser[idx][15] = '\0';
		for (int j = 0; j < 15; j++) {
			if (known_ser[idx][j] == '\r' || known_ser[idx][j] == ' ')
				{ known_ser[idx][j] = '\0'; break; }
		}
		route_learn(idx, known_ser[idx]);
	}
	if (strncmp(line, "V\t", 2) == 0) {
		long mv = atol(line + 2);
		if (mv > 0) known_vbat[idx] = mv / 1000.0f;
	}
}

// =======================================================================
// Text aggregator
// =======================================================================

void reset_rx(int idx) {
	rx_len[idx] = 0;
}

// find start of current line in buf[0..len-1] ending at \n at pos len-1
static int line_start(const char* buf, int len) {
	for (int i = len - 2; i >= 0; i--)
		if (buf[i] == '\n') return i + 1;
	return 0;
}

void read_device(int idx) {
	if (hex_busy[idx]) return;
	while (ports[idx]->available()) {
		char c = ports[idx]->read();
		// skip remainder of HEX frame split across loop() calls
		if (hex_skip[idx]) {
			if (c == '\n') {
				if (ports[idx]->available() && ports[idx]->peek() == ':') {
					// stay in skip mode ? another HEX frame follows
				} else {
					hex_skip[idx] = false;
				}
			}
			continue;
		}
		if (rx_len[idx] == 0 && (c == '\r' || c == '\n')) continue;
		// discard stray HEX frames ? only at line start (rx_len==0 after \n)
		// mid-line ':' is valid in OR\t0x00000001
		if (rx_len[idx] == 0 && c == ':') {
			hex_skip[idx] = true;
			continue;
		}
		if (rx_len[idx] >= BUF_SIZE - 1) { reset_rx(idx); continue; }
		rx_buf[idx][rx_len[idx]++] = c;
		if (c == '\n') {
			int ls = line_start(rx_buf[idx], rx_len[idx]);
			// discard any line starting with ':'
			if (rx_buf[idx][ls] == ':') {
				rx_len[idx] = ls;
				continue;
			}
			// detect HEX frame injected mid-line e.g. "HSDS\t0:4AAAAFD\n"
			// VE.Direct values never contain ':' except "OR\t0x..."
			bool has_hex = false;
			for (int j = ls; j < rx_len[idx] - 1; j++) {
				if (rx_buf[idx][j] == ':' && (j == 0 || rx_buf[idx][j-1] != 'x')) {
					has_hex = true;
					// truncate line at the ':' ? discard HEX suffix
					rx_len[idx] = j;
					hex_skip[idx] = true;
					break;
				}
			}
			if (has_hex) continue;
			char linebuf[32] = {0};
			int len = rx_len[idx] - 1 - ls;
			if (len > 0 && len < 32) {
				strncpy(linebuf, &rx_buf[idx][ls], len);
				for (int j = 0; j < len; j++) if (linebuf[j] == '\r') { linebuf[j] = '\0'; break; }
				learn_from_line(idx, linebuf);
			}
			if (strncmp(&rx_buf[idx][ls], "Checksum\t", 9) == 0) {
				// Checksum byte is at ls+9; HEX frame may follow immediately as ':'
				// stop before ':' to avoid forwarding the HEX frame in the block
				int end = ls + 10;   // past "Checksum\t" (9 chars) + checksum byte (1)
				while (end < rx_len[idx] && rx_buf[idx][end] != ':' && rx_buf[idx][end] != '\n') end++;
				if (end < rx_len[idx] && rx_buf[idx][end] == '\n') end++;
				q_push(rx_buf[idx], end);
				dev_last_seen[idx] = millis();
				if (end < rx_len[idx] && rx_buf[idx][end] == ':') hex_skip[idx] = true;
				reset_rx(idx);
				continue;
			}
		}
	}
}

void send_next() {
	if (q_empty()) return;
	while (out_pos < q_len[q_head]) {
		if (Serial.availableForWrite() == 0) return;
		Serial.write(q_buf[q_head][out_pos++]);
	}
	last_send = millis();
	out_pos   = 0;
	q_head    = (q_head + 1) % Q_SIZE;
}

// =======================================================================

void setup() {
	Serial.begin(BAUD_OUT);
	for (int i = 0; i < N; i++) ports[i]->begin(port_baud[i]);
#if TEMP_ENABLE
	temp_sensors.begin();
	temp_count = temp_sensors.getDeviceCount();
	temp_sensors.setResolution(12);   // 12-bit = 0.0625 deg C resolution
	if (temp_count > 0) {
		// first conversion: wait blocking so getTempCByIndex() returns a
		// valid value immediately rather than NaN/garbage on the first read
		temp_sensors.setWaitForConversion(true);
		temp_sensors.requestTemperatures();
		temp_sensors.setWaitForConversion(false);
	}
	temp_last = millis() - TEMP_INTERVAL;   // force immediate first send_temp_blocks
#endif
}

#if TEMP_ENABLE
// Emit one pseudo VE.Direct block per DS18B20 sensor into the TX queue.
// Block format mimics a real VE.Direct device so the de-aggregator and
// Venus OS handle it transparently.
void send_temp_blocks() {
	if (temp_count == 0) return;
	if (millis() - temp_last < TEMP_INTERVAL) return;
	temp_last = millis();

	// retrieve results from previous async conversion
	for (int s = 0; s < temp_count; s++) {
		float t = temp_sensors.getTempCByIndex(s);
		if (t == DEVICE_DISCONNECTED_C) continue;

		// SER# encodes pin and sensor index: TEMP-P2-S0, TEMP-P2-S1, ...
		char ser[24], tmp[16];
		sprintf(ser, "TEMP-P%d-S%d", TEMP_PIN, s);
		// manual float formatting -- avoid sprintf %f (often unsupported on AVR)
		int t_whole = (int)t;
		int t_frac  = abs((int)((t - t_whole) * 100));
		sprintf(tmp, "%d.%02d", t_whole, t_frac);

		// build block in a local buffer
		char blk[256];
		int  pos = 0;
		pos += sprintf(blk + pos, "PID\t0x9999\r\n");
		pos += sprintf(blk + pos, "SER#\t%s\r\n", ser);
		pos += sprintf(blk + pos, "FW\t100\r\n");
		pos += sprintf(blk + pos, "TEMP\t%s\r\n", tmp);

		// compute VE.Direct checksum: sum of all bytes (incl Checksum line)
		// must equal 0 mod 256
		// "Checksum\t" + cs_byte + "\r\n" -- cs_byte chosen so total = 0
		uint8_t sum = 0;
		for (int i = 0; i < pos; i++) sum += (uint8_t)blk[i];
		// add the fixed bytes of "Checksum\t" (9 chars) + "\r\n" (2 chars)
		const char* cs_label = "Checksum\t";
		for (int i = 0; i < 9; i++) sum += (uint8_t)cs_label[i];
		sum += '\r'; sum += '\n';
		uint8_t cs_byte = (256 - sum) & 0xFF;

		pos += sprintf(blk + pos, "Checksum\t");
		blk[pos++] = (char)cs_byte;
		blk[pos++] = '\r';
		blk[pos++] = '\n';
		blk[pos]   = '\0';

		q_push(blk, pos);
	}

	// request next conversion (non-blocking, ready after ~750ms at 12-bit)
	temp_sensors.requestTemperatures();
}
#endif

void send_alive() {
	if (!q_empty()) return;
	if (millis() - last_send >= ALIVE_TIMEOUT) {
		Serial.print("ALIVE\r\n");
		last_send = millis();
	}
}

void loop() {
	read_cmd();
	send_next();
	for (int i = 0; i < N; i++) {
		read_device(i);
		send_next();
	}
#if TEMP_ENABLE
	send_temp_blocks();
	send_next();
#endif
	send_alive();
}
