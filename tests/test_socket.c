/* Opt-in hardened control-socket tests for the libuvch264src element (Task 16).
 *
 * Same self-contained shape as test_device_select.c / test_ptz.c: the element
 * translation units, the libuvc mock, and the driver are linked into ONE
 * executable with the element type registered statically, so start()/stop() run
 * the REAL socket bind/unbind against the filesystem (the mock only stands in for
 * libuvc, not for socket(2)). Each gst-check test is its own ctest entry, and
 * each creates a private 0700 $XDG_RUNTIME_DIR so the cases never collide under
 * `ctest -j`.
 *
 * Covered:
 *   test_socket_default_off  control-socket defaults FALSE: start() binds no
 *                            socket, the path property stays NULL, the runtime
 *                            dir gains no socket file.
 *   test_socket_hardened     two enabled instances: each binds a per-instance
 *                            path under $XDG_RUNTIME_DIR, mode 0600, the two
 *                            paths differ, and both are unlinked on stop().
 *
 * Results are captured while the pipeline is live and only asserted after
 * teardown: with CK_FORK=no a failing fail_unless longjmps out and would
 * otherwise leave the control thread keeping the process alive until the ctest
 * timeout (see test_device_select.c).
 */

#include <sys/stat.h>
#include <glib/gstdio.h>
#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

static void
setup (void)
{
  const gchar *core_plugin = g_getenv ("GST_COREELEMENTS_PLUGIN");
  if (core_plugin != NULL && *core_plugin != '\0') {
    GError *lerr = NULL;
    GstPlugin *p = gst_plugin_load_file (core_plugin, &lerr);
    fail_unless (p != NULL, "could not load core-elements plugin '%s': %s",
        core_plugin, lerr ? lerr->message : "(unknown)");
    gst_object_unref (p);
  }

  static gboolean registered = FALSE;
  if (!registered) {
    fail_unless (gst_element_register (NULL, "libuvch264src", GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register libuvch264src");
    registered = TRUE;
  }

  mock_uvc_reset ();
}

/* A private runtime dir per test so the cases stay isolated; g_dir_make_tmp
 * creates it 0700, exactly the protection $XDG_RUNTIME_DIR carries in prod. */
static gchar *
make_runtime_dir (void)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp ("uvc-sock-XXXXXX", &err);
  fail_unless (dir != NULL, "could not create runtime dir: %s",
      err ? err->message : "(unknown)");
  g_setenv ("XDG_RUNTIME_DIR", dir, TRUE);
  return dir;
}

static int
count_sockets_in_dir (const gchar * dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  if (d == NULL)
    return -1;

  int n = 0;
  const gchar *name;
  while ((name = g_dir_read_name (d)) != NULL) {
    gchar *full = g_build_filename (dir, name, NULL);
    GStatBuf st;
    if (g_lstat (full, &st) == 0 && S_ISSOCK (st.st_mode))
      n++;
    g_free (full);
  }
  g_dir_close (d);
  return n;
}

static GstElement *
build_pipeline (const gchar * name, GstElement ** src_out)
{
  GstElement *pipeline = gst_pipeline_new (name);
  GstElement *src = gst_element_factory_make ("libuvch264src", NULL);
  GstElement *sink = gst_element_factory_make ("fakesink", NULL);

  fail_unless (pipeline != NULL && src != NULL && sink != NULL,
      "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);
  g_object_set (src, "index", "0", NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");

  *src_out = src;
  return pipeline;
}

GST_START_TEST (test_socket_default_off)
{
  gchar *runtime = make_runtime_dir ();
  mock_uvc_set_device_count (1);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline ("off", &src);

  GstStateChangeReturn sret =
      gst_element_set_state (pipeline, GST_STATE_PAUSED);

  gboolean enabled = TRUE;
  gchar *path = NULL;
  g_object_get (src, "control-socket", &enabled, "control-socket-path", &path,
      NULL);
  int sock_files = count_sockets_in_dir (runtime);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (sret != GST_STATE_CHANGE_FAILURE, "start() should have succeeded");
  fail_unless (!enabled, "control-socket must default to FALSE");
  fail_unless (path == NULL,
      "control-socket-path must be NULL when disabled, got '%s'",
      path ? path : "(null)");
  fail_unless (sock_files == 0,
      "no socket must be bound when disabled, found %d in %s", sock_files,
      runtime);

  g_free (path);
  g_rmdir (runtime);
  g_free (runtime);
}

GST_END_TEST;

GST_START_TEST (test_socket_hardened)
{
  gchar *runtime = make_runtime_dir ();
  mock_uvc_set_device_count (1);

  GstElement *srcA = NULL, *srcB = NULL;
  GstElement *pipeA = build_pipeline ("a", &srcA);
  GstElement *pipeB = build_pipeline ("b", &srcB);
  g_object_set (srcA, "control-socket", TRUE, NULL);
  g_object_set (srcB, "control-socket", TRUE, NULL);

  GstStateChangeReturn sretA =
      gst_element_set_state (pipeA, GST_STATE_PAUSED);
  GstStateChangeReturn sretB =
      gst_element_set_state (pipeB, GST_STATE_PAUSED);

  gchar *pathA = NULL, *pathB = NULL;
  g_object_get (srcA, "control-socket-path", &pathA, NULL);
  g_object_get (srcB, "control-socket-path", &pathB, NULL);

  GStatBuf stA, stB;
  int rA = (pathA != NULL) ? g_lstat (pathA, &stA) : -1;
  int rB = (pathB != NULL) ? g_lstat (pathB, &stB) : -1;
  gboolean issockA = (rA == 0) && S_ISSOCK (stA.st_mode);
  gboolean issockB = (rB == 0) && S_ISSOCK (stB.st_mode);
  unsigned int modeA = (rA == 0) ? (stA.st_mode & 0777) : 0;
  unsigned int modeB = (rB == 0) ? (stB.st_mode & 0777) : 0;
  gboolean underA = (pathA != NULL) && g_str_has_prefix (pathA, runtime);
  gboolean underB = (pathB != NULL) && g_str_has_prefix (pathB, runtime);
  gboolean distinct =
      (pathA != NULL) && (pathB != NULL) && (g_strcmp0 (pathA, pathB) != 0);

  gst_element_set_state (pipeA, GST_STATE_NULL);
  gst_element_set_state (pipeB, GST_STATE_NULL);

  GStatBuf gone;
  gboolean goneA = (pathA != NULL) && (g_lstat (pathA, &gone) != 0);
  gboolean goneB = (pathB != NULL) && (g_lstat (pathB, &gone) != 0);

  gst_object_unref (pipeA);
  gst_object_unref (pipeB);

  fail_unless (sretA != GST_STATE_CHANGE_FAILURE
      && sretB != GST_STATE_CHANGE_FAILURE,
      "start() should succeed for both instances");
  fail_unless (pathA != NULL && pathB != NULL,
      "each enabled instance must resolve a path (A='%s' B='%s')",
      pathA ? pathA : "(null)", pathB ? pathB : "(null)");
  fail_unless (underA && underB,
      "socket paths must live under $XDG_RUNTIME_DIR (%s): A='%s' B='%s'",
      runtime, pathA, pathB);
  fail_unless (issockA && issockB, "bound paths must be sockets");
  fail_unless (modeA == 0600 && modeB == 0600,
      "sockets must be mode 0600 (A=%o B=%o)", modeA, modeB);
  fail_unless (distinct,
      "two instances must not collide on one path: '%s'", pathA);
  fail_unless (goneA && goneB, "sockets must be unlinked on stop()");

  g_free (pathA);
  g_free (pathB);
  g_rmdir (runtime);
  g_free (runtime);
}

GST_END_TEST;

static Suite *
socket_suite (void)
{
  Suite *s = suite_create ("libuvch264src-socket");
  TCase *tc = tcase_create ("socket");

  tcase_set_timeout (tc, 60);
  tcase_add_checked_fixture (tc, setup, NULL);
  suite_add_tcase (s, tc);

  tcase_add_test (tc, test_socket_default_off);
  tcase_add_test (tc, test_socket_hardened);

  return s;
}

GST_CHECK_MAIN (socket);
