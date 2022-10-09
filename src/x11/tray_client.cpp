#include "x11/tray_client.hpp"

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>

#include "utils/memory.hpp"
#include "x11/connection.hpp"
#include "x11/ewmh.hpp"
#include "x11/winspec.hpp"

POLYBAR_NS

/*
 * TODO proper background of wrapper window
 *
 * Do first possible:
 *
 * 1. Use PARENT_RELATIVE if tray window depths, etc. matches the bar window
 * 2. Use pseudo-transparency when activated (make sure the depths match)
 * 3. Use background color
 */
tray_client::tray_client(
    const logger& log, connection& conn, xcb_window_t parent, xcb_window_t win, size s, uint32_t desired_background)
    : m_log(log)
    , m_connection(conn)
    , m_name(ewmh_util::get_wm_name(win))
    , m_client(win)
    , m_size(s)
    , m_desired_background(desired_background)
    , m_background_manager(background_manager::make()) {
  auto geom = conn.get_geometry(win);
  auto attrs = conn.get_window_attributes(win);
  int client_depth = geom->depth;
  auto client_visual = attrs->visual;
  auto client_colormap = attrs->colormap;

  m_log.trace("%s: depth: %u, width: %u, height: %u", name(), client_depth, geom->width, geom->height);

  /*
   * Create embedder window for tray icon
   *
   * The embedder window inherits the depth, visual and color map from the icon window in order for reparenting to
   * always work, even if the icon window uses ParentRelative for some of its pixmaps (back pixmap or border pixmap).
   */
  // clang-format off
  m_wrapper = winspec(conn)
    << cw_size(s.w, s.h)
    << cw_pos(0, 0)
    << cw_depth(client_depth)
    << cw_visual(client_visual)
    << cw_parent(parent)
    << cw_class(XCB_WINDOW_CLASS_INPUT_OUTPUT)
    // The X server requires the border pixel to be defined if the depth doesn't match the parent (bar) window
    << cw_params_border_pixel(conn.screen()->black_pixel)
    << cw_params_backing_store(XCB_BACKING_STORE_WHEN_MAPPED)
    << cw_params_save_under(true)
    << cw_params_event_mask(XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
        | XCB_EVENT_MASK_PROPERTY_CHANGE
        | XCB_EVENT_MASK_STRUCTURE_NOTIFY
        | XCB_EVENT_MASK_EXPOSURE)
    << cw_params_colormap(client_colormap)
    << cw_flush(true);
  // clang-format on

  // TODO destroy in destructor
  xcb_pixmap_t pixmap = m_connection.generate_id();

  try {
    m_connection.create_pixmap_checked(client_depth, pixmap, m_wrapper, s.w, s.h);
  } catch (const std::exception& err) {
    // TODO in case of error, fall back to desired_background
    m_log.err("Failed to create pixmap for tray background (err: %s)", err.what());
    throw;
  }

  try {
    m_connection.change_window_attributes_checked(m_wrapper, XCB_CW_BACK_PIXMAP, &pixmap);
  } catch (const std::exception& err) {
    // TODO in case of error, fall back to desired_background
    m_log.err("Failed to set tray window back pixmap (%s)", err.what());
    throw;
  }

  // TODO destroy in destructor
  xcb_gcontext_t gc = m_connection.generate_id();
  try {
    xcb_params_gc_t params{};
    uint32_t mask = 0;
    XCB_AUX_ADD_PARAM(&mask, &params, graphics_exposures, 1);
    std::array<uint32_t, 32> values;
    connection::pack_values(mask, &params, values);
    m_connection.create_gc_checked(gc, pixmap, mask, values.data());
  } catch (const std::exception& err) {
    m_log.err("Failed to create gcontext for tray background (err: %s)", err.what());
    throw;
  }

  xcb_visualtype_t* visual = m_connection.visual_type_for_id(client_visual);
  if (!visual) {
    // TODO in case of error, fall back to desired_background
    throw std::runtime_error("Failed to get root visual for tray background");
  }

  m_surface = make_unique<cairo::xcb_surface>(m_connection, pixmap, visual, s.w, s.h);
  m_context = make_unique<cairo::context>(*m_surface, m_log);

  observe_background();
}

tray_client::~tray_client() {
  if (m_client != XCB_NONE) {
    xembed::unembed(m_connection, m_client, m_connection.root());
  }

  if (m_wrapper != XCB_NONE) {
    m_connection.destroy_window(m_wrapper);
  }
}

string tray_client::name() const {
  return "tray_client(" + m_connection.id(m_client) + ", " + m_name + ")";
}

unsigned int tray_client::width() const {
  return m_size.w;
}

unsigned int tray_client::height() const {
  return m_size.h;
}

void tray_client::clear_window() const {
  // Do not produce Expose events for the embedder because that triggers an infinite loop.
  m_connection.clear_area_checked(0, embedder(), 0, 0, width(), height());
  m_connection.clear_area_checked(1, client(), 0, 0, width(), height());
}

void tray_client::update_client_attributes() const {
  uint32_t configure_mask = 0;
  std::array<uint32_t, 32> configure_values{};
  xcb_params_cw_t configure_params{};

  XCB_AUX_ADD_PARAM(
      &configure_mask, &configure_params, event_mask, XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY);

  connection::pack_values(configure_mask, &configure_params, configure_values);

  m_log.trace("%s: Update client window", name());
  m_connection.change_window_attributes_checked(client(), configure_mask, configure_values.data());
}

void tray_client::reparent() const {
  m_log.trace("%s: Reparent client", name());
  m_connection.reparent_window_checked(client(), embedder(), 0, 0);
}

/**
 * Is this the client for the given client window
 */
bool tray_client::match(const xcb_window_t& win) const {
  return win == m_client;
}

/**
 * Get client window mapped state
 */
bool tray_client::mapped() const {
  return m_mapped;
}

/**
 * Set client window mapped state
 */
void tray_client::mapped(bool state) {
  if (m_mapped != state) {
    m_log.trace("%s: set mapped: %i", name(), state);
    m_mapped = state;
  }
}

/**
 * Sets the client window's visibility.
 *
 * Use this to trigger a mapping/unmapping
 */
void tray_client::hidden(bool state) {
  m_hidden = state;
}

/**
 * Whether the current state indicates the client should be mapped.
 */
bool tray_client::should_be_mapped() const {
  if (m_hidden) {
    return false;
  }

  if (is_xembed_supported()) {
    return m_xembed.is_mapped();
  }

  return true;
}

xcb_window_t tray_client::embedder() const {
  return m_wrapper;
}

xcb_window_t tray_client::client() const {
  return m_client;
}

void tray_client::query_xembed() {
  m_xembed_supported = xembed::query(m_connection, m_client, m_xembed);

  if (is_xembed_supported()) {
    m_log.trace("%s: %s", name(), get_xembed().to_string());
  } else {
    m_log.trace("%s: no xembed");
  }
}

bool tray_client::is_xembed_supported() const {
  return m_xembed_supported;
}

const xembed::info& tray_client::get_xembed() const {
  return m_xembed;
}

void tray_client::notify_xembed() const {
  if (is_xembed_supported()) {
    m_log.trace("%s: Send embedded notification to client", name());
    xembed::notify_embedded(m_connection, client(), embedder(), m_xembed.get_version());
  }
}

void tray_client::add_to_save_set() const {
  m_log.trace("%s: Add client window to the save set", name());
  m_connection.change_save_set_checked(XCB_SET_MODE_INSERT, client());
}

/**
 * Make sure that the window mapping state is correct
 */
void tray_client::ensure_state() const {
  bool new_state = should_be_mapped();

  if (new_state == m_mapped) {
    return;
  }

  m_log.trace("%s: ensure_state (hidden=%i, mapped=%i, should_be_mapped=%i)", name(), m_hidden, m_mapped, new_state);

  if (new_state) {
    m_log.trace("%s: Map client", name());
    m_connection.map_window_checked(embedder());
    m_connection.map_window_checked(client());
  } else {
    m_log.trace("%s: Unmap client", name());
    m_connection.unmap_window_checked(client());
    m_connection.unmap_window_checked(embedder());
  }
}

/**
 * Configure window position
 */
void tray_client::set_position(int x, int y) {
  m_log.trace("%s: moving to (%d, %d)", name(), x, y);

  position new_pos{x, y};

  if (new_pos == m_pos) {
    return;
  }

  m_pos = new_pos;

  uint32_t configure_mask = 0;
  array<uint32_t, 32> configure_values{};
  xcb_params_configure_window_t configure_params{};

  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, width, m_size.w);
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, height, m_size.h);
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, x, x);
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, y, y);
  connection::pack_values(configure_mask, &configure_params, configure_values);
  m_connection.configure_window_checked(embedder(), configure_mask, configure_values.data());

  configure_mask = 0;
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, width, m_size.w);
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, height, m_size.h);
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, x, 0);
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, y, 0);
  connection::pack_values(configure_mask, &configure_params, configure_values);
  m_connection.configure_window_checked(client(), configure_mask, configure_values.data());

  // The position has changed, we need a new background slice.
  observe_background();
}

/**
 * Respond to client resize/move requests
 */
void tray_client::configure_notify() const {
  xcb_configure_notify_event_t notify;
  notify.response_type = XCB_CONFIGURE_NOTIFY;
  notify.event = client();
  notify.window = client();
  notify.override_redirect = false;
  notify.above_sibling = 0;
  notify.x = 0;
  notify.y = 0;
  notify.width = m_size.w;
  notify.height = m_size.h;
  notify.border_width = 0;

  unsigned int mask{XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  m_connection.send_event_checked(false, client(), mask, reinterpret_cast<const char*>(&notify));
}

/**
 * Redraw background using the observed background slice.
 */
void tray_client::update_bg() const {
  m_log.trace("%s: Update background", name());

  // Composite background slice with background color.
  m_context->clear();
  *m_context << CAIRO_OPERATOR_SOURCE << *m_bg_slice->get_surface();
  m_context->paint();
  *m_context << CAIRO_OPERATOR_OVER << m_desired_background;
  m_context->paint();

  m_surface->flush();

  clear_window();
  m_connection.flush();
}

void tray_client::observe_background() {
  xcb_rectangle_t rect{0, 0, static_cast<uint16_t>(m_size.w), static_cast<uint16_t>(m_size.h)};
  m_bg_slice = m_background_manager.observe(rect, embedder());

  update_bg();
}

POLYBAR_NS_END
