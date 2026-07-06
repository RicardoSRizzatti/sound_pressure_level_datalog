/***************************************************************************//**
 * @file
 * @brief Persisted device configuration implementation.
 ******************************************************************************/
#include "spl_config.h"

#include "app_led.h"
#include "nvm3_default.h"
#include "spl_dsp.h"
#include "spl_toct.h"

#define KEY_CONFIG 0x0003u

#define CONFIG_VERSION 1u

static const spl_config_t config_defaults = {
  .version = CONFIG_VERSION,
  .metrics = SPL_METRIC_LAEQ | SPL_METRIC_LAFMAX,
  .interval_s = 20,
  .cal_offset_cdb = 12300,   // SPL_DSP_DEFAULT_CAL_OFFSET_DB in centi-dB
};

static spl_config_t config;

static void apply(void)
{
  spl_dsp_set_cal_offset((float)config.cal_offset_cdb / 100.0f);
  spl_dsp_set_extended((config.metrics & SPL_METRIC_EXTENDED) != 0);
  spl_toct_set_enabled((config.metrics & SPL_METRIC_SPECTRUM) != 0);
}

#define SPL_METRIC_ALL \
  (SPL_METRIC_LAEQ | SPL_METRIC_LAFMAX | SPL_METRIC_SPECTRUM | SPL_METRIC_EXTENDED)

static bool valid(const spl_config_t *cfg)
{
  return cfg->version == CONFIG_VERSION
         && cfg->metrics != 0
         && (cfg->metrics & ~SPL_METRIC_ALL) == 0
         && cfg->interval_s >= SPL_CONFIG_INTERVAL_MIN_S
         && cfg->interval_s <= SPL_CONFIG_INTERVAL_MAX_S;
}

void spl_config_init(void)
{
  spl_config_t stored;
  uint32_t type;
  size_t len;

  config = config_defaults;
  if (nvm3_getObjectInfo(nvm3_defaultHandle, KEY_CONFIG, &type, &len) == SL_STATUS_OK
      && type == NVM3_OBJECTTYPE_DATA && len == sizeof(stored)
      && nvm3_readData(nvm3_defaultHandle, KEY_CONFIG, &stored, sizeof(stored)) == SL_STATUS_OK
      && valid(&stored)) {
    config = stored;
  }
  apply();
}

const spl_config_t *spl_config_get(void)
{
  return &config;
}

bool spl_config_set(const spl_config_t *cfg)
{
  if (!valid(cfg)) {
    return false;
  }
  config = *cfg;
  apply();
  bool ok = nvm3_writeData(nvm3_defaultHandle, KEY_CONFIG,
                           &config, sizeof(config)) == SL_STATUS_OK;
  // Visual acknowledgement that a configuration was received and applied.
  app_led_blink_config_ack();
  return ok;
}
