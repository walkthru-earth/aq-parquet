# Wi-Fi transport host fixture

`pixi run wifi-link-test` includes the real `wifi_link.cpp` and runs it over
localhost TCP sockets. These headers replace Arduino Wi-Fi/mDNS, the monotonic
clock and FreeRTOS task creation; the mutexes use host timed mutexes. Test code
steps the server explicitly and provides in-memory settings and a silent log.

The fixture checks concurrent session isolation, bounded framed requests,
response routing, handshake/idle timing and LAN settings reconciliation. It
does not measure ESP32 radio coexistence, phone background behavior, RAM,
throughput or SD storage. The generic connectivity build and board builds
remain separate gates. The test may require permission for localhost sockets
in a restricted execution sandbox.
