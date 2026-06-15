/* Hardware-independent plugin-load smoke test for the libuvch264src element.
 *
 * This deliberately exercises ONLY the registration / introspection surface:
 *   - the plugin module loads and registers in the GStreamer registry,
 *   - both element factories (primary + alias) exist,
 *   - the element instantiates as a GstPushSrc,
 *   - the documented "index" property is present with its default,
 *   - the native "pan"/"tilt"/"zoom" PTZ properties are present (int, default 0),
 *   - the opt-in "control-socket" property is present (boolean, default off),
 *   - the opt-in "reconnect" property is present (boolean, default off),
 *   - the ALWAYS "src" pad template advertises H.264 AND H.265 caps.
 *
 * No UVC device is opened: gst_element_factory_make() only runs class/instance
 * init; the camera is only touched on the READY->PAUSED start() transition,
 * which this test never triggers. Safe to run on CI without capture hardware.
 */

#include <gst/check/gstcheck.h>
#include <gst/base/gstpushsrc.h>

#define PLUGIN_NAME "libuvch264src"
#define ELEMENT_NAME "libuvch264src"
#define ELEMENT_ALIAS "libuvch26xsrc"

GST_START_TEST (test_plugin_is_registered)
{
  GstPlugin *plugin =
      gst_registry_find_plugin (gst_registry_get (), PLUGIN_NAME);
  fail_unless (plugin != NULL,
      "GStreamer plugin '%s' is not registered - is GST_PLUGIN_PATH pointing "
      "at the freshly built module directory?", PLUGIN_NAME);
  gst_object_unref (plugin);
}

GST_END_TEST;

GST_START_TEST (test_element_factories_exist)
{
  GstElementFactory *factory = gst_element_factory_find (ELEMENT_NAME);
  fail_unless (factory != NULL, "element factory '%s' not found", ELEMENT_NAME);
  gst_object_unref (factory);

  GstElementFactory *alias = gst_element_factory_find (ELEMENT_ALIAS);
  fail_unless (alias != NULL, "alias factory '%s' not found", ELEMENT_ALIAS);
  gst_object_unref (alias);
}

GST_END_TEST;

GST_START_TEST (test_element_creates_and_is_pushsrc)
{
  GstElement *element = gst_element_factory_make (ELEMENT_NAME, NULL);
  fail_unless (element != NULL, "could not instantiate '%s'", ELEMENT_NAME);
  fail_unless (GST_IS_PUSH_SRC (element), "'%s' is not a GstPushSrc",
      ELEMENT_NAME);
  gst_object_unref (element);
}

GST_END_TEST;

GST_START_TEST (test_element_has_index_property)
{
  GstElement *element = gst_element_factory_make (ELEMENT_NAME, NULL);
  fail_unless (element != NULL);

  GParamSpec *pspec =
      g_object_class_find_property (G_OBJECT_GET_CLASS (element), "index");
  fail_unless (pspec != NULL, "expected 'index' property is missing");
  fail_unless (pspec->value_type == G_TYPE_STRING,
      "'index' property should be a string");

  gchar *index = NULL;
  g_object_get (element, "index", &index, NULL);
  fail_unless (index != NULL && g_strcmp0 (index, "0") == 0,
      "default 'index' should be \"0\", got '%s'", index ? index : "(null)");
  g_free (index);

  gst_object_unref (element);
}

GST_END_TEST;

/* Native PTZ properties (Task 12): pan/tilt/zoom are class-level G_TYPE_INT
 * spec'd with default 0. They exist in introspection on every device; the
 * real per-device range is enforced at set time, so the default is what a
 * freshly created (unstarted) element reports here. */
GST_START_TEST (test_element_has_ptz_properties)
{
  GstElement *element = gst_element_factory_make (ELEMENT_NAME, NULL);
  fail_unless (element != NULL);

  GObjectClass *klass = G_OBJECT_GET_CLASS (element);
  const gchar *axes[] = { "pan", "tilt", "zoom" };

  for (guint i = 0; i < G_N_ELEMENTS (axes); i++) {
    GParamSpec *pspec = g_object_class_find_property (klass, axes[i]);
    fail_unless (pspec != NULL, "expected '%s' property is missing", axes[i]);
    fail_unless (pspec->value_type == G_TYPE_INT,
        "'%s' property should be an int", axes[i]);

    gint value = -1;
    g_object_get (element, axes[i], &value, NULL);
    fail_unless (value == 0, "default '%s' should be 0, got %d", axes[i], value);
  }

  gst_object_unref (element);
}

GST_END_TEST;

/* Opt-in PTZ control socket (Task 16): boolean, default FALSE so nothing binds
 * a Unix-domain socket unless explicitly enabled. */
GST_START_TEST (test_element_has_control_socket_property)
{
  GstElement *element = gst_element_factory_make (ELEMENT_NAME, NULL);
  fail_unless (element != NULL);

  GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (element),
      "control-socket");
  fail_unless (pspec != NULL, "expected 'control-socket' property is missing");
  fail_unless (pspec->value_type == G_TYPE_BOOLEAN,
      "'control-socket' property should be a boolean");

  gboolean enabled = TRUE;
  g_object_get (element, "control-socket", &enabled, NULL);
  fail_unless (enabled == FALSE, "default 'control-socket' should be FALSE");

  gst_object_unref (element);
}

GST_END_TEST;

/* Opt-in in-element auto-reconnect (Task 18): boolean, default FALSE so a
 * mid-stream disconnect always errors out unless reconnect is explicitly on. */
GST_START_TEST (test_element_has_reconnect_property)
{
  GstElement *element = gst_element_factory_make (ELEMENT_NAME, NULL);
  fail_unless (element != NULL);

  GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (element),
      "reconnect");
  fail_unless (pspec != NULL, "expected 'reconnect' property is missing");
  fail_unless (pspec->value_type == G_TYPE_BOOLEAN,
      "'reconnect' should be boolean");

  gboolean val = TRUE;
  g_object_get (element, "reconnect", &val, NULL);
  fail_unless (val == FALSE, "default 'reconnect' should be FALSE");

  gst_object_unref (element);
}

GST_END_TEST;

GST_START_TEST (test_src_pad_template)
{
  GstElementFactory *factory = gst_element_factory_find (ELEMENT_NAME);
  fail_unless (factory != NULL);

  const GList *templates =
      gst_element_factory_get_static_pad_templates (factory);
  gboolean found_src = FALSE;

  for (const GList *l = templates; l != NULL; l = l->next) {
    GstStaticPadTemplate *tmpl = (GstStaticPadTemplate *) l->data;

    if (tmpl->direction == GST_PAD_SRC
        && g_strcmp0 (tmpl->name_template, "src") == 0) {
      found_src = TRUE;
      fail_unless (tmpl->presence == GST_PAD_ALWAYS,
          "src pad template should be ALWAYS present");

      GstCaps *caps = gst_static_caps_get (&tmpl->static_caps);
      fail_unless (caps != NULL && !gst_caps_is_empty (caps),
          "src pad template caps are empty");

      gchar *caps_str = gst_caps_to_string (caps);
      fail_unless (g_strrstr (caps_str, "video/x-h264") != NULL,
          "src caps missing video/x-h264: %s", caps_str);
      fail_unless (g_strrstr (caps_str, "video/x-h265") != NULL,
          "src caps missing video/x-h265: %s", caps_str);
      g_free (caps_str);
      gst_caps_unref (caps);
    }
  }

  fail_unless (found_src, "no ALWAYS src pad template named 'src'");
  gst_object_unref (factory);
}

GST_END_TEST;

static Suite *
plugin_load_suite (void)
{
  Suite *s = suite_create ("libuvch264src-plugin-load");
  TCase *tc = tcase_create ("smoke");

  suite_add_tcase (s, tc);
  tcase_add_test (tc, test_plugin_is_registered);
  tcase_add_test (tc, test_element_factories_exist);
  tcase_add_test (tc, test_element_creates_and_is_pushsrc);
  tcase_add_test (tc, test_element_has_index_property);
  tcase_add_test (tc, test_element_has_ptz_properties);
  tcase_add_test (tc, test_element_has_control_socket_property);
  tcase_add_test (tc, test_element_has_reconnect_property);
  tcase_add_test (tc, test_src_pad_template);

  return s;
}

GST_CHECK_MAIN (plugin_load);
