/*
 *  Copyright © 2011 Igalia S.L.
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

#if !defined (__EPHY_EPIPHANY_H_INSIDE__) && !defined (EPIPHANY_COMPILATION)
#error "Only <epiphany/epiphany.h> can be included directly."
#endif

#ifndef EPHY_WEB_APP_UTILS_H
#define EPHY_WEB_APP_UTILS_H

#include <glib.h>
#include <gtk/gtk.h>

#include "ephy-web-view.h"

G_BEGIN_DECLS

typedef struct {
    char *name;
    char *icon_url;
    char *url;
    char install_date[128];
} EphyWebApplication;

#define EPHY_WEB_APP_PREFIX "app-"

char    *ephy_web_application_create (EphyWebView *view, const char *title, GdkPixbuf *icon);

gboolean ephy_web_application_delete (const char *name);

char    *ephy_web_application_get_profile_directory (const char *app_name);

GList   *ephy_web_application_get_application_list (void);

void     ephy_web_application_free_application_list (GList *list);

G_END_DECLS

#endif
