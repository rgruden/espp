#include <chrono>
#include <deque>
#include <stdlib.h>
#include <vector>

#include "ws-s3-touch.hpp"

#include "kalman_filter.hpp"
#include "madgwick_filter.hpp"

using namespace std::chrono_literals;

static constexpr size_t MAX_CIRCLES = 100;
static std::deque<lv_obj_t *> circles;

static std::recursive_mutex lvgl_mutex;
static void draw_circle(int x0, int y0, int radius);
static void clear_circles();

extern "C" void app_main(void) {
  espp::Logger logger(
      {.tag = "Waveshare S3 Touch Example", .level = espp::Logger::Verbosity::INFO});
  logger.info("Starting example!");

  //! [ws-s3-touch example]
  using Bsp = espp::WsS3Touch;
  auto &bsp = Bsp::get();
  bsp.set_log_level(espp::Logger::Verbosity::INFO);

  auto touch_callback = [&](const auto &touch) {
    // NOTE: since we're directly using the touchpad data, and not using the
    // TouchpadInput + LVGL, we'll need to ensure the touchpad data is
    // converted into proper screen coordinates instead of simply using the
    // raw values.
    static auto previous_touchpad_data = bsp.touchpad_convert(touch);
    auto touchpad_data = bsp.touchpad_convert(touch);
    if (touchpad_data != previous_touchpad_data) {
      logger.info("Touch: {}", touchpad_data);
      previous_touchpad_data = touchpad_data;
      // if the button is pressed, clear the circles
      if (touchpad_data.btn_state) {
        std::lock_guard<std::recursive_mutex> lock(lvgl_mutex);
        clear_circles();
      }
      // if there is a touch point, draw a circle and play a click sound
      if (touchpad_data.num_touch_points > 0) {
        // set the PWM / frequency for the buzzer based on the touch point (x -> pwm, y ->
        // frequency)
        float pwm =
            touchpad_data.x / static_cast<float>(bsp.lcd_width()) * 100.0f; // scale to 0-100%
        // scale frequency to be in range [50 Hz, 10 KHz]
        static constexpr float min_frequency_hz = 50.0f;
        static constexpr float max_frequency_hz = 10000.0f;
        // make it a logarithmic scale so that the frequency is more sensitive to
        // the lower end of the touchpad
        float frequency_hz =
            min_frequency_hz * std::pow(max_frequency_hz / min_frequency_hz,
                                        touchpad_data.y / static_cast<float>(bsp.lcd_height()));
        bsp.buzzer(pwm, frequency_hz);
        std::lock_guard<std::recursive_mutex> lock(lvgl_mutex);
        draw_circle(touchpad_data.x, touchpad_data.y, 10);
      } else {
        // if there are no touch points, stop the buzzer
        bsp.buzzer(0.0f);
      }
    }
  };

  // initialize the button, clear the circles on the screen
  logger.info("Initializing the button");
  auto on_button_pressed = [&](const auto &event) {
    if (event.active) {
      logger.info("Button pressed");
      std::lock_guard<std::recursive_mutex> lock(lvgl_mutex);
      clear_circles();
      // play a click sound
      bsp.buzzer(50.0f, 1000.0f); // 50% duty cycle, 1 kHz frequency
    } else {
      logger.info("Button released");
      // stop the buzzer
      bsp.buzzer(0.0f); // stop the buzzer
    }
  };
  if (!bsp.initialize_button(on_button_pressed)) {
    logger.error("Failed to initialize button!");
    return;
  }

  // initialize the buzzer
  if (!bsp.initialize_buzzer()) {
    logger.error("Failed to initialize buzzer!");
    return;
  }
  // initialize the LCD
  if (!bsp.initialize_lcd()) {
    logger.error("Failed to initialize LCD!");
    return;
  }
  // set the pixel buffer to be 50 lines high
  static constexpr size_t pixel_buffer_size = bsp.lcd_width() * 50;
  // initialize the LVGL display
  if (!bsp.initialize_display(pixel_buffer_size)) {
    logger.error("Failed to initialize display!");
    return;
  }
  // initialize the touchpad
  if (!bsp.initialize_touch(touch_callback)) {
    logger.error("Failed to initialize touchpad!");
    return;
  }

  // initialize the RTC
  if (!bsp.initialize_rtc()) {
    logger.error("Failed to initialize RTC!");
    return;
  }
  // now set the time on the RTC
  std::tm timeinfo{
      .tm_sec = 0,
      .tm_min = 42,
      .tm_hour = 13,
      .tm_mday = 24,
      .tm_mon = 10,  // 0-11, so 10 is November
      .tm_year = 123 // years since 1900, so 100 is 2000
  };
  std::error_code ec;
  bsp.rtc()->set_time(timeinfo, ec);
  if (ec) {
    logger.error("Failed to set RTC time: {}", ec.message());
    return;
  }

  // make the filter we'll use for the IMU to compute the orientation
  static constexpr float angle_noise = 0.001f;
  static constexpr float rate_noise = 0.1f;
  static espp::KalmanFilter<2> kf;
  kf.set_process_noise(rate_noise);
  kf.set_measurement_noise(angle_noise);
  static constexpr float beta = 0.9f; // higher = more accelerometer, lower = more gyro
  static espp::MadgwickFilter f(beta);

  using Imu = Bsp::Imu;
  auto kalman_filter_fn = [](float dt, const Imu::Value &accel,
                             const Imu::Value &gyro) -> Imu::Value {
    // Apply Kalman filter
    float accelRoll = atan2(accel.y, accel.z);
    float accelPitch = atan2(-accel.x, sqrt(accel.y * accel.y + accel.z * accel.z));
    kf.predict({espp::deg_to_rad(gyro.x), espp::deg_to_rad(gyro.y)}, dt);
    kf.update({accelRoll, accelPitch});
    float roll, pitch;
    std::tie(roll, pitch) = kf.get_state();
    // return the computed orientation
    Imu::Value orientation{};
    orientation.roll = roll;
    orientation.pitch = pitch;
    orientation.yaw = 0.0f;
    return orientation;
  };

  auto madgwick_filter_fn = [](float dt, const Imu::Value &accel,
                               const Imu::Value &gyro) -> Imu::Value {
    // Apply Madgwick filter
    f.update(dt, accel.x, accel.y, accel.z, espp::deg_to_rad(gyro.x), espp::deg_to_rad(gyro.y),
             espp::deg_to_rad(gyro.z));
    float roll, pitch, yaw;
    f.get_euler(roll, pitch, yaw);
    // return the computed orientation
    Imu::Value orientation{};
    orientation.roll = espp::deg_to_rad(roll);
    orientation.pitch = espp::deg_to_rad(pitch);
    orientation.yaw = espp::deg_to_rad(yaw);
    return orientation;
  };

  // initialize the IMU
  if (!bsp.initialize_imu(kalman_filter_fn)) {
    logger.error("Failed to initialize IMU!");
    return;
  }

  logger.info("Initialization complete, starting LVGL!");

  // set the background color to black
  lv_obj_t *bg = lv_obj_create(lv_screen_active());
  lv_obj_set_size(bg, bsp.lcd_width(), bsp.lcd_height());
  lv_obj_set_style_bg_color(bg, lv_color_make(0, 0, 0), 0);

  // add text in the center of the screen
  lv_obj_t *label = lv_label_create(lv_screen_active());
  static std::string label_text =
      "\n\n\n\nTouch the screen!\nPress the home button to clear circles.";
  lv_label_set_text(label, label_text.c_str());
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);

  /*Create style*/
  static lv_style_t style_line0;
  lv_style_init(&style_line0);
  lv_style_set_line_width(&style_line0, 8);
  lv_style_set_line_color(&style_line0, lv_palette_main(LV_PALETTE_BLUE));
  lv_style_set_line_rounded(&style_line0, true);

  // make a line for showing the direction of "down"
  lv_obj_t *line0 = lv_line_create(lv_screen_active());
  static lv_point_precise_t line_points0[] = {{0, 0}, {bsp.lcd_width(), bsp.lcd_height()}};
  lv_line_set_points(line0, line_points0, 2);
  lv_obj_add_style(line0, &style_line0, 0);

  /*Create style*/
  static lv_style_t style_line1;
  lv_style_init(&style_line1);
  lv_style_set_line_width(&style_line1, 8);
  lv_style_set_line_color(&style_line1, lv_palette_main(LV_PALETTE_RED));
  lv_style_set_line_rounded(&style_line1, true);

  // make a line for showing the direction of "down"
  lv_obj_t *line1 = lv_line_create(lv_screen_active());
  static lv_point_precise_t line_points1[] = {{0, 0}, {bsp.lcd_width(), bsp.lcd_height()}};
  lv_line_set_points(line1, line_points1, 2);
  lv_obj_add_style(line1, &style_line1, 0);

  // make a label centered at the very top for the RTC time
  lv_obj_t *rtc_label = lv_label_create(lv_screen_active());
  lv_label_set_text(rtc_label, "");
  lv_obj_align(rtc_label, LV_ALIGN_TOP_MID, 0, 20); // add an offset so that it's always visible
  lv_obj_set_style_text_align(rtc_label, LV_TEXT_ALIGN_LEFT, 0);

  // add a button in the top left which (when pressed) will rotate the display
  // through 0, 90, 180, 270 degrees
  lv_obj_t *btn = lv_btn_create(lv_screen_active());
  lv_obj_set_size(btn, 50, 50);
  lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t *label_btn = lv_label_create(btn);
  lv_label_set_text(label_btn, LV_SYMBOL_REFRESH);
  // center the text in the button
  lv_obj_align(label_btn, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(
      btn,
      [](auto event) {
        std::lock_guard<std::recursive_mutex> lock(lvgl_mutex);
        clear_circles();
        static auto rotation = LV_DISPLAY_ROTATION_0;
        rotation = static_cast<lv_display_rotation_t>((static_cast<int>(rotation) + 1) % 4);
        lv_display_t *disp = lv_display_get_default();
        lv_disp_set_rotation(disp, rotation);
      },
      LV_EVENT_PRESSED, nullptr);

  // disable scrolling on the screen (so that it doesn't behave weirdly when
  // rotated and drawing with your finger)
  lv_obj_set_scrollbar_mode(lv_screen_active(), LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

  logger.info("Starting LVGL task handler!");

  // start a simple thread to do the lv_task_handler every 16ms
  espp::Task lv_task({.callback = [](std::mutex &m, std::condition_variable &cv) -> bool {
                        {
                          std::lock_guard<std::recursive_mutex> lock(lvgl_mutex);
                          lv_task_handler();
                        }
                        std::unique_lock<std::mutex> lock(m);
                        cv.wait_for(lock, 16ms);
                        return false;
                      },
                      .task_config = {
                          .name = "lv_task",
                          .stack_size_bytes = 6 * 1024,
                      }});
  lv_task.start();

  // set the display brightness to be 75%
  bsp.brightness(75.0f);

  // make a task to read the rtc and print it to console
  espp::Task rtc_task({.callback = [&](std::mutex &m, std::condition_variable &cv) -> bool {
                         auto start = std::chrono::steady_clock::now();
                         static auto &bsp = Bsp::get();
                         static auto rtc = bsp.rtc();
                         std::error_code ec;
                         std::tm timeinfo = rtc->get_time(ec);
                         if (ec) {
                           logger.error("Failed to get RTC time: {}", ec.message());
                         } else {
                           // update the label with the current time
                           std::lock_guard<std::recursive_mutex> lock(lvgl_mutex);
                           lv_label_set_text_fmt(rtc_label, "%02d:%02d:%02d - %02d/%02d/%04d",
                                                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                                                 timeinfo.tm_mday, timeinfo.tm_mon + 1,
                                                 timeinfo.tm_year + 1900);
                         }
                         std::unique_lock<std::mutex> lock(m);
                         cv.wait_until(lock, start + 1s);
                         return false;
                       },
                       .task_config = {
                           .name = "rtc_task",
                           .stack_size_bytes = 4 * 1024,
                       }});
  rtc_task.start();

  // make a task to read out the IMU data and print it to console
  espp::Task imu_task(
      {.callback = [&](std::mutex &m, std::condition_variable &cv) -> bool {
         // sleep first in case we don't get IMU data and need to exit early
         {
           std::unique_lock<std::mutex> lock(m);
           cv.wait_for(lock, 10ms);
         }
         static auto &bsp = Bsp::get();
         static auto imu = bsp.imu();

         auto now = esp_timer_get_time(); // time in microseconds
         static auto t0 = now;
         auto t1 = now;
         float dt = (t1 - t0) / 1'000'000.0f; // convert us to s
         t0 = t1;

         std::error_code ec;
         // update the imu data
         if (!imu->update(dt, ec)) {
           return false;
         }
         // get accel
         auto accel = imu->get_accelerometer();
         auto gyro = imu->get_gyroscope();
         auto temp = imu->get_temperature();
         auto orientation = imu->get_orientation();
         auto gravity_vector = imu->get_gravity_vector();

         // NOTE: because of the moutning of the IMU w.r.t the mounting of the
         // screen we have to rotate the axes.
         std::swap(gravity_vector.x, gravity_vector.y);
         gravity_vector.y = -gravity_vector.y;

         // now update the gravity vector line to show the direction of "down"
         // taking into account the configured rotation of the display
         auto rotation = lv_display_get_rotation(lv_display_get_default());
         if (rotation == LV_DISPLAY_ROTATION_90) {
           std::swap(gravity_vector.x, gravity_vector.y);
           gravity_vector.x = -gravity_vector.x;
         } else if (rotation == LV_DISPLAY_ROTATION_180) {
           gravity_vector.x = -gravity_vector.x;
           gravity_vector.y = -gravity_vector.y;
         } else if (rotation == LV_DISPLAY_ROTATION_270) {
           std::swap(gravity_vector.x, gravity_vector.y);
           gravity_vector.y = -gravity_vector.y;
         }

         std::string text = fmt::format("{}\n\n\n\n\n", label_text);
         text += fmt::format("Accel: {:02.2f} {:02.2f} {:02.2f}\n", accel.x, accel.y, accel.z);
         text += fmt::format("Gyro: {:03.2f} {:03.2f} {:03.2f}\n", espp::deg_to_rad(gyro.x),
                             espp::deg_to_rad(gyro.y), espp::deg_to_rad(gyro.z));
         text += fmt::format("Angle: {:03.2f} {:03.2f}\n", espp::rad_to_deg(orientation.roll),
                             espp::rad_to_deg(orientation.pitch));
         text += fmt::format("Temp: {:02.1f} C\n", temp);

         // use the pitch to to draw a line on the screen indiating the
         // direction from the center of the screen to "down"
         int x0 = bsp.lcd_width() / 2;
         int y0 = bsp.lcd_height() / 2;

         int x1 = x0 + 50 * gravity_vector.x;
         int y1 = y0 + 50 * gravity_vector.y;

         static lv_point_precise_t line_points0[] = {{x0, y0}, {x1, y1}};
         line_points0[1].x = x1;
         line_points0[1].y = y1;

         // Now show the madgwick filter
         auto madgwick_orientation = madgwick_filter_fn(dt, accel, gyro);
         float roll = madgwick_orientation.roll;
         float pitch = madgwick_orientation.pitch;
         [[maybe_unused]] float yaw = madgwick_orientation.yaw;
         float vx = sin(pitch);
         float vy = -cos(pitch) * sin(roll);
         [[maybe_unused]] float vz = -cos(pitch) * cos(roll);

         // NOTE: because of the moutning of the IMU w.r.t the mounting of the
         // screen we have to rotate the axes.
         std::swap(vx, vy);
         vy = -vy;

         // now update the line to show the direction of "down" based on the
         // configured rotation of the display
         if (rotation == LV_DISPLAY_ROTATION_90) {
           std::swap(vx, vy);
           vx = -vx;
         } else if (rotation == LV_DISPLAY_ROTATION_180) {
           vx = -vx;
           vy = -vy;
         } else if (rotation == LV_DISPLAY_ROTATION_270) {
           std::swap(vx, vy);
           vy = -vy;
         }

         x1 = x0 + 50 * vx;
         y1 = y0 + 50 * vy;

         static lv_point_precise_t line_points1[] = {{x0, y0}, {x1, y1}};
         line_points1[1].x = x1;
         line_points1[1].y = y1;

         std::lock_guard<std::recursive_mutex> lock(lvgl_mutex);
         lv_label_set_text(label, text.c_str());
         lv_line_set_points(line0, line_points0, 2);
         lv_line_set_points(line1, line_points1, 2);

         return false;
       },
       .task_config = {
           .name = "IMU",
           .stack_size_bytes = 6 * 1024,
           .priority = 10,
           .core_id = 0,
       }});
  imu_task.start();

  logger.info("Example started, waiting for touch events...");

  // loop forever
  while (true) {
    std::this_thread::sleep_for(1s);
  }
  //! [ws-s3-touch example]
}

static void draw_circle(int x0, int y0, int radius) {
  // if the number of circles is greater than the max, remove the oldest circle
  if (circles.size() > MAX_CIRCLES) {
    lv_obj_delete(circles.front());
    circles.pop_front();
  }
  lv_obj_t *my_Cir = lv_obj_create(lv_screen_active());
  lv_obj_set_scrollbar_mode(my_Cir, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_size(my_Cir, radius * 2, radius * 2);
  lv_obj_set_pos(my_Cir, x0 - radius, y0 - radius);
  lv_obj_set_style_radius(my_Cir, LV_RADIUS_CIRCLE, 0);
  // ensure the circle ignores touch events (so things behind it can still be
  // interacted with)
  lv_obj_clear_flag(my_Cir, LV_OBJ_FLAG_CLICKABLE);
  circles.push_back(my_Cir);
}

static void clear_circles() {
  // remove the circles from lvgl
  for (auto circle : circles) {
    lv_obj_delete(circle);
  }
  // clear the vector
  circles.clear();
}
