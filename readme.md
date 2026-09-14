# Secure Real-Time Web-Based Stock Price Tracker

A complete, fully working C backend and Vanilla JS single-page application demonstrating a secure, concurrent, real-time stock price tracker using WebSockets and TLS.

## Quick Start

1. Install dependencies:
   - Ubuntu/Debian: `sudo apt install gcc libssl-dev libsqlite3-dev`
   - macOS: `brew install openssl sqlite3`
2. Generate TLS certificates:
   ```bash
   make cert
   ```
3. Compile the system:
   ```bash
   make
   ```
4. Start the server:
   ```bash
   make run
   ```
5. Open your browser and navigate to `http://localhost:8080`.
6. Enter `token123` to connect and view live stock prices.

## TCP State Analysis & Architecture Details

1. **Full TCP/WebSocket connection lifecycle:**
   - **SYN → SYN-ACK → ACK**: The standard three-way TCP handshake occurs first.
   - **TLS ClientHello → ServerHello → Certificate → Finished**: OpenSSL handles the TLS handshake over the established TCP socket.
   - **HTTP GET Upgrade → 101 Switching Protocols**: The client requests to switch from HTTP to WebSocket protocol. The server responds with `101 Switching Protocols` containing the Base64/SHA-1 encoded Sec-WebSocket-Accept key.
   - **WebSocket data frames**: Server streams stock prices using WebSocket framing (FIN=1, Opcode=0x1 text).
   - **FIN → FIN-ACK → ACK**: On graceful close, the client or server initiates a FIN packet, and the counterpart ACKs it before sending its own FIN. On an abrupt close, an `RST` is sent.

2. **How SO_REUSEADDR prevents TIME_WAIT blocking server restart:**
   - After a TCP connection is closed locally, it normally enters the `TIME_WAIT` state for ~2 MSL (Maximum Segment Lifetime) to ensure delayed packets aren't misrouted. `SO_REUSEADDR` allows the server to immediately bind to the 8443 port even if old connections are in `TIME_WAIT`.

3. **How TCP_NODELAY eliminates Nagle buffering:**
   - By default, Nagle's algorithm groups small packets to reduce network overhead. In a low-latency stock ticker, this delays updates. `TCP_NODELAY` immediately flushes small JSON price ticks over the network, ensuring real-time delivery.

4. **How SO_KEEPALIVE detects dead clients:**
   - Sends empty probe packets if the connection is idle. If a client drops offline abruptly (e.g., pulling the ethernet cable without a FIN/RST), `SO_KEEPALIVE` ensures the OS eventually detects the dead peer, prompting `SSL_read` to return an error, allowing the server to clean up thread resources without application-level heartbeats.

5. **How SO_SNDBUF/SO_RCVBUF affect throughput:**
   - Tuning the send and receive buffers provides enough kernel-level capacity to handle bursty stock price updates under load. If `SO_SNDBUF` is too small, writes to slow clients will block, affecting the broadcast loop.

6. **Persistent TCP connections vs HTTP polling:**
   - Polling requires full HTTP request/response headers, TCP handshakes, and TLS negotiation for every tick, causing massive overhead. Persistent WebSockets keep the pipe open, exchanging only minimal framing headers (2-10 bytes) per price update.

7. **Concurrency Control:**
   - The global `clients[]` array is protected by a single `pthread_mutex_t`, ensuring safe addition/removal of clients across threads, and preventing race conditions when the stock simulator thread broadcasts updates. SQLite writes are wrapped in `BEGIN/COMMIT` to maintain ACID compliance and batch write performance.

8. **Observing TCP states:**
   - You can observe active connections during runtime using: `ss -tan | grep 8443` or `netstat -an | grep 8443`.