/* $Id$ */
/*-
 * Copyright (c) 2004 os-cillation e.K.
 *
 * Written by Benedikt Meurer <benny@xfce.org>.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <terminal/terminal-accel-map.h>
#include <terminal/terminal-app.h>
#include <terminal/terminal-config.h>
#include <terminal/terminal-preferences.h>
#include <terminal/terminal-window.h>



static void               terminal_app_class_init       (TerminalAppClass *klass);
static void               terminal_app_init             (TerminalApp      *app);
static void               terminal_app_finalize         (GObject          *object);
static void               terminal_app_update_accels    (TerminalApp      *app);
static void               terminal_app_unregister       (DBusConnection   *connection,
                                                         void             *user_data);
static DBusHandlerResult  terminal_app_message          (DBusConnection   *connection,
                                                         DBusMessage      *message,
                                                         void             *user_data);
static void               terminal_app_new_window       (TerminalWindow   *window,
                                                         const gchar      *working_directory,
                                                         TerminalApp      *app);
static void               terminal_app_window_destroyed (GtkWidget        *window,
                                                         TerminalApp      *app);



struct _TerminalApp
{
  GObject              __parent__;
  TerminalPreferences *preferences;
  TerminalAccelMap    *accel_map;
  gchar               *initial_menu_bar_accel;
  GList               *windows;
  guint                server_running : 1;
};



static GObjectClass *parent_class;

static const struct DBusObjectPathVTable terminal_app_vtable =
{
  terminal_app_unregister,
  terminal_app_message,
  NULL,
};



G_DEFINE_TYPE (TerminalApp, terminal_app, G_TYPE_OBJECT);



static void
terminal_app_class_init (TerminalAppClass *klass)
{
  GObjectClass *gobject_class;
  
  parent_class = g_type_class_peek_parent (klass);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = terminal_app_finalize;
}



static void
terminal_app_init (TerminalApp *app)
{
  app->preferences = terminal_preferences_get ();
  g_signal_connect_swapped (G_OBJECT (app->preferences), "notify::shortcuts-no-menukey",
                            G_CALLBACK (terminal_app_update_accels), app);

  /* remember the original menu bar accel */
  g_object_get (G_OBJECT (gtk_settings_get_default ()),
                "gtk-menu-bar-accel", &app->initial_menu_bar_accel,
                NULL);

  terminal_app_update_accels (app);

  /* connect the accel map */
  app->accel_map = terminal_accel_map_new ();
}



static void
terminal_app_finalize (GObject *object)
{
  TerminalApp *app = TERMINAL_APP (object);
  GList       *lp;

  for (lp = app->windows; lp != NULL; lp = lp->next)
    {
      g_signal_handlers_disconnect_by_func (G_OBJECT (lp->data), G_CALLBACK (terminal_app_window_destroyed), app);
      g_signal_handlers_disconnect_by_func (G_OBJECT (lp->data), G_CALLBACK (terminal_app_new_window), app);
      gtk_widget_destroy (GTK_WIDGET (lp->data));
    }
  g_list_free (app->windows);

  g_signal_handlers_disconnect_by_func (G_OBJECT (app->preferences),
                                        G_CALLBACK (terminal_app_update_accels),
                                        app);
  g_object_unref (G_OBJECT (app->preferences));

  if (app->initial_menu_bar_accel != NULL)
    g_free (app->initial_menu_bar_accel);

  g_object_unref (G_OBJECT (app->accel_map));

  G_OBJECT_CLASS (parent_class)->finalize (object);
}



static void
terminal_app_update_accels (TerminalApp *app)
{
  const gchar *accel;
  gboolean     shortcuts_no_menukey;

  g_object_get (G_OBJECT (app->preferences), 
                "shortcuts-no-menukey", &shortcuts_no_menukey,
                NULL);

  if (shortcuts_no_menukey)
    accel = "<Shift><Control><Mod1><Mod2><Mod3><Mod4><Mod5>F10";
  else
    accel = app->initial_menu_bar_accel;

  gtk_settings_set_string_property (gtk_settings_get_default (),
                                    "gtk-menu-bar-accel", accel,
                                    "Terminal");
}



static void
terminal_app_unregister (DBusConnection *connection,
                         void           *user_data)
{
}



static DBusHandlerResult
terminal_app_message (DBusConnection  *connection,
                      DBusMessage     *message,
                      void            *user_data)
{
  DBusMessageIter  iter;
  TerminalApp     *app = TERMINAL_APP (user_data);
  DBusMessage     *reply;
  GError          *error = NULL;
  gchar          **argv;
  gint             argc;

  if (dbus_message_is_method_call (message, 
                                   TERMINAL_DBUS_INTERFACE,
                                   TERMINAL_DBUS_METHOD_LAUNCH))
    {
      if (!dbus_message_iter_init (message, &iter))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

      if (!dbus_message_iter_get_string_array (&iter, &argv, &argc))
        {
          reply = dbus_message_new_error (message, TERMINAL_DBUS_ERROR, "Invalid arguments");
          dbus_connection_send (connection, reply, NULL);
          dbus_message_unref (reply);
          return DBUS_HANDLER_RESULT_HANDLED;
        }

      if (!terminal_app_process (app, argv, argc, &error))
        {
          reply = dbus_message_new_error (message, TERMINAL_DBUS_ERROR, error->message);
          dbus_connection_send (connection, reply, NULL);
          dbus_message_unref (reply);
        }
      else
        {
          reply = dbus_message_new_method_return (message);
          dbus_connection_send (connection, reply, NULL);
        }

      dbus_free_string_array (argv);
      dbus_message_unref (reply);
    }
  else if (dbus_message_is_signal (message,
                                   DBUS_INTERFACE_ORG_FREEDESKTOP_LOCAL,
                                   "Disconnected"))
    {
      g_printerr (_("D-BUS message bus disconnected, exiting...\n"));
      gtk_main_quit ();
    }
  else
    {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

  return DBUS_HANDLER_RESULT_HANDLED;
}



static void
terminal_app_new_window (TerminalWindow *window,
                         const gchar    *working_directory,
                         TerminalApp    *app)
{
  TerminalWindowAttr *win_attr;
  TerminalTabAttr    *tab_attr;

  win_attr = terminal_window_attr_new ();
  tab_attr = win_attr->tabs->data;
  tab_attr->directory = g_strdup (working_directory);

  terminal_app_open_window (app, win_attr);

  terminal_window_attr_free (win_attr);
}



static void
terminal_app_window_destroyed (GtkWidget   *window,
                               TerminalApp *app)
{
  g_return_if_fail (g_list_find (app->windows, window) != NULL);

  app->windows = g_list_remove (app->windows, window);

  if (G_UNLIKELY (app->windows == NULL))
    gtk_main_quit ();
}



/**
 * terminal_app_new:
 *
 * Return value :
 **/
TerminalApp*
terminal_app_new (void)
{
  return g_object_new (TERMINAL_TYPE_APP, NULL);
}



/**
 * terminal_app_start_server:
 * @app         : A #TerminalApp.
 * @error       : The location to store the error to, or %NULL.
 *
 * Return value : %TRUE on success, %FALSE on failure.
 **/
gboolean
terminal_app_start_server (TerminalApp *app,
                           GError     **error)
{
  DBusConnection *connection;
  DBusError       derror;
  
  g_return_val_if_fail (TERMINAL_IS_APP (app), FALSE);

  if (G_UNLIKELY (app->server_running))
    return TRUE;

  connection = exo_dbus_bus_connection ();
  if (G_UNLIKELY (connection == NULL))
    {
      g_set_error (error, DBUS_GERROR, DBUS_GERROR_FAILED,
                   _("Unable to connect to D-BUS message daemon"));
      return FALSE;
    }

  dbus_error_init (&derror);

  if (!dbus_bus_acquire_service (connection, TERMINAL_DBUS_SERVICE, 0, &derror))
    {
      g_set_error (error, DBUS_GERROR, DBUS_GERROR_FAILED,
                   _("Unable to acquire service %s: %s"),
                   TERMINAL_DBUS_SERVICE, derror.message);
      dbus_error_free (&derror);
      return FALSE;
    }

  if (dbus_connection_register_object_path (connection,
                                            TERMINAL_DBUS_PATH,
                                            &terminal_app_vtable,
                                            app) < 0)
    {
      g_set_error (error, DBUS_GERROR, DBUS_GERROR_FAILED,
                   _("Unable to register object %s\n"),
                   TERMINAL_DBUS_PATH);
      return FALSE;
    }

  app->server_running = 1;

  return TRUE;
}



/**
 * terminal_app_process:
 * @app
 * @argv
 * @argc
 * @error
 *
 * Return value:
 **/
gboolean
terminal_app_process (TerminalApp  *app,
                      gchar       **argv,
                      gint          argc,
                      GError      **error)
{
  GList *attrs;
  GList *lp;

  if (!terminal_options_parse (argc, argv, &attrs, NULL, error))
    return FALSE;

  for (lp = attrs; lp != NULL; lp = lp->next)
    terminal_app_open_window (app, lp->data);

  g_list_foreach (attrs, (GFunc) terminal_window_attr_free, NULL);
  g_list_free (attrs);

  return TRUE;
}



/**
 * terminal_app_open_window:
 * @app   : A #TerminalApp object.
 * @attr  : The attributes for the new window.
 **/
void
terminal_app_open_window (TerminalApp         *app,
                          TerminalWindowAttr  *attr)
{
  TerminalTabAttr *tab_attr;
  GtkWidget       *window;
  GtkWidget       *terminal;
  GList           *lp;

  g_return_if_fail (TERMINAL_IS_APP (app));
  g_return_if_fail (attr != NULL);

  window = terminal_window_new (attr->menubar, attr->borders, attr->toolbars);
  g_signal_connect (G_OBJECT (window), "destroy",
                    G_CALLBACK (terminal_app_window_destroyed), app);
  g_signal_connect (G_OBJECT (window), "new-window",
                    G_CALLBACK (terminal_app_new_window), app);
  app->windows = g_list_append (app->windows, window);

  if (attr->role != NULL)
    gtk_window_set_role (GTK_WINDOW (window), attr->role);
  if (attr->startup_id != NULL)
    terminal_window_set_startup_id (TERMINAL_WINDOW (window), attr->startup_id);

  for (lp = attr->tabs; lp != NULL; lp = lp->next)
    {
      terminal = terminal_widget_new ();

      tab_attr = lp->data;
      if (tab_attr->command != NULL)
        terminal_widget_set_custom_command (TERMINAL_WIDGET (terminal), tab_attr->command);
      if (tab_attr->directory != NULL)
        terminal_widget_set_working_directory (TERMINAL_WIDGET (terminal), tab_attr->directory);
      if (tab_attr->title != NULL)
        terminal_widget_set_custom_title (TERMINAL_WIDGET (terminal), tab_attr->title);

      terminal_window_add (TERMINAL_WINDOW (window), TERMINAL_WIDGET (terminal));

      /* if this was the first tab, we set the geometry string now
       * and show the window. This is required to work around a hang
       * in Gdk, which I failed to figure the cause til now.
       */
      if (lp == attr->tabs)
        {
          if (attr->geometry != NULL && !gtk_window_parse_geometry (GTK_WINDOW (window), attr->geometry))
            g_printerr (_("Invalid geometry string \"%s\"\n"), attr->geometry);
          gtk_widget_show (window);
        }

      terminal_widget_launch_child (TERMINAL_WIDGET (terminal));
    }
}



/**
 * terminal_app_try_invoke:
 * @argc
 * @argv
 * @error
 *
 * Return value:
 **/
gboolean
terminal_app_try_invoke (gint              argc,
                         gchar           **argv,
                         GError          **error)
{
  DBusMessageIter  iter;
  DBusConnection  *connection;
  DBusMessage     *message;
  DBusMessage     *result;
  DBusError        derror;

  connection = exo_dbus_bus_connection ();
  if (G_UNLIKELY (connection == NULL))
    return FALSE;

  message = dbus_message_new_method_call (TERMINAL_DBUS_SERVICE,
                                          TERMINAL_DBUS_PATH,
                                          TERMINAL_DBUS_INTERFACE,
                                          TERMINAL_DBUS_METHOD_LAUNCH);
  dbus_message_set_auto_activation (message, FALSE);
  dbus_message_append_iter_init (message, &iter);
  dbus_message_iter_append_string_array (&iter, (const gchar **) argv, argc);

  dbus_error_init (&derror);
  result = dbus_connection_send_with_reply_and_block (connection,
                                                      message,
                                                      2000, &derror);
  dbus_message_unref (message);

  if (result == NULL)
    {
      g_set_error (error, DBUS_GERROR, DBUS_GERROR_FAILED,
                   "%s", derror.message);
      dbus_error_free (&derror);
      return FALSE;
    }

  if (dbus_message_is_error (result, TERMINAL_DBUS_ERROR))
    {
      dbus_set_error_from_message (&derror, result);
      g_set_error (error, DBUS_GERROR, DBUS_GERROR_FAILED,
                   "%s", derror.message);
      dbus_message_unref (result);
      dbus_error_free (&derror);
      return FALSE;
    }

  dbus_message_unref (result);
  return TRUE;
}




