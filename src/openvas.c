/* Portions Copyright (C) 2009-2019 Greenbone Networks GmbH
 * Portions Copyright (C) 2006 Software in the Public Interest, Inc.
 * Based on work Copyright (C) 1998 - 2006 Tenable Network Security, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @mainpage
 *
 * @section Introduction
 * @verbinclude README.md
 *
 * @section license License Information
 * @verbinclude COPYING
 */

/**
 * @file
 * OpenVAS Scanner main module, runs the scanner.
 */

#include "../misc/plugutils.h"     /* nvticache_free */
#include "../misc/vendorversion.h" /* for vendor_version_set */
#include "attack.h"                /* for attack_network */
#include "pluginlaunch.h"          /* for init_loading_shm */
#include "processes.h"             /* for create_process */
#include "sighand.h"               /* for openvas_signal */

#include <errno.h>  /* for errno() */
#include <fcntl.h>  /* for open() */
#include <gcrypt.h> /* for gcry_control */
#include <glib.h>
#include <grp.h>
#include <gvm/base/logging.h> /* for setup_log_handler, load_log_configuration, free_log_configuration*/
#include <gvm/base/nvti.h>      /* for prefs_get() */
#include <gvm/base/prefs.h>     /* for prefs_get() */
#include <gvm/base/proctitle.h> /* for proctitle_set */
#include <gvm/util/kb.h>        /* for KB_PATH_DEFAULT */
#include <gvm/util/nvticache.h> /* nvticache_free */
#include <gvm/util/uuidutils.h> /* gvm_uuid_make */
#include <netdb.h>              /* for addrinfo */
#include <pwd.h>
#include <signal.h> /* for SIGTERM */
#include <stdio.h>  /* for fflush() */
#include <stdlib.h> /* for atoi() */
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h> /* for waitpid */
#include <unistd.h>   /* for close() */

#ifdef GIT_REV_AVAILABLE
#include "gitrevision.h"
#endif

#if GNUTLS_VERSION_NUMBER < 0x030300
#include "../misc/network.h" /* openvas_SSL_init */
#endif

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "sd   main"

#define PROCTITLE_WAITING "openvas: Waiting for incoming connections"
#define PROCTITLE_LOADING "openvas: Loading Handler"
#define PROCTITLE_RELOADING "openvas: Reloading"
#define PROCTITLE_SERVING "openvas: Serving %s"

/**
 * Globals that should not be touched (used in utils module).
 */
int global_max_hosts = 15;
int global_max_checks = 10;

/**
 * @brief Logging parameters, as passed to setup_log_handlers.
 */
GSList *log_config = NULL;

static volatile int loading_stop_signal = 0;
static volatile int termination_signal = 0;
static char *global_scan_id = NULL;

typedef struct
{
  char *option;
  char *value;
} openvas_option;

/**
 * @brief Default values for scanner options. Must be NULL terminated.
 */
static openvas_option openvas_defaults[] = {
  {"plugins_folder", OPENVAS_NVT_DIR},
  {"include_folders", OPENVAS_NVT_DIR},
  {"max_hosts", "30"},
  {"max_checks", "10"},
  {"log_whole_attack", "no"},
  {"log_plugins_name_at_load", "no"},
  {"optimize_test", "yes"},
  {"network_scan", "no"},
  {"non_simult_ports", "139, 445, 3389, Services/irc"},
  {"plugins_timeout", G_STRINGIFY (NVT_TIMEOUT)},
  {"scanner_plugins_timeout", G_STRINGIFY (SCANNER_NVT_TIMEOUT)},
  {"safe_checks", "yes"},
  {"auto_enable_dependencies", "yes"},
  {"drop_privileges", "no"},
  // Empty options must be "\0", not NULL, to match the behavior of
  // prefs_init.
  {"report_host_details", "yes"},
  {"db_address", KB_PATH_DEFAULT},
  {NULL, NULL}};

gchar *unix_socket_path = NULL;

static void
set_globals_from_preferences (void)
{
  const char *str;

  if ((str = prefs_get ("max_hosts")) != NULL)
    {
      global_max_hosts = atoi (str);
      if (global_max_hosts <= 0)
        global_max_hosts = 15;
    }

  if ((str = prefs_get ("max_checks")) != NULL)
    {
      global_max_checks = atoi (str);
      if (global_max_checks <= 0)
        global_max_checks = 10;
    }
}

static void
reload_openvas (void);

static void
handle_reload_signal (int sig)
{
  (void) sig;
  reload_openvas ();
}

static void
handle_termination_signal (int sig)
{
  termination_signal = sig;
}

/**
 * @brief Initializes main scanner process' signal handlers.
 */
static void
init_signal_handlers ()
{
  openvas_signal (SIGTERM, handle_termination_signal);
  openvas_signal (SIGINT, handle_termination_signal);
  openvas_signal (SIGQUIT, handle_termination_signal);
  openvas_signal (SIGHUP, handle_reload_signal);
  openvas_signal (SIGCHLD, sighand_chld);
}

/* Restarts the scanner by reloading the configuration. */
static void
reload_openvas ()
{
  static gchar *rc_name = NULL;
  const char *config_file;
  int i, ret;

  /* Ignore SIGHUP while reloading. */
  openvas_signal (SIGHUP, SIG_IGN);

  proctitle_set (PROCTITLE_RELOADING);
  /* Setup logging. */
  rc_name = g_build_filename (OPENVAS_SYSCONF_DIR, "openvas_log.conf", NULL);
  if (g_file_test (rc_name, G_FILE_TEST_EXISTS))
    log_config = load_log_configuration (rc_name);
  g_free (rc_name);
  setup_log_handlers (log_config);
  g_message ("Reloading the scanner.\n");

  /* Reload config file. */
  config_file = prefs_get ("config_file");
  for (i = 0; openvas_defaults[i].option != NULL; i++)
    prefs_set (openvas_defaults[i].option, openvas_defaults[i].value);
  prefs_config (config_file);

  /* Reload the plugins */
  ret = plugins_init ();
  set_globals_from_preferences ();

  g_message ("Finished reloading the scanner.");
  openvas_signal (SIGHUP, handle_reload_signal);
  proctitle_set (PROCTITLE_WAITING);
  if (ret)
    exit (1);
}

/**
 * @brief Read the scan preferences from redis
 * @input scan_id Scan ID used as key to find the corresponding KB where
 *                to take the preferences from.
 * @return 0 on success, -1 if the kb is not found or no prefs are found in
 *         the kb.
 */
static int
load_scan_preferences (const char *scan_id)
{
  char key[1024];
  kb_t kb;
  struct kb_item *res = NULL;

  g_debug ("Start loading scan preferences.");
  if (!scan_id)
    return -1;

  snprintf (key, sizeof (key), "internal/%s/scanprefs", scan_id);
  kb = kb_find (prefs_get ("db_address"), key);
  if (!kb)
    return -1;

  res = kb_item_get_all (kb, key);
  if (!res)
    return -1;

  while (res)
    {
      gchar **pref = g_strsplit (res->v_str, "|||", 2);
      if (pref[0])
        prefs_set (pref[0], pref[1] ?: "");
      g_strfreev (pref);
      res = res->next;
    }
  snprintf (key, sizeof (key), "internal/%s", scan_id);
  kb_item_set_str (kb, key, "ready", 0);
  g_debug ("End loading scan preferences.");

  kb_item_free (res);
  return 0;
}

static void
handle_client (struct scan_globals *globals)
{
  kb_t net_kb = NULL;

  /* Load preferences from Redis. Scan started with a scan_id. */
  if (load_scan_preferences (globals->scan_id))
    {
      g_warning ("No preferences found for the scan %s", globals->scan_id);
      exit (0);
    }

  attack_network (globals, &net_kb);
  if (net_kb != NULL)
    {
      kb_delete (net_kb);
      net_kb = NULL;
    }
}

static void
scanner_thread (struct scan_globals *globals)
{
  nvticache_reset ();

  globals->scan_id = g_strdup (global_scan_id);

  handle_client (globals);

  exit (0);
}

/**
 * @brief Get the pid and ppid from /proc to find the running scan pids.
 *        Send SIGUSR2 kill signal to all running scans to stop them.
 */
static void
stop_all_scans (void)
{
  int i, ispid;
  GDir *proc = NULL;
  const gchar *piddir = NULL;
  gchar *pidstatfn = NULL;
  gchar **contents_split = NULL;
  gchar *contents = NULL;
  GError *error = NULL;
  gchar *parentID = NULL;
  gchar *processID = NULL;

  proc = g_dir_open ("/proc", 0, &error);
  if (error != NULL)
    {
      g_message ("Unable to open directory: %s\n", error->message);
      g_error_free (error);
      return;
    }
  while ((piddir = g_dir_read_name (proc)) != NULL)
    {
      ispid = 1;
      for (i = 0; i < (int) strlen (piddir); i++)
        if (!g_ascii_isdigit (piddir[i]))
          {
            ispid = 0;
            break;
          }
      if (!ispid)
        continue;

      pidstatfn = g_strconcat ("/proc/", piddir, "/stat", NULL);
      if (g_file_get_contents (pidstatfn, &contents, NULL, NULL))
        {
          contents_split = g_strsplit (contents, " ", 6);
          parentID = g_strdup (contents_split[3]);
          processID = g_strdup (contents_split[0]);

          g_free (pidstatfn);
          pidstatfn = NULL;
          g_free (contents);
          contents = NULL;
          g_strfreev (contents_split);
          contents_split = NULL;

          if (atoi (parentID) == (int) getpid ())
            {
              g_message ("Stopping running scan with PID: %s", processID);
              kill (atoi (processID), SIGUSR2);
            }
          g_free (parentID);
          parentID = NULL;
          g_free (processID);
          processID = NULL;
        }
      else
        {
          g_free (pidstatfn);
          pidstatfn = NULL;
          continue;
        }
    }

  if (proc)
    g_dir_close (proc);
}

/**
 * @brief Check if Redis Server is up and if the KB exists. If KB does not
 * exist,force a reload and stop all the running scans.
 */
void
check_kb_status ()
{
  int waitredis = 5, waitkb = 5, ret = 0;

  kb_t kb_access_aux;

  while (waitredis != 0)
    {
      ret = kb_new (&kb_access_aux, prefs_get ("db_address"));
      if (ret)
        {
          g_message ("Redis connection lost. Trying to reconnect.");
          waitredis--;
          sleep (5);
          continue;
        }
      else
        {
          kb_delete (kb_access_aux);
          break;
        }
    }

  if (waitredis == 0)
    {
      g_message ("Critical Redis connection error.");
      exit (1);
    }
  while (waitkb != 0)
    {
      kb_access_aux = kb_find (prefs_get ("db_address"), NVTICACHE_STR);
      if (!kb_access_aux)
        {
          g_message ("Redis kb not found. Trying again in 2 seconds.");
          waitkb--;
          sleep (2);
          continue;
        }
      else
        {
          kb_lnk_reset (kb_access_aux);
          g_free (kb_access_aux);
          break;
        }
    }

  if (waitredis != 5 || waitkb == 0)
    {
      g_message ("Redis connection error. Stopping all the running scans.");
      stop_all_scans ();
      reload_openvas ();
    }
}

/**
 * @brief Initialize everything.
 *
 * @param config_file Path to config file for initialization
 */
static int
init_openvas (const char *config_file)
{
  static gchar *rc_name = NULL;
  int i;

  for (i = 0; openvas_defaults[i].option != NULL; i++)
    prefs_set (openvas_defaults[i].option, openvas_defaults[i].value);
  prefs_config (config_file);

  /* Setup logging. */
  rc_name = g_build_filename (OPENVAS_SYSCONF_DIR, "openvas_log.conf", NULL);
  if (g_file_test (rc_name, G_FILE_TEST_EXISTS))
    log_config = load_log_configuration (rc_name);
  g_free (rc_name);
  setup_log_handlers (log_config);
  set_globals_from_preferences ();

  return 0;
}

static int
flush_all_kbs ()
{
  kb_t kb;
  int rc;

  rc = kb_new (&kb, prefs_get ("db_address"));
  if (rc)
    return rc;

  rc = kb_flush (kb, NVTICACHE_STR);
  return rc;
}

static void
gcrypt_init ()
{
  if (gcry_control (GCRYCTL_ANY_INITIALIZATION_P))
    return;
  gcry_check_version (NULL);
  gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
  gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);
  gcry_control (GCRYCTL_RESUME_SECMEM_WARN);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED);
}

void
start_single_task_scan ()
{
  struct scan_globals *globals;
  int ret = 0;

#if GNUTLS_VERSION_NUMBER < 0x030300
  if (openvas_SSL_init () < 0)
    g_message ("Could not initialize openvas SSL!");
#endif

#ifdef OPENVAS_GIT_REVISION
  g_message ("openvas %s (GIT revision %s) started", OPENVAS_VERSION,
             OPENVAS_GIT_REVISION);
#else
  g_message ("openvas %s started", OPENVAS_VERSION);
#endif

  openvas_signal (SIGHUP, SIG_IGN);
  ret = plugins_init ();
  if (ret)
    exit (0);
  init_signal_handlers ();

  globals = g_malloc0 (sizeof (struct scan_globals));

  scanner_thread (globals);
  exit (0);
}

/**
 * @brief openvas.
 * @param argc Argument count.
 * @param argv Argument vector.
 */
int
main (int argc, char *argv[])
{
  int ret;

  proctitle_init (argc, argv);
  gcrypt_init ();

  static gboolean display_version = FALSE;
  static gchar *config_file = NULL;
  static gchar *vendor_version_string = NULL;
  static gchar *scan_id = NULL;
  static gboolean print_specs = FALSE;
  static gboolean print_sysconfdir = FALSE;
  static gboolean update_vt_info = FALSE;
  GError *error = NULL;
  GOptionContext *option_context;
  static GOptionEntry entries[] = {
    {"version", 'V', 0, G_OPTION_ARG_NONE, &display_version,
     "Display version information", NULL},
    {"config-file", 'c', 0, G_OPTION_ARG_FILENAME, &config_file,
     "Configuration file", "<filename>"},
    {"vendor-version", '\0', 0, G_OPTION_ARG_STRING, &vendor_version_string,
     "Use <string> as vendor version.", "<string>"},
    {"cfg-specs", 's', 0, G_OPTION_ARG_NONE, &print_specs,
     "Print configuration settings", NULL},
    {"sysconfdir", 'y', 0, G_OPTION_ARG_NONE, &print_sysconfdir,
     "Print system configuration directory (set at compile time)", NULL},
    {"update-vt-info", 'u', 0, G_OPTION_ARG_NONE, &update_vt_info,
     "Updates VT info into redis store from VT files", NULL},
    {"scan-start", '\0', 0, G_OPTION_ARG_STRING, &scan_id,
     "ID of scan to start. ID and related data must be stored into redis "
     "before.",
     "<string>"},
    {NULL, 0, 0, 0, NULL, NULL, NULL}};

  option_context =
    g_option_context_new ("- Open Vulnerability Assessment System");
  g_option_context_add_main_entries (option_context, entries, NULL);
  if (!g_option_context_parse (option_context, &argc, &argv, &error))
    {
      g_print ("%s\n\n", error->message);
      exit (0);
    }
  g_option_context_free (option_context);

  /* --sysconfdir */
  if (print_sysconfdir)
    {
      g_print ("%s\n", SYSCONFDIR);
      exit (0);
    }

  /* --version */
  if (display_version)
    {
      printf ("OpenVAS Scanner %s\n", OPENVAS_VERSION);
#ifdef OPENVAS_GIT_REVISION
      printf ("GIT revision %s\n", OPENVAS_GIT_REVISION);
#endif
      printf ("Most new code since 2005: (C) 2019 Greenbone Networks GmbH\n");
      printf (
        "Nessus origin: (C) 2004 Renaud Deraison <deraison@nessus.org>\n");
      printf ("License GPLv2: GNU GPL version 2\n");
      printf (
        "This is free software: you are free to change and redistribute it.\n"
        "There is NO WARRANTY, to the extent permitted by law.\n\n");
      exit (0);
    }

  /* Switch to UTC so that OTP times are always in UTC. */
  if (setenv ("TZ", "utc 0", 1) == -1)
    {
      g_print ("%s\n\n", strerror (errno));
      exit (0);
    }
  tzset ();

  unix_socket_path = g_build_filename (OPENVAS_RUN_DIR, "openvas.sock", NULL);

  if (vendor_version_string)
    vendor_version_set (vendor_version_string);

  if (!config_file)
    config_file = OPENVAS_CONF;
  if (update_vt_info)
    {
      if (init_openvas (config_file))
        return 1;
      if (plugins_init ())
        return 1;
      return 0;
    }

  if (init_openvas (config_file))
    return 1;

  if (scan_id)
    {
      global_scan_id = g_strdup (scan_id);
      start_single_task_scan ();
      exit (0);
    }

  /* special treatment */
  if (print_specs)
    {
      prefs_dump ();
      exit (0);
    }
  if (flush_all_kbs ())
    exit (1);

#if GNUTLS_VERSION_NUMBER < 0x030300
  if (openvas_SSL_init () < 0)
    g_message ("Could not initialize openvas SSL!");
#endif

  /* Ignore SIGHUP while reloading. */
  openvas_signal (SIGHUP, SIG_IGN);

  ret = plugins_init ();
  if (ret)
    return 1;

  exit (0);
}