// VE.Direct readtext -- v2.0 -- Teensy 4.1
//
// Reads N VE.Direct text streams (Serial1..7 RX) and multiplexes them
// into one aggregated stream on Serial8 TX or SerialUSB (19200 baud).
//
// Complete blocks are forwarded immediately; HEX frames stripped.
// Responds to WHO\n with READTEXT Teensy41 N=<N>\n
// Sends ALIVE\r\n every 10s when idle.
//
// Note: BSS138 level shifters required on all RX/TX pins (3.3V <-> 5V).
// Output consumed by vedirect_deaggregator.py for Venus OS integration.
// Serial1..7 RX -> devices, Serial8 or SerialUSB -> output
// Circular TX queue ? blocks dropped silently if queue full (receiver will notice).
//
// OUTPUT_USB 0 -> TX8 pin (Serial8)
// OUTPUT_USB 1 -> SerialUSB native USB (/dev/ttyACM0)

// Teensy 4.1: 512KB RAM ? large buffers
#define SERIAL_RX_BUFFER_SIZE 1024
  // ~427ms at 19200 baud per port

#define OUTPUT_USB     0
#define ALIVE_TIMEOUT  10000UL
#define BAUD_VEDIRECT  19200
#define BAUD_UPSTREAM  115200
#define BAUD_OUT       19200
#define BUF_SIZE       300          // max bytes per VE.Direct block
#define N              7            // number of input ports
#define Q_SIZE         12           // TX queue depth

// DS18B20 1-Wire temperature sensor
// Set TEMP_ENABLE to 0 to disable. Set TEMP_PIN to the DATA pin.
// Wiring: VCC->5V, GND->GND, DATA->TEMP_PIN, 4.7k pull-up 5V->DATA.
// Requires: OneWire + DallasTemperature (Arduino Library Manager).
#define TEMP_ENABLE     1
#define TEMP_PIN        2
#define TEMP_INTERVAL   5000UL

#if TEMP_ENABLE
#include <OneWire.h>
#include <DallasTemperature.h>
OneWire            temp_wire(TEMP_PIN);
DallasTemperature  temp_sensors(&temp_wire);
unsigned long      temp_last  = 0;
int                temp_count = 0;
#endif
#if BUF_SIZE > SERIAL_RX_BUFFER_SIZE
#error "BUF_SIZE exceeds SERIAL_RX_BUFFER_SIZE ? increase hardware RX buffer"
#endif

#if OUTPUT_USB
	#define SEROUT SerialUSB
#else
	#define SEROUT Serial8
#endif

HardwareSerial* ports[N] = {
	&Serial1, &Serial2, &Serial3, &Serial4,
	&Serial5, &Serial6, &Serial7
};

uint32_t port_baud[N] = {
	BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_VEDIRECT,
	BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_VEDIRECT
};

// -- receive buffers ----------------------------------------------------
char rx_buf[N][BUF_SIZE];
int  rx_len[N]  = {0, 0, 0, 0, 0, 0, 0};
int  rx_line[N] = {0, 0, 0, 0, 0, 0, 0};

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

// -- output state ------------------------------------------------------
int           out_pos   = 0;
unsigned long last_send = 0;


#if TEMP_ENABLE
void send_temp_blocks() {
	if (temp_count == 0) return;
	if (millis() - temp_last < TEMP_INTERVAL) return;
	temp_last = millis();
	for (int s = 0; s < temp_count; s++) {
		float t = temp_sensors.getTempCByIndex(s);
		if (t == DEVICE_DISCONNECTED_C) continue;
		char ser[24], tmp[16];
		sprintf(ser, "TEMP-P%d-S%d", TEMP_PIN, s);
		sprintf(tmp, "%.2f", t);
		char blk[256];
		int  pos = 0;
		pos += sprintf(blk + pos, "PID\t0x9999\r\n");
		pos += sprintf(blk + pos, "SER#\t%s\r\n", ser);
		pos += sprintf(blk + pos, "FW\t100\r\n");
		pos += sprintf(blk + pos, "TEMP\t%s\r\n", tmp);
		uint8_t sum = 0;
		for (int i = 0; i < pos; i++) sum += (uint8_t)blk[i];
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
	temp_sensors.requestTemperatures();
}
#endif

void setup() {
	SEROUT.begin(BAUD_OUT);
#if TEMP_ENABLE
	temp_sensors.begin();
	temp_count = temp_sensors.getDeviceCount();
	temp_sensors.setResolution(12);
	temp_sensors.setWaitForConversion(false);
	temp_sensors.requestTemperatures();
	temp_last = millis();
#endif
	for (int i = 0; i < N; i++) ports[i]->begin(port_baud[i]);
}

// -- receive ------------------------------------------------------------

void read_device(int idx) {
	while (ports[idx]->available()) {
		char c = ports[idx]->read();
		if (rx_len[idx] == 0 && (c == '\r' || c == '\n')) continue;
		if (rx_len[idx] >= BUF_SIZE - 1) {
			rx_len[idx]  = 0;
			rx_line[idx] = 0;
			continue;
		}
		rx_buf[idx][rx_len[idx]++] = c;
		if (c == '\n') {
			if (strncmp(&rx_buf[idx][rx_line[idx]], "Checksum\t", 9) == 0) {
					// stop before ':' ? HEX frames may be appended directly after the checksum byte
					int end = rx_line[idx] + 10;   // past "Checksum\t" (9) + checksum byte (1)
					while (end < rx_len[idx] && rx_buf[idx][end] != ':' && rx_buf[idx][end] != '\n') end++;
					if (end < rx_len[idx] && rx_buf[idx][end] == '\n') end++;
					q_push(rx_buf[idx], end);   // drop silently if full
					rx_len[idx]  = 0;
					rx_line[idx] = 0;
					continue;
				}
			rx_line[idx] = rx_len[idx];
		}
	}
}

// -- send --------------------------------------------------------------

void send_next() {
	if (q_empty()) return;
	while (out_pos < q_len[q_head]) {
		if (SEROUT.availableForWrite() == 0) return;
		SEROUT.write(q_buf[q_head][out_pos++]);
	}
	last_send = millis();
	out_pos   = 0;
	q_head    = (q_head + 1) % Q_SIZE;
}

void send_alive() {
	if (!q_empty()) return;
	if (millis() - last_send >= ALIVE_TIMEOUT) {
		SEROUT.print("ALIVE\r\n");
		last_send = millis();
	}
}

void read_cmd() {
	static char buf[16];
	static int  len = 0;
	while (Serial.available()) {
		char c = Serial.read();
		if (c == '\n' || c == '\r') {
			buf[len] = '\0';
			if (strcmp(buf, "WHO") == 0)
				SEROUT.print("READTEXT Teensy41 N=" + String(N) + "\n");
			len = 0;
		} else if (len < 15) {
			buf[len++] = c;
		}
	}
}

void loop() {
	read_cmd();
	send_next();
	for (int i = 0; i < N; i++) read_device(i);
	send_next();
#if TEMP_ENABLE
	send_temp_blocks();
	send_next();
#endif
	send_alive();
}
