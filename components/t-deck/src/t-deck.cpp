#include "t-deck.hpp"

using namespace espp;

TDeck::TDeck()
    : BaseComponent("TDeck") {
  // initialize the pweripheral power pin and set it to high
  gpio_set_direction(peripheral_power_pin_, GPIO_MODE_OUTPUT);
  peripheral_power(true);
  // set all CS pins to be high to disable the devices
  gpio_set_direction(lcd_cs_io, GPIO_MODE_OUTPUT);
  gpio_set_level(lcd_cs_io, 1);
  gpio_set_direction(sdcard_cs, GPIO_MODE_OUTPUT);
  gpio_set_level(sdcard_cs, 1);
  gpio_set_direction(lora_cs_io, GPIO_MODE_OUTPUT);
  gpio_set_level(lora_cs_io, 1);
}

void TDeck::peripheral_power(bool on) { gpio_set_level(peripheral_power_pin_, on); }

bool TDeck::peripheral_power() const { return gpio_get_level(peripheral_power_pin_); }

////////////////////////
//   SPI Functions    //
////////////////////////

bool TDeck::init_spi_bus() {
  if (spi_bus_initialized_) {
    return true;
  }

  spi_bus_config_t bus_cfg;
  memset(&bus_cfg, 0, sizeof(bus_cfg));
  bus_cfg.mosi_io_num = spi_mosi_io;
  bus_cfg.miso_io_num = spi_miso_io;
  bus_cfg.sclk_io_num = spi_sclk_io;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = frame_buffer_size * sizeof(lv_color_t) + 100;
  auto ret = spi_bus_initialize(spi_num, &bus_cfg,
                                SDSPI_DEFAULT_DMA); // SPI_DMA_CH_AUTO); // SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    logger_.error("Failed to initialize bus.");
    return false;
  }

  logger_.info("SPI bus initialized");
  spi_bus_initialized_ = true;

  return true;
}

////////////////////////
// Keyboard Functions //
////////////////////////

bool TDeck::initialize_keyboard(bool start_task, const TDeck::keypress_callback_t &key_cb,
                                std::chrono::milliseconds poll_interval) {
  if (keyboard_) {
    logger_.warn("Keyboard already initialized, not initializing again!");
    return false;
  }
  logger_.info("Initializing keyboard input");
  keyboard_ = std::make_shared<espp::TKeyboard>(espp::TKeyboard::Config{
      .write = std::bind(&espp::I2c::write, &internal_i2c_, std::placeholders::_1,
                         std::placeholders::_2, std::placeholders::_3),
      .read = std::bind(&espp::I2c::read, &internal_i2c_, std::placeholders::_1,
                        std::placeholders::_2, std::placeholders::_3),
      .key_cb = key_cb,
      .polling_interval = poll_interval,
      .auto_start = start_task,
      .log_level = espp::Logger::Verbosity::WARN});
  return true;
}

std::shared_ptr<espp::TKeyboard> TDeck::keyboard() const { return keyboard_; }

/////////////////////////
// Trackball Functions //
/////////////////////////

bool TDeck::initialize_trackball(const TDeck::trackball_callback_t &trackball_cb, int sensitivity) {
  if (pointer_input_) {
    logger_.warn("Trackball already initialized, not initializing again!");
    return false;
  }
  logger_.info("Initializing trackball input");
  pointer_input_ = std::make_shared<espp::PointerInput>(espp::PointerInput::Config{
      .read = std::bind(&TDeck::trackball_read, this, std::placeholders::_1, std::placeholders::_2,
                        std::placeholders::_3, std::placeholders::_4),
      .log_level = espp::Logger::Verbosity::WARN});

  // store the callback
  trackball_callback_ = trackball_cb;

  // add the interrupts for the trackball
  interrupts_.add_interrupt(trackball_up_interrupt_pin);
  interrupts_.add_interrupt(trackball_down_interrupt_pin);
  interrupts_.add_interrupt(trackball_left_interrupt_pin);
  interrupts_.add_interrupt(trackball_right_interrupt_pin);
  interrupts_.add_interrupt(trackball_btn_interrupt_pin);

  // set the sensitivity
  set_trackball_sensitivity(sensitivity);

  return true;
}

std::shared_ptr<espp::PointerInput> TDeck::pointer_input() const { return pointer_input_; }

espp::PointerData TDeck::trackball_data() const { return trackball_data_; }

void TDeck::trackball_read(int &x, int &y, bool &left_pressed, bool &right_pressed) {
  std::lock_guard<std::recursive_mutex> lock(trackball_data_mutex_);
  x = trackball_data_.x;
  y = trackball_data_.y;
  left_pressed = trackball_data_.left_pressed;
  right_pressed = trackball_data_.right_pressed;
}

void TDeck::set_trackball_sensitivity(int sensitivity) { trackball_sensitivity_ = sensitivity; }

void TDeck::on_trackball_interrupt(const espp::Interrupt::Event &event) {
  int diff = trackball_sensitivity_;
  std::lock_guard lock(trackball_data_mutex_);
  if (event.gpio_num == trackball_up) {
    trackball_data_.y += diff;
  } else if (event.gpio_num == trackball_down) {
    trackball_data_.y -= diff;
  } else if (event.gpio_num == trackball_left) {
    trackball_data_.x -= diff;
  } else if (event.gpio_num == trackball_right) {
    trackball_data_.x += diff;
  } else if (event.gpio_num == trackball_btn) {
    trackball_data_.left_pressed = event.active;
  }
  trackball_data_.x = std::clamp<int>(trackball_data_.x, 0, lcd_width_ - 1);
  trackball_data_.y = std::clamp<int>(trackball_data_.y, 0, lcd_height_ - 1);
  if (trackball_callback_) {
    trackball_callback_(trackball_data_);
  }
}

////////////////////////
// Touchpad Functions //
////////////////////////

bool TDeck::initialize_touch(const TDeck::touch_callback_t &touch_cb) {
  if (gt911_ || touchpad_input_) {
    logger_.warn("Touch already initialized, not initializing again!");
    return false;
  }

  if (!display_) {
    logger_.warn("You should call initialize_display() before initialize_touch(), otherwise lvgl "
                 "will not properly handle the touchpad input!");
  }
  logger_.info("Initializing touch input");

  gt911_ = std::make_unique<espp::Gt911>(espp::Gt911::Config{
      .write = std::bind(&espp::I2c::write, &internal_i2c_, std::placeholders::_1,
                         std::placeholders::_2, std::placeholders::_3),
      .read = std::bind(&espp::I2c::read, &internal_i2c_, std::placeholders::_1,
                        std::placeholders::_2, std::placeholders::_3),
      .log_level = espp::Logger::Verbosity::WARN});

  touchpad_input_ = std::make_shared<espp::TouchpadInput>(espp::TouchpadInput::Config{
      .touchpad_read =
          std::bind(&TDeck::touchpad_read, this, std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3, std::placeholders::_4),
      .swap_xy = touch_swap_xy,
      .invert_x = touch_invert_x,
      .invert_y = touch_invert_y,
      .log_level = espp::Logger::Verbosity::WARN});

  // store the callback
  touch_callback_ = touch_cb;

  // add the touch interrupt pin
  interrupts_.add_interrupt(touch_interrupt_pin_);

  return true;
}

bool TDeck::update_gt911() {
  // ensure the gt911 is initialized
  if (!gt911_) {
    return false;
  }
  // get the latest data from the device
  std::error_code ec;
  bool new_data = gt911_->update(ec);
  if (ec) {
    logger_.error("could not update gt911: {}\n", ec.message());
    std::lock_guard<std::recursive_mutex> lock(touchpad_data_mutex_);
    touchpad_data_ = {};
    return false;
  }
  if (!new_data) {
    return false;
  }
  // get the latest data from the touchpad
  TouchpadData temp_data;
  gt911_->get_touch_point(&temp_data.num_touch_points, &temp_data.x, &temp_data.y);
  temp_data.btn_state = gt911_->get_home_button_state();
  // update the touchpad data
  std::lock_guard<std::recursive_mutex> lock(touchpad_data_mutex_);
  touchpad_data_ = temp_data;
  return true;
}

std::shared_ptr<espp::TouchpadInput> TDeck::touchpad_input() const { return touchpad_input_; }

espp::TouchpadData TDeck::touchpad_data() const { return touchpad_data_; }

void TDeck::touchpad_read(uint8_t *num_touch_points, uint16_t *x, uint16_t *y, uint8_t *btn_state) {
  std::lock_guard<std::recursive_mutex> lock(touchpad_data_mutex_);
  *num_touch_points = touchpad_data_.num_touch_points;
  *x = touchpad_data_.x;
  *y = touchpad_data_.y;
  *btn_state = touchpad_data_.btn_state;
}

espp::TouchpadData TDeck::touchpad_convert(const espp::TouchpadData &data) const {
  TouchpadData temp_data;
  temp_data.num_touch_points = data.num_touch_points;
  temp_data.x = data.x;
  temp_data.y = data.y;
  temp_data.btn_state = data.btn_state;
  if (temp_data.num_touch_points == 0) {
    return temp_data;
  }
  if (touch_swap_xy) {
    std::swap(temp_data.x, temp_data.y);
  }
  if (touch_invert_x) {
    temp_data.x = lcd_width_ - (temp_data.x + 1);
  }
  if (touch_invert_y) {
    temp_data.y = lcd_height_ - (temp_data.y + 1);
  }
  // get the orientation of the display
  auto rotation = lv_display_get_rotation(lv_display_get_default());
  switch (rotation) {
  case LV_DISPLAY_ROTATION_0:
    break;
  case LV_DISPLAY_ROTATION_90:
    temp_data.y = lcd_height_ - (temp_data.y + 1);
    std::swap(temp_data.x, temp_data.y);
    break;
  case LV_DISPLAY_ROTATION_180:
    temp_data.x = lcd_width_ - (temp_data.x + 1);
    temp_data.y = lcd_height_ - (temp_data.y + 1);
    break;
  case LV_DISPLAY_ROTATION_270:
    temp_data.x = lcd_width_ - (temp_data.x + 1);
    std::swap(temp_data.x, temp_data.y);
    break;
  default:
    break;
  }
  return temp_data;
}

////////////////////////
// Display Functions //
////////////////////////

// the user flag for the callbacks does two things:
// 1. Provides the GPIO level for the data/command pin, and
// 2. Sets some bits for other signaling (such as LVGL FLUSH)
static constexpr int FLUSH_BIT = (1 << (int)espp::display_drivers::Flags::FLUSH_BIT);
static constexpr int DC_LEVEL_BIT = (1 << (int)espp::display_drivers::Flags::DC_LEVEL_BIT);

// This function is called (in irq context!) just before a transmission starts.
// It will set the D/C line to the value indicated in the user field
// (DC_LEVEL_BIT).
//
// cppcheck-suppress constParameterCallback
static void IRAM_ATTR lcd_spi_pre_transfer_callback(spi_transaction_t *t) {
  static auto lcd_dc_io = TDeck::get_lcd_dc_gpio();
  uint32_t user_flags = (uint32_t)(t->user);
  bool dc_level = user_flags & DC_LEVEL_BIT;
  gpio_set_level(lcd_dc_io, dc_level);
}

// This function is called (in irq context!) just after a transmission ends. It
// will indicate to lvgl that the next flush is ready to be done if the
// FLUSH_BIT is set.
//
// cppcheck-suppress constParameterCallback
static void IRAM_ATTR lcd_spi_post_transfer_callback(spi_transaction_t *t) {
  uint16_t user_flags = (uint32_t)(t->user);
  bool should_flush = user_flags & FLUSH_BIT;
  if (should_flush) {
    lv_display_t *disp = lv_display_get_default();
    lv_display_flush_ready(disp);
  }
}

bool TDeck::initialize_lcd() {
  if (lcd_handle_) {
    logger_.warn("LCD already initialized, not initializing again!");
    return false;
  }

  if (!init_spi_bus()) {
    logger_.error("Failed to initialize SPI bus for LCD.");
    return false;
  }

  esp_err_t ret;
  memset(&lcd_config_, 0, sizeof(lcd_config_));
  lcd_config_.mode = 0;
  // lcd_config_.flags = SPI_DEVICE_NO_RETURN_RESULT;
  lcd_config_.clock_speed_hz = lcd_clock_speed;
  lcd_config_.input_delay_ns = 0;
  lcd_config_.spics_io_num = lcd_cs_io;
  lcd_config_.queue_size = spi_queue_size;
  lcd_config_.pre_cb = lcd_spi_pre_transfer_callback;
  lcd_config_.post_cb = lcd_spi_post_transfer_callback;

  // Attach the LCD to the SPI bus
  ret = spi_bus_add_device(spi_num, &lcd_config_, &lcd_handle_);
  ESP_ERROR_CHECK(ret);
  // initialize the controller
  using namespace std::placeholders;
  DisplayDriver::initialize(espp::display_drivers::Config{
      .write_command = std::bind(&TDeck::write_command, this, _1, _2, _3),
      .lcd_send_lines = std::bind(&TDeck::write_lcd_lines, this, _1, _2, _3, _4, _5, _6),
      .reset_pin = lcd_reset_io,
      .data_command_pin = lcd_dc_io,
      .reset_value = reset_value,
      .invert_colors = invert_colors,
      .swap_xy = swap_xy,
      .mirror_x = mirror_x,
      .mirror_y = mirror_y,
      .mirror_portrait = mirror_portrait});
  return true;
}

bool TDeck::initialize_display(size_t pixel_buffer_size) {
  if (!lcd_handle_) {
    logger_.error(
        "LCD not initialized, you must call initialize_lcd() before initialize_display()!");
    return false;
  }
  if (display_) {
    logger_.warn("Display already initialized, not initializing again!");
    return false;
  }
  // initialize the display / lvgl
  using namespace std::chrono_literals;
  display_ = std::make_shared<Display<Pixel>>(
      Display<Pixel>::LvglConfig{.width = lcd_width_,
                                 .height = lcd_height_,
                                 .flush_callback = DisplayDriver::flush,
                                 .rotation_callback = DisplayDriver::rotate,
                                 .rotation = rotation},
      Display<Pixel>::LcdConfig{.backlight_pin = backlight_io,
                                .backlight_on_value = backlight_value},
      Display<Pixel>::DynamicMemoryConfig{
          .pixel_buffer_size = pixel_buffer_size,
          .double_buffered = true,
          .allocation_flags = MALLOC_CAP_8BIT | MALLOC_CAP_DMA,
      });

  frame_buffer0_ =
      (uint8_t *)heap_caps_malloc(frame_buffer_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
  frame_buffer1_ =
      (uint8_t *)heap_caps_malloc(frame_buffer_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
  return true;
}

std::shared_ptr<espp::Display<TDeck::Pixel>> TDeck::display() const { return display_; }

void IRAM_ATTR TDeck::lcd_wait_lines() {
  spi_transaction_t *rtrans;
  esp_err_t ret;
  // logger_.debug("Waiting for {} queued transactions", num_queued_trans);
  // Wait for all transactions to be done and get back the results.
  while (num_queued_trans) {
    ret = spi_device_get_trans_result(lcd_handle_, &rtrans, 10 / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
      logger_.error("Display: Could not get spi trans result: {} '{}'", ret, esp_err_to_name(ret));
    }
    num_queued_trans--;
    // We could inspect rtrans now if we received any info back. The LCD is treated as write-only,
    // though.
  }
}

void IRAM_ATTR TDeck::write_command(uint8_t command, std::span<const uint8_t> parameters,
                                    uint32_t user_data) {
  lcd_wait_lines();
  memset(&trans[0], 0, sizeof(spi_transaction_t));
  memset(&trans[1], 0, sizeof(spi_transaction_t));

  trans[0].length = 8;
  trans[0].user = reinterpret_cast<void *>(user_data);
  trans[0].flags = SPI_TRANS_USE_TXDATA;
  trans[0].tx_data[0] = command;

  trans[1].length = parameters.size() * 8;
  if (parameters.size() <= 4) {
    // copy the data pointer to trans[0].tx_data
    memcpy(trans[1].tx_data, parameters.data(), parameters.size());
    trans[1].flags = SPI_TRANS_USE_TXDATA;
  } else if (!parameters.empty()) {
    trans[1].tx_buffer = parameters.data();
    trans[1].flags = 0;
  }
  trans[1].user = reinterpret_cast<void *>(
      user_data | (1 << static_cast<int>(display_drivers::Flags::DC_LEVEL_BIT)));

  esp_err_t ret = spi_device_queue_trans(lcd_handle_, &trans[0], 10 / portTICK_PERIOD_MS);
  if (ret != ESP_OK) {
    logger_.error("Couldn't queue spi command trans for display: {} '{}'", ret,
                  esp_err_to_name(ret));
  } else {
    ++num_queued_trans;
    if (!parameters.empty()) {
      ret = spi_device_queue_trans(lcd_handle_, &trans[1], 10 / portTICK_PERIOD_MS);
      if (ret != ESP_OK) {
        logger_.error("Couldn't queue spi data trans for display: {} '{}'", ret,
                      esp_err_to_name(ret));
      } else {
        ++num_queued_trans;
      }
    }
  }
}

void IRAM_ATTR TDeck::write_lcd_lines(int xs, int ys, int xe, int ye, const uint8_t *data,
                                      uint32_t user_data) {
  // if we haven't waited by now, wait here...
  lcd_wait_lines();
  esp_err_t ret;
  size_t length = (xe - xs + 1) * (ye - ys + 1) * 2;
  if (length == 0) {
    logger_.error("lcd_send_lines: Bad length: ({},{}) to ({},{})", xs, ys, xe, ye);
  }
  // initialize the spi transactions
  for (int i = 0; i < 6; i++) {
    memset(&trans[i], 0, sizeof(spi_transaction_t));
    if ((i & 1) == 0) {
      // Even transfers are commands
      trans[i].length = 8;
      trans[i].user = (void *)0;
    } else {
      // Odd transfers are data
      trans[i].length = 8 * 4;
      trans[i].user = (void *)DC_LEVEL_BIT;
    }
    trans[i].flags = SPI_TRANS_USE_TXDATA;
  }
  trans[0].tx_data[0] = (uint8_t)DisplayDriver::Command::caset;
  trans[1].tx_data[0] = (xs) >> 8;
  trans[1].tx_data[1] = (xs)&0xff;
  trans[1].tx_data[2] = (xe) >> 8;
  trans[1].tx_data[3] = (xe)&0xff;
  trans[2].tx_data[0] = (uint8_t)DisplayDriver::Command::raset;
  trans[3].tx_data[0] = (ys) >> 8;
  trans[3].tx_data[1] = (ys)&0xff;
  trans[3].tx_data[2] = (ye) >> 8;
  trans[3].tx_data[3] = (ye)&0xff;
  trans[4].tx_data[0] = (uint8_t)DisplayDriver::Command::ramwr;
  trans[5].tx_buffer = data;
  trans[5].length = length * 8;
  // undo SPI_TRANS_USE_TXDATA flag
  trans[5].flags = SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL;
  // we need to keep the dc bit set, but also add our flags
  trans[5].user = (void *)(DC_LEVEL_BIT | user_data);
  // Queue all transactions.
  for (int i = 0; i < 6; i++) {
    ret = spi_device_queue_trans(lcd_handle_, &trans[i], 10 / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
      logger_.error("Couldn't queue spi trans for display: {} '{}'", ret, esp_err_to_name(ret));
    } else {
      num_queued_trans++;
    }
  }
  // When we are here, the SPI driver is busy (in the background) getting the
  // transactions sent. That happens mostly using DMA, so the CPU doesn't have
  // much to do here. We're not going to wait for the transaction to finish
  // because we may as well spend the time calculating the next line. When that
  // is done, we can call lcd_wait_lines, which will wait for the transfers
  // to be done and check their status.
}

void TDeck::write_lcd_frame(const uint16_t xs, const uint16_t ys, const uint16_t width,
                            const uint16_t height, uint8_t *data) {
  if (data) {
    // have data, fill the area with the color data
    lv_area_t area{.x1 = (lv_coord_t)(xs),
                   .y1 = (lv_coord_t)(ys),
                   .x2 = (lv_coord_t)(xs + width - 1),
                   .y2 = (lv_coord_t)(ys + height - 1)};
    DisplayDriver::fill(nullptr, &area, data);
  } else {
    // don't have data, so clear the area (set to 0)
    DisplayDriver::clear(xs, ys, width, height);
  }
}

TDeck::Pixel *TDeck::vram0() const {
  if (!display_) {
    return nullptr;
  }
  return display_->vram0();
}

TDeck::Pixel *TDeck::vram1() const {
  if (!display_) {
    return nullptr;
  }
  return display_->vram1();
}

uint8_t *TDeck::frame_buffer0() const { return frame_buffer0_; }

uint8_t *TDeck::frame_buffer1() const { return frame_buffer1_; }

void TDeck::brightness(float brightness) {
  brightness = std::clamp(brightness, 0.0f, 100.0f) / 100.0f;
  // display expects a value between 0 and 1
  display_->set_brightness(brightness);
}

float TDeck::brightness() const {
  // display returns a value between 0 and 1
  return display_->get_brightness();
}
