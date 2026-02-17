#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef ESP_PLATFORM
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #include "esp_timer.h"
#else
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/socket.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
  #ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC 1
  #endif
#endif

#include "globals.h"
#include "varnum.h"
#include "procedures.h"
#include "tools.h"

#ifndef htonll
  static uint64_t htonll (uint64_t value) {
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value >> 32))) |
           ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32);
  #else
    return value;
  #endif
  }
#endif

// Total bytes read via recv_all; used for packet length reconciliation.
uint64_t total_bytes_received = 0;

ssize_t recv_all (int client_fd, void *buf, size_t n, uint8_t require_first) {
  char *p = buf;
  size_t total = 0;

  // Timestamp of last successful socket progress.
  int64_t last_update_time = get_program_time();

  // Optionally return early when no first byte is immediately available.
  if (require_first) {
    ssize_t r = recv(client_fd, p, 1, MSG_PEEK);
    if (r <= 0) {
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0; // No first byte available yet.
      }
      return -1; // Socket error or closed connection.
    }
  }

  // Keep reading until exactly n bytes are received.
  while (total < n) {
    ssize_t r = recv(client_fd, p + total, n - total, 0);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Enforce I/O timeout while socket is stalled.
        if (get_program_time() - last_update_time > NETWORK_TIMEOUT_TIME) {
          disconnectClient(&client_fd, -1);
          return -1;
        }
        task_yield();
        continue;
      } else {
        total_bytes_received += total;
        return -1; // Unrecoverable socket error.
      }
    } else if (r == 0) {
      // Peer closed connection before full read.
      total_bytes_received += total;
      return total;
    }
    total += r;
    last_update_time = get_program_time();
  }

  total_bytes_received += total;
  return total; // Full read completed.
}

ssize_t send_all (int client_fd, const void *buf, ssize_t len) {
  // Serialize buffer as raw bytes.
  const uint8_t *p = (const uint8_t *)buf;
  ssize_t sent = 0;

  // Timestamp of last successful socket progress.
  int64_t last_update_time = get_program_time();

  // Keep sending until all bytes are written.
  while (sent < len) {
    #ifdef _WIN32
      ssize_t n = send(client_fd, p + sent, len - sent, 0);
    #else
      ssize_t n = send(client_fd, p + sent, len - sent, MSG_NOSIGNAL);
    #endif
    if (n > 0) { // Some data was sent.
      sent += n;
      last_update_time = get_program_time();
      continue;
    }
    if (n == 0) { // Peer closed connection.
      errno = ECONNRESET;
      return -1;
    }
    // Retry on transient socket conditions.
    #ifdef _WIN32 // Handle WinSock non-blocking retry codes.
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
    #else
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    #endif
      // Enforce I/O timeout while socket is stalled.
      if (get_program_time() - last_update_time > NETWORK_TIMEOUT_TIME) {
        disconnectClient(&client_fd, -2);
        return -1;
      }
      task_yield();
      continue;
    }
    return -1; // Unrecoverable socket error.
  }

  return sent;
}

void discard_all (int client_fd, size_t remaining, uint8_t require_first) {
  while (remaining > 0) {
    size_t recv_n = remaining > MAX_RECV_BUF_LEN ? MAX_RECV_BUF_LEN : remaining;
    ssize_t received = recv_all(client_fd, recv_buffer, recv_n, require_first);
    if (received < 0) return;
    if (received > remaining) return;
    remaining -= received;
    require_first = false;
  }
}

ssize_t writeByte (int client_fd, uint8_t byte) {
  return send_all(client_fd, &byte, 1);
}
ssize_t writeUint16 (int client_fd, uint16_t num) {
  uint16_t be = htons(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint32 (int client_fd, uint32_t num) {
  uint32_t be = htonl(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint64 (int client_fd, uint64_t num) {
  uint64_t be = htonll(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeFloat (int client_fd, float num) {
  uint32_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonl(bits);
  return send_all(client_fd, &bits, sizeof(bits));
}
ssize_t writeDouble (int client_fd, double num) {
  uint64_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonll(bits);
  return send_all(client_fd, &bits, sizeof(bits));
}

uint8_t readByte (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 1, false);
  return recv_buffer[0];
}
uint16_t readUint16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((uint16_t)recv_buffer[0] << 8) | recv_buffer[1];
}
int16_t readInt16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((int16_t)recv_buffer[0] << 8) | (int16_t)recv_buffer[1];
}
uint32_t readUint32 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 4, false);
  return ((uint32_t)recv_buffer[0] << 24) |
         ((uint32_t)recv_buffer[1] << 16) |
         ((uint32_t)recv_buffer[2] << 8) |
         ((uint32_t)recv_buffer[3]);
}
uint64_t readUint64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((uint64_t)recv_buffer[0] << 56) |
         ((uint64_t)recv_buffer[1] << 48) |
         ((uint64_t)recv_buffer[2] << 40) |
         ((uint64_t)recv_buffer[3] << 32) |
         ((uint64_t)recv_buffer[4] << 24) |
         ((uint64_t)recv_buffer[5] << 16) |
         ((uint64_t)recv_buffer[6] << 8) |
         ((uint64_t)recv_buffer[7]);
}
int64_t readInt64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((int64_t)recv_buffer[0] << 56) |
         ((int64_t)recv_buffer[1] << 48) |
         ((int64_t)recv_buffer[2] << 40) |
         ((int64_t)recv_buffer[3] << 32) |
         ((int64_t)recv_buffer[4] << 24) |
         ((int64_t)recv_buffer[5] << 16) |
         ((int64_t)recv_buffer[6] << 8) |
         ((int64_t)recv_buffer[7]);
}
float readFloat (int client_fd) {
  uint32_t bytes = readUint32(client_fd);
  float output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}
double readDouble (int client_fd) {
  uint64_t bytes = readUint64(client_fd);
  double output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}

// Reads a length-prefixed payload into recv_buffer with bounds checking.
ssize_t readLengthPrefixedData (int client_fd) {
  uint32_t length = readVarInt(client_fd);
  if (length >= MAX_RECV_BUF_LEN) {
    printf("ERROR: Received length (%u) exceeds maximum (%u)\n", length, MAX_RECV_BUF_LEN);
    disconnectClient(&client_fd, -1);
    recv_count = 0;
    return 0;
  }
  return recv_all(client_fd, recv_buffer, length, false);
}

// Reads a protocol string into recv_buffer.
void readString (int client_fd) {
  recv_count = readLengthPrefixedData(client_fd);
  recv_buffer[recv_count] = '\0';
}
// Reads a protocol string capped at max_length bytes.
void readStringN (int client_fd, uint32_t max_length) {
  // Fallback to uncapped path if cap exceeds buffer size.
  if (max_length >= MAX_RECV_BUF_LEN) {
    readString(client_fd);
    return;
  }
  // Read full string if it is within cap.
  uint32_t length = readVarInt(client_fd);
  if (max_length > length) {
    recv_count = recv_all(client_fd, recv_buffer, length, false);
    recv_buffer[recv_count] = '\0';
    return;
  }
  // Read up to cap, discard remaining bytes from wire.
  recv_count = recv_all(client_fd, recv_buffer, max_length, false);
  recv_buffer[recv_count] = '\0';
  uint8_t dummy;
  for (uint32_t i = max_length; i < length; i ++) {
    recv_all(client_fd, &dummy, 1, false);
  }
}

uint32_t fast_rand () {
  rng_seed ^= rng_seed << 13;
  rng_seed ^= rng_seed >> 17;
  rng_seed ^= rng_seed << 5;
  return rng_seed;
}

uint64_t splitmix64 (uint64_t state) {
  uint64_t z = state + 0x9e3779b97f4a7c15;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
  z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
  return z ^ (z >> 31);
}

#ifndef ESP_PLATFORM
// Returns monotonic time in microseconds for interval measurements.
int64_t get_program_time () {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}
#endif
