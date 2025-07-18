#include "led.hpp"

using namespace espp;

std::mutex Led::fade_service_mutex;
bool Led::fade_service_installed = false;

bool IRAM_ATTR Led::cb_ledc_fade_end_event(const ledc_cb_param_t *param, void *user_arg) {
  portBASE_TYPE taskAwoken = pdFALSE;

  if (param->event == LEDC_FADE_END_EVT) {
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_arg;
    xSemaphoreGiveFromISR(sem, &taskAwoken);
  }

  return (taskAwoken == pdTRUE);
}

Led::Led(const Config &config) noexcept
    : BaseComponent("Led", config.log_level)
    , duty_resolution_(config.duty_resolution)
    , max_raw_duty_((uint32_t)(std::pow(2, (int)duty_resolution_) - 1))
    , channels_(config.channels) {

  logger_.info("Initializing timer");
  ledc_timer_config_t ledc_timer;
  memset(&ledc_timer, 0, sizeof(ledc_timer));
  ledc_timer.duty_resolution = duty_resolution_;
  ledc_timer.freq_hz = config.frequency_hz;
  ledc_timer.speed_mode = config.speed_mode;
  ledc_timer.timer_num = config.timer;
  ledc_timer.clk_cfg = config.clock_config;
  ledc_timer_config(&ledc_timer);

  logger_.info("Initializing channels");
  for (const auto &conf : channels_) {
    uint32_t actual_duty = std::clamp(conf.duty, 0.0f, 100.0f) * max_raw_duty_ / 100.0f;
    ledc_channel_config_t channel_conf;
    memset(&channel_conf, 0, sizeof(channel_conf));
    channel_conf.channel = conf.channel;
    channel_conf.duty = actual_duty;
    channel_conf.gpio_num = conf.gpio;
    channel_conf.speed_mode = conf.speed_mode;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    channel_conf.sleep_mode = conf.sleep_mode;
#endif
    channel_conf.hpoint = 0;
    channel_conf.timer_sel = conf.timer;
    channel_conf.flags.output_invert = conf.output_invert;
    ledc_channel_config(&channel_conf);
  }

  logger_.info("Initializing the fade service");
  {
    std::lock_guard<std::mutex> lock(fade_service_mutex);
    if (!fade_service_installed) {
      auto install_fn = []() -> esp_err_t { return ledc_fade_func_install(0); };
      auto err = espp::task::run_on_core(install_fn, config.isr_core_id);
      if (err != ESP_OK) {
        logger_.error("install ledc fade service failed {}", esp_err_to_name(err));
      }
      fade_service_installed = err == ESP_OK;
    }
  }

  ledc_cbs_t callbacks = {.fade_cb = &Led::cb_ledc_fade_end_event};

  // we associate each channel with its own semaphore so that they can be
  // faded / controlled independently.
  logger_.info("Creating semaphores");
  fade_semaphores_.resize(channels_.size());
  for (auto &sem : fade_semaphores_) {
    sem = xSemaphoreCreateBinary();
    // go ahead and give to the semaphores so the functions will work
    xSemaphoreGive(sem);
  }
  for (int i = 0; i < channels_.size(); i++) {
    ledc_cb_register(channels_[i].speed_mode, channels_[i].channel, &callbacks,
                     (void *)fade_semaphores_[i]);
  }
}

Led::~Led() {
  // clean up the semaphores
  for (auto &sem : fade_semaphores_) {
    // take the semaphore (so that we don't delete it until no one is
    // blocked on it)
    xSemaphoreTake(sem, portMAX_DELAY);
    // and delete it
    vSemaphoreDelete(sem);
  }
}

void Led::uninstall_isr() {
  std::lock_guard<std::mutex> lock(fade_service_mutex);
  if (fade_service_installed) {
    ledc_fade_func_uninstall();
    fade_service_installed = false;
  }
}

bool Led::can_change(ledc_channel_t channel) {
  int index = get_channel_index(channel);
  if (index == -1) {
    return false;
  }
  auto &sem = fade_semaphores_[index];
  return uxSemaphoreGetCount(sem) == 1;
}

std::optional<float> Led::get_duty(ledc_channel_t channel) const {
  int index = get_channel_index(channel);
  if (index == -1) {
    return {};
  }
  const auto &conf = channels_[index];
  auto raw_duty = ledc_get_duty(conf.speed_mode, conf.channel);
  return (float)raw_duty / (float)max_raw_duty_ * 100.0f;
}

bool Led::set_duty(ledc_channel_t channel, float duty_percent) {
  int index = get_channel_index(channel);
  if (index == -1) {
    return false;
  }
  auto conf = channels_[index];
  auto &sem = fade_semaphores_[index];
  // ensure that it's not fading if it is
  xSemaphoreTake(sem, portMAX_DELAY);
  uint32_t actual_duty = std::clamp(duty_percent, 0.0f, 100.0f) * max_raw_duty_ / 100.0f;
  esp_err_t err = ledc_set_duty(conf.speed_mode, conf.channel, actual_duty);
  if (err != ESP_OK) {
    logger_.error("Failed to set duty for channel {}: {}", conf.channel, esp_err_to_name(err));
    xSemaphoreGive(sem);
    return false;
  }
  err = ledc_update_duty(conf.speed_mode, conf.channel);
  // make sure others can set this channel now as well
  xSemaphoreGive(sem);
  if (err != ESP_OK) {
    logger_.error("Failed to update duty for channel {}: {}", conf.channel, esp_err_to_name(err));
    return false;
  }
  return true;
}

std::optional<size_t> Led::get_frequency(ledc_channel_t channel) const {
  int index = get_channel_index(channel);
  if (index == -1) {
    return {};
  }
  const auto &conf = channels_[index];
  return ledc_get_freq(conf.speed_mode, conf.timer);
}

bool Led::set_frequency(ledc_channel_t channel, size_t frequency_hz) {
  int index = get_channel_index(channel);
  if (index == -1) {
    return false;
  }
  auto conf = channels_[index];
  auto &sem = fade_semaphores_[index];
  // ensure that it's not fading if it is
  xSemaphoreTake(sem, portMAX_DELAY);
  auto err = ledc_set_freq(conf.speed_mode, conf.timer, frequency_hz);
  // make sure others can set this channel now as well
  xSemaphoreGive(sem);
  if (err != ESP_OK) {
    logger_.error("Failed to set frequency for channel {}: {}", conf.channel, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool Led::set_pwm(ledc_channel_t channel, size_t frequency_hz, float duty_percent) {
  int index = get_channel_index(channel);
  if (index == -1) {
    return false;
  }
  auto conf = channels_[index];
  auto &sem = fade_semaphores_[index];
  // ensure that it's not fading if it is
  xSemaphoreTake(sem, portMAX_DELAY);
  auto err = ledc_set_freq(conf.speed_mode, conf.timer, frequency_hz);
  if (err != ESP_OK) {
    logger_.error("Failed to set frequency for channel {}: {}", conf.channel, esp_err_to_name(err));
    xSemaphoreGive(sem);
    return false;
  }
  uint32_t actual_duty = std::clamp(duty_percent, 0.0f, 100.0f) * max_raw_duty_ / 100.0f;
  err = ledc_set_duty(conf.speed_mode, conf.channel, actual_duty);
  if (err != ESP_OK) {
    logger_.error("Failed to set duty for channel {}: {}", conf.channel, esp_err_to_name(err));
    xSemaphoreGive(sem);
    return false;
  }
  err = ledc_update_duty(conf.speed_mode, conf.channel);
  // make sure others can set this channel now as well
  xSemaphoreGive(sem);
  if (err != ESP_OK) {
    logger_.error("Failed to update duty for channel {}: {}", conf.channel, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool Led::set_fade_with_time(ledc_channel_t channel, float duty_percent, uint32_t fade_time_ms) {
  int index = get_channel_index(channel);
  if (index == -1) {
    return false;
  }
  auto conf = channels_[index];
  auto &sem = fade_semaphores_[index];
  // ensure that it's not fading if it is
  xSemaphoreTake(sem, portMAX_DELAY);
  uint32_t actual_duty = std::clamp(duty_percent, 0.0f, 100.0f) * max_raw_duty_ / 100.0f;
  esp_err_t err = ledc_set_fade_with_time(conf.speed_mode, conf.channel, actual_duty, fade_time_ms);
  if (err != ESP_OK) {
    logger_.error("Failed to set fade for channel {}: {}", conf.channel, esp_err_to_name(err));
    xSemaphoreGive(sem);
    return false;
  }
  err = ledc_fade_start(conf.speed_mode, conf.channel, LEDC_FADE_NO_WAIT);
  if (err != ESP_OK) {
    logger_.error("Failed to start fade for channel {}: {}", conf.channel, esp_err_to_name(err));
    xSemaphoreGive(sem);
    return false;
  }
  // NOTE: we don't give the semaphore back here because that is the job of
  // the ISR
  return true;
}

int Led::get_channel_index(ledc_channel_t channel) const {
  for (int i = 0; i < channels_.size(); i++) {
    if (channels_[i].channel == channel) {
      return i;
    }
  }
  return -1;
}
