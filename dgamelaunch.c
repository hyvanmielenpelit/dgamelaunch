/* dgamelaunch.c
 *
 * (c)2001-4 M. Drew Streib <dtype@dtype.org>
 * also parts (c) 2003-4 Joshua Kwan <joshk@triplehelix.org>,
 * Brett Carrington <brettcar@segvio.org>,
 * Jilles Tjoelker <jilles@stack.nl>
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * See this program in action at http://alt.org/nethack/
 *
 * This is a little wrapper for nethack (and soon other programs) that
 * will allow them to be run from a telnetd session, chroot, shed privs,
 * make a simple login, then play the game.
 */

#define _GNU_SOURCE

#include "dgamelaunch.h"
#include "config.h"
#include "ttyplay.h"
#include "ttyrec.h"

/* a request from the author: please leave some remnance of
 * 'based on dgamelaunch version xxx' in any derivative works, or
 * even keep the line the same altogether. I'm probably happy 
 * to make any changes you need. */

/* ************************************************************* */

/* program stuff */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>          /* ttyrec */
#include <sys/stat.h>
#ifdef USE_RLIMIT
#include <sys/resource.h>
#endif

#ifdef USE_SHMEM
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

#include <libgen.h>
#include <stdlib.h>
#include <curses.h>
#include <locale.h>

#ifdef USE_SQLITE3
# include <sqlite3.h>
#endif

#if defined(__FreeBSD__)
# include <libutil.h>
#elif defined(__NetBSD__)
# include <util.h>
#elif defined(__APPLE__)
# include <unistd.h>
#else
# include <crypt.h>
#endif

#ifdef __linux__
# include <pty.h>
#endif

#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>


extern FILE* yyin;
extern int yyparse ();

/* global variables */

char * __progname;
int  g_idle_alarm_enabled = 0;
int  showplayers = 0;
int  initplayer = 0;
void (*g_chain_winch)(int);

#ifndef USE_SQLITE3
int f_num = 0;
struct dg_user **users = NULL;
#endif
struct dg_user *me = NULL;
struct dg_banner banner;

static struct dg_watchcols default_watchcols[] = {
    {SORTMODE_NONE, SORTMODE_NONE,        1, "", "%s)"},
    {SORTMODE_USERNAME, SORTMODE_USERNAME,    4, "Username", "%-10s"},
    {SORTMODE_PRODUCT, SORTMODE_PRODUCT,    15, "Game", "%-11s"},
    {SORTMODE_VERSION, SORTMODE_VERSION,    27, "Version", "%-9s"},
    {SORTMODE_WINDOWSIZE, SORTMODE_WINDOWSIZE, 37, "Size", "%s"},
    {SORTMODE_STARTTIME, SORTMODE_STARTTIME,  44, "Start time (UTC+0)", "%s"},
    {SORTMODE_IDLETIME, SORTMODE_IDLETIME,   65, "Idle time", "%-10s"},
#ifdef USE_SHMEM
    {SORTMODE_WATCHERS, SORTMODE_WATCHERS,   76, "W", "%s"},
#endif
};

static struct dg_loggedgames_cols default_loggedgames_cols[] = {
    {LOGGEDGAME_COL_RANK, 1, "#", "%-4s)"},
    {LOGGEDGAME_COL_NAME, 6, "Name", "%s)"},
    {LOGGEDGAME_COL_ROLE, 17, "Rol", "%s"},
    {LOGGEDGAME_COL_RACE, 21, "Rac", "%s"},
    {LOGGEDGAME_COL_GENDER, 25, "G", "%s"},
    {LOGGEDGAME_COL_ALIGNMENT, 27, "A", "%s"},
    {LOGGEDGAME_COL_DIFFICULTY, 29, "D", "%s"},
    {LOGGEDGAME_COL_POINTS, 31, "Score", "%-11s"},
    {LOGGEDGAME_COL_TURNS, 43, "Turns", "%-6s"},
    {LOGGEDGAME_COL_TIME, 50, "Time", "%s"},
    {LOGGEDGAME_COL_DEATH, 68, "Outcome", "%s"}    
};

int color_remap[16] = {
    COLOR_PAIR(9) | A_NORMAL,
    COLOR_PAIR(COLOR_BLUE) | A_NORMAL,
    COLOR_PAIR(COLOR_GREEN) | A_NORMAL,
    COLOR_PAIR(COLOR_CYAN) | A_NORMAL,
    COLOR_PAIR(COLOR_RED) | A_NORMAL,
    COLOR_PAIR(COLOR_MAGENTA) | A_NORMAL,
    COLOR_PAIR(COLOR_YELLOW) | A_NORMAL,
    COLOR_PAIR(COLOR_BLACK) | A_NORMAL,
    COLOR_PAIR(10) | A_BOLD,
    COLOR_PAIR(COLOR_BLUE) | A_BOLD,
    COLOR_PAIR(COLOR_GREEN) | A_BOLD,
    COLOR_PAIR(COLOR_CYAN) | A_BOLD,
    COLOR_PAIR(COLOR_RED) | A_BOLD,
    COLOR_PAIR(COLOR_MAGENTA) | A_BOLD,
    COLOR_PAIR(COLOR_YELLOW) | A_BOLD,
    COLOR_PAIR(COLOR_WHITE) | A_BOLD,
};

static struct dg_watchcols *default_watchcols_list[DGL_MAXWATCHCOLS + 1];
static struct dg_loggedgames_cols *default_loggedgames_cols_list[DGL_MAXLOGGEDGAMESCOLS + 1];

struct dg_user *
cpy_me(struct dg_user *me)
{
  struct dg_user *tmp = malloc(sizeof(struct dg_user));

  if (tmp && me)
  {
#ifdef USE_SQLITE3
    tmp->id = me->id;
#endif
    if (me->username)
      tmp->username = strdup(me->username);
    if (me->email)
      tmp->email = strdup(me->email);
    if (me->env)
      tmp->env = strdup(me->env);
    if (me->password)
      tmp->password = strdup(me->password);
    tmp->flags = me->flags;
    tmp->password_type = me->password_type;
  }
  return tmp;
}

#ifndef HAVE_SETENV
int
mysetenv (const char* name, const char* value, int overwrite)
{
  int retval;
  char *buf = NULL;
  
  if (getenv(name) == NULL || overwrite)
  {
    size_t len = strlen(name) + 1 + strlen(value) + 1; /* NAME=VALUE\0 */
    buf = malloc(len);
    snprintf(buf, len, "%s=%s", name, value);
    retval = putenv(buf);
  }
  else
    retval = -1;
  
  return retval;  
}
#else /* use native setenv */
# define mysetenv setenv
#endif

/* ************************************************************* */
/* for ttyrec */

void
ttyrec_getpty ()
{
#ifdef HAVE_OPENPTY
    if (openpty (&master, &slave, NULL, NULL, NULL) == -1) {
	debug_write("cannot openpty");
	graceful_exit (61);
    }
#else
    if ((master = open ("/dev/ptmx", O_RDWR)) < 0) {
	debug_write("cannot open /dev/ptmx");
	graceful_exit (62);
    }
  grantpt (master);
  unlockpt (master);
  if ((slave = open ((const char *) ptsname (master), O_RDWR)) < 0)
    {
	debug_write("cannot open master ptsname");
      graceful_exit (65);
    }
#endif
  ioctl (slave, TIOCSWINSZ, (char *) &win);
  tcsetattr(slave, TCSANOW, &tt);
}

/* ************************************************************* */

static int dgl_signal_blocked = 0;
static sigset_t dgl_signal_blockmask;
static sigset_t dgl_signal_oldmask;

void
signals_block()
{
    if (!dgl_signal_blocked) {
	sigemptyset(&dgl_signal_blockmask);
	sigaddset(&dgl_signal_blockmask, SIGHUP);
	sigaddset(&dgl_signal_blockmask, SIGINT);
	sigaddset(&dgl_signal_blockmask, SIGQUIT);
	sigaddset(&dgl_signal_blockmask, SIGTERM);
	sigprocmask(SIG_BLOCK, &dgl_signal_blockmask, &dgl_signal_oldmask);
	dgl_signal_blocked = 1;
    }
}

void
signals_release()
{
    if (dgl_signal_blocked) {
	sigprocmask(SIG_SETMASK, &dgl_signal_oldmask, NULL);
	dgl_signal_blocked = 0;
    }
}


/* ************************************************************* */

char *
get_mainmenu_name()
{
    if (loggedin) {
	if (me && (me->flags & DGLACCT_ADMIN)) return "mainmenu_admin";
	return "mainmenu_user";
    }
    return "mainmenu_anon";
}


char*
gen_ttyrec_filename ()
{
  time_t rawtime;
  struct tm *ptm;
  char *ttyrec_filename = calloc(100, sizeof(char));

  /* append time to filename */
  time (&rawtime);
  ptm = gmtime (&rawtime);
  snprintf (ttyrec_filename, 100, "%04i-%02i-%02i.%02i:%02i:%02i_%s.ttyrec",
            ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
            ptm->tm_hour, ptm->tm_min, ptm->tm_sec, chosengamename);
  return ttyrec_filename;
}

/* ************************************************************* */

char*
gen_inprogress_lock (int game, pid_t pid, char* ttyrec_filename)
{
  char *lockfile = NULL, filebuf[80];
  int fd;
  size_t len, wrlen;
  struct flock fl = { 0 };

  snprintf (filebuf, sizeof(filebuf), "%d\n%d\n%d\n",
		  pid, win.ws_row, win.ws_col);

  wrlen = strlen(filebuf);

  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;

  len = strlen(dgl_format_str(game, me, myconfig[game]->inprogressdir, NULL)) + strlen(me->username) + strlen(ttyrec_filename) + 13;
  lockfile = calloc(len, sizeof(char));

  snprintf (lockfile, len, "%s%s:%s", dgl_format_str(game, me, myconfig[game]->inprogressdir, NULL),
            me->username, ttyrec_filename);

  fd = open (lockfile, O_WRONLY | O_CREAT, 0644);
  if (fcntl (fd, F_SETLKW, &fl) == -1) {
      debug_write("cannot fnctl inprogress-lock");
    graceful_exit (68);
  }

  if (write (fd, filebuf, wrlen) != wrlen) {
      debug_write("inprogress-lock write");
      graceful_exit(70);
  }

  return lockfile;
}

/* ************************************************************* */

#ifdef USE_SHMEM
int hup_shm_idx = -1;
char *hup_shm_ttyrec_fn = NULL;
#endif

void
catch_sighup (int signum)
{
  if (child)
    {
      sleep (10);
      kill (child, SIGHUP);
      sleep (5);
    }
#ifdef USE_SHMEM
  signals_block();
  if (hup_shm_idx != -1) {
      struct dg_shm *shm_dg_data = NULL;
      struct dg_shm_game *shm_dg_game = NULL;
      shm_init(&shm_dg_data, &shm_dg_game);

      shm_sem_wait(shm_dg_data);
      if (shm_dg_game[hup_shm_idx].in_use &&
	  !strcmp(shm_dg_game[hup_shm_idx].ttyrec_fn, hup_shm_ttyrec_fn) &&
	  (shm_dg_game[hup_shm_idx].nwatchers > 0)) {
	  shm_dg_game[hup_shm_idx].nwatchers--;
      }
      shm_sem_post(shm_dg_data);
      hup_shm_idx = -1;
      free(hup_shm_ttyrec_fn);
  }
  signals_release();
#endif
  debug_write("catchup sighup");
  graceful_exit (7);
}

/* ************************************************************* */

int
dgl_getch(void)
{
    const int c = getch();
    idle_alarm_reset();
    return c;
}

/* ************************************************************* */

static void
dgl_idle_kill(int signal)
{
    kill(0, SIGHUP);
}

void
idle_alarm_set_enabled(int enabled)
{
    signal(SIGALRM, SIG_IGN);
    g_idle_alarm_enabled = enabled;
    idle_alarm_reset();
    if (enabled)
        signal(SIGALRM, dgl_idle_kill);
}

void
idle_alarm_reset(void)
{
    if (g_idle_alarm_enabled && globalconfig.menu_max_idle_time > 0)
        alarm(globalconfig.menu_max_idle_time);
}

/* ************************************************************* */


char *
bannerstrmangle(char *buf, char *bufnew, int buflen, char *fromstr, char *tostr)
{
    char *loc;
    char *b = buf;

    memset (bufnew, 0, buflen);

    if (strstr(b, fromstr)) {
	int i = 0;
	while ((loc = strstr (b, fromstr)) != NULL) {
	    for (; i < buflen; i++) {
		if (loc != b)
		    bufnew[i] = *(b++);
		else {
		    strlcat (bufnew, tostr, buflen);
		    b += strlen(fromstr);
		    i += strlen(tostr);
		    break;
                }

		if (strlen (b) == 0)
		    break;
	    }
	}

	if (*b)
	    strlcat(bufnew, b, buflen);
    } else strncpy(bufnew, buf, buflen);
    return bufnew;
}

void
banner_var_add(char *name, char *value, int special)
{
    struct dg_banner_var *tmp = (struct dg_banner_var *)malloc(sizeof(struct dg_banner_var));

    if (!tmp) return;

    tmp->name = strdup(name);
    tmp->value = strdup(value);
    tmp->special = special;
    tmp->next = globalconfig.banner_var_list;
    globalconfig.banner_var_list = tmp;
}

void
banner_var_free()
{
    struct dg_banner_var *tmp;
    struct dg_banner_var *bv = globalconfig.banner_var_list;
    while (bv) {
	tmp = bv->next;
	free(bv->name);
	free(bv->value);
	free(bv);
	bv = tmp;
    }
    globalconfig.banner_var_list = NULL;
}

char *
banner_var_resolve(struct dg_banner_var *bv)
{
  static char tmpbuf[DGL_BANNER_LINELEN+1];
  time_t tstamp;
  struct tm *ptm;
  if (!bv) return NULL;
  if (!bv->special) return bv->value;
  time(&tstamp);
  ptm = gmtime(&tstamp);
  strftime(tmpbuf, DGL_BANNER_LINELEN, bv->value, ptm);
  return tmpbuf;
}

char *
banner_var_value(char *name)
{
    struct dg_banner_var *bv = globalconfig.banner_var_list;
    while (bv) {
	if (!strcmp(bv->name, name)) return banner_var_resolve(bv);
	bv = bv->next;
    }
    return NULL;
}

void
freebanner(struct dg_banner *ban)
{
    unsigned int l;
    if (!ban) return;
    l = ban->len;

    while (l > 0) {
	l--;
	free(ban->lines[l]);
    }
    free(ban->lines);
    ban->len = 0;
    ban->lines = NULL;
}

void
banner_addline(struct dg_banner *ban, char *line)
{
    size_t len = strlen(line);
    if (!ban) return;
    ban->len++;
    ban->lines = realloc (ban->lines, sizeof (char *) * ban->len);
    if (len >= DGL_BANNER_LINELEN) {
	len = DGL_BANNER_LINELEN;
	ban->lines[ban->len - 1] = malloc(len);
	strncpy(ban->lines[ban->len - 1], line, len);
	ban->lines[ban->len - 1][len-1] = '\0';
    } else
	ban->lines[ban->len - 1] = strdup(line);
}

void
loadbanner (char *fname, struct dg_banner *ban)
{
  FILE *bannerfile;
  char buf[DGL_BANNER_LINELEN+1];
  if (ban->len > 23) return;

  memset (buf, 0, DGL_BANNER_LINELEN);

  bannerfile = fopen (fname, "r");

  if (!bannerfile)
    {
	if (ban->len == 0)
	    banner_addline(ban, "### dgamelaunch " PACKAGE_VERSION " - network console game launcher");
	snprintf(buf, DGL_BANNER_LINELEN, "### NOTE: administrator has not installed a %s file", fname);
	banner_addline(ban, buf);
	return;
    }

  while (fgets (buf, DGL_BANNER_LINELEN, bannerfile) != NULL)
    {
      char bufnew[DGL_BANNER_LINELEN+1];
      int slen;

      memset (bufnew, 0, DGL_BANNER_LINELEN);

      slen = strlen(buf);
      if ((slen > 0) && (buf[slen-1] == '\n')) buf[slen-1] = '\0';

      strncpy(bufnew, buf, DGL_BANNER_LINELEN);
      if (strstr(bufnew, "$INCLUDE(")) {
	  char *fn = bufnew + 9;
	  char *fn_end = strchr(fn, ')');
	  if (fn_end) {
	      *fn_end = '\0';
	      if (strcmp(fname, fn)) {
		  loadbanner(fn, ban);
	      }
	  }
      } else {
	  char tmpbufnew[DGL_BANNER_LINELEN+1];
	  struct dg_banner_var *bv = globalconfig.banner_var_list;
	  while (bv) {
	      strncpy(bufnew, bannerstrmangle(bufnew, tmpbufnew, DGL_BANNER_LINELEN, bv->name, banner_var_resolve(bv)), DGL_BANNER_LINELEN);
	      bv = bv->next;
	  }
	  strncpy(bufnew, bannerstrmangle(bufnew, tmpbufnew, DGL_BANNER_LINELEN, "$VERSION", PACKAGE_STRING), DGL_BANNER_LINELEN);
	  if (me && loggedin) {
	      strncpy(bufnew, bannerstrmangle(bufnew, tmpbufnew, DGL_BANNER_LINELEN, "$USERNAME", me->username), DGL_BANNER_LINELEN);
	  } else {
	      strncpy(bufnew, bannerstrmangle(bufnew, tmpbufnew, DGL_BANNER_LINELEN, "$USERNAME", "[Anonymous]"), DGL_BANNER_LINELEN);
	  }

    if (game_chosen)
    {
      for (int userchoice = 0; userchoice < num_games; userchoice++)
      {
        if (!strcmp(myconfig[userchoice]->game_id, chosengamename) || !strcmp(myconfig[userchoice]->shortname, chosengamename))
        {
            strncpy(bufnew, bannerstrmangle(bufnew, tmpbufnew, DGL_BANNER_LINELEN, "$GAMENAME", myconfig[userchoice]->game_name), DGL_BANNER_LINELEN);
        }
      }
    }

    banner_addline(ban, bufnew);
      }

      memset (buf, 0, DGL_BANNER_LINELEN);

      if (ban->len >= 24)
	  break;
  }

  fclose (bannerfile);
}

void drawbanner(struct dg_banner *ban)
{
  unsigned int i;
  char *tmpch, *tmpch2, *splch;
  int attr = 0, oattr = 0;
  int removedlines = 0;
  char msg[512];

  if (!ban)
    return;

  for (i = 0; i < ban->len; i++)
  {
    char *tmpbuf = strdup(ban->lines[i]);
    char *tmpbuf2 = tmpbuf;
    int x = 1;

    if(strncmp(tmpbuf2, "$RMLINEIFGAME(", strlen("$RMLINEIFGAME(")) == 0)
    {
      char *start = tmpbuf2 + strlen("$RMLINEIFGAME(");
      char *end = strstr(start, ")");;

      if (start && end)
      {
        int length = end - start;
        char *gamename = (char *)malloc(length + 1);
        memcpy(gamename, start, length);
        gamename[length] = '\0';

        if(strcmp(gamename, chosengamename) == 0)
        {
          //Skip line
          removedlines++;
          free(gamename);
          continue;
        }
        else 
        {
          free(gamename);
          end++;
          tmpbuf2 = end; 
        }
      }
    }
    else if (strncmp(tmpbuf2, "$WINDOW_SIZE_WIDGET", strlen("$WINDOW_SIZE_WIDGET")) == 0)
    {
      int local_cols = 0; 
      int local_rows = 0;
      if(newwindowcols > 0)
      {
        local_cols = newwindowcols;
        newwindowcols = 0;
      }
      else
      {
        local_cols = dgl_local_COLS;
      }

      if(newwindowrows > 0)
      {
        local_rows = newwindowrows;
        newwindowrows = 0;
      }
      else 
      {
        local_rows = dgl_local_LINES;
      }

      int red = COLOR_PAIR(COLOR_RED) | A_BOLD; // | A_NORMAL;
      int iswidgetred = 0;

      if(myconfig[selected_game]->recommended_columns > 0 && local_cols < myconfig[selected_game]->recommended_columns && 
        myconfig[selected_game]->recommended_rows > 0 && local_rows < myconfig[selected_game]->recommended_rows)
      {
        iswidgetred=1;
      }
      else if(myconfig[selected_game]->recommended_columns > 0 &&  local_cols < myconfig[selected_game]->recommended_columns
        && (myconfig[selected_game]->recommended_rows == 0 || local_rows >= myconfig[selected_game]->recommended_rows))
      {
        iswidgetred=1;
      }
      else if((myconfig[selected_game]->recommended_columns == 0 || local_cols >= myconfig[selected_game]->recommended_columns)
        && (myconfig[selected_game]->recommended_rows > 0 && local_rows < myconfig[selected_game]->recommended_rows))
      {
        iswidgetred=1;
      }

      if(iswidgetred)
      {
        attron(red);
      }

      char *rec_term_size = banner_var_value("$RECTERMSIZE");
      mvaddstr(1 + i - removedlines, x, rec_term_size);
      x += strlen(rec_term_size);
      sprintf(msg, "%dx%d", myconfig[selected_game]->recommended_columns, myconfig[selected_game]->recommended_rows);
      mvaddstr(1 + i - removedlines, x, msg);
      x += strlen(msg);
              
      if(iswidgetred)
      {
        attroff(red);
      }

      // removedlines--;
      // x=1;
      char *your_term_size = banner_var_value("$YOURTERMSIZE");
      mvaddstr(1 + i - removedlines, x, your_term_size);
      x += strlen(your_term_size);

      if(myconfig[selected_game]->recommended_columns > 0 && local_cols < myconfig[selected_game]->recommended_columns && 
        myconfig[selected_game]->recommended_rows > 0 && local_rows < myconfig[selected_game]->recommended_rows)
      {
        //All red
        attron(red);
        sprintf(msg, "%dx%d",local_cols, local_rows);
        mvaddstr(1 + i - removedlines, x, msg);
        attroff(red);
        x += strlen(msg);
      }
      else if(myconfig[selected_game]->recommended_columns > 0 &&  local_cols < myconfig[selected_game]->recommended_columns
        && (myconfig[selected_game]->recommended_rows == 0 || local_rows >= myconfig[selected_game]->recommended_rows))
      {
        //Only cols red
        attron(red);
        sprintf(msg, "%d", local_cols);
        mvaddstr(1 + i - removedlines, x, msg);
        attroff(red);
        x += strlen(msg);
        sprintf(msg, "x%d", local_rows);
        mvaddstr(1 + i - removedlines, x, msg);
        x += strlen(msg);
      }
      else if((myconfig[selected_game]->recommended_columns == 0 || local_cols >= myconfig[selected_game]->recommended_columns)
        && (myconfig[selected_game]->recommended_rows > 0 && local_rows < myconfig[selected_game]->recommended_rows))
      {
        //Only rows red
        sprintf(msg, "%dx", local_cols);
        mvaddstr(1 + i - removedlines, x, msg);
        x += strlen(msg);
        sprintf(msg, "%d", local_rows);
        attron(red);
        mvaddstr(1 + i - removedlines, x, msg);
        attroff(red);
        x += strlen(msg);
      }
      else
      {
        //All in white
        sprintf(msg, "%dx%d", local_cols, local_rows);
        mvaddstr(1 + i - removedlines, x, msg);
        x += strlen(msg);
      }

      char *your_term_size_end = banner_var_value("$YOURTERMSIZEEND");
      mvaddstr(1 + i - removedlines, x, your_term_size_end);
      x += strlen(your_term_size_end);
     
      continue;
      
    }

    int mode = -1;

    do
    {
      char* defgamename = strstr(tmpbuf2, "$DEFGAMENAME(");
      char* dattr = strstr(tmpbuf2, "$ATTR(");
      char* end = NULL;
      char* start = NULL;
      int length = 0;
      if(defgamename == NULL && dattr == NULL)
      {
        if(strlen(tmpbuf2) > 0)
        {
          mode = 0;
        }
        else 
        {
          //end
          mode = -1;
        }
      }
      else if(defgamename == NULL)
      {
        mode = 1; //attr
        start = dattr;
      }
      else if(dattr == NULL)
      {
        mode = 2; //defgamename
        start = defgamename;
      }
      else
      {
        //Both found
        if(dattr < defgamename)
        {
          mode = 1;
          start = dattr;
        }
        else
        {
          mode = 2;
          start = defgamename;
        }
      }

      if(mode >= 1 && start > tmpbuf2)
      {
        //Print start of string
        int start_length = start - tmpbuf2;
        char *start_part = (char *)malloc(start_length + 1);
        memcpy(start_part, tmpbuf2, start_length);
        start_part[start_length] = '\0';

        mvaddstr(1 + i - removedlines, x, start_part);
        x += start_length;

        free(start_part);
      }

      if (mode == 2)
      {        
        start += strlen("$DEFGAMENAME(");
        end = strstr(start, ")");
        if(end == NULL)
        {
          mvaddstr(1 + i - removedlines, x, tmpbuf2);
          continue;
        }

        length = end - start;
        char *gamename = (char *)malloc(length + 1);
        memcpy(gamename, start, length);
        gamename[length] = '\0';

        int userchoice;
        for (userchoice = 0; userchoice < num_games; userchoice++)
        {
          if (!strcmp(myconfig[userchoice]->game_id, gamename) || !strcmp(myconfig[userchoice]->shortname, gamename))
          {
            mvaddstr(1 + i - removedlines, x, myconfig[userchoice]->game_name);
            length = strlen(myconfig[userchoice]->game_name);
            x += length;
          }
        }

        free(gamename);

        end++;
        tmpbuf2 = end;
      }
      else if (mode == 1)
      {
        end = strstr(dattr, ")");
        if(end == NULL)
        {
          mvaddstr(1 + i - removedlines, x, tmpbuf2);
          continue;
        }
        tmpch = dattr;
        tmpch2 = end;
        int spl = 0;
        char *nxttmpch;
        oattr = attr;
        attr = A_NORMAL;
        *tmpch2 = '\0'; //This will replace ) with \0 to end line reading
        tmpch += 6;
        nxttmpch = tmpch;
        do
        {
          spl = 0;
          splch = strchr(tmpch, ';');
          if (splch && *splch)
          {
            spl = 1;
            nxttmpch = splch;
            *nxttmpch = '\0';
            nxttmpch++;
          }
          if (tmpch && *tmpch)
          {
            switch (*tmpch)
            {
            default:
              break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            {
              int num = atoi(tmpch);
              if (num >= 0 && num <= 15)
                attr |= color_remap[num];
            }
            break;
            case 'b':
              attr |= A_BOLD;
              break;
            case 's':
              attr |= A_STANDOUT;
              break;
            case 'u':
              attr |= A_UNDERLINE;
              break;
            case 'r':
              attr |= A_REVERSE;
              break;
            case 'd':
              attr |= A_DIM;
              break;
            }
          }
          else
            attr = A_NORMAL;
          tmpch = nxttmpch;
        } while (spl);

        if (oattr)
          attroff(oattr);
        if (attr)
          attron(attr);

        tmpch2++;
        tmpbuf2 = tmpch2;
      }
      else if (mode == 0)
      {
        mvaddstr(1 + i - removedlines, x, tmpbuf2);
        length = strlen(tmpbuf2);
        tmpbuf2 += length;
        x += length;
      }
    } while (mode >= 0);

    free(tmpbuf);
  }
}

void
shm_sem_wait(struct dg_shm *shm_dg_data)
{
#ifdef USE_SHMEM
    if (sem_wait(&(shm_dg_data->dg_sem)) == -1) {
	debug_write("sem_wait");
	graceful_exit(77);
    }
#endif
}

void
shm_sem_post(struct dg_shm *shm_dg_data)
{
#ifdef USE_SHMEM
    if (sem_post(&(shm_dg_data->dg_sem)) == -1) {
	debug_write("sem_post");
	graceful_exit(78);
    }
#endif
}

void
shm_update(struct dg_shm *shm_dg_data, struct dg_game **games, int len)
{
#ifdef USE_SHMEM
    int di, i;
    struct dg_shm_game *shm_dg_game = (struct dg_shm_game *)(shm_dg_data + sizeof(struct dg_shm));

    signals_block();
    shm_sem_wait(shm_dg_data);

    for (di = 0; di < shm_dg_data->max_n_games; di++)
	if (shm_dg_game[di].in_use) {
	    int delgame = 1;
	    for (i = 0; i < len; i++) {
		if (!strcmp(games[i]->ttyrec_fn, shm_dg_game[di].ttyrec_fn)) {
		    delgame = 0;
		    games[i]->is_in_shm = 1;
		    games[i]->shm_idx = di;
		    games[i]->nwatchers = shm_dg_game[di].nwatchers;
		    break;
		}
	    }
	    if (delgame) {
		shm_dg_game[di].in_use = 0;
		if (shm_dg_data->cur_n_games > 0) shm_dg_data->cur_n_games--;
	    }
	}

    if (shm_dg_data->cur_n_games < shm_dg_data->max_n_games) {
	for (i = 0; i < len; i++)
	    if (!games[i]->is_in_shm) {
		for (di = 0; di < shm_dg_data->max_n_games; di++)
		    if (!shm_dg_game[di].in_use) {
			shm_dg_game[di].in_use = 1;
			shm_dg_game[di].nwatchers = 0;
			games[i]->nwatchers = 0;
			games[i]->is_in_shm = 1;
			games[i]->shm_idx = di;
			shm_dg_data->cur_n_games++;
			strncpy(shm_dg_game[di].ttyrec_fn, games[i]->ttyrec_fn, 150);
			break;
		    }
	    }
    }

    shm_sem_post(shm_dg_data);
    signals_release();
#endif
}

void
shm_mk_keys(key_t *shm_key, key_t *shm_sem_key)
{
#ifdef USE_SHMEM
    if ((*shm_key = ftok(globalconfig.passwd, 'R')) == -1) {
	debug_write("ftok shm_key");
	graceful_exit(71);
    }
    if ((*shm_sem_key = ftok(globalconfig.passwd, 'S')) == -1) {
	debug_write("ftok shm_sem_key");
	graceful_exit(72);
    }
#endif
}

#ifdef USE_SHMEM
int
shm_free()
{
    key_t shm, sem;
    int   shm_id;
    int shm_size = sizeof(struct dg_shm) + shm_n_games * sizeof(struct dg_shm_game);
    shm_mk_keys(&shm, &sem);
    if ((shm_id = shmget(shm, shm_size, 0644)) != -1) {
	shmctl(shm_id, IPC_RMID, NULL);
	return 0;
    }
    return 1;
}
#endif

void
shm_init(struct dg_shm **shm_dg_data, struct dg_shm_game **shm_dg_game)
{
#ifdef USE_SHMEM
  key_t shm_key;
  key_t shm_sem_key;
  int   shm_id;
  int   shm_size;
  void *shm_data = NULL;
  int   shm_data_existed = 0;

  shm_mk_keys(&shm_key, &shm_sem_key);

  /* max. shm_n_games simultaneous games recorded in the shared memory */
  shm_size = sizeof(struct dg_shm) + shm_n_games * sizeof(struct dg_shm_game);

  /* connect to (and possibly create) the segment */
  if ((shm_id = shmget(shm_key, shm_size, 0644 | IPC_CREAT | IPC_EXCL)) == -1) {
      /* creation failed, so it already exists. attach to it */
      shm_data_existed = 1;
      if ((shm_id = shmget(shm_key, shm_size, 0644)) == -1) {
	  debug_write("shmget");
	  graceful_exit(73);
      }
  }

  /* attach to the segment to get a pointer to it: */
  shm_data = shmat(shm_id, (void *)0, 0);
  if (shm_data == (char *)(-1)) {
      debug_write("shmat");
      graceful_exit(74);
  }
  if (!shm_data) {
      debug_write("shm_data == null");
      graceful_exit(75);
  }

  (*shm_dg_data) = (struct dg_shm *)shm_data;
  (*shm_dg_game) = (struct dg_shm_game *)((*shm_dg_data) + sizeof(struct dg_shm));

  if (!shm_data_existed && shm_data) {
      memset(*shm_dg_game, 0, shm_n_games*sizeof(struct dg_shm_game));
      (*shm_dg_data)->max_n_games = shm_n_games;
      (*shm_dg_data)->cur_n_games = 0;
      if (sem_init(&((*shm_dg_data)->dg_sem), 1,1) == -1) {
	  debug_write("sem_init");
	  graceful_exit(76);
      }
  }
#endif /* USE_SHMEM */
}

#ifdef USE_SHMEM
void
shm_dump()
{
    struct dg_shm *shm_dg_data = NULL;
    struct dg_shm_game *shm_dg_game = NULL;
    int di, unused = -1;
    shm_init(&shm_dg_data, &shm_dg_game);
    shm_sem_wait(shm_dg_data);
    for (di = 0; di < shm_dg_data->max_n_games; di++) {
	if (shm_dg_game[di].in_use) {
	    if (unused != -1) {
		if (unused != di-1)
		    fprintf(stderr, "%i-%i:\tunused\n", unused, di-1);
		else
		    fprintf(stderr, "%i:\tunused\n", unused);
		unused = -1;
	    }
	    fprintf(stderr, "%i:\t\"%s\"\twatchers:%li\n", di, shm_dg_game[di].ttyrec_fn, shm_dg_game[di].nwatchers);
	} else {
	    if (unused == -1) unused = di;
	}
    }
    if (unused != -1) {
	if (unused != di-1)
	    fprintf(stderr, "%i-%i:\tunused\n", unused, di-1);
	else
	    fprintf(stderr, "%i:\tunused\n", unused);
	unused = -1;
    }
    shm_sem_post(shm_dg_data);
    shmdt(shm_dg_data);
}
#endif

static
struct dg_watchcols **
globalconfig_watch_columns()
{
    if (globalconfig.n_watch_columns)
        return globalconfig.watch_columns;

    if (!*default_watchcols_list) {
        int i;
        for (i = 0; i < ARRAY_SIZE(default_watchcols); ++i)
            default_watchcols_list[i] = &default_watchcols[i];
    }
    return default_watchcols_list;
}

static
struct dg_loggedgames_cols **
globalconfig_loggedgames_columns()
{
    // if (globalconfig.n_watch_columns)
    //     return globalconfig.watch_columns;

    if (!*default_loggedgames_cols_list) {
        int i;
        for (i = 0; i < ARRAY_SIZE(default_loggedgames_cols); ++i)
            default_loggedgames_cols_list[i] = &default_loggedgames_cols[i];
    }
    return default_loggedgames_cols_list;
}

static
int
watchcol_find_index(struct dg_watchcols **watchcols,
                    int sortmode)
{
    int i;
    for (i = 0; watchcols[i]; ++i)
        if (watchcols[i]->sortmode == sortmode)
            return i;
    return -1;
}

static
void
sortmode_increment(struct dg_watchcols **watchcols,
                   dg_sortmode *sortmode,
                   int direction)
{
    int watch_column_index = watchcol_find_index(watchcols, *sortmode);
    int n_watchcols;
    int wrap_count = 0;
    const dg_sortmode old_sortmode = *sortmode;

    for (n_watchcols = 0; watchcols[n_watchcols]; ++n_watchcols)
        ;

    if (watch_column_index == -1 || !n_watchcols)
        return;

    do {
        watch_column_index += direction;

        if (watch_column_index < 0) {
            watch_column_index = n_watchcols - 1;
            ++wrap_count;
        } else if (watch_column_index >= n_watchcols) {
            watch_column_index = 0;
            ++wrap_count;
        }

        *sortmode = watchcols[watch_column_index]->sortmode;
    } while (wrap_count < 2 && !*sortmode);

    if (!*sortmode)
        *sortmode = old_sortmode;
}

char *
get_timediff(time_t ctime, long seconds)
{
    static char data[32];
    long secs, mins, hours;

    secs = (ctime - seconds);

    if (showplayers) {
	snprintf(data, 10, "%ld", secs);
	return data;
    }

    hours = (secs / 3600);
    secs -= (hours * 3600);
    mins = (secs / 60) % 60;
    secs -= (mins*60);
    if (hours)
	snprintf(data, 10, "%ldh %ldm", hours, mins);
    else if (mins)
	snprintf(data, 10, "%ldm %lds", mins, secs);
    else if (secs > 4)
	snprintf(data, 30, "%lds", secs);
    else
	snprintf(data, 10, " ");
    return data;
}

static void
game_get_column_data(struct dg_game *game,
                     char selectorchar,
                     time_t ctime, struct dg_shm_game *shm_dg_game,
                     char *data, int bufsz, int *hilite,
                     dg_sortmode which_data)
{
  *data = 0;

  switch (which_data)
  {
  default:
    break;
  case SORTMODE_NONE:
    data[0] = selectorchar;
    data[1] = '\0';
    break;

  case SORTMODE_USERNAME:
    snprintf(data, bufsz, "%s", game->name);
    break;

  case SORTMODE_PRODUCT:
    snprintf(data, bufsz, "%s",
             myconfig[game->gamenum]->product);
    break;

  case SORTMODE_VERSION:
    snprintf(data, bufsz, "%s",
             myconfig[game->gamenum]->version);
    break;

  case SORTMODE_WINDOWSIZE:
    snprintf(data, bufsz, "%dx%d", game->ws_col, game->ws_row);
    // if (showplayers)
    //   snprintf(data, bufsz, "%dx%d", game->ws_col, game->ws_row);
    // else
    //   snprintf(data, bufsz, "%3dx%3d", game->ws_col, game->ws_row);
    if ((game->ws_col > COLS || game->ws_row > LINES))
      *hilite = CLR_RED;
    break;

  case SORTMODE_STARTTIME:
    snprintf(data, bufsz, "%s %s", game->date,
             game->time);
    break;

  case SORTMODE_DURATION:
  {
    /* TODO: populate_games() should put st_ctime into game struct */
    struct tm timetm;
    char tmptimebuf[32];
    snprintf(tmptimebuf, 30, "%s %s", game->date, game->time);
    tmptimebuf[31] = '\0';
    strptime(tmptimebuf, "%Y-%m-%d %H:%M:%S", &timetm);
    snprintf(data, 10, "%s", get_timediff(ctime, mktime(&timetm)));
  }
  break;

  case SORTMODE_IDLETIME:
    snprintf(data, 10, "%s", get_timediff(ctime, game->idle_time));
    break;

  case SORTMODE_EXTRA_INFO:
    if (game->extra_info)
      strlcpy(data, game->extra_info, bufsz);
    break;

#ifdef USE_SHMEM
  case SORTMODE_WATCHERS:
    snprintf(data, bufsz, "%li",
             (game->is_in_shm ? shm_dg_game[game->shm_idx].nwatchers : -1));
    break;
#endif
  }
  data[bufsz - 1] = '\0';
}

void inprogressmenu(int gameid)
{
  const char *selectorchars = "abcdefghijklmnoprstuvwxyzABCDEFGHIJKLMNOPRSTUVWXYZ";
  int i, menuchoice, len = 20, offset = 0;
  static dg_sortmode sortmode = NUM_SORTMODES;
  struct dg_game **games = NULL;
  char ttyrecname[130], gametype[10], idletime[10];
  sigset_t oldmask, toblock;
  int idx = -1;
  int shm_idx = -1;
  int max_height = -1;
  int selected = -1;

  int resizex = -1;
  int resizey = -1;

  char *selectedgame = NULL;

  int abs_max_height;
  int top_banner_hei = 5;
  int btm_banner_hei = 3;
  int btm;

  int title_attr = A_STANDOUT;
  int selected_attr = A_BOLD;

  int require_enter = 0; /* TODO: make configurable */

  time_t ctime;

  struct dg_shm *shm_dg_data = NULL;
  struct dg_shm_game *shm_dg_game = NULL;

  struct dg_watchcols **watchcols = globalconfig_watch_columns();
  struct dg_watchcols **curr_watchcol;

  if (sortmode == NUM_SORTMODES)
    sortmode = globalconfig.sortmode;

  abs_max_height = strlen(selectorchars);

  shm_init(&shm_dg_data, &shm_dg_game);

  games = populate_games(gameid, &len, me);
  shm_update(shm_dg_data, games, len);
  games = sort_games(games, len, sortmode);

  while (1)
  {
    term_resize_check();
    max_height = dgl_local_LINES - (top_banner_hei + btm_banner_hei) - 1;
    if (max_height < 2)
    {
      free_populated_games(games, len);
      return;
    }
    if (max_height > abs_max_height)
      max_height = abs_max_height;

    if (len == 0)
      offset = 0;

    erase();
    drawbanner(&banner);

    if (len > 0)
    {
      while (offset >= len)
      {
        offset -= max_height;
      }
      if (offset < 0)
        offset = 0;
      mvaddstr(3, 1, "The following games are in progress:");

      for (curr_watchcol = watchcols; *curr_watchcol; ++curr_watchcol)
      {
        struct dg_watchcols *wcol = *curr_watchcol;
        char *col = wcol->colname;
        int x = wcol->x;
        while (*col == ' ')
        {
          x++;
          col++;
        }
        if (sortmode == wcol->sortmode)
          attron(title_attr);
        mvprintw(top_banner_hei, x, col);
        if (sortmode == wcol->sortmode)
          attroff(title_attr);
      }
    }

    signals_block();
    shm_sem_wait(shm_dg_data);

    (void)time(&ctime);

    for (i = 0; i < max_height; i++)
    {
      if (i + offset >= len)
        break;

      if (i + offset == selected)
        attron(selected_attr);

      for (curr_watchcol = watchcols; *curr_watchcol; ++curr_watchcol)
      {
        struct dg_watchcols *col = *curr_watchcol;
        char tmpbuf[80];
        int hilite = 0;
        game_get_column_data(games[i + offset],
                             selectorchars[i],
                             ctime, shm_dg_game,
                             tmpbuf, sizeof tmpbuf, &hilite,
                             (dg_sortmode)col->dat);
        if (hilite)
        {
          attron(hilite);
        }

        mvprintw(top_banner_hei + 1 + i, col->x, col->fmt, tmpbuf);
        
        if (hilite)
        {
          attron(CLR_NORMAL);
          hilite = 0;
        }
      }

      if (i + offset == selected)
        attroff(selected_attr);
    }

    shm_sem_post(shm_dg_data);
    signals_release();

    btm = dgl_local_LINES - btm_banner_hei - top_banner_hei;
    if (len <= max_height)
      btm = i + 1;

    if (len > 0)
    {
      mvprintw((btm + top_banner_hei), 1, "(%d-%d of %d)", offset + 1, offset + i, len);
      mvaddstr((btm + 2 + top_banner_hei), 1, "Watch which game? ('?' for help) => ");
    }
    else
    {
      mvprintw(top_banner_hei, 4, "Sorry, no games available for viewing.");
      mvaddstr((btm + 2 + top_banner_hei), 1, "Press 'q' to return, or '?' for help => ");
    }

    refresh();

    switch ((menuchoice = dgl_getch()))
    {
    case KEY_DOWN:
      selected++;
      if (selected >= len)
        selected = 0;
      while (selected < offset)
        offset -= max_height;
      while (selected >= offset + max_height)
        offset += max_height;
      break;
    case KEY_UP:
      if (selected != -1)
      {
        if (selected == 0)
          selected = len;
        selected--;
      }
      else
        selected = len - 1;
      while (selected < offset)
        offset -= max_height;
      while (selected >= offset + max_height)
        offset += max_height;
      break;
    case '*':
      if (len > 0)
      {
        int cnt = 20;
        (void)time(&ctime);
        do
        {
          idx = random() % len;
        } while ((--cnt > 0) ||
                 !((games[idx]->ws_col <= COLS) &&
                   (games[idx]->ws_row <= LINES) &&
                   ((ctime - games[idx]->idle_time) < 15)));
        selected = idx;
        goto watchgame;
      }
      break;
    case '?':
      (void)runmenuloop(dgl_find_menu("watchmenu_help"));
      break;
    case '/':
    {
      int match = -1;
      int firstmatch = -1;
      int nmatches = 0;
      char findname[DGL_PLAYERNAMELEN + 1];
      if (len <= 0)
        break;
      findname[0] = '\0';
      mvprintw((btm + 2 + top_banner_hei), 1, "Watch which player? =>                 "); /* stupid... */
      mvaddstr((btm + 2 + top_banner_hei), 1, "Watch which player? => ");
      if ((mygetnstr(findname, DGL_PLAYERNAMELEN, 1) == OK) && (strlen(findname) > 1))
      {
        int mlen = strlen(findname);
        for (i = 0; i < len; i++)
          if (!strncasecmp(games[i]->name, findname, mlen))
          {
            if (firstmatch == -1)
              firstmatch = i;
            match = i;
            nmatches++;
          }
        if (nmatches > 1)
          match = firstmatch;
        if (match > -1)
        {
          idx = match;
          selected = idx;
          goto watchgame;
        }
      }
    }
    break;
    case KEY_NPAGE:
    case '>':
      if ((offset + max_height) < len)
        offset += max_height;
      break;
    case KEY_PPAGE:
    case '<':
      if ((offset - max_height) < 0)
        offset = 0;
      else
        offset -= max_height;
      break;

    case ERR:
    case 'q':
    case 'Q':
    case '\x1b':
      goto leavewatchgame;
    case KEY_RIGHT:
    case '.':
      sortmode_increment(watchcols, &sortmode, 1);
      break;
    case KEY_LEFT:
    case ',':
      sortmode_increment(watchcols, &sortmode, -1);
      break;

    case 12:
    case 18: /* ^L, ^R */
      if (globalconfig.utf8esc)
        (void)write(1, "\033%G", 3);
      clear();
      break;

    case 13:
    case 10:
    case KEY_ENTER:
      if (selected >= 0 && selected < len)
      {
        idx = selected;
        goto watchgame;
      }
      break;

    default:
      if (strchr(selectorchars, menuchoice) && (len > 0))
      {
        int sidx = strchr(selectorchars, menuchoice) - selectorchars;

        if ((sidx > max_height) || (sidx >= len))
        {
          selected = -1;
          break;
        }

        idx = sidx + offset;
        if (require_enter)
        {
          if (selected == idx)
            selected = -1;
          else
            selected = idx;
          break;
        }
        else
          selected = idx;
      watchgame:
        if (!(idx >= 0 && idx < len && games[idx] && games[idx]->name))
          goto leavewatchgame;
        /* valid choice has been made */
        chosen_name = strdup(games[idx]->name);
        snprintf(ttyrecname, 130, "%s",
                 games[idx]->ttyrec_fn);

        clear();
        refresh();
        endwin();
        if (globalconfig.utf8esc)
          (void)write(1, "\033%G", 3);
#ifdef USE_SHMEM
        signals_block();
        if (games[idx]->is_in_shm)
        {
          shm_idx = games[idx]->shm_idx;
          shm_sem_wait(shm_dg_data);
          if (shm_dg_game[shm_idx].in_use &&
              !strcmp(shm_dg_game[shm_idx].ttyrec_fn, games[idx]->ttyrec_fn))
          {
            shm_dg_game[shm_idx].nwatchers++;
            games[idx]->nwatchers++;
          }
          hup_shm_idx = shm_idx;
          hup_shm_ttyrec_fn = strdup(games[idx]->ttyrec_fn);
          shm_sem_post(shm_dg_data);
        }
        signals_release();
#endif
        resizey = games[idx]->ws_row;
        resizex = games[idx]->ws_col;
        if (loggedin)
          setproctitle("%s [watching %s]", me->username, chosen_name);
        else
          setproctitle("<Anonymous> [watching %s]", chosen_name);
        ttyplay_main(ttyrecname, 1, resizex, resizey);
        if (loggedin)
          setproctitle("%s", me->username);
        else
          setproctitle("<Anonymous>");
#ifdef USE_SHMEM
        signals_block();
        if (games[idx]->is_in_shm)
        {
          hup_shm_idx = -1;
          free(hup_shm_ttyrec_fn);
          shm_sem_wait(shm_dg_data);
          if (shm_dg_game[shm_idx].in_use &&
              !strcmp(shm_dg_game[shm_idx].ttyrec_fn, games[idx]->ttyrec_fn) &&
              (shm_dg_game[shm_idx].nwatchers > 0))
          {
            shm_dg_game[shm_idx].nwatchers--;
            games[idx]->nwatchers--;
          }
          shm_sem_post(shm_dg_data);
        }
        signals_release();
#endif
        initcurses();
        redrawwin(stdscr);
      }
    }

    if (selected >= 0 && selected < len)
    {
      selectedgame = strdup(games[selected]->name);
    }
    games = populate_games(gameid, &len, NULL); /* FIXME: should be 'me' instead of 'NULL' */
    shm_update(shm_dg_data, games, len);
    games = sort_games(games, len, sortmode);
    if (selectedgame)
    {
      selected = -1;
      for (i = 0; i < len; i++)
        if (!strcmp(games[i]->name, selectedgame))
        {
          selected = i;
          break;
        }
      free(selectedgame);
      selectedgame = NULL;
    }
  }
leavewatchgame:
  free_populated_games(games, len);
#ifdef USE_SHMEM
  shmdt(shm_dg_data);
#endif
}

void
inprogressdisplay (int gameid)
{
    const char *selectorchars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPRSTUVWXYZ";
  int i, len = 20;
  static dg_sortmode sortmode = NUM_SORTMODES;
  struct dg_game **games = NULL;
  int shm_idx = -1;

  time_t ctime;

  struct dg_shm *shm_dg_data = NULL;
  struct dg_shm_game *shm_dg_game = NULL;

  struct dg_watchcols **watchcols = globalconfig_watch_columns();
  struct dg_watchcols **curr_watchcol;

  if (sortmode == NUM_SORTMODES)
      sortmode = globalconfig.sortmode;

  shm_init(&shm_dg_data, &shm_dg_game);

  games = populate_games (gameid, &len, NULL); /* FIXME: should be 'me' instead of 'NULL' */
  shm_update(shm_dg_data, games, len);
  games = sort_games (games, len, sortmode);

  signals_block();
  shm_sem_wait(shm_dg_data);

  (void) time(&ctime);

  for (i = 0; i < 100; i++) {
    if (i >= len)
      break;

    for (curr_watchcol = watchcols; *curr_watchcol; ++curr_watchcol) {
      struct dg_watchcols *col = *curr_watchcol;
      if ((dg_sortmode)col->dat == SORTMODE_NONE)
        continue;
      char tmpbuf[80];
      int hilite = 0;
      game_get_column_data(games[i],
        selectorchars[i],
        ctime, shm_dg_game,
        tmpbuf, sizeof tmpbuf, &hilite, (dg_sortmode)col->dat);
      fprintf(stdout, "%s#", tmpbuf); /* format in col->fmt */
    }
    fprintf(stdout, "\n");
  }

  shm_sem_post(shm_dg_data);
  signals_release();

  free_populated_games(games, len);

#ifdef USE_SHMEM
  shmdt(shm_dg_data);
#endif
}

void set_xlogfile_defaults(struct dg_xlogfile_data *line)
{
  line->achieve = 0;
  line->align0 = NULL;
  line->align = NULL;
  line->birthdate = 0;
  line->conduct = 0;
  line->death = NULL;
  line->deathdate = 0;
  line->deathdnum = 0;
  line->deathlev = 0;
  line->deaths = 0;
  line->difficulty = 0;
  line->endtime = 0;
  line->flags = 0;
  line->gender0 = NULL;
  line->gender = NULL;
  line->hp = 0;
  line->maxhp = 0;
  line->maxlvl = 0;
  line->mode = NULL;
  line->name = NULL;
  line->points = 0;
  line->race = NULL;
  line->realtime = 0;
  line->role = NULL;
  line->rownumber = 0;
  line->starttime = 0;
  line->turns = 0;
  line->uid = 0;
  line->version = NULL;
}

/* ************************************************************* */

/*
 * Show Latest Games
 */

void latestgamesmenu(int gameid)
{
  char filename[512];
  memset(filename, 0, 512);

  sprintf(filename, "%s%s", myconfig[gameid]->logdir, "xlogfile");

  int maxlinenum = 50;
  int linenum = 0;
  struct dg_xlogfile_data **lines = malloc(sizeof(size_t) * maxlinenum);

  //Check if xlogfile exists
  if(access(filename, R_OK) != -1)
  {
    FILE* filestream = fopen(filename, "r");
    
    if(filestream == NULL)
    {
      char message[600];
      memset(filename, 0, 512);
      sprintf(message, "Cannot read from file: %s", filename);
      debug_write(message);
      return;
    }
   
    int linelength = 0;
    int finishafterthiscolumn = 0;

    char message[256];
    char *buf = malloc(DGL_XLOGFILE_LINELEN * sizeof(char));
    char *buf2 = buf;
    char *foundchar, *equalchar, *value;
    memset(buf, 0, DGL_XLOGFILE_LINELEN);

    while(fgets(buf, DGL_XLOGFILE_LINELEN, filestream) != NULL)
    {
      buf2 = buf;

      if(linenum == maxlinenum)
      {
        //Autoincrease lines size
        int newmaxlinenum = 4 * maxlinenum;
        struct dg_xlogfile_data **lines2 = malloc(sizeof(size_t) * newmaxlinenum);
        memcpy(lines2, lines, sizeof(size_t) * maxlinenum);
        free(lines);
        lines = lines2;
        maxlinenum = newmaxlinenum;
      }

      finishafterthiscolumn = 0;
    
      struct dg_xlogfile_data *line = malloc(sizeof(struct dg_xlogfile_data));

      set_xlogfile_defaults(line);
      
      lines[linenum] = line;
      linelength = strlen(buf2);
      line->rownumber = linenum + 1;

      //Add a line feed, if it is missing from the end of the line
      if(buf2[linelength - 1] != '\n')
      {
        buf2[linelength] = '\n';
        buf2[linelength + 1] = '\0';
        linelength++;
      }

      while(finishafterthiscolumn == 0)
      {
        foundchar = strstr(buf2, "\t");
        if(foundchar == NULL)
        {
            //last column
            //remove /n
            buf2[strlen(buf2) - 1] = '\0';
            finishafterthiscolumn = 1;
        }
        else
        {
            *foundchar = '\0';
        }
        
        equalchar = strstr(buf2, "=");

        if(equalchar == NULL)
        {
          sprintf(message, "Equal sign not found in a column in a xlogfile: %s", buf2);
          debug_write(message);
          continue;
        }

        *equalchar = '\0';
        value = equalchar + 1;
        
        if(strcmp(buf2, "version") == 0)
        {
          line->version = strdup(value);
        }
        else if (strcmp(buf2, "points") == 0)
        {
          line->points = atoll(value);
        }
        else if (strcmp(buf2, "deathdnum") == 0)
        {
          line->deathdnum = atoi(value);
        }
        else if (strcmp(buf2, "deathlev") == 0)
        {
          line->deathlev = atoi(value);
        }
        else if (strcmp(buf2, "maxlvl") == 0)
        {
          line->maxlvl = atoi(value);
        }
        else if (strcmp(buf2, "hp") == 0)
        {
          line->hp = atoi(value);
        }
        else if (strcmp(buf2, "maxhp") == 0)
        {
          line->maxhp = atoi(value);
        }
        else if (strcmp(buf2, "deaths") == 0)
        {
          line->deaths = atoi(value);
        }
        else if (strcmp(buf2, "deathdate") == 0)
        {
          line->deathdate = (time_t)atoll(value);
        }
        else if (strcmp(buf2, "birthdate") == 0)
        {
          line->birthdate = (time_t)atoll(value);
        }
        else if (strcmp(buf2, "uid") == 0)
        {
          line->uid = atoi(value);
        }
        else if (strcmp(buf2, "role") == 0)
        {
          line->role = strdup(value);
        }
        else if (strcmp(buf2, "race") == 0)
        {
          line->race = strdup(value);
        }
        else if (strcmp(buf2, "gender") == 0)
        {
          line->gender = strdup(value);
        }
        else if (strcmp(buf2, "align") == 0)
        {
          line->align = strdup(value);
        }
        else if (strcmp(buf2, "name") == 0)
        {
          line->name = strdup(value);
        }
        else if (strcmp(buf2, "death") == 0)
        {
          line->death = strdup(value);
        }
        else if (strcmp(buf2, "conduct") == 0)
        {
          line->conduct = atoi(value);
        }
        else if (strcmp(buf2, "turns") == 0)
        {
          line->turns = atol(value);
        }
        else if (strcmp(buf2, "achieve") == 0)
        {
          line->achieve = atoi(value);
        }
        else if (strcmp(buf2, "realtime") == 0)
        {
          line->realtime = atol(value);
        }
        else if (strcmp(buf2, "starttime") == 0)
        {
          line->starttime = (time_t) atoll(value);
        }
        else if (strcmp(buf2, "endtime") == 0)
        {
          line->endtime = (time_t) atoll(value);
        }
        else if (strcmp(buf2, "gender0") == 0)
        {
          line->gender0 = strdup(value);
        }
        else if (strcmp(buf2, "align0") == 0)
        {
          line->align0 = strdup(value);
        }
        else if (strcmp(buf2, "flags") == 0)
        {
          line->flags = atoi(value);
        }
        else if (strcmp(buf2, "difficulty") == 0)
        {
          line->difficulty = atoi(value);
        }
        else if (strcmp(buf2, "mode") == 0)
        {
          line->mode = strdup(value);
        }
        else
        {
          //Unknown column, skip
        }

        if(foundchar != NULL)
        {
          buf2 = foundchar + 1;
        }
      }    

      linenum++;
    }

    fclose(filestream);
    free(buf);

  }

  int filter_mode = LOGGEDGAME_FILTER_MODE_NORMAL;
  int filter_death = LOGGEDGAME_FILTER_DEATH_ALL;
  int selected_filter_mode = LOGGEDGAME_FILTER_MODE_NORMAL;
  int selected_filter_death = LOGGEDGAME_FILTER_DEATH_ALL;
  int sort_mode = LOGGEDGAME_SORTMODE_POINTS;
  int selected_sort_mode = LOGGEDGAME_SORTMODE_POINTS;
  int display_mode = LOGGEDGAME_DISPLAY_MODE_TOPSCORES;
  int sortdone = 0;
  int filterlinesdone = 0, filterlinenum = 0;
  int maxfilterlinenum = 50;
  struct dg_xlogfile_data **filterlines = NULL;

  struct dg_xlogfile_data **sortlines = NULL;
  void *bigline = NULL;

  int page = 0;
  int pagesize = 10;
  int pagenum = 1;
  int resetpage = 1;
  int firstitem = 0;
  int lastitem = 0;
  int lastpage = 0;
  int update_term_size = 0;
  int old_maxshownlines = 0;

  while(1)
  {
    term_resize_check();
    erase();
    drawbanner(&banner);

    //Game Name
    
    int y_header = 1 + 2;
    int y_row = 0;

    if (game_chosen)
    {
      for (int userchoice = 0; userchoice < num_games; userchoice++)
      {
        if (strcmp(myconfig[userchoice]->shortname, chosengamename) == 0)
        {
            mvaddstr(y_header, 1, myconfig[userchoice]->game_name);
        }
      }
    }

    //Display Mode Header
    y_header++;
    int x_col = 1;
    switch(display_mode)
    {
      case LOGGEDGAME_DISPLAY_MODE_LATESTGAMES:
        if(myconfig[gameid]->minturns > 0)
        {
          char message[256];
          sprintf(message, "Latest Games - Games Over %d Turns", myconfig[gameid]->minturns);
          mvaddstr(y_header, x_col, message);
        }
        else
        {
          mvaddstr(y_header, x_col, "Latest Games");
        }        
        break;
      case LOGGEDGAME_DISPLAY_MODE_LATESTASCENSION:
        mvaddstr(y_header, x_col, "Latest Ascensions");
        break;
      case LOGGEDGAME_DISPLAY_MODE_TOPSCORES:
        if(myconfig[gameid]->minturns > 0)
        {
          char message[256];
          sprintf(message, "Top Scores - Games Over %d Turns", myconfig[gameid]->minturns);
          mvaddstr(y_header, x_col, message);
        }
        else
        {
          mvaddstr(y_header, x_col, "Top Scores");
        }
        break;
      default:
        mvaddstr(y_header, x_col, "Error");
    }

    if(linenum > 0)
    {
      if(resetpage)
      {
        page = 0;

        if (!filterlinesdone || filterlines == NULL || selected_filter_mode != filter_mode || selected_filter_death != filter_death || selected_sort_mode != sort_mode)
        {
          if (bigline != NULL)
          {
            free(bigline);
            bigline = NULL;
          }
          int filterlines_are_also_sortlines = (sortlines == filterlines);
          if (sortlines != NULL)
          {
            free(sortlines);
            sortlines = NULL;
          }
          if(filterlines_are_also_sortlines)
          {
            filterlines = NULL;
          }
          else
          {
            if (filterlines != NULL)
            {
              free(filterlines);
              filterlines = NULL;
            }          
          }
          
          filterlinesdone = 0;
          filter_mode = selected_filter_mode;
          filter_death = selected_filter_death;
        }

        if (!filterlinesdone)
        {
          filterlines = malloc(sizeof(size_t) * maxfilterlinenum);
          filterlinenum = 0;

          //Filtered lines
          for (int i = 0; i < linenum; i++)
          {
            int right_mode_filter = 0;
            int right_death_filter = 0;
            if (filter_mode == LOGGEDGAME_FILTER_MODE_NORMAL && strcmp(lines[i]->mode, "normal") == 0)
            {
              right_mode_filter = 1;
            }
            else if (filter_mode == LOGGEDGAME_FILTER_MODE_EXPLORE && strcmp(lines[i]->mode, "explore") == 0)
            {
              right_mode_filter = 1;
            }
            else if (filter_mode == LOGGEDGAME_FILTER_MODE_WIZARD && strcmp(lines[i]->mode, "debug") == 0)
            {
              right_mode_filter = 1;
            }
            else if (filter_mode == LOGGEDGAME_FILTER_MODE_ALL)
            {
              right_mode_filter = 1;
            }

            if (filter_death == LOGGEDGAME_FILTER_DEATH_ASCENDED && strcmp(lines[i]->death, "ascended") == 0)
            {
              right_death_filter = 1;
            }
            else if (filter_death == LOGGEDGAME_FILTER_DEATH_ALL)
            {
              right_death_filter = 1;
            }

            if (right_mode_filter && right_death_filter && lines[i]->turns >= myconfig[gameid]->minturns)
            {
              if (filterlinenum == maxfilterlinenum)
              {
                //Autoincrease filterlines size
                int newmaxfilterlinenum = 4 * maxfilterlinenum;
                struct dg_xlogfile_data **filterlines2 = malloc(sizeof(size_t) * newmaxfilterlinenum);
                memcpy(filterlines2, filterlines, sizeof(size_t) * maxfilterlinenum);
                free(filterlines);
                filterlines = filterlines2;
                maxfilterlinenum = newmaxfilterlinenum;
              }

              filterlines[filterlinenum] = lines[i];

              filterlinenum++;
            }
          }

          filterlinesdone = 1;
        }

        if (selected_sort_mode != sort_mode || !sortdone)
        {
          //Do sort
          sort_mode = selected_sort_mode;
          if (sort_mode == LOGGEDGAME_SORTMODE_POINTS || sort_mode == LOGGEDGAME_SORTMODE_ENDTIME)
          {
            sortlines = calloc(filterlinenum, sizeof(size_t));
            bigline = calloc(filterlinenum, sizeof(struct dg_xlogfile_data));

            for (int i = 0; i < filterlinenum; i++)
            {
              //Set Pointer
              sortlines[i] = bigline + sizeof(struct dg_xlogfile_data) * i;
              //Copy contents
              memcpy(bigline + sizeof(struct dg_xlogfile_data) * i, filterlines[i], sizeof(struct dg_xlogfile_data));
            }

            int (*comparefunc)(void*, void*) = NULL;
            if(sort_mode == LOGGEDGAME_SORTMODE_POINTS)
            {
              comparefunc = compare_xlogfile_data_points;
            }
            else if (sort_mode == LOGGEDGAME_SORTMODE_ENDTIME)
            {
              comparefunc = compare_xlogfile_data_endtime;
            }
            else
            {
              //Error
              char message[80];
              sprintf(message, "Wrong sort mode: %d", sort_mode);
              debug_write(message);
              comparefunc = compare_xlogfile_data_points;
            }
            

            qsort(sortlines[0], filterlinenum, sizeof(struct dg_xlogfile_data), compare_xlogfile_data_points);

            filterlines = sortlines;
          }
          else if(sort_mode == LOGGEDGAME_SORTMODE_NONE)
          {
            //Do nothing
          }
          else
          {
            //Not implemented, error
            char message[80];
            sprintf(message, "Unknown sort mode: %d", sort_mode);
            debug_write(message);
          }
          
          sortdone = 1;
        }
      }
      else
      {
        //Do paging
      }

      if(filterlinenum > 0)
      {
        //Header Row
        y_header += 2;
        
        struct dg_loggedgames_cols **columns = globalconfig_loggedgames_columns();

        for(int col = 0; col < NUM_LOGGEDGAME_COLS; col++)
        {
          int x = columns[col]->x;
          mvaddstr(y_header, x, columns[col]->colname);
        }


        // Row Lines
        
        y_row = y_header + 1;
        int maxshownlines = filterlinenum;
        int maxtablelines = dgl_local_LINES - 17;
        struct dg_xlogfile_data *curline = NULL;

        if(maxshownlines > maxtablelines)
        {
          maxshownlines = maxtablelines;
        }
        if(maxshownlines > filterlinenum)
        {
          maxshownlines = filterlinenum;
        }

        if(resetpage || (update_term_size && maxshownlines != old_maxshownlines))
        {
          pagesize = maxshownlines;
          if(pagesize > 0)
          {
            pagenum = 1 + (filterlinenum - 1) / pagesize;
            firstitem = 0;
            lastitem = pagesize - 1;
            lastpage = pagenum - 1;
          }
          else
          {
            pagenum = 0;
            firstitem = -1;
            lastitem = -1;
            lastpage = 0;
          }
          update_term_size = 0;
        }
        else
        {
          //Do paging
          firstitem = page * pagesize;
          lastitem = firstitem + pagesize - 1;        
        }
        
        if(lastitem >= filterlinenum)
        {
          lastitem = filterlinenum - 1;
        }

        old_maxshownlines = maxshownlines;
        resetpage = 0;

        if(firstitem >= 0 && lastitem >= 0)
        {
          for(int i = firstitem; i <= lastitem; i++)
          {
            curline = filterlines[filterlinenum - 1 - i];
            for(int col = 0; col < NUM_LOGGEDGAME_COLS; col++)
            {
              int x = columns[col]->x;
              char* value;
              int freevalue = 0;
              int donotprintvalue = 0;
              switch(columns[col]->coltype)
              {
                case LOGGEDGAME_COL_RANK:
                value = malloc(10 * sizeof(char));
                sprintf(value, "%d", i + 1);
                freevalue = 1;
                break;

                case LOGGEDGAME_COL_NAME:
                value = curline->name;
                break;

                case LOGGEDGAME_COL_ROLE:
                value = curline->role;
                break;

                case LOGGEDGAME_COL_RACE:
                value = curline->race;
                break;

                case LOGGEDGAME_COL_GENDER:
                value = malloc(2 * sizeof(char));
                strncpy(value, curline->gender, 1);
                value[1] = '\0';
                freevalue = 1;
                break;

                case LOGGEDGAME_COL_ALIGNMENT:
                value = malloc(2 * sizeof(char));
                strncpy(value, curline->align, 1);
                value[1] = '\0';
                freevalue = 1;
                break;

                case LOGGEDGAME_COL_DIFFICULTY:
                switch(curline->difficulty)
                {
                  case -4:
                  value = "E";
                  break;

                  case -3:
                  value = "e";
                  break;

                  case -2:
                  value = "a";
                  break;

                  case -1:
                  value = "h";
                  break;

                  case 0:
                  value = "x";
                  break;
                
                  case 1:
                  value = "m";
                  break;

                  case 2:
                  value = "M";
                  break;

                  default:
                  value = "?";
                  break;
                }
                break;

                case LOGGEDGAME_COL_POINTS:
                  value = insert_commas_ll(curline->points);
                  freevalue = 1;
                break;

                case LOGGEDGAME_COL_TURNS:
                  value = malloc(8 * sizeof(char));
                  sprintf(value, "%d", curline->turns);
                  freevalue = 1;
                break;

                case LOGGEDGAME_COL_TIME:
                  value = calloc(20, sizeof(char));
                  struct tm *timeinfo = malloc(sizeof(struct tm));
                  time_t time = (time_t)curline->endtime;
                  localtime_r(&time, timeinfo);

                  strftime(value, 20 * sizeof(char), "%Y-%m-%d %H:%M", timeinfo);

                  free(timeinfo);
                  freevalue = 1;
                break;

                case LOGGEDGAME_COL_DEATH:
                  ;
                  int maxwidth = dgl_local_COLS - x;
                  if(maxwidth > 0)
                  {
                    value = calloc(maxwidth + 1, sizeof(char));
                    strncpy(value, curline->death, maxwidth);
                    value[0] = toupper(value[0]);
                    freevalue = 1;
                  }
                  else
                  {
                    donotprintvalue = 1;
                  }
                  
                break;

                default:
                  donotprintvalue = 1;
                break;
              }
              if(donotprintvalue == 0)
              {
                mvaddstr(y_row, x, value);
              }
              if(freevalue)
              {
                free(value);
              }
            }
            
            y_row++;
          }

          if(pagenum > 1)
          {
            char pager[80];
            sprintf(pager, "(%d-%d of %d)", firstitem + 1, lastitem + 1, filterlinenum);
            mvaddstr(y_row, 1, pager);
            y_row++;
          }
        }               
      }
      else
      {
        y_header += 2;
        if(display_mode == LOGGEDGAME_DISPLAY_MODE_LATESTGAMES)
        {
          mvaddstr(y_header, 1, "No games yet");
        }
        else if (display_mode == LOGGEDGAME_DISPLAY_MODE_LATESTASCENSION)
        {
          mvaddstr(y_header, 1, "No ascensions yet");
        }
        else if(display_mode == LOGGEDGAME_DISPLAY_MODE_TOPSCORES)
        {
          mvaddstr(y_header, 1, "No top scores yet");
        }
        else
        {
          mvaddstr(y_header, 1, "Error");
        }
      }
      
    }
    else 
    {
      y_row = y_header + 2;
      mvaddstr(y_row, 1, "No games yet");
      y_row++;
    }

    int y_footer = dgl_local_LINES - 8;
    mvaddstr(y_footer, 1, "t) Top scores  g) Latest games  a) Latest ascension");
    y_footer += 2;
    mvaddstr(y_footer, 1, "Browse with arrow keys  r) Resize to terminal");
    y_footer += 2;
    mvaddstr(y_footer, 1, "q) Return");
    y_footer += 2;
    mvaddstr(y_footer, 1, "=>");
    mvprintw(y_footer, 3, "");

waitforcommand:
    ;
    int menuchoice = 0;
    switch ((menuchoice = dgl_getch()))
    {
      case 'R':
      case 'r':
        //signal(SIGWINCH, sigwinch_func);
        update_term_size = 1;
        resetpage = 0;
        break;
      case 'A':
      case 'a':
        display_mode = LOGGEDGAME_DISPLAY_MODE_LATESTASCENSION;
        selected_sort_mode = LOGGEDGAME_SORTMODE_NONE;
        selected_filter_mode = LOGGEDGAME_FILTER_MODE_NORMAL;
        selected_filter_death = LOGGEDGAME_FILTER_DEATH_ASCENDED;
        resetpage=1;
        break;
      case 'G':
      case 'g':
        display_mode = LOGGEDGAME_DISPLAY_MODE_LATESTGAMES;
        selected_sort_mode = LOGGEDGAME_SORTMODE_NONE;
        selected_filter_mode = LOGGEDGAME_FILTER_MODE_NORMAL;
        selected_filter_death = LOGGEDGAME_FILTER_DEATH_ALL;
        resetpage=1;
        break;
      case 'T':
      case 't':
        display_mode = LOGGEDGAME_DISPLAY_MODE_TOPSCORES;
        selected_sort_mode = LOGGEDGAME_SORTMODE_POINTS;
        selected_filter_mode = LOGGEDGAME_FILTER_MODE_NORMAL;
        selected_filter_death = LOGGEDGAME_FILTER_DEATH_ALL;
        resetpage=1;
        break;
      case KEY_DOWN:
      case KEY_RIGHT:
      case KEY_NPAGE:
        if(page < lastpage)
        {
          page++;
          resetpage=0;
        }
        else
        {
         goto waitforcommand;
        }
        break;
      case KEY_UP:
      case KEY_LEFT:
      case KEY_PPAGE:
        if(page > 0)
        {
          page--;
          resetpage=0;
        }
        else 
        {
          goto waitforcommand;
        }
        break;
      case ERR:
      case 'q':
      case 'Q':
      case '\x1b':
        goto exitloggedgamesmenu;
      default:
        goto waitforcommand;
    }
  }

exitloggedgamesmenu:
  //Free lines
  for(int i = 0; i < linenum; i++)
  {
    struct dg_xlogfile_data *line = lines[i];
    free_dg_xlogfile_data(line);
  }

  free(lines);

  if (bigline != NULL)
  {
    free(bigline);
  }
  int filterlines_are_also_sortlines = (sortlines == filterlines);
  if (sortlines != NULL)
  {
    free(sortlines);
  }
  if (!filterlines_are_also_sortlines && filterlines != NULL)
  {
    free(filterlines);
  }
}

int compare_xlogfile_data_points(const void *s1, const void *s2)
{
  struct dg_xlogfile_data *d1 = (struct dg_xlogfile_data *)s1; 
  struct dg_xlogfile_data *d2 = (struct dg_xlogfile_data *)s2;

  // if (d1->points > d2->points)
  //   return 1;
  // else if (d2->points > d1->points)
  //   return -1;

  // return 0;

  long long points_difference = d1->points - d2->points;
  if(points_difference > INT_MAX)
  {
    return INT_MAX;
  }
  else if(points_difference < INT_MIN)
  {
    return INT_MIN;
  }
  else
  {
    return (int) points_difference;
  }
  
}

int compare_xlogfile_data_endtime(const void *s1, const void *s2)
{
  struct dg_xlogfile_data *d1 = (struct dg_xlogfile_data *)s1; 
  struct dg_xlogfile_data *d2 = (struct dg_xlogfile_data *)s2;

  double diff = difftime(d1->endtime, d2->endtime);
  
  if(diff > (double) INT_MAX)
  {
    return INT_MAX;
  }
  else if(diff < (double) INT_MIN)
  {
    return INT_MIN;
  }
  else
  {
    return (int) diff;
  }
}

void free_dg_xlogfile_data(struct dg_xlogfile_data *line)
{
  if(!line) return;
  if(line->align0) free (line->align0);
  if(line->align) free (line->align);
  if(line->death) free (line->death);
  if(line->gender0) free (line->gender0);
  if(line->gender) free (line->gender);
  if(line->mode) free (line->mode);
  if(line->name) free (line->name);
  if(line->race) free (line->race);
  if(line->role) free (line->role);
  if(line->version) free (line->version);
  free(line);
}

char* insert_commas_ll (long long n) {
    char* buf = calloc(20, sizeof(char));
    sprintf(buf, "%d", n);
    char* result = insert_commas(buf);
    free(buf);
    return result;
}

char* insert_commas (char* src) {
    int n_length = strlen(src);
    int comma_number = (n_length - 1) / 3;
    char* buf2 = calloc(n_length + comma_number + 1, sizeof(char));
    int cur_comma_count = 0;
    int char_count = 0, num_count = 0;

    for(int i = n_length - 1; i >= 0; i--)
    {
      if(num_count % 3 == 0 && num_count > 0)
      {
        buf2[i + comma_number - cur_comma_count] = ',';
        cur_comma_count++;
        char_count++;
      }
      buf2[i + comma_number - cur_comma_count] = src[i];
      char_count++;
      num_count++;
    }
    return buf2;
}

/* ************************************************************* */

/*
 * Check email address, returns 1 if valid, 0 otherwise.
 * Doesn't recognize addresses with parts in double-quotes.
 * Addresses with a colon in them are always rejected.
 */
int
check_email (char *s)
{
  char *atomchars = "!#$%&'*+-/=?^_`{|}~" "0123456789"
    "abcdefghijklmnopqrstuvwxyz" "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  int f;

  if (*s == '@')
    return 0;

  while (*s != '\0' && *s != '@')
    {
      if (strchr(atomchars, *s) == NULL)
        return 0;
      s++;
      if (*s == '.')
        s++;
    }

  if (*s == '\0')
    return 0;
  s++;

  f = 0;
  while (*s != '\0')
    {
      if (strchr(atomchars, *s) == NULL)
        return 0;
      s++;
      if (*s == '.')
        s++, f = 1;
    }

  return f;
}

void
change_email ()
{
  char buf[81];

  clear();

  if (me->flags & DGLACCT_EMAIL_LOCK) {
      drawbanner(&banner);
      mvprintw(5, 1, "Sorry, you cannot change the email.--More--");
      dgl_getch();
      return;
  }

  for (;;)
  {
      drawbanner(&banner);

    mvprintw(3, 1, "Your current email is: %s", me->email);
    mvaddstr(4, 1, "Please enter a new one (max 80 chars; blank line aborts)");
    mvaddstr(6, 1, "=> ");

    if (mygetnstr (buf, 80, 1) != OK)
	return;

    if (*buf == '\0')
      return;
    else if (!strcmp(me->email, buf))
    {
      clear();
      mvaddstr (8, 1, "That's the same one as before. Try again?");
      move(1,1);
    }
    else if (check_email (buf))
    {
      mvprintw (8, 1, "Changing email address to '%s'. Confirm (y/n): ", buf);
      if (dgl_getch() == 'y')
      {
	free(me->email);
	me->email = strdup(buf);
	writefile(0);
	return;
      }
      else
      {
	mvaddstr(9, 1, "No changes made. Press any key to continue...");
	dgl_getch();
	return;
      }
    }
    else
    {
      clear();
      mvaddstr (8, 1, "That doesn't look like an email address to me.");
      move(1,1);
    }
  }
}

int
changepw (int dowrite)
{
  char buf[DGL_PASSWDLEN+1];
  int error = 2;

  /* A precondition is that struct `me' exists because we can be not-yet-logged-in. */
  if (!me) {
      debug_write("no 'me' in changepw");
    graceful_exit (122);        /* Die. */
  }

  if (me->flags & DGLACCT_PASSWD_LOCK) {
      clear();
      drawbanner(&banner);
      mvprintw(5, 1, "Sorry, you cannot change the password.--More--");
      dgl_getch();
      return 0;
  }

  while (error)
  {
    char repeatbuf[DGL_PASSWDLEN + 1];
    clear();

    drawbanner(&banner);

    mvprintw(5, 1, "Please enter a%s password of up to %i characters.", loggedin ? " new" : "", DGL_PASSWDLEN);
    mvaddstr(6, 1, "Blank line to abort.");
    mvaddstr(8, 1, "=> ");

    if (error == 1)
    {
      mvaddstr(13, 1, "Sorry, the passwords don't match. Try again.");
      move(8, 4);
    }

    refresh();

    if (mygetnstr(buf, DGL_PASSWDLEN, 0) != OK)
      return 0;

    if (*buf == '\0')
      return 0;

    if (strchr(buf, ':') != NULL)
    {
      debug_write("cannot have ':' in passwd");
      graceful_exit(112);
    }

    mvaddstr(12, 1, "And again:");
    mvaddstr(13, 1, "=> ");

    if (mygetnstr(repeatbuf, DGL_PASSWDLEN, 0) != OK)
      return 0;

    if (!strcmp(buf, repeatbuf))
      error = 0;
    else
      error = 1;
  }

  setpw(buf, dowrite);

  return 1;
}

void setpw(char* pw, int dowrite)
{
  int i;
  char salt[] = "$6$1234567890123456";

  if (((i = open("/dev/urandom", O_RDONLY)) == -1) ||
      (read(i, salt + 3, 16) != 16) ||
      (close(i) == -1))
    graceful_exit(150);

  for (i = 3; i < 3 + 16; i++)
    salt[i] = ("abcdefghijklmnopqrstuvwxyzABCDEF"
               "GHIJKLMNOPQRSTUVWXYZ0123456789./")[salt[i] & 63];
  
  free(me->password);
  me->password = strdup (crypt (pw, salt));
  me->password_type = 1;

  if (dowrite)
    writefile (0);
}

/* ************************************************************* */

void
wall_email(char *from, char *msg)
{
    int len, i;
    struct dg_game **games = NULL;
    char spool_fn[1024+1];
    FILE *user_spool = NULL;
    struct flock fl = { 0 };

    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (!from || !msg) return;

    if (strlen(from) < 1) {
	fprintf(stderr, "Error: wall: 'from' username is too short!\n");
	debug_write("wall: 'from' username too short");
	graceful_exit(121);
    }

    if (strlen(msg) >= DGL_MAILMSGLEN) {
	fprintf(stderr, "Error: wall: message too long!\n");
	debug_write("wall: message too long");
	graceful_exit(120);
    }

    games = populate_games(-1, &len, me);

    if (len == 0) {
	fprintf(stderr, "Error: wall: no one's logged in!\n");
	debug_write("wall: no people playing");
	graceful_exit(118);
    }

    for (i = 0; i < len; i++) {
	int game = games[i]->gamenum;
	int fnamelen;
	if (strlen(myconfig[game]->spool) < 1) continue;

	snprintf (spool_fn, 1024, "%s/%s", myconfig[game]->spool, games[i]->name);

	if ((user_spool = fopen (spool_fn, "a")) == NULL) continue;

	while (fcntl(fileno(user_spool), F_SETLK, &fl) == -1) {
	    if (errno != EAGAIN) continue;
	    sleep (1);
	}
	fprintf(user_spool, "%s:%s\n", from, msg);
	fclose(user_spool);
    }
    free_populated_games(games, len);
}

void
domailuser (char *username)
{
  unsigned int len, i;
  char *spool_fn, message[DGL_MAILMSGLEN+1];
  FILE *user_spool = NULL;
  time_t now;
  int mail_empty = 1;
  int game;
  struct flock fl = { 0 };

  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;

  assert (loggedin);

  game = 0; /*TODO: find_curr_player_game(username) */

  if (strlen(myconfig[game]->spool) < 1) return;

  len = strlen(myconfig[game]->spool) + strlen (username) + 1;
  spool_fn = malloc (len + 1);
  time (&now);
  snprintf (spool_fn, len + 1, "%s/%s", myconfig[game]->spool, username);

  /* print the enter your message line */
  clear ();
  drawbanner (&banner);
  mvprintw (5, 1,
            "Enter your message here. It is to be one line only and %i characters or less.",
	    DGL_MAILMSGLEN);
  mvaddstr (7, 1, "=> ");

  if (mygetnstr (message, DGL_MAILMSGLEN, 1) != OK)
      return;

  for (i = 0; i < strlen (message); i++)
    {
      if (message[i] != ' ' && message[i] != '\n' && message[i] != '\t')
        mail_empty = 0;
    }

  if (mail_empty)
    {
      mvaddstr (9, 1, "This scroll appears to be blank.");
      mvaddstr (10, 1, "(Aborting your message.)");
      mvaddstr (12, 1, "--More--");
      dgl_getch ();
      return;
    }

  if ((user_spool = fopen (spool_fn, "a")) == NULL)
    {
      mvaddstr (9, 1,
                "You fall into the water!  You sink like a rock.");
      mvprintw (10, 1,
                "(Couldn't open %s'%c spool file.  Aborting.)",
                username, (username[strlen (username) - 1] != 's') ? 's' : 0);
      mvaddstr (12, 1, "--More--");
      dgl_getch ();
      return;
    }

  mvaddstr (9, 1, "Sending your scroll...");
  refresh ();

  /* Getting a lock on the mailspool... */
  while (fcntl (fileno (user_spool), F_SETLK, &fl) == -1)
    {
      if (errno != EAGAIN)
        {
          mvaddstr (10, 1,
                    "(Received a weird error from fcntl.  Aborting.)");
	  mvaddstr (12, 1, "--More--");
          dgl_getch ();
          return;
        }
      sleep (1);
    }

  fprintf (user_spool, "%s:%s\n", me->username, message);

  /* 
   * Don't unlock the file ourselves, this way it will be done automatically
   * after all data has been written. (Using file locking with stdio is icky.)
   */

  fclose (user_spool);

  mvaddstr (9, 1, "Scroll delivered!         ");
  move(9, 19); /* Pedantry! */
  refresh ();
  sleep (2);

  return;
}


/* ************************************************************* */

void
freefile ()
{
#ifndef USE_SQLITE3
  int i;

  /* free existing mem, clear existing entries */
  for (i = 0; i < f_num; i++)
    {
      if (users[i] != me)
      {
	free (users[i]->password);
	free (users[i]->username);
	free (users[i]->email);
	free (users[i]->env);
	free (users[i]);
      }
    }

  if (users)
    free (users);

  users = NULL;
  f_num = 0;
#endif
}

/* ************************************************************* */

void
initcurses ()
{
  printf("\033[2J");
  if (newterm(NULL, stdout, stdin) == NULL) {
      if (!globalconfig.defterm || (newterm(globalconfig.defterm, stdout, stdin) == NULL)) {
	  debug_write("cannot create newterm");
	  graceful_exit(60);
      }
      mysetenv("TERM", globalconfig.defterm, 1);
  }
  cbreak ();
  noecho ();
  nonl ();
  intrflush (stdscr, FALSE);
  keypad (stdscr, TRUE);
#ifdef USE_NCURSES_COLOR
  start_color();
  use_default_colors();

  init_pair(COLOR_BLACK, COLOR_WHITE, COLOR_BLACK);
  init_pair(COLOR_RED, COLOR_RED, COLOR_BLACK);
  init_pair(COLOR_GREEN, COLOR_GREEN, COLOR_BLACK);
  init_pair(COLOR_YELLOW, COLOR_YELLOW, COLOR_BLACK);
  init_pair(COLOR_BLUE, COLOR_BLUE, COLOR_BLACK);
  init_pair(COLOR_MAGENTA, COLOR_MAGENTA, COLOR_BLACK);
  init_pair(COLOR_CYAN, COLOR_CYAN, COLOR_BLACK);
  init_pair(COLOR_WHITE, COLOR_WHITE, COLOR_BLACK);
  init_pair(9, 0, COLOR_BLACK);
  init_pair(10, COLOR_BLACK, COLOR_BLACK);
  init_pair(11, -1, -1);

  if (globalconfig.utf8esc) (void) write(1, "\033%G", 3);
#endif
  clear();
  refresh();
}

/* ************************************************************* */

void
autologin (char* user, char *pass)
{
  struct dg_user *tmp;
  tmp = userexist(user, 0);
  if (tmp) {
      me = cpy_me(tmp);
      if ((passwordgood(pass) || initplayer == 1) && !(me->flags & DGLACCT_LOGIN_LOCK)) {
	  loggedin = 1;
	  setproctitle ("%s", me->username);
	  dgl_exec_cmdqueue(globalconfig.cmdqueue[DGLTIME_LOGIN], 0, me);
      }
  }
}

void
loginprompt (int from_ttyplay)
{
  char user_buf[DGL_PLAYERNAMELEN+1], pw_buf[DGL_PASSWDLEN+2];
  int error = 2;

  loggedin = 0;

  while (error)
    {
      clear ();

      drawbanner (&banner);

      if (from_ttyplay == 1)
	mvaddstr (4, 1, "This operation requires you to be logged in.");

      mvaddstr (5, 1,
                "Please enter your username. (blank entry aborts)");
      mvaddstr (7, 1, "=> ");

      if (error == 1)
        {
          mvaddstr (9, 1, "There was a problem with your last entry.");
          move (7, 4);
        }

      refresh ();

      if (mygetnstr (user_buf, DGL_PLAYERNAMELEN, 1) != OK)
	  return;

      if (*user_buf == '\0')
        return;

      error = 1;

      {
	  struct dg_user *tmpme;
	  if ((tmpme = userexist(user_buf, 0))) {
	      me = cpy_me(tmpme);
	      error = 0;
	  }
      }
    }

  clear ();

  drawbanner (&banner);

  mvaddstr (5, 1, "Please enter your password.");
  mvaddstr (7, 1, "=> ");

  refresh ();

  if (mygetnstr (pw_buf, DGL_PASSWDLEN, 0) != OK)
      return;

  if (passwordgood (pw_buf))
  {
	    if (me->flags & DGLACCT_LOGIN_LOCK)
      {
          clear ();
          mvprintw(5, 1, "Sorry, that account has been banned.--More--");
          dgl_getch();
          return;
      }

      loggedin = 1;
      if (from_ttyplay)
	      setproctitle("%s [watching %s]", me->username, chosen_name);
      else
	      setproctitle("%s", me->username);
      dgl_exec_cmdqueue(globalconfig.cmdqueue[DGLTIME_LOGIN], 0, me);
  }
  else 
  {
    me = NULL;
    if (from_ttyplay == 1)
    {
      mvaddstr(9, 1, "Login failed. Returning to game.");
      refresh();
      sleep(2);
    }
  } 
}

/* ************************************************************* */

void
newuser ()
{
  char buf[1024], dirname[100];
  int error = 2;
  unsigned int i;

  loggedin = 0;

#ifndef USE_SQLITE3
  if (f_num >= globalconfig.max)
  {
      clear ();

      drawbanner (&banner);

      mvaddstr (5, 1, "Sorry, too many users have registered now.");
      mvaddstr (6, 1, "You might email the server administrator.");
      mvaddstr (7, 1, "Press return to return to the menu. ");
      dgl_getch ();

      return;
  }
#endif

  if (me)
    free (me);

  me = calloc (1, sizeof (struct dg_user));

  while (error)
    {
      clear ();

      sprintf(buf, "%i character max.", globalconfig.max_newnick_len);

      drawbanner (&banner);

      mvaddstr (5, 1, "Welcome new user. Please enter a username.");
      mvaddstr (6, 1,
                "Only characters and numbers are allowed, with no spaces.");
      mvaddstr (7, 1, buf);
      mvaddstr (9, 1, "=> ");

      if (error == 1)
        {
          mvaddstr (11, 1, "There was a problem with your last entry.");
          move (9, 4);
        }

      refresh ();

      if (mygetnstr (buf, globalconfig.max_newnick_len, 1) != OK)
	  buf[0] = 0;

      if (*buf == '\0') {
	  free(me);
	  me = NULL;
	  return;
      }

      if (!userexist(buf, 1)) {
	  error = 0;
      } else
	  error = 1;

      for (i = 0; i < strlen (buf); i++)
        {
          if (!isalnum((int)buf[i]))
            error = 1;
        }

      if (strlen (buf) < 2)
        error = 1;

      if (strlen (buf) == 0)
      {
	free(me);
	me = NULL;
        return;
      }
    }

  me->username = strdup (buf);

  /* password step */

  clear ();

  if (!changepw (0))                  /* Calling changepw instead to prompt twice. */
  {
    free(me->username);
    free(me);
    me = NULL;
    return;
  }

  /* email step */

  error = 2;
  while (error != 0)
    {
      clear ();

      drawbanner (&banner);

      mvaddstr (5, 1, "Please enter your email address.");
      mvaddstr (6, 1, "This is sent _nowhere_ but will be used if you ask"
        " the sysadmin for lost");
      mvaddstr (7, 1, "password help. Please use a correct one. It only"
        " benefits you.");
      mvaddstr (8, 1, "80 character max. No ':' characters. Blank line"
        " aborts.");
      mvaddstr (10, 1, "=> ");

      if (error == 1)
        {
          mvaddstr (12, 1, "There was a problem with your last entry.");
          move (10, 4);
        }

      refresh ();
      if (mygetnstr (buf, 80, 1) != OK)
	  buf[0] = 0;

      if (check_email (buf))
        error = 0;
      else
        error = 1;
 
      if (*buf == '\0')
      {
        free (me->username);
        free (me->password);
        free (me);
        me = NULL;
        return;
      }
    }

  me->email = strdup (buf);
  me->env = calloc (1, 1);
  me->flags = 0;
  me->password_type = 1;

  loggedin = 1;

  setproctitle ("%s", me->username);

  dgl_exec_cmdqueue(globalconfig.cmdqueue[DGLTIME_REGISTER], 0, me);

  writefile (1);
}

/* ************************************************************* */

int
passwordgood (char *cpw)
{
  char *crypted;
  assert (me != NULL);

  if(me->password_type == 0)
  {
    crypted = crypt (cpw, cpw);
    if (crypted == NULL)
        return 0;
    if (!strncmp (crypted, me->password, DGL_PASSWDLEN) || !strncmp (cpw, me->password, DGL_PASSWDLEN))
    {
      //Update to new format
      setpw(cpw, 1);
      return 1;
    }
  }
  else if (me->password_type == 1)
  {
    if (!strncmp (crypt (cpw, me->password), me->password, 128))
      return 1;
  }

  return 0;
}

/* ************************************************************* */

int
readfile (int nolock)
{
#ifndef USE_SQLITE3
  FILE *fp = NULL, *fpl = NULL;
  char buf[1200];
  struct flock fl = { 0 };

  fl.l_type = F_RDLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;

  memset (buf, 0, 1024);

  /* read new stuff */

  if (!nolock)
    {
      fpl = fopen (globalconfig.lockfile, "r");
      if (!fpl) {
	  debug_write("cannot fopen lockfile");
        graceful_exit (106);
      }
      if (fcntl (fileno (fpl), F_SETLKW, &fl) == -1) {
	  debug_write("cannot fcntl lockfile");
        graceful_exit (95);
      }
    }

  fp = fopen (globalconfig.passwd, "r");
  if (!fp) {
      debug_write("cannot fopen passwd file");
    graceful_exit (106);
  }

  /* once per name in the file */
  while (fgets (buf, 1200, fp))
    {
      char *b = buf, *n = buf;

      users = realloc (users, sizeof (struct dg_user *) * (f_num + 1));
      users[f_num] = malloc (sizeof (struct dg_user));
      users[f_num]->username = (char *) calloc (DGL_PLAYERNAMELEN+2, sizeof (char));
      users[f_num]->email = (char *) calloc (82, sizeof (char));
      users[f_num]->password = (char *) calloc (DGL_PASSWDLEN+2, sizeof (char));
      users[f_num]->env = (char *) calloc (1026, sizeof (char));

      /* name field, must be valid */
      while (*b != ':')
        {
          if (!isalnum((int)*b))
            return 1;
          users[f_num]->username[(b - n)] = *b;
          b++;
          if ((b - n) >= DGL_PLAYERNAMELEN) {
	      debug_write("name field too long");
            graceful_exit (100);
	  }
        }

      /* advance to next field */
      n = b + 1;
      b = n;

      /* email field */
      while (*b != ':')
        {
          users[f_num]->email[(b - n)] = *b;
          b++;
          if ((b - n) > 80) {
	      debug_write("email field too long");
            graceful_exit (101);
	  }
        }

      /* advance to next field */
      n = b + 1;
      b = n;

      /* pw field */
      while (*b != ':')
        {
          users[f_num]->password[(b - n)] = *b;
          b++;
          if ((b - n) >= DGL_PASSWDLEN) {
	      debug_write("passwd field too long");
            graceful_exit (102);
	  }
        }

      /* advance to next field */
      n = b + 1;
      b = n;

      /* env field */
      while ((*b != '\n') && (*b != 0) && (*b != EOF))
        {
          users[f_num]->env[(b - n)] = *b;
          b++;
          if ((b - n) >= 1024) {
	      debug_write("env field too long");
            graceful_exit (103);
	  }
        }

      f_num++;
      /* prevent a buffer overrun here */
      if (f_num > globalconfig.max)
      {
	fprintf(stderr,"ERROR: number of users in database exceeds maximum. Exiting.\n");
	debug_write("too many users in database");
        graceful_exit (109);
      }
    }

  if (!nolock)
      fclose (fpl);
  fclose (fp);
#endif
  return 0;
}

/* ************************************************************* */

#ifndef USE_SQLITE3
struct dg_user *userexist_tmp_me = NULL;

struct dg_user *
userexist (char *cname, int isnew)
{
  int i;

  if (userexist_tmp_me) {
      free(userexist_tmp_me->username);
      free(userexist_tmp_me->email);
      free(userexist_tmp_me->env);
      free(userexist_tmp_me->password);
      free(userexist_tmp_me);
      userexist_tmp_me = NULL;
  }

  for (i = 0; i < f_num; i++)
    {
	if (!strncasecmp (cname, users[i]->username, (isnew ? globalconfig.max_newnick_len : DGL_PLAYERNAMELEN))) {
	    userexist_tmp_me = cpy_me(users[i]);
	    return userexist_tmp_me;
	}
    }

  return NULL;
}
#else

struct dg_user *userexist_tmp_me = NULL;

static int
userexist_callback(void *NotUsed, int argc, char **argv, char **colname)
{
    int i;
    NotUsed = NULL;

    userexist_tmp_me = malloc(sizeof(struct dg_user));

    for (i = 0; i < argc; i++) {
	if (!strcmp(colname[i], "username"))
	    userexist_tmp_me->username = strdup(argv[i]);
	else if (!strcmp(colname[i], "email"))
	    userexist_tmp_me->email = strdup(argv[i]);
	else if (!strcmp(colname[i], "env"))
	    userexist_tmp_me->env = strdup(argv[i]);
	else if (!strcmp(colname[i], "password"))
	    userexist_tmp_me->password = strdup(argv[i]);
	else if (!strcmp(colname[i], "flags"))
	    userexist_tmp_me->flags = atoi(argv[i]);
	else if (!strcmp(colname[i], "password_type"))
	    userexist_tmp_me->password_type = atoi(argv[i]);
	else if (!strcmp(colname[i], "id"))
	    userexist_tmp_me->id = atoi(argv[i]);
    }
    return 0;
}

struct dg_user *
userexist(char *cname, int isnew)
{
  sqlite3 *db;
  char *errmsg = NULL;
  int ret, retry = 10;

  char *qbuf;

  char tmpbuf[DGL_PLAYERNAMELEN + 2];

  memset(tmpbuf, 0, DGL_PLAYERNAMELEN + 2);
  strncpy(tmpbuf, cname, (isnew ? globalconfig.max_newnick_len : DGL_PLAYERNAMELEN));

  /* Check that the nick doesn't interfere with already registered nicks */
  if (isnew && (strlen(cname) >= globalconfig.max_newnick_len))
    strcat(tmpbuf, "%");

  qbuf = sqlite3_mprintf("select * from dglusers where username = '%q' collate nocase limit 1", tmpbuf);

  ret = sqlite3_open(globalconfig.passwd, &db);
  if (ret)
  {
    sqlite3_close(db);
    debug_write("sqlite3_open failed");
    graceful_exit(96);
  }

  if (userexist_tmp_me)
  {
    free(userexist_tmp_me->username);
    free(userexist_tmp_me->email);
    free(userexist_tmp_me->env);
    free(userexist_tmp_me->password);
    free(userexist_tmp_me);
    userexist_tmp_me = NULL;
  }

  sqlite3_busy_timeout(db, 10000);
  ret = sqlite3_exec(db, qbuf, userexist_callback, 0, &errmsg);

  sqlite3_free(qbuf);

  if (ret != SQLITE_OK)
  {
    sqlite3_close(db);
    debug_write("sqlite3_exec failed");
    graceful_exit(108);
  }
  sqlite3_close(db);

  return userexist_tmp_me;
}
#endif

/* ************************************************************* */

void
write_canned_rcfile (int game, char *target)
{
  FILE *canned, *newfile;
  char buf[1024], *rfn;
  size_t bytes, len;

  len = strlen(myconfig[game]->rcfile) + 2;
  rfn = malloc(len);
  snprintf (rfn, len, "/%s", myconfig[game]->rcfile);

  if (!(newfile = fopen (target, "w")))
    {
    bail:
      mvaddstr (13, 1,
                "You don't know how to write that! You write \"%s was here\" and the scroll disappears.");
      mvaddstr (14, 1,
                "(Sorry, but I couldn't open one of the config files. This is a bug.)");
      return;
    }

  if (!(canned = fopen (rfn, "r")))
    goto bail;

  free(rfn);

  while ((bytes = fread (buf, 1, 1024, canned)) > 0)
    {
      if (fwrite (buf, 1, bytes, newfile) != bytes)
        {
          if (ferror (newfile))
            {
              mvaddstr (13, 1, "Your hand slips while engraving.");
              mvaddstr (14, 1,
                        "(Encountered a problem writing the new file. This is a bug.)");
              fclose (canned);
              fclose (newfile);
              return;
            }
        }
    }

  fclose (canned);
  fclose (newfile);
  chmod (target, default_fmode);
}



/* ************************************************************* */

#ifndef USE_SQLITE3
void
writefile (int requirenew)
{
  FILE *fp, *fpl;
  int i = 0;
  int my_done = 0;
  struct flock fl = { 0 };

  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;

  signals_block();

  fpl = fopen (globalconfig.lockfile, "r+");
  if (!fpl)
    {
	signals_release();
      debug_write("writefile locking failed");
      graceful_exit (115);
    }
  if (fcntl (fileno (fpl), F_SETLK, &fl))
    {
	signals_release();
      debug_write("writefile fcntl failed");
      graceful_exit (107);
    }

  fl.l_type = F_UNLCK;

  freefile ();
  readfile (1);

  fp = fopen (globalconfig.passwd, "w");
  if (!fp)
    {
	signals_release();
      debug_write("passwd file fopen failed");
      graceful_exit (99);
    }

  for (i = 0; i < f_num; i++)
    {
      if (loggedin && !strncmp (me->username, users[i]->username, DGL_PLAYERNAMELEN))
        {
          if (requirenew)
            {
              /* this is if someone managed to register at the same time
               * as someone else. just die. */
	      fclose(fp);
	      fclose(fpl);
	      signals_release();
	      debug_write("two users registering at the same time");
              graceful_exit (111);
            }
          fprintf (fp, "%s:%s:%s:%s\n", me->username, me->email, me->password,
                   me->env);
          my_done = 1;
        }
      else
        {
          fprintf (fp, "%s:%s:%s:%s\n", users[i]->username, users[i]->email,
                   users[i]->password, users[i]->env);
        }
    }
  if (loggedin && !my_done)
    {                           /* new entry */
      if (f_num < globalconfig.max)
        fprintf (fp, "%s:%s:%s:%s\n", me->username, me->email, me->password,
                 me->env);
      else /* Oops, someone else registered the last available slot first */
	{
          fclose(fp);
	  fclose(fpl);
	  signals_release();
	  debug_write("too many users in passwd db already");
          graceful_exit (116);
	}
    }

  fclose (fp);
  fclose (fpl);

  signals_release();
}
#else
void writefile(int requirenew)
{
  sqlite3 *db;
  char *errmsg = NULL;
  int ret, retry = 10;

  char *qbuf;

  if (requirenew)
  {
    qbuf = sqlite3_mprintf("insert into dglusers (username, email, env, password, flags, password_type) values ('%q', '%q', '%q', '%q', %li, %li)", me->username, me->email, me->env, me->password, me->flags, me->password_type);
  }
  else
  {
    qbuf = sqlite3_mprintf("update dglusers set username='%q', email='%q', env='%q', password='%q', flags=%li, password_type=%li where id=%i", me->username, me->email, me->env, me->password, me->flags, me->password_type, me->id);
  }

  ret = sqlite3_open(globalconfig.passwd, &db);
  if (ret)
  {
    sqlite3_close(db);
    debug_write("writefile sqlite3_open failed");
    graceful_exit(97);
  }

  sqlite3_busy_timeout(db, 10000);
  ret = sqlite3_exec(db, qbuf, NULL, NULL, &errmsg);

  sqlite3_free(qbuf);

  if (ret != SQLITE_OK)
  {
    sqlite3_close(db);
    debug_write("writefile sqlite3_exec failed");
    graceful_exit(98);
  }
  sqlite3_close(db);
}
#endif

/* ************************************************************* */

int
purge_stale_locks (int game)
{
  DIR *pdir;
  struct dirent *dent;
  char* dir;
  size_t len;
  short firsttime = 1;

  dir = strdup(dgl_format_str(game, me, myconfig[game]->inprogressdir, NULL));

  if (!(pdir = opendir (dir))) {
      debug_write("purge_stale_locks dir open failed");
    graceful_exit (200);
  }

  free(dir);

  while ((dent = readdir (pdir)) != NULL)
    {
      FILE *ipfile;
      char *colon, *fn;
      char buf[16];
      pid_t pid;
      size_t len;
      int seconds = 0;

      if (!strcmp (dent->d_name, ".") || !strcmp (dent->d_name, ".."))
        continue;

      colon = strchr (dent->d_name, ':');
      /* should never happen */
      if (!colon) {
	  debug_write("purge_stale_locks !colon");
        graceful_exit (201);
      }
      if (colon - dent->d_name != strlen(me->username))
        continue;
      if (strncmp (dent->d_name, me->username, colon - dent->d_name))
        continue;

      len = strlen (dent->d_name) + strlen(dgl_format_str(game, me, myconfig[game]->inprogressdir, NULL)) + 1;
      fn = malloc (len);

      snprintf (fn, len, "%s%s", dgl_format_str(game, me, myconfig[game]->inprogressdir, NULL), dent->d_name);

      if (!(ipfile = fopen (fn, "r"))) {
	  debug_write("purge_stale_locks fopen inprogressdir fail");
        graceful_exit (202);
      }

      if (fgets (buf, 16, ipfile) == NULL) {
	  debug_write("purge_stale_locks fgets ipfile fail");
        graceful_exit (203);
      }

      fclose (ipfile);

      if (firsttime)
      {
	clear ();
	drawbanner (&banner);

#define HUP_WAIT 10 /* seconds before HUPPING */
	mvprintw (3, 1,
	    "There are some stale %s processes, will recover in %d  seconds.",
            myconfig[game]->game_name, HUP_WAIT);
	mvaddstr (4, 1,
	    "Press a key NOW if you don't want this to happen!");

	move (3, 51 + strlen(myconfig[game]->game_name)); /* pedantry */
	halfdelay(10);

	for (seconds = HUP_WAIT - 1; seconds >= 0; seconds--)
	{
	  if (dgl_getch() != ERR)
	  {
	    nocbreak(); /* leave half-delay */
	    cbreak();
	    return 0;
	  }
	  mvprintw (3, 50 + strlen(myconfig[game]->game_name), "%d%s", seconds, (seconds > 9) ? "" : " ");
	}

	nocbreak();
	cbreak();

	firsttime = 0;
      }

      clear ();
      refresh ();

      pid = atoi (buf);

      kill (pid, SIGHUP);

      errno = 0;

      /* Wait for it to stop running */
      seconds = 0;
      while (kill (pid, 0) == 0)
        {
          seconds++;
          sleep (1);
          if (seconds == 10)
            {
              mvprintw (3, 1,
                        "Couldn't terminate one of your stale %s processes gracefully.", myconfig[game]->game_name);
              mvaddstr (4, 1, "Force its termination? [yn] ");
              if (tolower (dgl_getch ()) == 'y')
                {
                  kill (pid, SIGTERM);
                  break;
                }
              else
                {
                  endwin ();
                  fprintf (stderr, "Sorry, no %s for you now, please "
                           "contact the admin.\n", myconfig[game]->game_name);
		  debug_write("could not terminate stale processes");
                  graceful_exit (1);
                }
            }
        }

      /* Don't remove the lock file until the process is dead. */
      unlink (fn);
      free (fn);
    }

  closedir (pdir);
  return 1;
}


int
runmenuloop(struct dg_menu *menu)
{
    struct dg_banner ban;
    struct dg_menuoption *tmpopt;
    int userchoice = 0;
    int doclear = 1;

    if (!menu) return 1;

    ban.lines = NULL;
    ban.len = 0;

    loadbanner(menu->banner_fn, &ban);
    while (1) {
      term_resize_check();
	  

	if (doclear) {
	    doclear = 0;
	    if (globalconfig.utf8esc) (void) write(1, "\033%G", 3);
	    clear();
	}
	drawbanner(&ban);
	if (menu->cursor_x >= 0 && menu->cursor_y >= 0)
	    mvprintw(menu->cursor_y, menu->cursor_x, "");
	refresh();
  
	userchoice = dgl_getch();
	if (userchoice == ERR) {
	    freebanner(&ban);
	    return 1;
	}

	tmpopt = menu->options;
	while (tmpopt) {
	    if (strchr(tmpopt->keys, userchoice)) {
		dgl_exec_cmdqueue(tmpopt->cmdqueue, selected_game, me);
		doclear = 1;
		break;
	    } else {
		tmpopt = tmpopt->next;
	    }
	}

	if (return_from_submenu > 0) {
	    freebanner(&ban);
	    return_from_submenu--;
	    return 0;
	}

	if (check_retard(0)) {
	    freebanner(&ban);
	    debug_write("retard");
	    graceful_exit(119);
	}
    }
}

int
main (int argc, char** argv)
{
  /* for chroot and program execution */
    char atrcfilename[81], *p, *auth = NULL;
  unsigned int len;
  int c, i;
  int userchoice;
  char *tmp;
  char *wall_email_str = NULL;
#ifdef USE_RLIMIT
  struct rlimit lim;
#endif

#ifndef HAVE_SETPROCTITLE
  /* save argc, argv */
  char** saved_argv;
  int saved_argc;

  saved_argc = argc;

  saved_argv = malloc(sizeof(char**) * (argc + 1));
  for (i = 0; i < argc; i++)
    saved_argv[i] = strdup(argv[i]);
  saved_argv[i] = '\0';
  
  compat_init_setproctitle(argc, argv);
  argv = saved_argv;
#endif

  p = getenv("DGLAUTH");

  /* Linux telnetd allows importing the USER envvar via telnet,
   * while FreeBSD does not.  FreeBSD, on the other hand, does allow
   * the LOGNAME envvar.  Check USER first, then LOGNAME.
   */
  if (p == NULL) {
      p = getenv("USER");
  }

  if (p == NULL) {
      p = getenv("LOGNAME");
  }

  if (p && *p != '\0')
    auth = strdup(p);
  /* else auth is still NULL */

  /* just to be sure */
  unsetenv("DGLAUTH"); unsetenv("USER"); unsetenv("LOGNAME");

  __progname = basename(strdup(argv[0]));

  while ((c = getopt(argc, argv, "csqh:pi:aeW:SD")) != -1)
  {
      /* Stop processing arguments at -c, so that user-provided
       * commands (via ssh for example) to the dgamelaunch login
       * shell are ignored.
       */
    if (c == 'c') break;
    switch (c)
    {
      case 's':
	showplayers = 1; break;

      case 'q':
	silent = 1; break;

      case 'i':
	if (optarg && *optarg != '\0') {
		if (p && *p != '\0')
			*p = '\0';

		p = strdup(optarg);
		initplayer = 1;

		if (auth && *auth != '\0')
			*auth = '\0';
	}
	break;

    case 'W':
	wall_email_str = strdup(optarg);
	break;

    case 'S': /* Free the shared memory block */
#ifdef USE_SHMEM
	if (shm_free()) {
	    if (!silent) fprintf(stderr, "nonexistent shmem block.\n");
	} else {
	    if (!silent) fprintf(stderr, "shmem block freed.\n");
	}
#else
	if (!silent) fprintf(stderr, "warning: dgamelaunch was compiled without shmem.\n");
#endif
	graceful_exit(0);
	break;

    case 'D': /* dump the shared memory block data */
#ifdef USE_SHMEM
	shm_dump();
#else
	if (!silent) fprintf(stderr, "warning: dgamelaunch was compiled without shmem.\n");
#endif
	graceful_exit(0);
	break;

      default:
	break; /*ignore */
    }
  }

  while (optind < argc)
  {
    size_t len = strlen(argv[optind]);
    memset(argv[optind++], 0, len);
  }
  setproctitle("<Anonymous>");

  srand(time(0));

  create_config();

  /* signal handlers */
  signal (SIGHUP, catch_sighup);
  signal (SIGINT, catch_sighup);
  signal (SIGQUIT, catch_sighup);
  signal (SIGTERM, catch_sighup);

  (void) tcgetattr (0, &tt);

  if (!globalconfig.flowctrl) {
      tt.c_iflag &= ~(IXON | IXOFF | IXANY); /* Disable XON/XOFF */
      (void) tcsetattr(0, TCSANOW, &tt);
  }

  if (-1 == ioctl (0, TIOCGWINSZ, (char *) &win) || win.ws_row < 4 ||
		  win.ws_col < 4) /* Rudimentary validity check */
    {
      win.ws_row = 24;
      win.ws_col = 80;
      win.ws_xpixel = win.ws_col * 8;
      win.ws_ypixel = win.ws_row * 8;
    }

  /* get master tty just before chroot (lives in /dev) */
  ttyrec_getpty ();

#ifdef USE_RLIMIT
#ifdef USE_RLIMIT_CORE
  /* enable and set core dump size */
  if (!getrlimit(RLIMIT_CORE, &lim)) {
      lim.rlim_cur = USE_RLIMIT_CORE;
      setrlimit(RLIMIT_CORE, &lim);
  }
#endif
#ifdef USE_RLIMIT_AS
  /* set maximum memory usage */
  if (!getrlimit(RLIMIT_AS, &lim)) {
      lim.rlim_cur = USE_RLIMIT_AS;
      setrlimit(RLIMIT_AS, &lim);
  }
#endif
#endif

  if (geteuid () != globalconfig.shed_uid)
    {
      /* chroot */
      if (chroot (globalconfig.chroot))
	{
	  perror ("cannot change root directory");
	  graceful_exit (2);
	}

      if (chdir ("/"))
	{
	  perror ("cannot chdir to root directory");
	  graceful_exit (3);
	}

      /* shed privs. this is done immediately after chroot. */
	  if (setgroups(1, &globalconfig.shed_gid) == -1)
	  {
		  perror("setgroups");
		  graceful_exit(4);
	  }
	  if (setgid(globalconfig.shed_gid) == -1)
	  {
		  perror("setgid");
		  graceful_exit(5);
	  }

	  if (setuid(globalconfig.shed_uid) == -1)
	  {
		  perror("setuid");
		  graceful_exit(6);
	  }

    }

  if (globalconfig.locale) {
      setlocale(LC_CTYPE, globalconfig.locale);
  }

  if (showplayers) {
    inprogressdisplay(-1);
    graceful_exit (0);
  }

  if (wall_email_str) {
      char *emailfrom = wall_email_str;
      char *emailmsg = strchr(wall_email_str, ':');
      if (!emailmsg) {
	  debug_write("wall: no mail msg");
	  graceful_exit(117);
      }
      *emailmsg = '\0';
      emailmsg++;
      if (emailmsg)
	  wall_email(emailfrom, emailmsg);
      graceful_exit(0);
  }

  banner.len = 0;
  banner.lines = NULL;
  loadbanner(globalconfig.banner, &banner);

  dgl_exec_cmdqueue(globalconfig.cmdqueue[DGLTIME_DGLSTART], 0, NULL);

  if (initplayer) {
	  char *user, *pass;

	  user = strdup(p);
	  pass = strdup(p);

	  autologin(user, pass);

	  if (loggedin) {
		dgl_exec_cmdqueue(globalconfig.cmdqueue[DGLTIME_REGISTER], 0, me);
		fprintf(stdout, "Setup of %s succeeded.\n", me->username);
		graceful_exit(0);
	  }
	  else {
		fprintf(stdout, "Setup of %s failed.\n", p);
		graceful_exit(10);
	  }
  }

  /* simple login routine, uses ncurses */
  if (readfile (0)) {
      debug_write("log in fail");
    graceful_exit (110);
  }

  if (auth)
  {
    char *user, *pass, *p;

    p = strchr(auth, ':');

    if (p)
    {
      pass = p + 1;

      if (*pass != '\0')
      {
        *p = '\0';
        user = auth;
        autologin(user, pass);
      }
    }
  }

  initcurses ();

  g_chain_winch = signal(SIGWINCH, sigwinch_func);

  term_resize_check();

  idle_alarm_set_enabled(1);

  while (1) {
      if (runmenuloop(dgl_find_menu(get_mainmenu_name())))
	  break;
  }

  idle_alarm_set_enabled(0);

  /* NOW we can safely kill this */
  freefile ();

  if (me)
    free (me);

  freebanner(&banner);
  banner_var_free();
  graceful_exit (20);

  return 1;
}
