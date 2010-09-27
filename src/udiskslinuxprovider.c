/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"

#include "udisksdaemon.h"
#include "udisksprovider.h"
#include "udiskslinuxprovider.h"
#include "udiskslinuxblock.h"

/**
 * SECTION:udiskslinuxprovider
 * @title: UDisksLinuxProvider
 * @short_description: Provider of Linux-specific objects
 *
 * This object is used to add/remove Linux specific objects. Right now
 * it only handles #UDisksLinuxBlock devices.
 */

typedef struct _UDisksLinuxProviderClass   UDisksLinuxProviderClass;

/**
 * UDisksLinuxProvider:
 *
 * The #UDisksLinuxProvider structure contains only private data and
 * should only be accessed using the provided API.
 */
struct _UDisksLinuxProvider
{
  UDisksProvider parent_instance;

  GUdevClient *gudev_client;

  /* maps from sysfs path to LinuxBlock instance */
  GHashTable *sysfs_to_block;
};

struct _UDisksLinuxProviderClass
{
  UDisksProviderClass parent_class;
};

static void
udisks_linux_provider_handle_uevent (UDisksLinuxProvider *provider,
                                     const gchar         *action,
                                     GUdevDevice         *device);

G_DEFINE_TYPE (UDisksLinuxProvider, udisks_linux_provider, UDISKS_TYPE_PROVIDER);

static void
udisks_linux_provider_finalize (GObject *object)
{
  UDisksLinuxProvider *provider = UDISKS_LINUX_PROVIDER (object);

  g_hash_table_unref (provider->sysfs_to_block);
  g_object_unref (provider->gudev_client);

  if (G_OBJECT_CLASS (udisks_linux_provider_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_linux_provider_parent_class)->finalize (object);
}

static void
udisks_linux_provider_init (UDisksLinuxProvider *provider)
{
}

static void
on_uevent (GUdevClient  *client,
           const gchar  *action,
           GUdevDevice  *device,
           gpointer      user_data)
{
  UDisksLinuxProvider *provider = UDISKS_LINUX_PROVIDER (user_data);
  g_print ("%s:%s: entering\n", G_STRLOC, G_STRFUNC);
  udisks_linux_provider_handle_uevent (provider, action, device);
}

static void
udisks_linux_provider_constructed (GObject *object)
{
  UDisksLinuxProvider *provider = UDISKS_LINUX_PROVIDER (object);
  const gchar *subsystems[] = {"block", NULL};
  GList *devices;
  GList *l;

  /* get ourselves an udev client */
  provider->gudev_client = g_udev_client_new (subsystems);
  g_signal_connect (provider->gudev_client,
                    "uevent",
                    G_CALLBACK (on_uevent),
                    provider);

  provider->sysfs_to_block = g_hash_table_new_full (g_str_hash,
                                                    g_str_equal,
                                                    g_free,
                                                    (GDestroyNotify) g_object_unref);

  /* TODO: maybe do two loops to properly handle dependency SNAFU? */
  devices = g_udev_client_query_by_subsystem (provider->gudev_client, "block");
  for (l = devices; l != NULL; l = l->next)
    udisks_linux_provider_handle_uevent (provider, "add", G_UDEV_DEVICE (l->data));
  g_list_foreach (devices, (GFunc) g_object_unref, NULL);
  g_list_free (devices);

  if (G_OBJECT_CLASS (udisks_linux_provider_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (udisks_linux_provider_parent_class)->constructed (object);
}


static void
udisks_linux_provider_class_init (UDisksLinuxProviderClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_linux_provider_finalize;
  gobject_class->constructed  = udisks_linux_provider_constructed;
}

/**
 * udisks_linux_provider_new:
 * @daemon: A #UDisksDaemon.
 *
 * Create a new provider object for Linux-specific objects / functionality.
 *
 * Returns: A #UDisksLinuxProvider object. Free with g_object_unref().
 */
UDisksLinuxProvider *
udisks_linux_provider_new (UDisksDaemon  *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return UDISKS_LINUX_PROVIDER (g_object_new (UDISKS_TYPE_LINUX_PROVIDER,
                                              "daemon", daemon,
                                              NULL));
}

/**
 * udisks_linux_provider_get_udev_client:
 * @provider: A #UDisksLinuxProvider.
 *
 * Gets the #GUdevClient used by @provider. The returned object is set
 * up so it emits #GUdevClient::uevent signals only for the
 * <literal>block</literal> subsystems.
 *
 * Returns: A #GUdevClient owned by @provider. Do not free.
 */
GUdevClient *
udisks_linux_provider_get_udev_client (UDisksLinuxProvider *provider)
{
  g_return_val_if_fail (UDISKS_IS_LINUX_PROVIDER (provider), NULL);
  return provider->gudev_client;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_provider_handle_uevent (UDisksLinuxProvider *provider,
                                     const gchar         *action,
                                     GUdevDevice         *device)
{
  const gchar *sysfs_path;
  UDisksLinuxBlock *block;

  g_print ("%s:%s: %s %s\n", G_STRLOC, G_STRFUNC,
           action, g_udev_device_get_sysfs_path (device));

  sysfs_path = g_udev_device_get_sysfs_path (device);
  if (g_strcmp0 (action, "remove") == 0)
    {
      block = g_hash_table_lookup (provider->sysfs_to_block, sysfs_path);
      if (block != NULL)
        {
          g_dbus_object_manager_unexport (udisks_daemon_get_object_manager (udisks_provider_get_daemon (UDISKS_PROVIDER (provider))),
                                          g_dbus_object_get_object_path (G_DBUS_OBJECT (block)));
          g_warn_if_fail (g_hash_table_remove (provider->sysfs_to_block, sysfs_path));
        }
    }
  else
    {
      block = g_hash_table_lookup (provider->sysfs_to_block, sysfs_path);
      if (block != NULL)
        {
          udisks_linux_block_uevent (block, action, device);
        }
      else
        {
          block = udisks_linux_block_new (udisks_provider_get_daemon (UDISKS_PROVIDER (provider)), device);
          g_dbus_object_manager_export (udisks_daemon_get_object_manager (udisks_provider_get_daemon (UDISKS_PROVIDER (provider))),
                                        G_DBUS_OBJECT (block));
          g_hash_table_insert (provider->sysfs_to_block, g_strdup (sysfs_path), block);
        }
    }
}
