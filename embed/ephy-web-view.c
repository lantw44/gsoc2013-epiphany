/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2008 Gustavo Noronha Silva
 *  Copyright © 2009 Igalia S.L.
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

#include "config.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <string.h>
#include <webkit/webkit.h>

#include "eel-gconf-extensions.h"
#include "ephy-base-embed.h"
#include "ephy-debug.h"
#include "ephy-embed.h"
#include "ephy-embed-container.h"
#include "ephy-embed-prefs.h"
#include "ephy-embed-shell.h"
#include "ephy-embed-single.h"
#include "ephy-embed-type-builtins.h"
#include "ephy-embed-utils.h"
#include "ephy-marshal.h"
#include "ephy-permission-manager.h"
#include "ephy-favicon-cache.h"
#include "ephy-history.h"
#include "ephy-string.h"
#include "ephy-web-view.h"
#include "ephy-zoom.h"

static void     ephy_web_view_class_init   (EphyWebViewClass *klass);
static void     ephy_web_view_init         (EphyWebView *gs);

#define MAX_HIDDEN_POPUPS       5
#define MAX_TITLE_LENGTH        512 /* characters */
#define RELOAD_DELAY            250 /* ms */
#define RELOAD_DELAY_MAX_TICKS  40  /* RELOAD_DELAY * RELOAD_DELAY_MAX_TICKS = 10 s */
#define EMPTY_PAGE              _("Blank page") /* Title for the empty page */

struct _EphyWebViewPrivate {
  EphyWebViewAddressExpire address_expire;
  EphyWebViewSecurityLevel security_level;
  EphyWebViewDocumentType document_type;
  EphyWebViewNavigationFlags nav_flags;

  /* Flags */
  guint is_blank : 1;
  guint is_loading : 1;
  guint visibility : 1;

  char *address;
  char *typed_address;
  char *title;
  int cur_requests;
  int total_requests;
  gint8 load_percent;
  char *loading_title;
  char *status_message;
  char *link_message;
  char *icon_address;
  GdkPixbuf *icon;

  /* File watch */
  GFileMonitor *monitor;
  gboolean monitor_directory;
  guint reload_scheduled_id;
  guint reload_delay_ticks;

  GSList *hidden_popups;
  GSList *shown_popups;
};

typedef struct {
  char *url;
  char *name;
  char *features;
} PopupInfo;

enum {
  PROP_0,
  PROP_ADDRESS,
  PROP_DOCUMENT_TYPE,
  PROP_HIDDEN_POPUP_COUNT,
  PROP_ICON,
  PROP_ICON_ADDRESS,
  PROP_LINK_MESSAGE,
  PROP_LOAD_PROGRESS,
  PROP_LOAD_STATUS,
  PROP_NAVIGATION,
  PROP_POPUPS_ALLOWED,
  PROP_SECURITY,
  PROP_STATUS_MESSAGE,
  PROP_EMBED_TITLE,
  PROP_TYPED_ADDRESS,
  PROP_VISIBLE,
};

#define EPHY_WEB_VIEW_GET_PRIVATE(object) (G_TYPE_INSTANCE_GET_PRIVATE ((object), EPHY_TYPE_WEB_VIEW, EphyWebViewPrivate))

G_DEFINE_TYPE (EphyWebView, ephy_web_view, WEBKIT_TYPE_WEB_VIEW)

static guint
popup_blocker_n_hidden (EphyWebView *view)
{
  return g_slist_length (view->priv->hidden_popups);
}

static void
popups_manager_free_info (PopupInfo *popup)
{
  g_free (popup->url);
  g_free (popup->name);
  g_free (popup->features);
  g_slice_free (PopupInfo, popup);
}

static void
popups_manager_show (PopupInfo *popup,
                     EphyWebView *view)
{
  EphyEmbedSingle *single;

  /* Only show popup with non NULL url */
  if (popup->url != NULL) {
    single = EPHY_EMBED_SINGLE
             (ephy_embed_shell_get_embed_single (embed_shell));

    ephy_embed_single_open_window (single, EPHY_EMBED (view), popup->url,
                                   popup->name, popup->features);
  }
  popups_manager_free_info (popup);
}

static void
popups_manager_show_all (EphyWebView *view)
{
  LOG ("popup_blocker_show_all: view %p", view);

  g_slist_foreach (view->priv->hidden_popups,
                   (GFunc)popups_manager_show, view);
  g_slist_free (view->priv->hidden_popups);
  view->priv->hidden_popups = NULL;

  g_object_notify (G_OBJECT (view), "hidden-popup-count");
}

static char *
popups_manager_new_window_info (EphyEmbedContainer *container)
{
  EphyEmbed *embed;
  EphyWebViewChrome chrome;
  gboolean is_popup;
  char *features;

  g_object_get (container, "chrome", &chrome, "is-popup", &is_popup, NULL);
  g_return_val_if_fail (is_popup, g_strdup (""));

  embed = ephy_embed_container_get_active_child (container);
  g_return_val_if_fail (embed != NULL, g_strdup (""));

  features = g_strdup_printf
             ("width=%d,height=%d,menubar=%d,status=%d,toolbar=%d",
              GTK_WIDGET (embed)->allocation.width,
              GTK_WIDGET (embed)->allocation.height,
              (chrome & EPHY_WEB_VIEW_CHROME_MENUBAR) > 0,
              (chrome & EPHY_WEB_VIEW_CHROME_STATUSBAR) > 0,
              (chrome & EPHY_WEB_VIEW_CHROME_TOOLBAR) > 0);

  return features;
}

static void
popups_manager_add (EphyWebView *view,
                    const char *url,
                    const char *name,
                    const char *features)
{
  EphyWebViewPrivate *priv = view->priv;
  PopupInfo *popup;

  LOG ("popups_manager_add: view %p, url %s, features %s",
       view, url, features);

  popup = g_slice_new (PopupInfo);

  popup->url = g_strdup (url);
  popup->name = g_strdup (name);
  popup->features = g_strdup (features);

  priv->hidden_popups = g_slist_prepend (priv->hidden_popups, popup);

  if (popup_blocker_n_hidden (view) > MAX_HIDDEN_POPUPS) {/* bug #160863 */
    /* Remove the oldest popup */
    GSList *l = view->priv->hidden_popups;

    while (l->next->next != NULL) {
      l = l->next;
    }

    popup = (PopupInfo *)l->next->data;
    popups_manager_free_info (popup);

    l->next = NULL;
  } else {
    g_object_notify (G_OBJECT (view), "hidden-popup-count");
  }
}

static void
popups_manager_hide (EphyEmbedContainer *container,
                     EphyWebView *parent_view)
{
  EphyEmbed *embed;
  char *location;
  char *features;

  embed = ephy_embed_container_get_active_child (container);
  g_return_if_fail (EPHY_IS_EMBED (embed));

  location = ephy_web_view_get_location (EPHY_GET_EPHY_WEB_VIEW_FROM_EMBED (embed), TRUE);
  if (location == NULL) return;

  features = popups_manager_new_window_info (container);

  popups_manager_add (parent_view, location, "" /* FIXME? maybe _blank? */, features);

  gtk_widget_destroy (GTK_WIDGET (container));

  g_free (location);
  g_free (features);
}

static void
popups_manager_hide_all (EphyWebView *view)
{
  LOG ("popup_blocker_hide_all: view %p", view);

  g_slist_foreach (view->priv->shown_popups,
                   (GFunc)popups_manager_hide, view);
  g_slist_free (view->priv->shown_popups);
  view->priv->shown_popups = NULL;
}

static void
ephy_web_view_set_popups_allowed (EphyWebView *view,
                                  gboolean allowed)
{
  char *location;
  EphyPermissionManager *manager;
  EphyPermission permission;

  location = ephy_web_view_get_location (view, TRUE);
  g_return_if_fail (location != NULL);

  manager = EPHY_PERMISSION_MANAGER
            (ephy_embed_shell_get_embed_single (embed_shell));
  g_return_if_fail (EPHY_IS_PERMISSION_MANAGER (manager));

  permission = allowed ? EPHY_PERMISSION_ALLOWED
               : EPHY_PERMISSION_DENIED;

  ephy_permission_manager_add_permission (manager, location, EPT_POPUP, permission);

  if (allowed) {
    popups_manager_show_all (view);
  } else {
    popups_manager_hide_all (view);
  }

  g_free (location);
}

static gboolean
ephy_web_view_get_popups_allowed (EphyWebView *view)
{
  EphyPermissionManager *permission_manager;
  EphyPermission response;
  char *location;
  gboolean allow;

  permission_manager = EPHY_PERMISSION_MANAGER
                       (ephy_embed_shell_get_embed_single (embed_shell));
  g_return_val_if_fail (EPHY_IS_PERMISSION_MANAGER (permission_manager),
                        FALSE);

  location = ephy_web_view_get_location (view, TRUE);
  if (location == NULL) return FALSE;/* FALSE, TRUE… same thing */

  response = ephy_permission_manager_test_permission
             (permission_manager, location, EPT_POPUP);

  switch (response) {
    case EPHY_PERMISSION_ALLOWED:
      allow = TRUE;
      break;
    case EPHY_PERMISSION_DENIED:
      allow = FALSE;
      break;
    case EPHY_PERMISSION_DEFAULT:
    default:
      allow = eel_gconf_get_boolean
              (CONF_SECURITY_ALLOW_POPUPS);
      break;
  }

  g_free (location);

  LOG ("ephy_web_view_get_popups_allowed: view %p, allowed: %d", view, allow);

  return allow;
}

static gboolean
popups_manager_remove_window (EphyWebView *view,
                              EphyEmbedContainer *container)
{
  view->priv->shown_popups = g_slist_remove (view->priv->shown_popups,
                                              container);

  return FALSE;
}

static void
popups_manager_add_window (EphyWebView *view,
                           EphyEmbedContainer *container)
{
  LOG ("popups_manager_add_window: view %p, container %p", view, container);

  view->priv->shown_popups = g_slist_prepend (view->priv->shown_popups, container);

  g_signal_connect_swapped (container, "destroy",
                            G_CALLBACK (popups_manager_remove_window),
                            view);
}

static void
disconnect_popup (EphyEmbedContainer *container,
                  EphyWebView *view)
{
  g_signal_handlers_disconnect_by_func
  (container, G_CALLBACK (popups_manager_remove_window), view);
}

void
ephy_web_view_popups_manager_reset (EphyWebView *view)
{
  g_slist_foreach (view->priv->hidden_popups,
                   (GFunc)popups_manager_free_info, NULL);
  g_slist_free (view->priv->hidden_popups);
  view->priv->hidden_popups = NULL;

  g_slist_foreach (view->priv->shown_popups,
                   (GFunc)disconnect_popup, view);
  g_slist_free (view->priv->shown_popups);
  view->priv->shown_popups = NULL;

  g_object_notify (G_OBJECT (view), "hidden-popup-count");
  g_object_notify (G_OBJECT (view), "popups-allowed");
}

static void
ephy_web_view_get_property (GObject *object,
                            guint prop_id,
                            GValue *value,
                            GParamSpec *pspec)
{
  EphyWebViewPrivate *priv = EPHY_WEB_VIEW (object)->priv;

  switch (prop_id) {
    case PROP_ADDRESS:
      g_value_set_string (value, priv->address);
      break;
    case PROP_LOAD_STATUS:
      g_value_set_boolean (value, priv->is_loading);
      break;
    case PROP_EMBED_TITLE:
      g_value_set_string (value, priv->title);
      break;
    case PROP_TYPED_ADDRESS:
      g_value_set_string (value, priv->typed_address);
      break;
    case PROP_DOCUMENT_TYPE:
      g_value_set_enum (value, priv->document_type);
      break;
    case PROP_HIDDEN_POPUP_COUNT:
      g_value_set_int (value, popup_blocker_n_hidden
                       (EPHY_WEB_VIEW (object)));
      break;
    case PROP_ICON:
      g_value_set_object (value, priv->icon);
      break;
    case PROP_ICON_ADDRESS:
      g_value_set_string (value, priv->icon_address);
      break;
    case PROP_LINK_MESSAGE:
      g_value_set_string (value, priv->link_message);
      break;
    case PROP_LOAD_PROGRESS:
      g_value_set_int (value, priv->load_percent);
      break;
    case PROP_NAVIGATION:
      g_value_set_flags (value, priv->nav_flags);
      break;
    case PROP_POPUPS_ALLOWED:
      g_value_set_boolean (value, ephy_web_view_get_popups_allowed
                           (EPHY_WEB_VIEW (object)));
      break;
    case PROP_SECURITY:
      g_value_set_enum (value, priv->security_level);
      break;
    case PROP_STATUS_MESSAGE:
      g_value_set_string (value, priv->status_message);
      break;
    case PROP_VISIBLE:
      g_value_set_boolean (value, priv->visibility);
      break;
    default:
      break;
  }
}

static void
ephy_web_view_set_property (GObject *object,
                            guint prop_id,
                            const GValue *value,
                            GParamSpec *pspec)
{
  switch (prop_id) {
    case PROP_ICON_ADDRESS:
      ephy_web_view_set_icon_address (EPHY_WEB_VIEW (object), g_value_get_string (value));
      break;
    case PROP_POPUPS_ALLOWED:
      ephy_web_view_set_popups_allowed (EPHY_WEB_VIEW (object), g_value_get_boolean (value));
      break;
    case PROP_TYPED_ADDRESS:
      ephy_web_view_set_typed_address (EPHY_WEB_VIEW (object), g_value_get_string (value),
                                       EPHY_WEB_VIEW_ADDRESS_EXPIRE_NOW);
      break;
      break;
    case PROP_ADDRESS:
    case PROP_DOCUMENT_TYPE:
    case PROP_HIDDEN_POPUP_COUNT:
    case PROP_ICON:
    case PROP_LINK_MESSAGE:
    case PROP_LOAD_PROGRESS:
    case PROP_LOAD_STATUS:
    case PROP_NAVIGATION:
    case PROP_SECURITY:
    case PROP_STATUS_MESSAGE:
    case PROP_EMBED_TITLE:
    case PROP_VISIBLE:
      /* read only */
      break;
    default:
      break;
  }
}

static void
ephy_web_view_file_monitor_cancel (EphyWebView *view)
{
  EphyWebViewPrivate *priv = view->priv;

  if (priv->monitor != NULL) {
    LOG ("Cancelling file monitor");
    
    g_file_monitor_cancel (G_FILE_MONITOR (priv->monitor));
    priv->monitor = NULL;
  }

  if (priv->reload_scheduled_id != 0) {
    LOG ("Cancelling scheduled reload");

    g_source_remove (priv->reload_scheduled_id);
    priv->reload_scheduled_id = 0;
  }

  priv->reload_delay_ticks = 0;
}

static void
ephy_web_view_dispose (GObject *object)
{
  ephy_web_view_file_monitor_cancel (EPHY_WEB_VIEW (object));

  G_OBJECT_CLASS (ephy_web_view_parent_class)->dispose (object);
}

static void
ephy_web_view_finalize (GObject *object)
{
  EphyWebViewPrivate *priv = EPHY_WEB_VIEW (object)->priv;

  if (priv->icon != NULL) {
    g_object_unref (priv->icon);
    priv->icon = NULL;
  }

  ephy_web_view_popups_manager_reset (EPHY_WEB_VIEW (object));

  g_free (priv->address);
  g_free (priv->typed_address);
  g_free (priv->title);
  g_free (priv->icon_address);
  g_free (priv->status_message);
  g_free (priv->link_message);
  g_free (priv->loading_title);

  G_OBJECT_CLASS (ephy_web_view_parent_class)->finalize (object);
}

static void
ephy_web_view_class_init (EphyWebViewClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = ephy_web_view_dispose;
  gobject_class->finalize = ephy_web_view_finalize;
  gobject_class->get_property = ephy_web_view_get_property;
  gobject_class->set_property = ephy_web_view_set_property;

  g_object_class_install_property (gobject_class,
                                   PROP_ADDRESS,
                                   g_param_spec_string ("address",
                                                        "Address",
                                                        "The view's address",
                                                        "",
                                                        G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
                                   PROP_TYPED_ADDRESS,
                                   g_param_spec_string ("typed-address",
                                                        "Typed Address",
                                                        "The typed address",
                                                        "",
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
                                   PROP_EMBED_TITLE,
                                   g_param_spec_string ("embed-title",
                                                        "Title",
                                                        "The view's title",
                                                        EMPTY_PAGE,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_object_class_install_property (gobject_class,
                                   PROP_SECURITY,
                                   g_param_spec_enum ("security-level",
                                                      "Security Level",
                                                      "The view's security level",
                                                      EPHY_TYPE_WEB_VIEW_SECURITY_LEVEL,
                                                      EPHY_WEB_VIEW_STATE_IS_UNKNOWN,
                                                      G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
                                   PROP_DOCUMENT_TYPE,
                                   g_param_spec_enum ("document-type",
                                                      "Document Type",
                                                      "The view's documen type",
                                                      EPHY_TYPE_WEB_VIEW_DOCUMENT_TYPE,
                                                      EPHY_WEB_VIEW_DOCUMENT_HTML,
                                                      G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_object_class_install_property (gobject_class,
                                   PROP_LOAD_PROGRESS,
                                   g_param_spec_int ("load-progress",
                                                     "Load progress",
                                                     "The view's load progress in percent",
                                                     0,
                                                     100,
                                                     0,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
                                   PROP_LOAD_STATUS,
                                   g_param_spec_boolean ("load-status",
                                                         "Load status",
                                                         "The view's load status",
                                                         FALSE,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
                                   PROP_NAVIGATION,
                                   g_param_spec_flags ("navigation",
                                                       "Navigation flags",
                                                       "The view's navigation flags",
                                                       EPHY_TYPE_WEB_VIEW_NAVIGATION_FLAGS,
                                                       0,
                                                       G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
                                   PROP_STATUS_MESSAGE,
                                   g_param_spec_string ("status-message",
                                                        "Status Message",
                                                        "The view's statusbar message",
                                                        NULL,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
                                   PROP_LINK_MESSAGE,
                                   g_param_spec_string ("link-message",
                                                        "Link Message",
                                                        "The view's link message",
                                                        NULL,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
                                   PROP_ICON,
                                   g_param_spec_object ("icon",
                                                        "Icon",
                                                        "The view icon's",
                                                        GDK_TYPE_PIXBUF,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_object_class_install_property (gobject_class,
                                   PROP_ICON_ADDRESS,
                                   g_param_spec_string ("icon-address",
                                                        "Icon address",
                                                        "The view icon's address",
                                                        NULL,
                                                        (G_PARAM_READWRITE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB)));
  g_object_class_install_property (gobject_class,
                                   PROP_HIDDEN_POPUP_COUNT,
                                   g_param_spec_int ("hidden-popup-count",
                                                     "Number of Blocked Popups",
                                                     "The view's number of blocked popup windows",
                                                     0,
                                                     G_MAXINT,
                                                     0,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_object_class_install_property (gobject_class,
                                   PROP_POPUPS_ALLOWED,
                                   g_param_spec_boolean ("popups-allowed",
                                                         "Popups Allowed",
                                                         "Whether popup windows are to be displayed",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_object_class_install_property (gobject_class,
                                   PROP_VISIBLE,
                                   g_param_spec_boolean ("visibility",
                                                         "Visibility",
                                                         "The view's visibility",
                                                         FALSE,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

/**
 * EphyWebView::ge-new-window:
 * @view:
 * @new_view: the newly opened #EphyWebView
 *
 * The ::ge_new_window signal is emitted after a new window has been opened by
 * the view. For example, when a JavaScript popup window is opened.
 **/
		g_signal_new ("ge_new_window",
                  EPHY_TYPE_WEB_VIEW,
                  G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (EphyWebViewClass, new_window),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE,
                  1,
                  GTK_TYPE_WIDGET);
/**
 * EphyWebView::ge-popup-blocked:
 * @view:
 * @address: The requested URL
 * @target: The requested window name, e.g. "_blank"
 * @features: The requested features: for example, "height=400,width=200"
 *
 * The ::ge_popup_blocked signal is emitted when the viewed web page requests
 * a popup window (with javascript:open()) but popup windows are not allowed.
 **/
		g_signal_new ("ge_popup_blocked",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (EphyWebViewClass, popup_blocked),
			      NULL, NULL,
			      ephy_marshal_VOID__STRING_STRING_STRING,
			      G_TYPE_NONE,
			      3,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);
/**
 * EphyWebView::ge-context-menu:
 * @embed:
 * @event: the #EphyEmbedEvent which triggered this signal
 *
 * The ::ge_context_menu signal is emitted when a context menu is to be
 * displayed. This will usually happen when the user right-clicks on a part of
 * @embed.
 **/
		g_signal_new ("ge_context_menu",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EphyWebViewClass, context_menu),
			      g_signal_accumulator_true_handled, NULL,
			      ephy_marshal_BOOLEAN__OBJECT,
			      G_TYPE_BOOLEAN,
			      1,
			      G_TYPE_OBJECT);
/**
 * EphyWebView::ge-favicon:
 * @embed:
 * @address: the URL to @embed's web site's favicon
 *
 * The ::ge_favicon signal is emitted when @embed discovers that a favourite
 * icon (favicon) is available for the site it is visiting.
 **/
		g_signal_new ("ge_favicon",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (EphyWebViewClass, favicon),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);
/**
 * EphyWebView::ge-search-link:
 * @embed:
 * @type: the mime-type of the search description
 * @title: the title of the news feed
 * @address: the URL to @embed's web site's search description
 *
 * The ::ge_rss signal is emitted when @embed discovers that a search
 * description is available for the site it is visiting.
 **/
		g_signal_new ("ge_search_link",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (EphyWebViewClass, search_link),
			      NULL, NULL,
			      ephy_marshal_VOID__STRING_STRING_STRING,
			      G_TYPE_NONE,
			      3,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);

/**
 * EphyWebView::ge-feed-link:
 * @embed:
 * @type: the mime-type of the news feed
 * @title: the title of the news feed
 * @address: the URL to @embed's web site's news feed
 *
 * The ::ge_rss signal is emitted when @embed discovers that a news feed
 * is available for the site it is visiting.
 **/
		g_signal_new ("ge_feed_link",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (EphyWebViewClass, feed_link),
			      NULL, NULL,
			      ephy_marshal_VOID__STRING_STRING_STRING,
			      G_TYPE_NONE,
			      3,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);
/**
 * EphyWebView::ge-dom-mouse-click:
 * @embed:
 * @event: the #EphyEmbedEvent which triggered this signal
 *
 * The ::ge_dom_mouse_click signal is emitted when the user clicks in @embed.
 **/
		g_signal_new ("ge_dom_mouse_click",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EphyWebViewClass, dom_mouse_click),
			      g_signal_accumulator_true_handled, NULL,
			      ephy_marshal_BOOLEAN__OBJECT,
			      G_TYPE_BOOLEAN,
			      1,
			      G_TYPE_OBJECT);
/**
 * EphyWebView::ge-dom-mouse-down:
 * @embed:
 * @event: the #EphyEmbedEvent which triggered this signal
 *
 * The ::ge_dom_mouse_down signal is emitted when the user depresses a mouse
 * button.
 **/
		g_signal_new ("ge_dom_mouse_down",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EphyWebViewClass, dom_mouse_down),
			      g_signal_accumulator_true_handled, NULL,
			      ephy_marshal_BOOLEAN__OBJECT,
			      G_TYPE_BOOLEAN,
			      1,
			      G_TYPE_OBJECT);
/**
 * EphyWebView::ge-modal-alert:
 * @embed:
 *
 * The ::ge-modal-alert signal is emitted when a DOM event will open a
 * modal alert.
 *
 * Return %TRUE to prevent the dialog from being opened.
 **/
		g_signal_new ("ge_modal_alert",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EphyWebViewClass, modal_alert),
			      g_signal_accumulator_true_handled, NULL,
			      ephy_marshal_BOOLEAN__VOID,
			      G_TYPE_BOOLEAN,
			      0);
/**
 * EphyWebView::ge-modal-alert-closed:
 * @embed:
 *
 * The ::ge-modal-alert-closed signal is emitted when a modal alert put up by a
 * DOM event was closed.
 **/
		g_signal_new ("ge_modal_alert_closed",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EphyWebViewClass, modal_alert_closed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE,
			      0);

/**
 * EphyWebView::ge-document-type:
 * @embed:
 * @type: the new document type
 *
 * The ::ge-document-type signal is emitted when @embed determines the type of its document.
 **/
		g_signal_new ("ge_document_type",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (EphyWebViewClass, document_type),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__ENUM,
			      G_TYPE_NONE,
			      1,
			      EPHY_TYPE_WEB_VIEW_DOCUMENT_TYPE);
/**
 * EphyWebView::dom-content-loaded:
 * @embed:
 *
 * The ::dom-content-loaded signal is emitted when 
 * the document has been loaded (excluding images and other loads initiated by this document).
 * That's true also for frameset and all the frames within it.
 **/
		g_signal_new ("dom_content_loaded",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (EphyWebViewClass, dom_content_loaded),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__POINTER,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_POINTER);

/**
 * EphyWebView::ge-search-key-press:
 * @embed:
 * @event: the #GdkEventKey which triggered this signal
 *
 * The ::ge-search-key-press signal is emitted for keypresses which
 * should be used for find implementations.
 **/
		g_signal_new ("ge-search-key-press",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EphyWebViewClass, search_key_press),
			      g_signal_accumulator_true_handled, NULL,
			      ephy_marshal_BOOLEAN__BOXED,
			      G_TYPE_BOOLEAN,
			      1,
			      GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);

/**
 * EphyWebView::close-request
 * @embed:
 *
 * The ::close signal is emitted when the embed request closing.
 * Return %TRUE to prevent closing. You HAVE to process removal of the embed
 * as soon as possible after that.
 **/
		g_signal_new ("close-request",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EphyWebViewClass, close_request),
			      g_signal_accumulator_true_handled, NULL,
			      ephy_marshal_BOOLEAN__VOID,
			      G_TYPE_BOOLEAN,
			      0);
/**
 * EphyWebView::content-blocked:
 * @embed:
 * @uri: blocked URI 
 *
 * The ::content-blocked signal is emitted when an url has been blocked.
 **/
		g_signal_new ("content-blocked",
			      EPHY_TYPE_WEB_VIEW,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EphyWebViewClass, content_blocked),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);
/**
 * EphyWebView::new-document-now:
 * @embed:
 * @uri: URI of the new content
 *
 * The ::new-document-now signal is emitted when a new page content
 * is being loaded into the browser. It's a good place to do view
 * related changes, for example to restore the zoom level of a page
 * or to set an user style sheet.
 **/
	       g_signal_new ("new-document-now",
			     EPHY_TYPE_WEB_VIEW,
			     G_SIGNAL_RUN_FIRST,
			     G_STRUCT_OFFSET (EphyWebViewClass, new_document_now),
			     NULL, NULL,
			     g_cclosure_marshal_VOID__STRING,
			     G_TYPE_NONE,
			     1,
			     G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);

  g_type_class_add_private (gobject_class, sizeof (EphyWebViewPrivate));
}

static void
icon_cache_changed_cb (EphyFaviconCache *cache,
                       const char *address,
                       EphyWebView *view)
{
  const char *icon_address;

  g_return_if_fail (address != NULL);

  icon_address = ephy_web_view_get_icon_address (view);

  /* is this for us? */
  if (icon_address != NULL &&
      strcmp (icon_address, address) == 0) {
    ephy_web_view_load_icon (view);
  }
}

static void
ge_document_type_cb (EphyWebView *view,
                     EphyWebViewDocumentType type,
                     gpointer user_data)
{
  if (view->priv->document_type != type) {
    view->priv->document_type = type;

    g_object_notify (G_OBJECT (view), "document-type");
  }
}

static void
ge_favicon_cb (EphyWebView *view,
               const char *address,
               gpointer user_data)
{
  ephy_web_view_set_icon_address (view, address);
}

static void
ge_new_window_cb (EphyWebView *view,
                  EphyWebView *new_view,
                  gpointer user_data)
{
  EphyEmbedContainer *container;

  g_return_if_fail (new_view != NULL);

  container = EPHY_EMBED_CONTAINER (gtk_widget_get_toplevel (GTK_WIDGET (new_view)));
  g_return_if_fail (container != NULL || !GTK_WIDGET_TOPLEVEL (container));

  popups_manager_add_window (view, container);
}

static void
ge_popup_blocked_cb (EphyWebView *view,
                     const char *url,
                     const char *name,
                     const char *features,
                     gpointer user_data)
{
  popups_manager_add (view, url, name, features);
}

static void
ephy_web_view_init (EphyWebView *web_view)
{
  EphyWebViewPrivate *priv;
  EphyFaviconCache *cache;

  priv = web_view->priv = EPHY_WEB_VIEW_GET_PRIVATE (web_view);

  priv->address_expire = EPHY_WEB_VIEW_ADDRESS_EXPIRE_NOW;
  priv->is_blank = TRUE;
  priv->title = g_strdup (EMPTY_PAGE);
  priv->document_type = EPHY_WEB_VIEW_DOCUMENT_HTML;
  priv->security_level = EPHY_WEB_VIEW_STATE_IS_UNKNOWN;
  priv->monitor_directory = FALSE;

  g_signal_connect_object (web_view, "ge_document_type",
                           G_CALLBACK (ge_document_type_cb),
                           web_view, (GConnectFlags)0);

  g_signal_connect_object (web_view, "ge_favicon",
                           G_CALLBACK (ge_favicon_cb),
                           web_view, (GConnectFlags)0);

  g_signal_connect_object (web_view, "ge_new_window",
                           G_CALLBACK (ge_new_window_cb),
                           web_view, (GConnectFlags)0);

  g_signal_connect_object (web_view, "ge_popup_blocked",
                           G_CALLBACK (ge_popup_blocked_cb),
                           web_view, (GConnectFlags)0);

  cache = EPHY_FAVICON_CACHE
          (ephy_embed_shell_get_favicon_cache (embed_shell));
  g_signal_connect_object (G_OBJECT (cache), "changed",
                           G_CALLBACK (icon_cache_changed_cb),
                           web_view, (GConnectFlags)0);
}

/**
 * ephy_web_view_new:
 *
 * Equivalent to g_object_new() but returns an #GtkWidget so you don't have
 * to cast it when dealing with most code.
 *
 * Return value: the newly created #EphyWebView widget
 **/
GtkWidget *
ephy_web_view_new (void)
{
  return GTK_WIDGET (g_object_new (EPHY_TYPE_WEB_VIEW, NULL));
}

/**
 * ephy_web_view_load_request:
 * @web_view: the #EphyWebView in which to load the request
 * @request: the #WebKitNetworkRequest to be loaded
 *
 * Loads the given #WebKitNetworkRequest in the given #EphyWebView.
 **/
void
ephy_web_view_load_request (EphyWebView *web_view,
                            WebKitNetworkRequest *request)
{
  WebKitWebFrame *main_frame;

	g_return_if_fail(EPHY_IS_WEB_VIEW(web_view));
	g_return_if_fail(WEBKIT_IS_NETWORK_REQUEST(request));

	main_frame = webkit_web_view_get_main_frame (WEBKIT_WEB_VIEW(web_view));
  webkit_web_frame_load_request(main_frame, request);
}

/**
 * ephy_web_view_load_url:
 * @embed: an #EphyWebView
 * @url: a URL
 *
 * Loads @url in @embed.
 **/
void
ephy_web_view_load_url (EphyWebView *view,
                        const char *url)
{
  g_return_if_fail (EPHY_IS_WEB_VIEW (view));
  g_return_if_fail (url);

  webkit_web_view_open (WEBKIT_WEB_VIEW (view), url);
}

/**
 * ephy_web_view_copy_back_history:
 * @source: the #EphyWebView from which to get the back history
 * @dest: the #EphyWebView to copy the history to
 *
 * Sets the back history (up to the current item) of @source as the
 * back history of @dest.
 *
 * Useful to keep the history when opening links in new tabs or
 * windows.
 **/
void
ephy_web_view_copy_back_history (EphyWebView *source,
																 EphyWebView *dest)
{
  WebKitWebView *source_view, *dest_view;
  WebKitWebBackForwardList* source_bflist, *dest_bflist;
  WebKitWebHistoryItem *item;
  GList *items;

	g_return_if_fail(EPHY_IS_WEB_VIEW(source));
	g_return_if_fail(EPHY_IS_WEB_VIEW(dest));

  source_view = WEBKIT_WEB_VIEW (source);
  dest_view = WEBKIT_WEB_VIEW (dest);

  source_bflist = webkit_web_view_get_back_forward_list (source_view);
  dest_bflist = webkit_web_view_get_back_forward_list (dest_view);

	items = webkit_web_back_forward_list_get_back_list_with_limit (source_bflist, EPHY_WEBKIT_BACK_FORWARD_LIMIT);
	/* We want to add the items in the reverse order here, so the
		 history ends up the same */
	items = g_list_reverse (items);
	for (; items; items = items->next) {
		item = (WebKitWebHistoryItem*)items->data;
		webkit_web_back_forward_list_add_item (dest_bflist, g_object_ref (item));
	}
	g_list_free (items);

  /* The ephy/gecko behavior is to add the current item of the source
     embed at the end of the back history, so keep doing that */
  item = webkit_web_back_forward_list_get_current_item (source_bflist);
  webkit_web_back_forward_list_add_item (dest_bflist, g_object_ref (item));
}

void
ephy_web_view_set_address (EphyWebView *embed,
                           const char *address)
{
  EphyWebViewPrivate *priv = embed->priv;
  GObject *object = G_OBJECT (embed);

  g_free (priv->address);
  priv->address = g_strdup (address);

  priv->is_blank = address == NULL ||
                   strcmp (address, "about:blank") == 0;

  if (priv->is_loading &&
      priv->address_expire == EPHY_WEB_VIEW_ADDRESS_EXPIRE_NOW &&
      priv->typed_address != NULL) {
    g_free (priv->typed_address);
    priv->typed_address = NULL;

    g_object_notify (object, "typed-address");
  }

  g_object_notify (object, "address");
}

static char*
get_title_from_address (const char *address)
{
  if (g_str_has_prefix (address, "file://")) 
    return g_strdup (address + 7);
  else
    return ephy_string_get_host_name (address);
}

void
ephy_web_view_set_title (EphyWebView *view,
                         const char *view_title)
{
  EphyWebViewPrivate *priv = view->priv;
  char *title = g_strdup (view_title);

  if (!priv->is_blank && (title == NULL || g_strstrip (title)[0] == '\0')) {
    g_free (title);
    title = get_title_from_address (priv->address);

    /* Fallback */
    if (title == NULL || title[0] == '\0') {
      g_free (title);
      title = g_strdup (EMPTY_PAGE);
      priv->is_blank = TRUE;
    }
  } else if (priv->is_blank) {
    g_free (title);
    title = g_strdup (EMPTY_PAGE);
  }

  g_free (priv->title);
  priv->title = ephy_string_shorten (title, MAX_TITLE_LENGTH);

  g_object_notify (G_OBJECT (view), "embed-title");
}

/**
 * ephy_web_view_get_is_blank:
 * @view: an #EphyWebView
 *
 * Returns whether the	@view's address is "blank".
 *
 * Return value: %TRUE if the @view's address is "blank"
 **/
gboolean
ephy_web_view_get_is_blank (EphyWebView *view)
{
  return view->priv->is_blank;
}

/**
 * ephy_web_view_get_address:
 * @view: an #EphyWebView
 *
 * Returns the address of the currently loaded page.
 *
 * Return value: @view's address. Will never be %NULL.
 **/
const char *
ephy_web_view_get_address (EphyWebView *view)
{
  EphyWebViewPrivate *priv = view->priv;
  return priv->address ? priv->address : "about:blank";
}

/**
 * ephy_web_view_get_title:
 * @view: an #EphyWebView
 *
 * Return value: the title of the web page displayed in @view
 **/
const char *
ephy_web_view_get_title (EphyWebView *view)
{
  return view->priv->title;
}

static void
ensure_page_info (EphyWebView *view, const char *address)
{
  EphyWebViewPrivate *priv = view->priv;

  if ((priv->address == NULL || priv->address[0] == '\0') &&
      priv->address_expire == EPHY_WEB_VIEW_ADDRESS_EXPIRE_NOW) {
    ephy_web_view_set_address (view, address);
  }

  /* FIXME huh?? */
  if (priv->title == NULL || priv->title[0] == '\0') {
    ephy_web_view_set_title (view, NULL);
  }
}

static void
update_net_state_message (EphyWebView *view, const char *uri, EphyWebViewNetState flags)
{
  const char *msg = NULL;
  char *host = NULL;

  if (uri != NULL)
    host = ephy_string_get_host_name (uri);

  if (host == NULL) goto out;

  /* IS_REQUEST and IS_NETWORK can be both set */
  if (flags & EPHY_WEB_VIEW_STATE_IS_REQUEST) {
    if (flags & EPHY_WEB_VIEW_STATE_REDIRECTING) {
      msg = _ ("Redirecting to “%s”…");
    } else if (flags & EPHY_WEB_VIEW_STATE_TRANSFERRING) {
      msg = _ ("Transferring data from “%s”…");
    } else if (flags & EPHY_WEB_VIEW_STATE_NEGOTIATING) {
      msg = _ ("Waiting for authorization from “%s”…");
    }
  }

  if (flags & EPHY_WEB_VIEW_STATE_IS_NETWORK) {
    if (flags & EPHY_WEB_VIEW_STATE_START) {
      msg = _ ("Loading “%s”…");
    }
  }

  if ((flags & EPHY_WEB_VIEW_STATE_IS_NETWORK) &&
      (flags & EPHY_WEB_VIEW_STATE_STOP)) {
    g_free (view->priv->status_message);
    view->priv->status_message = NULL;
    g_object_notify (G_OBJECT (view), "status-message");

  } else if (msg != NULL) {
    g_free (view->priv->status_message);
    g_free (view->priv->loading_title);
    view->priv->status_message = g_strdup_printf (msg, host);
    view->priv->loading_title = g_strdup_printf (msg, host);
    g_object_notify (G_OBJECT (view), "status-message");
    g_object_notify (G_OBJECT (view), "embed-title");
  }

 out:
    g_free (host);
}

static void
update_navigation_flags (EphyWebView *view)
{
  EphyWebViewPrivate *priv = view->priv;
  guint flags = 0;
  WebKitWebView *web_view = WEBKIT_WEB_VIEW (view);

  if (ephy_web_view_can_go_up (view)) {
    flags |= EPHY_WEB_VIEW_NAV_UP;
  }

  if (webkit_web_view_can_go_back (web_view)) {
    flags |= EPHY_WEB_VIEW_NAV_BACK;
  }

  if (webkit_web_view_can_go_forward (web_view)) {
    flags |= EPHY_WEB_VIEW_NAV_FORWARD;
  }

  if (priv->nav_flags != (EphyWebViewNavigationFlags)flags) {
    priv->nav_flags = (EphyWebViewNavigationFlags)flags;

    g_object_notify (G_OBJECT (view), "navigation");
  }
}

static int
build_load_percent (int requests_done, int requests_total)
{
  int percent = 0;

  if (requests_total > 0) {
    percent = (requests_done * 100) / requests_total;
    percent = CLAMP (percent, 0, 100);
  }

  return percent;
}

void
ephy_web_view_set_load_percent (EphyWebView *view, int percent)
{
  EphyWebViewPrivate *priv = view->priv;

  if (percent != priv->load_percent) {
    priv->load_percent = percent;

    g_object_notify (G_OBJECT (view), "load-progress");
  }
}

static void
build_progress_from_requests (EphyWebView *view, EphyWebViewNetState state)
{
  int load_percent;

  if (state & EPHY_WEB_VIEW_STATE_IS_REQUEST) {
    if (state & EPHY_WEB_VIEW_STATE_START) {
      view->priv->total_requests++;
    } else if (state & EPHY_WEB_VIEW_STATE_STOP) {
      view->priv->cur_requests++;
    }

    load_percent = build_load_percent (view->priv->cur_requests,
                                       view->priv->total_requests);

    ephy_web_view_set_load_percent (view, load_percent);
  }
}

static void
ephy_web_view_set_load_status (EphyWebView *view, gboolean status)
{
  EphyWebViewPrivate *priv = view->priv;
  guint is_loading;

  is_loading = status != FALSE;

  if (is_loading != priv->is_loading) {
    priv->is_loading = is_loading;

    g_object_notify (G_OBJECT (view), "load-status");
  }
}

void
ephy_web_view_update_from_net_state (EphyWebView *view,
                                     const char *uri,
                                     EphyWebViewNetState state)
{
  EphyWebViewPrivate *priv = view->priv;

  update_net_state_message (view, uri, state);

  if (state & EPHY_WEB_VIEW_STATE_IS_NETWORK) {
    if (state & EPHY_WEB_VIEW_STATE_START) {
      GObject *object = G_OBJECT (view);

      g_object_freeze_notify (object);

      priv->total_requests = 0;
      priv->cur_requests = 0;

      ephy_web_view_set_load_percent (view, 0);
      ephy_web_view_set_load_status (view, TRUE);

      ensure_page_info (view, uri);

      g_object_notify (object, "embed-title");

      g_object_thaw_notify (object);
    } else if (state & EPHY_WEB_VIEW_STATE_STOP) {
      GObject *object = G_OBJECT (view);

      g_object_freeze_notify (object);

      ephy_web_view_set_load_percent (view, 100);
      ephy_web_view_set_load_status (view, FALSE);

      g_free (priv->loading_title);
      priv->loading_title = NULL;

      priv->address_expire = EPHY_WEB_VIEW_ADDRESS_EXPIRE_NOW;

      g_object_notify (object, "embed-title");

      g_object_thaw_notify (object);
    }

    update_navigation_flags (view);
  }

  build_progress_from_requests (view, state);
}

void
ephy_web_view_set_loading_title (EphyWebView *view,
                                 const char *title,
                                 gboolean is_address)
{
  EphyWebViewPrivate *priv = view->priv;
  char *freeme = NULL;

  g_free (priv->loading_title);
  priv->loading_title = NULL;

  if (is_address) {
    title = freeme = get_title_from_address (title);
  }

  if (title != NULL && title[0] != '\0') {
    /* translators: %s here is the address of the web page */
    priv->loading_title = g_strdup_printf (_ ("Loading “%s”…"), title);
  } else {
    priv->loading_title = g_strdup (_ ("Loading…"));
  }

  g_free (freeme);
}

static gboolean
ephy_web_view_file_monitor_reload_cb (EphyWebView *view)
{
  EphyWebViewPrivate *priv = view->priv;

  if (priv->reload_delay_ticks > 0) {
    priv->reload_delay_ticks--;

    /* Run again */
    return TRUE;
  }

  if (priv->is_loading) {
    /* Wait a bit to reload if we're still loading! */
    priv->reload_delay_ticks = RELOAD_DELAY_MAX_TICKS / 2;

    /* Run again */
    return TRUE;
  }

  priv->reload_scheduled_id = 0;

  LOG ("Reloading file '%s'", ephy_web_view_get_address (view));
  webkit_web_view_reload (EPHY_GET_WEBKIT_WEB_VIEW_FROM_EMBED (view));

  /* don't run again */
  return FALSE;
}

static void
ephy_web_view_file_monitor_cb (GFileMonitor *monitor,
                               GFile *file,
                               GFile *other_file,
                               GFileMonitorEvent event_type,
                               EphyWebView *view)
{
  gboolean should_reload;
  EphyWebViewPrivate *priv = view->priv;

  switch (event_type) {
    /* These events will always trigger a reload: */
    case G_FILE_MONITOR_EVENT_CHANGED:
    case G_FILE_MONITOR_EVENT_CREATED:
      should_reload = TRUE;
      break;

    /* These events will only trigger a reload for directories: */
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
      should_reload = priv->monitor_directory;
      break;

    /* These events don't trigger a reload: */
    case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    default:
      should_reload = FALSE;
      break;
  }

  if (should_reload) {
    /* We make a lot of assumptions here, but basically we know
     * that we just have to reload, by construction.
     * Delay the reload a little bit so we don't endlessly
     * reload while a file is written.
     */
    if (priv->reload_delay_ticks == 0) {
      priv->reload_delay_ticks = 1;
    } else {
      /* Exponential backoff */
      priv->reload_delay_ticks = MIN (priv->reload_delay_ticks * 2,
                                      RELOAD_DELAY_MAX_TICKS);
    }

    if (priv->reload_scheduled_id == 0) {
      priv->reload_scheduled_id =
        g_timeout_add (RELOAD_DELAY,
                       (GSourceFunc)ephy_web_view_file_monitor_reload_cb, view);
    }
  }
}

static void
ephy_web_view_update_file_monitor (EphyWebView *view,
                                   const gchar *address)
{
  EphyWebViewPrivate *priv = view->priv;
  gboolean local;
  gchar *anchor;
  gchar *url;
  GFile *file;
  GFileType file_type;
  GFileInfo *file_info;
  GFileMonitor *monitor = NULL;

  if (priv->monitor != NULL &&
      priv->address != NULL && address != NULL &&
      strcmp (priv->address, address) == 0) {
    /* same address, no change needed */
    return;
  }

  ephy_web_view_file_monitor_cancel (view);

  local = g_str_has_prefix (address, "file://");
  if (local == FALSE) return;

  /* strip off anchors */
  anchor = strchr (address, '#');
  if (anchor != NULL) {
    url = g_strndup (address, anchor - address);
  } else {
    url = g_strdup (address);
  }

  file = g_file_new_for_uri (url);
  file_info = g_file_query_info (file,
                                 G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                 0, NULL, NULL);
  if (file_info == NULL) {
    g_object_unref (file);
    g_free (url);
    return;
  }

  file_type = g_file_info_get_file_type (file_info);
  g_object_unref (file_info);

  if (file_type == G_FILE_TYPE_DIRECTORY) {
    monitor = g_file_monitor_directory (file, 0, NULL, NULL);
    g_signal_connect (monitor, "changed",
                      G_CALLBACK (ephy_web_view_file_monitor_cb),
                      view);
    priv->monitor_directory = TRUE;
    LOG ("Installed monitor for directory '%s'", url);
  }
  else if (file_type == G_FILE_TYPE_REGULAR) {
    monitor = g_file_monitor_file (file, 0, NULL, NULL);
    g_signal_connect (monitor, "changed",
                      G_CALLBACK (ephy_web_view_file_monitor_cb),
                      view);
    priv->monitor_directory = FALSE;
    LOG ("Installed monitor for file '%s'", url);
  }
  priv->monitor = monitor;
  g_object_unref (file);
  g_free (url);
}

void
ephy_web_view_location_changed (EphyWebView *view,
                                const char *location)
{
  GObject *object = G_OBJECT (view);

  g_object_freeze_notify (object);

  /* do this up here so we still have the old address around */
  ephy_web_view_update_file_monitor (view, location);

  /* Do not expose about:blank to the user, an empty address
     bar will do better */
  if (location == NULL || location[0] == '\0' ||
      strcmp (location, "about:blank") == 0) {
    ephy_web_view_set_address (view, NULL);
    ephy_web_view_set_title (view, EMPTY_PAGE);
  } else {
    char *view_address;

    /* we do this to get rid of an eventual password in the URL */
    view_address = ephy_web_view_get_location (view, TRUE);
    ephy_web_view_set_address (view, view_address);
    ephy_web_view_set_loading_title (view, view_address, TRUE);
    g_free (view_address);
  }

  ephy_web_view_set_link_message (view, NULL);
  ephy_web_view_set_icon_address (view, NULL);
  update_navigation_flags (view);

  g_object_notify (object, "embed-title");

  g_object_thaw_notify (object);
}

void
ephy_web_view_set_icon_address (EphyWebView *view,
                                const char *address)
{
  GObject *object = G_OBJECT (view);
  EphyWebViewPrivate *priv = view->priv;
  EphyHistory *history;

  g_free (priv->icon_address);
  priv->icon_address = g_strdup (address);

  if (priv->icon != NULL) {
    g_object_unref (priv->icon);
    priv->icon = NULL;

    g_object_notify (object, "icon");
  }

  if (priv->icon_address) {
    history = EPHY_HISTORY (ephy_embed_shell_get_global_history (embed_shell));
    ephy_history_set_icon (history, priv->address, priv->icon_address);

    ephy_web_view_load_icon (view);
  }

  g_object_notify (object, "icon-address");
}

/**
 * ephy_web_view_can_go_up:
 * @view: an #EphyWebView
 *
 * Returns whether @view can travel to a higher-level directory on the server.
 * For example, for http://www.example.com/subdir/index.html, returns %TRUE; for
 * http://www.example.com/index.html, returns %FALSE.
 *
 * Return value: %TRUE if @view can browse to a higher-level directory
 **/
gboolean
ephy_web_view_can_go_up (EphyWebView *view)
{
  return FALSE;
}

/**
 * ephy_web_view_get_load_status:
 * @view: an #EphyWebView
 *
 * Returns whether the web page in @view has finished loading. A web page is
 * only finished loading after all images, styles, and other dependencies have
 * been downloaded and rendered.
 *
 * Return value: %TRUE if the page is still loading, %FALSE if complete
 **/
gboolean
ephy_web_view_get_load_status (EphyWebView *view)
{
  return view->priv->is_loading;
}

const char *
ephy_web_view_get_loading_title (EphyWebView *view)
{
  return view->priv->loading_title;
}

/**
 * ephy_web_view_get_icon_address:
 * @view: an #EphyWebView
 *
 * Returns a URL which points to @view's site icon.
 *
 * Return value: the URL of @view's site icon
 **/
const char *
ephy_web_view_get_icon_address (EphyWebView *view)
{
  return view->priv->icon_address;
}

/**
 * ephy_wew_view_get_icon:
 * @view: an #EphyWebView
 *
 * Returns the view's site icon as a #GdkPixbuf,
 * or %NULL if it is not available.
 *
 * Return value: a the view's site icon
 **/
GdkPixbuf *
ephy_web_view_get_icon (EphyWebView *view)
{
  return view->priv->icon;
}

/**
 * ephy_web_view_get_document_type:
 * @view: an #EphyWebView
 *
 * Returns the type of document loaded in the @view
 *
 * Return value: the #EphyWebViewDocumentType
 **/
EphyWebViewDocumentType
ephy_web_view_get_document_type (EphyWebView *view)
{
  return view->priv->document_type;
}

/**
 * ephy_web_view_get_load_percent:
 * @view: an #EphyWebView
 *
 * Returns the page load percentage (displayed in the progressbar).
 *
 * Return value: a percentage from 0 to 100.
 **/
int
ephy_web_view_get_load_percent (EphyWebView *view)
{
  return view->priv->load_percent;
}

/**
 * ephy_web_view_get_navigation_flags:
 * @view: an #EphyWebView
 *
 * Returns @view's navigation flags.
 *
 * Return value: @view's navigation flags
 **/
EphyWebViewNavigationFlags
ephy_web_view_get_navigation_flags (EphyWebView *view)
{
  return view->priv->nav_flags;
}

/**
 * ephy_web_view_get_status_message:
 * @view: an #EphyWebView
 *
 * Returns the message displayed in @view's #EphyWindow's
 * #EphyStatusbar. If the user is hovering the mouse over a hyperlink,
 * this function will return the same value as
 * ephy_web_view_get_link_message(). Otherwise, it will return a network
 * status message, or NULL.
 *
 * The message returned has a limited lifetime, and so should be copied with
 * g_strdup() if it must be stored.
 *
 * Return value: The current statusbar message
 **/
const char *
ephy_web_view_get_status_message (EphyWebView *view)
{
  EphyWebViewPrivate *priv = view->priv;

  if (priv->link_message && priv->link_message[0] != '\0') {
    return priv->link_message;
  } else if (priv->status_message) {
    return priv->status_message;
  } else {
    return NULL;
  }
}

/**
 * ephy_web_view_get_link_message:
 * @view: an #EphyWebView
 *
 * When the user is hovering the mouse over a hyperlink, returns the URL of the
 * hyperlink.
 *
 * Return value: the URL of the link over which the mouse is hovering
 **/
const char *
ephy_web_view_get_link_message (EphyWebView *view)
{
  g_return_val_if_fail (EPHY_IS_WEB_VIEW (view), NULL);

  return view->priv->link_message;
}

/**
 * ephy_web_view_get_visibility:
 * @view: an #EphyWebView
 *
 * Returns whether the @view's toplevel is visible or not. Used
 * mostly for popup visibility management.
 *
 * Return value: %TRUE if @view's "visibility" property is set
 **/
gboolean
ephy_web_view_get_visibility (EphyWebView *view)
{
  return view->priv->visibility;
}

void
ephy_web_view_set_link_message (EphyWebView *view,
                                char *link_message)
{
  EphyWebViewPrivate *priv = view->priv;

  g_free (priv->link_message);

  priv->link_message = ephy_embed_utils_link_message_parse (link_message);

  g_object_notify (G_OBJECT (view), "status-message");
  g_object_notify (G_OBJECT (view), "link-message");
}

void
ephy_web_view_load_icon (EphyWebView *view)
{
  EphyWebViewPrivate *priv = view->priv;
  EphyEmbedShell *shell;
  EphyFaviconCache *cache;

  if (priv->icon_address == NULL || priv->icon != NULL) return;

  shell = ephy_embed_shell_get_default ();
  cache = EPHY_FAVICON_CACHE (ephy_embed_shell_get_favicon_cache (shell));

  /* ephy_favicon_cache_get returns a reference already */
  priv->icon = ephy_favicon_cache_get (cache, priv->icon_address);

  g_object_notify (G_OBJECT (view), "icon");
}

void
ephy_web_view_set_security_level (EphyWebView *view,
                                  EphyWebViewSecurityLevel level)
{
  EphyWebViewPrivate *priv = view->priv;

  if (priv->security_level != level) {
    priv->security_level = level;

    g_object_notify (G_OBJECT (view), "security-level");
  }
}

void
ephy_web_view_set_visibility (EphyWebView *view,
                              gboolean visibility)
{
  EphyWebViewPrivate *priv = view->priv;

  if (priv->visibility != visibility) {
    priv->visibility = visibility;

    g_object_notify (G_OBJECT (view), "visibility");
  }
}

/**
 * ephy_web_view_get_typed_address:
 * @view: an #EphyWebView
 *
 * Returns the text that @view's #EphyWindow will display in its location toolbar
 * entry when @view is selected.
 *
 * This is not guaranteed to be the same as @view's location,
 * available through ephy_web_view_get_location(). As the user types a new address
 * into the location entry, ephy_web_view_get_location()'s returned string will
 * change.
 *
 * Return value: @view's #EphyWindow's location entry when @view is selected
 **/
const char *
ephy_web_view_get_typed_address (EphyWebView *view)
{
  return view->priv->typed_address;
}

/**
 * ephy_web_view_set_typed_address:
 * @view: an #EphyWebView
 * @address: the new typed address, or %NULL to clear it
 * @expire: when to expire this address_expire
 * 
 * Sets the text that @view's #EphyWindow will display in its location toolbar
 * entry when @view is selected.
 **/
void
ephy_web_view_set_typed_address (EphyWebView *view,
                                 const char *address,
                                 EphyWebViewAddressExpire expire)
{
  EphyWebViewPrivate *priv = EPHY_WEB_VIEW (view)->priv;

  g_free (priv->typed_address);
  priv->typed_address = g_strdup (address);

  if (expire == EPHY_WEB_VIEW_ADDRESS_EXPIRE_CURRENT &&
      !priv->is_loading) {
    priv->address_expire = EPHY_WEB_VIEW_ADDRESS_EXPIRE_NOW;
  } else {
    priv->address_expire = expire;
  }

  g_object_notify (G_OBJECT (view), "typed-address");
}

/**
 * ephy_web_view_has_modified_forms:
 * @view: an #EphyWebView
 *
 * Returns %TRUE if the user has modified &lt;input&gt; or &lt;textarea&gt;
 * values in @view's loaded document.
 *
 * Return value: %TRUE if @view has user-modified forms
 **/
gboolean
ephy_web_view_has_modified_forms (EphyWebView *view)
{
  return FALSE;
}

/**
 * ephy_web_view_get_location:
 * @view: an #EphyWebView
 * @toplevel: %FALSE to return the location of the focused frame only
 *
 * Returns the URL of the web page displayed in @view.
 *
 * If the web page contains frames, @toplevel will determine which location to
 * retrieve. If @toplevel is %TRUE, the return value will be the location of the
 * frameset document. If @toplevel is %FALSE, the return value will be the
 * location of the currently-focused frame.
 *
 * Return value: the URL of the web page displayed in @view
 **/
char *
ephy_web_view_get_location (EphyWebView *view,
                            gboolean toplevel)
{
  /* FIXME: follow the toplevel parameter */
  WebKitWebFrame *web_frame = webkit_web_view_get_main_frame (WEBKIT_WEB_VIEW (view));
  return g_strdup (webkit_web_frame_get_uri (web_frame));
}

/**
 * ephy_web_view_go_up:
 * @view: an #EphyWebView
 *
 * Moves @view one level up in its web page's directory hierarchy.
 **/
void
ephy_web_view_go_up (EphyWebView *view)
{
}

/**
 * ephy_web_view_get_js_status:
 * @view: an #EphyWebView
 *
 * Displays the message JavaScript is attempting to display in the statusbar.
 *
 * Note that Epiphany does not display JavaScript statusbar messages.
 *
 * Return value: a message from JavaScript meant to be displayed in the
 *		 statusbar
 **/
char *
ephy_web_view_get_js_status (EphyWebView *view)
{
  return NULL;
}

/**
 * ephy_web_view_get_security_level:
 * @view: an #EphyWebView
 * @level: return value of security level
 * @description: return value of the description of the security level
 *
 * Fetches the #EphyWebViewSecurityLevel and a newly-allocated string description
 * of the security state of @view.
 **/
void
ephy_web_view_get_security_level (EphyWebView *view,
                                  EphyWebViewSecurityLevel *level,
                                  char **description)
{
  if (level) {
    const gchar *uri = ephy_web_view_get_address (view);

    /* FIXME: as a temporary workaround, determine security level
       based on the existence of a 'https' prefix for the URI */
    if (uri && g_str_has_prefix(uri, "https"))
      *level = EPHY_WEB_VIEW_STATE_IS_SECURE_HIGH;
    else
      *level = EPHY_WEB_VIEW_STATE_IS_UNKNOWN;
  }
}

/**
 * ephy_web_view_show_page_certificate:
 * @view: an #EphyWebView
 *
 * Shows a dialogue displaying the certificate of the currently loaded page
 * of @view, if it was loaded over a secure connection; else does nothing.
 **/
void
ephy_web_view_show_page_certificate (EphyWebView *view)
{
}

/**
 * ephy_web_view_set_print_preview_mode:
 * @view: an #EphyWebView
 * @preview_mode: Whether the print preview mode is enabled.
 *
 * Enable and disable the print preview mode.
 **/
void
ephy_web_view_set_print_preview_mode (EphyWebView *view,
                                      gboolean preview_mode)
{
}

/**
 * ephy_web_view_print_preview_n_pages:
 * @view: an #EphyWebView
 *
 * Returns the number of pages which would appear in @view's loaded document
 * if it were to be printed.
 *
 * Return value: the number of pages in @view's loaded document
 **/
int
ephy_web_view_print_preview_n_pages (EphyWebView *view)
{
  return 0;
}

/**
 * ephy_web_view_print_preview_navigate:
 * @view: an #EphyWebView
 * @type: an #EphyPrintPreviewNavType which determines where to navigate
 * @page: if @type is %EPHY_WEB_VIEW_PRINTPREVIEW_GOTO_PAGENUM, the desired page number
 *
 * Navigates @view's print preview.
 **/
void
ephy_web_view_print_preview_navigate (EphyWebView *view,
                                      EphyWebViewPrintPreviewNavType type,
                                      int page)
{
}

/**
 * ephy_web_view_get_go_up_list:
 * @view: an #EphyWebView
 *
 * Returns a list of (%char *) URLs to higher-level directories on the same
 * server, in order of deepest to shallowest. For example, given
 * "http://www.example.com/dir/subdir/file.html", will return a list containing
 * "http://www.example.com/dir/subdir/", "http://www.example.com/dir/" and
 * "http://www.example.com/".
 *
 * Return value: a list of URLs higher up in @view's web page's directory
 * hierarchy
 **/
GSList *
ephy_web_view_get_go_up_list (EphyWebView *view)
{
  return NULL;
}