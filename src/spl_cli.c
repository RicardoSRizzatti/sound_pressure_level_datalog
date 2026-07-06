/***************************************************************************//**
 * @file
 * @brief Bench CLI commands implementation.
 ******************************************************************************/
#include "spl_cli.h"

#include <stdio.h>

#include "em_device.h"

#include "app_measurement.h"
#include "app_watchdog.h"
#include "mic_pdm.h"
#include "sl_cli.h"
#include "sl_cli_handles.h"
#include "spl_config.h"
#include "spl_dsp.h"
#include "spl_store.h"
#include "spl_toct.h"

static void cmd_status(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  const spl_config_t *cfg = spl_config_get();
  printf("uptime_s: %lu\r\n", (unsigned long)app_measurement_uptime_s());
  printf("boot_id: %lu\r\n", (unsigned long)spl_store_boot_id());
  printf("wdog_resets: %lu\r\n", (unsigned long)spl_store_watchdog_reset_count());
  printf("records: %lu (seq %lu..%lu)\r\n",
         (unsigned long)spl_store_count(),
         (unsigned long)spl_store_oldest_seq(),
         (unsigned long)(spl_store_next_seq() ? spl_store_next_seq() - 1 : 0));
  printf("interval_s: %u  metrics: 0x%02x\r\n", cfg->interval_s, cfg->metrics);
  printf("cal_offset_db: %d.%02d\r\n",
         cfg->cal_offset_cdb / 100, abs(cfg->cal_offset_cdb % 100));
  printf("laf_now_db: %d.%01d\r\n",
         (int)spl_dsp_current_laf_db(),
         abs((int)(spl_dsp_current_laf_db() * 10.0f) % 10));
  printf("mic_overruns: %lu\r\n", (unsigned long)mic_pdm_overrun_count());
  printf("core_clock_hz: %lu\r\n", (unsigned long)SystemCoreClock);
  printf("mic_blocks: %lu  ch0 peak/mean: %d/%ld  ch1 peak/mean: %d/%ld\r\n",
         (unsigned long)mic_pdm_blocks_total(),
         (int)mic_pdm_last_peak(0), (long)mic_pdm_last_mean(0),
         (int)mic_pdm_last_peak(1), (long)mic_pdm_last_mean(1));
}

static void cmd_dump(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  spl_record_t rec;
  printf("seq,boot_id,uptime_s,laeq_db,lafmax_db\r\n");
  for (uint32_t seq = spl_store_oldest_seq(); seq < spl_store_next_seq(); seq++) {
    if (spl_store_read(seq, &rec)) {
      printf("%lu,%lu,%lu,%d.%02d,%d.%02d\r\n",
             (unsigned long)rec.seq,
             (unsigned long)rec.boot_id,
             (unsigned long)rec.uptime_s,
             rec.laeq_cdb / 100, abs(rec.laeq_cdb % 100),
             rec.lafmax_cdb / 100, abs(rec.lafmax_cdb % 100));
    }
    // A full dump takes seconds at 115200 baud: keep the audio pipeline and
    // the watchdog alive while printing.
    mic_pdm_process();
    app_watchdog_process();
  }
  printf("end\r\n");
}

static void cmd_clear(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  spl_store_clear();
  printf("cleared\r\n");
}

static void cmd_cal(sl_cli_command_arg_t *arguments)
{
  spl_config_t cfg = *spl_config_get();
  cfg.cal_offset_cdb = (int16_t)sl_cli_get_argument_int32(arguments, 0);
  if (spl_config_set(&cfg)) {
    printf("cal_offset_cdb: %d\r\n", cfg.cal_offset_cdb);
  } else {
    printf("error\r\n");
  }
}

static void cmd_interval(sl_cli_command_arg_t *arguments)
{
  spl_config_t cfg = *spl_config_get();
  cfg.interval_s = (uint16_t)sl_cli_get_argument_uint32(arguments, 0);
  if (spl_config_set(&cfg)) {
    app_measurement_apply_config();
    printf("interval_s: %u\r\n", cfg.interval_s);
  } else {
    printf("error (range %u..%u s)\r\n",
           SPL_CONFIG_INTERVAL_MIN_S, SPL_CONFIG_INTERVAL_MAX_S);
  }
}

static void cmd_metrics(sl_cli_command_arg_t *arguments)
{
  spl_config_t cfg = *spl_config_get();
  cfg.metrics = (uint8_t)sl_cli_get_argument_uint32(arguments, 0);
  if (spl_config_set(&cfg)) {
    printf("metrics: 0x%02x\r\n", cfg.metrics);
  } else {
    printf("error (bits: 1=LAeq 2=LAFmax 4=espectro)\r\n");
  }
}

static void cmd_ext(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  if (!(spl_config_get()->metrics & SPL_METRIC_EXTENDED)) {
    printf("metricas estendidas desabilitadas (metrics bit 0x08)\r\n");
    return;
  }
  uint32_t seq = spl_store_next_seq();
  spl_extended_t e;
  if (seq == 0 || !spl_store_read_extended(seq - 1, &e)) {
    printf("nenhum registro estendido ainda\r\n");
    return;
  }
  printf("seq %lu\r\n", (unsigned long)(seq - 1));
  printf("LAFmin: %d.%02d\r\n", e.lafmin_cdb / 100, abs(e.lafmin_cdb % 100));
  printf("LASmax: %d.%02d\r\n", e.lasmax_cdb / 100, abs(e.lasmax_cdb % 100));
  printf("LASmin: %d.%02d\r\n", e.lasmin_cdb / 100, abs(e.lasmin_cdb % 100));
  printf("LCpeak: %d.%02d\r\n", e.lcpeak_cdb / 100, abs(e.lcpeak_cdb % 100));
  printf("LAE: %d.%02d\r\n", e.lae_cdb / 100, abs(e.lae_cdb % 100));
  printf("L10: %d.%02d\r\n", e.l10_cdb / 100, abs(e.l10_cdb % 100));
  printf("L50: %d.%02d\r\n", e.l50_cdb / 100, abs(e.l50_cdb % 100));
  printf("L90: %d.%02d\r\n", e.l90_cdb / 100, abs(e.l90_cdb % 100));
}

static void cmd_spec(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  int16_t bands[SPL_TOCT_BANDS];
  if (!spl_toct_enabled()) {
    printf("espectro desabilitado (metrics bit 0x04)\r\n");
    return;
  }
  if (!spl_toct_last(bands)) {
    printf("nenhum intervalo fechado ainda\r\n");
    return;
  }
  printf("banda_Hz,LZeq_dB\r\n");
  for (uint32_t b = 0; b < SPL_TOCT_BANDS; b++) {
    uint32_t dhz = spl_toct_center_dhz(b);
    if (dhz % 10 == 0) {
      printf("%lu,", (unsigned long)(dhz / 10));
    } else {
      printf("%lu.%lu,", (unsigned long)(dhz / 10), (unsigned long)(dhz % 10));
    }
    printf("%d.%02d\r\n", bands[b] / 100, abs(bands[b] % 100));
  }
}

static void cmd_testtone(sl_cli_command_arg_t *arguments)
{
  uint32_t freq = sl_cli_get_argument_uint32(arguments, 0);
  int32_t amp = sl_cli_get_argument_int32(arguments, 1);
  mic_pdm_set_test_tone(freq, amp);
  if (freq == 0) {
    printf("test tone off\r\n");
  } else {
    printf("test tone: %lu Hz @ %ld cdBFS\r\n",
           (unsigned long)freq, (long)amp);
  }
}

static void cmd_hang(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  printf("hanging on purpose; watchdog should reset in ~2 s\r\n");
  while (1) {
    // Deliberate lock-up to exercise the watchdog recovery path.
  }
}

static const sl_cli_command_info_t cmd_info_status =
  SL_CLI_COMMAND(cmd_status, "Show device status", "",
                 { SL_CLI_ARG_END, });
static const sl_cli_command_info_t cmd_info_dump =
  SL_CLI_COMMAND(cmd_dump, "Dump all records as CSV", "",
                 { SL_CLI_ARG_END, });
static const sl_cli_command_info_t cmd_info_clear =
  SL_CLI_COMMAND(cmd_clear, "Erase all records", "",
                 { SL_CLI_ARG_END, });
static const sl_cli_command_info_t cmd_info_cal =
  SL_CLI_COMMAND(cmd_cal, "Set calibration offset", "offset in centi-dB",
                 { SL_CLI_ARG_INT32, SL_CLI_ARG_END, });
static const sl_cli_command_info_t cmd_info_interval =
  SL_CLI_COMMAND(cmd_interval, "Set measurement interval", "seconds",
                 { SL_CLI_ARG_UINT32, SL_CLI_ARG_END, });
static const sl_cli_command_info_t cmd_info_metrics =
  SL_CLI_COMMAND(cmd_metrics, "Set metric bitmask", "1=LAeq 2=LAFmax 4=espectro",
                 { SL_CLI_ARG_UINT32, SL_CLI_ARG_END, });
static const sl_cli_command_info_t cmd_info_ext =
  SL_CLI_COMMAND(cmd_ext, "Show last interval extended metrics", "",
                 { SL_CLI_ARG_END, });
static const sl_cli_command_info_t cmd_info_spec =
  SL_CLI_COMMAND(cmd_spec, "Show last interval 1/3-octave spectrum", "",
                 { SL_CLI_ARG_END, });
static const sl_cli_command_info_t cmd_info_testtone =
  SL_CLI_COMMAND(cmd_testtone, "Inject a synthetic sine instead of mic input",
                 "freq Hz (0=off)" SL_CLI_UNIT_SEPARATOR "amplitude centi-dBFS",
                 { SL_CLI_ARG_UINT32, SL_CLI_ARG_INT32, SL_CLI_ARG_END, });
static const sl_cli_command_info_t cmd_info_hang =
  SL_CLI_COMMAND(cmd_hang, "Lock up to test the watchdog", "",
                 { SL_CLI_ARG_END, });

static sl_cli_command_entry_t command_table[] = {
  { "status", &cmd_info_status, false },
  { "dump", &cmd_info_dump, false },
  { "clear", &cmd_info_clear, false },
  { "cal", &cmd_info_cal, false },
  { "interval", &cmd_info_interval, false },
  { "metrics", &cmd_info_metrics, false },
  { "ext", &cmd_info_ext, false },
  { "spec", &cmd_info_spec, false },
  { "testtone", &cmd_info_testtone, false },
  { "hang", &cmd_info_hang, false },
  { NULL, NULL, false },
};

static sl_cli_command_group_t command_group = {
  { NULL },
  false,
  command_table
};

void spl_cli_init(void)
{
  sl_cli_command_add_command_group(sl_cli_inst_handle, &command_group);
}
