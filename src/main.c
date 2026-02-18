#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifdef ESP_PLATFORM
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "nvs_flash.h"
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_timer.h"
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
#else
  #include <sys/types.h>
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/stat.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "packets.h"
#include "worldgen.h"
#include "registries.h"
#include "procedures.h"
#include "serialize.h"

static uint8_t templateChunkCompatActive () {
  #ifdef CHUNK_TEMPLATE_VISIBILITY_COMPAT
    const char *enable_env = getenv("NETHR_ENABLE_TEMPLATE_CHUNKS");
    return (enable_env != NULL && enable_env[0] == '1');
  #else
    return false;
  #endif
}

static uint8_t parseSeedOverride (const char *env_name, uint32_t *out) {
  const char *value = getenv(env_name);
  if (value == NULL || value[0] == '\0') return false;
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (end == NULL || *end != '\0') return false;
  *out = (uint32_t)parsed;
  return true;
}

static uint8_t parseIntOverride (const char *env_name, int *out) {
  const char *value = getenv(env_name);
  if (value == NULL || value[0] == '\0') return false;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if (end == NULL || *end != '\0') return false;
  *out = (int)parsed;
  return true;
}

static uint8_t shouldLogPlayRxPacket (int packet_id) {
  // High-frequency packets that create log spam during normal play.
  if (packet_id == 0x00) return false; // Accept teleportation
  if (packet_id == 0x0C) return false; // Client tick
  if (packet_id == 0x19) return false; // Interact
  if (packet_id == 0x28) return false; // Player action (dig/place related loop)
  if (packet_id == 0x29) return false; // Player command
  if (packet_id == 0x2A) return false; // Player input
  if (packet_id == 0x2B) return false; // Client status
  if (packet_id == 0x3C) return false; // Swing arm / animation intent
  if (packet_id == 0x1B) return false; // Keep-alive response
  if (packet_id == 0x1D) return false; // Set player position
  if (packet_id == 0x1E) return false; // Set player position + rotation
  if (packet_id == 0x1F) return false; // Set player rotation
  if (packet_id == 0x20) return false; // Set movement flags
  return true;
}

#if !defined(ESP_PLATFORM) && !defined(_WIN32)

#define ADMIN_PIPE_PATH "/tmp/nethr-admin.pipe"
#define ADMIN_PIPE_PREFIX "§c[SYSTEM] "
#define ADMIN_PIPE_READ_SIZE 256
#define ADMIN_PIPE_MAX_LINE 220

static int admin_pipe_fd = -1;
static char admin_pipe_line[ADMIN_PIPE_MAX_LINE];
static size_t admin_pipe_line_len = 0;

static void broadcastSystemMessage (const char *message, size_t len) {
  if (len == 0) return;

  char out[sizeof(ADMIN_PIPE_PREFIX) + ADMIN_PIPE_MAX_LINE];
  size_t prefix_len = sizeof(ADMIN_PIPE_PREFIX) - 1;
  if (len > ADMIN_PIPE_MAX_LINE) len = ADMIN_PIPE_MAX_LINE;

  memcpy(out, ADMIN_PIPE_PREFIX, prefix_len);
  memcpy(out + prefix_len, message, len);

  FOR_EACH_VISIBLE_PLAYER(i) {
    sc_systemChat(player_data[i].client_fd, out, (uint16_t)(prefix_len + len));
  }
}

static void flushAdminPipeLine () {
  while (admin_pipe_line_len > 0 &&
    (admin_pipe_line[admin_pipe_line_len - 1] == '\n' || admin_pipe_line[admin_pipe_line_len - 1] == '\r')
  ) admin_pipe_line_len --;

  if (admin_pipe_line_len > 0) {
    broadcastSystemMessage(admin_pipe_line, admin_pipe_line_len);
  }

  admin_pipe_line_len = 0;
}

static void pollAdminPipe () {
  if (admin_pipe_fd == -1) return;

  char read_buf[ADMIN_PIPE_READ_SIZE];
  while (true) {
    ssize_t received = read(admin_pipe_fd, read_buf, sizeof(read_buf));
    if (received <= 0) {
      if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("admin pipe read failed");
      }
      break;
    }

    for (ssize_t i = 0; i < received; i ++) {
      if (read_buf[i] == '\n') {
        flushAdminPipeLine();
      } else if (read_buf[i] != '\r' && admin_pipe_line_len < ADMIN_PIPE_MAX_LINE) {
        admin_pipe_line[admin_pipe_line_len++] = read_buf[i];
      }
    }
  }
}

static void initAdminPipe () {
  struct stat st;
  if (stat(ADMIN_PIPE_PATH, &st) == 0) {
    if (!S_ISFIFO(st.st_mode)) {
      fprintf(stderr, "admin pipe path exists but is not a FIFO: %s\n", ADMIN_PIPE_PATH);
      return;
    }
  } else if (errno == ENOENT) {
    if (mkfifo(ADMIN_PIPE_PATH, 0600) != 0) {
      perror("mkfifo failed");
      return;
    }
  } else {
    perror("stat admin pipe failed");
    return;
  }

  chmod(ADMIN_PIPE_PATH, 0600);
  admin_pipe_fd = open(ADMIN_PIPE_PATH, O_RDWR | O_NONBLOCK);
  if (admin_pipe_fd == -1) {
    perror("open admin pipe failed");
    return;
  }

  printf("Admin pipe ready: %s\n", ADMIN_PIPE_PATH);
}

static void shutdownAdminPipe () {
  if (admin_pipe_fd != -1) close(admin_pipe_fd);
}

#endif

/* Dispatches one parsed packet to its state-specific handler. */
void handlePacket (int client_fd, int length, int packet_id, int state) {

  // Track bytes consumed while this packet is processed.
  uint64_t bytes_received_start = total_bytes_received;

  switch (packet_id) {

    case 0x00:
      if (state == STATE_NONE) {
        if (cs_handshake(client_fd)) break;
      } else if (state == STATE_STATUS) {
        if (sc_statusResponse(client_fd)) break;
      } if (state == STATE_LOGIN) {
        uint8_t uuid[16];
        char name[16];
        if (cs_loginStart(client_fd, uuid, name)) break;
        if (reservePlayerData(client_fd, uuid, name)) {
          recv_count = 0;
          return;
        }
        if (sc_loginSuccess(client_fd, uuid, name)) break;
      } else if (state == STATE_CONFIGURATION) {
        if (cs_clientInformation(client_fd)) break;
      } else if (state == STATE_PLAY) {
        cs_acceptTeleportation(client_fd);
      }
      break;

    case 0x01:
      // Status ping: echo payload and close.
      if (state == STATE_STATUS) {
        writeByte(client_fd, 9);
        writeByte(client_fd, 0x01);
        writeUint64(client_fd, readUint64(client_fd));
        // Mark intentional close after status pong.
        recv_count = -2;
        return;
      }
      break;

    case 0x02:
      if (state == STATE_CONFIGURATION) cs_pluginMessage(client_fd);
      break;

    case 0x03:
      if (state == STATE_LOGIN) {
        printf("Client Acknowledged Login\n\n");
        setClientState(client_fd, STATE_CONFIGURATION);
        #ifdef SEND_BRAND
        if (sc_sendPluginMessage(client_fd, "minecraft:brand", (uint8_t *)brand, brand_len)) break;
        #endif
        if (sc_updateEnabledFeatures(client_fd)) break;
        if (sc_knownPacks(client_fd)) break;
      } else if (state == STATE_CONFIGURATION) {
        printf("Client Acknowledged Configuration\n\n");
        printf("Transitioning client %d to PLAY; sending initial play packets\n\n", client_fd);

        // Promote client to PLAY and send initial world/player state.
        setClientState(client_fd, STATE_PLAY);
        sc_loginPlay(client_fd);
        #ifdef DEBUG_LOGIN_ONLY
          printf("DEBUG_LOGIN_ONLY active: not sending spawn/chunk packets after Play Login\n\n");
          break;
        #endif

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        spawnPlayer(player);

        // Register already connected players for this client.
        FOR_EACH_VISIBLE_PLAYER(i) {
          sc_playerInfoUpdateAddPlayer(client_fd, player_data[i]);
          sc_spawnEntityPlayer(client_fd, player_data[i]);
        }

        if (!templateChunkCompatActive()) {
          // Spawn currently allocated mobs for this client in procedural mode.
          uint8_t uuid[16];
          uint32_t r = fast_rand();
          memcpy(uuid, &r, 4);
          // Reuse mob index as stable UUID suffix.
          for (int i = 0; i < MAX_MOBS; i ++) {
            if (mob_data[i].type == 0) continue;
            if ((mob_data[i].data & 31) == 0) continue;
            memcpy(uuid + 4, &i, 4);
            sc_spawnEntity(
              client_fd, -2 - i, uuid,
              mob_data[i].type, mob_data[i].x, mob_data[i].y, mob_data[i].z,
              0, 0
            );
            broadcastMobMetadata(client_fd, -2 - i);
          }
        }

      }
      break;

    case 0x07:
      if (state == STATE_CONFIGURATION) {
        if (cs_knownPacks(client_fd, length)) break;
        printf("Sending required Registry/Tags transfer for PLAY login holder decoding\n\n");
        if (sc_registries(client_fd)) break;
        sc_finishConfiguration(client_fd);
      }
      break;

    case 0x08:
      if (state == STATE_PLAY) cs_chat(client_fd);
      break;

    case 0x0B:
      if (state == STATE_PLAY) cs_clientStatus(client_fd);
      break;

    case 0x0C: // Client tick (unused).
      break;

    case 0x0A:
      if (state == STATE_PLAY) cs_chunkBatchReceived(client_fd);
      break;

    case 0x11:
      if (state == STATE_PLAY) cs_clickContainer(client_fd);
      break;

    case 0x12:
      if (state == STATE_PLAY) cs_closeContainer(client_fd);
      break;

    case 0x1B:
      if (state == STATE_PLAY) {
        // Serverbound keep-alive is ignored.
        discard_all(client_fd, length, false);
      }
      break;

    case 0x19:
      if (state == STATE_PLAY) cs_interact(client_fd);
      break;

    case 0x1D:
    case 0x1E:
    case 0x1F:
    case 0x20:
      if (state == STATE_PLAY) {

        double x, y, z;
        float yaw, pitch;
        uint8_t on_ground;

        // Decode movement payload variant.
        if (packet_id == 0x1D) cs_setPlayerPosition(client_fd, &x, &y, &z, &on_ground);
        else if (packet_id == 0x1F) cs_setPlayerRotation (client_fd, &yaw, &pitch, &on_ground);
        else if (packet_id == 0x20) cs_setPlayerMovementFlags (client_fd, &on_ground);
        else cs_setPlayerPositionAndRotation(client_fd, &x, &y, &z, &yaw, &pitch, &on_ground);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        uint8_t block_feet = getBlockAt(player->x, player->y, player->z);
        uint8_t swimming = block_feet >= B_water && block_feet < B_water + 8;

        // Apply basic fall-damage logic.
        if (on_ground) {
          int16_t damage = player->grounded_y - player->y - 3;
          if (damage > 0 && (GAMEMODE == 0 || GAMEMODE == 2) && !swimming) {
            hurtEntity(client_fd, -1, D_fall, damage);
          }
          player->grounded_y = player->y;
        } else if (swimming) {
          player->grounded_y = player->y;
        }

        // Movement flags only.
        if (packet_id == 0x20) break;

        // Update stored rotation when present in packet.
        if (packet_id != 0x1D) {
          player->yaw = ((short)(yaw + 540) % 360 - 180) * 127 / 180;
          player->pitch = pitch / 90.0f * 127.0f;
        }

        // Control whether this update is rebroadcast to other clients.
        uint8_t should_broadcast = true;

        #ifndef BROADCAST_ALL_MOVEMENT
          // Limit movement rebroadcasts to once per tick.
          should_broadcast = !(player->flags & 0x40);
          if (should_broadcast) player->flags |= 0x40;
        #endif

        #ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
          // Increase throttling as player count grows.
          if (++player->packets_since_update < client_count) {
            should_broadcast = false;
          } else {
            // Keep existing should_broadcast decision and reset cadence.
            player->packets_since_update = 0;
          }
        #endif

        if (should_broadcast) {
          // Derive missing rotation fields from cached player state.
          if (packet_id == 0x1D) {
            yaw = player->yaw * 180 / 127;
            pitch = player->pitch * 90 / 127;
          }
          // Broadcast movement to visible clients.
          FOR_EACH_VISIBLE_OTHER_PLAYER(i, client_fd) {
            if (packet_id == 0x1F) {
              sc_updateEntityRotation(player_data[i].client_fd, client_fd, player->yaw, player->pitch);
            } else {
              double old_x = (double)player->x + (player->x >= 0 ? 0.5 : -0.5);
              double old_z = (double)player->z + (player->z >= 0 ? 0.5 : -0.5);
              double old_y = (double)player->y;
              sc_moveEntityPosRot(
                player_data[i].client_fd, client_fd,
                old_x, old_y, old_z,
                x, y, z,
                player->yaw, player->pitch
              );
            }
            sc_setHeadRotation(player_data[i].client_fd, client_fd, player->yaw);
          }
        }

        // Rotation-only update.
        if (packet_id == 0x1F) break;

        // Approximate hunger drain by movement-packet frequency.
        if (player->saturation == 0) {
          if (player->hunger > 0) player->hunger--;
          player->saturation = 200;
          sc_setHealth(client_fd, player->health, player->hunger, player->saturation);
        } else if (player->flags & 0x08) {
          player->saturation -= 1;
        }

        // Quantize to block coordinates.
        short cx = x, cy = y, cz = z;
        if (x < 0) cx -= 1;
        if (z < 0) cz -= 1;
        // Compute current chunk coordinates.
        short _x = (cx < 0 ? cx - 16 : cx) / 16, _z = (cz < 0 ? cz - 16 : cz) / 16;
        // Compute chunk delta from previous position.
        short dx = _x - (player->x < 0 ? player->x - 16 : player->x) / 16;
        short dz = _z - (player->z < 0 ? player->z - 16 : player->z) / 16;

        // Clamp vertical position to world bounds.
        if (cy < 0) {
          cy = 0;
          player->grounded_y = 0;
          sc_synchronizePlayerPosition(client_fd, cx, 0, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        } else if (cy > 255) {
          cy = 255;
          sc_synchronizePlayerPosition(client_fd, cx, 255, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        }

        // Persist quantized position.
        player->x = cx;
        player->y = cy;
        player->z = cz;

        // Stop if player stayed in the same chunk.
        if (dx == 0 && dz == 0) break;

        // Avoid re-sending recently visited chunks.
        int found = false;
        for (int i = 0; i < VISITED_HISTORY; i ++) {
          if (player->visited_x[i] == _x && player->visited_z[i] == _z) {
            found = true;
            break;
          }
        }
        if (found) break;

        // Shift visit history and append current chunk.
        for (int i = 0; i < VISITED_HISTORY - 1; i ++) {
          player->visited_x[i] = player->visited_x[i + 1];
          player->visited_z[i] = player->visited_z[i + 1];
        }
        player->visited_x[VISITED_HISTORY - 1] = _x;
        player->visited_z[VISITED_HISTORY - 1] = _z;

        if (!templateChunkCompatActive()) {
          // Dynamic mob spawning stays active in procedural mode.
          uint32_t r = fast_rand();
          uint8_t in_nether_zone = player->z >= NETHER_ZONE_OFFSET;
          // Gate spawn attempts to preserve tick stability.
          if (r % PASSIVE_SPAWN_CHANCE == 0) {
            // Spawn candidate near chunk edge in movement direction.
            short mob_x = (_x + dx * view_distance) * 16 + ((r >> 4) & 15);
            short mob_z = (_z + dz * view_distance) * 16 + ((r >> 8) & 15);
            // Search upward for a valid spawn column.
            uint8_t mob_y = cy - 8;
            uint8_t b_low = getBlockAt(mob_x, mob_y - 1, mob_z);
            uint8_t b_mid = getBlockAt(mob_x, mob_y, mob_z);
            uint8_t b_top = getBlockAt(mob_x, mob_y + 1, mob_z);
            while (mob_y < 255) {
              if ( // Require solid ground and free blocks at feet/head.
                !isPassableBlock(b_low) &&
                isPassableSpawnBlock(b_mid) &&
                isPassableSpawnBlock(b_top)
              ) break;
              b_low = b_mid;
              b_mid = b_top;
              b_top = getBlockAt(mob_x, mob_y + 2, mob_z);
              mob_y ++;
            }
            if (mob_y != 255) {
              // Spawn passives by day above ground, hostiles otherwise.
              if ((world_time < 13000 || world_time > 23460) && mob_y > 48) {
                if (in_nether_zone) {
                  // Keep nether-zone population sparse.
                  if ((r >> 12) & 1) spawnMob(ENTITY_TYPE_ZOMBIE, mob_x, mob_y, mob_z, 20); // Zombie stand-in
                } else {
                  uint32_t mob_choice = (r >> 12) % 5;
                  if (mob_choice == 0) spawnMob(ENTITY_TYPE_CHICKEN, mob_x, mob_y, mob_z, 4); // Chicken
                  else if (mob_choice == 1) spawnMob(ENTITY_TYPE_COW, mob_x, mob_y, mob_z, 10); // Cow
                  else if (mob_choice == 2) spawnMob(ENTITY_TYPE_PIG, mob_x, mob_y, mob_z, 10); // Pig
                  else if (mob_choice == 3) spawnMob(ENTITY_TYPE_SHEEP, mob_x, mob_y, mob_z, 8); // Sheep
                  else if (getMobCountByType(ENTITY_TYPE_VILLAGER) < MAX_VILLAGERS) {
                    spawnMob(ENTITY_TYPE_VILLAGER, mob_x, mob_y, mob_z, 20); // Villager
                  } else {
                    spawnMob(ENTITY_TYPE_COW, mob_x, mob_y, mob_z, 10); // Cow fallback
                  }
                }
              } else if (!in_nether_zone || ((r >> 13) & 1)) {
                spawnMob(ENTITY_TYPE_ZOMBIE, mob_x, mob_y, mob_z, 20); // Zombie
              }
            }
          }
        }

        int count = 0;
        #ifdef DEV_LOG_CHUNK_GENERATION
          printf("Sending new chunks (%d, %d)\n", _x, _z);
          clock_t start, end;
          start = clock();
        #endif

        sc_setCenterChunk(client_fd, _x, _z);

        while (dx != 0) {
          sc_chunkDataAndUpdateLight(client_fd, _x + dx * view_distance, _z);
          count ++;
          for (int i = 1; i <= view_distance; i ++) {
            sc_chunkDataAndUpdateLight(client_fd, _x + dx * view_distance, _z - i);
            sc_chunkDataAndUpdateLight(client_fd, _x + dx * view_distance, _z + i);
            count += 2;
          }
          dx += dx > 0 ? -1 : 1;
        }
        while (dz != 0) {
          sc_chunkDataAndUpdateLight(client_fd, _x, _z + dz * view_distance);
          count ++;
          for (int i = 1; i <= view_distance; i ++) {
            sc_chunkDataAndUpdateLight(client_fd, _x - i, _z + dz * view_distance);
            sc_chunkDataAndUpdateLight(client_fd, _x + i, _z + dz * view_distance);
            count += 2;
          }
          dz += dz > 0 ? -1 : 1;
        }

        #ifdef DEV_LOG_CHUNK_GENERATION
          end = clock();
          double total_ms = (double)(end - start) / CLOCKS_PER_SEC * 1000;
          printf("Generated %d chunks in %.0f ms (%.2f ms per chunk)\n", count, total_ms, total_ms / (double)count);
        #endif

      }
      break;

    case 0x29:
      if (state == STATE_PLAY) cs_playerCommand(client_fd);
      break;

    case 0x2A:
      if (state == STATE_PLAY) cs_playerInput(client_fd);
      break;

    case 0x2B:
      if (state == STATE_PLAY) cs_playerLoaded(client_fd);
      break;

    case 0x34:
      if (state == STATE_PLAY) cs_setHeldItem(client_fd);
      break;
	
    case 0x3C:
      if (state == STATE_PLAY) cs_swingArm(client_fd);
      break;

    case 0x28:
      if (state == STATE_PLAY) cs_playerAction(client_fd);
      break;

    case 0x3F:
      if (state == STATE_PLAY) cs_useItemOn(client_fd);
      break;

    case 0x40:
      if (state == STATE_PLAY) cs_useItem(client_fd);
      break;

    default:
      #ifdef DEV_LOG_UNKNOWN_PACKETS
        printf("Unknown packet: 0x");
        if (packet_id < 16) printf("0");
        printf("%X, length: %d, state: %d\n\n", packet_id, length, state);
      #endif
      discard_all(client_fd, length, false);
      break;

  }

  // Recover stream alignment if handler consumed fewer bytes than expected.
  int processed_length = total_bytes_received - bytes_received_start;
  if (processed_length == length) return;

  if (length > processed_length) {
    discard_all(client_fd, length - processed_length, false);
  }

  #ifdef DEV_LOG_LENGTH_DISCREPANCY
  printf("WARNING: Packet 0x");
  if (packet_id < 16) printf("0");
  printf("%X parsed incorrectly!\n  Expected: %d, parsed: %d\n\n", packet_id, length, processed_length);
  #endif
  #ifdef DEV_LOG_UNKNOWN_PACKETS
  if (processed_length == 0) {
    printf("Unknown packet: 0x");
    if (packet_id < 16) printf("0");
    printf("%X, length: %d, state: %d\n\n", packet_id, length, state);
  }
  #endif

}

static const char *stateName (int state) {
  switch (state) {
    case STATE_NONE: return "none";
    case STATE_STATUS: return "status";
    case STATE_LOGIN: return "login";
    case STATE_TRANSFER: return "transfer";
    case STATE_CONFIGURATION: return "configuration";
    case STATE_PLAY: return "play";
    default: return "unknown";
  }
}

static void logDisconnectContext (
  const char *where, int client_fd, int cause, int state, int length, int packet_id, ssize_t recv_result
) {
#ifdef _WIN32
  int socket_errno = WSAGetLastError();
  printf(
    "Disconnect context (%s): fd=%d cause=%d state=%d(%s) length=%d packet_id=%d recv=%zd wsa=%d\n",
    where, client_fd, cause, state, stateName(state), length, packet_id, recv_result, socket_errno
  );
#else
  int socket_errno = errno;
  printf(
    "Disconnect context (%s): fd=%d cause=%d state=%d(%s) length=%d packet_id=%d recv=%zd errno=%d (%s)\n",
    where, client_fd, cause, state, stateName(state), length, packet_id, recv_result, socket_errno, strerror(socket_errno)
  );
#endif
}

int main () {
  #ifdef _WIN32 // Initialize WinSock.
    WSADATA wsa;
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(EXIT_FAILURE);
      }
  #endif

  int meta_status = loadWorldMeta();
  if (meta_status == -1) {
    printf("WARNING: Failed to parse world.meta, using built-in seed defaults\n");
  }

  if (parseSeedOverride("NETHR_WORLD_SEED", &world_seed_raw)) {
    printf("Seed override: NETHR_WORLD_SEED=%u\n", world_seed_raw);
  }
  if (parseSeedOverride("NETHR_RNG_SEED", &rng_seed_raw)) {
    printf("Seed override: NETHR_RNG_SEED=%u\n", rng_seed_raw);
  }
  int view_distance_override = 0;
  if (parseIntOverride("NETHR_VIEW_DISTANCE", &view_distance_override)) {
    if (view_distance_override < 2) view_distance_override = 2;
    if (view_distance_override > 16) view_distance_override = 16;
    view_distance = view_distance_override;
    printf("View distance override: NETHR_VIEW_DISTANCE=%d\n", view_distance);
  }

  // Hash runtime seeds before first use.
  world_seed = splitmix64(world_seed_raw);
  rng_seed = splitmix64(rng_seed_raw);

  printf("World seed (raw): %u\n", world_seed_raw);
  printf("RNG seed (raw): %u\n", rng_seed_raw);
  printf("World seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((world_seed >> (8 * i)) & 255));
  printf("\nRNG seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((rng_seed >> (8 * i)) & 255));
  printf("\n");
  if (world_spawn_locked) {
    printf("World spawn (from meta): x=%d y=%u z=%d\n", world_spawn_x, world_spawn_y, world_spawn_z);
  }
  printf("View distance: %d\n", view_distance);
  printf("\n");

  // Mark all block-change slots as unused.
  for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
    block_changes[i].block = 0xFF;
  }
  invalidateBlockChangeIndex();

  // Initialize persistence backend when enabled.
  if (initSerializer()) exit(EXIT_FAILURE);
  ensureWorldSpawn();
  saveWorldMeta();

  // Initialize client slots and state tables.
  int clients[MAX_PLAYERS], client_index = 0;
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    clients[i] = -1;
    client_states[i * 2] = -1;
    player_data[i].client_fd = -1;
  }

  // Create listening TCP socket.
  int server_fd, opt = 1;
  struct sockaddr_in server_addr, client_addr;
  socklen_t addr_len = sizeof(client_addr);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }
#ifdef _WIN32
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
      (const char*)&opt, sizeof(opt)) < 0) {
#else
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif    
    perror("socket options failed");
    exit(EXIT_FAILURE);
  }

  // Bind socket to configured port.
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  // Start listening for incoming connections.
  if (listen(server_fd, 5) < 0) {
    perror("listen failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }
  printf("Server listening on port %d...\n", PORT);
  printf("Build marker: chunk-v7-template-pool\n");

  // Use non-blocking I/O to avoid stalling the main loop.
  #ifdef _WIN32
    u_long mode = 1; // 1 = non-blocking.
    if (ioctlsocket(server_fd, FIONBIO, &mode) != 0) {
      fprintf(stderr, "Failed to set non-blocking mode\n");
      exit(EXIT_FAILURE);
    }
  #else
  int flags = fcntl(server_fd, F_GETFL, 0);
  fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
  #endif

  #if !defined(ESP_PLATFORM) && !defined(_WIN32)
    initAdminPipe();
  #endif

  // Timestamp of last completed server tick.
  int64_t last_tick_time = get_program_time();

  // Main loop: accept connections, process admin pipe, service one client packet.
  while (true) {
    // Yield to scheduler/idle task when applicable.
    task_yield();

    // Try to accept one new connection.
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (clients[i] != -1) continue;
      clients[i] = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
      // Put accepted client socket in non-blocking mode.
      if (clients[i] != -1) {
        printf("New client, fd: %d\n", clients[i]);
      #ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(clients[i], FIONBIO, &mode);
      #else
        int flags = fcntl(clients[i], F_GETFL, 0);
        fcntl(clients[i], F_SETFL, flags | O_NONBLOCK);
      #endif
        client_count ++;
      }
      break;
    }

    #if !defined(ESP_PLATFORM) && !defined(_WIN32)
      pollAdminPipe();
      flush_all_send_buffers();
    #endif

    // Round-robin through active client slots.
    client_index ++;
    if (client_index == MAX_PLAYERS) client_index = 0;
    if (clients[client_index] == -1) continue;

    // Run server tick at configured interval.
    int64_t time_since_last_tick = get_program_time() - last_tick_time;
    if (time_since_last_tick > TIME_BETWEEN_TICKS) {
      handleServerTick(time_since_last_tick);
      flush_all_send_buffers();
      last_tick_time = get_program_time();
    }

    // Process one packet from selected client.
    int client_fd = clients[client_index];
    int state = getClientState(client_fd);
    int length = -1;
    int packet_id = -1;

    // Ensure at least two bytes are available before parsing VarInts.
    #ifdef _WIN32
    recv_count = recv(client_fd, recv_buffer, 2, MSG_PEEK);
    if (recv_count == 0) {
      logDisconnectContext("peek", client_fd, 1, state, length, packet_id, recv_count);
      disconnectClient(&clients[client_index], 1);
      continue;
    }
    if (recv_count == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK) {
        continue; // No data yet, keep client alive.
      } else {
        logDisconnectContext("peek", client_fd, 1, state, length, packet_id, recv_count);
        disconnectClient(&clients[client_index], 1);
        continue;
      }
    }
    #else
    recv_count = recv(client_fd, &recv_buffer, 2, MSG_PEEK);
    if (recv_count < 2) {
      if (recv_count == 0 || (recv_count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        logDisconnectContext("peek", client_fd, 1, state, length, packet_id, recv_count);
        disconnectClient(&clients[client_index], 1);
      }
      continue;
    }
    #endif
    // Development-only raw world dump/upload protocol.
    #ifdef DEV_ENABLE_BEEF_DUMPS
    // 0xBEEF: stream world state to client, then disconnect.
    if (recv_buffer[0] == 0xBE && recv_buffer[1] == 0xEF && getClientState(client_fd) == STATE_NONE) {
      // Client must know fixed buffer sizes.
      send_all(client_fd, block_changes, sizeof(block_changes));
      send_all(client_fd, player_data, sizeof(player_data));
      // Flush writes and drain remaining inbound bytes.
      shutdown(client_fd, SHUT_WR);
      recv_all(client_fd, recv_buffer, sizeof(recv_buffer), false);
      disconnectClient(&clients[client_index], 6);
      continue;
    }
    // 0xFEED: read world state from client, persist, then disconnect.
    if (recv_buffer[0] == 0xFE && recv_buffer[1] == 0xED && getClientState(client_fd) == STATE_NONE) {
      // Consume magic bytes already peeked above.
      recv_all(client_fd, recv_buffer, 2, false);
      // Overwrite in-memory world/player buffers.
      recv_all(client_fd, block_changes, sizeof(block_changes), false);
      recv_all(client_fd, player_data, sizeof(player_data), false);
      // Rebuild block_changes_count from restored data.
      for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
        if (block_changes[i].block == 0xFF) continue;
        if (block_changes[i].block == B_chest) i += 14;
        if (i >= block_changes_count) block_changes_count = i + 1;
      }
      invalidateBlockChangeIndex();
      // Persist imported state.
      writeBlockChangesToDisk(0, block_changes_count);
      writePlayerDataToDisk();
      disconnectClient(&clients[client_index], 7);
      continue;
    }
    #endif

    // Parse packet length.
    length = readVarInt(client_fd);
    if (length == VARNUM_ERROR) {
      logDisconnectContext("read-length-varint", client_fd, 2, state, length, packet_id, recv_count);
      disconnectClient(&clients[client_index], 2);
      continue;
    }
    // Parse packet ID.
    packet_id = readVarInt(client_fd);
    if (packet_id == VARNUM_ERROR) {
      logDisconnectContext("read-packet-id-varint", client_fd, 3, state, length, packet_id, recv_count);
      disconnectClient(&clients[client_index], 3);
      continue;
    }
    // State may have changed since peek in rare cases.
    state = getClientState(client_fd);
    if (state == STATE_CONFIGURATION) {
      printf(
        "Configuration RX: fd=%d packet=0x%02X length=%d payload=%d\n",
        client_fd, packet_id, length, length - sizeVarInt(packet_id)
      );
    } else if (state == STATE_PLAY) {
      if (shouldLogPlayRxPacket(packet_id)) {
        printf(
          "Play RX: fd=%d packet=0x%02X length=%d payload=%d\n",
          client_fd, packet_id, length, length - sizeVarInt(packet_id)
        );
      }
    }
    // Reject legacy list ping probe.
    if (state == STATE_NONE && length == 254 && packet_id == 122) {
      logDisconnectContext("legacy-list-ping", client_fd, 5, state, length, packet_id, recv_count);
      disconnectClient(&clients[client_index], 5);
      continue;
    }
    // Dispatch packet payload.
    handlePacket(client_fd, length - sizeVarInt(packet_id), packet_id, state);
    flush_all_send_buffers();
    if (recv_count == -2) {
      disconnectClient(&clients[client_index], 8);
      continue;
    }
    if (recv_count == 0 || (recv_count == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      logDisconnectContext("post-handle", client_fd, 4, state, length, packet_id, recv_count);
      disconnectClient(&clients[client_index], 4);
      continue;
    }

  }

  #if !defined(ESP_PLATFORM) && !defined(_WIN32)
    shutdownAdminPipe();
  #endif
  close(server_fd);
 
  #ifdef _WIN32 // Cleanup WinSock.
    WSACleanup();
  #endif

  printf("Server closed.\n");

}

#ifdef ESP_PLATFORM

void nethr_main (void *pvParameters) {
  main();
  vTaskDelete(NULL);
}

static void wifi_event_handler (void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    printf("Got IP, starting server...\n\n");
    xTaskCreate(nethr_main, "nethr", 4096, NULL, 5, NULL);
  }
}

void wifi_init () {
  nvs_flash_init();
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

  wifi_config_t wifi_config = {
    .sta = {
      .ssid = WIFI_SSID,
      .password = WIFI_PASS,
      .threshold.authmode = WIFI_AUTH_WPA2_PSK
    }
  };

  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_start();
}

void app_main () {
  esp_timer_early_init();
  wifi_init();
}

#endif
