/***************************************************************************//**
 * @file
 * @brief Custom BLE GATT service implementation (docs/protocolo_ble.md).
 ******************************************************************************/
#include "spl_ble.h"

#include <string.h>

#include "app.h"
#include "app_log.h"
#include "app_measurement.h"
#include "app_watchdog.h"
#include "gatt_db.h"
#include "mic_pdm.h"
#include "spl_config.h"
#include "spl_store.h"

// Sync Control opcodes (app -> device).
#define SYNC_CMD_START 0x01
#define SYNC_CMD_ACK   0x02
#define SYNC_CMD_STOP  0x03
// Sync Control events (device -> app).
#define SYNC_EVT_RANGE 0x81
#define SYNC_EVT_DONE  0x82
#define SYNC_EVT_ERR   0x8F

#define RECORD_WIRE_SIZE 16u
// Cap of records per notification; the actual count also honours the MTU.
#define MAX_RECORDS_PER_NOTIFY 14u

// Spectrum wire entry: boot_id u32 + seq u32 + 30 x i16 = 68 bytes.
#define SPECTRUM_WIRE_SIZE (8u + 2u * SPL_STORE_SPEC_BANDS)
#define MAX_SPECTRA_PER_NOTIFY 3u

static uint8_t active_connection = SL_BT_INVALID_CONNECTION_HANDLE;
static uint16_t att_mtu = 23;

static bool status_notif_enabled;
static bool records_notif_enabled;
static bool spectra_notif_enabled;

// Sync session state. Records stream first, then (when the app subscribed
// to Spectra) the spectra of the same range, then DONE.
static bool sync_active;
static bool sync_done_pending;   // backlog drained, DONE not yet delivered
static uint32_t sync_next_seq;   // next record to send
static uint32_t sync_start_seq;  // first seq of this session
static bool sync_spec_phase;     // records done, streaming spectra
static uint32_t sync_spec_seq;   // next spectrum to send

static void pack_u16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void pack_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint16_t unpack_u16(const uint8_t *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t unpack_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void build_status(uint8_t out[16])
{
  uint32_t overruns = mic_pdm_overrun_count();
  pack_u32(out + 0, app_measurement_uptime_s());
  pack_u32(out + 4, spl_store_boot_id());
  pack_u32(out + 8, spl_store_count());
  pack_u16(out + 12, (uint16_t)spl_store_watchdog_reset_count());
  pack_u16(out + 14, overruns > UINT16_MAX ? UINT16_MAX : (uint16_t)overruns);
}

static void pack_record(uint8_t *p, const spl_record_t *rec)
{
  pack_u32(p + 0, rec->boot_id);
  pack_u32(p + 4, rec->seq);
  pack_u32(p + 8, rec->uptime_s);
  pack_u16(p + 12, (uint16_t)rec->laeq_cdb);
  pack_u16(p + 14, (uint16_t)rec->lafmax_cdb);
}

static sl_status_t send_sync_event(uint8_t evt, uint32_t a, uint32_t b, uint8_t n_words)
{
  uint8_t buf[9];
  buf[0] = evt;
  pack_u32(buf + 1, a);
  if (n_words == 2) {
    pack_u32(buf + 5, b);
  }
  return sl_bt_gatt_server_send_indication(active_connection,
                                           gattdb_spl_sync_ctrl,
                                           (uint8_t)(1 + 4 * n_words), buf);
}

// ---- User read/write handlers ----------------------------------------------

static void handle_read(sl_bt_evt_gatt_server_user_read_request_t *req)
{
  uint8_t buf[SPL_STORE_MAX_BOOT_EPOCHS * 8];
  uint16_t len = 0;
  uint8_t att_err = 0;

  switch (req->characteristic) {
    case gattdb_spl_config: {
      const spl_config_t *cfg = spl_config_get();
      buf[0] = cfg->metrics;
      pack_u16(buf + 1, cfg->interval_s);
      buf[3] = 0;
      len = 4;
      break;
    }
    case gattdb_spl_cal:
      pack_u16(buf, (uint16_t)spl_config_get()->cal_offset_cdb);
      len = 2;
      break;
    case gattdb_spl_boot_epochs: {
      spl_boot_epoch_t table[SPL_STORE_MAX_BOOT_EPOCHS];
      uint32_t n = spl_store_get_boot_epochs(table);
      for (uint32_t i = 0; i < n; i++) {
        pack_u32(buf + i * 8, table[i].boot_id);
        pack_u32(buf + i * 8 + 4, table[i].epoch_boot);
      }
      len = (uint16_t)(n * 8);
      break;
    }
    case gattdb_spl_status:
      build_status(buf);
      len = 16;
      break;
    default:
      att_err = 0x0A;   // Attribute Not Found / not handled here
      break;
  }

  (void)sl_bt_gatt_server_send_user_read_response(req->connection,
                                                  req->characteristic,
                                                  att_err, len, buf, NULL);
}

static void handle_write(sl_bt_evt_gatt_server_user_write_request_t *req)
{
  uint8_t att_err = 0;
  const uint8_t *data = req->value.data;
  size_t len = req->value.len;

  switch (req->characteristic) {
    case gattdb_spl_config: {
      if (len != 4) {
        att_err = 0x0D;   // Invalid Attribute Value Length
        break;
      }
      spl_config_t cfg = *spl_config_get();
      cfg.metrics = data[0];
      cfg.interval_s = unpack_u16(data + 1);
      if (!spl_config_set(&cfg)) {
        att_err = 0xFF;   // Out of Range
      } else {
        app_measurement_apply_config();
      }
      break;
    }
    case gattdb_spl_cal: {
      if (len != 2) {
        att_err = 0x0D;
        break;
      }
      spl_config_t cfg = *spl_config_get();
      cfg.cal_offset_cdb = (int16_t)unpack_u16(data);
      if (!spl_config_set(&cfg)) {
        att_err = 0xFF;
      }
      break;
    }
    case gattdb_spl_time_sync: {
      if (len != 8) {
        att_err = 0x0D;
        break;
      }
      uint64_t epoch_ms = 0;
      for (int i = 7; i >= 0; i--) {
        epoch_ms = (epoch_ms << 8) | data[i];
      }
      uint32_t epoch_boot = (uint32_t)(epoch_ms / 1000u)
                            - app_measurement_uptime_s();
      if (!spl_store_set_boot_epoch(epoch_boot)) {
        att_err = 0x0E;   // Unlikely Error
      }
      break;
    }
    case gattdb_spl_sync_ctrl: {
      if (len < 1) {
        att_err = 0x0D;
        break;
      }
      switch (data[0]) {
        case SYNC_CMD_START:
          if (len != 5 || !records_notif_enabled) {
            att_err = 0x0D;
            break;
          }
          sync_next_seq = unpack_u32(data + 1);
          if (sync_next_seq < spl_store_oldest_seq()
              || sync_next_seq > spl_store_next_seq()) {
            // Pedido fora da janela (ex.: app com estado de uma "vida"
            // anterior do dispositivo, pós mass-erase): recomeça do mais
            // antigo; o app deduplica por boot_id+seq.
            sync_next_seq = spl_store_oldest_seq();
          }
          sync_active = true;
          sync_done_pending = false;
          sync_start_seq = sync_next_seq;
          sync_spec_phase = false;
          (void)send_sync_event(SYNC_EVT_RANGE,
                                spl_store_oldest_seq(), spl_store_next_seq(), 2);
          app_proceed();
          break;
        case SYNC_CMD_ACK:
          if (len != 5) {
            att_err = 0x0D;
            break;
          }
          spl_store_delete_through(unpack_u32(data + 1));
          break;
        case SYNC_CMD_STOP:
          sync_active = false;
          sync_done_pending = false;
          break;
        default:
          att_err = 0xFF;
          break;
      }
      break;
    }
    default:
      att_err = 0x0A;
      break;
  }

  (void)sl_bt_gatt_server_send_user_write_response(req->connection,
                                                   req->characteristic,
                                                   att_err);
}

// ---- Event dispatch ----------------------------------------------------------

void spl_ble_on_event(sl_bt_msg_t *evt)
{
  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_bt_evt_connection_opened_id:
      active_connection = evt->data.evt_connection_opened.connection;
      att_mtu = 23;
      break;

    case sl_bt_evt_gatt_mtu_exchanged_id:
      att_mtu = evt->data.evt_gatt_mtu_exchanged.mtu;
      break;

    case sl_bt_evt_connection_closed_id:
      active_connection = SL_BT_INVALID_CONNECTION_HANDLE;
      status_notif_enabled = false;
      records_notif_enabled = false;
      spectra_notif_enabled = false;
      sync_active = false;
      sync_done_pending = false;
      break;

    case sl_bt_evt_gatt_server_characteristic_status_id: {
      sl_bt_evt_gatt_server_characteristic_status_t *st =
        &evt->data.evt_gatt_server_characteristic_status;
      if (st->status_flags != sl_bt_gatt_server_client_config) {
        break;
      }
      bool enabled = (st->client_config_flags != sl_bt_gatt_disable);
      if (st->characteristic == gattdb_spl_status) {
        status_notif_enabled = enabled;
      } else if (st->characteristic == gattdb_spl_records) {
        records_notif_enabled = enabled;
        if (!enabled) {
          sync_active = false;
          sync_done_pending = false;
        }
      } else if (st->characteristic == gattdb_spl_spectra) {
        spectra_notif_enabled = enabled;
      }
      break;
    }

    case sl_bt_evt_gatt_server_user_read_request_id:
      handle_read(&evt->data.evt_gatt_server_user_read_request);
      break;

    case sl_bt_evt_gatt_server_user_write_request_id:
      handle_write(&evt->data.evt_gatt_server_user_write_request);
      break;

    default:
      break;
  }
}

// ---- Record streaming ----------------------------------------------------------

void spl_ble_process(void)
{
  if (active_connection == SL_BT_INVALID_CONNECTION_HANDLE) {
    return;
  }

  // DONE must actually reach the app: the indication send fails while the
  // TX queue is still flushing the record burst, so retry until accepted.
  if (sync_done_pending) {
    if (send_sync_event(SYNC_EVT_DONE,
                        spl_store_next_seq() ? spl_store_next_seq() - 1 : 0,
                        0, 1) == SL_STATUS_OK) {
      sync_done_pending = false;
      sync_active = false;
    } else {
      app_proceed();   // try again on the next main-loop pass
    }
    return;
  }

  if (!sync_active) {
    return;
  }

  // Keep the BLE health flag fresh while a sync session is alive.
  app_watchdog_set_ble_healthy(true);

  while (sync_next_seq < spl_store_next_seq()) {
    // Fill one notification with as many consecutive records as fit the MTU.
    uint8_t payload[MAX_RECORDS_PER_NOTIFY * RECORD_WIRE_SIZE];
    uint32_t max_by_mtu = (uint32_t)(att_mtu - 3) / RECORD_WIRE_SIZE;
    if (max_by_mtu == 0) {
      max_by_mtu = 1;
    }
    if (max_by_mtu > MAX_RECORDS_PER_NOTIFY) {
      max_by_mtu = MAX_RECORDS_PER_NOTIFY;
    }

    uint32_t n = 0;
    spl_record_t rec;
    while (n < max_by_mtu && (sync_next_seq + n) < spl_store_next_seq()
           && spl_store_read(sync_next_seq + n, &rec)) {
      pack_record(payload + n * RECORD_WIRE_SIZE, &rec);
      n++;
    }
    if (n == 0) {
      // Gap (records deleted meanwhile): skip forward.
      sync_next_seq++;
      continue;
    }

    sl_status_t sc = sl_bt_gatt_server_send_notification(
      active_connection, gattdb_spl_records,
      (uint16_t)(n * RECORD_WIRE_SIZE), payload);
    if (sc != SL_STATUS_OK) {
      // TX queue full: try again on the next main-loop pass.
      app_proceed();
      return;
    }
    sync_next_seq += n;
  }

  // Records drained; stream the spectra of the same range if subscribed.
  if (spectra_notif_enabled) {
    if (!sync_spec_phase) {
      sync_spec_phase = true;
      sync_spec_seq = sync_start_seq;
    }
    while (sync_spec_seq < spl_store_next_seq()) {
      uint8_t payload[MAX_SPECTRA_PER_NOTIFY * SPECTRUM_WIRE_SIZE];
      uint32_t max_by_mtu = (uint32_t)(att_mtu - 3) / SPECTRUM_WIRE_SIZE;
      if (max_by_mtu == 0) {
        max_by_mtu = 1;
      }
      if (max_by_mtu > MAX_SPECTRA_PER_NOTIFY) {
        max_by_mtu = MAX_SPECTRA_PER_NOTIFY;
      }

      // Pack consecutive existing spectra (intervals without the spectrum
      // metric enabled simply have no entry and are skipped).
      uint32_t bundle_start = sync_spec_seq;
      uint32_t n = 0;
      spl_spectrum_t sp;
      while (n < max_by_mtu && sync_spec_seq < spl_store_next_seq()) {
        if (spl_store_read_spectrum(sync_spec_seq, &sp)) {
          uint8_t *p = payload + n * SPECTRUM_WIRE_SIZE;
          pack_u32(p, sp.boot_id);
          pack_u32(p + 4, sp.seq);
          for (uint32_t b = 0; b < SPL_STORE_SPEC_BANDS; b++) {
            pack_u16(p + 8 + 2 * b, (uint16_t)sp.bands_cdb[b]);
          }
          n++;
          sync_spec_seq++;
        } else if (n == 0) {
          sync_spec_seq++;   // gap: keep scanning
        } else {
          break;             // flush what we have, resume scanning after send
        }
      }
      if (n == 0) {
        continue;   // reached next_seq while scanning gaps
      }

      sl_status_t sc = sl_bt_gatt_server_send_notification(
        active_connection, gattdb_spl_spectra,
        (uint16_t)(n * SPECTRUM_WIRE_SIZE), payload);
      if (sc != SL_STATUS_OK) {
        sync_spec_seq = bundle_start;   // rebuild this bundle on the next pass
        app_proceed();
        return;
      }
    }
  }

  // Backlog drained: deliver DONE (retried above if the queue is full).
  sync_done_pending = true;
  spl_ble_process();
}

void spl_ble_notify_new_record(void)
{
  if (status_notif_enabled
      && active_connection != SL_BT_INVALID_CONNECTION_HANDLE) {
    uint8_t buf[16];
    build_status(buf);
    (void)sl_bt_gatt_server_send_notification(active_connection,
                                              gattdb_spl_status,
                                              sizeof(buf), buf);
  }
  if (sync_active) {
    app_proceed();   // stream the new record too
  }
}
