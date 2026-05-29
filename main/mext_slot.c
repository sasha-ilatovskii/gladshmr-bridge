// Unified mext protocol handler — handles both grid and arc slots.
//
// One task per CDC slot. Each task:
//   1. Reads its slot type from g_slots[slot_idx] on every connect
//   2. Responds to mext handshake (QUERY / GET_ID / GET_GRIDSZ) for that type
//   3. Parses and silently absorbs LED/ring commands from norns
//   4. Drains its event queue (g_slot_queues[slot_idx]) and forwards
//      key/encoder events to norns via CDC write

#include "mext_slot.h"
#include "ble_raw.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mext.h"
#include "monome_slots.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

#define TAG "mext_slot"
#define MEXT_SLOT_CORE 1

// One event queue per slot (input events flowing ESP32 → norns)
static QueueHandle_t s_queues[MAX_MONOME_SLOTS];

// -----------------------------------------------------------------------
// CDC write helper
// -----------------------------------------------------------------------
static inline void cdc_write(uint8_t itf, const uint8_t* buf, size_t len)
{
  tud_cdc_n_write(itf, buf, len);
  tud_cdc_n_write_flush(itf);
}

// -----------------------------------------------------------------------
// mext handshake responses — grid
// -----------------------------------------------------------------------
static void grid_query(uint8_t itf)
{
  uint8_t r0[] = {MEXT_SYSTEM_QUERY_RESPONSE, SS_KEY_GRID, 1};
  cdc_write(itf, r0, sizeof(r0));
  uint8_t r1[] = {MEXT_SYSTEM_QUERY_RESPONSE, SS_TILT, 1};
  cdc_write(itf, r1, sizeof(r1));
}

static void grid_id(uint8_t itf)
{
  uint8_t r[33] = {MEXT_SYSTEM_ID};
  strncpy((char*) &r[1], GRID_ID_STR, 32);
  cdc_write(itf, r, sizeof(r));
}

static void grid_gridsz(uint8_t itf, const monome_slot_t* s)
{
  uint8_t r[] = {MEXT_SYSTEM_GRIDSZ, s->param1, s->param2};
  ESP_LOGI(TAG, "[CDC%d] grid gridsz %dx%d", itf, s->param1, s->param2);
  cdc_write(itf, r, sizeof(r));
}

// -----------------------------------------------------------------------
// mext handshake responses — arc
// -----------------------------------------------------------------------
static void arc_query(uint8_t itf, const monome_slot_t* s)
{
  uint8_t r[] = {MEXT_SYSTEM_QUERY_RESPONSE, SS_ENCODER, s->param1};
  ESP_LOGI(TAG, "[CDC%d] arc query rings=%d", itf, s->param1);
  cdc_write(itf, r, sizeof(r));
}

static void arc_id(uint8_t itf, const monome_slot_t* s)
{
  char id[11]; // "a" + up to 3 digits (0-255) + "000001" + NUL = 11
  snprintf(id, sizeof(id), "a%d000001", s->param1);
  uint8_t r[33] = {MEXT_SYSTEM_ID};
  strncpy((char*) &r[1], id, 32);
  ESP_LOGI(TAG, "[CDC%d] arc id '%s'", itf, id);
  cdc_write(itf, r, sizeof(r));
}

static void arc_gridsz(uint8_t itf)
{
  // cols=0, rows=0 → norns detects as arc
  uint8_t r[] = {MEXT_SYSTEM_GRIDSZ, ARC_COLS, ARC_ROWS};
  cdc_write(itf, r, sizeof(r));
}

// -----------------------------------------------------------------------
// mext parser — byte-at-a-time state machine
// -----------------------------------------------------------------------
typedef enum
{
  ST_HEADER,
  ST_PAYLOAD
} parse_state_t;

typedef struct
{
  parse_state_t state;
  uint8_t hdr;
  uint8_t payload[64];
  uint8_t plen;
  uint8_t pidx;
} parser_t;

static void parser_reset(parser_t* p)
{
  p->state = ST_HEADER;
  p->pidx = 0;
}

static void process_byte(uint8_t itf, int slot_idx, parser_t* par, uint8_t b)
{
  const monome_slot_t* slot = &g_slots[slot_idx];

  switch (par->state)
  {
  case ST_HEADER:
    par->hdr = b;
    par->plen = MEXT_INCOMING_PAYLOAD_LEN[b];
    par->pidx = 0;

    if (par->plen == 0)
    {
      if (slot->type == MONOME_SLOT_GRID)
      {
        switch (par->hdr)
        {
        case MEXT_SYSTEM_QUERY:
          grid_query(itf);
          break;
        case MEXT_SYSTEM_GET_ID:
          grid_id(itf);
          break;
        case MEXT_SYSTEM_GET_GRIDSZ:
          grid_gridsz(itf, slot);
          break;
        case MEXT_LED_ALL_OFF:
        case MEXT_LED_ALL_ON:
        {
          uint8_t pkt[1] = {par->hdr};
          ble_raw_send_state(pkt, 1);
          break;
        }
        default:
          ESP_LOGW(TAG, "[CDC%d] unknown 0-payload grid cmd 0x%02x", itf, par->hdr);
          break;
        }
      }
      else if (slot->type == MONOME_SLOT_ARC)
      {
        switch (par->hdr)
        {
        case MEXT_SYSTEM_QUERY:
          arc_query(itf, slot);
          break;
        case MEXT_SYSTEM_GET_ID:
          arc_id(itf, slot);
          break;
        case MEXT_SYSTEM_GET_GRIDSZ:
          arc_gridsz(itf);
          break;
        default:
          ESP_LOGW(TAG, "[CDC%d] unknown 0-payload arc cmd 0x%02x", itf, par->hdr);
        }
      }
    }
    else
    {
      par->state = ST_PAYLOAD;
    }
    break;

  case ST_PAYLOAD:
    par->payload[par->pidx++] = b;
    if (par->pidx >= par->plen)
    {
      par->state = ST_HEADER;

      // Forward the command to the iPad as raw mext bytes: [header, payload...]
      uint8_t pkt[1 + 64];
      pkt[0] = par->hdr;
      memcpy(&pkt[1], par->payload, par->plen);
      ble_raw_send_state(pkt, 1 + par->plen);
      ESP_LOGD(TAG, "[CDC%d] completed cmd 0x%02x plen=%d", itf, par->hdr, par->plen);
    }
    break;
  }
}

// -----------------------------------------------------------------------
// Outbound events: ESP32 → norns (key / encoder)
// Returns number of events forwarded.
// -----------------------------------------------------------------------
static int flush_events(uint8_t itf, int slot_idx)
{
  if (!tud_cdc_n_connected(itf))
    return 0;

  int count = 0;
  uart_msg_t msg;
  while (xQueueReceive(s_queues[slot_idx], &msg, 0) == pdTRUE)
  {
    uint8_t ev[8];
    int ev_len = 0;

    switch (msg.type)
    {
    case UART_MSG_KEY_DOWN:
      ev[0] = MEXT_KEY_DOWN;
      ev[1] = msg.data[0];
      ev[2] = msg.data[1];
      ev_len = 3;
      break;
    case UART_MSG_KEY_UP:
      ev[0] = MEXT_KEY_UP;
      ev[1] = msg.data[0];
      ev[2] = msg.data[1];
      ev_len = 3;
      break;
    case UART_MSG_ENC_DELTA:
      ev[0] = MEXT_ENCODER_DELTA;
      ev[1] = msg.data[0];
      ev[2] = msg.data[1];
      ev_len = 3;
      break;
    case UART_MSG_ENC_SWITCH_DOWN:
      ev[0] = MEXT_ENCODER_SWITCH_DOWN;
      ev[1] = msg.data[0];
      ev_len = 2;
      break;
    case UART_MSG_ENC_SWITCH_UP:
      ev[0] = MEXT_ENCODER_SWITCH_UP;
      ev[1] = msg.data[0];
      ev_len = 2;
      break;
    case UART_MSG_TILT:
      ev[0] = MEXT_TILT; // 0x81
      // norns' Lua grid.tilt wrapper drops the MEXT tilt sensor and forwards
      // the next three values as g.tilt(x, y, z), so pack axes accordingly.
      ev[1] = msg.data[1]; // x, received by norns as the sensor field
      ev[2] = msg.data[2]; // y lo
      ev[3] = 0;           // y hi
      ev[4] = msg.data[3]; // z lo
      ev[5] = 0;           // z hi
      ev[6] = 0;           // unused z lo
      ev[7] = 0;           // z hi
      ev_len = 8;
      break;
    default:
      break;
    }

    if (ev_len > 0)
    {
      if (msg.type == UART_MSG_TILT)
      {
        int16_t norns_y = (int16_t) (ev[2] | (ev[3] << 8));
        int16_t norns_z = (int16_t) (ev[4] | (ev[5] << 8));
        ESP_LOGD(TAG,
                 "[CDC%d] tilt sensor=%d norns_x=%d norns_y=%d norns_z=%d",
                 itf,
                 msg.data[0],
                 ev[1],
                 norns_y,
                 norns_z);
      }
      else
      {
        ESP_LOGD(TAG, "[CDC%d] send hdr=0x%02x len=%d", itf, ev[0], ev_len);
      }
      cdc_write(itf, ev, ev_len);
      count++;
    }
  }
  return count;
}

// -----------------------------------------------------------------------
// Per-slot CDC task
// -----------------------------------------------------------------------
static void slot_task(void* arg)
{
  int slot_idx = (int) (uintptr_t) arg;
  uint8_t itf = (uint8_t) slot_idx;
  parser_t par = {0};
  parser_reset(&par);

  ESP_LOGI(TAG, "slot %d task started (CDC %d)", slot_idx, itf);

  while (1)
  {
    const monome_slot_t* slot = &g_slots[slot_idx];

    if (slot->type == MONOME_SLOT_DISABLED || !tud_cdc_n_connected(itf))
    {
      parser_reset(&par);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Inbound: norns → ESP32 (LED / ring commands)
    // Drain the entire FIFO in one shot — a single partial read per tick
    // leaves data in the buffer long enough for the host to get EAGAIN.
    uint32_t cdc_bytes = 0;
    uint32_t avail;
    while ((avail = tud_cdc_n_available(itf)) > 0)
    {
      uint8_t buf[64];
      uint32_t n = tud_cdc_n_read(itf, buf, avail < sizeof(buf) ? avail : sizeof(buf));
      for (uint32_t i = 0; i < n; i++)
      {
        process_byte(itf, slot_idx, &par, buf[i]);
      }
      cdc_bytes += n;
    }

    // Outbound: input events → norns
    int events = flush_events(itf, slot_idx);

    // Yield: sleep only when the iteration was idle so we don't introduce
    // a full-tick delay on every key press or LED burst.
    if (cdc_bytes == 0 && events == 0)
    {
      vTaskDelay(1);
    }
    else
    {
      taskYIELD();
    }
  }
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------
void mext_slot_init(void)
{
  for (int i = 0; i < MAX_MONOME_SLOTS; i++)
  {
    s_queues[i] = xQueueCreate(32, sizeof(uart_msg_t));

    char name[16];
    snprintf(name, sizeof(name), "mext_slot%d", i);
    xTaskCreatePinnedToCore(slot_task, name, 4096, (void*) (uintptr_t) i, 5, NULL, MEXT_SLOT_CORE);
  }
}

void mext_slot_dispatch(const uart_msg_t* msg)
{
  // Route to the first slot whose type matches the event category.
  for (int i = 0; i < MAX_MONOME_SLOTS; i++)
  {
    const monome_slot_t* slot = &g_slots[i];
    bool match = false;

    switch (msg->type)
    {
    case UART_MSG_KEY_DOWN:
    case UART_MSG_KEY_UP:
    case UART_MSG_TILT:
      match = (slot->type == MONOME_SLOT_GRID);
      break;
    case UART_MSG_ENC_DELTA:
    case UART_MSG_ENC_SWITCH_DOWN:
    case UART_MSG_ENC_SWITCH_UP:
      match = (slot->type == MONOME_SLOT_ARC);
      break;
    default:
      break;
    }

    if (match)
    {
      xQueueSend(s_queues[i], msg, 0);
      return;
    }
  }
}
