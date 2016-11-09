/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stdio.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "config.h"

#include "pinos/client/pinos.h"
#include "pinos/client/log.h"

#include "pinos/server/daemon.h"
#include "pinos/server/node.h"
#include "pinos/server/client-node.h"
#include "pinos/server/dbus-client-node.h"
#include "pinos/server/client.h"
#include "pinos/server/link.h"
#include "pinos/server/data-loop.h"
#include "pinos/server/main-loop.h"

#include "pinos/dbus/org-pinos.h"

#define PINOS_DAEMON_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_DAEMON, PinosDaemonPrivate))

struct _PinosDaemonPrivate
{
  PinosDaemon *daemon;

  PinosDaemon1 *iface;
  guint id;
  GDBusConnection *connection;
  GDBusObjectManagerServer *server_manager;

  gchar *object_path;

  GHashTable *clients;
  PinosDataLoop *data_loop;
  PinosMainLoop *main_loop;

  PinosProperties *properties;

  PinosListener object_added;
  PinosListener object_removed;

  GHashTable *node_factories;

  SpaSupport support[4];
};

enum
{
  PROP_0,
  PROP_CONNECTION,
  PROP_PROPERTIES,
  PROP_OBJECT_PATH,
};

static void try_link_port (PinosNode *node, PinosPort *port, PinosDaemon *this);

static void
handle_client_appeared (PinosClient *client, gpointer user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;

  pinos_log_debug ("daemon %p: appeared %p", daemon, client);

  g_hash_table_insert (priv->clients, (gpointer) pinos_client_get_sender (client), client);
}

static void
handle_client_vanished (PinosClient *client, gpointer user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;

  pinos_log_debug ("daemon %p: vanished %p", daemon, client);
  g_hash_table_remove (priv->clients, (gpointer) pinos_client_get_sender (client));
}

static PinosClient *
sender_get_client (PinosDaemon *daemon,
                   const gchar *sender,
                   gboolean create)
{
  PinosDaemonPrivate *priv = daemon->priv;
  PinosClient *client;

  client = g_hash_table_lookup (priv->clients, sender);
  if (client == NULL && create) {
    client = pinos_client_new (daemon, sender, NULL);

    pinos_log_debug ("daemon %p: new client %p for %s", daemon, client, sender);
    g_signal_connect (client,
                      "appeared",
                      (GCallback) handle_client_appeared,
                      daemon);
    g_signal_connect (client,
                      "vanished",
                      (GCallback) handle_client_vanished,
                      daemon);
  }
  return client;
}

static void
handle_remove_node (PinosNode *node,
                    gpointer   user_data)
{
  PinosClient *client = user_data;

  pinos_log_debug ("client %p: node %p remove", daemon, node);
  pinos_client_remove_object (client, G_OBJECT (node));
}

static gboolean
handle_create_node (PinosDaemon1           *interface,
                    GDBusMethodInvocation  *invocation,
                    const gchar            *arg_factory_name,
                    const gchar            *arg_name,
                    GVariant               *arg_properties,
                    gpointer                user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;
  PinosNodeFactory *factory;
  PinosNode *node;
  PinosClient *client;
  const gchar *sender, *object_path;
  PinosProperties *props;

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (daemon, sender, TRUE);

  pinos_log_debug ("daemon %p: create node: %s", daemon, sender);

  props = pinos_properties_from_variant (arg_properties);

  factory = g_hash_table_lookup (priv->node_factories, arg_factory_name);
  if (factory == NULL)
    goto no_factory;

  node = pinos_node_factory_create_node (factory,
                                         client,
                                         arg_name,
                                         props);
  pinos_properties_free (props);

  if (node == NULL)
    goto no_node;

  pinos_client_add_object (client, G_OBJECT (node));

  g_signal_connect (node,
                    "remove",
                    (GCallback) handle_remove_node,
                    client);

  object_path = pinos_node_get_object_path (node);
  pinos_log_debug ("daemon %p: added node %p with path %s", daemon, node, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));
  g_object_unref (node);

  return TRUE;

  /* ERRORS */
no_factory:
  {
    pinos_log_debug ("daemon %p: could find factory named %s", daemon, arg_factory_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't find factory");
    return TRUE;
  }
no_node:
  {
    pinos_log_debug ("daemon %p: could create node named %s from factory %s", daemon, arg_name, arg_factory_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't create node");
    return TRUE;
  }
}


static void
on_link_port_unlinked (PinosLink   *link,
                       PinosPort   *port,
                       PinosDaemon *this)
{
  pinos_log_debug ("daemon %p: link %p: port %p unlinked", this, link, port);

  if (port->direction == PINOS_DIRECTION_OUTPUT && link->input)
    try_link_port (link->input->node, link->input, this);
}

static void
on_link_state_notify (GObject     *obj,
                      GParamSpec  *pspec,
                      PinosDaemon *daemon)
{
  PinosLink *link = PINOS_LINK (obj);
  GError *error = NULL;
  PinosLinkState state;

  state = pinos_link_get_state (link, &error);
  switch (state) {
    case PINOS_LINK_STATE_ERROR:
    {
      pinos_log_debug ("daemon %p: link %p: state error: %s", daemon, link, error->message);

      if (link->input && link->input->node)
        pinos_node_report_error (link->input->node, g_error_copy (error));
      if (link->output && link->output->node)
        pinos_node_report_error (link->output->node, g_error_copy (error));
      break;
    }

    case PINOS_LINK_STATE_UNLINKED:
      pinos_log_debug ("daemon %p: link %p: unlinked", daemon, link);

#if 0
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_NODE_LINK,
                   "error node unlinked");

      if (link->input && link->input->node)
        pinos_node_report_error (link->input->node, g_error_copy (error));
      if (link->output && link->output->node)
        pinos_node_report_error (link->output->node, g_error_copy (error));
#endif
      break;

    case PINOS_LINK_STATE_INIT:
    case PINOS_LINK_STATE_NEGOTIATING:
    case PINOS_LINK_STATE_ALLOCATING:
    case PINOS_LINK_STATE_PAUSED:
    case PINOS_LINK_STATE_RUNNING:
      break;
  }
}

static void
try_link_port (PinosNode *node, PinosPort *port, PinosDaemon *this)
{
  PinosClient *client;
  PinosProperties *props;
  const gchar *path;
  GError *error = NULL;
  PinosLink *link;

  props = pinos_node_get_properties (node);
  if (props == NULL)
    return;

  path = pinos_properties_get (props, "pinos.target.node");

  if (path) {
    PinosPort *target;

    target = pinos_daemon_find_port (this,
                                     port,
                                     path,
                                     NULL,
                                     NULL,
                                     &error);
    if (target == NULL)
      goto error;

    if (port->direction == PINOS_DIRECTION_OUTPUT)
      link = pinos_port_link (port, target, NULL, NULL, &error);
    else
      link = pinos_port_link (target, port, NULL, NULL, &error);

    if (link == NULL)
      goto error;

    client = pinos_node_get_client (node);
    if (client)
      pinos_client_add_object (client, G_OBJECT (link));

    g_signal_connect (link, "port-unlinked", (GCallback) on_link_port_unlinked, this);
    g_signal_connect (link, "notify::state", (GCallback) on_link_state_notify, this);
    pinos_link_activate (link);

    g_object_unref (link);
  }
  return;

error:
  {
    pinos_node_report_error (node, error);
    return;
  }
}

static void
on_port_added (PinosNode *node, PinosPort *port, PinosDaemon *this)
{
  try_link_port (node, port, this);
}

static void
on_port_removed (PinosNode *node, PinosPort *port, PinosDaemon *this)
{
}

static void
on_node_created (PinosNode      *node,
                 PinosDaemon    *this)
{
  GList *ports, *walk;

  ports = pinos_node_get_ports (node, PINOS_DIRECTION_INPUT);
  for (walk = ports; walk; walk = g_list_next (walk))
    on_port_added (node, walk->data, this);

  ports = pinos_node_get_ports (node, PINOS_DIRECTION_OUTPUT);
  for (walk = ports; walk; walk = g_list_next (walk))
    on_port_added (node, walk->data, this);

  g_signal_connect (node, "port-added", (GCallback) on_port_added, this);
  g_signal_connect (node, "port-removed", (GCallback) on_port_removed, this);
}


static void
on_node_state_change (PinosNode      *node,
                      PinosNodeState  old,
                      PinosNodeState  state,
                      PinosDaemon    *this)
{
  pinos_log_debug ("daemon %p: node %p state change %s -> %s", this, node,
                        pinos_node_state_as_string (old),
                        pinos_node_state_as_string (state));

  if (old == PINOS_NODE_STATE_CREATING && state == PINOS_NODE_STATE_SUSPENDED)
    on_node_created (node, this);
}

static void
on_node_added (PinosDaemon *daemon, PinosNode *node)
{
  PinosNodeState state;
  PinosDaemonPrivate *priv = daemon->priv;

  pinos_log_debug ("daemon %p: node %p added", daemon, node);

  g_object_set (node, "data-loop", priv->data_loop, NULL);

  g_signal_connect (node, "state-change", (GCallback) on_node_state_change, daemon);

  state = pinos_node_get_state (node);
  if (state > PINOS_NODE_STATE_CREATING) {
    on_node_created (node, daemon);
  }
}

static void
on_node_removed (PinosDaemon *daemon, PinosNode *node)
{
  pinos_log_debug ("daemon %p: node %p removed", daemon, node);
  g_signal_handlers_disconnect_by_data (node, daemon);
}

static gboolean
handle_create_client_node (PinosDaemon1           *interface,
                           GDBusMethodInvocation  *invocation,
                           const gchar            *arg_name,
                           GVariant               *arg_properties,
                           gpointer                user_data)
{
  PinosDaemon *daemon = user_data;
  PinosNode *node;
  PinosClient *client;
  const gchar *sender, *object_path;
  PinosProperties *props;
  GError *error = NULL;
  GUnixFDList *fdlist;
  GSocket *socket, *rtsocket;
  gint fdidx, rtfdidx;

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (daemon, sender, TRUE);

  pinos_log_debug ("daemon %p: create client-node: %s", daemon, sender);
  props = pinos_properties_from_variant (arg_properties);

  node =  pinos_client_node_new (daemon,
                                 client,
                                 arg_name,
                                 props);
  pinos_properties_free (props);

  socket = pinos_client_node_get_socket_pair (PINOS_CLIENT_NODE (node), &error);
  if (socket == NULL)
    goto no_socket;

  rtsocket = pinos_client_node_get_rtsocket_pair (PINOS_CLIENT_NODE (node), &error);
  if (rtsocket == NULL)
    goto no_socket;

  pinos_client_add_object (client, G_OBJECT (node));

  object_path = pinos_node_get_object_path (PINOS_NODE (node));
  pinos_log_debug ("daemon %p: add client-node %p, %s", daemon, node, object_path);
  g_object_unref (node);

  fdlist = g_unix_fd_list_new ();
  fdidx = g_unix_fd_list_append (fdlist, g_socket_get_fd (socket), &error);
  rtfdidx = g_unix_fd_list_append (fdlist, g_socket_get_fd (rtsocket), &error);
  g_object_unref (socket);
  g_object_unref (rtsocket);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
           g_variant_new ("(ohh)", object_path, fdidx, rtfdidx), fdlist);
  g_object_unref (fdlist);

  return TRUE;

no_socket:
  {
    pinos_log_debug ("daemon %p: could not create socket %s", daemon, error->message);
    g_object_unref (node);
    goto exit_error;
  }
exit_error:
  {
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_clear_error (&error);
    return TRUE;
  }
}

static void
export_server_object (PinosDaemon              *daemon,
                      GDBusObjectManagerServer *manager)
{
  PinosDaemonPrivate *priv = daemon->priv;
  PinosObjectSkeleton *skel;

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_SERVER);

  pinos_object_skeleton_set_daemon1 (skel, priv->iface);

  g_dbus_object_manager_server_export (manager, G_DBUS_OBJECT_SKELETON (skel));
  g_free (priv->object_path);
  priv->object_path = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (skel)));
  g_object_unref (skel);
}

static void
bus_acquired_handler (GDBusConnection *connection,
                      const gchar     *name,
                      gpointer         user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;
  GDBusObjectManagerServer *manager = priv->server_manager;

  priv->connection = connection;

  export_server_object (daemon, manager);

  g_dbus_object_manager_server_set_connection (manager, connection);
}

static void
name_acquired_handler (GDBusConnection *connection,
                       const gchar     *name,
                       gpointer         user_data)
{
}

static void
name_lost_handler (GDBusConnection *connection,
                   const gchar     *name,
                   gpointer         user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;
  GDBusObjectManagerServer *manager = priv->server_manager;

  g_dbus_object_manager_server_unexport (manager, PINOS_DBUS_OBJECT_SERVER);
  g_dbus_object_manager_server_set_connection (manager, connection);
  g_clear_pointer (&priv->object_path, g_free);
  priv->connection = connection;
}

/**
 * pinos_daemon_new:
 * @properties: #PinosProperties
 *
 * Make a new #PinosDaemon object with given @properties
 *
 * Returns: a new #PinosDaemon
 */
PinosDaemon *
pinos_daemon_new (PinosProperties *properties)
{
  return g_object_new (PINOS_TYPE_DAEMON, "properties", properties, NULL);
}

const gchar *
pinos_daemon_get_object_path (PinosDaemon *daemon)
{
  PinosDaemonPrivate *priv;

  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);
  priv = daemon->priv;

  return priv->object_path;
}

/**
 * pinos_daemon_start:
 * @daemon: a #PinosDaemon
 *
 * Start the @daemon.
 */
void
pinos_daemon_start (PinosDaemon *daemon)
{
  PinosDaemonPrivate *priv;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));

  priv = daemon->priv;
  g_return_if_fail (priv->id == 0);

  pinos_log_debug ("daemon %p: start", daemon);

  priv->id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             PINOS_DBUS_SERVICE,
                             G_BUS_NAME_OWNER_FLAGS_REPLACE,
                             bus_acquired_handler,
                             name_acquired_handler,
                             name_lost_handler,
                             daemon,
                             NULL);
}

/**
 * pinos_daemon_stop:
 * @daemon: a #PinosDaemon
 *
 * Stop the @daemon.
 */
void
pinos_daemon_stop (PinosDaemon *daemon)
{
  PinosDaemonPrivate *priv = daemon->priv;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));

  pinos_log_debug ("daemon %p: stop", daemon);

  if (priv->id != 0) {
    g_bus_unown_name (priv->id);
    priv->id = 0;
  }
}

/**
 * pinos_daemon_export_uniquely:
 * @daemon: a #PinosDaemon
 * @skel: a #GDBusObjectSkeleton
 *
 * Export @skel with @daemon with a unique name
 *
 * Returns: the unique named used to export @skel.
 */
gchar *
pinos_daemon_export_uniquely (PinosDaemon         *daemon,
                              GDBusObjectSkeleton *skel)
{
  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (G_IS_DBUS_OBJECT_SKELETON (skel), NULL);

  g_dbus_object_manager_server_export_uniquely (daemon->priv->server_manager, skel);

  return g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (skel)));
}

/**
 * pinos_daemon_unexport:
 * @daemon: a #PinosDaemon
 * @object_path: an object path
 *
 * Unexport the object on @object_path
 */
void
pinos_daemon_unexport (PinosDaemon *daemon,
                       const gchar *object_path)
{
  g_return_if_fail (PINOS_IS_DAEMON (daemon));
  g_return_if_fail (g_variant_is_object_path (object_path));

  g_dbus_object_manager_server_unexport (daemon->priv->server_manager, object_path);
}

static void
on_registry_object_added (PinosListener *listener,
                          void          *data)
{
  PinosObject *object = data;
  PinosDaemonPrivate *priv = SPA_CONTAINER_OF (listener, PinosDaemonPrivate, object_added);
  PinosDaemon *daemon = priv->daemon;

  if (object->type == daemon->registry.uri.node) {
    PinosNode *node = object->implementation;

    on_node_added (daemon, node);
  } else if (object->type == daemon->registry.uri.node_factory) {
    PinosNodeFactory *factory = object->implementation;
    gchar *name;

    g_object_get (factory, "name", &name, NULL);
    g_hash_table_insert (priv->node_factories, name, g_object_ref (factory));
  }
}

static void
on_registry_object_removed (PinosListener *listener,
                            void          *data)
{
  PinosObject *object = data;
  PinosDaemonPrivate *priv = SPA_CONTAINER_OF (listener, PinosDaemonPrivate, object_added);
  PinosDaemon *daemon = priv->daemon;

  if (object->type == daemon->registry.uri.node) {
    PinosNode *node = object->implementation;

    on_node_removed (daemon, node);
  } else if (object->type == daemon->registry.uri.node_factory) {
    PinosNodeFactory *factory = object->implementation;
    gchar *name;

    g_object_get (factory, "name", &name, NULL);
    g_hash_table_remove (priv->node_factories, name);
    g_free (name);
  }
}

/**
 * pinos_daemon_find_port:
 * @daemon: a #PinosDaemon
 * @other_port: a port to be compatible with
 * @name: a port name
 * @props: port properties
 * @format_filter: a format filter
 * @error: location for an error
 *
 * Find the best port in @daemon that matches the given parameters.
 *
 * Returns: a #PinosPort or %NULL when no port could be found.
 */
PinosPort *
pinos_daemon_find_port (PinosDaemon     *daemon,
                        PinosPort       *other_port,
                        const gchar     *name,
                        PinosProperties *props,
                        GPtrArray       *format_filters,
                        GError         **error)
{
  PinosPort *best = NULL;
  gboolean have_name;
  guint i;

  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

  have_name = name ? strlen (name) > 0 : FALSE;

  pinos_log_debug ("name \"%s\", %d", name, have_name);

  for (i = 0; i < pinos_map_get_size (&daemon->registry.objects); i++) {
    PinosObject *o = pinos_map_lookup (&daemon->registry.objects, i);
    PinosNode *n;

    if (o->type != daemon->registry.uri.node)
      continue;

    n = o->implementation;
    if (n == NULL || n->flags & PINOS_NODE_FLAG_REMOVING)
      continue;

    pinos_log_debug ("node path \"%s\"", pinos_node_get_object_path (n));

    if (have_name) {
      if (g_str_has_suffix (pinos_node_get_object_path (n), name)) {
        pinos_log_debug ("name \"%s\" matches node %p", name, n);

        best = pinos_node_get_free_port (n, pinos_direction_reverse (other_port->direction));
        if (best)
          break;
      }
    } else {
    }
  }
  if (best == NULL) {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_NOT_FOUND,
                 "No matching Node found");
  }
  return best;
}


G_DEFINE_TYPE (PinosDaemon, pinos_daemon, G_TYPE_OBJECT);

static void
pinos_daemon_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosDaemon *this = PINOS_DAEMON (_object);
  PinosDaemonPrivate *priv = this->priv;

  switch (prop_id) {
    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    case PROP_CONNECTION:
      g_value_set_object (value, priv->connection);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (this, prop_id, pspec);
      break;
  }
}

static void
pinos_daemon_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosDaemon *this = PINOS_DAEMON (_object);
  PinosDaemonPrivate *priv = this->priv;

  switch (prop_id) {
    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (this, prop_id, pspec);
      break;
  }
}

static void
pinos_daemon_constructed (GObject * object)
{
  PinosDaemon *this = PINOS_DAEMON_CAST (object);
  PinosDaemonPrivate *priv = this->priv;

  pinos_log_debug ("daemon %p: constructed", object);
  pinos_daemon1_set_user_name (priv->iface, g_get_user_name ());
  pinos_daemon1_set_host_name (priv->iface, g_get_host_name ());
  pinos_daemon1_set_version (priv->iface, PACKAGE_VERSION);
  pinos_daemon1_set_name (priv->iface, PACKAGE_NAME);
  pinos_daemon1_set_cookie (priv->iface, g_random_int());
  pinos_daemon1_set_properties (priv->iface, pinos_properties_to_variant (priv->properties));

  G_OBJECT_CLASS (pinos_daemon_parent_class)->constructed (object);

  pinos_object_init (&this->object,
                     this->registry.uri.daemon,
                     this,
                     NULL);
  pinos_registry_add_object (&this->registry, &this->object);
}

static void
pinos_daemon_dispose (GObject * object)
{
  PinosDaemon *this = PINOS_DAEMON_CAST (object);

  pinos_log_debug ("daemon %p: dispose", object);

  pinos_daemon_stop (this);


  G_OBJECT_CLASS (pinos_daemon_parent_class)->dispose (object);
}

static void
pinos_daemon_finalize (GObject * object)
{
  PinosDaemon *this = PINOS_DAEMON_CAST (object);
  PinosDaemonPrivate *priv = this->priv;

  pinos_log_debug ("daemon %p: finalize", object);

  pinos_registry_remove_object (&this->registry, &this->object);

  g_clear_object (&priv->server_manager);
  g_clear_object (&priv->iface);
  g_clear_object (&priv->data_loop);
  g_hash_table_unref (priv->clients);
  g_hash_table_unref (priv->node_factories);

  G_OBJECT_CLASS (pinos_daemon_parent_class)->finalize (object);
}

static void
pinos_daemon_class_init (PinosDaemonClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosDaemonPrivate));

  gobject_class->constructed = pinos_daemon_constructed;
  gobject_class->dispose = pinos_daemon_dispose;
  gobject_class->finalize = pinos_daemon_finalize;

  gobject_class->set_property = pinos_daemon_set_property;
  gobject_class->get_property = pinos_daemon_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_CONNECTION,
                                   g_param_spec_object ("connection",
                                                        "Connection",
                                                        "The DBus connection",
                                                        G_TYPE_DBUS_CONNECTION,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "Client properties",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_PATH,
                                   g_param_spec_string ("object-path",
                                                        "Object Path",
                                                        "The object path",
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

}

static void
pinos_daemon_init (PinosDaemon * daemon)
{
  PinosDaemonPrivate *priv = daemon->priv = PINOS_DAEMON_GET_PRIVATE (daemon);

  priv->daemon = daemon;

  pinos_log_debug ("daemon %p: new", daemon);
  priv->iface = pinos_daemon1_skeleton_new ();
  g_signal_connect (priv->iface, "handle-create-node", (GCallback) handle_create_node, daemon);
  g_signal_connect (priv->iface, "handle-create-client-node", (GCallback) handle_create_client_node, daemon);

  pinos_registry_init (&daemon->registry);

  priv->object_added.notify = on_registry_object_added;
  pinos_signal_add (&daemon->registry.object_added, &priv->object_added);

  priv->object_removed.notify = on_registry_object_removed;
  pinos_signal_add (&daemon->registry.object_removed, &priv->object_removed);

  priv->server_manager = g_dbus_object_manager_server_new (PINOS_DBUS_OBJECT_PREFIX);
  priv->clients = g_hash_table_new (g_str_hash, g_str_equal);
  priv->node_factories = g_hash_table_new_full (g_str_hash,
                                                g_str_equal,
                                                g_free,
                                                g_object_unref);

  daemon->main_loop = pinos_main_loop_new (g_main_context_get_thread_default ());
  priv->data_loop = pinos_data_loop_new();

  daemon->log = pinos_log_get();

  priv->support[0].uri = SPA_ID_MAP_URI;
  priv->support[0].data = daemon->registry.map;
  priv->support[1].uri = SPA_LOG_URI;
  priv->support[1].data = daemon->log;
  priv->support[2].uri = SPA_POLL__DataLoop;
  priv->support[2].data = &priv->data_loop->poll;
  priv->support[3].uri = SPA_POLL__MainLoop;
  priv->support[3].data = &daemon->main_loop->poll;
  daemon->support = priv->support;
  daemon->n_support = 4;
}
