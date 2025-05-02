/*
 *  Bit-banging driver for Sharp LS0XXB7 displays. It currently only works with
 *  the LS014B7DD01 display.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sharp_mip_parallel, CONFIG_DISPLAY_LOG_LEVEL);

#include <stdint.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

struct rgb_port {
  const struct device *port;
  // Bitmask of GPIO indexed into config->rgb that are connected to this port.
  uint8_t rgb_idx_mask;
  gpio_port_pins_t port_mask;
};

// Private mutable data for the driver.
struct sharp_mip_data {
  K_KERNEL_STACK_MEMBER(vcom_thread_stack, /*size=*/256);
  struct k_thread vcom_thread;

  // Optimization for RGB pins.
  struct rgb_port rgb_ports[2];
};

// Private const data for the driver.
struct sharp_mip_config {
  uint16_t height;
  uint16_t width;

  struct gpio_dt_spec intb;
  struct gpio_dt_spec gsp;
  struct gpio_dt_spec gck;
  struct gpio_dt_spec gen;
  struct gpio_dt_spec bsp;
  struct gpio_dt_spec bck;
  struct gpio_dt_spec rgb[6];

  struct gpio_dt_spec vcom_vb;
  struct gpio_dt_spec va;
  int vcom_freq;
};

static void vcom_thread(void *config, void *unused1, void *unused2) {
  const struct sharp_mip_config *cfg = config;

  // Ensure out of phase.
  gpio_pin_set_dt(&cfg->vcom_vb, 1);
  gpio_pin_set_dt(&cfg->va, 0);

  while (1) {
    gpio_pin_toggle_dt(&cfg->vcom_vb);
    gpio_pin_toggle_dt(&cfg->va);
    k_msleep(1000 / cfg->vcom_freq);
  }
}

#define SET_GPIO_OUTPUT(ret, name)                       \
  do {                                                   \
    if (!device_is_ready(name.port)) {                   \
      LOG_ERR(#name " GPIO port not ready");             \
      return -ENODEV;                                    \
    }                                                    \
    ret = gpio_pin_configure_dt(&name, GPIO_OUTPUT);     \
    if (ret < 0) {                                       \
      LOG_ERR(#name " GPIO pin config failed: %d", ret); \
      return ret;                                        \
    }                                                    \
  } while (0)

static int sharp_mip_init(const struct device *dev) {
  const struct sharp_mip_config *config = dev->config;
  struct sharp_mip_data *data = dev->data;

  int ret;

  SET_GPIO_OUTPUT(ret, config->intb);
  SET_GPIO_OUTPUT(ret, config->gsp);
  SET_GPIO_OUTPUT(ret, config->gck);
  SET_GPIO_OUTPUT(ret, config->gen);
  SET_GPIO_OUTPUT(ret, config->bsp);
  SET_GPIO_OUTPUT(ret, config->bck);
  for (int i = 0; i < sizeof(config->rgb) / sizeof(config->rgb[0]); i++) {
    SET_GPIO_OUTPUT(ret, config->rgb[i]);
  }

  SET_GPIO_OUTPUT(ret, config->vcom_vb);
  SET_GPIO_OUTPUT(ret, config->va);
  k_thread_create(&data->vcom_thread, data->vcom_thread_stack,
                  K_KERNEL_STACK_SIZEOF(data->vcom_thread_stack),
                  (k_thread_entry_t)vcom_thread, /*arg1=*/(void *)config,
                  /*arg2=*/NULL, /*arg3=*/NULL, K_PRIO_PREEMPT(7),
                  /*options=*/0, K_NO_WAIT);

  // Collect RGB gpios into data->rgb_ports.
  for (int gpio_idx = 0;
       gpio_idx < sizeof(config->rgb) / sizeof(config->rgb[0]); gpio_idx++) {
    for (int port_idx = 0;
         port_idx < sizeof(data->rgb_ports) / sizeof(data->rgb_ports[0]);
         port_idx++) {
      if (data->rgb_ports[port_idx].port == config->rgb[gpio_idx].port ||
          data->rgb_ports[port_idx].port == NULL) {
        data->rgb_ports[port_idx].port = config->rgb[gpio_idx].port;
        data->rgb_ports[port_idx].rgb_idx_mask |= (1 << gpio_idx);
        data->rgb_ports[port_idx].port_mask |= (1 << config->rgb[gpio_idx].pin);
        break;
      }
    }
  }

  LOG_DBG("Sharp MIP display initialized. Resolution: %dx%d", config->width,
          config->height);

  for (int i = 0; i < sizeof(data->rgb_ports) / sizeof(data->rgb_ports[0]);
       i++) {
    LOG_DBG("RGB port %d: %p, mask: 0x%02x", i, data->rgb_ports[i].port,
            data->rgb_ports[i].rgb_idx_mask);
  }

  return 0;
}

static inline void set_rgb(bool is_msb, int x0, const uint8_t *buf,
                           const struct sharp_mip_config *cfg,
                           const struct sharp_mip_data *data) {
#if CONFIG_SHARP_LS0XXB7_DISPLAY_MODE_COLOR

// Convert from RGB565 to RGB222 by dropping the least significant bits.
#define CVT_5_TO_2_BITS(val) (((val) >> 3) & 0x03)
#define CVT_6_TO_2_BITS(val) (((val) >> 4) & 0x03)

// TODO: Make this endianess-agnostic.
// Extract 2-bit R, G, B values from a lil-endian 16-bit RGB565 pointed by buf.
#define _R(buf) CVT_5_TO_2_BITS(((buf)[1] >> 3) & 0x1f)
#define _G(buf) CVT_6_TO_2_BITS((((buf)[1] & 0x7) << 3) | ((buf)[0] >> 5))
#define _B(buf) CVT_5_TO_2_BITS(((buf)[0]) & 0x1f)

// Get either MSB or LSB from from a 2-bit value.
#define GET_SIG_BIT(v, is_msb) ((v) >> ((is_msb) ? 0 : 1) & 0x1)

// Set bit on port register if rgb_idx is on this port.
#define VAL_BIT_IF_ON_PORT(port, rgb_idx, v2bit)                   \
  ((data->rgb_ports[(port)].rgb_idx_mask & BIT(rgb_idx))           \
       ? (GET_SIG_BIT((v2bit), is_msb) << cfg->rgb[(rgb_idx)].pin) \
       : 0)

  // Offset into buf for 16-bit RGB565 data at column x0.
  const uint8_t *b = buf + 2 * x0;

  for (int port_idx = 0;
       port_idx < sizeof(data->rgb_ports) / sizeof(data->rgb_ports[0]);
       port_idx++) {
    // Skip port if it's not connected to any RGB pins.
    if (data->rgb_ports[port_idx].port == NULL) {
      continue;
    }

    const gpio_port_value_t val = VAL_BIT_IF_ON_PORT(port_idx, 0, _R(b + 0)) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 1, _R(b + 2)) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 2, _G(b + 0)) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 3, _G(b + 2)) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 4, _B(b + 0)) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 5, _B(b + 2));

    gpio_port_set_masked(data->rgb_ports[port_idx].port,
                         data->rgb_ports[port_idx].port_mask, val);
  }

#elif CONFIG_SHARP_LS0XXB7_DISPLAY_MODE_MONOCHROME

#define GET_BUF_BIT(buf, x) (((uint8_t *)buf)[(x) / 8] >> ((x) % 8) & 0x1)

#define VAL_BIT_IF_ON_PORT(port, rgb_idx, buf, x)             \
  ((data->rgb_ports[(port)].rgb_idx_mask & BIT(rgb_idx))      \
       ? (GET_BUF_BIT((buf), (x)) << cfg->rgb[(rgb_idx)].pin) \
       : 0)

  for (int port_idx = 0;
       port_idx < sizeof(data->rgb_ports) / sizeof(data->rgb_ports[0]);
       port_idx++) {
    // Skip port if it's not connected to any RGB pins.
    if (data->rgb_ports[port_idx].port == NULL) {
      continue;
    }

    const gpio_port_value_t val = VAL_BIT_IF_ON_PORT(port_idx, 0, buf, x0 + 0) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 1, buf, x0 + 1) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 2, buf, x0 + 0) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 3, buf, x0 + 1) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 4, buf, x0 + 0) |
                                  VAL_BIT_IF_ON_PORT(port_idx, 5, buf, x0 + 1);

    gpio_port_set_masked(data->rgb_ports[port_idx].port,
                         data->rgb_ports[port_idx].port_mask, val);
  }

#endif  // CONFIG_SHARP_LS0XXB7_DISPLAY_MODE
}

static inline void set(const struct gpio_dt_spec *gpio) {
  gpio_pin_set_raw(gpio->port, gpio->pin, 1);
}
static inline void clear(const struct gpio_dt_spec *gpio) {
  gpio_pin_set_raw(gpio->port, gpio->pin, 0);
}
static inline void toggle(const struct gpio_dt_spec *gpio) {
  int val = gpio_pin_get_raw(gpio->port, gpio->pin);
  gpio_pin_set_raw(gpio->port, gpio->pin, !val);
}

static inline void send_half_line(bool is_msb, const void *buf,
                                  const struct sharp_mip_config *cfg,
                                  const struct sharp_mip_data *data) {
  clear(&cfg->bsp);
  set(&cfg->bsp);

  for (int i = 1; i <= 144; i++) {
    toggle(&cfg->bck);

    if (i == 2) {
      clear(&cfg->bsp);
    }
    if (i >= 1 && i <= 140) {
      // Prepare RGB pins for the next BCK edge.
      set_rgb(is_msb, /*x0=*/2 * (i - 1), buf, cfg, data);
    }
  }
}

static int sharp_mip_write(const struct device *dev, const uint16_t x,
                           const uint16_t y,
                           const struct display_buffer_descriptor *desc,
                           const void *buf) {
  const struct sharp_mip_config *cfg = dev->config;

  // The display only supports partial updates at full rows. I.e.: we can
  // start drawing at (y = 10, x = 0), but _not_ at (y, x = 10).
  if (x != 0) {
    LOG_ERR(
        "Full screen or partial updates must be at full rows! x: %d, y = %d, "
        "buf size: %d",
        x, y, desc->buf_size);
    return -ENOTSUP;
  }

  // Offset for GCK. For full-screen updates, this is 2. For partial updates,
  // it's 2 plus the number of half-lines to skip.
  const int gck_offset = 2 + y * 2;

  // The last GCK index of the last sent half-line.
  const int gck_last_half_line = gck_offset + 2 * desc->height - 1;

  LOG_DBG(
      "Sharp MIP display write. x: %d, y: %d; buf size: %d (buf height: %d,buf "
      "width: %d). Offset: %d, last: %d",
      x, y, desc->buf_size, desc->height, desc->width, gck_offset,
      gck_last_half_line);

  clear(&cfg->gck);
  set(&cfg->intb);
  set(&cfg->gsp);

  // 1-indexed to match the datasheet and improve debugging.
  for (int i = 1; i <= 568; i++) {
    toggle(&cfg->gck);

    if (i == 2) {
      clear(&cfg->gsp);
    }

    if (i == gck_offset) {
      send_half_line(/*is_msb=*/true, buf, cfg, dev->data);
    } else if (i >= gck_offset + 1 && i <= gck_last_half_line) {
      set(&cfg->gen);

      // This display sends MSB for a full line at a GCK edge and then the LSB
      // for the same line at the next GCK edge.
      const uint16_t display_line = (i - gck_offset) / 2;
      const bool is_msb = (i - gck_offset) % 2 == 0;

#if CONFIG_SHARP_LS0XXB7_DISPLAY_MODE_COLOR
      // Color buffer, 16 bits per pixel.
      // Each line has buf_width pixels, which is buf_width * 2.
      uint8_t *line_buf = (uint8_t *)buf + display_line * desc->width * 2;
#elif CONFIG_SHARP_LS0XXB7_DISPLAY_MODE_MONOCHROME
      // Monochrome buffer, 1 bit per pixel.
      // Each line has 280 pixels, which is 280 / 8 = 35 bytes.
      uint8_t *line_buf = (uint8_t *)buf + display_line * 35;
#endif  // CONFIG_SHARP_LS0XXB7_DISPLAY_MODE

      send_half_line(is_msb, line_buf, cfg, dev->data);
      clear(&cfg->gen);
    } else if (i == gck_last_half_line + 1) {
      set(&cfg->gen);
      clear(&cfg->gen);
    } else if (i == 566) {
      clear(&cfg->intb);
    }
  }

  return 0;
}

static void sharp_mip_get_capabilities(
    const struct device *dev, struct display_capabilities *capabilities) {
  const struct sharp_mip_config *config = dev->config;

  capabilities->x_resolution = config->width;
  capabilities->y_resolution = config->height;

#if CONFIG_SHARP_LS0XXB7_DISPLAY_MODE_COLOR
  // TODO: This is wasteful. We are requesting 16 bits per pixel here, and we
  // only need 6 bits per pixel. In 2025-03, Zephyr introduced
  // (https://github.com/zephyrproject-rtos/zephyr/pull/86821) the
  // PIXEL_FORMAT_L_8. It's intended for 8-bit grayscale, but I think we can
  // use it for 6-bit color here, saving us half the buffer size.
  capabilities->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
  capabilities->current_pixel_format = PIXEL_FORMAT_RGB_565;
#elif CONFIG_SHARP_LS0XXB7_DISPLAY_MODE_MONOCHROME
  capabilities->supported_pixel_formats = PIXEL_FORMAT_MONO10;
  capabilities->current_pixel_format = PIXEL_FORMAT_MONO10;
#endif  // CONFIG_SHARP_LS0XXB7_DISPLAY_MODE

  // We need partial updates to contain full rows. Note that as of writing,
  // this flag only takes effect for monochrome pixel formats.
  capabilities->screen_info = SCREEN_INFO_X_ALIGNMENT_WIDTH;

  // TODO: get from config.
  capabilities->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

struct display_driver_api sharp_mip_driver_api = {
    .write = sharp_mip_write,
    .get_capabilities = sharp_mip_get_capabilities,
};

#define SHARP_MIP_DEFINE(node_id)                                  \
  static struct sharp_mip_data data_##node_id = {                  \
      .rgb_ports =                                                 \
          {                                                        \
              {.port = NULL, .rgb_idx_mask = 0, .port_mask = 0},   \
              {.port = NULL, .rgb_idx_mask = 0, .port_mask = 0},   \
          },                                                       \
  };                                                               \
  static const struct sharp_mip_config config_##node_id = {        \
      .height = DT_PROP(node_id, height),                          \
      .width = DT_PROP(node_id, width),                            \
      .intb = GPIO_DT_SPEC_GET(node_id, intb_gpios),               \
      .gsp = GPIO_DT_SPEC_GET(node_id, gsp_gpios),                 \
      .gck = GPIO_DT_SPEC_GET(node_id, gck_gpios),                 \
      .gen = GPIO_DT_SPEC_GET(node_id, gen_gpios),                 \
      .bsp = GPIO_DT_SPEC_GET(node_id, bsp_gpios),                 \
      .bck = GPIO_DT_SPEC_GET(node_id, bck_gpios),                 \
      .rgb = {GPIO_DT_SPEC_GET_BY_IDX(node_id, rgb_gpios, 0),      \
              GPIO_DT_SPEC_GET_BY_IDX(node_id, rgb_gpios, 1),      \
              GPIO_DT_SPEC_GET_BY_IDX(node_id, rgb_gpios, 2),      \
              GPIO_DT_SPEC_GET_BY_IDX(node_id, rgb_gpios, 3),      \
              GPIO_DT_SPEC_GET_BY_IDX(node_id, rgb_gpios, 4),      \
              GPIO_DT_SPEC_GET_BY_IDX(node_id, rgb_gpios, 5)},     \
      .vcom_vb = GPIO_DT_SPEC_GET(node_id, vb_gpios),              \
      .va = GPIO_DT_SPEC_GET(node_id, va_gpios),                   \
      .vcom_freq = DT_PROP(node_id, vcom_freq),                    \
  };                                                               \
  DEVICE_DT_DEFINE(node_id, sharp_mip_init, NULL, &data_##node_id, \
                   &config_##node_id, POST_KERNEL,                 \
                   CONFIG_DISPLAY_INIT_PRIORITY, &sharp_mip_driver_api);

DT_FOREACH_STATUS_OKAY(sharp_ls0xxb7_bitbang, SHARP_MIP_DEFINE);
