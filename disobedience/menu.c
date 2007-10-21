/*
 * This file is part of DisOrder.
 * Copyright (C) 2006, 2007 Richard Kettlewell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */
/** @file disobedience/menu.c
 * @brief Main menu
 */

#include "disobedience.h"

static GtkWidget *selectall_widget;
static GtkWidget *properties_widget;

/** @brief Main menu widgets */
GtkItemFactory *mainmenufactory;

static void about_popup_got_version(void *v, const char *value);

/** @brief Called when the quit option is activated
 *
 * Just exits.
 */
static void quit_program(gpointer attribute((unused)) callback_data,
                         guint attribute((unused)) callback_action,
                         GtkWidget attribute((unused)) *menu_item) {
  D(("quit_program"));
  exit(0);
}

/* TODO can we have a single parameterized callback for all these */

/** @brief Called when the select all option is activated
 *
 * Calls the per-tab select all function.
 */
static void select_all(gpointer attribute((unused)) callback_data,
                       guint attribute((unused)) callback_action,
                       GtkWidget attribute((unused)) *menu_item) {
  GtkWidget *tab = gtk_notebook_get_nth_page
    (GTK_NOTEBOOK(tabs), gtk_notebook_current_page(GTK_NOTEBOOK(tabs)));
  const struct tabtype *t = g_object_get_data(G_OBJECT(tab), "type");

  t->selectall_activate(tab);
}

/** @brief Called when the track properties option is activated
 *
 * Calls the per-tab properties function.
 */
static void properties_item(gpointer attribute((unused)) callback_data,
                            guint attribute((unused)) callback_action,
                            GtkWidget attribute((unused)) *menu_item) {
  GtkWidget *tab = gtk_notebook_get_nth_page
    (GTK_NOTEBOOK(tabs), gtk_notebook_current_page(GTK_NOTEBOOK(tabs)));
  const struct tabtype *t = g_object_get_data(G_OBJECT(tab), "type");

  t->properties_activate(tab);
}

/** @brief Called when the login option is activated */
static void login(gpointer attribute((unused)) callback_data,
                  guint attribute((unused)) callback_action,
                  GtkWidget attribute((unused)) *menu_item) {
  login_box();
}

/** @brief Update menu state
 *
 * Determines option sensitivity according to the current tab and adjusts the
 * widgets accordingly.  Knows about @ref DISORDER_CONNECTED so the callbacks
 * need not.
 */
void menu_update(int page) {
  GtkWidget *tab = gtk_notebook_get_nth_page
    (GTK_NOTEBOOK(tabs),
     page < 0 ? gtk_notebook_current_page(GTK_NOTEBOOK(tabs)) : page);
  const struct tabtype *t = g_object_get_data(G_OBJECT(tab), "type");

  assert(t != 0);
  gtk_widget_set_sensitive(properties_widget,
                           (t->properties_sensitive(tab)
                            && (disorder_eclient_state(client) & DISORDER_CONNECTED)));
  gtk_widget_set_sensitive(selectall_widget,
                           t->selectall_sensitive(tab));
}
   
/** @brief Fetch version in order to display the about... popup */
static void about_popup(gpointer attribute((unused)) callback_data,
                        guint attribute((unused)) callback_action,
                        GtkWidget attribute((unused)) *menu_item) {
  D(("about_popup"));

  gtk_label_set_text(GTK_LABEL(report_label), "getting server version");
  disorder_eclient_version(client,
                           about_popup_got_version,
                           0);
}

/** @brief Callde when version arrives, displays about... popup */
static void about_popup_got_version(void attribute((unused)) *v,
                                    const char *value) {
  GtkWidget *w;
  char *server_version_string;

  byte_xasprintf(&server_version_string, "Server version %s", value);
  w = gtk_dialog_new_with_buttons("About DisOrder",
                                  GTK_WINDOW(toplevel),
                                  (GTK_DIALOG_MODAL
                                   |GTK_DIALOG_DESTROY_WITH_PARENT),
                                  GTK_STOCK_OK,
                                  GTK_RESPONSE_ACCEPT,
                                  (char *)NULL);
  gtk_container_add(GTK_CONTAINER(GTK_DIALOG(w)->vbox),
                    gtk_label_new("DisOrder client " VERSION));
  gtk_container_add(GTK_CONTAINER(GTK_DIALOG(w)->vbox),
                    gtk_label_new(server_version_string));
  gtk_container_add(GTK_CONTAINER(GTK_DIALOG(w)->vbox),
                    gtk_label_new("(c) 2004-2007 Richard Kettlewell"));
  gtk_widget_show_all(w);
  gtk_dialog_run(GTK_DIALOG(w));
  gtk_widget_destroy(w);
}

/** @brief Create the menu bar widget */
GtkWidget *menubar(GtkWidget *w) {
  static const GtkItemFactoryEntry entries[] = {
    {
      (char *)"/File",                  /* path */
      0,                                /* accelerator */
      0,                                /* callback */
      0,                                /* callback_action */
      (char *)"<Branch>",               /* item_type */
      0                                 /* extra_data */
    },
    { 
      (char *)"/File/Login",            /* path */
      (char *)"<CTRL>L",                /* accelerator */
      login,                            /* callback */
      0,                                /* callback_action */
      0,                                /* item_type */
      0                                 /* extra_data */
    },
    {
      (char *)"/File/Quit Disobedience", /* path */
      (char *)"<CTRL>Q",                /* accelerator */
      quit_program,                     /* callback */
      0,                                /* callback_action */
      (char *)"<StockItem>",            /* item_type */
      GTK_STOCK_QUIT                    /* extra_data */
    },
    
    {
      (char *)"/Edit",                  /* path */
      0,                                /* accelerator */
      0,                                /* callback */
      0,                                /* callback_action */
      (char *)"<Branch>",               /* item_type */
      0                                 /* extra_data */
    },
    {
      (char *)"/Edit/Select all tracks", /* path */
      (char *)"<CTRL>A",                /* accelerator */
      select_all,                       /* callback */
      0,                                /* callback_action */
      0,                                /* item_type */
      0                                 /* extra_data */
    },
    {
      (char *)"/Edit/Track properties", /* path */
      0,                                /* accelerator */
      properties_item,                  /* callback */
      0,                                /* callback_action */
      0,                                /* item_type */
      0                                 /* extra_data */
    },
    
    {
      (char *)"/Control",               /* path */
      0,                                /* accelerator */
      0,                                /* callback */
      0,                                /* callback_action */
      (char *)"<Branch>",               /* item_type */
      0                                 /* extra_data */
    },
    {
      (char *)"/Control/Scratch",       /* path */
      (char *)"<CTRL>S",                /* accelerator */
      0,                                /* callback */
      0,                                /* callback_action */
      0,                                /* item_type */
      0                                 /* extra_data */
    },
    {
      (char *)"/Control/Random play",   /* path */
      (char *)"<CTRL>R",                /* accelerator */
      0,                                /* callback */
      0,                                /* callback_action */
      (char *)"<CheckItem>",            /* item_type */
      0                                 /* extra_data */
    },
    
    {
      (char *)"/Help",                  /* path */
      0,                                /* accelerator */
      0,                                /* callback */
      0,                                /* callback_action */
      (char *)"<Branch>",               /* item_type */
      0                                 /* extra_data */
    },
    {
      (char *)"/Help/About DisOrder",   /* path */
      0,                                /* accelerator */
      about_popup,                      /* callback */
      0,                                /* callback_action */
      (char *)"<StockItem>",            /* item_type */
      GTK_STOCK_ABOUT                   /* extra_data */
    },
  };

  GtkAccelGroup *accel = gtk_accel_group_new();

  D(("add_menubar"));
  /* TODO: item factories are deprecated in favour of some XML thing */
  mainmenufactory = gtk_item_factory_new(GTK_TYPE_MENU_BAR, "<GdisorderMain>",
                                         accel);
  gtk_item_factory_create_items(mainmenufactory,
                                sizeof entries / sizeof *entries,
                                (GtkItemFactoryEntry *)entries,
                                0);
  gtk_window_add_accel_group(GTK_WINDOW(w), accel);
  selectall_widget = gtk_item_factory_get_widget(mainmenufactory,
						 "<GdisorderMain>/Edit/Select all tracks");
  properties_widget = gtk_item_factory_get_widget(mainmenufactory,
						  "<GdisorderMain>/Edit/Track properties");
  assert(selectall_widget != 0);
  assert(properties_widget != 0);
  return gtk_item_factory_get_widget(mainmenufactory,
                                     "<GdisorderMain>");
  /* menu bar had better not expand vertically if the window is too big */
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
