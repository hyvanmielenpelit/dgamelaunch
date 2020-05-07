/* IMPORTANT defines */

#ifndef __DGAMELAUNCH_H
#define __DGAMELAUNCH_H

#include "config.h"
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

#ifdef USE_SHMEM
#include <semaphore.h>
#endif

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

#define dglsign(x) (x < 0 ? -1 : (x > 0 ? 1 : 0))

#define DGL_PLAYERNAMELEN 30 /* max. length of player name */
#define DGL_PASSWDLEN 20 /* max. length of passwords */
#define DGL_MAILMSGLEN 80 /* max. length of mail message */

#define DGL_MAXWATCHCOLS 10
#define DGL_MAXLOGGEDGAMESCOLS 12

#define DGL_BANNER_LINELEN 256 /* max. length of banner lines*/
#define DGL_XLOGFILE_LINELEN 2048

#ifdef USE_NCURSES_COLOR
# define CLR_NORMAL  COLOR_PAIR(11)   | A_NORMAL
# define CLR_RED     COLOR_PAIR(COLOR_RED)   | A_NORMAL
#else
# define CLR_NORMAL  0
# define CLR_RED     0
#endif
extern int color_remap[];

typedef enum
{
    DGLACCT_ADMIN       = 0x01,	/* admin account */
    DGLACCT_LOGIN_LOCK  = 0x02,	/* account is banned and cannot login */
    DGLACCT_PASSWD_LOCK = 0x04,	/* account password cannot be changed */
    DGLACCT_EMAIL_LOCK  = 0x08	/* account email cannot be changed */
} dgl_acct_flag;

typedef enum
{
    DGLTIME_DGLSTART = 0,	/* when someone telnets in */
    DGLTIME_LOGIN,		/* right after user login */
    DGLTIME_REGISTER,		/* right after new nick is registered */
    DGLTIME_GAMESTART,		/* right before a game is started */
    DGLTIME_GAMEEND,		/* right after a game is ended */
    NUM_DGLTIMES
} dglcmd_times;

typedef enum
{
    DGLCMD_NONE = 0,
    DGLCMD_RAWPRINT,	/* rawprint "foo" */
    DGLCMD_MKDIR,	/* mkdir foo */
    DGLCMD_CHDIR,	/* chdir foo */
    DGLCMD_IF_NX_CP,	/* ifnxcp foo bar */
    DGLCMD_CP,		/* cp foo bar */
    DGLCMD_UNLINK,	/* unlink foo */
    DGLCMD_EXEC,	/* exec foo bar */
    DGLCMD_SETENV,	/* setenv foo bar */
    DGLCMD_WATCH_MENU,  /* watch_menu */
    DGLCMD_LOGGEDGAMES_MENU,  /* logged_games_menu */
    DGLCMD_LOGIN,       /* ask_login */
    DGLCMD_REGISTER,	/* ask_register */
    DGLCMD_QUIT,	/* quit */
    DGLCMD_CHMAIL,	/* chmail */
    DGLCMD_CHPASSWD,	/* chpasswd */
    DGLCMD_PLAYGAME,	/* play_game "foo" */
    DGLCMD_PLAY_IF_EXIST,	/* play_if_exist "game" "file" */
    DGLCMD_SUBMENU,	/* submenu "foo" */
    DGLCMD_RETURN,	/* return */
    DGLCMD_RETURNMANY,	/* returnmany "2" */
    DGLCMD_GAMENAME, /* gamename "gnollhack" */
    DGLCMD_CLEARGAMENAME, /* cleargamename */
    DGLCMD_SKIPIFGAME, /* skipifgame */
    DGLCMD_RESIZEWINDOW /* resizewindow */
} dglcmd_actions;

typedef enum
{
    SORTMODE_NONE = 0,
    SORTMODE_USERNAME,
    SORTMODE_PRODUCT,
    SORTMODE_VERSION,
    SORTMODE_WINDOWSIZE,
    SORTMODE_STARTTIME,
    SORTMODE_DURATION,
    SORTMODE_IDLETIME,
    SORTMODE_EXTRA_INFO,
#ifdef USE_SHMEM
    SORTMODE_WATCHERS,
#endif
    NUM_SORTMODES
} dg_sortmode;

static const char *SORTMODE_NAME[NUM_SORTMODES] = {
    "Unsorted",
    "Username",
    "Product",
    "Version"
    "Windowsize",
    "Starttime",
    "Duration",
    "Idletime",
    "Extrainfo",
#ifdef USE_SHMEM
    "Watchers",
#endif
};

struct dg_banner_var {
    char *name;
    char *value;
    int special;
    struct dg_banner_var *next;
};

struct dg_cmdpart
{
    dglcmd_actions cmd;
    char *param1;
    char *param2;
    struct dg_cmdpart *next;
};

struct dg_user
{
#ifdef USE_SQLITE3
    long id;
#endif
  char *username;
  char *email;
  char *env;
  char *password;
  int flags;			/* dgl_acct_flag bitmask */
};

struct dg_banner
{
  char **lines;
  unsigned int len;
};

struct dg_menuoption
{
    char *keys;
    struct dg_cmdpart *cmdqueue;
    struct dg_menuoption *next;
};

struct dg_menu
{
    char *banner_fn;
    struct dg_banner banner;
    int cursor_x, cursor_y;
    struct dg_menuoption *options;
};

struct dg_menulist
{
    char *menuname;
    struct dg_menu *menu;
    struct dg_menulist *next;
};

struct dg_shm
{
#ifdef USE_SHMEM
    sem_t dg_sem;
#endif
    long max_n_games;
    long cur_n_games;
};

struct dg_shm_game
{
    long  in_use;
    long  nwatchers;
    char  ttyrec_fn[150];
};

struct dg_game
{
  char *ttyrec_fn;
  char *name;
  char *product;
  char *version;
  char *date;
  char *time;
  time_t idle_time;
  int ws_row, ws_col; /* Window size */
  int gamenum;
  int is_in_shm;
  int shm_idx;
  int nwatchers;

  char *extra_info;
  int extra_info_weight;
};

struct dg_config
{
    char* game_path;
    char* game_name;
    char* game_id;
    char* shortname;
    char* product;
    char* version;
    int recommended_columns;
    int recommended_rows;
    int minturns;
    char* rcfile;
    char* ttyrecdir;
    char* spool;
    char* inprogressdir;
    char* logdir;
    char* defaultoptionsdir;
    int num_args; /* # of bin_args */
    char **bin_args; /* args for game binary */
    char *rc_fmt;
    struct dg_cmdpart *cmdqueue;
    struct dg_cmdpart *postcmdqueue;
    int max_idle_time;
    char *extra_info_file;
    int encoding; // -1 = run --print-charset
};

struct dg_watchcols {
    int dat;
    int sortmode;
    int x;
    char *colname;
    char *fmt;
};

typedef enum
{
    LOGGEDGAME_COL_RANK = 0,
    LOGGEDGAME_COL_NAME,
    LOGGEDGAME_COL_ROLE,
    LOGGEDGAME_COL_RACE,
    LOGGEDGAME_COL_GENDER,
    LOGGEDGAME_COL_ALIGNMENT,
    LOGGEDGAME_COL_DIFFICULTY,
    LOGGEDGAME_COL_POINTS,
    LOGGEDGAME_COL_TURNS,
    LOGGEDGAME_COL_TIME,
    LOGGEDGAME_COL_DEATH,
    NUM_LOGGEDGAME_COLS
} dg_loggedgame_coltypes;

typedef enum
{
    LOGGEDGAME_FILTER_MODE_ALL = 0,
    LOGGEDGAME_FILTER_MODE_NORMAL = 1,
    LOGGEDGAME_FILTER_MODE_EXPLORE = 2, 
    LOGGEDGAME_FILTER_MODE_WIZARD = 4
} dg_loggedgame_mode_filter;

typedef enum
{
    LOGGEDGAME_FILTER_DEATH_ALL = 0,
    LOGGEDGAME_FILTER_DEATH_ASCENDED = 1
} dg_loggedgame_death_filter;

typedef enum
{
    LOGGEDGAME_SORTMODE_NONE = 0,
    LOGGEDGAME_SORTMODE_POINTS,
    LOGGEDGAME_SORTMODE_ENDTIME
} dg_loggedgame_sortmode;

typedef enum
{
    LOGGEDGAME_DISPLAY_MODE_LATESTGAMES = 0,
    LOGGEDGAME_DISPLAY_MODE_LATESTASCENSION,
    LOGGEDGAME_DISPLAY_MODE_TOPSCORES
} dg_loggedgame_display_mode;

struct dg_loggedgames_cols {
    int coltype;
    int x;
    char *colname;
    char *fmt;
};


struct dg_xlogfile_data 
{
    int rownumber;
    char *version;
    long long points;
    int deathdnum;
    int deathlev;
    int maxlvl;
    int hp;
    int maxhp;
    int deaths;
    time_t deathdate;
    time_t birthdate;
    int uid;
    char *role;
    char *race;

    char *gender;
    char *align;

    char *name;
    char *death;
    int conduct;
    long turns;
    int achieve;
    long realtime;
    time_t starttime;
    time_t endtime;

    char *gender0;
    char *align0;

    int flags;
    int difficulty;
    char *mode;
};

struct dg_globalconfig
{
    char* chroot;
    char* dglroot;
    char* banner;
    unsigned long max;
    int max_newnick_len; /* maximum length of new registered names. must be less than 20 chars. */
    char* shed_user;
    char* shed_group;
    uid_t shed_uid;
    gid_t shed_gid;
    char* passwd;
    char* lockfile;
    int allow_registration; /* allow registering new nicks */
    int sortmode; /* default watching-screen sortmode */
    struct dg_banner_var *banner_var_list;
    char *locale;
    int utf8esc; /* send select-utf8-charset escape code */
    char *defterm; /* default TERM in case user TERM is unknown  */
    int flowctrl; /* XON/XOFF for games? */

    struct dg_cmdpart *cmdqueue[NUM_DGLTIMES];

    /* NULL terminated list of dg_watchcols pointers */
    struct dg_watchcols *watch_columns[DGL_MAXWATCHCOLS + 1];
    int n_watch_columns;

    struct dg_menulist *menulist;
    int menu_max_idle_time;
};



/* Global variables */
extern int shm_n_games; /* TODO: make configurable */

extern char* config; /* file path */
extern struct dg_config **myconfig;
extern char *chosen_name;
extern int loggedin;
extern int game_chosen;
extern char* chosengamename;
extern int silent;
extern int set_max;

extern int selected_game;
extern int return_from_submenu;
extern int redrawbanner;
extern int newwindowrows;
extern int newwindowcols;
extern int curses_resize;

extern struct dg_globalconfig globalconfig;

extern int num_games;

extern mode_t default_fmode;

extern int dgl_local_COLS;
extern int dgl_local_LINES;

extern char last_ttyrec[512];

/* dgamelaunch.c */
extern void create_config(void);
extern void ttyrec_getmaster(void);
extern char *get_mainmenu_name(void);
extern char *gen_ttyrec_filename(void);
extern char *gen_inprogress_lock(int game, pid_t pid, char *ttyrec_filename);
extern void catch_sighup(int signum);
extern void loadbanner(char *fname, struct dg_banner *ban);
extern void drawbanner(struct dg_banner *ban);
extern void banner_var_add(char *name, char *value, int special);
extern char *banner_var_value(char *name);
extern void banner_var_free(void);
extern int check_retard(int reset);
extern char *dgl_format_str(int game, struct dg_user *me, char *str, char *plrname);

extern void term_resize_check();
extern void sigwinch_func(int sig);

extern struct dg_menu *dgl_find_menu(char *menuname);

extern int dgl_exec_cmdqueue(struct dg_cmdpart *queue, int game, struct dg_user *me);

extern void free_populated_games(struct dg_game **games, int len);
extern struct dg_game **populate_games(int game, int *l, struct dg_user *me);

#ifdef USE_DEBUGFILE
extern void debug_write(char *str);
#else
#define debug_write(str)
#endif

extern struct dg_game **sort_games(struct dg_game **games, int len, dg_sortmode sortmode);

int runmenuloop(struct dg_menu *menu);

extern void signals_block(void);
extern void signals_release(void);

extern void shm_sem_wait(struct dg_shm *shm_dg_data);
extern void shm_sem_post(struct dg_shm *shm_dg_data);
extern void shm_update(struct dg_shm *shm_dg_data, struct dg_game **games, int len);
extern void shm_mk_keys(key_t *shm_key, key_t *shm_sem_key);
extern void shm_init(struct dg_shm **shm_dg_data, struct dg_shm_game **shm_dg_game);

extern int dgl_getch(void);
extern void idle_alarm_set_enabled(int enabled);
extern void idle_alarm_reset(void);
extern void inprogressmenu(int gameid);
extern void latestgamesmenu(int gameid);
extern void set_xlogfile_defaults(struct dg_xlogfile_data *line);
extern char* insert_commas_ll (long long n);
extern char* insert_commas (char* src);
extern void free_dg_xlogfile_data(struct dg_xlogfile_data *line);
extern int compare_xlogfile_data_points(const void *s1, const void *s2);
extern int compare_xlogfile_data_endtime(const void *s1, const void *s2);

extern void change_email(void);
extern int changepw(int dowrite);
extern void domailuser(char *username);
extern void drawmenu(void);
extern void freefile(void);
extern void initcurses(void);
extern void loginprompt(int from_ttyplay);
extern void newuser(void);
extern void autologin(char *user, char *pass);
extern int passwordgood(char *cpw);
extern int readfile(int nolock);
extern struct dg_user *userexist(char *cname, int isnew);
extern void write_canned_rcfile(int game, char *target);
extern void writefile(int requirenew);
extern void graceful_exit(int status);
extern int purge_stale_locks(int game);
/*extern int menuloop(void);*/
extern void ttyrec_getpty(void);
#ifndef HAVE_SETENV
extern int mysetenv (const char* name, const char* value, int overwrite);
#else
# define mysetenv setenv
#endif
#ifndef HAVE_SETPROCTITLE
void compat_init_setproctitle(int argc, char *argv[]);
void setproctitle(const char *fmt, ...);
#endif

/* strlcpy.c */
extern size_t strlcpy (char *dst, const char *src, size_t siz);
extern size_t strlcat (char *dst, const char *src, size_t siz);

/* mygetnstr.c */
extern int mygetnstr(char *buf, int maxlen, int doecho);

#endif
