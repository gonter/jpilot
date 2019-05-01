/*******************************************************************************
 * alarms.c
 * A module of J-Pilot http://jpilot.org
 *
 * Copyright (C) 2000-2014 by Judd Montgomery
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
 ******************************************************************************/
/*
 * The PalmOS datebook will alarm on private records even when they are hidden
 * and they show up on the screen.  Right, or wrong, who knows.
 * I will do the same.
 *
 * Throughout the code, event_time is the time of the event
 * alarm_time is the event_time - advance
 * remind_time is the time a window is to be popped up (it may be postponed)
 */

/********************************* Includes ***********************************/
#include "config.h"
#include <gtk/gtk.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pi-calendar.h>

#include "alarms.h"
#include "i18n.h"
#include "utils.h"
#include "calendar.h"
#include "log.h"
#include "prefs.h"

/********************************* Constants **********************************/
/* This is how often to check for alarms in seconds */
/* Every call takes CPU time(not much), so you may want it to be greater */
#define ALARM_INTERVAL 10

/* Constants used in converting to seconds */
#define MIN_IN_SECS 60
#define HR_IN_SECS  3600
#define DAY_IN_SECS 86400

#define PREV_ALARM_MASK 1
#define NEXT_ALARM_MASK 2

/* Uncomment for verbose debugging of the alarm code */
/* #define ALARMS_DEBUG */

/******************************* Global vars **********************************/
/* main jpilot window */
extern GtkWidget *window;

static struct jp_alarms *alarm_list=NULL;
static struct jp_alarms *Plast_alarm_list=NULL;
static struct jp_alarms *next_alarm=NULL;

static int glob_skip_all_alarms;
static int total_alarm_windows;

/****************************** Prototypes ************************************/
typedef enum {
   ALARM_NONE = 0,
   ALARM_NEW,
   ALARM_MISSED,
   ALARM_POSTPONED
} AlarmType;

struct jp_alarms {
   unsigned int unique_id;
   AlarmType type;
   time_t event_time;
   time_t alarm_advance;
   struct jp_alarms *next;
};

struct alarm_dialog_data {
   unsigned int unique_id;
   time_t remind_time;
   GtkWidget *remind_entry;
   GtkWidget *radio1;
   GtkWidget *radio2;
   int button_hit;
};

static void alarms_add_to_list(unsigned int unique_id,
                               AlarmType type,
                               time_t alarm_time,
                               time_t alarm_advance);

/****************************** Main Code *************************************/
/* Alarm GUI */

/* Start of Dialog window code */
static void cb_dialog_button(GtkWidget *widget, gpointer data)
{
   struct alarm_dialog_data *Pdata;
   GtkWidget *w;

   w = gtk_widget_get_toplevel(widget);
   Pdata = gtk_object_get_data(GTK_OBJECT(w), "alarm");
   if (Pdata) {
      Pdata->button_hit = GPOINTER_TO_INT(data);
   }
   gtk_widget_destroy(w);
}

static gboolean cb_destroy_dialog(GtkWidget *widget)
{
   struct alarm_dialog_data *Pdata;
   time_t ltime;
   time_t advance;
   time_t remind;

   total_alarm_windows--;
#ifdef ALARMS_DEBUG
   printf("total_alarm_windows=%d\n",total_alarm_windows);
#endif
   Pdata = gtk_object_get_data(GTK_OBJECT(widget), "alarm");
   if (!Pdata) {
      return FALSE;
   }
   if (Pdata->button_hit==DIALOG_SAID_2) {
      remind = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(Pdata->remind_entry));
      jp_logf(JP_LOG_DEBUG, "remind = [%d]\n", remind);
      set_pref(PREF_REMIND_IN, remind, NULL, TRUE);
      if (GTK_TOGGLE_BUTTON(Pdata->radio1)->active) {
         set_pref(PREF_REMIND_UNITS, 0, NULL, TRUE);
         remind *= MIN_IN_SECS;
      } else {
         set_pref(PREF_REMIND_UNITS, 1, NULL, TRUE);
         remind *= HR_IN_SECS;
      }
      time(&ltime);
      localtime(&ltime);
      advance = -(ltime + remind - Pdata->remind_time);
      alarms_add_to_list(Pdata->unique_id,
                         ALARM_POSTPONED,
                         Pdata->remind_time,
                         advance);
   }
   free(Pdata);

   /* Done with cleanup.  Let GTK continue with removing widgets */
   return FALSE;
}

static int dialog_alarm(char *title, char *reason,
                        char *time_str, char *desc_str, char *note_str,
                        unsigned int unique_id,
                        time_t remind_time)
{
   GSList *group;
   GtkWidget *button, *label;
   GtkWidget *hbox1, *vbox1;
   GtkWidget *vbox_temp;
   GtkWidget *alarm_dialog;
   GtkWidget *remind_entry;
   GtkWidget *radio1;
   GtkWidget *radio2;
   struct alarm_dialog_data *Pdata;
   long pref_units;
   long pref_entry;
   GtkWidget *image;
   char *markup;

   /* Prevent alarms from going crazy and using all resources */
   if (total_alarm_windows > 20) {
      return EXIT_FAILURE;
   }
   total_alarm_windows++;
#ifdef ALARMS_DEBUG
   printf("total_alarm_windows=%d\n",total_alarm_windows);
#endif
   alarm_dialog = gtk_widget_new(GTK_TYPE_WINDOW,
                                 "type", GTK_WINDOW_TOPLEVEL,
                                 "title", title,
                                 NULL);

   gtk_signal_connect(GTK_OBJECT(alarm_dialog), "destroy",
                      GTK_SIGNAL_FUNC(cb_destroy_dialog), alarm_dialog);

   gtk_window_set_transient_for(GTK_WINDOW(alarm_dialog), GTK_WINDOW(window));
   gtk_window_stick(GTK_WINDOW(alarm_dialog));

   vbox1 = gtk_vbox_new(FALSE, 5);
   gtk_container_add(GTK_CONTAINER(alarm_dialog), vbox1);

   hbox1 = gtk_hbox_new(FALSE, 5);
   gtk_box_pack_start(GTK_BOX(vbox1), hbox1, FALSE, FALSE, 0);

   image = gtk_image_new_from_stock(GTK_STOCK_DIALOG_INFO, GTK_ICON_SIZE_DIALOG);
   gtk_box_pack_start(GTK_BOX(hbox1), image, FALSE, FALSE, 12);

   /* Label */
   label = gtk_label_new("");
   if (note_str[0] == '\0') {
      markup = g_markup_printf_escaped("<b><big>%s</big></b>\n\n%s\n\n%s",
               desc_str, reason, time_str);
   } else {
      markup = g_markup_printf_escaped("<b><big>%s</big></b>\n\n%s\n\n%s\n\n%s",
               desc_str, reason, time_str, note_str);
   }
   gtk_label_set_markup(GTK_LABEL(label), markup);
   g_free(markup);
   gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
   gtk_box_pack_start(GTK_BOX(hbox1), label, FALSE, FALSE, 6);

   /* remind delay */
   hbox1 = gtk_hbox_new(FALSE, 0);
   remind_entry = gtk_spin_button_new_with_range(0, 59, 1);
   gtk_box_pack_start(GTK_BOX(hbox1), remind_entry, FALSE, FALSE, 2);

   vbox_temp = gtk_vbox_new(FALSE, 0);
   gtk_box_pack_start(GTK_BOX(hbox1), vbox_temp, FALSE, TRUE, 4);

   radio1 = gtk_radio_button_new_with_label(NULL, _("Minutes"));
   group = gtk_radio_button_group(GTK_RADIO_BUTTON(radio1));
   radio2 = gtk_radio_button_new_with_label(group, _("Hours"));
   gtk_radio_button_group(GTK_RADIO_BUTTON(radio2));

   gtk_box_pack_start(GTK_BOX(vbox_temp), radio1, TRUE, TRUE, 0);
   gtk_box_pack_start(GTK_BOX(vbox_temp), radio2, TRUE, TRUE, 0);

   gtk_box_pack_start(GTK_BOX(vbox1), hbox1, TRUE, TRUE, 2);

   get_pref(PREF_REMIND_IN, &pref_entry, NULL);
   gtk_spin_button_set_value(GTK_SPIN_BUTTON(remind_entry), pref_entry);

   get_pref(PREF_REMIND_UNITS, &pref_units, NULL);
   if (pref_units) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio2), TRUE);
   } else {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio1), TRUE);
   }

   /* Buttons */
   gtk_container_set_border_width(GTK_CONTAINER(hbox1), 12);

   button = gtk_button_new_with_label(_("Remind me"));
   gtk_signal_connect(GTK_OBJECT(button), "clicked",
                      GTK_SIGNAL_FUNC(cb_dialog_button),
                      GINT_TO_POINTER(DIALOG_SAID_2));
   gtk_box_pack_start(GTK_BOX(hbox1), button, TRUE, TRUE, 4);

   button = gtk_button_new_from_stock(GTK_STOCK_CLOSE);
   gtk_signal_connect(GTK_OBJECT(button), "clicked",
                      GTK_SIGNAL_FUNC(cb_dialog_button),
                      GINT_TO_POINTER(DIALOG_SAID_1));
   gtk_box_pack_start(GTK_BOX(hbox1), button, TRUE, TRUE, 4);

   Pdata = malloc(sizeof(struct alarm_dialog_data));
   if (Pdata) {
      Pdata->unique_id = unique_id;
      Pdata->remind_time = remind_time;
      /* Set the default button pressed to OK */
      Pdata->button_hit = DIALOG_SAID_1;
      Pdata->remind_entry=remind_entry;
      Pdata->radio1=radio1;
      Pdata->radio2=radio2;
   }
   gtk_object_set_data(GTK_OBJECT(alarm_dialog), "alarm", Pdata);

   gtk_widget_show_all(alarm_dialog);

   return EXIT_SUCCESS;
}
/* End Alarm GUI */

static time_t tm_copy_with_dst_adj(struct tm *dest, struct tm *src)
{
   memcpy(dest, src, sizeof(struct tm));
   dest->tm_isdst=-1;
   return mktime(dest);
}

#ifdef ALARMS_DEBUG
static const char *print_date(const time_t t1)
{
   struct tm *Pnow;
   static char str[100];

   Pnow = localtime(&t1);
   strftime(str, sizeof(str), "%B %d, %Y %H:%M:%S", Pnow);
   return str;
}
static const char *print_type(AlarmType type)
{
   switch (type) {
    case ALARM_NONE:
      return "ALARM_NONE";
    case ALARM_NEW:
      return "ALARM_NEW";
    case ALARM_MISSED:
      return "ALARM_MISSED";
    case ALARM_POSTPONED:
      return "ALARM_POSTPONED";
    default:
      return "? ALARM_UNKNOWN";
   }
}
#endif

static void alarms_add_to_list(unsigned int unique_id,
                        AlarmType type,
                        time_t event_time,
                        time_t alarm_advance)
{
   struct jp_alarms *temp_alarm;

#ifdef ALARMS_DEBUG
   printf("alarms_add_to_list()\n");
#endif

   temp_alarm = malloc(sizeof(struct jp_alarms));
   if (!temp_alarm) {
      jp_logf(JP_LOG_WARN, "alarms_add_to_list: %s\n", _("Out of memory"));
      return;
   }
   temp_alarm->unique_id = unique_id;
   temp_alarm->type = type;
   temp_alarm->event_time = event_time;
   temp_alarm->alarm_advance = alarm_advance;
   temp_alarm->next = NULL;
   if (Plast_alarm_list) {
      Plast_alarm_list->next=temp_alarm;
      Plast_alarm_list=temp_alarm;
   } else {
      alarm_list=Plast_alarm_list=temp_alarm;
   }
   Plast_alarm_list=temp_alarm;
}

static void alarms_remove_from_to_list(unsigned int unique_id)
{
   struct jp_alarms *temp_alarm, *prev_alarm, *next_alarm;

#ifdef ALARMS_DEBUG
   printf("remove from list(%d)\n", unique_id);
#endif
   for(prev_alarm=NULL, temp_alarm=alarm_list;
       temp_alarm;
       temp_alarm=next_alarm) {
      if (temp_alarm->unique_id==unique_id) {
         /* Tail of list? */
         if (temp_alarm->next==NULL) {
            Plast_alarm_list=prev_alarm;
         }
         /* Last of list? */
         if (Plast_alarm_list==alarm_list) {
            Plast_alarm_list=alarm_list=NULL;
         }
         if (prev_alarm) {
            prev_alarm->next=temp_alarm->next;
         } else {
            /* Head of list */
            alarm_list=temp_alarm->next;
         }
         free(temp_alarm);
         return;
      } else {
         prev_alarm=temp_alarm;
         next_alarm=temp_alarm->next;
      }
   }
}

static void free_alarms_list(int mask)
{
   struct jp_alarms *ta, *ta_next;

   if (mask&PREV_ALARM_MASK) {
      for (ta=alarm_list; ta; ta=ta_next) {
         ta_next=ta->next;
         free(ta);
      }
      Plast_alarm_list=alarm_list=NULL;
   }

   if (mask&NEXT_ALARM_MASK) {
      for (ta=next_alarm; ta; ta=ta_next) {
         ta_next=ta->next;
         free(ta);
      }
      next_alarm=NULL;
   }
}

static void alarms_write_file(void)
{
   FILE *out;
   char line[256];
   int fail, n;
   time_t ltime;
   struct tm *now;

   jp_logf(JP_LOG_DEBUG, "alarms_write_file()\n");

   time(&ltime);
   now = localtime(&ltime);
   /* Alarm is triggered within ALARM_INTERVAL/2 seconds of the minute.
    * Potentially need to round timestamp up to the correct minute. */
   if ((59 - now->tm_sec) <= ALARM_INTERVAL/2) {
      now->tm_min++;
      mktime(now);
   }
   
   out=jp_open_home_file(EPN".alarms.tmp", "w");
   if (!out) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s%s\n"), EPN, ".alarms.tmp");
      return;
   }
   fail=0;
   g_snprintf(line, sizeof(line), "%s",
           "# This file was generated by "EPN", changes will be lost\n");
   n = fwrite(line, strlen(line), 1, out);
   if (n<1) fail=1;

   g_snprintf(line, sizeof(line), "%s",
           "# This is the last time that "EPN" was ran\n");
   n = fwrite(line, strlen(line), 1, out);
   if (n<1) fail=1;

   sprintf(line, "UPTODATE %d %d %d %d %d\n",
           now->tm_year+1900,
           now->tm_mon+1,
           now->tm_mday,
           now->tm_hour,
           now->tm_min
          );
   n = fwrite(line, strlen(line), 1, out);
   if (n<1) fail=1;

   fclose(out);

   if (fail) {
      unlink_file(EPN".alarms.tmp");
   } else {
      rename_file(EPN".alarms.tmp", EPN".alarms");
   }
}

/*
 * This attempts to make the command safe.
 * I'm sure I'm missing things.
 */
static void make_command_safe(char *command)
{
   int i, len;
   char c;

   len = strlen(command);
   for (i=0; i<len; i++) {
      c=command[i];
      if (strchr("\r\n|&;()<>", c)) {
         command[i]=' ';
      }
   }
}

/*
 * Process an alarm occurrence
 *   Pop up alarm window.
 *   Do alarm setting (play sound, or whatever).
 *   if user postpones then put in postponed alarm list.
 */
static int alarms_do_one(struct CalendarEvent *cale,
                         unsigned long unique_id,
                         time_t t_alarm,
                         AlarmType type)
{
   struct tm *Pnow;
   struct tm begin;
   struct tm end;
   char time_str[255];
   char desc_str[255];
   char note_str[255];
   char pref_time[50];
   char time1_str[50];
   char time2_str[50];
   char date_str[50];
   char command[1024];
   char *reason;
   long wants_windows;
   long do_command;
   const char *pref_date;
   const char *pref_command;
   char c1, c2;
   int i, len;

   alarms_write_file();

   switch (type) {
    case ALARM_NONE:
      return EXIT_SUCCESS;
    case ALARM_NEW:
      reason=_("Appointment Reminder");
      break;
    case ALARM_MISSED:
      reason=_("Past Appointment");
      break;
    case ALARM_POSTPONED:
      reason=_("Postponed Appointment");
      break;
    default:
      reason=_("Appointment");
   }
   get_pref(PREF_SHORTDATE, NULL, &pref_date);
   get_pref_time_no_secs(pref_time);

   Pnow = localtime(&t_alarm);

   strftime(date_str, sizeof(date_str), pref_date, Pnow);
   tm_copy_with_dst_adj(&begin, &(cale->begin));
   strftime(time1_str, sizeof(time1_str), pref_time, &begin);
   tm_copy_with_dst_adj(&end, &(cale->end));
   strftime(time2_str, sizeof(time2_str), pref_time, &end);
   if (strcmp(time1_str,time2_str) == 0)
      g_snprintf(time_str, sizeof(time_str), "%s %s", date_str, time1_str);
   else
      g_snprintf(time_str, sizeof(time_str), "%s %s-%s", date_str, time1_str, time2_str);

   desc_str[0]='\0';
   note_str[0]='\0';
   if (cale->description) {
      g_strlcpy(desc_str, cale->description, sizeof(desc_str));
   }
   if (cale->note) {
      g_strlcpy(note_str, cale->note, sizeof(note_str));
   }

   get_pref(PREF_ALARM_COMMAND, NULL, &pref_command);
   get_pref(PREF_DO_ALARM_COMMAND, &do_command, NULL);
#ifdef ALARMS_DEBUG
   printf("pref_command = [%s]\n", pref_command);
#endif
   memset(command, 0, sizeof(command));
   if (do_command) {
      command[0]='\0';
      for (i=0; i<MAX_PREF_LEN-1; i++) {
         c1 = pref_command[i];
         len = strlen(command);
         if (c1=='%') {
            c2 = pref_command[i+1];
            /* expand '%t' */
            if (c2=='t') {
               i++;
               strncat(command, time1_str, sizeof(command)-2-len);
               continue;
            }
            /* expand '%d' */
            if (c2=='d') {
               i++;
               strncat(command, date_str, sizeof(command)-2-len);
               continue;
            }
#ifdef ENABLE_ALARM_SHELL_DANGER
            /* expand '%D' */
            if (c2=='D') {
               i++;
               strncat(command, desc_str, sizeof(command)-2-len);
               continue;
            }
            /* expand '%N' */
            if (c2=='N') {
               i++;
               strncat(command, note_str, sizeof(command)-2-len);
               continue;
            }
#endif
         }
         if (len<sizeof(command)-4) {
            command[len++]=c1;
            command[len]='\0';
         }
         if (c1=='\0') {
            break;
         }
      }
      command[sizeof(command)-2]='\0';

      make_command_safe(command);
      jp_logf(JP_LOG_STDOUT|JP_LOG_FILE, _("executing command = [%s]\n"), command);
      if (system(command) == -1) {
         jp_logf(JP_LOG_WARN, "system call failed %s %d\n", __FILE__, __LINE__);
      }
   }

   get_pref(PREF_OPEN_ALARM_WINDOWS, &wants_windows, NULL);

   if (wants_windows) {
      return dialog_alarm(_("J-Pilot Alarm"), reason,
                          time_str, desc_str, note_str,
                          unique_id,
                          t_alarm);
   }
   return EXIT_SUCCESS;
}

/*
 * See if next_alarm is due in less than ALARM_INTERVAL/2 secs.
 * If it is, then do_alarm and find_next_alarm.
 */
static gint cb_timer_alarms(gpointer data)
{
   struct jp_alarms *temp_alarm, *ta_next;
   CalendarEventList *alm_list;
   CalendarEventList *temp_al;
   static int first=1;
   time_t t, diff;
   time_t t_alarm_time;
   struct tm *Ptm;
   struct tm copy_tm;

   alm_list=NULL;

   if (first) {
      alarms_write_file();
      first=0;
   }

   time(&t);

   for (temp_alarm=alarm_list; temp_alarm; temp_alarm=ta_next) {
      ta_next=temp_alarm->next;
      diff = temp_alarm->event_time - t - temp_alarm->alarm_advance;
      if (temp_alarm->type!=ALARM_MISSED) {
         if (diff >= ALARM_INTERVAL/2) {
            continue;
         }
      }
      if (alm_list==NULL) {
         get_days_calendar_events2(&alm_list, NULL, 0, 0, 1, CATEGORY_ALL, NULL);
      }
#ifdef ALARMS_DEBUG
      printf("unique_id=%d\n", temp_alarm->unique_id);
      printf("type=%s\n", print_type(temp_alarm->type));
      printf("event_time=%s\n", print_date(temp_alarm->event_time));
      printf("alarm_advance=%ld\n", temp_alarm->alarm_advance);
#endif
      for (temp_al = alm_list; temp_al; temp_al=temp_al->next) {
         if (temp_al->mcale.unique_id == temp_alarm->unique_id) {
#ifdef ALARMS_DEBUG
            printf("%s\n", temp_al->mcale.cale.description);
#endif
            alarms_do_one(&(temp_al->mcale.cale),
                          temp_alarm->unique_id,
                          temp_alarm->event_time,
                          ALARM_MISSED);
            break;
         }
      }
      /* CAUTION, this modifies the list we are parsing and
       * removes the current node */
      if (temp_al)
         alarms_remove_from_to_list(temp_al->mcale.unique_id);
   }

   if (next_alarm) {
      diff = next_alarm->event_time - t - next_alarm->alarm_advance;
      if (diff <= ALARM_INTERVAL/2) {
         if (alm_list==NULL) {
            get_days_calendar_events2(&alm_list, NULL, 0, 0, 1, CATEGORY_ALL, NULL);
         }
         for (temp_alarm=next_alarm; temp_alarm; temp_alarm=ta_next) {
            for (temp_al = alm_list; temp_al; temp_al=temp_al->next) {
               if (temp_al->mcale.unique_id == temp_alarm->unique_id) {
#ifdef ALARMS_DEBUG
                  printf("** next unique_id=%d\n", temp_alarm->unique_id);
                  printf("** next type=%s\n", print_type(temp_alarm->type));
                  printf("** next event_time=%s\n", print_date(temp_alarm->event_time));
                  printf("** next alarm_advance=%ld\n", temp_alarm->alarm_advance);
                  printf("** next %s\n", temp_al->mcale.cale.description);
#endif
                  alarms_do_one(&(temp_al->mcale.cale),
                                temp_alarm->unique_id,
                                temp_alarm->event_time,
                                ALARM_NEW);
                  break;
               }
            }
            /* This may not be exactly right */
            t_alarm_time = temp_alarm->event_time + 1;
#ifdef ALARMS_DEBUG
            printf("** t_alarm_time-->%s\n", print_date(t_alarm_time));
#endif
            ta_next=temp_alarm->next;
            free(temp_alarm);
            next_alarm = ta_next;
         }
         Ptm = localtime(&t_alarm_time);
         memcpy(&copy_tm, Ptm, sizeof(struct tm));
         alarms_find_next(&copy_tm, &copy_tm, TRUE);
      }
   }
   if (alm_list) {
      free_CalendarEventList(&alm_list);
   }

   return TRUE;
}


/*
 * Find the next appointment alarm
 *  if soonest_only then return the next alarm, 
 *  else return all alarms that occur between the two dates.
 */
int alarms_find_next(struct tm *date1_in, struct tm *date2_in, int soonest_only)
{
   CalendarEventList *alm_list;
   CalendarEventList *temp_al;
   struct jp_alarms *ta;

   time_t adv;
   time_t ltime;
   time_t t1, t2;
   time_t t_alarm;
   time_t t_end;
   time_t t_prev;
   time_t t_future;
   struct tm *tm_temp;
   struct tm date1, date2;
   struct tm tm_prev, tm_next;
   int prev_found, next_found;
   int add_a_next;

   jp_logf(JP_LOG_DEBUG, "alarms_find_next()\n");

   if (glob_skip_all_alarms) return EXIT_SUCCESS;

   if (!date1_in) {
      time(&ltime);
      tm_temp = localtime(&ltime);
   } else {
      tm_temp=date1_in;
   }
   memset(&date1, 0, sizeof(date1));
   date1.tm_year=tm_temp->tm_year;
   date1.tm_mon=tm_temp->tm_mon;
   date1.tm_mday=tm_temp->tm_mday;
   date1.tm_hour=tm_temp->tm_hour;
   date1.tm_min=tm_temp->tm_min;
   date1.tm_sec=tm_temp->tm_sec;
   date1.tm_isdst=tm_temp->tm_isdst;

   if (!date2_in) {
      time(&ltime);
      tm_temp = localtime(&ltime);
   } else {
      tm_temp=date2_in;
   }
   memset(&date2, 0, sizeof(date2));
   date2.tm_year=tm_temp->tm_year;
   date2.tm_mon=tm_temp->tm_mon;
   date2.tm_mday=tm_temp->tm_mday;
   date2.tm_hour=tm_temp->tm_hour;
   date2.tm_min=tm_temp->tm_min;
   date2.tm_sec=tm_temp->tm_sec;
   date2.tm_isdst=tm_temp->tm_isdst;

   t1=mktime_dst_adj(&date1);
   t2=mktime_dst_adj(&date2);

#ifdef ALARMS_DEBUG
   char str[100];
   struct tm *Pnow;

   strftime(str, sizeof(str), "%B %d, %Y %H:%M", &date1);
   printf("date1=%s\n", str);
   strftime(str, sizeof(str), "%B %d, %Y %H:%M", &date2);
   printf("date2=%s\n", str);
   Pnow = localtime(&t1);
   strftime(str, sizeof(str), "%B %d, %Y %H:%M", Pnow);
   printf("[Now]=%s\n", str);
#endif

   if (!soonest_only) {
      free_alarms_list(PREV_ALARM_MASK | NEXT_ALARM_MASK);
   } else {
      free_alarms_list(NEXT_ALARM_MASK);
   }

   alm_list=NULL;
   get_days_calendar_events2(&alm_list, NULL, 0, 0, 1, CATEGORY_ALL, NULL);

   for (temp_al=alm_list; temp_al; temp_al=temp_al->next) {
      /* No alarm, skip */
      if (!temp_al->mcale.cale.alarm) {
         continue;
      }
#ifdef ALARMS_DEBUG
      printf("\n[%s]\n", temp_al->mcale.cale.description);
#endif
      /* Check for ordinary non-repeating appt starting before date1 */
      if (temp_al->mcale.cale.repeatType == calendarRepeatNone) {
         t_alarm = mktime_dst_adj(&(temp_al->mcale.cale.begin));
         if (t_alarm < t1) {
#ifdef ALARMS_DEBUG
            printf("afn: non repeat before t1, t_alarm<t1, %ld<%ld\n",t_alarm,t1);
#endif
            continue;
         }
      }

      /* Check that we are not past any appointment end date */
      if (!(temp_al->mcale.cale.repeatForever)) {
         t_end = mktime_dst_adj(&(temp_al->mcale.cale.repeatEnd));
         /* We need to add 24 hours to the end date to make it inclusive */
         t_end += DAY_IN_SECS;
         if (t_end < t2) {
#ifdef ALARMS_DEBUG
            printf("afn: past end date\n");
#endif
            continue;
         }
      }

      /* Calculate the alarm advance in seconds */
      adv = 0;
      switch (temp_al->mcale.cale.advanceUnits) {
       case advMinutes:
         adv = temp_al->mcale.cale.advance*MIN_IN_SECS;
         break;
       case advHours:
         adv = temp_al->mcale.cale.advance*HR_IN_SECS;
         break;
       case advDays:
         adv = temp_al->mcale.cale.advance*DAY_IN_SECS;
         break;
      }

#ifdef ALARMS_DEBUG
      printf("alarm advance %d ", temp_al->mcale.cale.advance);
      switch (temp_al->mcale.cale.advanceUnits) {
       case advMinutes:
         printf("minutes\n");
         break;
       case advHours:
         printf("hours\n");
         break;
       case advDays:
         printf("days\n");
         break;
      }
      printf("adv=%ld\n", adv);
#endif

      prev_found=next_found=0;

      find_prev_next(&(temp_al->mcale.cale),
                     adv,
                     &date1,
                     &date2,
                     &tm_prev,
                     &tm_next,
                     &prev_found,
                     &next_found);
      t_prev=mktime_dst_adj(&tm_prev);
      t_future=mktime_dst_adj(&tm_next);

      /* Skip the alarms if they are before date1 or after date2 */
      if (prev_found) {
         if (t_prev - adv < t1) {
#ifdef ALARMS_DEBUG
            printf("failed prev is before t1\n");
#endif
            prev_found=0;
         }
         if (t_prev - adv > t2) {
#ifdef ALARMS_DEBUG
            printf("failed prev is after t2\n");
#endif
            continue;
         }
      }
      if (next_found) {
         /* Check that we are not past any appointment end date */
         if (!(temp_al->mcale.cale.repeatForever)) {
            t_end = mktime_dst_adj(&(temp_al->mcale.cale.repeatEnd));
            /* We need to add 24 hours to the end date to make it inclusive */
            t_end += DAY_IN_SECS;
            if (t_future > t_end) {
#ifdef ALARMS_DEBUG
               printf("failed future is after t_end\n");
#endif
               next_found=0;
            }
         }
      }
#ifdef ALARMS_DEBUG
      printf("t1=       %s\n", print_date(t1));
      printf("t2=       %s\n", print_date(t2));
      printf("t_prev=   %s\n", prev_found ? print_date(t_prev):"None");
      printf("t_future= %s\n", next_found ? print_date(t_future):"None");
      printf("alarm me= %s\n", next_found ? print_date(t_future-adv):"None");
      printf("desc=[%s]\n", temp_al->mcale.cale.description);
#endif

      if (!soonest_only) {
         if (prev_found) {
            alarms_add_to_list(temp_al->mcale.unique_id, ALARM_MISSED, t_prev, adv);
         }
      }
      if (next_found) {
         add_a_next=0;
         if (next_alarm==NULL) {
            add_a_next=1;
         } else if
           (t_future - adv <= (next_alarm->event_time - next_alarm->alarm_advance)) {
              add_a_next=1;
              if (t_future - adv < (next_alarm->event_time - next_alarm->alarm_advance)) {
#ifdef ALARMS_DEBUG
                 printf("next alarm=%s\n", print_date(next_alarm->event_time - next_alarm->alarm_advance));
                 printf("freeing next alarms\n");
#endif
                 free_alarms_list(NEXT_ALARM_MASK);
              }
           }

         if (add_a_next) {
#ifdef ALARMS_DEBUG
            printf("found a new next\n");
#endif
            ta = malloc(sizeof(struct jp_alarms));
            if (ta) {
               ta->next = next_alarm;
               next_alarm = ta;
               next_alarm->unique_id = temp_al->mcale.unique_id;
               next_alarm->type = ALARM_NEW;
               next_alarm->event_time = t_future;
               next_alarm->alarm_advance = adv;
            }
         }
      }
   }
   free_CalendarEventList(&alm_list);

   return EXIT_SUCCESS;
}

/*
 * At startup check when rc file was written and find all past-due alarms.
 * Add them to the postponed alarm list with 0 minute reminder.
 * Find next alarm and put it in list
 */
int alarms_init(unsigned char skip_past_alarms,
                unsigned char skip_all_alarms)
{
   FILE *in;
   time_t ltime;
   struct tm now, *Pnow;
   struct tm tm1;
   char line[256];
   int found_uptodate;
   int year, mon, day, hour, min, n;

   jp_logf(JP_LOG_DEBUG, "alarms_init()\n");

   alarm_list=NULL;
   Plast_alarm_list=NULL;
   next_alarm=NULL;

   total_alarm_windows = 0;
   glob_skip_all_alarms = skip_all_alarms;

   if (skip_past_alarms) {
      alarms_write_file();
   }
   if (skip_all_alarms) {
      alarms_write_file();
      return EXIT_SUCCESS;
   }

   found_uptodate=0;
   in=jp_open_home_file(EPN".alarms", "r");
   if (!in) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s%s\n"), EPN, ".alarms");
      return EXIT_FAILURE;
   }

   while (!feof(in)) {
      line[0]='\0';
      if (fgets(line, sizeof(line)-1, in) == NULL) {
         // jp_logf(JP_LOG_WARN, "fgets failed %s %d\n", __FILE__, __LINE__);
         break;
      }
      line[sizeof(line)-1] = '\0';
      if (line[0]=='#') continue;
      if (!strncmp(line, "UPTODATE ", 9)) {
         n = sscanf(line+9, "%d %d %d %d %d\n", &year, &mon, &day, &hour, &min);
         if (n==5) {
            found_uptodate=1;
         }
         /* Avoid corner case of retriggering alarms that are set for 
          * exactly the same time as the UPTODATE timestamp */
         min = min + 1;
         jp_logf(JP_LOG_DEBUG, "UPTODATE %d %d %d %d %d\n", year, mon, day, hour, min);
      }
   }

   time(&ltime);
   Pnow = localtime(&ltime);
   memset(&now, 0, sizeof(now));
   now.tm_year=Pnow->tm_year;
   now.tm_mon=Pnow->tm_mon;
   now.tm_mday=Pnow->tm_mday;
   now.tm_hour=Pnow->tm_hour;
   now.tm_min=Pnow->tm_min;
   now.tm_isdst=-1;
   mktime(&now);

   /* Corner case where current time == UPTODATE time.
    * Must adjust current time forward by 1 minute so that upper 
    * bound of search range (current time) is not smaller than
    * lower bound (UPTODATE time) */
   if (found_uptodate &&
      (now.tm_min  == min-1) &&
      (now.tm_hour == hour)  &&
      (now.tm_mon  == mon-1) &&
      (now.tm_year == year-1900)) {
      now.tm_min += 1;
      mktime(&now);
   }

   /* No UPTODATE, use current time for search lower bound */
   if (!found_uptodate) {
      alarms_write_file();
      year = now.tm_year+1900;
      mon = now.tm_mon+1;
      day = now.tm_mday;
      hour = now.tm_hour;
      min = now.tm_min;
   }

   memset(&tm1, 0, sizeof(tm1));
   tm1.tm_year=year-1900;
   tm1.tm_mon=mon-1;
   tm1.tm_mday=day;
   tm1.tm_hour=hour;
   tm1.tm_min=min;
   tm1.tm_isdst=-1;

   mktime(&tm1);

   alarms_find_next(&tm1, &now, FALSE);

   /* Pop up reminder windows for expired alarms immediately
    * rather than waiting ALARM_INTERVAL seconds and then doing it */
   cb_timer_alarms(NULL);

   gtk_timeout_add(ALARM_INTERVAL*CLOCK_TICK, cb_timer_alarms, NULL);

   return EXIT_SUCCESS;
}
