// VE.Direct Aggregator — Arduino Mega 2560
// Serial1 RX → charger or upstream aggregator 1
// Serial2 RX → charger or upstream aggregator 2
// Serial3 RX → charger or upstream aggregator 3
// Serial0 TX → RS485 output
// Baud: 19200 on all ports
//
// Input type is detected automatically:
//   direct charger:      normal VE.Direct lines
//   upstream aggregator: starts with ---\tN marker
//
// Output marker before each packet:
//   ---\tN\r\n  (N = total number of blocks in this packet)
//
// Supports star topology and cascading up to 3 levels.

#define BAUD         19200
#define BUF_SIZE     512   // per direct charger port
#define UP_BUF_SIZE  2048  // per upstream port (fits up to ~9 blocks)
#define DETECT_LINES 3    // lines to read before deciding input type

HardwareSerial* ports[] = {&Serial1, &Serial2, &Serial3};
const int N = sizeof(ports) / sizeof(ports[0]);

// --- input type detection ---
enum PortType { UNKNOWN, DIRECT, UPSTREAM };
PortType port_type[3] = {UNKNOWN, UNKNOWN, UNKNOWN};
int  detect_cnt[3]  = {0, 0, 0};   // lines seen during detection
bool saw_marker[3] = {false, false, false};

// --- direct charger buffers ---
char  buf[3][BUF_SIZE];
int   buf_len[3] = {0, 0, 0};
bool  ready[3]   = {false, false, false};
char  prev[3]    = {0, 0, 0};

// --- upstream aggregator buffers ---
char  up_buf[3][UP_BUF_SIZE];
int   up_len[3]      = {0, 0, 0};
bool  up_ready[3]    = {false, false, false};
int   up_expect[3]   = {0, 0, 0};  // blocks expected from marker
int   up_received[3] = {0, 0, 0};  // blocks received so far
char  up_prev[3]     = {0, 0, 0};

void setup() {
	Serial.begin(BAUD);
	for (int i = 0; i < N; i++) {
		ports[i]->begin(BAUD);
	}
}

// read one complete line into dst, return true when \n received
bool read_line(int idx, char* dst, int& dst_len, int max_len, char& lp) {
	HardwareSerial* port = ports[idx];
	while (port->available()) {
		char c = port->read();
		if (dst_len < max_len - 1) dst[dst_len++] = c;
		if (c == '\n') { lp = c; return true; }
		lp = c;
	}
	return false;
}

// detect input type by watching first lines
void detect_type(int idx) {
	HardwareSerial* port = ports[idx];
	while (port->available()) {
		char c = port->read();
		if (c == '\n') {
			detect_cnt[idx]++;
			if (saw_marker[idx]) {
				port_type[idx] = UPSTREAM;
				return;
			}
			if (detect_cnt[idx] >= DETECT_LINES) {
				port_type[idx] = DIRECT;
				return;
			}
		}
		if (c == '-' && detect_cnt[idx] == 0) saw_marker[idx] = true;
	}
}

// read a direct charger port, detect block end via double \n
void read_direct(int idx) {
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
		prev[idx] = c;
		if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
	}
}

// read an upstream aggregator port
// waits for ---\tN marker, then collects N complete blocks
void read_upstream(int idx) {
	HardwareSerial* port = ports[idx];
	while (port->available() && !up_ready[idx]) {
		char c = port->read();

		if (up_expect[idx] == 0) {
			// waiting for ---\tN\r\n marker
			if (up_len[idx] < UP_BUF_SIZE - 1) up_buf[idx][up_len[idx]++] = c;
			if (c == '\n') {
				up_buf[idx][up_len[idx]] = '\0';
				// check if line starts with ---\t
				if (up_len[idx] >= 5 && up_buf[idx][0] == '-' && up_buf[idx][1] == '-' && up_buf[idx][2] == '-') {
					up_expect[idx]  = atoi(&up_buf[idx][4]);  // N after \t
					up_received[idx]= 0;
					up_len[idx]     = 0;                      // discard marker line
					up_prev[idx]    = 0;
				} else {
					up_len[idx] = 0;                          // discard garbage line
				}
			}
		} else {
			// collecting blocks, count complete blocks via double \n
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
	Serial.print("---\t");
	Serial.print(count);
	Serial.print("\r\n");

	// send all ready blocks
	for (int i = 0; i < N; i++) {
		if (port_type[i] == DIRECT && ready[i]) {
			Serial.write(buf[i], buf_len[i]);
			Serial.flush();
			buf_len[i] = 0;
			ready[i]   = false;
			prev[i]    = 0;
		}
		if (port_type[i] == UPSTREAM && up_ready[i]) {
			Serial.write(up_buf[i], up_len[i]);
			Serial.flush();
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
