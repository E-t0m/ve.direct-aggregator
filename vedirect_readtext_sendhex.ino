// VE.Direct readtext_sendhex — Arduino Mega 2560
// Text aggregation + HEX power control channel
//
// ── Wiring ──────────────────────────────────────────────────────────────
//   Serial1 RX/TX  ↔  VE.Direct device or upstream aggregator 1
//   Serial2 RX/TX  ↔  VE.Direct device or upstream aggregator 2
//   Serial3 RX/TX  ↔  VE.Direct device or upstream aggregator 3
//   Serial0 TX     →  aggregated text stream out + OK/ERR/HEX_REPLY
//   Serial0 RX     ←  SET/HEX commands from downstream / host
//
// ── Output format ───────────────────────────────────────────────────────
//   Plain sequential VE.Direct text stream.
//   Each block starts with PID\t... and ends with \r\n\r\n.
//   Blocks sent immediately when complete, one at a time.
//   Receiving end identifies devices by PID field.
//
// ── Compatibility ────────────────────────────────────────────────────────
//   Compatible with: any VE.Direct text parser
//   Not compatible with: Cerbo GX / Venus GX as direct receiver
//
// ── SET command (received on Serial0 RX) ────────────────────────────────
//   SET <pid> <watts>\n     limit single charger
//   SET ALL <watts>\n       limit all chargers
//
// ── HEX command (received on Serial0 RX) ────────────────────────────────
//   HEX <pid> <hex_string>\n    send raw HEX to any VE.Direct device
//   HEX ALL <hex_string>\n      send raw HEX to all direct devices
//
// ── Replies (sent on Serial0 TX) ────────────────────────────────────────
//   OK <pid> <watts>W <amps>A\n
//   ERR <pid> timeout\n
//   ERR <pid> verify set=XA rb=YA\n
//   HEX_REPLY <pid> :<hex_response>\n
//
// ── HEX sequence per charger ────────────────────────────────────────────
//   1. send HEX SET/command
//   2. wait for ACK (1s timeout)
//   3. for SET: send GET to verify, restore text mode (:154\n)
//   4. send OK/ERR/HEX_REPLY
//   Only the affected device port pauses during HEX (~50-100ms typical).
//   All other ports continue reading and sending unaffected.

#define BAUD             19200
#define BAUD_OUT         19200    // set to 115200 for higher throughput
#define BUF_SIZE         512
#define DETECT_LINES     3
#define CMD_BUF_SIZE     64
#define HEX_TIMEOUT      1000
#define REG_MAX_CURRENT  0x2015   // Charge Current Limit (0.1A, volatile)
#define VBAT_FALLBACK    24.0f    // V, used until first Vbat received
#define MAX_ROUTES       12       // max PIDs remembered per port for routing
#define PID_TIMEOUT      10000UL  // ms — PID/route expires after this inactivity

HardwareSerial* ports[] = {&Serial1, &Serial2, &Serial3};
const int N = sizeof(ports) / sizeof(ports[0]);

// --- port type ---
enum PortType { UNKNOWN, DIRECT };
PortType port_type[3]  = {UNKNOWN, UNKNOWN, UNKNOWN};
int      detect_cnt[3] = {0, 0, 0};
char     detect_buf[3][16];
int      detect_buf_len[3] = {0, 0, 0};

// --- DIRECT buffers ---
char  buf[3][BUF_SIZE];
int   buf_len[3] = {0, 0, 0};
bool  ready[3]   = {false, false, false};
char  prev[3]    = {0, 0, 0};


// --- learned data ---
// known_pid: direct devices on this port (index 0)
char  known_pid[3][16];                          // PID of directly connected device
bool  pid_known[3]  = {false, false, false};
unsigned long pid_last_seen[3] = {0, 0, 0};  // millis() of last direct PID
float known_vbat[3] = {VBAT_FALLBACK, VBAT_FALLBACK, VBAT_FALLBACK};

// route table: PIDs seen arriving from each port (direct + forwarded blocks)
char  route_pid[3][MAX_ROUTES][16];              // PIDs seen on each port
unsigned long route_last_seen[3][MAX_ROUTES];    // millis() of last block
int   route_cnt[3] = {0, 0, 0};  // number of known routes per port

// --- command buffer ---
char cmd_buf[CMD_BUF_SIZE];
int  cmd_len = 0;
bool hex_busy[3] = {false, false, false};   // per-port HEX busy flag

// ═══════════════════════════════════════════════════════════════════════
// HEX protocol helpers
// ═══════════════════════════════════════════════════════════════════════

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

void send_text_mode(HardwareSerial* port) { port->print(":154\n"); }

bool wait_hex_reply(HardwareSerial* port, char* rbuf, int rsize) {
	unsigned long t0 = millis();
	int pos = 0;
	while (millis() - t0 < (unsigned long)HEX_TIMEOUT) {
		if (port->available()) {
			char c = port->read();
			if (pos == 0 && c != ':') continue;
			if (pos < rsize - 1) rbuf[pos++] = c;
			if (c == '\n') { rbuf[pos] = '\0'; return true; }
		}
	}
	rbuf[0] = '\0';
	return false;
}

uint16_t parse_hex_value(const char* reply) {
	// :5<reg_lo><reg_hi><flags><val_lo><val_hi><cs>\n — returns raw deciamps (0.1A)
	if (strlen(reply) < 13) return 0xFFFF;
	char slo[3] = {reply[9],  reply[10], 0};
	char shi[3] = {reply[11], reply[12], 0};
	return (uint16_t)(((uint8_t)strtol(shi,NULL,16) << 8) | (uint8_t)strtol(slo,NULL,16));
}

// ═══════════════════════════════════════════════════════════════════════
// SET / HEX execution
// ═══════════════════════════════════════════════════════════════════════

void exec_set(int idx, uint32_t watts) {
	HardwareSerial* port = ports[idx];
	const char* pid = known_pid[idx];
	char reply[64];
	hex_busy[idx] = true;

	float    vbat     = (known_vbat[idx] > 1.0f) ? known_vbat[idx] : VBAT_FALLBACK;
	float    amps     = (float)watts / vbat;
	uint16_t deciamps = (uint16_t)(amps * 10.0f + 0.5f);

	send_hex_set(port, REG_MAX_CURRENT, deciamps);

	if (!wait_hex_reply(port, reply, sizeof(reply))) {
		char msg[48]; sprintf(msg, "ERR %s timeout\n", pid);
		Serial.print(msg); send_text_mode(port); return;
	}
	delay(30);
	send_hex_get(port, REG_MAX_CURRENT);
	if (!wait_hex_reply(port, reply, sizeof(reply))) {
		char msg[48]; sprintf(msg, "ERR %s timeout\n", pid);
		Serial.print(msg); send_text_mode(port); return;
	}

	uint16_t readback = parse_hex_value(reply);
	char msg[64];
	if (readback == deciamps) {
		char abuf[12]; dtostrf(deciamps / 10.0f, 4, 1, abuf);
		sprintf(msg, "OK %s %luW %sA\n", pid, watts, abuf);
	} else {
		char as[12]; dtostrf(deciamps / 10.0f, 4, 1, as);
		char ar[12]; dtostrf(readback / 10.0f, 4, 1, ar);
		sprintf(msg, "ERR %s verify set=%sA rb=%sA\n", pid, as, ar);
	}
	send_text_mode(port);
	hex_busy[idx] = false;
	Serial.print(msg);
}

void exec_hex(int idx, const char* hex_str) {
	HardwareSerial* port = ports[idx];
	const char* pid = known_pid[idx];
	char reply[64];
	hex_busy[idx] = true;

	port->print(hex_str);
	if (hex_str[strlen(hex_str)-1] != '\n') port->print('\n');

	if (!wait_hex_reply(port, reply, sizeof(reply))) {
		char msg[48]; sprintf(msg, "ERR %s timeout\n", pid);
		Serial.print(msg); return;
	}
	char msg[96];
	sprintf(msg, "HEX_REPLY %s %s", pid, reply);
	hex_busy[idx] = false;
	Serial.print(msg);
}

// learn route: remember which port a PID was seen on
void route_learn(int port_idx, const char* pid) {
	unsigned long now = millis();
	// search existing route
	for (int j = 0; j < route_cnt[port_idx]; j++) {
		if (strcmp(route_pid[port_idx][j], pid) == 0) {
			route_last_seen[port_idx][j] = now;
			return;
		}
	}
	// new PID — find free or oldest slot
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
	strncpy(route_pid[port_idx][slot], pid, 15);
	route_pid[port_idx][slot][15] = '\0';
	route_last_seen[port_idx][slot] = now;
}

// find which port a PID was last seen on (-1 if unknown or expired)
int route_find(const char* pid) {
	unsigned long now = millis();
	for (int i = 0; i < N; i++) {
		for (int j = 0; j < route_cnt[i]; j++) {
			if (strcmp(route_pid[i][j], pid) == 0) {
				if (now - route_last_seen[i][j] > PID_TIMEOUT) return -1;  // expired
				return i;
			}
		}
	}
	return -1;  // unknown
}

// forward command on all input ports — used when route not yet learned
void forward_all(const char* fwd) {
	for (int i = 0; i < N; i++) {
		if (port_type[i] == DIRECT) {
			ports[i]->print(fwd);
			ports[i]->print('\n');
		}
	}
}

void process_cmd(char* line) {
	bool is_set = (strncmp(line, "SET ", 4) == 0);
	bool is_hex = (strncmp(line, "HEX ", 4) == 0);
	if (!is_set && !is_hex) return;

	char* p = line + 4;
	char pid_str[20]; int ti = 0;
	while (*p && *p != ' ' && ti < 19) pid_str[ti++] = *p++;
	pid_str[ti] = '\0';
	if (*p == ' ') p++;

	// expire PIDs not seen for 10s — allows device swap without restart
	unsigned long now = millis();
	for (int i = 0; i < N; i++) {
		if (pid_known[i] && (now - pid_last_seen[i] > PID_TIMEOUT))
			pid_known[i] = false;
	}

	bool is_all = (strcmp(pid_str, "ALL") == 0);
	if (is_hex) {
		if (is_all) {
			for (int i = 0; i < N; i++)
				if (port_type[i] == DIRECT && pid_known[i]) exec_hex(i, p);
			char fwd[96]; sprintf(fwd, "HEX ALL %s", p);
			forward_all(fwd);
		} else {
			bool found = false;
			for (int i = 0; i < N; i++) {
				if (port_type[i] == DIRECT && pid_known[i] &&
					strcmp(known_pid[i], pid_str) == 0) {
					exec_hex(i, p); found = true; break;
				}
			}
			if (!found) {
				int route = route_find(pid_str);
				char fwd[96]; sprintf(fwd, "HEX %s %s", pid_str, p);
				if (route >= 0) { ports[route]->print(fwd); ports[route]->print('\n'); }
				else forward_all(fwd);
			}
		}
			return;
	}

	// SET
	uint32_t watts = (uint32_t)atol(p);
	if (is_all) {
		// pseudo-multicast: send HEX SET to all direct ports simultaneously
		for (int i = 0; i < N; i++) {
			if (port_type[i] == DIRECT && pid_known[i]) {
				hex_busy[i] = true;
				float vbat = (known_vbat[i] > 1.0f) ? known_vbat[i] : VBAT_FALLBACK;
				uint16_t da = (uint16_t)((float)watts / vbat * 10.0f + 0.5f);
				send_hex_set(ports[i], REG_MAX_CURRENT, da);
			}
		}
		// collect replies, verify, restore text mode one by one
		for (int i = 0; i < N; i++) {
			if (port_type[i] == DIRECT && pid_known[i]) {
				float    vbat = (known_vbat[i] > 1.0f) ? known_vbat[i] : VBAT_FALLBACK;
				uint16_t da   = (uint16_t)((float)watts / vbat * 10.0f + 0.5f);
				char reply[64]; char msg[64];
				if (!wait_hex_reply(ports[i], reply, sizeof(reply))) {
					sprintf(msg, "ERR %s timeout\n", known_pid[i]);
					Serial.print(msg); send_text_mode(ports[i]);
					hex_busy[i] = false; continue;
				}
				delay(30);
				send_hex_get(ports[i], REG_MAX_CURRENT);
				if (!wait_hex_reply(ports[i], reply, sizeof(reply))) {
					sprintf(msg, "ERR %s timeout\n", known_pid[i]);
				} else {
					uint16_t rb = parse_hex_value(reply);
					if (rb == da) {
						char abuf[12]; dtostrf(da/10.0f, 4, 1, abuf);
						sprintf(msg, "OK %s %luW %sA\n", known_pid[i], watts, abuf);
					} else {
						char as[12]; dtostrf(da/10.0f, 4, 1, as);
						char ar[12]; dtostrf(rb/10.0f, 4, 1, ar);
						sprintf(msg, "ERR %s verify set=%sA rb=%sA\n", known_pid[i], as, ar);
					}
				}
				send_text_mode(ports[i]);
				hex_busy[i] = false;
				Serial.print(msg);
			}
		}
		char fwd[32]; sprintf(fwd, "SET ALL %lu", watts);
		forward_all(fwd);
	} else {
		bool found = false;
		for (int i = 0; i < N; i++) {
			if (port_type[i] == DIRECT && pid_known[i] &&
				strcmp(known_pid[i], pid_str) == 0) {
				exec_set(i, watts); found = true; break;
			}
		}
		if (!found) {
			char fwd[48]; sprintf(fwd, "SET %s %lu", pid_str, watts);
			int route = route_find(pid_str);
			if (route >= 0) { ports[route]->print(fwd); ports[route]->print('\n'); }
			else forward_all(fwd);
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════
// Command reader
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
// PID and Vbat learning
// ═══════════════════════════════════════════════════════════════════════

void learn_from_line(int idx, const char* line) {
	if (strncmp(line, "PID\t", 4) == 0) {
		strncpy(known_pid[idx], line + 4, 15);
		known_pid[idx][15] = '\0';
		for (int j = 0; j < 15; j++) {
			if (known_pid[idx][j] == '\r' || known_pid[idx][j] == ' ')
				{ known_pid[idx][j] = '\0'; break; }
		}
		pid_known[idx]    = true;
		pid_last_seen[idx] = millis();   // refresh on every block
	}
	if (strncmp(line, "V\t", 2) == 0) {
		long mv = atol(line + 2);
		if (mv > 0) known_vbat[idx] = mv / 1000.0f;
	}
}

void extract_line(int idx, int end_pos) {
	int start = end_pos - 1;
	while (start > 0 && buf[idx][start-1] != '\n') start--;
	int len = end_pos - start;
	if (len < 2 || len > 30) return;
	char linebuf[32];
	strncpy(linebuf, buf[idx] + start, len);
	linebuf[len] = '\0';
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
		if (detect_buf_len[idx] < 15) detect_buf[idx][detect_buf_len[idx]++] = c;
		if (c == '\n') {
			detect_cnt[idx]++;
			detect_buf_len[idx] = 0;
			if (detect_cnt[idx] >= DETECT_LINES) {
				port_type[idx] = DIRECT;
				return;
			}
		}
	}
}

void read_direct(int idx) {
	if (hex_busy[idx]) return;
	HardwareSerial* port = ports[idx];
	while (port->available() && !ready[idx]) {
		char c = port->read();
		if (c == '\n' && prev[idx] == '\n') {
			if (buf_len[idx] > 0) {
				if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
				ready[idx] = true;
			}
			prev[idx] = 0;
			return;
		}
		if (c == '\n' && buf_len[idx] > 0) extract_line(idx, buf_len[idx]);
		prev[idx] = c;
		if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
	}
}


// send one ready block — first ready port wins, no mixing possible
void send_next() {
	for (int i = 0; i < N; i++) {
		if (port_type[i] == DIRECT && ready[i]) {
			Serial.write(buf[i], buf_len[i]);
			Serial.flush();
			buf_len[i] = 0; ready[i] = false; prev[i] = 0;
			return;
		}

	}
}

// ═══════════════════════════════════════════════════════════════════════

void setup() {
	Serial.begin(BAUD_OUT);
	for (int i = 0; i < N; i++) ports[i]->begin(BAUD);
}

void loop() {
	read_cmd();
	for (int i = 0; i < N; i++) {
		if      (port_type[i] == UNKNOWN)  detect_type(i);
		else if (port_type[i] == DIRECT)   read_direct(i);
	}
	send_next();
}
