/* restore_gui.c
 * A module of J-Pilot http://jpilot.org
 * 
 * Copyright (C) 2001-2002 by Judd Montgomery
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include <gtk/gtk.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>


#include "utils.h"
#include "prefs.h"
#include "sync.h"
#include "log.h"
#include "i18n.h"


static GtkWidget *user_entry;
static GtkWidget *user_id_entry;
static GtkWidget *restore_clist;

static gboolean cb_restore_destroy(GtkWidget *widget)
{
   gtk_main_quit();
   return FALSE;
}

static void
cb_restore_ok(GtkWidget *widget,
	     gpointer   data)
{
   GList *list, *temp_list;
   char *text;
   char file[300];
   char home_dir[300];

   list=GTK_CLIST(restore_clist)->selection;

   get_home_file_name("", home_dir, 255);

   /* Remove anything that was supposed to be installed */
   g_snprintf(file, 298, "%s/"EPN"_to_install", home_dir);
   unlink(file);

   printf("---------- Restore ----------\n");
   for (temp_list=list; temp_list; temp_list = temp_list->next) {
      gtk_clist_get_text(GTK_CLIST(restore_clist), (int)temp_list->data, 0, &text);
      jp_logf(LOG_DEBUG, "row %ld [%s]\n", (long) temp_list->data, text);
      g_snprintf(file, 298, "%s/backup/%s", home_dir, text);
      install_append_line(file);
   }

   setup_sync(SYNC_NO_PLUGINS|SYNC_OVERRIDE_USER|SYNC_RESTORE);

   gtk_widget_destroy(data);
}

static void
cb_restore_quit(GtkWidget *widget,
	       gpointer   data)
{
   gtk_widget_destroy(data);
}

static int populate_clist()
{
   char *row_text[1];
   DIR *dir;
   struct dirent *dirent;
   char path[256];
   int i, num;

   get_home_file_name("backup", path, 255);

   cleanup_path(path);
   jp_logf(LOG_DEBUG, "opening dir %s\n", path);
   dir = opendir(path);
   num = 0;
   if (!dir) {
      jp_logf(LOG_DEBUG, "opening dir failed\n");
   } else {
      for (i=0; (dirent = readdir(dir)); i++) {
	 if (i>1000) {
	    jp_logf(LOG_WARN, "load_plugins_sub1(): infinite loop\n");
	    return -1;
	 }
	 if (dirent->d_name[0]=='.') {
	    continue;
	 }
	 if (!strncmp(dirent->d_name, "Unsaved Preferences", 17)) {
	    jp_logf(LOG_DEBUG, "skipping %s\n", dirent->d_name);
	    continue;
	 }
	 row_text[0]=dirent->d_name;
	 gtk_clist_append(GTK_CLIST(restore_clist), row_text);
      }
      num = i;
      closedir(dir);
   }
   for (i=0; i<num; i++) {
      gtk_clist_select_row(GTK_CLIST(restore_clist), i, 0);
   }

   return 0;
}


int restore_gui(int w, int h, int x, int y)
{
   GtkWidget *restore_window;
   GtkWidget *button;
   GtkWidget *vbox;
   GtkWidget *hbox;
   GtkWidget *scrolled_window;
   GtkWidget *label;
   const char *svalue;
   long ivalue;
   char str_int[10];

   jp_logf(LOG_DEBUG, "restore_gui()\n");

   restore_window = gtk_widget_new(GTK_TYPE_WINDOW,
				   "type", GTK_WINDOW_DIALOG,
				   "x", x, "y", y,
				   "width", w, "height", h,
				   "title", _("Restore Handheld"),
				   NULL);

   gtk_container_set_border_width(GTK_CONTAINER(restore_window), 5);

   gtk_window_set_default_size(GTK_WINDOW(restore_window), w, h);

   gtk_window_set_modal(GTK_WINDOW(restore_window), TRUE);

   gtk_signal_connect(GTK_OBJECT(restore_window), "destroy",
		      GTK_SIGNAL_FUNC(cb_restore_destroy), restore_window);

   vbox = gtk_vbox_new(FALSE, 0);
   gtk_container_add(GTK_CONTAINER(restore_window), vbox);

   /* Label for instructions */
   label = gtk_label_new(_("To restore your handheld:"));
   gtk_misc_set_alignment(GTK_MISC(label), 0, 0);   
   gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
   label = gtk_label_new(_("1. Choose all the applications you wish to restore.  The default is all."));
   gtk_misc_set_alignment(GTK_MISC(label), 0, 0);   
   gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
   label = gtk_label_new(_("2. Enter the User Name and User ID."));
   gtk_misc_set_alignment(GTK_MISC(label), 0, 0);   
   gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
   label = gtk_label_new(_("3. Press the OK button."));
   gtk_misc_set_alignment(GTK_MISC(label), 0, 0);   
   gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
   label = gtk_label_new(_("This will overwrite data that is currently on the handheld."));
   gtk_misc_set_alignment(GTK_MISC(label), 0, 0);   
   gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

   /* Put the memo list window up */
   scrolled_window = gtk_scrolled_window_new(NULL, NULL);
   gtk_container_set_border_width(GTK_CONTAINER(scrolled_window), 0);
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
				  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

   restore_clist = gtk_clist_new(1);

   gtk_clist_set_shadow_type(GTK_CLIST(restore_clist), SHADOW);
   gtk_clist_set_selection_mode(GTK_CLIST(restore_clist), GTK_SELECTION_EXTENDED);

   gtk_container_add(GTK_CONTAINER(scrolled_window), GTK_WIDGET(restore_clist));

   /* User entry */
   hbox = gtk_hbox_new(FALSE, 5);
   gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
   label = gtk_label_new(_("User Name"));
   gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
   user_entry = gtk_entry_new_with_max_length(126);
   get_pref(PREF_USER, NULL, &svalue);
   if (svalue) {
      gtk_entry_set_text(GTK_ENTRY(user_entry), svalue);
   }
   gtk_box_pack_start(GTK_BOX(hbox), user_entry, TRUE, TRUE, 0);


   /* User ID entry */
   hbox = gtk_hbox_new(FALSE, 5);
   gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
   label = gtk_label_new(_("User ID"));
   gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
   user_id_entry = gtk_entry_new_with_max_length(10);
   get_pref(PREF_USER_ID, &ivalue, NULL);
   sprintf(str_int, "%ld", ivalue);
   gtk_entry_set_text(GTK_ENTRY(user_id_entry), str_int);
   gtk_box_pack_start(GTK_BOX(hbox), user_id_entry, TRUE, TRUE, 0);


   hbox = gtk_hbox_new(FALSE, 0);
   gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

   button = gtk_button_new_with_label(_("OK"));
   gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
   gtk_signal_connect(GTK_OBJECT(button), "clicked",
		      GTK_SIGNAL_FUNC(cb_restore_ok), restore_window);

   button = gtk_button_new_with_label(_("Cancel"));
   gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
   gtk_signal_connect(GTK_OBJECT(button), "clicked",
		      GTK_SIGNAL_FUNC(cb_restore_quit), restore_window);

   populate_clist();

   gtk_widget_show_all(restore_window);

   gtk_main();

   return 0;
}
