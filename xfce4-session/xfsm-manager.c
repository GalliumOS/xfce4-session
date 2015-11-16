/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2003-2006 Benedikt Meurer <benny@xfce.org>
 * Copyright (c) 2008 Brian Tarricone <bjt23@cornell.edu>
 * Copyright (c) 2010 Jannis Pohlmann <jannis@xfce.org>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/* unfortunately, glibc doesn't have a wrapper for the ioprio_set ()
 * syscall, so we have to do it the hard way.  also, it seems some
 * systems don't have <linux/ioprio.h>, so i'll copy the defines here.
 */
#ifdef HAVE_ASM_UNISTD_H
#  include <asm/unistd.h>
#  include <sys/syscall.h>
#  ifdef __NR_ioprio_set
#    ifdef HAVE_WORKING_LINUX_IOPRIO_H
#      include <linux/ioprio.h>
#    else  /* if !HAVE_WORKING_LINUX_IOPRIO_H */
#      define IOPRIO_CLASS_SHIFT              (13)
#      define IOPRIO_PRIO_MASK                ((1UL << IOPRIO_CLASS_SHIFT) - 1)
#      define IOPRIO_PRIO_VALUE(class, data)  (((class) << IOPRIO_CLASS_SHIFT) | data)
#      define IOPRIO_WHO_PROCESS              (1)
#      define IOPRIO_CLASS_IDLE               (3)
#    endif  /* !HAVE_WORKING_LINUX_IOPRIO_H */
#  endif  /* __NR_ioprio_set */
#endif  /* HAVE_ASM_UNISTD_H */

#include <dbus/dbus-glib-lowlevel.h>

#include <X11/ICE/ICElib.h>
#include <X11/SM/SMlib.h>

#include <gdk-pixbuf/gdk-pixdata.h>
#include <gtk/gtk.h>

#include <libwnck/libwnck.h>

#include <libxfce4ui/libxfce4ui.h>

#include <libxfsm/xfsm-splash-engine.h>
#include <libxfsm/xfsm-util.h>

#include <xfce4-session/xfsm-manager.h>
#include <xfce4-session/xfsm-chooser-icon.h>
#include <xfce4-session/xfsm-chooser.h>
#include <xfce4-session/xfsm-global.h>
#include <xfce4-session/xfsm-legacy.h>
#include <xfce4-session/xfsm-startup.h>
#include <xfce4-session/xfsm-marshal.h>
#include <xfce4-session/xfsm-error.h>
#include <xfce4-session/xfsm-logout-dialog.h>


#define DEFAULT_SESSION_NAME "Default"


struct _XfsmManager
{
  GObject parent;

  XfsmManagerState state;

  XfsmShutdownType  shutdown_type;
  XfsmShutdown     *shutdown_helper;
  gboolean          save_session;

  gboolean         session_chooser;
  gchar           *session_name;
  gchar           *session_file;
  gchar           *checkpoint_session_name;

  gboolean         start_at;

  gboolean         compat_gnome;
  gboolean         compat_kde;

  GQueue          *starting_properties;
  GQueue          *pending_properties;
  GQueue          *restart_properties;
  GQueue          *running_clients;

  gboolean         failsafe_mode;
  GQueue          *failsafe_clients;

  guint            die_timeout_id;

  DBusGConnection *session_bus;
};

typedef struct _XfsmManagerClass
{
  GObjectClass parent;

  /*< signals >*/
  void (*state_changed) (XfsmManager     *manager,
                         XfsmManagerState old_state,
                         XfsmManagerState new_state);

  void (*client_registered) (XfsmManager *manager,
                             const gchar *client_object_path);

  void (*shutdown_cancelled) (XfsmManager *manager);
} XfsmManagerClass;

typedef struct
{
  XfsmManager *manager;
  XfsmClient  *client;
  guint        timeout_id;
} XfsmSaveTimeoutData;

typedef struct
{
  XfsmManager     *manager;
  XfsmShutdownType type;
  gboolean         allow_save;
} ShutdownIdleData;

enum
{
  SIG_STATE_CHANGED = 0,
  SIG_CLIENT_REGISTERED,
  SIG_SHUTDOWN_CANCELLED,
  N_SIGS,
};


static void       xfsm_manager_finalize (GObject *obj);

static gboolean   xfsm_manager_startup (XfsmManager *manager);
static void       xfsm_manager_start_client_save_timeout (XfsmManager *manager,
                                                          XfsmClient  *client);
static void       xfsm_manager_cancel_client_save_timeout (XfsmManager *manager,
                                                           XfsmClient  *client);
static gboolean   xfsm_manager_save_timeout (gpointer user_data);
static void       xfsm_manager_load_settings (XfsmManager   *manager,
                                              XfconfChannel *channel);
static gboolean   xfsm_manager_load_session (XfsmManager *manager);
static void       xfsm_manager_dbus_class_init (XfsmManagerClass *klass);
static void       xfsm_manager_dbus_init (XfsmManager *manager);
static void       xfsm_manager_dbus_cleanup (XfsmManager *manager);


static guint signals[N_SIGS] = { 0, };


G_DEFINE_TYPE(XfsmManager, xfsm_manager, G_TYPE_OBJECT)


static void
xfsm_manager_class_init (XfsmManagerClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *)klass;

  gobject_class->finalize = xfsm_manager_finalize;

  signals[SIG_STATE_CHANGED] = g_signal_new ("state-changed",
                                             XFSM_TYPE_MANAGER,
                                             G_SIGNAL_RUN_LAST,
                                             G_STRUCT_OFFSET (XfsmManagerClass,
                                                              state_changed),
                                             NULL, NULL,
                                             xfsm_marshal_VOID__UINT_UINT,
                                             G_TYPE_NONE, 2,
                                             G_TYPE_UINT, G_TYPE_UINT);

  signals[SIG_CLIENT_REGISTERED] = g_signal_new ("client-registered",
                                                 XFSM_TYPE_MANAGER,
                                                 G_SIGNAL_RUN_LAST,
                                                 G_STRUCT_OFFSET (XfsmManagerClass,
                                                                  client_registered),
                                                 NULL, NULL,
                                                 g_cclosure_marshal_VOID__STRING,
                                                 G_TYPE_NONE, 1,
                                                 G_TYPE_STRING);

  signals[SIG_SHUTDOWN_CANCELLED] = g_signal_new ("shutdown-cancelled",
                                                  XFSM_TYPE_MANAGER,
                                                  G_SIGNAL_RUN_LAST,
                                                  G_STRUCT_OFFSET (XfsmManagerClass,
                                                                   shutdown_cancelled),
                                                  NULL, NULL,
                                                  g_cclosure_marshal_VOID__VOID,
                                                  G_TYPE_NONE, 0);

  xfsm_manager_dbus_class_init (klass);
}


static void
xfsm_manager_init (XfsmManager *manager)
{
  manager->state = XFSM_MANAGER_STARTUP;
  manager->session_chooser = FALSE;
  manager->failsafe_mode = TRUE;
  manager->shutdown_type = XFSM_SHUTDOWN_LOGOUT;
  manager->shutdown_helper = xfsm_shutdown_get ();
  manager->save_session = TRUE;

  manager->pending_properties = g_queue_new ();
  manager->starting_properties = g_queue_new ();
  manager->restart_properties = g_queue_new ();
  manager->running_clients = g_queue_new ();
  manager->failsafe_clients = g_queue_new ();
}

static void
xfsm_manager_finalize (GObject *obj)
{
  XfsmManager *manager = XFSM_MANAGER(obj);

  xfsm_manager_dbus_cleanup (manager);

  if (manager->die_timeout_id != 0)
    g_source_remove (manager->die_timeout_id);

  g_object_unref (manager->shutdown_helper);

  g_queue_foreach (manager->pending_properties, (GFunc) xfsm_properties_free, NULL);
  g_queue_free (manager->pending_properties);

  g_queue_foreach (manager->starting_properties, (GFunc) xfsm_properties_free, NULL);
  g_queue_free (manager->starting_properties);

  g_queue_foreach (manager->restart_properties, (GFunc) xfsm_properties_free, NULL);
  g_queue_free (manager->restart_properties);

  g_queue_foreach (manager->running_clients, (GFunc) g_object_unref, NULL);
  g_queue_free (manager->running_clients);

  g_queue_foreach (manager->failsafe_clients, (GFunc) xfsm_failsafe_client_free, NULL);
  g_queue_free (manager->failsafe_clients);

  g_free (manager->session_name);
  g_free (manager->session_file);
  g_free (manager->checkpoint_session_name);

  G_OBJECT_CLASS (xfsm_manager_parent_class)->finalize (obj);
}


#ifdef G_CAN_INLINE
static inline void
#else
static void
#endif
xfsm_manager_set_state (XfsmManager     *manager,
                        XfsmManagerState state)
{
  XfsmManagerState old_state;

  /* idea here is to use this to set state always so we don't forget
   * to emit the signal */

  if (state == manager->state)
    return;

  old_state = manager->state;
  manager->state = state;

  xfsm_verbose ("\nstate is now %s\n",
                state == XFSM_MANAGER_STARTUP ? "XFSM_MANAGER_STARTUP" :
                state == XFSM_MANAGER_IDLE ?  "XFSM_MANAGER_IDLE" :
                state == XFSM_MANAGER_CHECKPOINT ? "XFSM_MANAGER_CHECKPOINT" :
                state == XFSM_MANAGER_SHUTDOWN ? "XFSM_MANAGER_SHUTDOWN" :
                state == XFSM_MANAGER_SHUTDOWNPHASE2 ? "XFSM_MANAGER_SHUTDOWNPHASE2" :
                "unknown");

  g_signal_emit (manager, signals[SIG_STATE_CHANGED], 0, old_state, state);
}


XfsmManager *
xfsm_manager_new (void)
{
  XfsmManager *manager = g_object_new (XFSM_TYPE_MANAGER, NULL);

  xfsm_manager_dbus_init (manager);

  return manager;
}


static gboolean
xfsm_manager_startup (XfsmManager *manager)
{
  xfsm_startup_foreign (manager);
  g_queue_sort (manager->pending_properties, (GCompareDataFunc) xfsm_properties_compare, NULL);
  xfsm_startup_begin (manager);
  return FALSE;
}


static void
xfsm_manager_restore_active_workspace (XfsmManager *manager,
                                       XfceRc      *rc)
{
  WnckWorkspace  *workspace;
  GdkDisplay     *display;
  WnckScreen     *screen;
  gchar           buffer[1024];
  gint            n, m;

  display = gdk_display_get_default ();
  for (n = 0; n < gdk_display_get_n_screens (display); ++n)
    {
      g_snprintf (buffer, 1024, "Screen%d_ActiveWorkspace", n);
      xfsm_verbose ("Attempting to restore %s\n", buffer);
      if (!xfce_rc_has_entry (rc, buffer))
        {
          xfsm_verbose ("no entry found\n");
          continue;
        }

      m = xfce_rc_read_int_entry (rc, buffer, 0);

      screen = wnck_screen_get (n);
      wnck_screen_force_update (screen);

      if (wnck_screen_get_workspace_count (screen) > m)
        {
          workspace = wnck_screen_get_workspace (screen, m);
          wnck_workspace_activate (workspace, GDK_CURRENT_TIME);
        }
    }
}


gboolean
xfsm_manager_handle_failed_properties (XfsmManager    *manager,
                                       XfsmProperties *properties)
{
  gint restart_style_hint;
  GError *error = NULL;

  /* Handle apps that failed to start, or died randomly, here */

  xfsm_properties_set_default_child_watch (properties);

  if (properties->restart_attempts_reset_id > 0)
    {
      g_source_remove (properties->restart_attempts_reset_id);
      properties->restart_attempts_reset_id = 0;
    }

  restart_style_hint = xfsm_properties_get_uchar (properties,
                                                  SmRestartStyleHint,
                                                  SmRestartIfRunning);

  if (restart_style_hint == SmRestartAnyway)
    {
      xfsm_verbose ("Client id %s died or failed to start, restarting anyway\n", properties->client_id);
      g_queue_push_tail (manager->restart_properties, properties);
    }
  else if (restart_style_hint == SmRestartImmediately)
    {
      if (++properties->restart_attempts > MAX_RESTART_ATTEMPTS)
        {
          xfsm_verbose ("Client Id = %s, reached maximum attempts [Restart attempts = %d]\n"
                        "   Will be re-scheduled for run on next startup\n",
                        properties->client_id, properties->restart_attempts);

          g_queue_push_tail (manager->restart_properties, properties);
        }
      else
        {
          xfsm_verbose ("Client Id = %s disconnected, restarting\n",
                        properties->client_id);

          if (G_UNLIKELY (!xfsm_startup_start_properties (properties, manager)))
            {
              /* this failure has nothing to do with the app itself, so
               * just add it to restart props */
              g_queue_push_tail (manager->restart_properties, properties);
            }
          else
            {
              /* put it back in the starting list */
              g_queue_push_tail (manager->starting_properties, properties);
            }
        }
    }
  else
    {
      gchar **discard_command;

      /* We get here if a SmRestartNever or SmRestartIfRunning client
       * has exited.  SmRestartNever clients shouldn't have discard
       * commands, but it can't hurt to run it if it has one for some
       * reason, and might clean up garbage we don't want. */
      xfsm_verbose ("Client Id %s exited, removing from session.\n",
                    properties->client_id);

      discard_command = xfsm_properties_get_strv (properties, SmDiscardCommand);
      if (discard_command != NULL && g_strv_length (discard_command) > 0)
        {
          /* Run the SmDiscardCommand after the client exited in any state,
           * but only if we don't expect the client to be restarted,
           * whether immediately or in the next session.
           *
           * NB: This used to also have the condition that the manager is
           * in the IDLE state, but this was removed because I can't see
           * why you'd treat a client that fails during startup any
           * differently, and this fixes a potential properties leak.
           *
           * Unfortunately the spec isn't clear about the usage of the
           * discard command. Have to check ksmserver/gnome-session, and
           * come up with consistent behaviour.
           *
           * But for now, this work-around fixes the problem of the
           * ever-growing number of xfwm4 session files when restarting
           * xfwm4 within a session.
           */
          xfsm_verbose ("Client Id = %s: running discard command %s:%d.\n\n",
                        properties->client_id, *discard_command,
                        g_strv_length (discard_command));

          if (!g_spawn_sync (xfsm_properties_get_string (properties, SmCurrentDirectory),
                             discard_command,
                             xfsm_properties_get_strv (properties, SmEnvironment),
                             G_SPAWN_SEARCH_PATH,
                             NULL, NULL,
                             NULL, NULL,
                             NULL, &error))
            {
              g_warning ("Failed to running discard command \"%s\": %s",
                         *discard_command, error->message);
              g_error_free (error);
            }
        }

      return FALSE;
    }

  return TRUE;
}


static gboolean
xfsm_manager_choose_session (XfsmManager *manager,
                             XfceRc      *rc)
{
  XfsmSessionInfo *session;
  GdkPixbuf       *preview_default = NULL;
  gboolean         load = FALSE;
  GList           *sessions = NULL;
  GList           *lp;
  gchar          **groups;
  gchar           *name;
  gint             result;
  gint             n;

  groups = xfce_rc_get_groups (rc);
  for (n = 0; groups[n] != NULL; ++n)
    {
      if (strncmp (groups[n], "Session: ", 9) == 0)
        {
          xfce_rc_set_group (rc, groups[n]);
          session = g_new0 (XfsmSessionInfo, 1);
          session->name = groups[n] + 9;
          session->atime = xfce_rc_read_int_entry (rc, "LastAccess", 0);
          session->preview = xfsm_load_session_preview (session->name);

          if (session->preview == NULL)
            {
              if (G_UNLIKELY (preview_default == NULL))
                {
                  preview_default = gdk_pixbuf_new_from_inline (-1, xfsm_chooser_icon_data,
                                                                FALSE, NULL);
                }

              session->preview = GDK_PIXBUF (g_object_ref (preview_default));
            }

          sessions = g_list_append (sessions, session);
        }
    }

  if (preview_default != NULL)
    g_object_unref (preview_default);

  if (sessions != NULL)
    {
      result = xfsm_splash_screen_choose (splash_screen, sessions,
                                          manager->session_name, &name);

      if (result == XFSM_CHOOSE_LOGOUT)
        {
          xfce_rc_close (rc);
          exit (EXIT_SUCCESS);
        }
      else if (result == XFSM_CHOOSE_LOAD)
        {
          load = TRUE;
        }

      if (manager->session_name != NULL)
        g_free (manager->session_name);
      manager->session_name = name;

      for (lp = sessions; lp != NULL; lp = lp->next)
        {
          session = (XfsmSessionInfo *) lp->data;
          g_object_unref (session->preview);
          g_free (session);
        }

      g_list_free (sessions);
    }

  g_strfreev (groups);

  return load;
}


static gboolean
xfsm_manager_load_session (XfsmManager *manager)
{
  XfsmProperties *properties;
  gchar           buffer[1024];
  XfceRc         *rc;
  gint            count;

  if (!g_file_test (manager->session_file, G_FILE_TEST_IS_REGULAR))
    {
      g_warning ("xfsm_manager_load_session: Something wrong with %s, Does it exist? Permissions issue?", manager->session_file);
      return FALSE;
    }

  rc = xfce_rc_simple_open (manager->session_file, FALSE);
  if (G_UNLIKELY (rc == NULL))
  {
    g_warning ("xfsm_manager_load_session: unable to open %s", manager->session_file);
    return FALSE;
  }

  if (manager->session_chooser && !xfsm_manager_choose_session (manager, rc))
    {
      g_warning ("xfsm_manager_load_session: failed to choose session");
      xfce_rc_close (rc);
      return FALSE;
    }

  g_snprintf (buffer, 1024, "Session: %s", manager->session_name);
  xfsm_verbose ("loading %s\n", buffer);

  xfce_rc_set_group (rc, buffer);
  count = xfce_rc_read_int_entry (rc, "Count", 0);
  if (G_UNLIKELY (count <= 0))
    {
      xfce_rc_close (rc);
      return FALSE;
    }

  while (count-- > 0)
    {
      g_snprintf (buffer, 1024, "Client%d_", count);
      properties = xfsm_properties_load (rc, buffer);
      if (G_UNLIKELY (properties == NULL))
        {
          xfsm_verbose ("%s has no properties. Skipping\n", buffer);
          continue;
        }
      if (xfsm_properties_check (properties))
        g_queue_push_tail (manager->pending_properties, properties);
      else
        {
          xfsm_verbose ("%s has invalid properties. Skipping\n", buffer);
          xfsm_properties_free (properties);
        }
    }

  xfsm_verbose ("Finished loading clients from rc file\n");

  /* load legacy applications */
  xfsm_legacy_load_session (rc);

  xfce_rc_close (rc);

  return g_queue_peek_head (manager->pending_properties) != NULL;
}


static gboolean
xfsm_manager_load_failsafe (XfsmManager   *manager,
                            XfconfChannel *channel,
                            gchar        **error)
{
  FailsafeClient *fclient;
  gchar          *failsafe_name;
  GdkDisplay     *display;
  gchar           propbuf[4096];
  gchar         **command;
  gchar           command_entry[256];
  gchar           screen_entry[256];
  gint            count;
  gint            i;
  gint            n_screen;

  failsafe_name = xfconf_channel_get_string (channel, "/general/FailsafeSessionName", NULL);
  if (G_UNLIKELY (!failsafe_name))
    {
      if (error)
        *error = g_strdup_printf (_("Unable to determine failsafe session name.  Possible causes: xfconfd isn't running (D-Bus setup problem); environment variable $XDG_CONFIG_DIRS is set incorrectly (must include \"%s\"), or xfce4-session is installed incorrectly."),
                                  SYSCONFDIR);
      return FALSE;
    }

  g_snprintf (propbuf, sizeof (propbuf), "/sessions/%s/IsFailsafe",
              failsafe_name);
  if (!xfconf_channel_get_bool (channel, propbuf, FALSE))
    {
      if (error)
        {
          *error = g_strdup_printf (_("The specified failsafe session (\"%s\") is not marked as a failsafe session."),
                                    failsafe_name);
        }
      g_free (failsafe_name);
      return FALSE;
    }

  display = gdk_display_get_default ();

  g_snprintf (propbuf, sizeof (propbuf), "/sessions/%s/Count", failsafe_name);
  count = xfconf_channel_get_int (channel, propbuf, 0);

  for (i = 0; i < count; ++i)
    {
      g_snprintf (command_entry, sizeof (command_entry),
                  "/sessions/%s/Client%d_Command", failsafe_name, i);
      command = xfconf_channel_get_string_list (channel, command_entry);
      if (G_UNLIKELY (command == NULL))
        continue;

      g_snprintf (screen_entry, sizeof (screen_entry),
                  "/sessions/%s/Client%d_PerScreen", failsafe_name, i);
      if (xfconf_channel_get_bool (channel, screen_entry, FALSE))
        {
          for (n_screen = 0; n_screen < gdk_display_get_n_screens (display); ++n_screen)
            {
              fclient = g_new0 (FailsafeClient, 1);
              if (n_screen == 0)
                fclient->command = command;
              else
                fclient->command = g_strdupv (command);
              fclient->screen = gdk_display_get_screen (display, n_screen);
              g_queue_push_tail (manager->failsafe_clients, fclient);
            }
        }
      else
        {
          fclient = g_new0 (FailsafeClient, 1);
          fclient->command = command;
          fclient->screen = gdk_screen_get_default ();
          g_queue_push_tail (manager->failsafe_clients, fclient);
        }
    }

  if (g_queue_peek_head (manager->failsafe_clients) == NULL)
    {
      if (error)
        *error = g_strdup (_("The list of applications in the failsafe session is empty."));
      return FALSE;
    }

  return TRUE;
}


static void
xfsm_manager_load_settings (XfsmManager   *manager,
                            XfconfChannel *channel)
{
  gboolean session_loaded = FALSE;

  manager->session_name = xfconf_channel_get_string (channel,
                                                     "/general/SessionName",
                                                     DEFAULT_SESSION_NAME);
  if (G_UNLIKELY (manager->session_name[0] == '\0'))
    {
      g_free (manager->session_name);
      manager->session_name = g_strdup (DEFAULT_SESSION_NAME);
    }

  manager->session_chooser = xfconf_channel_get_bool (channel, "/chooser/AlwaysDisplay", FALSE);

  session_loaded = xfsm_manager_load_session (manager);

  if (session_loaded)
    {
      xfsm_verbose ("Session \"%s\" loaded successfully.\n\n", manager->session_name);
      manager->failsafe_mode = FALSE;
    }
  else
    {
      gchar *errorstr = NULL;

      if (!xfsm_manager_load_failsafe (manager, channel, &errorstr))
        {
          if (G_LIKELY (splash_screen != NULL))
            {
              xfsm_splash_screen_free (splash_screen);
              splash_screen = NULL;
            }

          /* FIXME: migrate this into the splash screen somehow so the
           * window doesn't look ugly (right now no WM is running, so it
           * won't have window decorations). */
          xfce_message_dialog (NULL, _("Session Manager Error"),
                               GTK_STOCK_DIALOG_ERROR,
                               _("Unable to load a failsafe session"),
                               errorstr,
                               GTK_STOCK_QUIT, GTK_RESPONSE_ACCEPT, NULL);
          g_free (errorstr);
          exit (EXIT_FAILURE);
        }
      manager->failsafe_mode = TRUE;
    }
}


void
xfsm_manager_load (XfsmManager   *manager,
                   XfconfChannel *channel)
{
  gchar *display_name;
  gchar *resource_name;
#ifdef HAVE_OS_CYGWIN
  gchar *s;
#endif

  manager->compat_gnome = xfconf_channel_get_bool (channel, "/compat/LaunchGNOME", FALSE);
  manager->compat_kde = xfconf_channel_get_bool (channel, "/compat/LaunchKDE", FALSE);
  manager->start_at = xfconf_channel_get_bool (channel, "/general/StartAssistiveTechnologies", FALSE);

  display_name  = xfsm_gdk_display_get_fullname (gdk_display_get_default ());

#ifdef HAVE_OS_CYGWIN
  /* rename a colon (:) to a hash (#) under cygwin. windows doesn't like
   * filenames with a colon... */
  for (s = display_name; *s != '\0'; ++s)
    if (*s == ':')
      *s = '#';
#endif

  resource_name = g_strconcat ("sessions/xfce4-session-", display_name, NULL);
  manager->session_file  = xfce_resource_save_location (XFCE_RESOURCE_CACHE, resource_name, TRUE);
  g_free (resource_name);
  g_free (display_name);

  xfsm_manager_load_settings (manager, channel);
}


gboolean
xfsm_manager_restart (XfsmManager *manager)
{
  GdkPixbuf *preview;
  unsigned   steps;

  g_assert (manager->session_name != NULL);

  /* setup legacy application handling */
  xfsm_legacy_init ();

  /* tell splash screen that the session is starting now */
  preview = xfsm_load_session_preview (manager->session_name);
  if (preview == NULL)
    preview = gdk_pixbuf_new_from_inline (-1, xfsm_chooser_icon_data, FALSE, NULL);
  steps = g_queue_get_length (manager->failsafe_mode ? manager->failsafe_clients : manager->pending_properties);
  xfsm_splash_screen_start (splash_screen, manager->session_name, preview, steps);
  g_object_unref (preview);

  g_idle_add ((GSourceFunc) xfsm_manager_startup, manager);

  return TRUE;
}


void
xfsm_manager_signal_startup_done (XfsmManager *manager)
{
  gchar buffer[1024];
  XfceRc *rc;

  xfsm_verbose ("Manager finished startup, entering IDLE mode now\n\n");
  xfsm_manager_set_state (manager, XFSM_MANAGER_IDLE);

  if (!manager->failsafe_mode)
    {
      /* restore active workspace, this has to be done after the
       * window manager is up, so we do it last.
       */
      g_snprintf (buffer, 1024, "Session: %s", manager->session_name);
      rc = xfce_rc_simple_open (manager->session_file, TRUE);
      xfce_rc_set_group (rc, buffer);
      xfsm_manager_restore_active_workspace (manager, rc);
      xfce_rc_close (rc);

      /* start legacy applications now */
      xfsm_legacy_startup ();
    }
}


XfsmClient*
xfsm_manager_new_client (XfsmManager *manager,
                         SmsConn      sms_conn,
                         gchar      **error)
{
  XfsmClient *client = NULL;

  if (G_UNLIKELY (manager->state != XFSM_MANAGER_IDLE)
      && G_UNLIKELY (manager->state != XFSM_MANAGER_STARTUP))
    {
      if (error != NULL)
        *error = "We don't accept clients while in CheckPoint/Shutdown state!";
      return NULL;
    }

  client = xfsm_client_new (manager, sms_conn);
  return client;
}


static gboolean
xfsm_manager_reset_restart_attempts (gpointer data)
{
  XfsmProperties *properties = data;

  properties->restart_attempts = 0;
  properties->restart_attempts_reset_id = 0;

  return FALSE;
}


gboolean
xfsm_manager_register_client (XfsmManager *manager,
                              XfsmClient  *client,
                              const gchar *previous_id)
{
  XfsmProperties *properties = NULL;
  gchar          *client_id;
  GList          *lp;
  SmsConn         sms_conn;

  sms_conn = xfsm_client_get_sms_connection (client);

  if (previous_id != NULL)
    {
      lp = g_queue_find_custom (manager->starting_properties,
                                previous_id,
                                (GCompareFunc) xfsm_properties_compare_id);
      if (lp != NULL)
        {
          properties = XFSM_PROPERTIES (lp->data);
          g_queue_delete_link (manager->starting_properties, lp);
        }
      else
        {
          lp = g_queue_find_custom (manager->pending_properties,
                                    previous_id,
                                    (GCompareFunc) xfsm_properties_compare_id);
          if (lp != NULL)
            {
              properties = XFSM_PROPERTIES (lp->data);
              g_queue_delete_link (manager->pending_properties, lp);
            }
        }

      /* If previous_id is invalid, the SM will send a BadValue error message
       * to the client and reverts to register state waiting for another
       * RegisterClient message.
       */
      if (properties == NULL)
        {
          xfsm_verbose ("Client Id = %s registering, failed to find matching "
                        "properties\n", previous_id);
          return FALSE;
        }

      /* cancel startup timer */
      if (properties->startup_timeout_id > 0)
        {
          g_source_remove (properties->startup_timeout_id);
          properties->startup_timeout_id = 0;
        }

      /* cancel the old child watch, and replace it with one that
       * doesn't really do anything but reap the child */
      xfsm_properties_set_default_child_watch (properties);

      xfsm_client_set_initial_properties (client, properties);

      /* if we've been restarted, we'll want to reset the restart
       * attempts counter if the client stays alive for a while */
      if (properties->restart_attempts > 0 && properties->restart_attempts_reset_id == 0)
        {
          properties->restart_attempts_reset_id = g_timeout_add (RESTART_RESET_TIMEOUT,
                                                                 xfsm_manager_reset_restart_attempts,
                                                                 properties);
        }
    }
  else
    {
      client_id = xfsm_generate_client_id (sms_conn);
      properties = xfsm_properties_new (client_id, SmsClientHostName (sms_conn));
      xfsm_client_set_initial_properties (client, properties);
      g_free (client_id);
    }

  g_queue_push_tail (manager->running_clients, client);

  SmsRegisterClientReply (sms_conn, (char *) xfsm_client_get_id (client));

  g_signal_emit (manager, signals[SIG_CLIENT_REGISTERED], 0,
                 xfsm_client_get_object_path (client));

  if (previous_id == NULL)
    {
      SmsSaveYourself (sms_conn, SmSaveLocal, False, SmInteractStyleNone, False);
      xfsm_client_set_state (client, XFSM_CLIENT_SAVINGLOCAL);
      xfsm_manager_start_client_save_timeout (manager, client);
    }

  if (previous_id != NULL && manager->state == XFSM_MANAGER_STARTUP)
    {
      /* Only continue the startup if the previous_id matched one of
       * the starting_properties. If there was no match above,
       * previous_id will be NULL here.  We don't need to continue when
       * in failsafe mode because in that case the failsafe session is
       * started all at once.
       */
      if (g_queue_peek_head (manager->starting_properties) == NULL)
        xfsm_startup_session_continue (manager);
    }

  return TRUE;
}


void
xfsm_manager_start_interact (XfsmManager *manager,
                             XfsmClient  *client)
{
  /* notify client of interact */
  SmsInteract (xfsm_client_get_sms_connection (client));
  xfsm_client_set_state (client, XFSM_CLIENT_INTERACTING);

  /* stop save yourself timeout */
  xfsm_manager_cancel_client_save_timeout (manager, client);
}


void
xfsm_manager_interact (XfsmManager *manager,
                       XfsmClient  *client,
                       int          dialog_type)
{
  GList *lp;

  if (G_UNLIKELY (xfsm_client_get_state (client) != XFSM_CLIENT_SAVING))
    {
      xfsm_verbose ("Client Id = %s, requested INTERACT, but client is not in SAVING mode\n"
                    "   Client will be disconnected now.\n\n",
                    xfsm_client_get_id (client));
      xfsm_manager_close_connection (manager, client, TRUE);
    }
  else if (G_UNLIKELY (manager->state != XFSM_MANAGER_CHECKPOINT)
        && G_UNLIKELY (manager->state != XFSM_MANAGER_SHUTDOWN))
    {
      xfsm_verbose ("Client Id = %s, requested INTERACT, but manager is not in CheckPoint/Shutdown mode\n"
                    "   Client will be disconnected now.\n\n",
                    xfsm_client_get_id (client));
      xfsm_manager_close_connection (manager, client, TRUE);
    }
  else
    {
      for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
           lp;
           lp = lp->next)
        {
          XfsmClient *cl = lp->data;
          if (xfsm_client_get_state (cl) == XFSM_CLIENT_INTERACTING)
            {
              /* a client is already interacting, so new client has to wait */
              xfsm_client_set_state (client, XFSM_CLIENT_WAITFORINTERACT);
              xfsm_manager_cancel_client_save_timeout(manager, client);
              return;
            }
        }

      xfsm_manager_start_interact (manager, client);
    }
}


void
xfsm_manager_interact_done (XfsmManager *manager,
                            XfsmClient  *client,
                            gboolean     cancel_shutdown)
{
  GList *lp;

  if (G_UNLIKELY (xfsm_client_get_state (client) != XFSM_CLIENT_INTERACTING))
    {
      xfsm_verbose ("Client Id = %s, send INTERACT DONE, but client is not in INTERACTING state\n"
                    "   Client will be disconnected now.\n\n",
                    xfsm_client_get_id (client));
      xfsm_manager_close_connection (manager, client, TRUE);
      return;
    }
  else if (G_UNLIKELY (manager->state != XFSM_MANAGER_CHECKPOINT)
        && G_UNLIKELY (manager->state != XFSM_MANAGER_SHUTDOWN))
    {
      xfsm_verbose ("Client Id = %s, send INTERACT DONE, but manager is not in CheckPoint/Shutdown state\n"
                    "   Client will be disconnected now.\n\n",
                    xfsm_client_get_id (client));
      xfsm_manager_close_connection (manager, client, TRUE);
      return;
    }

  xfsm_client_set_state (client, XFSM_CLIENT_SAVING);

  /* Setting the cancel-shutdown field to True indicates that the user
   * has requested that the entire shutdown be cancelled. Cancel-
   * shutdown may only be True if the corresponding SaveYourself
   * message specified True for the shutdown field and Any or Error
   * for the interact-style field. Otherwise, cancel-shutdown must be
   * False.
   */
  if (cancel_shutdown && manager->state == XFSM_MANAGER_SHUTDOWN)
    {
      /* we go into checkpoint state from here... */
      xfsm_manager_set_state (manager, XFSM_MANAGER_CHECKPOINT);

      /* If a shutdown is in progress, the user may have the option
       * of cancelling the shutdown. If the shutdown is cancelled
       * (specified in the "Interact Done" message), the session
       * manager should send a "Shutdown Cancelled" message to each
       * client that requested to interact.
       */
      for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
           lp;
           lp = lp->next)
        {
          XfsmClient *cl = lp->data;
          if (xfsm_client_get_state (cl) != XFSM_CLIENT_WAITFORINTERACT)
            continue;

          /* reset all clients that are waiting for interact */
          xfsm_client_set_state (client, XFSM_CLIENT_SAVING);
          SmsShutdownCancelled (xfsm_client_get_sms_connection (cl));
        }

        g_signal_emit (manager, signals[SIG_SHUTDOWN_CANCELLED], 0);
    }
  else
    {
      /* let next client interact */
      for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
           lp;
           lp = lp->next)
        {
          XfsmClient *cl = lp->data;
          if (xfsm_client_get_state (cl) == XFSM_CLIENT_WAITFORINTERACT)
            {
              xfsm_manager_start_interact (manager, cl);
              break;
            }
        }
    }

  /* restart save yourself timeout for client */
  xfsm_manager_start_client_save_timeout (manager, client);
}


static void
xfsm_manager_save_yourself_global (XfsmManager     *manager,
                                   gint             save_type,
                                   gboolean         shutdown,
                                   gint             interact_style,
                                   gboolean         fast,
                                   XfsmShutdownType shutdown_type,
                                   gboolean         allow_shutdown_save)
{
  gboolean  shutdown_save = allow_shutdown_save;
  GList    *lp;
  GError   *error = NULL;

  xfsm_verbose ("entering");

  if (shutdown)
    {
      if (!fast && shutdown_type == XFSM_SHUTDOWN_ASK)
        {
          /* if we're not specifying fast shutdown, and we're ok with
           * prompting then ask the user what to do */
          if (!xfsm_logout_dialog (manager->session_name, &manager->shutdown_type, &shutdown_save))
            return;

          /* |allow_shutdown_save| is ignored if we prompt the user.  i think
           * this is the right thing to do. */
        }

      /* update shutdown type if we didn't prompt the user */
      if (shutdown_type != XFSM_SHUTDOWN_ASK)
        manager->shutdown_type = shutdown_type;

      /* we only save the session and quit if we're actually shutting down;
       * suspend and hibernate will (if successful) return us to
       * exactly the same state, so there's no need to save session */
      if (manager->shutdown_type == XFSM_SHUTDOWN_SUSPEND
          || manager->shutdown_type == XFSM_SHUTDOWN_HIBERNATE)
        {
          if (!xfsm_shutdown_try_type (manager->shutdown_helper,
                                       manager->shutdown_type,
                                       &error))
            {
              xfce_message_dialog (NULL, _("Shutdown Failed"),
                                   GTK_STOCK_DIALOG_ERROR,
                                   manager->shutdown_type == XFSM_SHUTDOWN_SUSPEND
                                   ? _("Failed to suspend session")
                                   : _("Failed to hibernate session"),
                                   error->message,
                                   GTK_STOCK_CLOSE, GTK_RESPONSE_ACCEPT,
                                   NULL);
              g_error_free (error);
            }

          /* at this point, either we failed to suspend/hibernate, or we
           * successfully suspended/hibernated, and we've been woken back
           * up, so return control to the user */
          return;
        }
    }

  /* don't save the session if shutting down without save */
  manager->save_session = !shutdown || shutdown_save;

  if (save_type == SmSaveBoth && !manager->save_session)
    {
      /* saving the session, so clients should
       * (prompt to) save the user data only */
      save_type = SmSaveGlobal;
    }

  xfsm_manager_set_state (manager,
                          shutdown
                          ? XFSM_MANAGER_SHUTDOWN
                          : XFSM_MANAGER_CHECKPOINT);

  /* handle legacy applications first! */
  if (manager->save_session)
      xfsm_legacy_perform_session_save ();

  for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
       lp;
       lp = lp->next)
    {
      XfsmClient *client = lp->data;
      XfsmProperties *properties = xfsm_client_get_properties (client);
      const gchar *program;

      /* xterm's session management is broken, so we won't
       * send a SAVE YOURSELF to xterms */
      program = xfsm_properties_get_string (properties, SmProgram);
      if (program != NULL && strcasecmp (program, "xterm") == 0)
        continue;

      if (xfsm_client_get_state (client) != XFSM_CLIENT_SAVINGLOCAL)
        {
          SmsSaveYourself (xfsm_client_get_sms_connection (client), save_type, shutdown,
                           interact_style, fast);
        }

      xfsm_client_set_state (client, XFSM_CLIENT_SAVING);
      xfsm_manager_start_client_save_timeout (manager, client);
    }
}


void
xfsm_manager_save_yourself (XfsmManager *manager,
                            XfsmClient  *client,
                            gint         save_type,
                            gboolean     shutdown,
                            gint         interact_style,
                            gboolean     fast,
                            gboolean     global)
{
  xfsm_verbose ("entering");

  if (G_UNLIKELY (xfsm_client_get_state (client) != XFSM_CLIENT_IDLE))
    {


      xfsm_verbose ("Client Id = %s, requested SAVE YOURSELF, but client is not in IDLE mode.\n"
                    "   Client will be nuked now.\n\n",
                    xfsm_client_get_id (client));
      xfsm_manager_close_connection (manager, client, TRUE);
      return;
    }
  else if (G_UNLIKELY (manager->state != XFSM_MANAGER_IDLE))
    {
      xfsm_verbose ("Client Id = %s, requested SAVE YOURSELF, but manager is not in IDLE mode.\n"
                    "   Client will be nuked now.\n\n",
                    xfsm_client_get_id (client));
      xfsm_manager_close_connection (manager, client, TRUE);
      return;
    }

  if (!global)
    {
      /* client requests a local checkpoint. We slightly ignore
       * shutdown here, since it does not make sense for a local
       * checkpoint.
       */
      SmsSaveYourself (xfsm_client_get_sms_connection (client), save_type, FALSE, interact_style, fast);
      xfsm_client_set_state (client, XFSM_CLIENT_SAVINGLOCAL);
      xfsm_manager_start_client_save_timeout (manager, client);
    }
  else
    xfsm_manager_save_yourself_global (manager, save_type, shutdown, interact_style, fast, XFSM_SHUTDOWN_ASK, TRUE);
}


void
xfsm_manager_save_yourself_phase2 (XfsmManager *manager,
                                   XfsmClient *client)
{
  xfsm_verbose ("entering");

  if (manager->state != XFSM_MANAGER_CHECKPOINT && manager->state != XFSM_MANAGER_SHUTDOWN)
    {
      SmsSaveYourselfPhase2 (xfsm_client_get_sms_connection (client));
      xfsm_client_set_state (client, XFSM_CLIENT_SAVINGLOCAL);
      xfsm_manager_start_client_save_timeout (manager, client);
    }
  else
    {
      xfsm_client_set_state (client, XFSM_CLIENT_WAITFORPHASE2);
      xfsm_manager_cancel_client_save_timeout (manager, client);

      if (!xfsm_manager_check_clients_saving (manager))
        xfsm_manager_maybe_enter_phase2 (manager);
    }
}


void
xfsm_manager_save_yourself_done (XfsmManager *manager,
                                 XfsmClient  *client,
                                 gboolean     success)
{
  xfsm_verbose ("entering");

  /* In xfsm_manager_interact_done we send SmsShutdownCancelled to clients in
     XFSM_CLIENT_WAITFORINTERACT state. They respond with SmcSaveYourselfDone
     (xsmp_shutdown_cancelled in libxfce4ui library) so we allow it here. */
  if (xfsm_client_get_state (client) != XFSM_CLIENT_SAVING &&
      xfsm_client_get_state (client) != XFSM_CLIENT_SAVINGLOCAL &&
      xfsm_client_get_state (client) != XFSM_CLIENT_WAITFORINTERACT)
    {
      xfsm_verbose ("Client Id = %s send SAVE YOURSELF DONE, while not being "
                    "in save mode. Prepare to be nuked!\n",
                    xfsm_client_get_id (client));

      xfsm_manager_close_connection (manager, client, TRUE);
    }

  /* remove client save timeout, as client responded in time */
  xfsm_manager_cancel_client_save_timeout (manager, client);

  if (xfsm_client_get_state (client) == XFSM_CLIENT_SAVINGLOCAL)
    {
      /* client completed local SaveYourself */
      xfsm_client_set_state (client, XFSM_CLIENT_IDLE);
      SmsSaveComplete (xfsm_client_get_sms_connection (client));
    }
  else if (manager->state != XFSM_MANAGER_CHECKPOINT && manager->state != XFSM_MANAGER_SHUTDOWN)
    {
      xfsm_verbose ("Client Id = %s, send SAVE YOURSELF DONE, but manager is not in CheckPoint/Shutdown mode.\n"
                    "   Client will be nuked now.\n\n",
                    xfsm_client_get_id (client));
      xfsm_manager_close_connection (manager, client, TRUE);
    }
  else
    {
      xfsm_client_set_state (client, XFSM_CLIENT_SAVEDONE);
      xfsm_manager_complete_saveyourself (manager);
    }
}


void
xfsm_manager_close_connection (XfsmManager *manager,
                               XfsmClient  *client,
                               gboolean     cleanup)
{
  IceConn ice_conn;
  GList *lp;

  xfsm_client_set_state (client, XFSM_CLIENT_DISCONNECTED);
  xfsm_manager_cancel_client_save_timeout (manager, client);

  if (cleanup)
    {
      SmsConn sms_conn = xfsm_client_get_sms_connection (client);
      ice_conn = SmsGetIceConnection (sms_conn);
      SmsCleanUp (sms_conn);
      IceSetShutdownNegotiation (ice_conn, False);
      IceCloseConnection (ice_conn);
    }

  if (manager->state == XFSM_MANAGER_SHUTDOWNPHASE2)
    {
      for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
           lp;
           lp = lp->next)
        {
          XfsmClient *cl = lp->data;
          if (xfsm_client_get_state (cl) != XFSM_CLIENT_DISCONNECTED)
            return;
        }

      /* all clients finished the DIE phase in time */
      if (manager->die_timeout_id)
        {
          g_source_remove (manager->die_timeout_id);
          manager->die_timeout_id = 0;
        }
      gtk_main_quit ();
    }
  else if (manager->state == XFSM_MANAGER_SHUTDOWN || manager->state == XFSM_MANAGER_CHECKPOINT)
    {
      xfsm_verbose ("Client Id = %s, closed connection in checkpoint state\n"
                    "   Session manager will show NO MERCY\n\n",
                    xfsm_client_get_id (client));

      /* stupid client disconnected in CheckPoint state, prepare to be nuked! */
      g_queue_remove (manager->running_clients, client);
      g_object_unref (client);
      xfsm_manager_complete_saveyourself (manager);
    }
  else
    {
      XfsmProperties *properties = xfsm_client_steal_properties (client);

      if (properties != NULL)
        {
          if (xfsm_properties_check (properties))
            {
              if (xfsm_manager_handle_failed_properties (manager, properties) == FALSE)
                xfsm_properties_free (properties);
            }
          else
            xfsm_properties_free (properties);
        }

      /* regardless of the restart style hint, the current instance of
       * the client is gone, so remove it from the client list and free it. */
      g_queue_remove (manager->running_clients, client);
      g_object_unref (client);
    }
}


void
xfsm_manager_close_connection_by_ice_conn (XfsmManager *manager,
                                           IceConn      ice_conn)
{
  GList *lp;

  for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
       lp;
       lp = lp->next)
    {
      XfsmClient *client = lp->data;
      if (SmsGetIceConnection (xfsm_client_get_sms_connection (client)) == ice_conn)
        {
          xfsm_manager_close_connection (manager, client, FALSE);
          break;
        }
    }

  /* be sure to close the Ice connection in any case */
  IceSetShutdownNegotiation (ice_conn, False);
  IceCloseConnection (ice_conn);
}


gboolean
xfsm_manager_terminate_client (XfsmManager *manager,
                               XfsmClient  *client,
                               GError **error)
{
  if (manager->state != XFSM_MANAGER_IDLE
      || xfsm_client_get_state (client) != XFSM_CLIENT_IDLE)
    {
      if (error)
        {
          g_set_error (error, XFSM_ERROR, XFSM_ERROR_BAD_STATE,
                       _("Can only terminate clients when in the idle state"));
        }
      return FALSE;
    }

  SmsDie (xfsm_client_get_sms_connection (client));

  return TRUE;
}


void
xfsm_manager_perform_shutdown (XfsmManager *manager)
{
  GList *lp;

  xfsm_verbose ("entering");

  /* send SmDie message to all clients */
  xfsm_manager_set_state (manager, XFSM_MANAGER_SHUTDOWNPHASE2);
  for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
       lp;
       lp = lp->next)
    {
      XfsmClient *client = lp->data;
      SmsDie (xfsm_client_get_sms_connection (client));
    }

  /* check for SmRestartAnyway clients that have already quit and
   * set a ShutdownCommand */
  for (lp = g_queue_peek_nth_link (manager->restart_properties, 0);
       lp;
       lp = lp->next)
    {
      XfsmProperties *properties = lp->data;
      gint            restart_style_hint;
      gchar         **shutdown_command;

      restart_style_hint = xfsm_properties_get_uchar (properties,
                                                      SmRestartStyleHint,
                                                      SmRestartIfRunning);
      shutdown_command = xfsm_properties_get_strv (properties, SmShutdownCommand);

      if (restart_style_hint == SmRestartAnyway && shutdown_command != NULL)
        {
          xfsm_verbose ("Client Id = %s, quit already, running shutdown command.\n\n",
                        properties->client_id);

          g_spawn_sync (xfsm_properties_get_string (properties, SmCurrentDirectory),
                        shutdown_command,
                        xfsm_properties_get_strv (properties, SmEnvironment),
                        G_SPAWN_SEARCH_PATH,
                        NULL, NULL,
                        NULL, NULL,
                        NULL, NULL);
        }
    }

  /* give all clients the chance to close the connection */
  manager->die_timeout_id = g_timeout_add (DIE_TIMEOUT,
                                           (GSourceFunc) gtk_main_quit,
                                           NULL);
}


gboolean
xfsm_manager_check_clients_saving (XfsmManager *manager)
{
  GList *lp;

  for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
       lp;
       lp = lp->next)
    {
      XfsmClient *client = lp->data;
      XfsmClientState state = xfsm_client_get_state (client);
      switch (state)
        {
          case XFSM_CLIENT_SAVING:
          case XFSM_CLIENT_WAITFORINTERACT:
          case XFSM_CLIENT_INTERACTING:
            return TRUE;
          default:
            break;
        }
    }

  return FALSE;
}


gboolean
xfsm_manager_maybe_enter_phase2 (XfsmManager *manager)
{
  gboolean entered_phase2 = FALSE;
  GList *lp;

  for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
       lp;
       lp = lp->next)
    {
      XfsmClient *client = lp->data;

      if (xfsm_client_get_state (client) == XFSM_CLIENT_WAITFORPHASE2)
        {
          entered_phase2 = TRUE;
          SmsSaveYourselfPhase2 (xfsm_client_get_sms_connection (client));
          xfsm_client_set_state (client, XFSM_CLIENT_SAVING);
          xfsm_manager_start_client_save_timeout (manager, client);

          xfsm_verbose ("Client Id = %s enters SAVE YOURSELF PHASE2.\n\n",
                        xfsm_client_get_id (client));
        }
    }

  return entered_phase2;
}


void
xfsm_manager_complete_saveyourself (XfsmManager *manager)
{
  GList *lp;

  /* Check if still clients in SAVING state or if we have to enter PHASE2
   * now. In either case, SaveYourself cannot be completed in this run.
   */
  if (xfsm_manager_check_clients_saving (manager) || xfsm_manager_maybe_enter_phase2 (manager))
    return;

  xfsm_verbose ("Manager finished SAVE YOURSELF, session data will be stored now.\n\n");

  /* all clients done, store session data */
  if (manager->save_session)
    xfsm_manager_store_session (manager);

  if (manager->state == XFSM_MANAGER_CHECKPOINT)
    {
      /* reset all clients to idle state */
      xfsm_manager_set_state (manager, XFSM_MANAGER_IDLE);
      for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
           lp;
           lp = lp->next)
        {
          XfsmClient *client = lp->data;
          xfsm_client_set_state (client, XFSM_CLIENT_IDLE);
          SmsSaveComplete (xfsm_client_get_sms_connection (client));
        }
    }
  else
    {
      /* shutdown the session */
      xfsm_manager_perform_shutdown (manager);
    }
}


static gboolean
xfsm_manager_save_timeout (gpointer user_data)
{
  XfsmSaveTimeoutData *stdata = user_data;

  xfsm_verbose ("Client id = %s, received SAVE TIMEOUT\n"
                "   Client will be disconnected now.\n\n",
                xfsm_client_get_id (stdata->client));

  /* returning FALSE below will free the data */
  g_object_steal_data (G_OBJECT (stdata->client), "--save-timeout-id");

  xfsm_manager_close_connection (stdata->manager, stdata->client, TRUE);

  return FALSE;
}


static void
xfsm_manager_start_client_save_timeout (XfsmManager *manager,
                                        XfsmClient  *client)
{
  XfsmSaveTimeoutData *sdata = g_new(XfsmSaveTimeoutData, 1);

  sdata->manager = manager;
  sdata->client = client;
  /* |sdata| will get freed when the source gets removed */
  sdata->timeout_id = g_timeout_add_full (G_PRIORITY_DEFAULT, SAVE_TIMEOUT,
                                          xfsm_manager_save_timeout,
                                          sdata, (GDestroyNotify) g_free);
  /* ... or, if the object gets destroyed first, the source will get
   * removed and will free |sdata| for us.  also, if there's a pending
   * timer, this call will clear it. */
  g_object_set_data_full (G_OBJECT (client), "--save-timeout-id",
                          GUINT_TO_POINTER (sdata->timeout_id),
                          (GDestroyNotify) g_source_remove);
}


static void
xfsm_manager_cancel_client_save_timeout (XfsmManager *manager,
                                         XfsmClient  *client)
{
  /* clearing out the data will call g_source_remove(), which will free it */
  g_object_set_data (G_OBJECT (client), "--save-timeout-id", NULL);
}


void
xfsm_manager_store_session (XfsmManager *manager)
{
  WnckWorkspace *workspace;
  GdkDisplay    *display;
  WnckScreen    *screen;
  XfceRc        *rc;
  GList         *lp;
  gchar          prefix[64];
  gchar         *backup;
  gchar         *group;
  gint           count = 0;
  gint           n, m;

  /* open file for writing, creates it if it doesn't exist */
  rc = xfce_rc_simple_open (manager->session_file, FALSE);
  if (G_UNLIKELY (rc == NULL))
    {
      fprintf (stderr,
               "xfce4-session: Unable to open session file %s for "
               "writing. Session data will not be stored. Please check "
               "your installation.\n",
               manager->session_file);
      return;
    }

  /* backup the old session file first */
  if (g_file_test (manager->session_file, G_FILE_TEST_IS_REGULAR))
    {
      backup = g_strconcat (manager->session_file, ".bak", NULL);
      unlink (backup);
      if (link (manager->session_file, backup))
          g_warning ("Failed to create session file backup");
      g_free (backup);
    }

  if (manager->state == XFSM_MANAGER_CHECKPOINT && manager->checkpoint_session_name != NULL)
    group = g_strconcat ("Session: ", manager->checkpoint_session_name, NULL);
  else
    group = g_strconcat ("Session: ", manager->session_name, NULL);
  xfce_rc_delete_group (rc, group, TRUE);
  xfce_rc_set_group (rc, group);
  g_free (group);

  for (lp = g_queue_peek_nth_link (manager->restart_properties, 0);
       lp;
       lp = lp->next)
    {
      XfsmProperties *properties = lp->data;
      g_snprintf (prefix, 64, "Client%d_", count);
      xfsm_properties_store (properties, rc, prefix);
      ++count;
    }

  for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
       lp;
       lp = lp->next)
    {
      XfsmClient     *client     = lp->data;
      XfsmProperties *properties = xfsm_client_get_properties (client);
      gint            restart_style_hint;

      if (properties == NULL || !xfsm_properties_check (xfsm_client_get_properties (client)))
        continue;
      restart_style_hint = xfsm_properties_get_uchar (properties,
                                                      SmRestartStyleHint,
                                                      SmRestartIfRunning);
      if (restart_style_hint == SmRestartNever)
        continue;

      g_snprintf (prefix, 64, "Client%d_", count);
      xfsm_properties_store (xfsm_client_get_properties (client), rc, prefix);
      ++count;
    }

  xfce_rc_write_int_entry (rc, "Count", count);

  /* store legacy applications state */
  xfsm_legacy_store_session (rc);

  /* store current workspace numbers */
  display = gdk_display_get_default ();
  for (n = 0; n < gdk_display_get_n_screens (display); ++n)
    {
      screen = wnck_screen_get (n);
      wnck_screen_force_update (screen);

      workspace = wnck_screen_get_active_workspace (screen);
      m = wnck_workspace_get_number (workspace);

      g_snprintf (prefix, 64, "Screen%d_ActiveWorkspace", n);
      xfce_rc_write_int_entry (rc, prefix, m);
    }

  /* remember time */
  xfce_rc_write_int_entry (rc, "LastAccess", time (NULL));

  xfce_rc_close (rc);

  g_free (manager->checkpoint_session_name);
  manager->checkpoint_session_name = NULL;
}


XfsmShutdownType
xfsm_manager_get_shutdown_type (XfsmManager *manager)
{
  return manager->shutdown_type;
}


XfsmManagerState
xfsm_manager_get_state (XfsmManager *manager)
{
  return manager->state;
}


GQueue *
xfsm_manager_get_queue (XfsmManager         *manager,
                        XfsmManagerQueueType q_type)
{
  switch(q_type)
    {
      case XFSM_MANAGER_QUEUE_PENDING_PROPS:
        return manager->pending_properties;
      case XFSM_MANAGER_QUEUE_STARTING_PROPS:
        return manager->starting_properties;
      case XFSM_MANAGER_QUEUE_RESTART_PROPS:
        return manager->restart_properties;
      case XFSM_MANAGER_QUEUE_RUNNING_CLIENTS:
        return manager->running_clients;
      case XFSM_MANAGER_QUEUE_FAILSAFE_CLIENTS:
        return manager->failsafe_clients;
      default:
        g_warning ("Requested invalid queue type %d", (gint)q_type);
        return NULL;
    }
}


gboolean
xfsm_manager_get_use_failsafe_mode (XfsmManager *manager)
{
  return manager->failsafe_mode;
}


gboolean
xfsm_manager_get_compat_startup (XfsmManager          *manager,
                                 XfsmManagerCompatType type)
{
  switch (type)
    {
      case XFSM_MANAGER_COMPAT_GNOME:
        return manager->compat_gnome;
      case XFSM_MANAGER_COMPAT_KDE:
        return manager->compat_kde;
      default:
        g_warning ("Invalid compat startup type %d", type);
        return FALSE;
    }
}


gboolean
xfsm_manager_get_start_at (XfsmManager *manager)
{
  return manager->start_at;
}


/*
 * dbus server impl
 */

static DBusHandlerResult xfsm_manager_watch_dbus_disconnect (DBusConnection *connection,
                                                             DBusMessage *message,
                                                             void *user_data);

static gboolean xfsm_manager_dbus_get_info (XfsmManager *manager,
                                            gchar      **OUT_name,
                                            gchar      **OUT_version,
                                            gchar      **OUT_vendor,
                                            GError     **error);
static gboolean xfsm_manager_dbus_list_clients (XfsmManager *manager,
                                                GPtrArray  **OUT_clients,
                                                GError     **error);
static gboolean xfsm_manager_dbus_get_state (XfsmManager *manager,
                                             guint       *OUT_state,
                                             GError     **error);
static gboolean xfsm_manager_dbus_checkpoint (XfsmManager *manager,
                                              const gchar *session_name,
                                              GError     **error);
static gboolean xfsm_manager_dbus_logout (XfsmManager *manager,
                                          gboolean     show_dialog,
                                          gboolean     allow_save,
                                          GError     **error);
static gboolean xfsm_manager_dbus_shutdown (XfsmManager *manager,
                                            gboolean     allow_save,
                                            GError     **error);
static gboolean xfsm_manager_dbus_can_shutdown (XfsmManager *manager,
                                                gboolean    *can_shutdown,
                                                GError     **error);
static gboolean xfsm_manager_dbus_restart (XfsmManager *manager,
                                           gboolean     allow_save,
                                           GError     **error);
static gboolean xfsm_manager_dbus_can_restart (XfsmManager *manager,
                                               gboolean    *can_restart,
                                               GError     **error);
static gboolean xfsm_manager_dbus_suspend (XfsmManager *manager,
                                           GError     **error);
static gboolean xfsm_manager_dbus_can_suspend (XfsmManager *manager,
                                               gboolean    *can_suspend,
                                               GError     **error);
static gboolean xfsm_manager_dbus_hibernate (XfsmManager *manager,
                                             GError     **error);
static gboolean xfsm_manager_dbus_can_hibernate (XfsmManager *manager,
                                                 gboolean    *can_hibernate,
                                                 GError     **error);


/* eader needs the above fwd decls */
#include <xfce4-session/xfsm-manager-dbus.h>


static void
xfsm_manager_dbus_class_init (XfsmManagerClass *klass)
{
  dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (klass),
                                   &dbus_glib_xfsm_manager_object_info);
}


static void
xfsm_manager_dbus_init (XfsmManager *manager)
{
  GError *error = NULL;
  DBusConnection *connection;

  manager->session_bus = dbus_g_bus_get (DBUS_BUS_SESSION, &error);

  if (G_UNLIKELY (!manager->session_bus))
    {
      g_critical ("Unable to contact D-Bus session bus: %s", error ? error->message : "Unknown error");
      if (error)
        g_error_free (error);
      return;
    }

  connection = dbus_g_connection_get_connection (manager->session_bus);
  dbus_connection_set_exit_on_disconnect (connection, FALSE);

  dbus_g_connection_register_g_object (manager->session_bus,
                                       "/org/xfce/SessionManager",
                                       G_OBJECT (manager));

  dbus_connection_add_filter (dbus_g_connection_get_connection (manager->session_bus),
                              xfsm_manager_watch_dbus_disconnect,
                              manager, NULL);
}


static void
xfsm_manager_dbus_cleanup (XfsmManager *manager)
{
  if (G_LIKELY (manager->session_bus))
    {
      dbus_connection_remove_filter (dbus_g_connection_get_connection (manager->session_bus),
                                     xfsm_manager_watch_dbus_disconnect,
                                     manager);
      dbus_g_connection_unref (manager->session_bus);
      manager->session_bus = NULL;
    }
}


static DBusHandlerResult
xfsm_manager_watch_dbus_disconnect (DBusConnection *connection,
                                    DBusMessage *message,
                                    void *user_data)
{
  if (dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL, "Disconnected"))
    {
      g_message ("Got disconnected from D-Bus.  Unless this happened during "
                 "session shutdown, this is probably a bad thing.");

      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


static gboolean
xfsm_manager_dbus_get_info (XfsmManager *manager,
                            gchar      **OUT_name,
                            gchar      **OUT_version,
                            gchar      **OUT_vendor,
                            GError     **error)
{
  *OUT_name = g_strdup (PACKAGE);
  *OUT_version = g_strdup (VERSION);
  *OUT_vendor = g_strdup ("Xfce");

  return TRUE;
}


static gboolean
xfsm_manager_dbus_list_clients (XfsmManager *manager,
                                GPtrArray  **OUT_clients,
                                GError     **error)
{
  GList *lp;

  *OUT_clients = g_ptr_array_sized_new (g_queue_get_length (manager->running_clients));

  for (lp = g_queue_peek_nth_link (manager->running_clients, 0);
       lp;
       lp = lp->next)
    {
      XfsmClient *client = XFSM_CLIENT (lp->data);
      gchar *object_path = g_strdup (xfsm_client_get_object_path (client));
      g_ptr_array_add (*OUT_clients, object_path);
    }

    return TRUE;
}


static gboolean
xfsm_manager_dbus_get_state (XfsmManager *manager,
                             guint       *OUT_state,
                             GError     **error)
{
  *OUT_state = manager->state;
  return TRUE;
}


static gboolean
xfsm_manager_dbus_checkpoint_idled (gpointer data)
{
  XfsmManager *manager = XFSM_MANAGER (data);

  xfsm_manager_save_yourself_global (manager, SmSaveBoth, FALSE,
                                     SmInteractStyleNone, FALSE,
                                     XFSM_SHUTDOWN_ASK, TRUE);

  return FALSE;
}


static gboolean
xfsm_manager_dbus_checkpoint (XfsmManager *manager,
                              const gchar *session_name,
                              GError     **error)
{
  if (manager->state != XFSM_MANAGER_IDLE)
    {
      g_set_error (error, XFSM_ERROR, XFSM_ERROR_BAD_STATE,
                   _("Session manager must be in idle state when requesting a checkpoint"));
      return FALSE;
    }

  g_free (manager->checkpoint_session_name);
  if (session_name[0] != '\0')
    manager->checkpoint_session_name = g_strdup (session_name);
  else
    manager->checkpoint_session_name = NULL;

  /* idle so the dbus call returns in the client */
  g_idle_add (xfsm_manager_dbus_checkpoint_idled, manager);

  return TRUE;
}


static gboolean
xfsm_manager_dbus_shutdown_idled (gpointer data)
{
  ShutdownIdleData *idata = data;

  xfsm_manager_save_yourself_global (idata->manager, SmSaveBoth, TRUE,
                                     SmInteractStyleAny, FALSE,
                                     idata->type, idata->allow_save);

  return FALSE;
}


static gboolean
xfsm_manager_save_yourself_dbus (XfsmManager       *manager,
                                 XfsmShutdownType   type,
                                 gboolean           allow_save,
                                 GError           **error)
{
  ShutdownIdleData *idata;

  if (manager->state != XFSM_MANAGER_IDLE)
    {
      g_set_error (error, XFSM_ERROR, XFSM_ERROR_BAD_STATE,
                   _("Session manager must be in idle state when requesting a shutdown"));
      return FALSE;
    }

  idata = g_new (ShutdownIdleData, 1);
  idata->manager = manager;
  idata->type = type;
  idata->allow_save = allow_save;
  g_idle_add_full (G_PRIORITY_DEFAULT, xfsm_manager_dbus_shutdown_idled,
                   idata, (GDestroyNotify) g_free);

  return TRUE;
}


static gboolean
xfsm_manager_dbus_logout (XfsmManager *manager,
                          gboolean     show_dialog,
                          gboolean     allow_save,
                          GError     **error)
{
  XfsmShutdownType type;

  g_return_val_if_fail (XFSM_IS_MANAGER (manager), FALSE);

  type = show_dialog ? XFSM_SHUTDOWN_ASK : XFSM_SHUTDOWN_LOGOUT;
  return xfsm_manager_save_yourself_dbus (manager, type,
                                          allow_save, error);
}


static gboolean
xfsm_manager_dbus_shutdown (XfsmManager *manager,
                            gboolean     allow_save,
                            GError     **error)
{
  g_return_val_if_fail (XFSM_IS_MANAGER (manager), FALSE);
  return xfsm_manager_save_yourself_dbus (manager, XFSM_SHUTDOWN_SHUTDOWN,
                                          allow_save, error);
}


static gboolean
xfsm_manager_dbus_can_shutdown (XfsmManager *manager,
                                gboolean    *can_shutdown,
                                GError     **error)
{
  g_return_val_if_fail (XFSM_IS_MANAGER (manager), FALSE);
  return xfsm_shutdown_can_shutdown (manager->shutdown_helper,
                                     can_shutdown, error);
}


static gboolean
xfsm_manager_dbus_restart (XfsmManager *manager,
                           gboolean     allow_save,
                           GError     **error)
{
  g_return_val_if_fail (XFSM_IS_MANAGER (manager), FALSE);
  return xfsm_manager_save_yourself_dbus (manager, XFSM_SHUTDOWN_RESTART,
                                          allow_save, error);
}


static gboolean
xfsm_manager_dbus_can_restart (XfsmManager *manager,
                               gboolean    *can_restart,
                               GError     **error)
{
  g_return_val_if_fail (XFSM_IS_MANAGER (manager), FALSE);
  return xfsm_shutdown_can_restart (manager->shutdown_helper,
                                    can_restart, error);
}


static gboolean
xfsm_manager_dbus_suspend (XfsmManager *manager,
                           GError     **error)
{
  g_return_val_if_fail (XFSM_IS_MANAGER (manager), FALSE);
  return xfsm_shutdown_try_suspend (manager->shutdown_helper, error);
}


static gboolean
xfsm_manager_dbus_can_suspend (XfsmManager *manager,
                               gboolean    *can_suspend,
                               GError     **error)
{
  gboolean retval;
  gboolean auth_suspend;

  g_return_val_if_fail (XFSM_IS_MANAGER (manager), FALSE);
  retval = xfsm_shutdown_can_suspend (manager->shutdown_helper,
                                      can_suspend, &auth_suspend, error);

  if (!auth_suspend)
    *can_suspend = FALSE;

  return retval;
}

static gboolean
xfsm_manager_dbus_hibernate (XfsmManager *manager,
                             GError     **error)
{
  g_return_val_if_fail (XFSM_IS_MANAGER (manager), FALSE);
  return xfsm_shutdown_try_hibernate (manager->shutdown_helper, error);
}


static gboolean
xfsm_manager_dbus_can_hibernate (XfsmManager *manager,
                                 gboolean    *can_hibernate,
                                 GError     **error)
{
  gboolean retval;
  gboolean auth_hibernate;

  g_return_val_if_fail (XFSM_IS_MANAGER (manager), FALSE);
  retval = xfsm_shutdown_can_hibernate (manager->shutdown_helper,
                                        can_hibernate, &auth_hibernate, error);

  if (!auth_hibernate)
    *can_hibernate = FALSE;

  return retval;
}
