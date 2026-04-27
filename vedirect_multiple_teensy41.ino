// VE.Direct Aggregator — Teensy 4.1
// Serial1–7 RX ← charger or upstream aggregator 1–7
// Serial8   TX → aggregated text stream out
//
// Note: VE.Direct is 5V TTL — use BSS138 level shifter on each RX input.
//
// Input type auto-detected per port:
//   DIRECT   — normal VE.Direct text stream
//   UPSTREAM — aggregated stream starting with ---\tN marker
//
// Note: VE.Direct is 5V TTL — use BSS138 level shifter on each RX input.
//       Teensy 4.1 operates at 3.3V logic.
//
// Output marker before each packet:
//   ---\tN\r\n  (N = total blocks in packet)

#define BAUD         19200
#define BAUD_OUT     19200   // set to 115200 for higher throughput
#define BUF_SIZE     512     // per DIRECT port
#define UP_BUF_SIZE  2048    // per UPSTREAM port (~9 blocks)
#define DETECT_LINES 3       // lines to observe before deciding type

HardwareSerial* ports[] = {
	&Serial1, &Serial2, &Serial3, &Serial4,
	&Serial5, &Serial6, &Serial7
};
const int N = sizeof(ports) / sizeof(ports[0]);

// --- port type detection ---
enum PortType { UNKNOWN, DIRECT, UPSTREAM };
PortType port_type[7]  = {UNKNOWN, UNKNOWN, UNKNOWN};
int      detect_cnt[7] = {0, 0, 0};
char     detect_buf[7][16];   // accumulate current line during detection
int      detect_buf_len[7] = {0, 0, 0};

// --- DIRECT charger buffers ---
char  buf[7][BUF_SIZE];
int   buf_len[7] = {0, 0, 0, 0, 0, 0, 0};
bool  ready[7]   = {false, false, false, false, false, false, false};
char  prev[7]    = {0, 0, 0, 0, 0, 0, 0};

// --- UPSTREAM aggregator buffers ---
char  up_buf[7][UP_BUF_SIZE];
int   up_len[7]      = {0, 0, 0, 0, 0, 0, 0};
bool  up_ready[7]    = {false, false, false, false, false, false, false};
int   up_expect[7]   = {0, 0, 0, 0, 0, 0, 0};
int   up_received[7] = {0, 0, 0, 0, 0, 0, 0};
char  up_prev[7]     = {0, 0, 0, 0, 0, 0, 0};

void setup() {
	Serial8.begin(BAUD_OUT);
	for (int i = 0; i < N; i++) {
		ports[i]->begin(BAUD);
	}
}

// detect input type by buffering complete lines and inspecting them
// more robust than checking first character only
void detect_type(int idx) {
	HardwareSerial* port = ports[idx];
	while (port->available()) {
		char c = port->read();
		// accumulate into line buffer
		if (detect_buf_len[idx] < 15) detect_buf[idx][detect_buf_len[idx]++] = c;
		if (c == '\n') {
			detect_buf[idx][detect_buf_len[idx]] = '\0';
			// check if this complete line is a ---\t marker
			if (detect_buf_len[idx] >= 5 &&
				detect_buf[idx][0] == '-' &&
				detect_buf[idx][1] == '-' &&
				detect_buf[idx][2] == '-') {
				port_type[idx] = UPSTREAM;
				// seed upstream state from this marker
				up_expect[idx]   = atoi(&detect_buf[idx][4]);
				up_received[idx] = 0;
				up_len[idx]      = 0;
				up_prev[idx]     = 0;
				return;
			}
			detect_cnt[idx]++;
			detect_buf_len[idx] = 0;
			if (detect_cnt[idx] >= DETECT_LINES) {
				port_type[idx] = DIRECT;
				return;
			}
		}
	}
}

// read a DIRECT charger port — buffer until double \n (block end)
void read_direct(int idx) {
	HardwareSerial* port = ports[idx];
	while (port->available() && !ready[idx]) {
		char c = port->read();
		if (c == '\n' && prev[idx] == '\n') {
			if (buf_len[idx] > 0) {
				if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
				ready[idx] = true;
			}
			prev[idx] = 0;   // always reset — prevents false trigger on next block
			return;
		}
		prev[idx] = c;
		if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
	}
}

// read an UPSTREAM port — wait for ---\tN marker, then collect N complete blocks
void read_upstream(int idx) {
	HardwareSerial* port = ports[idx];
	while (port->available() && !up_ready[idx]) {
		char c = port->read();

		if (up_expect[idx] == 0) {
			// waiting for next ---\tN marker
			if (up_len[idx] < UP_BUF_SIZE - 1) up_buf[idx][up_len[idx]++] = c;
			if (c == '\n') {
				up_buf[idx][up_len[idx]] = '\0';
				if (up_len[idx] >= 5 &&
					up_buf[idx][0] == '-' &&
					up_buf[idx][1] == '-' &&
					up_buf[idx][2] == '-') {
					up_expect[idx]   = atoi(&up_buf[idx][4]);
					up_received[idx] = 0;
					up_len[idx]      = 0;
					up_prev[idx]     = 0;
				} else {
					up_len[idx] = 0;   // discard non-marker line
				}
			}
		} else {
			// collecting blocks — count complete blocks via double \n
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
	// count ready blocks across all ports
	int count = 0;
	for (int i = 0; i < N; i++) {
		if (port_type[i] == DIRECT   && ready[i])    count++;
		if (port_type[i] == UPSTREAM && up_ready[i]) count += up_expect[i];
	}
	if (count == 0) return;

	// send marker
	Serial8.print("---\t");
	Serial8.print(count);
	Serial8.print("\r\n");

	// send all ready blocks sequentially
	for (int i = 0; i < N; i++) {
		if (port_type[i] == DIRECT && ready[i]) {
			Serial8.write(buf[i], buf_len[i]);
			Serial8.flush();
			buf_len[i] = 0;
			ready[i]   = false;
			prev[i]    = 0;
		}
		if (port_type[i] == UPSTREAM && up_ready[i]) {
			Serial8.write(up_buf[i], up_len[i]);
			Serial8.flush();
			up_len[i]      = 0;
			up_ready[i]    = false;
			up_expect[i]   = 0;
			up_received[i] = 0;
			up_prev[i]     = 0;
		}
	}
}

void loop() {
	for (int i = 0; i < N; i++) {
		if      (port_type[i] == UNKNOWN)  detect_type(i);
		else if (port_type[i] == DIRECT)   read_direct(i);
		else if (port_type[i] == UPSTREAM) read_upstream(i);
	}
	send_blocks();
}
