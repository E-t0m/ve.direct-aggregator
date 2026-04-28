// VE.Direct Aggregator — Arduino Mega 2560
// Text aggregation + HEX power control channel
//
// ── Wiring ──────────────────────────────────────────────────────────────
//
//   Serial1 RX/TX  ↔  charger or upstream Mega 1
//   Serial2 RX/TX  ↔  charger or upstream Mega 2
//   Serial3 RX/TX  ↔  charger or upstream Mega 3
//   Serial0 TX     →  aggregated text stream out + OK/ERR replies
//   Serial0 RX     ←  SET commands from downstream Mega or RPi
//
//   In cascade: downstream Mega TX0 → this Mega RX0
//               this Mega TX0       → downstream Mega RX0
//               this Mega TX1/2/3   → upstream Mega RX0  (SET forwarding)
//               upstream Mega TX0   → this Mega RX1/2/3  (text + replies)
//
// ── Text aggregation ────────────────────────────────────────────────────
//
//   Unchanged from base version.
//   Input type auto-detected: DIRECT (charger) or UPSTREAM (aggregator).
//   Output format: ---\tN\r\n followed by N complete VE.Direct blocks.
//   Pauses during HEX command execution (hex_busy flag).
//
// ── SET command channel ─────────────────────────────────────────────────
//
//   Received on Serial0 RX:
//     SET <pid> <watts>\n     limit single charger by PID
//     SET ALL <watts>\n       limit all chargers (pseudo-multicast)
//
//   Sent on Serial0 TX:
//     OK <pid> <watts>\n      setting verified by HEX GET re-read
//     ERR <pid> timeout\n     no HEX reply within HEX_TIMEOUT ms
//     ERR <pid> verify\n      re-read value does not match
//
//   Watts → milliamps conversion uses last known Vbat per charger port.
//   SET ALL sends HEX to all direct ports simultaneously (pseudo-multicast),
//   then verifies and restores text mode one by one.
//   Unknown PIDs are forwarded on TX of all upstream ports.
//   OK/ERR replies from upstream are passed through on Serial0 TX.
//
// ── HEX sequence per charger ────────────────────────────────────────────
//
//   1. send  :8<reg><flags><value><cs>\n   (SET register)
//   2. wait  for HEX ACK reply             (1s timeout → ERR timeout)
//   3. send  :7<reg><flags><cs>\n          (GET register for verify)
//   4. wait  for HEX GET reply             (1s timeout → ERR timeout)
//   5. check readback == sent value        (mismatch → ERR verify)
//   6. send  :154\n                        (switch back to text mode)
//   7. send  OK/ERR on Serial0             (do NOT wait for text resume)
//
// ── Text gap during HEX ─────────────────────────────────────────────────
//
//   The affected charger stops sending text frames while in HEX mode.
//   No special notification is sent — the gap in the stream is the signal.
//   Other chargers continue unaffected.

#define BAUD             19200
#define BAUD_OUT         19200    // increase to 115200 for more throughput
#define BUF_SIZE         512
#define UP_BUF_SIZE      2048
#define DETECT_LINES     3
#define CMD_BUF_SIZE     64
#define HEX_TIMEOUT      1000     // ms, timeout for HEX reply
#define REG_MAX_CURRENT  0x2015    // VE.Direct: Charge Current Limit (unit: 0.1A, volatile — safe for frequent writes)

HardwareSerial* ports[] = {&Serial1, &Serial2, &Serial3};
const int N = sizeof(ports) / sizeof(ports[0]);

// ── port type detection ─────────────────────────────────────────────────
enum PortType { UNKNOWN, DIRECT, UPSTREAM };
PortType port_type[3]  = {UNKNOWN, UNKNOWN, UNKNOWN};
int      detect_cnt[3] = {0, 0, 0};
bool     saw_marker[3] = {false, false, false};

// ── direct charger text buffers ─────────────────────────────────────────
char buf[3][BUF_SIZE];
int  buf_len[3] = {0, 0, 0};
bool ready[3]   = {false, false, false};
char prev[3]    = {0, 0, 0};

// ── upstream text buffers ───────────────────────────────────────────────
char up_buf[3][UP_BUF_SIZE];
int  up_len[3]      = {0, 0, 0};
bool up_ready[3]    = {false, false, false};
int  up_expect[3]   = {0, 0, 0};
int  up_received[3] = {0, 0, 0};
char up_prev[3]     = {0, 0, 0};

// ── per-port learned data ───────────────────────────────────────────────
char  known_pid[3][16];           // PID string, e.g. "0xA053"
bool  pid_known[3]  = {false, false, false};
float known_vbat[3] = {24.0f, 24.0f, 24.0f};  // Vbat in V, default 24V

// ── SET command receive buffer ──────────────────────────────────────────
char cmd_buf[CMD_BUF_SIZE];
int  cmd_len = 0;

// ── HEX busy flag — pauses text aggregation ────────────────────────────
bool hex_busy = false;

// ═══════════════════════════════════════════════════════════════════════
// VE.Direct HEX protocol helpers
// ═══════════════════════════════════════════════════════════════════════

// compute VE.Direct HEX checksum over a hex-encoded data string
// checksum = (0x55 - sum_of_all_bytes) & 0xFF
uint8_t hex_checksum(const char* s) {
	uint8_t sum = 0;
	while (s[0] && s[1]) {
		uint8_t hi = (s[0] >= 'A') ? s[0] - 'A' + 10 : s[0] - '0';
		uint8_t lo = (s[1] >= 'A') ? s[1] - 'A' + 10 : s[1] - '0';
		sum += (hi << 4) | lo;
		s += 2;
	}
	return (0x55 - sum) & 0xFF;
}

// send VE.Direct HEX SET command (type 8, little-endian 16-bit value)
// :8<reg_lo><reg_hi><flags><val_lo><val_hi><cs>\n
void send_hex_set(HardwareSerial* port, uint16_t reg, uint16_t value) {
	char data[16];
	sprintf(data, "08%02X%02X00%02X%02X",
		(uint8_t)(reg & 0xFF), (uint8_t)(reg >> 8),
		(uint8_t)(value & 0xFF), (uint8_t)(value >> 8));
	uint8_t cs = hex_checksum(data);
	char msg[24];
	sprintf(msg, ":%s%02X\n", data, cs);
	port->print(msg);
}

// send VE.Direct HEX GET command (type 7)
// :7<reg_lo><reg_hi><flags><cs>\n
void send_hex_get(HardwareSerial* port, uint16_t reg) {
	char data[12];
	sprintf(data, "07%02X%02X00",
		(uint8_t)(reg & 0xFF), (uint8_t)(reg >> 8));
	uint8_t cs = hex_checksum(data);
	char msg[16];
	sprintf(msg, ":%s%02X\n", data, cs);
	port->print(msg);
}

// send VE.Direct text mode restore command
void send_text_mode(HardwareSerial* port) {
	port->print(":154\n");
}

// wait for a complete HEX reply line starting with ':'
// returns true if received within HEX_TIMEOUT ms
bool wait_hex_reply(HardwareSerial* port, char* rbuf, int rsize) {
	unsigned long t0 = millis();
	int pos = 0;
	while (millis() - t0 < (unsigned long)HEX_TIMEOUT) {
		if (port->available()) {
			char c = port->read();
			if (pos == 0 && c != ':') continue;    // skip until HEX start
			if (pos < rsize - 1) rbuf[pos++] = c;
			if (c == '\n') { rbuf[pos] = '\0'; return true; }
		}
	}
	rbuf[0] = '\0';
	return false;
}

// parse 16-bit little-endian value from HEX GET reply
// reply format: :5<reg_lo><reg_hi><flags><val_lo><val_hi><cs>\n
// positions:     0 1 2 3 4 5 6 7 8 9 10 11 12 13
// returns raw register value — for 0x2015: deciamps (0.1A units)
// convert to amps: value / 10.0f
uint16_t parse_hex_value(const char* reply) {
	// reply[0]=':' reply[1]='5' then 4 reg chars + 2 flag chars = pos 9
	if (strlen(reply) < 13) return 0xFFFF;
	char slo[3] = {reply[9],  reply[10], 0};
	char shi[3] = {reply[11], reply[12], 0};
	uint8_t lo = (uint8_t)strtol(slo, NULL, 16);
	uint8_t hi = (uint8_t)strtol(shi, NULL, 16);
	return (uint16_t)((hi << 8) | lo);
}

// ═══════════════════════════════════════════════════════════════════════
// SET execution on a single direct port
// ═══════════════════════════════════════════════════════════════════════

void exec_set(int idx, uint32_t watts) {
	HardwareSerial* port = ports[idx];
	const char* pid      = known_pid[idx];
	char reply[64];

	// convert watts to amps (0.1A resolution for register 0x2015)
	// guard against zero/invalid Vbat
	float    vbat     = (known_vbat[idx] > 1.0f) ? known_vbat[idx] : 24.0f;
	float    amps     = (float)watts / vbat;				// e.g. 19.5 A
	uint16_t deciamps = (uint16_t)(amps * 10.0f + 0.5f);	// round to nearest 0.1A

	// 1. send SET
	send_hex_set(port, REG_MAX_CURRENT, deciamps);

	// 2. wait for HEX ACK
	if (!wait_hex_reply(port, reply, sizeof(reply))) {
		char msg[48];
		sprintf(msg, "ERR %s timeout\n", pid);
		Serial.print(msg);
		send_text_mode(port);
		return;
	}

	// 3. verify: GET register
	delay(30);
	send_hex_get(port, REG_MAX_CURRENT);

	// 4. wait for GET reply
	if (!wait_hex_reply(port, reply, sizeof(reply))) {
		char msg[48];
		sprintf(msg, "ERR %s timeout\n", pid);
		Serial.print(msg);
		send_text_mode(port);
		return;
	}

	// 5. check readback — compare in deciamps, report in A with 1 decimal
	uint16_t readback = parse_hex_value(reply);
	char msg[64];
	if (readback == deciamps) {
		float a_set = deciamps / 10.0f;
		// format: OK <pid> <watts>W <amps>A
		char abuf[12]; dtostrf(a_set, 4, 1, abuf);
		sprintf(msg, "OK %s %luW %sA\n", pid, watts, abuf);
	} else {
		float a_set = deciamps  / 10.0f;
		float a_rb  = readback  / 10.0f;
		char abuf_set[12]; dtostrf(a_set, 4, 1, abuf_set);
		char abuf_rb[12];  dtostrf(a_rb,  4, 1, abuf_rb);
		sprintf(msg, "ERR %s verify set=%sA rb=%sA\n", pid, abuf_set, abuf_rb);
	}

	// 6. restore text mode — do NOT wait for text to resume
	send_text_mode(port);

	// 7. send result
	Serial.print(msg);
}

// ═══════════════════════════════════════════════════════════════════════
// Forward SET/HEX command on TX of all upstream ports
// ═══════════════════════════════════════════════════════════════════════

void forward_upstream(const char* fwd) {
	for (int i = 0; i < N; i++) {
		if (port_type[i] == UPSTREAM) {
			ports[i]->print(fwd);
			ports[i]->print('\n');
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════
// Execute arbitrary HEX string on a single direct port
// hex_str must be a complete VE.Direct HEX line e.g. ":154\n"
// ═══════════════════════════════════════════════════════════════════════

void exec_hex(int idx, const char* hex_str) {
	HardwareSerial* port = ports[idx];
	const char* pid      = known_pid[idx];
	char reply[64];

	// send raw HEX string — no parsing, no conversion
	port->print(hex_str);
	if (hex_str[strlen(hex_str)-1] != '\n') port->print('\n');

	// wait for reply
	if (!wait_hex_reply(port, reply, sizeof(reply))) {
		char msg[48];
		sprintf(msg, "ERR %s timeout\n", pid);
		Serial.print(msg);
		return;
	}

	// pass reply back downstream with PID prefix
	char msg[96];
	sprintf(msg, "HEX_REPLY %s %s", pid, reply);
	Serial.print(msg);
}

// ═══════════════════════════════════════════════════════════════════════
// Process a complete command line (SET or HEX)
// ═══════════════════════════════════════════════════════════════════════

void process_cmd(char* line) {
	bool is_set = (strncmp(line, "SET ", 4) == 0);
	bool is_hex = (strncmp(line, "HEX ", 4) == 0);
	if (!is_set && !is_hex) return;

	char* p = line + 4;

	// parse PID or ALL
	char pid_str[20];
	int ti = 0;
	while (*p && *p != ' ' && ti < 19) pid_str[ti++] = *p++;
	pid_str[ti] = '\0';
	if (*p == ' ') p++;
	// p now points to: watts (SET) or hex_string (HEX)

	bool is_all = (strcmp(pid_str, "ALL") == 0);

	if (is_hex) {
		// ── HEX command: pass raw string to charger(s) ──────────────────
		hex_busy = true;
		if (is_all) {
			for (int i = 0; i < N; i++) {
				if (port_type[i] == DIRECT && pid_known[i]) exec_hex(i, p);
			}
			char fwd[96];
			sprintf(fwd, "HEX ALL %s", p);
			forward_upstream(fwd);
		} else {
			bool found = false;
			for (int i = 0; i < N; i++) {
				if (port_type[i] == DIRECT && pid_known[i] &&
					strcmp(known_pid[i], pid_str) == 0) {
					exec_hex(i, p);
					found = true;
					break;
				}
			}
			if (!found) {
				char fwd[96];
				sprintf(fwd, "HEX %s %s", pid_str, p);
				forward_upstream(fwd);
			}
		}
		hex_busy = false;
		return;
	}

	// ── SET command: watts-based power limit ────────────────────────────
	uint32_t watts = (uint32_t)atol(p);
	if (watts == 0 && p[0] != '0') return;   // reject malformed input

	hex_busy = true;

	if (is_all) {
		// pseudo-multicast: send HEX SET to all direct ports simultaneously
		// register 0x2015 unit is 0.1A  →  reg_value = round(amps * 10)
		for (int i = 0; i < N; i++) {
			if (port_type[i] == DIRECT && pid_known[i]) {
				float vbat = (known_vbat[i] > 1.0f) ? known_vbat[i] : 24.0f;
				float amps = (float)watts / vbat;
				uint16_t da = (uint16_t)(amps * 10.0f + 0.5f);
				send_hex_set(ports[i], REG_MAX_CURRENT, da);
			}
		}
		// collect replies, verify, restore text mode one by one
		for (int i = 0; i < N; i++) {
			if (port_type[i] == DIRECT && pid_known[i]) {
				float    vbat = (known_vbat[i] > 1.0f) ? known_vbat[i] : 24.0f;
				float    amps = (float)watts / vbat;
				uint16_t da   = (uint16_t)(amps * 10.0f + 0.5f);
				char reply[64]; char msg[64];

				if (!wait_hex_reply(ports[i], reply, sizeof(reply))) {
					sprintf(msg, "ERR %s timeout\n", known_pid[i]);
					Serial.print(msg);
					send_text_mode(ports[i]);
					continue;
				}
				delay(30);
				send_hex_get(ports[i], REG_MAX_CURRENT);
				if (!wait_hex_reply(ports[i], reply, sizeof(reply))) {
					sprintf(msg, "ERR %s timeout\n", known_pid[i]);
				} else {
					uint16_t rb = parse_hex_value(reply);
					if (rb == da) {
						char abuf[12]; dtostrf(da / 10.0f, 4, 1, abuf);
						sprintf(msg, "OK %s %luW %sA\n", known_pid[i], watts, abuf);
					} else {
						char abuf_set[12]; dtostrf(da / 10.0f, 4, 1, abuf_set);
						char abuf_rb[12];  dtostrf(rb / 10.0f, 4, 1, abuf_rb);
						sprintf(msg, "ERR %s verify set=%sA rb=%sA\n", known_pid[i], abuf_set, abuf_rb);
					}
				}
				send_text_mode(ports[i]);
				Serial.print(msg);
			}
		}
		// forward to upstream MCUs
		char fwd[32];
		sprintf(fwd, "SET ALL %lu", watts);
		forward_upstream(fwd);

	} else {
		// single PID
		bool found = false;
		for (int i = 0; i < N; i++) {
			if (port_type[i] == DIRECT && pid_known[i] &&
				strcmp(known_pid[i], pid_str) == 0) {
				exec_set(i, watts);
				found = true;
				break;
			}
		}
		if (!found) {
			// unknown PID — forward to all upstream MCUs
			char fwd[48];
			sprintf(fwd, "SET %s %lu", pid_str, watts);
			forward_upstream(fwd);
		}
	}

	hex_busy = false;
}

// ═══════════════════════════════════════════════════════════════════════
// Read SET commands on Serial0 RX
// ═══════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════
// PID and Vbat learning from direct charger text stream
// ═══════════════════════════════════════════════════════════════════════

void learn_from_line(int idx, const char* line) {
	// learn PID (only once, PIDs don't change)
	if (!pid_known[idx] && strncmp(line, "PID\t", 4) == 0) {
		strncpy(known_pid[idx], line + 4, 15);
		known_pid[idx][15] = '\0';
		// strip any trailing \r or whitespace
		for (int j = 0; j < 15; j++) {
			if (known_pid[idx][j] == '\r' || known_pid[idx][j] == ' ') {
				known_pid[idx][j] = '\0'; break;
			}
		}
		pid_known[idx] = true;
	}
	// learn Vbat every cycle — raw field "V" = battery voltage in mV → stored as V
	if (strncmp(line, "V\t", 2) == 0) {
		long mv = atol(line + 2);
		if (mv > 0) known_vbat[idx] = mv / 1000.0f;	// mV → V
	}
}

void extract_line(int idx, int end_pos) {
	// find start of current line in buf (scan back from end_pos for previous \n)
	int start = end_pos - 1;
	while (start > 0 && buf[idx][start - 1] != '\n') start--;
	int len = end_pos - start;
	if (len < 2 || len > 30) return;
	char linebuf[32];
	strncpy(linebuf, buf[idx] + start, len);
	linebuf[len] = '\0';
	// strip \r
	for (int j = 0; j < len; j++) if (linebuf[j] == '\r') { linebuf[j] = '\0'; break; }
	learn_from_line(idx, linebuf);
}

// ═══════════════════════════════════════════════════════════════════════
// Text aggregator
// ═══════════════════════════════════════════════════════════════════════

void detect_type(int idx) {
	HardwareSerial* port = ports[idx];
	while (port->available()) {
		char c = port->read();
		if (c == '\n') {
			detect_cnt[idx]++;
			if (saw_marker[idx])                    { port_type[idx] = UPSTREAM; return; }
			if (detect_cnt[idx] >= DETECT_LINES)    { port_type[idx] = DIRECT;   return; }
		}
		if (c == '-' && detect_cnt[idx] == 0) saw_marker[idx] = true;
	}
}

void read_direct(int idx) {
	if (hex_busy) return;
	HardwareSerial* port = ports[idx];
	while (port->available() && !ready[idx]) {
		char c = port->read();
		if (c == '\n' && prev[idx] == '\n') {
			if (buf_len[idx] > 0) {
				if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
				ready[idx] = true;
			}
			return;
		}
		// extract lines for PID/Vbat learning
		if (c == '\n' && buf_len[idx] > 0) extract_line(idx, buf_len[idx]);
		prev[idx] = c;
		if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
	}
}

void read_upstream(int idx) {
	if (hex_busy) return;
	HardwareSerial* port = ports[idx];
	while (port->available() && !up_ready[idx]) {
		char c = port->read();
		if (up_expect[idx] == 0) {
			// between blocks: look for ---\tN marker or OK/ERR reply
			if (up_len[idx] < UP_BUF_SIZE - 1) up_buf[idx][up_len[idx]++] = c;
			if (c == '\n') {
				up_buf[idx][up_len[idx]] = '\0';
				if (up_len[idx] >= 5 &&
					up_buf[idx][0] == '-' && up_buf[idx][1] == '-' && up_buf[idx][2] == '-') {
					// aggregator marker
					up_expect[idx]   = atoi(&up_buf[idx][4]);
					up_received[idx] = 0;
					up_len[idx]      = 0;
					up_prev[idx]     = 0;
				} else if (strncmp(up_buf[idx], "OK ",  3) == 0 ||
				           strncmp(up_buf[idx], "ERR ", 4) == 0) {
					// upstream reply — pass through downstream
					Serial.print(up_buf[idx]);
					up_len[idx] = 0;
				} else {
					up_len[idx] = 0;
				}
			}
		} else {
			// collecting N blocks, count block ends (double \n)
			if (c == '\n' && up_prev[idx] == '\n') {
				if (up_len[idx] < UP_BUF_SIZE - 1) up_buf[idx][up_len[idx]++] = c;
				up_received[idx]++;
				if (up_received[idx] >= up_expect[idx]) {
					up_ready[idx] = true;
					return;
				}
			} else {
				if (up_len[idx] < UP_BUF_SIZE - 1) up_buf[idx][up_len[idx]++] = c;
			}
			up_prev[idx] = c;
		}
	}
}

void send_blocks() {
	if (hex_busy) return;
	int count = 0;
	for (int i = 0; i < N; i++) {
		if (port_type[i] == DIRECT   && ready[i])    count++;
		if (port_type[i] == UPSTREAM && up_ready[i]) count += up_expect[i];
	}
	if (count == 0) return;

	Serial.print("---\t");
	Serial.print(count);
	Serial.print("\r\n");

	for (int i = 0; i < N; i++) {
		if (port_type[i] == DIRECT && ready[i]) {
			Serial.write(buf[i], buf_len[i]);
			Serial.flush();
			buf_len[i] = 0; ready[i] = false; prev[i] = 0;
		}
		if (port_type[i] == UPSTREAM && up_ready[i]) {
			Serial.write(up_buf[i], up_len[i]);
			Serial.flush();
			up_len[i] = 0; up_ready[i] = false;
			up_expect[i] = 0; up_received[i] = 0; up_prev[i] = 0;
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════

void setup() {
	Serial.begin(BAUD_OUT);
	for (int i = 0; i < N; i++) {
		ports[i]->begin(BAUD);
	}
}

void loop() {
	read_cmd();
	for (int i = 0; i < N; i++) {
		if      (port_type[i] == UNKNOWN)  detect_type(i);
		else if (port_type[i] == DIRECT)   read_direct(i);
		else if (port_type[i] == UPSTREAM) read_upstream(i);
	}
	send_blocks();
}
