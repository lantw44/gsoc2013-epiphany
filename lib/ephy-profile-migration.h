/*
 *  Copyright © 2009 Xan López
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#ifndef EPHY_PROFILE_MIGRATION_H
#define EPHY_PROFILE_MIGRATION_H

#include <glib.h>
#include <gnome-keyring.h>

#define FORM_USERNAME_KEY "form_username"
#define FORM_PASSWORD_KEY "form_password"

void _ephy_profile_migrate (void);

void _ephy_profile_store_form_auth_data (const char *uri,
                                         const char *form_username,
                                         const char *form_password,
                                         const char *username,
                                         const char *password);

GList*
_ephy_profile_query_form_auth_data (const char *uri,
                                    const char *form_username,
                                    const char *form_password,
                                    GnomeKeyringOperationGetListCallback callback,
                                    gpointer data,
                                    GDestroyNotify destroy_data);

#endif