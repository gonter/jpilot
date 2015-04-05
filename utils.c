/*******************************************************************************
 * utils.c
 * A module of J-Pilot http://jpilot.org
 *
 * Copyright (C) 1999-2014 by Judd Montgomery
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

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <utime.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef USE_FLOCK
#  include <sys/file.h>
#else
#  include <fcntl.h>
#endif

#include <pi-source.h>

#include "utils.h"
#include "i18n.h"
#include "log.h"
#include "prefs.h"
#include "sync.h"
#include "plugins.h"
#include "otherconv.h"

/********************************* Constants **********************************/
/* For versioning of files */
#define FILE_VERSION     "version"
#define FILE_VERSION2    "version2"
#define FILE_VERSION2_CR "version2\n"

#define NUM_CAT_ITEMS 16
#define DAY_IN_SECS 86400
/* RFC2445 line length is 75. This length does not include value field such as
 * "DESCRIPTION:" which brings line length to nearly 75. */
#define ICAL_LINE_LENGTH 58
/* RFCs require CRLF for newline */
#define CRLF "\x0D\x0A"
#define CR '\x0D'
#define LF '\x0A'

#define min(a,b) (((a) < (b)) ? (a) : (b))

/* Uncomment for verbose debugging of the alarm code */
/* #define ALARMS_DEBUG */

/******************************* Global vars **********************************/
/* Stuff for the dialog window */
extern GtkWidget *glob_dialog;
extern GtkWidget *glob_date_label;
static int dialog_result;

unsigned int glob_find_id;

/* GTK_TIMEOUT timer identifer for "Today:" label */
extern gint glob_date_timer_tag;

/****************************** Prototypes ************************************/
static gboolean cb_destroy(GtkWidget *widget);
static void cb_quit(GtkWidget *widget, gpointer data);
static void cb_today(GtkWidget *widget, gpointer data);
static int write_to_next_id(unsigned int unique_id);
static int write_to_next_id_open(FILE *pc_out, unsigned int unique_id);
static int forward_backward_in_ce_time(const struct CalendarEvent *cale,
                                       struct tm *t,
                                       int forward_or_backward);
static int str_to_iv_str(char *dest, int destsz, char *src, int isical);

/****************************** Main Code *************************************/
/*
 * This is a slow algorithm, but its not used much
 */
int add_days_to_date(struct tm *date, int n)
{
   int ndim;
   int fdom;
   int flag;
   int i;

   get_month_info(date->tm_mon, 1, date->tm_year, &fdom, &ndim);
   for (i=0; i<n; i++) {
      flag = 0;
      if (++(date->tm_mday) > ndim) {
         date->tm_mday=1;
         flag = 1;
         if (++(date->tm_mon) > 11) {
            date->tm_mon=0;
            flag = 1;
            if (++(date->tm_year)>137) {
               date->tm_year = 137;
            }
         }
      }
      if (flag) {
         get_month_info(date->tm_mon, 1, date->tm_year, &fdom, &ndim);
      }
   }
   date->tm_isdst=-1;
   mktime(date);

   return EXIT_SUCCESS;
}

/*
 * This function will increment the date by n number of months and
 * adjust the day to the last day of the month if it exceeds the number
 * of days in the new month
 */
int add_months_to_date(struct tm *date, int n)
{
   int i;
   int days_in_month[]={31,28,31,30,31,30,31,31,30,31,30,31
   };

   for (i=0; i<n; i++) {
      if (++(date->tm_mon) > 11) {
         date->tm_mon=0;
         if (++(date->tm_year)>137) {
            date->tm_year = 137;
         }
      }
   }

   if ((date->tm_year%4 == 0) &&
       !(((date->tm_year+1900)%100==0) && ((date->tm_year+1900)%400!=0))) {
      days_in_month[1]++;
   }

   if (date->tm_mday > days_in_month[date->tm_mon]) {
      date->tm_mday = days_in_month[date->tm_mon];
   }

   date->tm_isdst=-1;
   mktime(date);
   return EXIT_SUCCESS;
}

/*
 * This function will increment the date by n number of years and
 * adjust feb 29th to feb 28th if its not a leap year
 */
static int add_or_sub_years_to_date(struct tm *date, int n)
{
   date->tm_year += n;

   if (date->tm_year>137) {
      date->tm_year = 137;
   }
   if (date->tm_year<3) {
      date->tm_year = 3;
   }
   /* Leap day/year */
   if ((date->tm_mon==1) && (date->tm_mday==29)) {
      if (!((date->tm_year%4 == 0) &&
            !(((date->tm_year+1900)%100==0) && ((date->tm_year+1900)%400!=0)))) {
         /* Move it back one day */
         date->tm_mday=28;
      }
   }
   return EXIT_SUCCESS;
}

int add_years_to_date(struct tm *date, int n)
{
   return add_or_sub_years_to_date(date, n);
}

/* This function is passed a bounded event description before it appears
 * on the gui (read-only) views. It checks if the event is a yearly repeat
 * (i.e. an anniversary) and then if the last 4 characters look like a
 * year. If so then it appends a "number of years" to the description.
 * This is handy for viewing ages on birthdays etc.  */
/* Either a or cale can be passed as NULL */
void append_anni_years(char *desc, int max, struct tm *date,
                       struct Appointment *appt, struct CalendarEvent *cale)
{
   int len;
   int year;
   /* Only append the years if this is a yearly repeating type (i.e. an
    * anniversary) */
   if ((!appt) && (!cale)) {
      return;
   }
   if ((appt) && (appt->repeatType != repeatYearly))
      return;
   if ((cale) && (cale->repeatType != calendarRepeatYearly))
      return;

   /* Only display this if the user option is enabled */
   if (!get_pref_int_default(PREF_DATEBOOK_ANNI_YEARS, FALSE))
      return;

   len = strlen(desc);

   /* Make sure we have room to insert what we want */
   if (len < 4 || len > (max - 7))
      return;

   /* Get and check for a year */
   year = strtoul(&desc[len - 4], NULL, 10);

   /* Only allow up to 3 digits to be added */
   if (year < 1100 || year > 3000)
      return;

   /* Append the number of years */
   sprintf(&desc[len], " (%d)", 1900 + date->tm_year - year);
}

static const char b64_dict[65] = {
   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
   "abcdefghijklmnopqrstuvwxyz"
   "0123456789+/=" };

static void base64_out(FILE *f, unsigned char *str)
{
   unsigned char *index, char1, char2, char3;
   int loop, pad;

   loop = strlen((char *)str)/3;     // process groups of 3 chars at a time
   pad  = strlen((char *)str) % 3;   // must pad if str not multiple of 3

   /* Convert 3 bytes at a time.  Padding at end calculated separately */
   for (index = str; loop>0; loop--, index+=3) {
      char1 = *index; char2 = *(index+1); char3 = *(index+2);
      fputc(b64_dict[char1>>2], f);
      fputc(b64_dict[(char1<<4 & 0x30) | (char2>>4)], f);
      fputc(b64_dict[(char2<<2 & 0x3c) | (char3>>6)], f);
      fputc(b64_dict[char3 & 0x3f], f);
   }

    /* Now deal with the trailing bytes */
   if (pad)
   {
      char1 = *index; char2 = *(index+1); char3 = *(index+2);
      fputc(b64_dict[char1>>2], f);
      fputc(b64_dict[(char1<<4 & 0x30) | (pad==2 ? char2>>4 : 0)], f );
      fputc(pad==1 ? '=' : b64_dict[(char2<<2 & 0x3c)], f );
      fputc('=', f);
   }

}

static unsigned int bytes_to_bin(unsigned char *bytes, unsigned int num_bytes)
{
   unsigned int i, n;
   n=0;
   for (i=0;i<num_bytes;i++) {
      n = n*256+bytes[i];
   }
   return n;
}

/* mon 0-11
 * day 1-31
 * year (year - 1900)
 * This function will bring up the cal at mon, day, year
 * After a new date is selected it will return mon, day, year
 */
int cal_dialog(GtkWindow *main_window,
               const char *title, int monday_is_fdow,
               int *mon, int *day, int *year)
{
   GtkWidget *button;
   GtkWidget *vbox;
   GtkWidget *hbox;
   GtkWidget *cal;
   GtkWidget *window;
   int return_code;

   window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
   gtk_window_set_title(GTK_WINDOW(window), title);
   gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);
   gtk_window_set_modal(GTK_WINDOW(window), TRUE);
   gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(main_window));

   gtk_signal_connect(GTK_OBJECT(window), "destroy",
                      GTK_SIGNAL_FUNC(cb_destroy), window);

   vbox = gtk_vbox_new(FALSE, 0);
   gtk_container_add(GTK_CONTAINER(window), vbox);

   cal = gtk_calendar_new();
   gtk_box_pack_start(GTK_BOX(vbox), cal, TRUE, TRUE, 0);

   hbox = gtk_hbutton_box_new();
   gtk_container_set_border_width(GTK_CONTAINER(hbox), 12);
   gtk_button_box_set_layout(GTK_BUTTON_BOX (hbox), GTK_BUTTONBOX_END);
   gtk_button_box_set_spacing(GTK_BUTTON_BOX(hbox), 6);
   gtk_container_add(GTK_CONTAINER(vbox), hbox);

   gtk_calendar_set_display_options(GTK_CALENDAR(cal),
                                GTK_CALENDAR_SHOW_HEADING |
                                GTK_CALENDAR_SHOW_DAY_NAMES |
                                GTK_CALENDAR_SHOW_WEEK_NUMBERS |
                                (monday_is_fdow ? GTK_CALENDAR_WEEK_START_MONDAY : 0));

   /* gtk_signal_connect(GTK_OBJECT(cal), "day_selected", cb_cal_sel, NULL); */
   gtk_signal_connect(GTK_OBJECT(cal), "day_selected_double_click", GTK_SIGNAL_FUNC(cb_quit),
                      GINT_TO_POINTER(CAL_DONE));

   gtk_calendar_select_month(GTK_CALENDAR(cal), *mon, (*year)+1900);
   gtk_calendar_select_day(GTK_CALENDAR(cal), *day);

   /* Cancel/Today/OK buttons */
   button = gtk_button_new_from_stock(GTK_STOCK_CANCEL);
   gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
   gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(cb_quit),
                      GINT_TO_POINTER(CAL_CANCEL));

   button = gtk_button_new_with_label(_("Today"));
   gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
   gtk_signal_connect(GTK_OBJECT(button), "clicked",
                      GTK_SIGNAL_FUNC(cb_today), cal);

   button = gtk_button_new_from_stock(GTK_STOCK_OK);
   gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
   gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(cb_quit),
                      GINT_TO_POINTER(CAL_DONE));

   gtk_object_set_data(GTK_OBJECT(window), "mon", mon);
   gtk_object_set_data(GTK_OBJECT(window), "day", day);
   gtk_object_set_data(GTK_OBJECT(window), "year", year);
   gtk_object_set_data(GTK_OBJECT(window), "return_code", &return_code);
   gtk_object_set_data(GTK_OBJECT(window), "cal", cal);
   
   gtk_widget_show_all(window);

   gtk_main();

   if (return_code == CAL_DONE) {
      *year -= 1900;
   }

   return return_code;
}

int cat_compare(const void *v1, const void *v2)
{
   struct sorted_cats *s1, *s2;
   s1=(struct sorted_cats *)v1; s2=(struct sorted_cats *)v2;
   if ((s1)->Pcat[0]=='\0') {
      return 1;
   }
   if ((s2)->Pcat[0]=='\0') {
      return -1;
   }
   return strcmp((s1)->Pcat, (s2)->Pcat);
}

static gboolean cb_destroy(GtkWidget *widget)
{
   gtk_main_quit();
   return FALSE;
}

static gboolean cb_destroy_dialog(GtkWidget *widget)
{
   glob_dialog = NULL;
   gtk_main_quit();

   return FALSE;
}

static void cb_dialog_button(GtkWidget *widget,
                             gpointer   data)
{
   dialog_result=GPOINTER_TO_INT(data);

   gtk_widget_destroy(glob_dialog);
}

static void cb_quit(GtkWidget *widget, gpointer data)
{
   unsigned int y,m,d;
   unsigned int *Py,*Pm,*Pd;
   int *Preturn_code;
   GtkWidget *cal=NULL;
   GtkWidget *window;

   window = gtk_widget_get_toplevel(widget);

   Preturn_code = gtk_object_get_data(GTK_OBJECT(window), "return_code");
   if (Preturn_code) *Preturn_code = GPOINTER_TO_INT(data);
   cal = gtk_object_get_data(GTK_OBJECT(window), "cal");

   if (Preturn_code && *Preturn_code==CAL_DONE) {
      if (cal) {
         gtk_calendar_get_date(GTK_CALENDAR(cal),&y,&m,&d);
         Pm = gtk_object_get_data(GTK_OBJECT(window), "mon");
         Pd = gtk_object_get_data(GTK_OBJECT(window), "day");
         Py = gtk_object_get_data(GTK_OBJECT(window), "year");
         if (Pm) *Pm=m;
         if (Pd) *Pd=d;
         if (Py) *Py=y;
      }
   }

   gtk_widget_destroy(window);
}

static void cb_today(GtkWidget *widget, gpointer data)
{
   time_t ltime;
   struct tm *now;
   GtkWidget *cal;

   cal = data;
   time(&ltime);
   now = localtime(&ltime);

   gtk_calendar_select_month(GTK_CALENDAR(cal), now->tm_mon, now->tm_year+1900);
   gtk_calendar_select_day(GTK_CALENDAR(cal), now->tm_mday);
}

/*
 *         JPA overwrite a host character set string by its
 *             conversion to a Palm Pilot character string
 */
void charset_j2p(char *buf, int max_len, long char_set)
{
   switch (char_set) {
    case CHAR_SET_JAPANESE: Euc2Sjis(buf, max_len); break;
    case CHAR_SET_LATIN1  : /* No conversion required */ break;
    case CHAR_SET_1250    : Lat2Win(buf,max_len); break;
    case CHAR_SET_1251    : koi8_to_win1251(buf, max_len); break;
    case CHAR_SET_1251_B  : win1251_to_koi8(buf, max_len); break;
    default:
      UTF_to_other(buf, max_len);
      break;
   }
}

/*
 *         JPA overwrite a Palm Pilot character string by its
 *             conversion to host character set
 */
void charset_p2j(char *const buf, int max_len, int char_set)
{
   char *newbuf;
   gchar *end;

   newbuf = charset_p2newj(buf, max_len, char_set);

   g_strlcpy(buf, newbuf, max_len);

   if (strlen(newbuf) >= max_len) {
      jp_logf(JP_LOG_WARN, "charset_p2j: buffer too small - original string before truncation [%s]\n", newbuf);
      if (char_set > CHAR_SET_UTF) {
         /* truncate the string on a UTF-8 character boundary */
         if (!g_utf8_validate(buf, -1, (const gchar **)&end))
            *end = 0;
      }
   }

   free(newbuf);
}

/*
 *         JPA convert a Palm Pilot character string to host
 *             equivalent without overwriting
 */
char *charset_p2newj(const char *buf, int max_len, int char_set)
{
   char *newbuf = NULL;

   /* Allocate a longer buffer if not done in conversion routine.
    * Only old conversion routines don't assign a buffer */
   switch (char_set) {
    case CHAR_SET_JAPANESE:
      if (max_len == -1) {
         max_len = 2*strlen(buf) + 1;
         newbuf = g_malloc(max_len);
      } else {
         newbuf = g_malloc(min(2*strlen(buf) + 1, max_len));
      }
      if (newbuf) {
         /* be safe, though string should fit into buf */
         g_strlcpy(newbuf, buf, max_len);
      }
      break;
    case CHAR_SET_LATIN1:
    case CHAR_SET_1250:
    case CHAR_SET_1251:
    case CHAR_SET_1251_B:
      if (max_len == -1) {
         max_len = strlen(buf) + 1;
         newbuf = g_malloc(max_len);
      } else {
         newbuf = g_malloc(min(strlen(buf) + 1, max_len));
      }
      if (newbuf) {
         /* be safe, though string should fit into buf */
         g_strlcpy(newbuf, buf, max_len);
      }
      break;
    default:
      /* All other encodings get a new buffer from other_to_UTF */
      break;
   }

   /* Now convert character encoding */
   switch (char_set) {
    case CHAR_SET_JAPANESE : Sjis2Euc(newbuf, max_len); break;
    case CHAR_SET_LATIN1   : /* No conversion required */ break;
    case CHAR_SET_1250     : Win2Lat(newbuf, max_len); break;
    case CHAR_SET_1251     : win1251_to_koi8(newbuf, max_len); break;
    case CHAR_SET_1251_B   : koi8_to_win1251(newbuf, max_len); break;
    default:
      newbuf = other_to_UTF(buf, max_len);
      break;
   }

   return (newbuf);
}

/* This function will copy an empty DB file 
 * from the share directory to the users JPILOT_HOME directory
 * if it doesn't exist already and its length is > 0 */
int check_copy_DBs_to_home(void)
{
   FILE *in, *out;
   struct stat sbuf;
   int i, c, r;
   char destname[FILENAME_MAX];
   char srcname[FILENAME_MAX];
   struct utimbuf times;
   char dbname_pdb[][32]={
      "DatebookDB.pdb",
      "CalendarDB-PDat.pdb",
      "AddressDB.pdb",
      "ContactsDB-PAdd.pdb",
      "ToDoDB.pdb",
      "TasksDB-PTod.pdb",
      "MananaDB.pdb",
      "MemoDB.pdb",
      "MemosDB-PMem.pdb",
      "Memo32DB.pdb",
      "ExpenseDB.pdb",
      ""
   };

   for (i=0; dbname_pdb[i][0]; i++) {
      get_home_file_name(dbname_pdb[i], destname, sizeof(destname));
      r = stat(destname, &sbuf);
      if (((r)&&(errno==ENOENT)) || (sbuf.st_size==0)) {
         /* The file doesn't exist or is zero in size, copy an empty DB file */
         if ((strlen(BASE_DIR) + strlen(EPN) + strlen(dbname_pdb[i])) > sizeof(srcname)) {
            jp_logf(JP_LOG_DEBUG, "copy_DB_to_home filename too long\n");
            return EXIT_FAILURE;
         }
         g_snprintf(srcname, sizeof(srcname), "%s/%s/%s/%s", BASE_DIR, "share", EPN, dbname_pdb[i]);
         in = fopen(srcname, "r");
         out = fopen(destname, "w");
         if (!in) {
            jp_logf(JP_LOG_WARN, _("Couldn't find empty DB file %s: %s\n"),
                    srcname, strerror(errno));
            jp_logf(JP_LOG_WARN, EPN);
            jp_logf(JP_LOG_WARN, _(" may not be installed.\n"));
            return EXIT_FAILURE;
         }
         if (!out) {
            fclose(in);
            return EXIT_FAILURE;
         }
         while ( (c=fgetc(in)) != EOF ) {
            fputc(c, out);
         }
         fclose(in);
         fclose(out);
         /* Set the dates on the file to be old (not up to date) */
         times.actime = 1;
         times.modtime = 1;
         utime(destname, &times);
      }
   }
   return EXIT_SUCCESS;
}

int check_hidden_dir(void)
{
   struct stat statb;
   char hidden_dir[FILENAME_MAX];

   get_home_file_name("", hidden_dir, sizeof(hidden_dir));
   hidden_dir[strlen(hidden_dir)-1]='\0';

   if (stat(hidden_dir, &statb)) {
      /* Directory isn't there, create it. 
       * Only user is given permission to enter and change directory contents
       * which provides some primitive privacy protection. */
      if (mkdir(hidden_dir, 0700)) {
         /* Can't create directory */
         jp_logf(JP_LOG_WARN, _("Can't create directory %s\n"), hidden_dir);
         return EXIT_FAILURE;
      }
      if (stat(hidden_dir, &statb)) {
         jp_logf(JP_LOG_WARN, _("Can't create directory %s\n"), hidden_dir);
         return EXIT_FAILURE;
      }
   }
   /* Is it a directory? */
   if (!S_ISDIR(statb.st_mode)) {
      jp_logf(JP_LOG_WARN, _("%s is not a directory\n"), hidden_dir);
      return EXIT_FAILURE;
   }
   /* Can we write in it? */
   if (access(hidden_dir, W_OK) != 0) {
      jp_logf(JP_LOG_WARN, _("Unable to get write permission for directory %s\n"), hidden_dir);
      return EXIT_FAILURE;
   } 

   return EXIT_SUCCESS;
}

/* This function removes extra slashes from a string */
void cleanup_path(char *path)
{
   register int s, d; /* source and destination */

   if (!path) return;
   for (s=d=0; path[s]!='\0'; s++,d++) {
      if ((path[s]=='/') && (path[s+1]=='/')) {
         d--;
         continue;
      }
      if (d!=s) {
         path[d]=path[s];
      }
   }
   path[d]='\0';
}

/* Compacts pc3 file by removing records which have been synced */
static int cleanup_pc_file(char *DB_name, unsigned int *max_id)
{
   PC3RecordHeader header;
   char pc_filename[FILENAME_MAX];
   char pc_filename2[FILENAME_MAX];
   FILE *pc_file;
   FILE *pc_file2;
   char *record;
   int r;
   int ret;
   int num;
   int compact_it;
   int next_id;

   r=0;
   *max_id = 0;
   next_id = 1;
   record = NULL;
   pc_file = pc_file2 = NULL;

   g_snprintf(pc_filename, sizeof(pc_filename), "%s.pc3", DB_name);
   g_snprintf(pc_filename2, sizeof(pc_filename2), "%s.pct", DB_name);

   pc_file = jp_open_home_file(pc_filename , "r");
   if (!pc_file) {
      return EXIT_FAILURE;
   }

   compact_it = 0;
   /* Scan through the file and see if it needs to be compacted */
   while(!feof(pc_file)) {
      read_header(pc_file, &header);
      if (feof(pc_file)) {
         break;
      }
      if (header.rt & SPENT_PC_RECORD_BIT) {
         compact_it=1;
         break;
      }
      if ((header.unique_id > *max_id)
          && (header.rt != PALM_REC)
          && (header.rt != MODIFIED_PALM_REC)
          && (header.rt != DELETED_PALM_REC)
          && (header.rt != REPLACEMENT_PALM_REC) ){
         *max_id = header.unique_id;
      }
      if (fseek(pc_file, header.rec_len, SEEK_CUR)) {
         jp_logf(JP_LOG_WARN, "fseek failed\n");
      }
   }

   if (!compact_it) {
      jp_logf(JP_LOG_DEBUG, "No compacting needed\n");
      jp_close_home_file(pc_file);
      return EXIT_SUCCESS;
   }

   fseek(pc_file, 0, SEEK_SET);

   pc_file2=jp_open_home_file(pc_filename2, "w");
   if (!pc_file2) {
      jp_close_home_file(pc_file);
      return EXIT_FAILURE;
   }

   while(!feof(pc_file)) {
      read_header(pc_file, &header);
      if (feof(pc_file)) {
         break;
      }
      if (header.rt & SPENT_PC_RECORD_BIT) {
         r++;
         if (fseek(pc_file, header.rec_len, SEEK_CUR)) {
            jp_logf(JP_LOG_WARN, "fseek failed\n");
            r = -1;
            break;
         }
         continue;
      } else {
         if (header.rt == NEW_PC_REC) {
            header.unique_id = next_id++;
         }
         if ((header.unique_id > *max_id)
             && (header.rt != PALM_REC)
             && (header.rt != MODIFIED_PALM_REC)
             && (header.rt != DELETED_PALM_REC)
             && (header.rt != REPLACEMENT_PALM_REC)
             ){
            *max_id = header.unique_id;
         }
         record = malloc(header.rec_len);
         if (!record) {
            jp_logf(JP_LOG_WARN, "cleanup_pc_file(): %s\n", _("Out of memory"));
            r = -1;
            break;
         }
         num = fread(record, header.rec_len, 1, pc_file);
         if (num != 1) {
            if (ferror(pc_file)) {
               r = -1;
               break;
            }
         }
         ret = write_header(pc_file2, &header);
         /* if (ret != 1) {
            r = -1;
            break;
         }*/
         ret = fwrite(record, header.rec_len, 1, pc_file2);
         if (ret != 1) {
            r = -1;
            break;
         }
         free(record);
         record = NULL;
      }
   }

   if (record) {
      free(record);
   }
   if (pc_file) {
      jp_close_home_file(pc_file);
   }
   if (pc_file2) {
      jp_close_home_file(pc_file2);
   }

   if (r>=0) {
      rename_file(pc_filename2, pc_filename);
   } else {
      unlink_file(pc_filename2);
   }

   return r;
}

/* Compact all pc3 files including plugins */
int cleanup_pc_files(void)
{
   int ret;
   int fail_flag;
   unsigned int max_id, max_max_id;
#ifdef ENABLE_PLUGINS
   GList *plugin_list, *temp_list;
   struct plugin_s *plugin;
#endif
   int i;
   char dbname[][32]={
      "DatebookDB",
      "AddressDB",
      "ToDoDB",
      "MemoDB",
      ""
   };

   /* Convert to new database names depending on prefs */
   rename_dbnames(dbname);

   fail_flag = 0;
   max_id = max_max_id = 0;

   for (i=0; dbname[i][0]; i++) {
      jp_logf(JP_LOG_DEBUG, "cleanup_pc_file for %s\n", dbname[i]);
      ret = cleanup_pc_file(dbname[i], &max_id);
      jp_logf(JP_LOG_DEBUG, "max_id was %d\n", max_id);
      if (ret<0) {
         fail_flag=1;
      } else if (max_id > max_max_id) {
         max_max_id = max_id;
      }
   }

#ifdef ENABLE_PLUGINS
   plugin_list = get_plugin_list();

   for (temp_list = plugin_list; temp_list; temp_list = temp_list->next) {
      plugin = (struct plugin_s *)temp_list->data;
      if ((plugin->db_name==NULL) || (plugin->db_name[0]=='\0')) {
         jp_logf(JP_LOG_DEBUG, "not calling cleanup_pc_file for: [%s]\n", plugin->db_name);
         continue;
      }
      jp_logf(JP_LOG_DEBUG, "cleanup_pc_file for [%s]\n", plugin->db_name);
      ret = cleanup_pc_file(plugin->db_name, &max_id);
      jp_logf(JP_LOG_DEBUG, "max_id was %d\n", max_id);
      if (ret<0) {
         fail_flag=1;
      } else if (max_id > max_max_id) {
         max_max_id = max_id;
      }
   }
#endif
   if (!fail_flag) {
      write_to_next_id(max_max_id);
   }

   return EXIT_SUCCESS;
}

/* returns 0 if not found, 1 if found */
int clist_find_id(GtkWidget *clist,
                  unsigned int unique_id,
                  int *found_at)
{
   int i, found;
   MyAddress *maddr;

   *found_at = 0;

   for (found = i = 0; i<GTK_CLIST(clist)->rows; i++) {
      maddr = gtk_clist_get_row_data(GTK_CLIST(clist), i);
      if (maddr < (MyAddress *)CLIST_MIN_DATA) {
         break;
      }
      if (maddr->unique_id==unique_id) {
         found = TRUE;
         *found_at = i;
         break;
      }
   }

   return found;
}

/* Encapsulate GTK function to make it free all resources */
void clist_clear(GtkCList *clist)
{
   GtkStyle *base_style, *row_style; 
   int i;

   base_style = gtk_widget_get_style(GTK_WIDGET(clist));
  
   for (i=0; i<GTK_CLIST(clist)->rows ; i++)
   {
      row_style = gtk_clist_get_row_style(GTK_CLIST(clist), i);
      if (row_style && (row_style != base_style))
      {
         g_object_unref(row_style);  
      }
   }

   gtk_clist_clear(GTK_CLIST(clist));
}

/* Encapsulate GTK tooltip function which no longer supports disabling as
 * of GTK 2.12 */
void set_tooltip(int show_tooltip, 
                        GtkTooltips *tooltips,
                        GtkWidget *widget,
                        const gchar *tip_text,
                        const gchar *tip_private)
{
   if (show_tooltip)
      gtk_tooltips_set_tip(tooltips, widget, tip_text, tip_private);
}


/* Encapsulate broken GTK function to make it work as documented */
void clist_select_row(GtkCList *clist, 
                             int       row,
                             int       column)
{
   clist->focus_row = row;
   gtk_clist_select_row(clist, row, column);
}

int dateToDays(struct tm *tm1)
{
   time_t t1;
   struct tm *gmt;
   struct tm tm2;
   static time_t adj = -1;

   memcpy(&tm2, tm1, sizeof(struct tm));
   tm2.tm_isdst = 0;
   tm2.tm_hour=12;
   t1 = mktime(&tm2);
   if (-1 == adj) {
      gmt = gmtime(&t1);
      adj = t1 - mktime(gmt);
   }
   return (t1+adj)/86400; /* There are 86400 secs in a day */
}

/*
 * This deletes a record from the appropriate Datafile
 */
int delete_pc_record(AppType app_type, void *VP, int flag)
{
   FILE *pc_in;
   PC3RecordHeader header;
   struct Appointment *appt;
   MyAppointment *mappt;
   struct CalendarEvent *cale;
   MyCalendarEvent *mcale;
   struct Address *addr;
   MyAddress *maddr;
   struct Contact *cont;
   MyContact *mcont;
   struct ToDo *todo;
   MyToDo *mtodo;
   struct Memo *memo;
   MyMemo *mmemo;
   char filename[FILENAME_MAX];
   pi_buffer_t *RecordBuffer = NULL;
   PCRecType record_type;
   unsigned int unique_id;
   unsigned char attrib;
#ifdef ENABLE_MANANA
   long ivalue;
#endif
   long memo_version;
   long char_set;

   jp_logf(JP_LOG_DEBUG, "delete_pc_record(%d, %d)\n", app_type, flag);

   if (VP==NULL) {
      return EXIT_FAILURE;
   }

   get_pref(PREF_CHAR_SET, &char_set, NULL);

   /* to keep the compiler happy with -Wall*/
   mappt=NULL;
   mcale=NULL;
   maddr=NULL;
   mcont=NULL;
   mtodo=NULL;
   mmemo=NULL;
   switch (app_type) {
    case DATEBOOK:
      mappt = (MyAppointment *) VP;
      record_type = mappt->rt;
      unique_id = mappt->unique_id;
      attrib = mappt->attrib;
      strcpy(filename, "DatebookDB.pc3");
      break;
    case CALENDAR:
      mcale = (MyCalendarEvent *) VP;
      record_type = mcale->rt;
      unique_id = mcale->unique_id;
      attrib = mcale->attrib;
      strcpy(filename, "CalendarDB-PDat.pc3");
      break;
    case ADDRESS:
      maddr = (MyAddress *) VP;
      record_type = maddr->rt;
      unique_id = maddr->unique_id;
      attrib = maddr->attrib;
      strcpy(filename, "AddressDB.pc3");
      break;
    case CONTACTS:
      mcont = (MyContact *) VP;
      record_type = mcont->rt;
      unique_id = mcont->unique_id;
      attrib = mcont->attrib;
      strcpy(filename, "ContactsDB-PAdd.pc3");
      break;
    case TODO:
      mtodo = (MyToDo *) VP;
      record_type = mtodo->rt;
      unique_id = mtodo->unique_id;
      attrib = mtodo->attrib;
#ifdef ENABLE_MANANA
      get_pref(PREF_MANANA_MODE, &ivalue, NULL);
      if (ivalue) {
         strcpy(filename, "MananaDB.pc3");
      } else {
         strcpy(filename, "ToDoDB.pc3");
      }
#else
      strcpy(filename, "ToDoDB.pc3");
#endif
      break;
    case MEMO:
      mmemo = (MyMemo *) VP;
      record_type = mmemo->rt;
      unique_id = mmemo->unique_id;
      attrib = mmemo->attrib;
      get_pref(PREF_MEMO_VERSION, &memo_version, NULL);
      switch (memo_version) {
       case 0:
       default:
         strcpy(filename, "MemoDB.pc3");
         break;
       case 1:
         strcpy(filename, "MemosDB-PMem.pc3");
         break;
       case 2:
         strcpy(filename, "Memo32DB.pc3");
         break;
      }
      break;
    default:
      return EXIT_SUCCESS;
   }

   if ((record_type==DELETED_PALM_REC) || (record_type==MODIFIED_PALM_REC)) {
      jp_logf(JP_LOG_INFO|JP_LOG_GUI, _("This record is already deleted.\n"
           "It is scheduled to be deleted from the Palm on the next sync.\n"));
      return EXIT_SUCCESS;
   }
   RecordBuffer = pi_buffer_new(0);
   switch (record_type) {
    case NEW_PC_REC:
    case REPLACEMENT_PALM_REC:
      pc_in=jp_open_home_file(filename, "r+");
      if (pc_in==NULL) {
         jp_logf(JP_LOG_WARN, _("Unable to open PC records file\n"));
         pi_buffer_free(RecordBuffer);
         return EXIT_FAILURE;
      }
      while(!feof(pc_in)) {
         read_header(pc_in, &header);
         if (feof(pc_in)) {
            jp_logf(JP_LOG_WARN, _("Couldn't find record to delete\n"));
            pi_buffer_free(RecordBuffer);
            jp_close_home_file(pc_in);
            return EXIT_FAILURE;
         }
         /* Keep unique ID intact */
         if (header.header_version==2) {
            if ((header.unique_id==unique_id) &&
                ((header.rt==NEW_PC_REC)||(header.rt==REPLACEMENT_PALM_REC))) {
               if (fseek(pc_in, -header.header_len, SEEK_CUR)) {
                  jp_logf(JP_LOG_WARN, "fseek failed\n");
               }
               header.rt=DELETED_PC_REC;
               write_header(pc_in, &header);
               jp_logf(JP_LOG_DEBUG, "record deleted\n");
               jp_close_home_file(pc_in);
               pi_buffer_free(RecordBuffer);
               return EXIT_SUCCESS;
            }
         } else {
            jp_logf(JP_LOG_WARN, _("Unknown header version %d\n"), header.header_version);
         }
         if (fseek(pc_in, header.rec_len, SEEK_CUR)) {
            jp_logf(JP_LOG_WARN, "fseek failed\n");
         }
      }

      jp_close_home_file(pc_in);
      pi_buffer_free(RecordBuffer);
      return EXIT_FAILURE;

    case PALM_REC:
      jp_logf(JP_LOG_DEBUG, "Deleting Palm ID %d\n", unique_id);
      pc_in=jp_open_home_file(filename, "a");
      if (pc_in==NULL) {
         jp_logf(JP_LOG_WARN, _("Unable to open PC records file\n"));
         pi_buffer_free(RecordBuffer);
         return EXIT_FAILURE;
      }

      header.unique_id=unique_id;
      if (flag==MODIFY_FLAG) {
         header.rt=MODIFIED_PALM_REC;
      } else {
         header.rt=DELETED_PALM_REC;
      }
      header.attrib=attrib;

      switch (app_type) {
       case DATEBOOK:
         appt=&mappt->appt;
         if (pack_Appointment(appt, RecordBuffer, datebook_v1) == -1) {
            PRINT_FILE_LINE;
            jp_logf(JP_LOG_WARN, "pack_Appointment %s\n", _("error"));
         }
         break;
       case CALENDAR:
         cale=&mcale->cale;
         if (pack_CalendarEvent(cale, RecordBuffer, calendar_v1) == -1) {
            PRINT_FILE_LINE;
            jp_logf(JP_LOG_WARN, "pack_CalendarEvent %s\n", _("error"));
         }
         break;
       case ADDRESS:
         addr=&maddr->addr;
         if (pack_Address(addr, RecordBuffer, address_v1) == -1) {
            PRINT_FILE_LINE;
            jp_logf(JP_LOG_WARN, "pack_Address %s\n", _("error"));
         }
         break;
       case CONTACTS:
         cont=&mcont->cont;
         if (jp_pack_Contact(cont, RecordBuffer) == -1) {
            PRINT_FILE_LINE;
            jp_logf(JP_LOG_WARN, "jp_pack_Contact %s\n", _("error"));
         }
         break;
       case TODO:
         todo=&mtodo->todo;
         if (pack_ToDo(todo, RecordBuffer, todo_v1) == -1) {
            PRINT_FILE_LINE;
            jp_logf(JP_LOG_WARN, "pack_ToDo %s\n", _("error"));
         }
         break;
       case MEMO:
         memo=&mmemo->memo;
         if (pack_Memo(memo, RecordBuffer, memo_v1) == -1) {
            PRINT_FILE_LINE;
            jp_logf(JP_LOG_WARN, "pack_Memo %s\n", _("error"));
         }
         break;
       default:
         jp_close_home_file(pc_in);
         pi_buffer_free(RecordBuffer);
         return EXIT_SUCCESS;
      } /* switch */

      header.rec_len = RecordBuffer->used;

      jp_logf(JP_LOG_DEBUG, "writing header to pc file\n");
      write_header(pc_in, &header);
      /* This record is used during sync to make sure that the palm record
       * hasn't changed before we delete it */
      jp_logf(JP_LOG_DEBUG, "writing record to pc file, %d bytes\n", header.rec_len);
      fwrite(RecordBuffer->data, header.rec_len, 1, pc_in);
      jp_logf(JP_LOG_DEBUG, "record deleted\n");
      jp_close_home_file(pc_in);
      pi_buffer_free(RecordBuffer);
      return EXIT_SUCCESS;
      break;
    default:
      break;
   } /* switch (record_type) */

   if (RecordBuffer)
      pi_buffer_free(RecordBuffer);

   return EXIT_SUCCESS;
}

/* nob = number of buttons */
int dialog_generic(GtkWindow *main_window,
                   char *title, int type,
                   char *text, int nob, char *button_text[])
{
   GtkWidget *button, *label1;
   GtkWidget *hbox1, *vbox1, *vbox2;
   int i;
   GtkWidget *image;
   char *markup;

   /* This gdk function call is required in order to avoid a GTK
    * error which causes X and the mouse pointer to lock up.
    * The lockup is generated whenever a modal dialog is created
    * from the callback routine of a clist. */
   gdk_pointer_ungrab(GDK_CURRENT_TIME);

   dialog_result=0;
   glob_dialog = gtk_widget_new(GTK_TYPE_WINDOW,
                                "type", GTK_WINDOW_TOPLEVEL,
                                "window_position", GTK_WIN_POS_MOUSE,
                                NULL);
   gtk_window_set_title(GTK_WINDOW(glob_dialog), title);
   gtk_window_set_modal(GTK_WINDOW(glob_dialog), TRUE);
   if (main_window) {
      gtk_window_set_transient_for(GTK_WINDOW(glob_dialog), GTK_WINDOW(main_window));
   }

   gtk_signal_connect(GTK_OBJECT(glob_dialog), "destroy",
                      GTK_SIGNAL_FUNC(cb_destroy_dialog), glob_dialog);

   vbox1 = gtk_vbox_new(FALSE, 5);
   gtk_container_add(GTK_CONTAINER(glob_dialog), vbox1);

   hbox1 = gtk_hbox_new(FALSE, 2);
   gtk_container_add(GTK_CONTAINER(vbox1), hbox1);
   gtk_container_set_border_width(GTK_CONTAINER(hbox1), 12);
   switch (type)
   {
      case DIALOG_INFO:
         image = gtk_image_new_from_stock(GTK_STOCK_DIALOG_INFO, GTK_ICON_SIZE_DIALOG);
         break;
      case DIALOG_QUESTION:
         image = gtk_image_new_from_stock(GTK_STOCK_DIALOG_QUESTION, GTK_ICON_SIZE_DIALOG);
         break;
      case DIALOG_ERROR:
         image = gtk_image_new_from_stock(GTK_STOCK_DIALOG_ERROR, GTK_ICON_SIZE_DIALOG);
         break;
      case DIALOG_WARNING:
         image = gtk_image_new_from_stock(GTK_STOCK_DIALOG_WARNING, GTK_ICON_SIZE_DIALOG);
         break;
      default:
         image = NULL;
   }
   if (image)
      gtk_box_pack_start(GTK_BOX(hbox1), image, FALSE, FALSE, 2);

   vbox2 = gtk_vbox_new(FALSE, 5);
   gtk_container_set_border_width(GTK_CONTAINER(vbox2), 5);
   gtk_box_pack_start(GTK_BOX(hbox1), vbox2, FALSE, FALSE, 2);

   /* Title and Information text */
   label1 = gtk_label_new(NULL);
   markup = g_markup_printf_escaped("<b><big>%s</big></b>\n\n%s", title, text);
   gtk_label_set_markup(GTK_LABEL(label1), markup);
   g_free(markup);
   gtk_box_pack_start(GTK_BOX(vbox2), label1, FALSE, FALSE, 2);

   /* Create buttons */
   hbox1 = gtk_hbutton_box_new();
   gtk_container_set_border_width(GTK_CONTAINER(hbox1), 12);
   gtk_button_box_set_layout(GTK_BUTTON_BOX (hbox1), GTK_BUTTONBOX_END);
   gtk_button_box_set_spacing(GTK_BUTTON_BOX(hbox1), 6);

   gtk_box_pack_start(GTK_BOX(vbox1), hbox1, FALSE, FALSE, 2);

   for (i=0; i < nob; i++) {
      if (0 == strcmp("OK", button_text[i]))
         button = gtk_button_new_from_stock(GTK_STOCK_OK);
      else
         if (0 == strcmp("Cancel", button_text[i]))
            button = gtk_button_new_from_stock(GTK_STOCK_CANCEL);
         else
            if (0 == strcmp("Yes", button_text[i]))
               button = gtk_button_new_from_stock(GTK_STOCK_YES);
            else
               if (0 == strcmp("No", button_text[i]))
                  button = gtk_button_new_from_stock(GTK_STOCK_NO);
               else
      button = gtk_button_new_with_label(_(button_text[i]));
      gtk_signal_connect(GTK_OBJECT(button), "clicked",
                         GTK_SIGNAL_FUNC(cb_dialog_button),
                         GINT_TO_POINTER(DIALOG_SAID_1 + i));
      gtk_box_pack_start(GTK_BOX(hbox1), button, TRUE, TRUE, 1);

      /* default button is the last one */
      if (i == nob-1)
      {
         GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
         gtk_widget_grab_default(button);
         gtk_widget_grab_focus(button);
      }
   }

   gtk_widget_show_all(glob_dialog);

   gtk_main();

   return dialog_result;
}

int dialog_generic_ok(GtkWidget *widget,
                      char *title, int type, char *text)
{
   char *button_text[] = {N_("OK")};

   if (widget) {
      return dialog_generic(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(widget))),
                            title, type, text, 1, button_text);
   }
   return dialog_generic(NULL, title, type, text, 1, button_text);
}

/*
 * Widget must be some widget used to get the main window from.
 * The main window passed in would be fastest.
 * changed is MODIFY_FLAG, or NEW_FLAG
 */
int dialog_save_changed_record(GtkWidget *widget, int changed)
{
   int b=0;
   char *button_text[] = {N_("No"), N_("Yes")};

   if ((changed!=MODIFY_FLAG) && (changed!=NEW_FLAG)) {
      return EXIT_SUCCESS;
   }

   if (changed==MODIFY_FLAG) {
      b=dialog_generic(GTK_WINDOW(gtk_widget_get_toplevel(widget)),
                       _("Save Changed Record?"), DIALOG_QUESTION,
                       _("Do you want to save the changes to this record?"),
                       2, button_text);
   }
   if (changed==NEW_FLAG) {
      b=dialog_generic(GTK_WINDOW(gtk_widget_get_toplevel(widget)),
                       _("Save New Record?"), DIALOG_QUESTION,
                       _("Do you want to save this new record?"),
                       2, button_text);
   }

   return b;
}
int dialog_save_changed_record_with_cancel(GtkWidget *widget, int changed)
{
   int b=0;
   char *button_text[] = {N_("Cancel"), N_("No"), N_("Yes")};

   if ((changed!=MODIFY_FLAG) && (changed!=NEW_FLAG)) {
      return EXIT_SUCCESS;
   }

   if (changed==MODIFY_FLAG) {
      b=dialog_generic(GTK_WINDOW(gtk_widget_get_toplevel(widget)),
                       _("Save Changed Record?"), DIALOG_QUESTION,
                       _("Do you want to save the changes to this record?"),
                       3, button_text);
   }
   if (changed==NEW_FLAG) {
      b=dialog_generic(GTK_WINDOW(gtk_widget_get_toplevel(widget)),
                       _("Save New Record?"), DIALOG_QUESTION,
                       _("Do you want to save this new record?"),
                       3, button_text);
   }

   return b;
}

void entry_set_multiline_truncate(GtkEntry *entry, gboolean value)
{
#  if GTK_MINOR_VERSION >= 10
      entry->truncate_multiline = value; 
#  endif
}

/* returns 1 if found */
/*        0 if eof */
int find_next_offset(mem_rec_header *mem_rh, long fpos,
                     unsigned int *next_offset,
                     unsigned char *attrib, unsigned int *unique_id)
{
   mem_rec_header *temp_mem_rh;
   unsigned char found = 0;
   unsigned long found_at;

   found_at=0xFFFFFF;
   for (temp_mem_rh=mem_rh; temp_mem_rh; temp_mem_rh = temp_mem_rh->next) {
      if ((temp_mem_rh->offset > fpos) && (temp_mem_rh->offset < found_at)) {
         found_at = temp_mem_rh->offset;
      }
      if ((temp_mem_rh->offset == fpos)) {
         found = 1;
         *attrib = temp_mem_rh->attrib;
         *unique_id = temp_mem_rh->unique_id;
      }
   }
   *next_offset = found_at;
   return found;
}

/* Finds next repeating event occurrence closest to srch_start_tm */
int find_next_rpt_event(struct CalendarEvent *cale,
                        struct tm *srch_start_tm,
                        struct tm *next_tm)
{
   struct tm prev_tm;
   int prev_found, next_found;

   find_prev_next(cale,
                  0,
                  srch_start_tm,
                  srch_start_tm,
                  &prev_tm,
                  next_tm,
                  &prev_found,
                  &next_found);

   return next_found;
}

/*
 * Search forwards and backwards in time to find alarms which bracket 
 * date1 and date2.
 *   For non-repeating appointments no searching is necessary.  
 *   The appt is either in the range or it is not.
 *   The algorithm for searching is time consuming.  To improve performance 
 *   this subroutine seeks to seed the search with a close guess for the 
 *   correct time before launching the search.
 *
 *   Math is explicitly done with integers so that divisions which might produce
 *   a float as a result will instead produce a truncated result.  
 *   Alternatively the C math functions such as floor could be used but there 
 *   seems little point in invoking such overhead.
 */
int find_prev_next(struct CalendarEvent *cale,
                   time_t adv,
                   struct tm *date1,
                   struct tm *date2,
                   struct tm *tm_prev,
                   struct tm *tm_next,
                   int *prev_found,
                   int *next_found)
{
   struct tm t;
   time_t t1, t2;
   time_t t_begin, t_end;
   time_t t_alarm;
   time_t t_offset;
   time_t t_temp;
   int forward, backward;
   int offset;
   int freq;
   int date1_days, begin_days;
   int fdom, ndim;
   int found, count, i;
   int safety_counter;
   int found_exception;
   int kill_update_next;
#ifdef ALARMS_DEBUG
   char str[100];
#endif

#ifdef ALARMS_DEBUG
   printf("fpn: entered find_previous_next\n");
#endif
   *prev_found=*next_found=0;
   forward=backward=1;

   t1=mktime_dst_adj(date1);
   t2=mktime_dst_adj(date2);

   memset(tm_prev, 0, sizeof(*tm_prev));
   memset(tm_next, 0, sizeof(*tm_next));

   /* Initialize search time with cale start time */
   memset(&t, 0, sizeof(t));
   t.tm_year=cale->begin.tm_year;
   t.tm_mon=cale->begin.tm_mon;
   t.tm_mday=cale->begin.tm_mday;
   t.tm_hour=cale->begin.tm_hour;
   t.tm_min=cale->begin.tm_min;
   t.tm_isdst=-1;
   mktime(&t);
#ifdef ALARMS_DEBUG
   strftime(str, sizeof(str), "%B %d, %Y %H:%M", &t);
   printf("fpn: appt_start=%s\n", str);
#endif

   /* Handle non-repeating appointments */        
   if (cale->repeatType == calendarRepeatNone) {
#ifdef ALARMS_DEBUG
      printf("fpn: repeatNone\n");
#endif
      t_alarm=mktime_dst_adj(&(cale->begin)) - adv;
      if ((t_alarm <= t2) && (t_alarm >= t1)) {
         memcpy(tm_prev, &(cale->begin), sizeof(struct tm));
         *prev_found=1;
#ifdef ALARMS_DEBUG
         printf("fpn: prev_found none\n");
#endif
      } else if (t_alarm > t2) {
         memcpy(tm_next, &(cale->begin), sizeof(struct tm));
         *next_found=1;
#ifdef ALARMS_DEBUG
         printf("fpn: next_found none\n");
#endif
      }
      return EXIT_SUCCESS;
   }

   /* Optimize initial start position of search */ 
   switch (cale->repeatType) {
    case repeatNone:
      /* Already handled.  Here only to shut up compiler warnings */
      break;
    case repeatDaily:
#ifdef ALARMS_DEBUG
      printf("fpn: repeatDaily\n");
#endif
      freq = cale->repeatFrequency;
      t_offset = freq * DAY_IN_SECS;
      t_alarm = mktime_dst_adj(&t);
      /* Jump to closest current date if appt. started in the past */
      if (t1 - adv > t_alarm)
      {
         t_alarm = ((((t1+adv)-t_alarm) / t_offset) * t_offset) + t_alarm;
         memcpy(&t, localtime(&t_alarm), sizeof(struct tm));
#ifdef ALARMS_DEBUG
         strftime(str, sizeof(str), "%B %d, %Y %H:%M", &t);
         printf("fpn: initial daily=%s\n", str);
#endif
      }
      break;
    case repeatWeekly:
#ifdef ALARMS_DEBUG
      printf("fpn: repeatWeekly\n");
#endif
      freq = cale->repeatFrequency;
      begin_days = dateToDays(&(cale->begin));
      date1_days = dateToDays(date1);
      /* Jump to closest current date if appt. started in the past */
      if (date1_days > begin_days) {
#ifdef ALARMS_DEBUG
         printf("fpn: begin_days %d date1_days %d\n", begin_days, date1_days);
         printf("fpn: date1->tm_wday %d appt->begin.tm_wday %d\n", date1->tm_wday, cale->begin.tm_wday);
#endif
         /* Jump by appropriate number of weeks */
         offset = date1_days - begin_days;
         offset = (offset/(freq*7))*(freq*7);
#ifdef ALARMS_DEBUG
         printf("fpn: offset %d\n", offset);
#endif

         add_days_to_date(&t, offset);
      }

      /* Within the week find which day is a repeat */
      found=0;
      for (count=0, i=t.tm_wday; i>=0; i--, count++) {
         if (cale->repeatDays[i]) {
            sub_days_from_date(&t, count);
            found=1;
            break;
         }
      }
      if (!found) {
         for (count=0, i=t.tm_wday; i<7; i++, count++) {
            if (cale->repeatDays[i]) {
               add_days_to_date(&t, count);
               found=1;
               break;
            }
         }
      }
#ifdef ALARMS_DEBUG
         strftime(str, sizeof(str), "%B %d, %Y %H:%M", &t);
         printf("fpn: initial weekly=%s\n", str);
#endif
      break;
    case repeatMonthlyByDay:
#ifdef ALARMS_DEBUG
      printf("fpn: repeatMonthlyByDay\n");
#endif
      /* Jump to closest current date if appt. started in the past */
      if ((date1->tm_year > cale->begin.tm_year) ||
          (date1->tm_mon  > cale->begin.tm_mon)) {
         /* First, adjust month */
         freq = cale->repeatFrequency;
         offset = ((date1->tm_year - cale->begin.tm_year)*12) -
                    cale->begin.tm_mon +
                    date1->tm_mon;
         offset = (offset/freq)*freq;
         add_months_to_date(&t, offset);

         /* Second, adjust to correct day in new month */
         get_month_info(t.tm_mon, 1, t.tm_year, &fdom, &ndim);
         t.tm_mday=((cale->repeatDay+7-fdom)%7) - ((cale->repeatDay)%7) + cale->repeatDay + 1;
#ifdef ALARMS_DEBUG
         printf("fpn: months offset = %d\n", offset);
         printf("fpn: %02d/01/%02d, fdom=%d\n", t.tm_mon+1, t.tm_year+1900, fdom);
         printf("fpn: mday = %d\n", t.tm_mday);
#endif
         if (t.tm_mday > ndim-1) {
            t.tm_mday -= 7;
         }
#ifdef ALARMS_DEBUG
         strftime(str, sizeof(str), "%B %d, %Y %H:%M", &t);
         printf("fpn: initial monthly by day=%s\n", str);
#endif
      }
      break;
    case repeatMonthlyByDate:
#ifdef ALARMS_DEBUG
      printf("fpn: repeatMonthlyByDate\n");
#endif
      /* Jump to closest current date if appt. started in the past */
      if ((date1->tm_year > cale->begin.tm_year) ||
          (date1->tm_mon  > cale->begin.tm_mon)) {
         freq = cale->repeatFrequency;
         offset = ((date1->tm_year - cale->begin.tm_year)*12) -
                    cale->begin.tm_mon +
                    date1->tm_mon;
         offset = (offset/freq)*freq;
#ifdef ALARMS_DEBUG
         printf("fpn: months offset = %d\n", offset);
#endif
         add_months_to_date(&t, offset);
      }
      break;
    case repeatYearly:
#ifdef ALARMS_DEBUG
      printf("fpn: repeatYearly\n");
#endif
      /* Jump to closest current date if appt. started in the past */
      if (date1->tm_year > cale->begin.tm_year) {
         freq = cale->repeatFrequency;
         offset = ((date1->tm_year - cale->begin.tm_year)/freq)*freq;
#ifdef ALARMS_DEBUG
         printf("fpn: (%d - %d)%%%d\n", date1->tm_year, cale->begin.tm_year, freq);
         printf("fpn: years offset = %d\n", offset);
#endif
         add_years_to_date(&t, offset);
      }
      break;

   } /* end switch on repeatType */

   /* Search forwards/backwards through time for alarms */
   safety_counter=0;
   while (forward || backward) {
      safety_counter++;
      if (safety_counter > 3000) {
         jp_logf(JP_LOG_STDOUT|JP_LOG_FILE, "find_prev_next(): %s\n", _("infinite loop, breaking\n"));
         if (cale->description) {
            jp_logf(JP_LOG_STDOUT|JP_LOG_FILE, "desc=[%s]\n", cale->description);
         }
         break;
      }

      kill_update_next = 0;
      t_temp = mktime_dst_adj(&t);
#ifdef ALARMS_DEBUG
      strftime(str, sizeof(str), "%B %d, %Y %H:%M", &t);
      printf("fpn: trying with=%s\n", str);
#endif

      /* Check for exceptions in repeat appointments */
      found_exception=0;
      for (i=0; i<cale->exceptions; i++) {
         if ((t.tm_mday==cale->exception[i].tm_mday) &&
             (t.tm_mon==cale->exception[i].tm_mon) &&
             (t.tm_year==cale->exception[i].tm_year)
             ) {
            found_exception=1;
            break;
         }
      }
      if (found_exception) {
         if (forward) {
            forward_backward_in_ce_time(cale, &t, 1);
            continue;
         }
         if (backward) {
            forward_backward_in_ce_time(cale, &t, -1);
            continue;
         }
      }

      /* Check that proposed alarm is not before the appt begin date */
      t_begin = mktime_dst_adj(&(cale->begin));
      if (t_temp < t_begin) {
#ifdef ALARMS_DEBUG
         printf("fpn: before begin date\n");
#endif
         backward=0;
         kill_update_next = 1;
      }
      /* Check that proposed alarm is not past appt end date */
      if (!(cale->repeatForever)) {
         t_end = mktime_dst_adj(&(cale->repeatEnd));
         if (t_temp >= t_end) {
#ifdef ALARMS_DEBUG
            printf("fpn: after end date\n");
#endif
            forward=0;
         }
      }

      /* Check if proposed alarm falls within the desired window [t1,t2] */
      t_temp-=adv;
      if (t_temp >= t2) {
         if (!kill_update_next) {
            memcpy(tm_next, &t, sizeof(t));
            *next_found=1;
            forward=0;
#ifdef ALARMS_DEBUG
            printf("fpn: next found\n");
#endif
         }
      } else {
         memcpy(tm_prev, &t, sizeof(t));
         *prev_found=1;
         backward=0;
#ifdef ALARMS_DEBUG
         printf("fpn: prev_found\n");
#endif
      }

      /* Change &t to the next/previous occurrence of the appointment 
       * and try search again */
      if (forward) {
         forward_backward_in_ce_time(cale, &t, 1);
         continue;
      }
      if (backward) {
         forward_backward_in_ce_time(cale, &t, -1);
         continue;
      }

   } /* end of while loop going forward/backward */

   return EXIT_SUCCESS;
}

/*
 * This routine takes time (t) and either advances t to the next
 * occurrence of a repeating appointment, or the previous occurrence
 *
 * appt is the appointment passed in
 * t is an in/out parameter
 * forward_or_backward should be 1 for forward or -1 for backward
 */
int forward_backward_in_ce_time(const struct CalendarEvent *cale,
                                struct tm *t,
                                int forward_or_backward)
{
   int count, dow, freq, fdom, ndim;

   freq = cale->repeatFrequency;

   /* Go forward in time */
   if (forward_or_backward==1) {
      switch (cale->repeatType) {
       case calendarRepeatNone:
#ifdef ALARMS_DEBUG
         printf("fbiat: repeatNone encountered.  This should never happen!\n");
#endif
         break;
       case calendarRepeatDaily:
         add_days_to_date(t, freq);
         break;
       case calendarRepeatWeekly:
         for (count=0, dow=t->tm_wday; count<14; count++) {
            add_days_to_date(t, 1);
#ifdef ALARMS_DEBUG
            printf("fbiat: weekly forward t.tm_wday=%d, freq=%d\n", t->tm_wday, freq);
#endif
            dow++;
            if (dow==7) {
#ifdef ALARMS_DEBUG
               printf("fbiat: dow==7\n");
#endif
               add_days_to_date(t, (freq-1)*7);
               dow=0;
            }
            if (cale->repeatDays[dow]) {
#ifdef ALARMS_DEBUG
               printf("fbiat: repeatDay[dow] dow=%d\n", dow);
#endif
               break;
            }
         }
         break;
       case calendarRepeatMonthlyByDay:
         add_months_to_date(t, freq);
         get_month_info(t->tm_mon, 1, t->tm_year, &fdom, &ndim);
         t->tm_mday=((cale->repeatDay+7-fdom)%7) - ((cale->repeatDay)%7) + cale->repeatDay + 1;
         if (t->tm_mday > ndim-1) {
            t->tm_mday -= 7;
         }
         break;
       case calendarRepeatMonthlyByDate:
         t->tm_mday=cale->begin.tm_mday;
         add_months_to_date(t, freq);
         break;
       case calendarRepeatYearly:
         t->tm_mday=cale->begin.tm_mday;
         add_years_to_date(t, freq);
         break;
      } /* switch on repeatType */

      return EXIT_SUCCESS;
   }

   /* Go back in time */
   if (forward_or_backward==-1) {
      switch (cale->repeatType) {
       case calendarRepeatNone:
#ifdef ALARMS_DEBUG
         printf("fbiat: repeatNone encountered.  This should never happen!\n");
#endif
         break;
       case calendarRepeatDaily:
         sub_days_from_date(t, freq);
         break;
       case calendarRepeatWeekly:
         for (count=0, dow=t->tm_wday; count<14; count++) {
            sub_days_from_date(t, 1);
#ifdef ALARMS_DEBUG
            printf("fbiat: weekly backward t.tm_wday=%d, freq=%d\n", t->tm_wday, freq);
#endif
            dow--;
            if (dow==-1) {
#ifdef ALARMS_DEBUG
               printf("fbiat: dow==-1\n");
#endif
               sub_days_from_date(t, (freq-1)*7);
               dow=6;
            }
            if (cale->repeatDays[dow]) {
#ifdef ALARMS_DEBUG
               printf("fbiat: repeatDay[dow] dow=%d\n", dow);
#endif
               break;
            }
         }
         break;
       case calendarRepeatMonthlyByDay:
         sub_months_from_date(t, freq);
         get_month_info(t->tm_mon, 1, t->tm_year, &fdom, &ndim);
         t->tm_mday=((cale->repeatDay+7-fdom)%7) - ((cale->repeatDay)%7) + cale->repeatDay + 1;
         if (t->tm_mday > ndim-1) {
            t->tm_mday -= 7;
         }
         break;
       case calendarRepeatMonthlyByDate:
         t->tm_mday=cale->begin.tm_mday;
         sub_months_from_date(t, freq);
         break;
       case calendarRepeatYearly:
         t->tm_mday=cale->begin.tm_mday;
         sub_years_from_date(t, freq);
         break;
      } /* switch on repeatType */
   }

   return EXIT_SUCCESS;
}

/* Displays usage string on supplied file handle */
void fprint_usage_string(FILE *out)
{
   fprintf(out, "%s [ -v || -h || [-d] [-p] [-a || -A] [-s] [-i] [-geometry] ]\n", EPN);
   fprintf(out, _(" -v display version and compile options\n"));
   fprintf(out, _(" -h display help text\n"));
   fprintf(out, _(" -d display debug info to stdout\n"));
   fprintf(out, _(" -p skip loading plugins\n"));
   fprintf(out, _(" -a ignore missed alarms since the last time program was run\n"));
   fprintf(out, _(" -A ignore all alarms past and future\n"));
   fprintf(out, _(" -s start sync using existing instance of GUI\n"));
   fprintf(out, _(" -i iconify program immediately after launch\n"));
   fprintf(out, _(" -geometry {X geometry} use specified geometry for main window\n\n"));
   fprintf(out, _(" The PILOTPORT and PILOTRATE environment variables specify\n"));
   fprintf(out, _(" which port to sync on, and at what speed.\n"));
   fprintf(out, _(" If PILOTPORT is not set then it defaults to /dev/pilot.\n"));
}

void free_mem_rec_header(mem_rec_header **mem_rh)
{
   mem_rec_header *h, *next_h;

   for (h=*mem_rh; h; h=next_h) {
      next_h=h->next;
      free(h);
   }
   *mem_rh=NULL;
}

void free_search_record_list(struct search_record **sr)
{
   struct search_record *temp_sr, *temp_sr_next;

   for (temp_sr = *sr; temp_sr; temp_sr=temp_sr_next) {
      temp_sr_next = temp_sr->next;
      free(temp_sr);
   }
   *sr = NULL;
}

/* Warning, this function will move the file pointer */
int get_app_info_size(FILE *in, int *size)
{
   unsigned char raw_header[LEN_RAW_DB_HEADER];
   DBHeader dbh;
   unsigned int offset;
   record_header rh;

   fseek(in, 0, SEEK_SET);

   if (fread(raw_header, LEN_RAW_DB_HEADER, 1, in) < 1) {
      jp_logf(JP_LOG_WARN, "fread failed %s %d\n", __FILE__, __LINE__);
   }
   if (feof(in)) {
      jp_logf(JP_LOG_WARN, "get_app_info_size(): %s\n", _("Error reading file"));
      return EXIT_FAILURE;
   }

   unpack_db_header(&dbh, raw_header);

   if (dbh.app_info_offset==0) {
      *size=0;
      return EXIT_SUCCESS;
   }
   if (dbh.sort_info_offset!=0) {
      *size = dbh.sort_info_offset - dbh.app_info_offset;
      return EXIT_SUCCESS;
   }
   if (dbh.number_of_records==0) {
      fseek(in, 0, SEEK_END);
      *size=ftell(in) - dbh.app_info_offset;
      return EXIT_SUCCESS;
   }

   if (fread(&rh, sizeof(record_header), 1, in) < 1) {
      jp_logf(JP_LOG_WARN, "fread failed %s %d\n", __FILE__, __LINE__);
   }
   offset = ((rh.Offset[0]*256+rh.Offset[1])*256+rh.Offset[2])*256+rh.Offset[3];
   *size=offset - dbh.app_info_offset;

   return EXIT_SUCCESS;
}

void get_compile_options(char *string, int len)
{
   g_snprintf(string, len,
              PN" version "VERSION"\n"
              "  Copyright (C) 1999-2014 by Judd Montgomery\n"
              "  judd@jpilot.org, http://jpilot.org\n"
              "\n"
              PN" comes with ABSOLUTELY NO WARRANTY; for details see the file\n"
              "COPYING included with the source code, or in /usr/share/docs/jpilot/.\n\n"
              "This program is free software; you can redistribute it and/or modify\n"
              "it under the terms of the GNU General Public License as published by\n"
              "the Free Software Foundation; version 2 of the License.\n\n"
              "%s %s %s\n"
              "%s\n"
              "  %s - %s\n"
              "  %s - %d.%d.%d\n"
              "  %s - %s\n"
              "  %s - %s\n"
              "  %s - %s\n"
              "  %s - %s\n"
              "  %s - %s\n"
              "  %s - %s\n"
              "  %s - %s",
              _("Date compiled"), __DATE__, __TIME__,
              _("Compiled with these options:"),

              _("Installed Path"),
              BASE_DIR,
              _("pilot-link version"),
              PILOT_LINK_VERSION,
              PILOT_LINK_MAJOR,
              PILOT_LINK_MINOR,
              _("USB support"),
              _("yes"),
              _("Private record support"),
#ifdef ENABLE_PRIVATE
              _("yes"),
#else
              _("no"),
#endif
              _("Datebk support"),
#ifdef ENABLE_DATEBK
              _("yes"),
#else
              _("no"),
#endif
              _("Plugin support"),
#ifdef ENABLE_PLUGINS
              _("yes"),
#else
              _("no"),
#endif
              _("Manana support"),
#ifdef ENABLE_MANANA
              _("yes"),
#else
              _("no"),
#endif
              _("NLS support (foreign languages)"),
#ifdef ENABLE_NLS
              _("yes"),
#else
              _("no"),
#endif
              _("GTK2 support"),
              _("yes")
              );
}

/* Get today's date and work out day in month. This is used to highlight
 * today in the gui (read-only) views. Returns the day of month if today
 * is in the passed month else returns -1.  */
int get_highlighted_today(struct tm *date)
{
   time_t now;
   struct tm* now_tm;

   /* Quit immediately if the user option is not enabled */
   if (!get_pref_int_default(PREF_DATEBOOK_HI_TODAY, FALSE))
      return -1;

   /* Get now time */
   now = time(NULL);
   now_tm = localtime(&now);

   /* Check if option is on and return today's day of month if the month
    * and year match was was passed in */
   if (now_tm->tm_mon != date->tm_mon || now_tm->tm_year != date->tm_year)
      return -1;

   /* Today is within the passed month, return the day of month */
   return now_tm->tm_mday;
}

/* creates the full path name of a file in the ~/.jpilot dir */
int get_home_file_name(const char *file, char *full_name, int max_size)
{
   char *home, default_path[]=".";

#ifdef ENABLE_PROMETHEON
   home = getenv("COPILOT_HOME");
#else
   home = getenv("JPILOT_HOME");
#endif
   if (!home) {/* No JPILOT_HOME var */
      home = getenv("HOME");
      if (!home) {/* No HOME var */
         fprintf(stderr, _("Can't get HOME environment variable\n"));
      }
   }
   if (!home) {
      home = default_path;
   }
   if (strlen(home)>(max_size-strlen(file)-strlen("/."EPN"/")-2)) {
      fprintf(stderr, _("HOME environment variable is too long to process\n"));
      home=default_path;
   }
   sprintf(full_name, "%s/."EPN"/%s", home, file);
   return EXIT_SUCCESS;
}

/*
 * month = 0-11
 * day = day of month 1-31
 * returns: 
 * dow = day of week for this date
 * ndim = number of days in month 28-31
 */
void get_month_info(int month, int day, int year, int *dow, int *ndim)
{
   time_t ltime;
   struct tm *now;
   struct tm new_time;
   int days_in_month[]={31,28,31,30,31,30,31,31,30,31,30,31
   };

   time(&ltime);
   now = localtime(&ltime);

   new_time.tm_sec=0;
   new_time.tm_min=0;
   new_time.tm_hour=11;
   new_time.tm_mday=day; /* day of month 1-31 */
   new_time.tm_mon=month;
   new_time.tm_year=year;
   new_time.tm_isdst=now->tm_isdst;

   mktime(&new_time);
   *dow = new_time.tm_wday;

   /* leap year */
   if (month == 1) {
      if ((year%4 == 0) &&
          !(((year+1900)%100==0) && ((year+1900)%400!=0))
          ) {
         days_in_month[1]++;
      }
   }
   *ndim = days_in_month[month];
}

int get_next_unique_pc_id(unsigned int *next_unique_id)
{
   FILE *pc_in_out;
   char file_name[FILENAME_MAX];
   char str[256];

   /* Check that file exists and is not empty.  If not,
    * create it and start unique id numbering from 1 */
   pc_in_out = jp_open_home_file(EPN".next_id", "a");
   if (pc_in_out==NULL) {
      jp_logf(JP_LOG_WARN, _("Error opening file: %s\n"),file_name);
      return EXIT_FAILURE;
   }
   if (ftell(pc_in_out)==0) {
      /* The file is new.  We have to write out the file header */
      *next_unique_id=1;
      write_to_next_id_open(pc_in_out, *next_unique_id);
   }
   jp_close_home_file(pc_in_out);

   /* Now that file has been verified we can use it to find the next id */
   pc_in_out = jp_open_home_file(EPN".next_id", "r+");
   if (pc_in_out==NULL) {
      jp_logf(JP_LOG_WARN, _("Error opening file: %s\n"),file_name);
      return EXIT_FAILURE;
   }
   memset(str, '\0', sizeof(FILE_VERSION)+4);
   if (fread(str, strlen(FILE_VERSION), 1, pc_in_out) < 1) {
      jp_logf(JP_LOG_WARN, "fread failed %s %d\n", __FILE__, __LINE__);
   }
   if (!strcmp(str, FILE_VERSION)) {
      /* Must be a versioned file */
      fseek(pc_in_out, 0, SEEK_SET);
      if (fgets(str, 200, pc_in_out) == NULL) {
         jp_logf(JP_LOG_WARN, "fgets failed %s %d\n", __FILE__, __LINE__);
      }
      if (fgets(str, 200, pc_in_out) == NULL) {
         jp_logf(JP_LOG_WARN, "fgets failed %s %d\n" __FILE__, __LINE__);
      }
      str[200]='\0';
      *next_unique_id = atoi(str);
   } else {
      fseek(pc_in_out, 0, SEEK_SET);
      if (fread(next_unique_id, sizeof(*next_unique_id), 1, pc_in_out) < 1) {
         jp_logf(JP_LOG_WARN, "fread failed %s %d\n", __FILE__, __LINE__);
      }
   }
   (*next_unique_id)++;
   if (fseek(pc_in_out, 0, SEEK_SET)) {
      jp_logf(JP_LOG_WARN, "fseek failed %s %d\n", __FILE__, __LINE__);
   }
   /* rewind(pc_in_out); */
   /* todo - if > 16777216 then cleanup */

   write_to_next_id_open(pc_in_out, *next_unique_id);
   jp_close_home_file(pc_in_out);
   
   return EXIT_SUCCESS;
}

int get_pixmaps(GtkWidget *widget,
                int which_pixmap,
                GdkPixmap **out_pixmap,
                GdkBitmap **out_mask)
{
   /* Externally stored icon definitions */
   #include "icons/clist_mini_icons.h"

   static int init_done=0;
   static GdkPixmap *pixmap_note;
   static GdkPixmap *pixmap_alarm;
   static GdkPixmap *pixmap_check;
   static GdkPixmap *pixmap_checked;
   static GdkPixmap *pixmap_float_check;
   static GdkPixmap *pixmap_float_checked;
   static GdkPixmap *pixmap_sdcard;
   static GdkBitmap *mask_note;
   static GdkBitmap *mask_alarm;
   static GdkBitmap *mask_check;
   static GdkBitmap *mask_checked;
   static GdkBitmap *mask_float_check;
   static GdkBitmap *mask_float_checked;
   static GdkBitmap *mask_sdcard;
   GtkStyle *style;

   /* Pixmaps are created only once when procedure is first called */
   if (!init_done) {

      init_done = 1;

      /* Make the note pixmap */
      style = gtk_widget_get_style(widget);
      pixmap_note = gdk_pixmap_create_from_xpm_d(widget->window, &mask_note,
                                                 &style->bg[GTK_STATE_NORMAL],
                                                 (gchar **)xpm_note);

      /* Make the alarm pixmap */
      pixmap_alarm = gdk_pixmap_create_from_xpm_d(widget->window, &mask_alarm,
                                                  &style->bg[GTK_STATE_NORMAL],
                                                  (gchar **)xpm_alarm);

      /* Make the check pixmap */
      pixmap_check = gdk_pixmap_create_from_xpm_d(widget->window, &mask_check,
                                                  &style->bg[GTK_STATE_NORMAL],
                                                  (gchar **)xpm_check);

      /* Make the checked pixmap */
      pixmap_checked = gdk_pixmap_create_from_xpm_d
                         (widget->window, &mask_checked,
                          &style->bg[GTK_STATE_NORMAL],
                          (gchar **)xpm_checked);

      /* Make the float_checked pixmap */
      pixmap_float_check = gdk_pixmap_create_from_xpm_d
                             (widget->window, &mask_float_check,
                              &style->bg[GTK_STATE_NORMAL],
                              (gchar **)xpm_float_check);

      /* Make the float_checked pixmap */
      pixmap_float_checked = gdk_pixmap_create_from_xpm_d
                               (widget->window, &mask_float_checked,
                                &style->bg[GTK_STATE_NORMAL],
                                (gchar **)xpm_float_checked);

      /* Make the sdcard pixmap */
      pixmap_sdcard = gdk_pixmap_create_from_xpm_d(widget->window, &mask_sdcard,
                                                   &style->bg[GTK_STATE_NORMAL],
                                                   (gchar **)xpm_sdcard);

   }   /* End initialization of pixmaps */

   switch (which_pixmap) {
    case PIXMAP_NOTE:
      *out_pixmap = pixmap_note;
      *out_mask = mask_note;
      break;
    case PIXMAP_ALARM:
      *out_pixmap = pixmap_alarm;
      *out_mask = mask_alarm;
      break;
    case PIXMAP_BOX_CHECK:
      *out_pixmap = pixmap_check;
      *out_mask = mask_check;
      break;
    case PIXMAP_BOX_CHECKED:
      *out_pixmap = pixmap_checked;
      *out_mask = mask_checked;
      break;
    case PIXMAP_FLOAT_CHECK:
      *out_pixmap = pixmap_float_check;
      *out_mask = mask_float_check;
      break;
    case PIXMAP_FLOAT_CHECKED:
      *out_pixmap = pixmap_float_checked;
      *out_mask = mask_float_checked;
      break;
    case PIXMAP_SDCARD:
      *out_pixmap = pixmap_sdcard;
      *out_mask = mask_sdcard;
      break;
    default:
      *out_pixmap = NULL;
      *out_mask = NULL;
   }

   return EXIT_SUCCESS;
}

int get_timeout_interval(void)
{
   const char *svalue;

   get_pref(PREF_TIME, NULL, &svalue);
   if (strstr(svalue,"%S"))
      return CLOCK_TICK;
   else
      return 60*CLOCK_TICK;
}

int jp_cal_dialog(GtkWindow *main_window,
                  const char *title, int monday_is_fdow,
                  int *mon, int *day, int *year)
{
   return cal_dialog(main_window,
                     title, monday_is_fdow,
                     mon, day, year);
}

void jp_charset_j2p(char *const buf, int max_len)
{
   long char_set;

   get_pref(PREF_CHAR_SET, &char_set, NULL);
   charset_j2p(buf, max_len, char_set);
}

void jp_charset_p2j(char *const buf, int max_len)
{
   long char_set;

   get_pref(PREF_CHAR_SET, &char_set, NULL);
   if (char_set == CHAR_SET_JAPANESE) 
      jp_Sjis2Euc(buf, max_len);
   else 
      charset_p2j(buf, max_len, char_set);

}

char* jp_charset_p2newj(const char *buf, int max_len)
{
   long char_set;

   get_pref(PREF_CHAR_SET, &char_set, NULL);
   return(charset_p2newj(buf, max_len, char_set));
}

int jp_close_home_file(FILE *pc_in)
{
   /* unlock access */
#ifndef USE_FLOCK
   struct flock lock;
   int  r;

   lock.l_type = F_UNLCK;
   lock.l_start = 0;
   lock.l_whence = SEEK_SET;
   lock.l_len = 0;
   r = fcntl(fileno(pc_in), F_SETLK, &lock);
   if (r == -1)
#else
   if (flock(fileno(pc_in), LOCK_UN) < 0)
#endif
      jp_logf(JP_LOG_WARN, "unlocking failed: %s\n", strerror(errno));

   return fclose(pc_in);
}

int jp_copy_file(char *src, char *dest)
{
   FILE *in, *out;
   int r;
   struct stat statb;
   struct utimbuf times;
   unsigned char buf[10002];

   if (!strcmp(src, dest)) {
      return EXIT_SUCCESS;
   }

   in = fopen(src, "r");
   out = fopen(dest, "w");
   if (!in) {
      return EXIT_FAILURE;
   }
   if (!out) {
      fclose(in);
      return EXIT_FAILURE;
   }
   while ((r = fread(buf, 1, sizeof(buf)-2, in))) {
      fwrite(buf, 1, r, out);
   }
   fclose(in);
   fclose(out);

   /* Set the create and modify times of new file to the same as the old */
   stat(src, &statb);
   times.actime = statb.st_atime;
   times.modtime = statb.st_mtime;
   utime(dest, &times);

   return EXIT_SUCCESS;
}

FILE *jp_open_home_file(const char *filename, const char *mode)
{
   char fullname[FILENAME_MAX];
   FILE *pc_in;

   get_home_file_name(filename, fullname, sizeof(fullname));

   pc_in = fopen(fullname, mode);
   if (pc_in == NULL) {
      pc_in = fopen(fullname, "w+");
      if (pc_in) {
         fclose(pc_in);
         pc_in = fopen(fullname, mode);
      }
   }

   /* if the file exists */
   if (pc_in)
   {
      /* lock access */
#ifndef USE_FLOCK
      struct flock lock;
      int  r;

      if (*mode == 'r')
         lock.l_type = F_RDLCK;
      else
         lock.l_type = F_WRLCK;
      lock.l_start = 0;
      lock.l_whence = SEEK_SET;
      lock.l_len = 0; /* Lock to the end of file */
      r = fcntl(fileno(pc_in), F_SETLK, &lock);
      if (r == -1)
#else
      if (flock(fileno(pc_in), LOCK_EX) < 0)
#endif
      {
         jp_logf(JP_LOG_WARN, "locking %s failed: %s\n", filename, strerror(errno));
         if (ENOLCK != errno)
         {
            fclose(pc_in);
            return NULL;
         }
         else
            jp_logf(JP_LOG_WARN, "continue without locking\n");
      }

      /* Enhance privacy by only allowing user to read & write files */
      chmod(fullname, 0600);
   }

   return pc_in;
}

size_t jp_strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
   size_t ret;

   gchar *utf8_text;
   gchar *local_format;

   /* the format string is UTF-8 encoded since it comes from a .po file */
   local_format = g_locale_from_utf8(format, -1, NULL, NULL, NULL);

   ret = strftime(s, max, local_format, tm);
   g_free(local_format);

   utf8_text = g_locale_to_utf8(s, -1, NULL, NULL, NULL);
   g_strlcpy(s, utf8_text, max);
   g_free(utf8_text);

   return ret;
}

/* RFC 2849 */
void ldif_out(FILE *f, const char *name, const char *fmt, ...)
{
   va_list ap;
   unsigned char buf[8192];
   char *p;
   int printable = 1;

   va_start(ap, fmt);
   vsnprintf((char *)buf, sizeof(buf), fmt, ap);
   if (buf[0] == ' ' || buf[0] == ':' || buf[0] == '<') /* SAFE-INIT-CHAR */ {
      printable = 0;
   }
   for (p = (char *)buf; *p && printable; p++) {
      if (*p < 32 || *p > 126) { /* SAFE-CHAR, excluding all control chars */
         printable = 0;
      }
      if (*p == ' ' && *(p + 1) == '\0') { /* note 8 */
         printable = 0;
      }
   }
   if (printable) {
      fprintf(f, "%s: %s\n", name, buf);
   } else {
      fprintf(f, "%s:: ", name);
      base64_out(f, buf);
      fprintf(f, "\n");
   }
}

/* Parse the string and replace CR and LFs with spaces
 * a null is written if len is reached */
void lstrncpy_remove_cr_lfs(char *dest, char *src, int len)
{
   int i;
   gchar* end;

   if ((!src) || (!dest)) {
      return;
   }

   dest[0]='\0';
   for (i=0; src[i] && (i<len); i++) {
      if ((src[i]=='\r') || (src[i]=='\n')) {
         dest[i]=' ';
      } else {
         dest[i]=src[i];
      }
   }
   if (i==len) {
      dest[i-1]='\0';
   } else {
      dest[i]='\0';
   }

   /* truncate the string on a UTF-8 character boundary */
   if (!g_utf8_validate(dest, -1, (const gchar **)&end))
     *end = 0;
}

int make_category_menu(GtkWidget **category_menu,
                       GtkWidget **cat_menu_item,
                       struct sorted_cats *sort_l,
                       void (*selection_callback)
                       (GtkWidget *item, int selection),
                       int add_an_all_item,
                       int add_edit_cat_item)
{
   GtkWidget *menu;
   GSList    *group;
   int i;
   int offset;

   *category_menu = gtk_option_menu_new();

   menu = gtk_menu_new();
   group = NULL;

   offset=0;
   if (add_an_all_item) {
      cat_menu_item[0] = gtk_radio_menu_item_new_with_label(group, _("All"));
      if (selection_callback) {
         gtk_signal_connect(GTK_OBJECT(cat_menu_item[0]), "activate",
                            GTK_SIGNAL_FUNC(selection_callback), GINT_TO_POINTER(CATEGORY_ALL));
      }
      group = gtk_radio_menu_item_group(GTK_RADIO_MENU_ITEM(cat_menu_item[0]));
      gtk_menu_append(GTK_MENU(menu), cat_menu_item[0]);
      gtk_widget_show(cat_menu_item[0]);
      offset=1;
   }

   for (i=0; i<NUM_CAT_ITEMS; i++) {
      if (sort_l[i].Pcat[0]) {
         cat_menu_item[i+offset] = gtk_radio_menu_item_new_with_label(
            group, sort_l[i].Pcat);
         if (selection_callback) {
            gtk_signal_connect(GTK_OBJECT(cat_menu_item[i+offset]), "activate",
                               GTK_SIGNAL_FUNC(selection_callback), GINT_TO_POINTER(sort_l[i].cat_num));
         }
         group = gtk_radio_menu_item_group(GTK_RADIO_MENU_ITEM(cat_menu_item[i+offset]));
         gtk_menu_append(GTK_MENU(menu), cat_menu_item[i+offset]);
         gtk_widget_show(cat_menu_item[i+offset]);
      }
      else
         cat_menu_item[i+offset] = NULL;
   }

   if (add_edit_cat_item) {
      cat_menu_item[i+offset] = gtk_radio_menu_item_new_with_label(group, 
                                   _("Edit Categories..."));
      if (selection_callback) {
         gtk_signal_connect(GTK_OBJECT(cat_menu_item[i+offset]), "activate",
                            GTK_SIGNAL_FUNC(selection_callback), GINT_TO_POINTER(i+offset));
      }
      group = gtk_radio_menu_item_group(GTK_RADIO_MENU_ITEM(cat_menu_item[i+offset]));
      gtk_menu_append(GTK_MENU(menu), cat_menu_item[i+offset]);
      gtk_widget_show(cat_menu_item[i+offset]);
   }

   gtk_option_menu_set_menu(GTK_OPTION_MENU(*category_menu), menu);

   return EXIT_SUCCESS;
}

time_t mktime_dst_adj(struct tm *tm)
{
   struct tm t;

   memcpy(&t, tm, sizeof(t));
   t.tm_isdst=-1;
   return mktime(&t);
}

char *multibyte_safe_memccpy(char *dst, const char *src, int c, size_t len)
{
   long char_set;

   if (len == 0) return NULL;
   if (dst == NULL) return NULL;
   if (src == NULL) return NULL;

   get_pref(PREF_CHAR_SET, &char_set, NULL);

   if (char_set == CHAR_SET_JAPANESE ||
       char_set == CHAR_SET_TRADITIONAL_CHINESE ||
       char_set == CHAR_SET_KOREAN
      ) {  /* Multibyte Characters */
      char *p, *q;
      int n = 0;

      p = (char *)src;
      q = dst;
      while ((*p) && (n < (len -2))) {
         if ((*p) & 0x80) {
            *q++ = *p++;
            n++;
            if (*p) {
               *q++ = *p++;
               n++;
            }
         } else {
            *q++ = *p++;
            n++;
         }
         if (*(p-1) == (char)(c & 0xff))
            return q;
      }
      if (!(*p & 0x80) && (n < len-1))
        *q++ = *p++;

      *q = '\0';
      return NULL;
   } else
      return memccpy(dst, src, c, len);
}

void multibyte_safe_strncpy(char *dst, char *src, size_t len)
{
   long char_set;

   get_pref(PREF_CHAR_SET, &char_set, NULL);

   if (char_set == CHAR_SET_JAPANESE ||
       char_set == CHAR_SET_TRADITIONAL_CHINESE ||
       char_set == CHAR_SET_KOREAN
      ) {
      char *p, *q;
      int n = 0;
      p = src; q = dst;
      while ((*p) && n < (len-2)) {
         if ((*p) & 0x80) {
            *q++ = *p++;
            n++;
            if (*p) {
               *q++ = *p++;
               n++;
            }
         } else {
            *q++ = *p++;
            n++;
         }
      }
      if (!(*p & 0x80 ) && (n < len-1))
        *q++ = *p++;

      *q = '\0';
   } else {
      strncpy(dst, src, len);
   }
}

int pdb_file_count_recs(char *DB_name, int *num)
{
   char local_pdb_file[FILENAME_MAX];
   char full_local_pdb_file[FILENAME_MAX];
   struct pi_file *pf;

   jp_logf(JP_LOG_DEBUG, "pdb_file_count_recs\n");

   *num = 0;

   g_snprintf(local_pdb_file, sizeof(local_pdb_file), "%s.pdb", DB_name);
   get_home_file_name(local_pdb_file, full_local_pdb_file, sizeof(full_local_pdb_file));

   pf = pi_file_open(full_local_pdb_file);
   if (!pf) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_local_pdb_file);
      return EXIT_FAILURE;
   }

   pi_file_get_entries(pf, num);

   pi_file_close(pf);

   return EXIT_SUCCESS;
}

int pdb_file_delete_record_by_id(char *DB_name, pi_uid_t uid_in)
{
   char local_pdb_file[FILENAME_MAX];
   char full_local_pdb_file[FILENAME_MAX];
   char full_local_pdb_file2[FILENAME_MAX];
   struct pi_file *pf1, *pf2;
   struct DBInfo infop;
   void *app_info;
   void *sort_info;
   void *record;
   int r;
   int idx;
   size_t size;
   int attr;
   int cat;
   pi_uid_t uid;

   jp_logf(JP_LOG_DEBUG, "pdb_file_delete_record_by_id\n");

   g_snprintf(local_pdb_file, sizeof(local_pdb_file), "%s.pdb", DB_name);
   get_home_file_name(local_pdb_file, full_local_pdb_file, sizeof(full_local_pdb_file));
   strcpy(full_local_pdb_file2, full_local_pdb_file);
   strcat(full_local_pdb_file2, "2");

   pf1 = pi_file_open(full_local_pdb_file);
   if (!pf1) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_local_pdb_file);
      return EXIT_FAILURE;
   }
   pi_file_get_info(pf1, &infop);
   pf2 = pi_file_create(full_local_pdb_file2, &infop);
   if (!pf2) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_local_pdb_file2);
      return EXIT_FAILURE;
   }

   pi_file_get_app_info(pf1, &app_info, &size);
   pi_file_set_app_info(pf2, app_info, size);

   pi_file_get_sort_info(pf1, &sort_info, &size);
   pi_file_set_sort_info(pf2, sort_info, size);

   for(idx=0;;idx++) {
      r = pi_file_read_record(pf1, idx, &record, &size, &attr, &cat, &uid);
      if (r<0) break;
      if (uid==uid_in) continue;
      pi_file_append_record(pf2, record, size, attr, cat, uid);
   }

   pi_file_close(pf1);
   pi_file_close(pf2);

   if (rename(full_local_pdb_file2, full_local_pdb_file) < 0) {
      jp_logf(JP_LOG_WARN, "pdb_file_delete_record_by_id(): %s\n", _("rename failed"));
   }

   return EXIT_SUCCESS;
}

/*
 * Original ID is in the case of a modification
 * new ID is used in the case of an add record
 */
int pdb_file_modify_record(char *DB_name, void *record_in, int size_in,
                           int attr_in, int cat_in, pi_uid_t uid_in)
{
   char local_pdb_file[FILENAME_MAX];
   char full_local_pdb_file[FILENAME_MAX];
   char full_local_pdb_file2[FILENAME_MAX];
   struct pi_file *pf1, *pf2;
   struct DBInfo infop;
   void *app_info;
   void *sort_info;
   void *record;
   int r;
   int idx;
   size_t size;
   int attr;
   int cat;
   int found;
   pi_uid_t uid;

   jp_logf(JP_LOG_DEBUG, "pdb_file_modify_record\n");

   g_snprintf(local_pdb_file, sizeof(local_pdb_file), "%s.pdb", DB_name);
   get_home_file_name(local_pdb_file, full_local_pdb_file, sizeof(full_local_pdb_file));
   strcpy(full_local_pdb_file2, full_local_pdb_file);
   strcat(full_local_pdb_file2, "2");

   pf1 = pi_file_open(full_local_pdb_file);
   if (!pf1) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_local_pdb_file);
      return EXIT_FAILURE;
   }
   pi_file_get_info(pf1, &infop);
   pf2 = pi_file_create(full_local_pdb_file2, &infop);
   if (!pf2) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_local_pdb_file2);
      return EXIT_FAILURE;
   }

   pi_file_get_app_info(pf1, &app_info, &size);
   pi_file_set_app_info(pf2, app_info, size);

   pi_file_get_sort_info(pf1, &sort_info, &size);
   pi_file_set_sort_info(pf2, sort_info, size);

   found = 0;

   for(idx=0;;idx++) {
      r = pi_file_read_record(pf1, idx, &record, &size, &attr, &cat, &uid);
      if (r<0) break;
      if (uid==uid_in) {
         pi_file_append_record(pf2, record_in, size_in, attr_in, cat_in, uid_in);
         found=1;
      } else {
         pi_file_append_record(pf2, record, size, attr, cat, uid);
      }
   }
   if (!found) {
      pi_file_append_record(pf2, record_in, size_in, attr_in, cat_in, uid_in);
   }

   pi_file_close(pf1);
   pi_file_close(pf2);

   if (rename(full_local_pdb_file2, full_local_pdb_file) < 0) {
      jp_logf(JP_LOG_WARN, "pdb_file_modify_record(): %s\n", _("rename failed"));
   }

   return EXIT_SUCCESS;
}

int pdb_file_read_record_by_id(char *DB_name,
                               pi_uid_t uid,
                               void **bufp, size_t *sizep, int *idxp,
                               int *attrp, int *catp)
{
   char local_pdb_file[FILENAME_MAX];
   char full_local_pdb_file[FILENAME_MAX];
   struct pi_file *pf1;
   void *temp_buf;
   int r;

   jp_logf(JP_LOG_DEBUG, "pdb_file_read_record_by_id\n");

   g_snprintf(local_pdb_file, sizeof(local_pdb_file), "%s.pdb", DB_name);
   get_home_file_name(local_pdb_file, full_local_pdb_file, sizeof(full_local_pdb_file));

   pf1 = pi_file_open(full_local_pdb_file);
   if (!pf1) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_local_pdb_file);
      return EXIT_FAILURE;
   }

   r = pi_file_read_record_by_id(pf1, uid, &temp_buf, sizep, idxp, attrp, catp);
   /* during the close bufp will be freed, so we copy it */
   if ( (r>=0) && (*sizep>0) ) {
      *bufp=malloc(*sizep);
      if (*bufp) {
         memcpy(*bufp, temp_buf, *sizep);
      }
   } else {
      *bufp=NULL;
   }

   pi_file_close(pf1);

   return r;
}

int pdb_file_write_app_block(char *DB_name, void *bufp, size_t size_in)
{
   char local_pdb_file[FILENAME_MAX];
   char full_local_pdb_file[FILENAME_MAX];
   char full_local_pdb_file2[FILENAME_MAX];
   struct pi_file *pf1, *pf2;
   struct DBInfo infop;
   void *app_info;
   void *sort_info;
   void *record;
   int r;
   int idx;
   size_t size;
   int attr;
   int cat;
   pi_uid_t uid;
   struct stat statb;
   struct utimbuf times;

   jp_logf(JP_LOG_DEBUG, "pdb_file_write_app_block\n");

   g_snprintf(local_pdb_file, sizeof(local_pdb_file), "%s.pdb", DB_name);
   get_home_file_name(local_pdb_file, full_local_pdb_file, sizeof(full_local_pdb_file));
   strcpy(full_local_pdb_file2, full_local_pdb_file);
   strcat(full_local_pdb_file2, "2");

   /* After we are finished, set the create and modify times of new file
      to the same as the old */
   stat(full_local_pdb_file, &statb);
   times.actime = statb.st_atime;
   times.modtime = statb.st_mtime;

   pf1 = pi_file_open(full_local_pdb_file);
   if (!pf1) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_local_pdb_file);
      return EXIT_FAILURE;
   }
   pi_file_get_info(pf1, &infop);
   pf2 = pi_file_create(full_local_pdb_file2, &infop);
   if (!pf2) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_local_pdb_file2);
      return EXIT_FAILURE;
   }

   pi_file_get_app_info(pf1, &app_info, &size);
   pi_file_set_app_info(pf2, bufp, size_in);

   pi_file_get_sort_info(pf1, &sort_info, &size);
   pi_file_set_sort_info(pf2, sort_info, size);

   for(idx=0;;idx++) {
      r = pi_file_read_record(pf1, idx, &record, &size, &attr, &cat, &uid);
      if (r<0) break;
      pi_file_append_record(pf2, record, size, attr, cat, uid);
   }

   pi_file_close(pf1);
   pi_file_close(pf2);

   if (rename(full_local_pdb_file2, full_local_pdb_file) < 0) {
      jp_logf(JP_LOG_WARN, "pdb_file_write_app_block(): %s\n", _("rename failed"));
   }

   utime(full_local_pdb_file, &times);

   return EXIT_SUCCESS;
}

/* DB_name is filename with extention and path, i.e: "/tmp/Net Prefs.prc" */
int pdb_file_write_dbinfo(char *full_DB_name, struct DBInfo *Pinfo_in)
{
   char full_local_pdb_file2[FILENAME_MAX];
   struct pi_file *pf1, *pf2;
   struct DBInfo infop;
   void *app_info;
   void *sort_info;
   void *record;
   int r;
   int idx;
   size_t size;
   int attr;
   int cat;
   pi_uid_t uid;
   struct stat statb;
   struct utimbuf times;

   jp_logf(JP_LOG_DEBUG, "pdb_file_write_dbinfo\n");

   g_snprintf(full_local_pdb_file2, sizeof(full_local_pdb_file2), "%s2", full_DB_name);

   /* After we are finished, set the create and modify times of new file
      to the same as the old */
   stat(full_DB_name, &statb);
   times.actime = statb.st_atime;
   times.modtime = statb.st_mtime;

   pf1 = pi_file_open(full_DB_name);
   if (!pf1) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_DB_name);
      return EXIT_FAILURE;
   }
   pi_file_get_info(pf1, &infop);
   /* Set the DBInfo to the one coming into the function */
   pf2 = pi_file_create(full_local_pdb_file2, Pinfo_in);
   if (!pf2) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s\n"), full_local_pdb_file2);
      return EXIT_FAILURE;
   }

   pi_file_get_app_info(pf1, &app_info, &size);
   pi_file_set_app_info(pf2, app_info, size);

   pi_file_get_sort_info(pf1, &sort_info, &size);
   pi_file_set_sort_info(pf2, sort_info, size);

   for(idx=0;;idx++) {
      r = pi_file_read_record(pf1, idx, &record, &size, &attr, &cat, &uid);
      if (r<0) break;
      pi_file_append_record(pf2, record, size, attr, cat, uid);
   }

   pi_file_close(pf1);
   pi_file_close(pf2);

   if (rename(full_local_pdb_file2, full_DB_name) < 0) {
      jp_logf(JP_LOG_WARN, "pdb_file_write_dbinfo(): %s\n", _("rename failed"));
   }

   utime(full_DB_name, &times);

   return EXIT_SUCCESS;
}

void print_string(char *str, int len)
{
   unsigned char c;
   int i;

   for (i=0;i<len;i++) {
      c=str[i];
      if (c < ' ' || c >= 0x7f)
         jp_logf(JP_LOG_STDOUT, "%x", c);
      else
         jp_logf(JP_LOG_STDOUT, "%c", c);
   }
   jp_logf(JP_LOG_STDOUT, "\n");
}

int read_gtkrc_file(void)
{
   char filename[FILENAME_MAX];
   char fullname[FILENAME_MAX];
   struct stat buf;
   const char *svalue;

   get_pref(PREF_RCFILE, NULL, &svalue);
   if (svalue) {
      jp_logf(JP_LOG_DEBUG, "rc file from prefs is %s\n", svalue);
   } else {
      jp_logf(JP_LOG_DEBUG, "rc file from prefs is NULL\n");
   }

   g_strlcpy(filename, svalue, sizeof(filename));

   /* Try to read the file out of the home directory first */
   get_home_file_name(filename, fullname, sizeof(fullname));

   if (stat(fullname, &buf)==0) {
      jp_logf(JP_LOG_DEBUG, "parsing %s\n", fullname);
      gtk_rc_parse(fullname);
      return EXIT_SUCCESS;
   }

   g_snprintf(fullname, sizeof(fullname), "%s/%s/%s/%s", BASE_DIR, "share", EPN, filename);
   if (stat(fullname, &buf)==0) {
      jp_logf(JP_LOG_DEBUG, "parsing %s\n", fullname);
      gtk_rc_parse(fullname);
      return EXIT_SUCCESS;
   }
   return EXIT_FAILURE;
}

/* Parse the string and replace CR and LFs with spaces */
void remove_cr_lfs(char *str)
{
   int i;

   if (!str) {
      return;
   }
   for (i=0; str[i]; i++) {
      if ((str[i]=='\r') || (str[i]=='\n')) {
         str[i]=' ';
      }
   }
}

void rename_dbnames(char dbname[][32])
{
   int i;
   long datebook_version, address_version, todo_version, memo_version;

   get_pref(PREF_DATEBOOK_VERSION, &datebook_version, NULL);
   get_pref(PREF_ADDRESS_VERSION, &address_version, NULL);
   get_pref(PREF_TODO_VERSION, &todo_version, NULL);
   get_pref(PREF_MEMO_VERSION, &memo_version, NULL);
   for (i=0; dbname[i] && dbname[i][0]; i++) {
      if (datebook_version==1) {
         if (!strcmp(dbname[i], "DatebookDB.pdb")) {
            strcpy(dbname[i], "CalendarDB-PDat.pdb");
         }
         if (!strcmp(dbname[i], "DatebookDB.pc3")) {
            strcpy(dbname[i], "CalendarDB-PDat.pc3");
         }
         if (!strcmp(dbname[i], "DatebookDB")) {
            strcpy(dbname[i], "CalendarDB-PDat");
         }
      }

      if (address_version==1) {
         if (!strcmp(dbname[i], "AddressDB.pdb")) {
            strcpy(dbname[i], "ContactsDB-PAdd.pdb");
         }
         if (!strcmp(dbname[i], "AddressDB.pc3")) {
            strcpy(dbname[i], "ContactsDB-PAdd.pc3");
         }
         if (!strcmp(dbname[i], "AddressDB")) {
            strcpy(dbname[i], "ContactsDB-PAdd");
         }
      }

      if (todo_version==1) {
         if (!strcmp(dbname[i], "ToDoDB.pdb")) {
            strcpy(dbname[i], "TasksDB-PTod.pdb");
         }
         if (!strcmp(dbname[i], "ToDoDB.pc3")) {
            strcpy(dbname[i], "TasksDB-PTod.pc3");
         }
         if (!strcmp(dbname[i], "ToDoDB")) {
            strcpy(dbname[i], "TasksDB-PTod");
         }
      }

      if (memo_version==1) {
         if (!strcmp(dbname[i], "MemoDB.pdb")) {
            strcpy(dbname[i], "MemosDB-PMem.pdb");
         }
         if (!strcmp(dbname[i], "MemoDB.pc3")) {
            strcpy(dbname[i], "MemosDB-PMem.pc3");
         }
         if (!strcmp(dbname[i], "MemoDB")) {
            strcpy(dbname[i], "MemosDB-PMem");
         }
      }

      if (memo_version==2) {
         if (!strcmp(dbname[i], "MemoDB.pdb")) {
            strcpy(dbname[i], "Memo32DB.pdb");
         }
         if (!strcmp(dbname[i], "MemoDB.pc3")) {
            strcpy(dbname[i], "Memo32DB.pc3");
         }
         if (!strcmp(dbname[i], "MemoDB")) {
            strcpy(dbname[i], "Memo32DB");
         }
      }
   }
}

int rename_file(char *old_filename, char *new_filename)
{
   char old_fullname[FILENAME_MAX];
   char new_fullname[FILENAME_MAX];

   get_home_file_name(old_filename, old_fullname, sizeof(old_fullname));
   get_home_file_name(new_filename, new_fullname, sizeof(new_fullname));

   return rename(old_fullname, new_fullname);
}

void set_bg_rgb_clist_row(GtkWidget *clist, int row, int r, int g, int b)
{
   GtkStyle *old_style, *new_style;
   GdkColor color;

   if ((old_style = gtk_widget_get_style(clist))) {
      new_style = gtk_style_copy(old_style);
   }
   else {
      new_style = gtk_style_new();
   }

   color.red=r;
   color.green=g;
   color.blue=b;
   color.pixel=0;

   new_style->base[GTK_STATE_NORMAL] = color;
   gtk_clist_set_row_style(GTK_CLIST(clist), row, new_style);
}

void set_fg_rgb_clist_cell(GtkWidget *clist, int row, int col, int r, int g, int b)
{
   GtkStyle *old_style, *new_style;
   GdkColor fg_color;

   if ((old_style = gtk_clist_get_row_style(GTK_CLIST(clist), row)) ||
       (old_style = gtk_widget_get_style(clist))) {
      new_style = gtk_style_copy(old_style);
   }
   else {
      new_style = gtk_style_new();
   }

   fg_color.red=r;
   fg_color.green=g;
   fg_color.blue=b;
   fg_color.pixel=0;

   new_style->fg[GTK_STATE_NORMAL]   = fg_color;
   new_style->fg[GTK_STATE_SELECTED] = fg_color;
   gtk_clist_set_cell_style(GTK_CLIST(clist), row, col, new_style);
}

int setup_sync(unsigned int flags)
{
   long num_backups;
   const char *svalue;
   const char *port;
   int r;
#ifndef HAVE_SETENV
   char str[80];
#endif
   struct my_sync_info sync_info;

   /* look in env for PILOTRATE first */
   if (!(getenv("PILOTRATE"))) {
      get_pref(PREF_RATE, NULL, &svalue);
      jp_logf(JP_LOG_DEBUG, "setting PILOTRATE=[%s]\n", svalue);
      if (svalue) {
#ifdef HAVE_SETENV
         setenv("PILOTRATE", svalue, TRUE);
#else
         sprintf(str, "PILOTRATE=%s", svalue);
         putenv(str);
#endif
      }
   }

   get_pref(PREF_PORT, NULL, &port);
   get_pref(PREF_NUM_BACKUPS, &num_backups, NULL);
   jp_logf(JP_LOG_DEBUG, "pref port=[%s]\n", port);
   jp_logf(JP_LOG_DEBUG, "num_backups=%d\n", num_backups);
   get_pref(PREF_USER, NULL, &svalue);
   g_strlcpy(sync_info.username, svalue, sizeof(sync_info.username));
   get_pref(PREF_USER_ID, &(sync_info.userID), NULL);

   get_pref(PREF_PC_ID, &(sync_info.PC_ID), NULL);
   if (sync_info.PC_ID == 0) {
      srandom(time(NULL));
      /* RAND_MAX is 32768 on Solaris machines for some reason.
       * If someone knows how to fix this, let me know. */
      if (RAND_MAX==32768) {
         sync_info.PC_ID = 1+(2000000000.0*random()/(2147483647+1.0));
      } else {
         sync_info.PC_ID = 1+(2000000000.0*random()/(RAND_MAX+1.0));
      }
      jp_logf(JP_LOG_WARN, _("PC ID is 0.\n"));
      jp_logf(JP_LOG_WARN, _("Generated a new PC ID.  It is %lu\n"), sync_info.PC_ID);
      set_pref(PREF_PC_ID, sync_info.PC_ID, NULL, TRUE);
   }
   
   sync_info.sync_over_ride = 0;
   g_strlcpy(sync_info.port, port, sizeof(sync_info.port));
   sync_info.flags=flags;
   sync_info.num_backups=num_backups;

   r = sync_once(&sync_info);

   return r;
}

/*
 * Copy src string into dest while escaping quotes with double quotes.
 * dest could be as long as strlen(src)*2.
 * Return value is the number of chars written to dest.
 */
int str_to_csv_str(char *dest, char *src)
{
   int s, d;

   if (dest) dest[0]='\0';
   if ((!src) || (!dest)) {
      return EXIT_SUCCESS;
   }

   s=d=0;
   while (src[s]) {
      if (src[s]=='\"') {
         dest[d++]='\"';
      }
      dest[d++]=src[s++];
   }
   dest[d++]='\0';

   return d;
}

/*
 * Copy src string into dest while escaping carriage returns with <br/>
 * dest could be as long as strlen(src)*5.
 * Return value is the number of chars written to dest.
 */
int str_to_keepass_str(char *dest, char *src)
{
   int s, d;

   if (dest) dest[0]='\0';
   if ((!src) || (!dest)) {
      return EXIT_SUCCESS;
   }

   s=d=0;
   while (src[s]) {
      if (src[s]=='\n') {
         dest[d++]='<';
         dest[d++]='b';
         dest[d++]='r';
         dest[d++]='/';
         dest[d++]='>';
	 s++;
	 continue;
      }
      if (src[s]=='&') {
         dest[d++]='&';
         dest[d++]='a';
         dest[d++]='m';
         dest[d++]='p';
         dest[d++]=';';
	 s++;
	 continue;
      }
      if (src[s]=='<') {
         dest[d++]='&';
         dest[d++]='l';
         dest[d++]='t';
         dest[d++]=';';
	 s++;
	 continue;
      }
      if (src[s]=='>') {
         dest[d++]='&';
         dest[d++]='g';
         dest[d++]='t';
         dest[d++]=';';
	 s++;
	 continue;
      }
      dest[d++]=src[s++];
   }
   dest[d++]='\0';

   return d;
}

/*
 * Quote a TEXT format string as specified by RFC 2445.
 * Wrap it at 60-ish characters.
 */
int str_to_ical_str(char *dest, int destsz, char *src)
{
   return str_to_iv_str(dest, destsz, src, 1);
}

/*
 * Quote for iCalendar (RFC 2445) or vCard (RFC 2426).
 * The only difference is that iCalendar also quotes semicolons.
 * Wrap at 70-ish characters.
 */
static int str_to_iv_str(char *dest, int destsz, char *src, int isical)
{
   int c, i;
   char *destend, *odest;

   if ((!src) || (!dest)) {
      return EXIT_SUCCESS;
   }

   odest = dest;
   destend = dest + destsz - 4; /* max 4 chars into dest per loop iteration */
   c=0;
   while (*src) {
      if (dest >= destend) {
         break;
      }
      if (c>ICAL_LINE_LENGTH) {
         /* Assume UTF-8 coding and stop on a valid character boundary */
         for (i=0; i<4; i++) {
            if ((*src & 0xC0) != 0x80) {
               if (*src) {
                  *dest++= CR; *dest++= LF;
                  *dest++=' ';
               }
               c=0;
               break;
            }
            *dest++=*src++;
         }

         if (c != 0) {
            jp_logf(JP_LOG_WARN,_("Invalid UTF-8 encoding in export string\n"));
            /* Force truncation of line anyways */
            *dest++= CR; *dest++= LF;
            *dest++=' ';
            c=0;
         }  
         continue;
      }
      if (*src=='\n') {
         *dest++='\\';
         *dest++='n';
         c+=2;
         src++;
         continue;
      }
      if (*src=='\\' || (isical && *src == ';') || *src == ',') {
         *dest++='\\';
         c++;
      }
      *dest++=*src++;
      c++;
   }
   *dest++='\0';

   return dest - odest;
}

/*
 * Quote a *TEXT-LIST-CHAR format string as specified by RFC 2426.
 * Wrap it at 60-ish characters.
 */
int str_to_vcard_str(char *dest, int destsz, char *src)
{
   return str_to_iv_str(dest, destsz, src, 0);
}

/*
 * This is a slow algorithm, but its not used much
 */
int sub_days_from_date(struct tm *date, int n)
{
   int ndim;
   int fdom;
   int flag;
   int reset_days;
   int i;

   get_month_info(date->tm_mon, 1, date->tm_year, &fdom, &ndim);
   for (i=0; i<n; i++) {
      flag = reset_days = 0;
      if (--(date->tm_mday) < 1) {
         date->tm_mday=28;
         reset_days = 1;
         flag = 1;
         if (--(date->tm_mon) < 0) {
            date->tm_mon=11;
            flag = 1;
            if (--(date->tm_year)<3) {
               date->tm_year = 3;
            }
         }
      }
      if (flag) {
         get_month_info(date->tm_mon, 1, date->tm_year, &fdom, &ndim);
      }
      /* this assumes that flag is always set when reset_days is set */
      if (reset_days) {
         date->tm_mday=ndim;
      }
   }
   date->tm_isdst=-1;
   mktime(date);

   return EXIT_SUCCESS;
}

/*
 * This function will decrement the date by n number of months and
 * adjust the day to the last day of the month if it exceeds the number
 * of days in the new month
 */
int sub_months_from_date(struct tm *date, int n)
{
   int i;
   int days_in_month[]={31,28,31,30,31,30,31,31,30,31,30,31};

   for (i=0; i<n; i++) {
      if (--(date->tm_mon) < 0) {
         date->tm_mon=11;
         if (--(date->tm_year)<3) {
            date->tm_year = 3;
         }
      }
   }

   if ((date->tm_year%4 == 0) &&
       !(((date->tm_year+1900)%100==0) && ((date->tm_year+1900)%400!=0))) {
      days_in_month[1]++;
   }

   if (date->tm_mday > days_in_month[date->tm_mon]) {
      date->tm_mday = days_in_month[date->tm_mon];
   }

   date->tm_isdst=-1;
   mktime(date);

   return EXIT_SUCCESS;
}

int sub_years_from_date(struct tm *date, int n)
{
   return add_or_sub_years_to_date(date, -n);
}

gint timeout_sync_up(gpointer data)
{
   time_t ltime;
   struct tm *now;
   int secs, diff_secs;
   int timeout_interval = get_timeout_interval();

   if (timeout_interval == CLOCK_TICK) {
      glob_date_timer_tag = gtk_timeout_add(timeout_interval, timeout_date, NULL);
   } else {
      /* Interval is in minutes.  Sync up with current time */
      time(&ltime);
      now = localtime(&ltime);
      secs = now->tm_sec;
      if (secs < 2) {
         glob_date_timer_tag = gtk_timeout_add(timeout_interval, timeout_date, NULL);
      } else {
         diff_secs = (secs < 61) ? 60-secs : 59;   // Account for leap seconds
         glob_date_timer_tag = gtk_timeout_add(diff_secs*CLOCK_TICK, timeout_sync_up, NULL);
      }
   } 
   
   timeout_date(NULL);  // Update label

   return FALSE;        // Destroy this timeout
}

gint timeout_date(gpointer data)
{
   char str[102];
   char datef[102];
   const char *svalue1, *svalue2;
   time_t ltime;
   struct tm *now;

   if (glob_date_label==NULL) {
      return FALSE;
   }

   time(&ltime);
   now = localtime(&ltime);

   /* Build a long date string */
   get_pref(PREF_LONGDATE, NULL, &svalue1);
   get_pref(PREF_TIME, NULL, &svalue2);
   if ((svalue1==NULL)||(svalue2==NULL)) {
      strcpy(datef, _("Today is %A, %x %X"));
   } else {
      sprintf(datef, _("Today is %%A, %s %s"), svalue1, svalue2);
   }
   jp_strftime(str, 100, datef, now);
   str[100]='\0';

   gtk_label_set_text(GTK_LABEL(glob_date_label), str);


   return TRUE;
}

/*
 * This undeletes a record from the appropriate Datafile
 */
int undelete_pc_record(AppType app_type, void *VP, int flag)
{
   PC3RecordHeader header;
   MyAppointment *mappt;
   MyCalendarEvent *mcale;
   MyAddress *maddr;
   MyContact *mcont;
   MyToDo *mtodo;
   MyMemo *mmemo;
   unsigned int unique_id;
   char filename[FILENAME_MAX];
   char filename2[FILENAME_MAX];
   FILE *pc_file  = NULL;
   FILE *pc_file2 = NULL;
   char *record;
   int found;
   int ret = -1;
   int num;
#ifdef ENABLE_MANANA
   long ivalue;
#endif
   char dbname[][32]={
   "DatebookDB.pc3",
        "AddressDB.pc3",
        "ToDoDB.pc3",
        "MemoDB.pc3",
        ""
   };

   if (VP==NULL) {
      return EXIT_FAILURE;
   }

   /* Convert to new database names if prefs set */
   rename_dbnames(dbname);
   
   /* to keep the compiler happy with -Wall*/
   mappt = NULL;
   mcale = NULL;
   maddr = NULL;
   mcont = NULL;
   mmemo = NULL;
   switch (app_type) {
    case DATEBOOK:
      mappt = (MyAppointment *) VP;
      unique_id = mappt->unique_id;
      strcpy(filename, dbname[0]);
      break;
    case CALENDAR:
      mcale = (MyCalendarEvent *) VP;
      unique_id = mcale->unique_id;
      strcpy(filename, dbname[0]);
      break;
    case ADDRESS:
      maddr = (MyAddress *) VP;
      unique_id = maddr->unique_id;
      strcpy(filename, dbname[1]);
      break;
    case CONTACTS:
      mcont = (MyContact *) VP;
      unique_id = mcont->unique_id;
      strcpy(filename, dbname[1]);
      break;
    case TODO:
      mtodo = (MyToDo *) VP;
      unique_id = mtodo->unique_id;
#ifdef ENABLE_MANANA
      get_pref(PREF_MANANA_MODE, &ivalue, NULL);
      if (ivalue) {
         strcpy(filename, "MananaDB.pc3");
      } else {
         strcpy(filename, dbname[2]);
      }
#else
      strcpy(filename, dbname[2]);
#endif
      break;
    case MEMO:
      mmemo = (MyMemo *) VP;
      unique_id = mmemo->unique_id;
      strcpy(filename, dbname[3]);
      break;
    default:
      return EXIT_SUCCESS;
   }

   found  = FALSE;
   record = NULL;

   g_snprintf(filename2, sizeof(filename2), "%s.pct", filename);

   pc_file = jp_open_home_file(filename , "r");
   if (!pc_file) {
      return EXIT_FAILURE;
   }

   pc_file2=jp_open_home_file(filename2, "w");
   if (!pc_file2) {
      jp_close_home_file(pc_file);
      return EXIT_FAILURE;
   }

   while(!feof(pc_file)) {
      read_header(pc_file, &header);
      if (feof(pc_file)) {
         break;
      }
      /* Skip copying DELETED_PALM_REC entry which undeletes it */
      if (header.unique_id == unique_id &&
          header.rt == DELETED_PALM_REC) {
         found = TRUE;
         if (fseek(pc_file, header.rec_len, SEEK_CUR)) {
            jp_logf(JP_LOG_WARN, "fseek failed\n");
            ret = -1;
            break;
         }
         continue;
      }
      /* Change header on DELETED_PC_REC to undelete this type */
      if ((header.unique_id == unique_id) &&
          (header.rt == DELETED_PC_REC)) {
         found = TRUE;
         header.rt = NEW_PC_REC;
      }

      /* Otherwise, keep whatever is there by copying it to the new pc3 file */
      record = malloc(header.rec_len);
      if (!record) {
         jp_logf(JP_LOG_WARN, "cleanup_pc_file(): Out of memory\n");
         ret = -1;
         break;
      }
      num = fread(record, header.rec_len, 1, pc_file);
      if (num != 1) {
         if (ferror(pc_file)) {
            ret = -1;
            break;
         }
      }
      ret = write_header(pc_file2, &header);
      ret = fwrite(record, header.rec_len, 1, pc_file2);
      if (ret != 1) {
         ret = -1;
         break;
      }
      free(record);
      record = NULL;
   }

   if (record) {
      free(record);
   }
   if (pc_file) {
      jp_close_home_file(pc_file);
   }
   if (pc_file2) {
      jp_close_home_file(pc_file2);
   }

   if (found) {
      rename_file(filename2, filename);
   } else {
      unlink_file(filename2);
   }

   return ret;
}

int unlink_file(char *filename)
{
   char fullname[FILENAME_MAX];

   get_home_file_name(filename, fullname, sizeof(fullname));

   return unlink(fullname);
}

int unpack_db_header(DBHeader *dbh, unsigned char *buffer)
{
   unsigned long temp;

   g_strlcpy(dbh->db_name, (char *)buffer, 32);
   dbh->flags = bytes_to_bin(buffer + 32, 2);
   dbh->version = bytes_to_bin(buffer + 34, 2);
   temp = bytes_to_bin(buffer + 36, 4);
   dbh->creation_time = pilot_time_to_unix_time(temp);
   temp = bytes_to_bin(buffer + 40, 4);
   dbh->modification_time = pilot_time_to_unix_time(temp);
   temp = bytes_to_bin(buffer + 44, 4);
   dbh->backup_time = pilot_time_to_unix_time(temp);
   dbh->modification_number = bytes_to_bin(buffer + 48, 4);
   dbh->app_info_offset = bytes_to_bin(buffer + 52, 4);
   dbh->sort_info_offset = bytes_to_bin(buffer + 56, 4);
   g_strlcpy(dbh->type, (char *)(buffer + 60), 5);
   g_strlcpy(dbh->creator_id, (char *)(buffer + 64), 5);
   g_strlcpy(dbh->unique_id_seed, (char *)(buffer + 68), 5);
   dbh->next_record_list_id = bytes_to_bin(buffer + 72, 4);
   dbh->number_of_records = bytes_to_bin(buffer + 76, 2);

   return EXIT_SUCCESS;
}

/* Validate CSV header before import
 * Current test merely checks for the correct number of fields in the header
 * but does not check name, type, etc.  More tests could also be added
 * to compare the jpilot version that produced the file with the jpilot
 * version that is importing the file. */ 
int verify_csv_header(const char *header, int num_fields, const char *file_name) 
{
   int i, comma_cnt;   

   for (i=0, comma_cnt=0; i<strlen(header); i++) {
      if (header[i] == ',') comma_cnt++; 
   }
   if (comma_cnt != num_fields-1) {
      jp_logf(JP_LOG_WARN, _("Incorrect header format for CSV import\n"
                             "Check line 1 of file %s\n"
                             "Aborting import\n"), file_name);
      return EXIT_FAILURE;
   } 

   return EXIT_SUCCESS;
}

static int write_to_next_id(unsigned int unique_id)
{
   FILE *pc_out;
   int ret;

   pc_out = jp_open_home_file(EPN".next_id", "r+");
   if (pc_out==NULL) {
      jp_logf(JP_LOG_WARN, _("Unable to open file: %s%s\n"), EPN, ".next_id");
      return EXIT_FAILURE;
   }

   ret = write_to_next_id_open(pc_out, unique_id);

   jp_close_home_file(pc_out);

   return ret;
}

static int write_to_next_id_open(FILE *pc_out, unsigned int unique_id)
{
   char id_str[50];

   if (fseek(pc_out, 0, SEEK_SET)) {
      jp_logf(JP_LOG_WARN, "fseek failed\n");
      return EXIT_FAILURE;
   }

   if (fwrite(FILE_VERSION2_CR, strlen(FILE_VERSION2_CR), 1, pc_out) != 1) {
      jp_logf(JP_LOG_WARN, _("Error writing version header to file: %s%s\n"), EPN, ".next_id");
      return EXIT_FAILURE;
   }
   sprintf(id_str, "%d\n", unique_id);
   if (fwrite(id_str, strlen(id_str), 1, pc_out) != 1) {
      jp_logf(JP_LOG_WARN, _("Error writing next id to file: %s%s"), EPN, ".next_id\n");
      return EXIT_FAILURE;
   }

   return EXIT_SUCCESS;
}

