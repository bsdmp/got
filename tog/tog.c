/*
 * Copyright (c) 2018, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <ctype.h>
#include <errno.h>
#define _XOPEN_SOURCE_EXTENDED /* for ncurses wide-character functions */
#include <curses.h>
#include <panel.h>
#include <locale.h>
#include <sha1.h>
#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <err.h>
#include <unistd.h>
#include <limits.h>
#include <wchar.h>
#include <time.h>
#include <pthread.h>
#include <libgen.h>
#include <regex.h>
#include <sched.h>

#include "got_version.h"
#include "got_error.h"
#include "got_object.h"
#include "got_reference.h"
#include "got_repository.h"
#include "got_diff.h"
#include "got_opentemp.h"
#include "got_utf8.h"
#include "got_cancel.h"
#include "got_commit_graph.h"
#include "got_blame.h"
#include "got_privsep.h"
#include "got_path.h"
#include "got_worktree.h"

#ifndef MIN
#define	MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
#endif

#ifndef MAX
#define	MAX(_a,_b) ((_a) > (_b) ? (_a) : (_b))
#endif

#define CTRL(x)		((x) & 0x1f)

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

struct tog_cmd {
	const char *name;
	const struct got_error *(*cmd_main)(int, char *[]);
	void (*cmd_usage)(void);
};

__dead static void	usage(int, int);
__dead static void	usage_log(void);
__dead static void	usage_diff(void);
__dead static void	usage_blame(void);
__dead static void	usage_tree(void);
__dead static void	usage_ref(void);

static const struct got_error*	cmd_log(int, char *[]);
static const struct got_error*	cmd_diff(int, char *[]);
static const struct got_error*	cmd_blame(int, char *[]);
static const struct got_error*	cmd_tree(int, char *[]);
static const struct got_error*	cmd_ref(int, char *[]);

static const struct tog_cmd tog_commands[] = {
	{ "log",	cmd_log,	usage_log },
	{ "diff",	cmd_diff,	usage_diff },
	{ "blame",	cmd_blame,	usage_blame },
	{ "tree",	cmd_tree,	usage_tree },
	{ "ref",	cmd_ref,	usage_ref },
};

enum tog_view_type {
	TOG_VIEW_DIFF,
	TOG_VIEW_LOG,
	TOG_VIEW_BLAME,
	TOG_VIEW_TREE,
	TOG_VIEW_REF,
};

#define TOG_EOF_STRING	"(END)"

struct commit_queue_entry {
	TAILQ_ENTRY(commit_queue_entry) entry;
	struct got_object_id *id;
	struct got_commit_object *commit;
	int idx;
};
TAILQ_HEAD(commit_queue_head, commit_queue_entry);
struct commit_queue {
	int ncommits;
	struct commit_queue_head head;
};

struct tog_color {
	STAILQ_ENTRY(tog_color) entry;
	regex_t regex;
	short colorpair;
};
STAILQ_HEAD(tog_colors, tog_color);

static struct got_reflist_head tog_refs = TAILQ_HEAD_INITIALIZER(tog_refs);
static struct got_reflist_object_id_map *tog_refs_idmap;

static const struct got_error *
tog_ref_cmp_by_name(void *arg, int *cmp, struct got_reference *re1,
    struct got_reference* re2)
{
	const char *name1 = got_ref_get_name(re1);
	const char *name2 = got_ref_get_name(re2);
	int isbackup1, isbackup2;

	/* Sort backup refs towards the bottom of the list. */
	isbackup1 = strncmp(name1, "refs/got/backup/", 16) == 0;
	isbackup2 = strncmp(name2, "refs/got/backup/", 16) == 0;
	if (!isbackup1 && isbackup2) {
		*cmp = -1;
		return NULL;
	} else if (isbackup1 && !isbackup2) {
		*cmp = 1;
		return NULL;
	}

	*cmp = got_path_cmp(name1, name2, strlen(name1), strlen(name2));
	return NULL;
}

static const struct got_error *
tog_load_refs(struct got_repository *repo, int sort_by_date)
{
	const struct got_error *err;

	err = got_ref_list(&tog_refs, repo, NULL, sort_by_date ?
	    got_ref_cmp_by_commit_timestamp_descending : tog_ref_cmp_by_name,
	    repo);
	if (err)
		return err;

	return got_reflist_object_id_map_create(&tog_refs_idmap, &tog_refs,
	    repo);
}

static void
tog_free_refs(void)
{
	if (tog_refs_idmap) {
		got_reflist_object_id_map_free(tog_refs_idmap);
		tog_refs_idmap = NULL;
	}
	got_ref_list_free(&tog_refs);
}

static const struct got_error *
add_color(struct tog_colors *colors, const char *pattern,
    int idx, short color)
{
	const struct got_error *err = NULL;
	struct tog_color *tc;
	int regerr = 0;

	if (idx < 1 || idx > COLOR_PAIRS - 1)
		return NULL;

	init_pair(idx, color, -1);

	tc = calloc(1, sizeof(*tc));
	if (tc == NULL)
		return got_error_from_errno("calloc");
	regerr = regcomp(&tc->regex, pattern,
	    REG_EXTENDED | REG_NOSUB | REG_NEWLINE);
	if (regerr) {
		static char regerr_msg[512];
		static char err_msg[512];
		regerror(regerr, &tc->regex, regerr_msg,
		    sizeof(regerr_msg));
		snprintf(err_msg, sizeof(err_msg), "regcomp: %s",
		    regerr_msg);
		err = got_error_msg(GOT_ERR_REGEX, err_msg);
		free(tc);
		return err;
	}
	tc->colorpair = idx;
	STAILQ_INSERT_HEAD(colors, tc, entry);
	return NULL;
}

static void
free_colors(struct tog_colors *colors)
{
	struct tog_color *tc;

	while (!STAILQ_EMPTY(colors)) {
		tc = STAILQ_FIRST(colors);
		STAILQ_REMOVE_HEAD(colors, entry);
		regfree(&tc->regex);
		free(tc);
	}
}

static struct tog_color *
get_color(struct tog_colors *colors, int colorpair)
{
	struct tog_color *tc = NULL;

	STAILQ_FOREACH(tc, colors, entry) {
		if (tc->colorpair == colorpair)
			return tc;
	}

	return NULL;
}

static int
default_color_value(const char *envvar)
{
	if (strcmp(envvar, "TOG_COLOR_DIFF_MINUS") == 0)
		return COLOR_MAGENTA;
	if (strcmp(envvar, "TOG_COLOR_DIFF_PLUS") == 0)
		return COLOR_CYAN;
	if (strcmp(envvar, "TOG_COLOR_DIFF_CHUNK_HEADER") == 0)
		return COLOR_YELLOW;
	if (strcmp(envvar, "TOG_COLOR_DIFF_META") == 0)
		return COLOR_GREEN;
	if (strcmp(envvar, "TOG_COLOR_TREE_SUBMODULE") == 0)
		return COLOR_MAGENTA;
	if (strcmp(envvar, "TOG_COLOR_TREE_SYMLINK") == 0)
		return COLOR_MAGENTA;
	if (strcmp(envvar, "TOG_COLOR_TREE_DIRECTORY") == 0)
		return COLOR_CYAN;
	if (strcmp(envvar, "TOG_COLOR_TREE_EXECUTABLE") == 0)
		return COLOR_GREEN;
	if (strcmp(envvar, "TOG_COLOR_COMMIT") == 0)
		return COLOR_GREEN;
	if (strcmp(envvar, "TOG_COLOR_AUTHOR") == 0)
		return COLOR_CYAN;
	if (strcmp(envvar, "TOG_COLOR_DATE") == 0)
		return COLOR_YELLOW;
	if (strcmp(envvar, "TOG_COLOR_REFS_HEADS") == 0)
		return COLOR_GREEN;
	if (strcmp(envvar, "TOG_COLOR_REFS_TAGS") == 0)
		return COLOR_MAGENTA;
	if (strcmp(envvar, "TOG_COLOR_REFS_REMOTES") == 0)
		return COLOR_YELLOW;
	if (strcmp(envvar, "TOG_COLOR_REFS_BACKUP") == 0)
		return COLOR_CYAN;

	return -1;
}

static int
get_color_value(const char *envvar)
{
	const char *val = getenv(envvar);

	if (val == NULL)
		return default_color_value(envvar);

	if (strcasecmp(val, "black") == 0)
		return COLOR_BLACK;
	if (strcasecmp(val, "red") == 0)
		return COLOR_RED;
	if (strcasecmp(val, "green") == 0)
		return COLOR_GREEN;
	if (strcasecmp(val, "yellow") == 0)
		return COLOR_YELLOW;
	if (strcasecmp(val, "blue") == 0)
		return COLOR_BLUE;
	if (strcasecmp(val, "magenta") == 0)
		return COLOR_MAGENTA;
	if (strcasecmp(val, "cyan") == 0)
		return COLOR_CYAN;
	if (strcasecmp(val, "white") == 0)
		return COLOR_WHITE;
	if (strcasecmp(val, "default") == 0)
		return -1;

	return default_color_value(envvar);
}


struct tog_diff_view_state {
	struct got_object_id *id1, *id2;
	const char *label1, *label2;
	FILE *f, *f1, *f2;
	int fd1, fd2;
	int first_displayed_line;
	int last_displayed_line;
	int eof;
	int diff_context;
	int ignore_whitespace;
	int force_text_diff;
	struct got_repository *repo;
	struct tog_colors colors;
	size_t nlines;
	off_t *line_offsets;
	int matched_line;
	int selected_line;

	/* passed from log view; may be NULL */
	struct tog_view *log_view;
};

pthread_mutex_t tog_mutex = PTHREAD_MUTEX_INITIALIZER;

struct tog_log_thread_args {
	pthread_cond_t need_commits;
	pthread_cond_t commit_loaded;
	int commits_needed;
	int load_all;
	struct got_commit_graph *graph;
	struct commit_queue *commits;
	const char *in_repo_path;
	struct got_object_id *start_id;
	struct got_repository *repo;
	int *pack_fds;
	int log_complete;
	sig_atomic_t *quit;
	struct commit_queue_entry **first_displayed_entry;
	struct commit_queue_entry **selected_entry;
	int *searching;
	int *search_next_done;
	regex_t *regex;
};

struct tog_log_view_state {
	struct commit_queue commits;
	struct commit_queue_entry *first_displayed_entry;
	struct commit_queue_entry *last_displayed_entry;
	struct commit_queue_entry *selected_entry;
	int selected;
	char *in_repo_path;
	char *head_ref_name;
	int log_branches;
	struct got_repository *repo;
	struct got_object_id *start_id;
	sig_atomic_t quit;
	pthread_t thread;
	struct tog_log_thread_args thread_args;
	struct commit_queue_entry *matched_entry;
	struct commit_queue_entry *search_entry;
	struct tog_colors colors;
};

#define TOG_COLOR_DIFF_MINUS		1
#define TOG_COLOR_DIFF_PLUS		2
#define TOG_COLOR_DIFF_CHUNK_HEADER	3
#define TOG_COLOR_DIFF_META		4
#define TOG_COLOR_TREE_SUBMODULE	5
#define TOG_COLOR_TREE_SYMLINK		6
#define TOG_COLOR_TREE_DIRECTORY	7
#define TOG_COLOR_TREE_EXECUTABLE	8
#define TOG_COLOR_COMMIT		9
#define TOG_COLOR_AUTHOR		10
#define TOG_COLOR_DATE			11
#define TOG_COLOR_REFS_HEADS		12
#define TOG_COLOR_REFS_TAGS		13
#define TOG_COLOR_REFS_REMOTES		14
#define TOG_COLOR_REFS_BACKUP		15

struct tog_blame_cb_args {
	struct tog_blame_line *lines; /* one per line */
	int nlines;

	struct tog_view *view;
	struct got_object_id *commit_id;
	int *quit;
};

struct tog_blame_thread_args {
	const char *path;
	struct got_repository *repo;
	struct tog_blame_cb_args *cb_args;
	int *complete;
	got_cancel_cb cancel_cb;
	void *cancel_arg;
};

struct tog_blame {
	FILE *f;
	off_t filesize;
	struct tog_blame_line *lines;
	int nlines;
	off_t *line_offsets;
	pthread_t thread;
	struct tog_blame_thread_args thread_args;
	struct tog_blame_cb_args cb_args;
	const char *path;
	int *pack_fds;
};

struct tog_blame_view_state {
	int first_displayed_line;
	int last_displayed_line;
	int selected_line;
	int blame_complete;
	int eof;
	int done;
	struct got_object_id_queue blamed_commits;
	struct got_object_qid *blamed_commit;
	char *path;
	struct got_repository *repo;
	struct got_object_id *commit_id;
	struct tog_blame blame;
	int matched_line;
	struct tog_colors colors;
};

struct tog_parent_tree {
	TAILQ_ENTRY(tog_parent_tree) entry;
	struct got_tree_object *tree;
	struct got_tree_entry *first_displayed_entry;
	struct got_tree_entry *selected_entry;
	int selected;
};

TAILQ_HEAD(tog_parent_trees, tog_parent_tree);

struct tog_tree_view_state {
	char *tree_label;
	struct got_object_id *commit_id;/* commit which this tree belongs to */
	struct got_tree_object *root;	/* the commit's root tree entry */
	struct got_tree_object *tree;	/* currently displayed (sub-)tree */
	struct got_tree_entry *first_displayed_entry;
	struct got_tree_entry *last_displayed_entry;
	struct got_tree_entry *selected_entry;
	int ndisplayed, selected, show_ids;
	struct tog_parent_trees parents; /* parent trees of current sub-tree */
	char *head_ref_name;
	struct got_repository *repo;
	struct got_tree_entry *matched_entry;
	struct tog_colors colors;
};

struct tog_reflist_entry {
	TAILQ_ENTRY(tog_reflist_entry) entry;
	struct got_reference *ref;
	int idx;
};

TAILQ_HEAD(tog_reflist_head, tog_reflist_entry);

struct tog_ref_view_state {
	struct tog_reflist_head refs;
	struct tog_reflist_entry *first_displayed_entry;
	struct tog_reflist_entry *last_displayed_entry;
	struct tog_reflist_entry *selected_entry;
	int nrefs, ndisplayed, selected, show_date, show_ids, sort_by_date;
	struct got_repository *repo;
	struct tog_reflist_entry *matched_entry;
	struct tog_colors colors;
};

/*
 * We implement two types of views: parent views and child views.
 *
 * The 'Tab' key switches focus between a parent view and its child view.
 * Child views are shown side-by-side to their parent view, provided
 * there is enough screen estate.
 *
 * When a new view is opened from within a parent view, this new view
 * becomes a child view of the parent view, replacing any existing child.
 *
 * When a new view is opened from within a child view, this new view
 * becomes a parent view which will obscure the views below until the
 * user quits the new parent view by typing 'q'.
 *
 * This list of views contains parent views only.
 * Child views are only pointed to by their parent view.
 */
TAILQ_HEAD(tog_view_list_head, tog_view);

struct tog_view {
	TAILQ_ENTRY(tog_view) entry;
	WINDOW *window;
	PANEL *panel;
	int nlines, ncols, begin_y, begin_x;
	int maxx, x; /* max column and current start column */
	int lines, cols; /* copies of LINES and COLS */
	int ch, count; /* current keymap and count prefix */
	int focussed; /* Only set on one parent or child view at a time. */
	int dying;
	struct tog_view *parent;
	struct tog_view *child;

	/*
	 * This flag is initially set on parent views when a new child view
	 * is created. It gets toggled when the 'Tab' key switches focus
	 * between parent and child.
	 * The flag indicates whether focus should be passed on to our child
	 * view if this parent view gets picked for focus after another parent
	 * view was closed. This prevents child views from losing focus in such
	 * situations.
	 */
	int focus_child;

	/* type-specific state */
	enum tog_view_type type;
	union {
		struct tog_diff_view_state diff;
		struct tog_log_view_state log;
		struct tog_blame_view_state blame;
		struct tog_tree_view_state tree;
		struct tog_ref_view_state ref;
	} state;

	const struct got_error *(*show)(struct tog_view *);
	const struct got_error *(*input)(struct tog_view **,
	    struct tog_view *, int);
	const struct got_error *(*close)(struct tog_view *);

	const struct got_error *(*search_start)(struct tog_view *);
	const struct got_error *(*search_next)(struct tog_view *);
	int search_started;
	int searching;
#define TOG_SEARCH_FORWARD	1
#define TOG_SEARCH_BACKWARD	2
	int search_next_done;
#define TOG_SEARCH_HAVE_MORE	1
#define TOG_SEARCH_NO_MORE	2
#define TOG_SEARCH_HAVE_NONE	3
	regex_t regex;
	regmatch_t regmatch;
};

static const struct got_error *open_diff_view(struct tog_view *,
    struct got_object_id *, struct got_object_id *,
    const char *, const char *, int, int, int, struct tog_view *,
    struct got_repository *);
static const struct got_error *show_diff_view(struct tog_view *);
static const struct got_error *input_diff_view(struct tog_view **,
    struct tog_view *, int);
static const struct got_error* close_diff_view(struct tog_view *);
static const struct got_error *search_start_diff_view(struct tog_view *);
static const struct got_error *search_next_diff_view(struct tog_view *);

static const struct got_error *open_log_view(struct tog_view *,
    struct got_object_id *, struct got_repository *,
    const char *, const char *, int);
static const struct got_error * show_log_view(struct tog_view *);
static const struct got_error *input_log_view(struct tog_view **,
    struct tog_view *, int);
static const struct got_error *close_log_view(struct tog_view *);
static const struct got_error *search_start_log_view(struct tog_view *);
static const struct got_error *search_next_log_view(struct tog_view *);

static const struct got_error *open_blame_view(struct tog_view *, char *,
    struct got_object_id *, struct got_repository *);
static const struct got_error *show_blame_view(struct tog_view *);
static const struct got_error *input_blame_view(struct tog_view **,
    struct tog_view *, int);
static const struct got_error *close_blame_view(struct tog_view *);
static const struct got_error *search_start_blame_view(struct tog_view *);
static const struct got_error *search_next_blame_view(struct tog_view *);

static const struct got_error *open_tree_view(struct tog_view *,
    struct got_object_id *, const char *, struct got_repository *);
static const struct got_error *show_tree_view(struct tog_view *);
static const struct got_error *input_tree_view(struct tog_view **,
    struct tog_view *, int);
static const struct got_error *close_tree_view(struct tog_view *);
static const struct got_error *search_start_tree_view(struct tog_view *);
static const struct got_error *search_next_tree_view(struct tog_view *);

static const struct got_error *open_ref_view(struct tog_view *,
    struct got_repository *);
static const struct got_error *show_ref_view(struct tog_view *);
static const struct got_error *input_ref_view(struct tog_view **,
    struct tog_view *, int);
static const struct got_error *close_ref_view(struct tog_view *);
static const struct got_error *search_start_ref_view(struct tog_view *);
static const struct got_error *search_next_ref_view(struct tog_view *);

static volatile sig_atomic_t tog_sigwinch_received;
static volatile sig_atomic_t tog_sigpipe_received;
static volatile sig_atomic_t tog_sigcont_received;
static volatile sig_atomic_t tog_sigint_received;
static volatile sig_atomic_t tog_sigterm_received;

static void
tog_sigwinch(int signo)
{
	tog_sigwinch_received = 1;
}

static void
tog_sigpipe(int signo)
{
	tog_sigpipe_received = 1;
}

static void
tog_sigcont(int signo)
{
	tog_sigcont_received = 1;
}

static void
tog_sigint(int signo)
{
	tog_sigint_received = 1;
}

static void
tog_sigterm(int signo)
{
	tog_sigterm_received = 1;
}

static int
tog_fatal_signal_received(void)
{
	return (tog_sigpipe_received ||
	    tog_sigint_received || tog_sigint_received);
}


static const struct got_error *
view_close(struct tog_view *view)
{
	const struct got_error *err = NULL;

	if (view->child) {
		view_close(view->child);
		view->child = NULL;
	}
	if (view->close)
		err = view->close(view);
	if (view->panel)
		del_panel(view->panel);
	if (view->window)
		delwin(view->window);
	free(view);
	return err;
}

static struct tog_view *
view_open(int nlines, int ncols, int begin_y, int begin_x,
    enum tog_view_type type)
{
	struct tog_view *view = calloc(1, sizeof(*view));

	if (view == NULL)
		return NULL;

	view->ch = 0;
	view->count = 0;
	view->type = type;
	view->lines = LINES;
	view->cols = COLS;
	view->nlines = nlines ? nlines : LINES - begin_y;
	view->ncols = ncols ? ncols : COLS - begin_x;
	view->begin_y = begin_y;
	view->begin_x = begin_x;
	view->window = newwin(nlines, ncols, begin_y, begin_x);
	if (view->window == NULL) {
		view_close(view);
		return NULL;
	}
	view->panel = new_panel(view->window);
	if (view->panel == NULL ||
	    set_panel_userptr(view->panel, view) != OK) {
		view_close(view);
		return NULL;
	}

	keypad(view->window, TRUE);
	return view;
}

static int
view_split_begin_x(int begin_x)
{
	if (begin_x > 0 || COLS < 120)
		return 0;
	return (COLS - MAX(COLS / 2, 80));
}

static const struct got_error *view_resize(struct tog_view *);

static const struct got_error *
view_splitscreen(struct tog_view *view)
{
	const struct got_error *err = NULL;

	view->begin_y = 0;
	view->begin_x = view_split_begin_x(0);
	view->nlines = LINES;
	view->ncols = COLS - view->begin_x;
	view->lines = LINES;
	view->cols = COLS;
	err = view_resize(view);
	if (err)
		return err;

	if (mvwin(view->window, view->begin_y, view->begin_x) == ERR)
		return got_error_from_errno("mvwin");

	return NULL;
}

static const struct got_error *
view_fullscreen(struct tog_view *view)
{
	const struct got_error *err = NULL;

	view->begin_x = 0;
	view->begin_y = 0;
	view->nlines = LINES;
	view->ncols = COLS;
	view->lines = LINES;
	view->cols = COLS;
	err = view_resize(view);
	if (err)
		return err;

	if (mvwin(view->window, view->begin_y, view->begin_x) == ERR)
		return got_error_from_errno("mvwin");

	return NULL;
}

static int
view_is_parent_view(struct tog_view *view)
{
	return view->parent == NULL;
}

static int
view_is_splitscreen(struct tog_view *view)
{
	return view->begin_x > 0;
}


static const struct got_error *
view_resize(struct tog_view *view)
{
	int nlines, ncols;

	if (view->lines > LINES)
		nlines = view->nlines - (view->lines - LINES);
	else
		nlines = view->nlines + (LINES - view->lines);

	if (view->cols > COLS)
		ncols = view->ncols - (view->cols - COLS);
	else
		ncols = view->ncols + (COLS - view->cols);

	if (view->child) {
		if (view->child->focussed)
			view->child->begin_x = view_split_begin_x(view->begin_x);
		if (view->child->begin_x == 0) {
			ncols = COLS;

			view_fullscreen(view->child);
			if (view->child->focussed)
				show_panel(view->child->panel);
			else
				show_panel(view->panel);
		} else {
			ncols = view->child->begin_x;

			view_splitscreen(view->child);
			show_panel(view->child->panel);
		}
	} else if (view->parent == NULL)
		ncols = COLS;

	if (wresize(view->window, nlines, ncols) == ERR)
		return got_error_from_errno("wresize");
	if (replace_panel(view->panel, view->window) == ERR)
		return got_error_from_errno("replace_panel");
	wclear(view->window);

	view->nlines = nlines;
	view->ncols = ncols;
	view->lines = LINES;
	view->cols = COLS;

	return NULL;
}

static const struct got_error *
view_close_child(struct tog_view *view)
{
	const struct got_error *err = NULL;

	if (view->child == NULL)
		return NULL;

	err = view_close(view->child);
	view->child = NULL;
	return err;
}

static const struct got_error *
view_set_child(struct tog_view *view, struct tog_view *child)
{
	view->child = child;
	child->parent = view;

	return view_resize(view);
}

static void
tog_resizeterm(void)
{
	int cols, lines;
	struct winsize size;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
		cols = 80;     /* Default */
		lines = 24;
	} else {
		cols = size.ws_col;
		lines = size.ws_row;
	}
	resize_term(lines, cols);
}

static const struct got_error *
view_search_start(struct tog_view *view)
{
	const struct got_error *err = NULL;
	char pattern[1024];
	int ret;

	if (view->search_started) {
		regfree(&view->regex);
		view->searching = 0;
		memset(&view->regmatch, 0, sizeof(view->regmatch));
	}
	view->search_started = 0;

	if (view->nlines < 1)
		return NULL;

	mvwaddstr(view->window, view->begin_y + view->nlines - 1, 0, "/");
	wclrtoeol(view->window);

	nocbreak();
	echo();
	ret = wgetnstr(view->window, pattern, sizeof(pattern));
	cbreak();
	noecho();
	if (ret == ERR)
		return NULL;

	if (regcomp(&view->regex, pattern, REG_EXTENDED | REG_NEWLINE) == 0) {
		err = view->search_start(view);
		if (err) {
			regfree(&view->regex);
			return err;
		}
		view->search_started = 1;
		view->searching = TOG_SEARCH_FORWARD;
		view->search_next_done = 0;
		view->search_next(view);
	}

	return NULL;
}

/*
 * Compute view->count from numeric user input.  User has five-tenths of a
 * second to follow each numeric keypress with another number to form count.
 * Return first non-numeric input or ERR and assign total to view->count.
 * XXX Should we add support for user-defined timeout?
 */
static int
get_compound_key(struct tog_view *view, int c)
{
	int x, n = 0;

	view->count = 0;
	halfdelay(5);  /* block for half a second */
	wattron(view->window, A_BOLD);
	wmove(view->window, view->nlines - 1, 0);
	wclrtoeol(view->window);
	waddch(view->window, ':');

	do {
		x = getcurx(view->window);
		if (x != ERR && x < view->ncols)
			waddch(view->window, c);
		/*
		 * Don't overflow. Max valid request should be the greatest
		 * between the longest and total lines; cap at 10 million.
		 */
		if (n >= 9999999)
			n = 9999999;
		else
			n = n * 10 + (c - '0');
	} while (((c = wgetch(view->window))) >= '0' && c <= '9' && c != ERR);

	/* Massage excessive or inapplicable values at the input handler. */
	view->count = n;

	wattroff(view->window, A_BOLD);
	cbreak();  /* return to blocking */
	return c;
}

static const struct got_error *
view_input(struct tog_view **new, int *done, struct tog_view *view,
    struct tog_view_list_head *views)
{
	const struct got_error *err = NULL;
	struct tog_view *v;
	int ch, errcode;

	*new = NULL;

	/* Clear "no matches" indicator. */
	if (view->search_next_done == TOG_SEARCH_NO_MORE ||
	    view->search_next_done == TOG_SEARCH_HAVE_NONE) {
		view->search_next_done = TOG_SEARCH_HAVE_MORE;
		view->count = 0;
	}

	if (view->searching && !view->search_next_done) {
		errcode = pthread_mutex_unlock(&tog_mutex);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_mutex_unlock");
		sched_yield();
		errcode = pthread_mutex_lock(&tog_mutex);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_mutex_lock");
		view->search_next(view);
		return NULL;
	}

	nodelay(stdscr, FALSE);
	/* Allow threads to make progress while we are waiting for input. */
	errcode = pthread_mutex_unlock(&tog_mutex);
	if (errcode)
		return got_error_set_errno(errcode, "pthread_mutex_unlock");
	/* If we have an unfinished count, don't get a new key map. */
	ch = view->ch;
	if ((view->count && --view->count == 0) || !view->count) {
		ch = wgetch(view->window);
		if (ch >= '1' && ch  <= '9')
			view->ch = ch = get_compound_key(view, ch);
	}
	errcode = pthread_mutex_lock(&tog_mutex);
	if (errcode)
		return got_error_set_errno(errcode, "pthread_mutex_lock");
	nodelay(stdscr, TRUE);

	if (tog_sigwinch_received || tog_sigcont_received) {
		tog_resizeterm();
		tog_sigwinch_received = 0;
		tog_sigcont_received = 0;
		TAILQ_FOREACH(v, views, entry) {
			err = view_resize(v);
			if (err)
				return err;
			err = v->input(new, v, KEY_RESIZE);
			if (err)
				return err;
			if (v->child) {
				err = view_resize(v->child);
				if (err)
					return err;
				err = v->child->input(new, v->child,
				    KEY_RESIZE);
				if (err)
					return err;
			}
		}
	}

	switch (ch) {
	case '\t':
		view->count = 0;
		if (view->child) {
			view->focussed = 0;
			view->child->focussed = 1;
			view->focus_child = 1;
		} else if (view->parent) {
			view->focussed = 0;
			view->parent->focussed = 1;
			view->parent->focus_child = 0;
			if (!view_is_splitscreen(view))
				err = view_fullscreen(view->parent);
		}
		break;
	case 'q':
		err = view->input(new, view, ch);
		view->dying = 1;
		break;
	case 'Q':
		*done = 1;
		break;
	case 'F':
		view->count = 0;
		if (view_is_parent_view(view)) {
			if (view->child == NULL)
				break;
			if (view_is_splitscreen(view->child)) {
				view->focussed = 0;
				view->child->focussed = 1;
				err = view_fullscreen(view->child);
			} else
				err = view_splitscreen(view->child);
			if (err)
				break;
			err = view->child->input(new, view->child,
			    KEY_RESIZE);
		} else {
			if (view_is_splitscreen(view)) {
				view->parent->focussed = 0;
				view->focussed = 1;
				err = view_fullscreen(view);
			} else {
				err = view_splitscreen(view);
				if (!err)
					err = view_resize(view->parent);
			}
			if (err)
				break;
			err = view->input(new, view, KEY_RESIZE);
		}
		break;
	case KEY_RESIZE:
		break;
	case '/':
		view->count = 0;
		if (view->search_start)
			view_search_start(view);
		else
			err = view->input(new, view, ch);
		break;
	case 'N':
	case 'n':
		if (view->search_started && view->search_next) {
			view->searching = (ch == 'n' ?
			    TOG_SEARCH_FORWARD : TOG_SEARCH_BACKWARD);
			view->search_next_done = 0;
			view->search_next(view);
		} else
			err = view->input(new, view, ch);
		break;
	default:
		err = view->input(new, view, ch);
		break;
	}

	return err;
}

static void
view_vborder(struct tog_view *view)
{
	PANEL *panel;
	const struct tog_view *view_above;

	if (view->parent)
		return view_vborder(view->parent);

	panel = panel_above(view->panel);
	if (panel == NULL)
		return;

	view_above = panel_userptr(panel);
	mvwvline(view->window, view->begin_y, view_above->begin_x - 1,
	    got_locale_is_utf8() ? ACS_VLINE : '|', view->nlines);
}

static int
view_needs_focus_indication(struct tog_view *view)
{
	if (view_is_parent_view(view)) {
		if (view->child == NULL || view->child->focussed)
			return 0;
		if (!view_is_splitscreen(view->child))
			return 0;
	} else if (!view_is_splitscreen(view))
		return 0;

	return view->focussed;
}

static const struct got_error *
view_loop(struct tog_view *view)
{
	const struct got_error *err = NULL;
	struct tog_view_list_head views;
	struct tog_view *new_view;
	int fast_refresh = 10;
	int done = 0, errcode;

	errcode = pthread_mutex_lock(&tog_mutex);
	if (errcode)
		return got_error_set_errno(errcode, "pthread_mutex_lock");

	TAILQ_INIT(&views);
	TAILQ_INSERT_HEAD(&views, view, entry);

	view->focussed = 1;
	err = view->show(view);
	if (err)
		return err;
	update_panels();
	doupdate();
	while (!TAILQ_EMPTY(&views) && !done && !tog_fatal_signal_received()) {
		/* Refresh fast during initialization, then become slower. */
		if (fast_refresh && fast_refresh-- == 0)
			halfdelay(10); /* switch to once per second */

		err = view_input(&new_view, &done, view, &views);
		if (err)
			break;
		if (view->dying) {
			struct tog_view *v, *prev = NULL;

			if (view_is_parent_view(view))
				prev = TAILQ_PREV(view, tog_view_list_head,
				    entry);
			else if (view->parent)
				prev = view->parent;

			if (view->parent) {
				view->parent->child = NULL;
				view->parent->focus_child = 0;

				err = view_resize(view->parent);
				if (err)
					break;
			} else
				TAILQ_REMOVE(&views, view, entry);

			err = view_close(view);
			if (err)
				goto done;

			view = NULL;
			TAILQ_FOREACH(v, &views, entry) {
				if (v->focussed)
					break;
			}
			if (view == NULL && new_view == NULL) {
				/* No view has focus. Try to pick one. */
				if (prev)
					view = prev;
				else if (!TAILQ_EMPTY(&views)) {
					view = TAILQ_LAST(&views,
					    tog_view_list_head);
				}
				if (view) {
					if (view->focus_child) {
						view->child->focussed = 1;
						view = view->child;
					} else
						view->focussed = 1;
				}
			}
		}
		if (new_view) {
			struct tog_view *v, *t;
			/* Only allow one parent view per type. */
			TAILQ_FOREACH_SAFE(v, &views, entry, t) {
				if (v->type != new_view->type)
					continue;
				TAILQ_REMOVE(&views, v, entry);
				err = view_close(v);
				if (err)
					goto done;
				break;
			}
			TAILQ_INSERT_TAIL(&views, new_view, entry);
			view = new_view;
		}
		if (view) {
			if (view_is_parent_view(view)) {
				if (view->child && view->child->focussed)
					view = view->child;
			} else {
				if (view->parent && view->parent->focussed)
					view = view->parent;
			}
			show_panel(view->panel);
			if (view->child && view_is_splitscreen(view->child))
				show_panel(view->child->panel);
			if (view->parent && view_is_splitscreen(view)) {
				err = view->parent->show(view->parent);
				if (err)
					goto done;
			}
			err = view->show(view);
			if (err)
				goto done;
			if (view->child) {
				err = view->child->show(view->child);
				if (err)
					goto done;
			}
			update_panels();
			doupdate();
		}
	}
done:
	while (!TAILQ_EMPTY(&views)) {
		view = TAILQ_FIRST(&views);
		TAILQ_REMOVE(&views, view, entry);
		view_close(view);
	}

	errcode = pthread_mutex_unlock(&tog_mutex);
	if (errcode && err == NULL)
		err = got_error_set_errno(errcode, "pthread_mutex_unlock");

	return err;
}

__dead static void
usage_log(void)
{
	endwin();
	fprintf(stderr,
	    "usage: %s log [-b] [-c commit] [-r repository-path] [path]\n",
	    getprogname());
	exit(1);
}

/* Create newly allocated wide-character string equivalent to a byte string. */
static const struct got_error *
mbs2ws(wchar_t **ws, size_t *wlen, const char *s)
{
	char *vis = NULL;
	const struct got_error *err = NULL;

	*ws = NULL;
	*wlen = mbstowcs(NULL, s, 0);
	if (*wlen == (size_t)-1) {
		int vislen;
		if (errno != EILSEQ)
			return got_error_from_errno("mbstowcs");

		/* byte string invalid in current encoding; try to "fix" it */
		err = got_mbsavis(&vis, &vislen, s);
		if (err)
			return err;
		*wlen = mbstowcs(NULL, vis, 0);
		if (*wlen == (size_t)-1) {
			err = got_error_from_errno("mbstowcs"); /* give up */
			goto done;
		}
	}

	*ws = calloc(*wlen + 1, sizeof(**ws));
	if (*ws == NULL) {
		err = got_error_from_errno("calloc");
		goto done;
	}

	if (mbstowcs(*ws, vis ? vis : s, *wlen) != *wlen)
		err = got_error_from_errno("mbstowcs");
done:
	free(vis);
	if (err) {
		free(*ws);
		*ws = NULL;
		*wlen = 0;
	}
	return err;
}

static const struct got_error *
expand_tab(char **ptr, const char *src)
{
	char	*dst;
	size_t	 len, n, idx = 0, sz = 0;

	*ptr = NULL;
	n = len = strlen(src);
	dst = malloc(n + 1);
	if (dst == NULL)
		return got_error_from_errno("malloc");

	while (idx < len && src[idx]) {
		const char c = src[idx];

		if (c == '\t') {
			size_t nb = TABSIZE - sz % TABSIZE;
			char *p;

			p = realloc(dst, n + nb);
			if (p == NULL) {
				free(dst);
				return got_error_from_errno("realloc");

			}
			dst = p;
			n += nb;
			memset(dst + sz, ' ', nb);
			sz += nb;
		} else
			dst[sz++] = src[idx];
		++idx;
	}

	dst[sz] = '\0';
	*ptr = dst;
	return NULL;
}

/*
 * Advance at most n columns from wline starting at offset off.
 * Return the index to the first character after the span operation.
 * Return the combined column width of all spanned wide character in
 * *rcol.
 */
static int
span_wline(int *rcol, int off, wchar_t *wline, int n, int col_tab_align)
{
	int width, i, cols = 0;

	if (n == 0) {
		*rcol = cols;
		return off;
	}

	for (i = off; wline[i] != L'\0'; ++i) {
		if (wline[i] == L'\t')
			width = TABSIZE - ((cols + col_tab_align) % TABSIZE);
		else
			width = wcwidth(wline[i]);

		if (width == -1) {
			width = 1;
			wline[i] = L'.';
		}

		if (cols + width > n)
			break;
		cols += width;
	}

	*rcol = cols;
	return i;
}

/*
 * Format a line for display, ensuring that it won't overflow a width limit.
 * With scrolling, the width returned refers to the scrolled version of the
 * line, which starts at (*wlinep)[*scrollxp]. The caller must free *wlinep.
 */
static const struct got_error *
format_line(wchar_t **wlinep, int *widthp, int *scrollxp,
    const char *line, int nscroll, int wlimit, int col_tab_align, int expand)
{
	const struct got_error *err = NULL;
	int cols;
	wchar_t *wline = NULL;
	char *exstr = NULL;
	size_t wlen;
	int i, scrollx;

	*wlinep = NULL;
	*widthp = 0;

	if (expand) {
		err = expand_tab(&exstr, line);
		if (err)
			return err;
	}

	err = mbs2ws(&wline, &wlen, expand ? exstr : line);
	free(exstr);
	if (err)
		return err;

	scrollx = span_wline(&cols, 0, wline, nscroll, col_tab_align);

	if (wlen > 0 && wline[wlen - 1] == L'\n') {
		wline[wlen - 1] = L'\0';
		wlen--;
	}
	if (wlen > 0 && wline[wlen - 1] == L'\r') {
		wline[wlen - 1] = L'\0';
		wlen--;
	}

	i = span_wline(&cols, scrollx, wline, wlimit, col_tab_align);
	wline[i] = L'\0';

	if (widthp)
		*widthp = cols;
	if (scrollxp)
		*scrollxp = scrollx;
	if (err)
		free(wline);
	else
		*wlinep = wline;
	return err;
}

static const struct got_error*
build_refs_str(char **refs_str, struct got_reflist_head *refs,
    struct got_object_id *id, struct got_repository *repo)
{
	static const struct got_error *err = NULL;
	struct got_reflist_entry *re;
	char *s;
	const char *name;

	*refs_str = NULL;

	TAILQ_FOREACH(re, refs, entry) {
		struct got_tag_object *tag = NULL;
		struct got_object_id *ref_id;
		int cmp;

		name = got_ref_get_name(re->ref);
		if (strcmp(name, GOT_REF_HEAD) == 0)
			continue;
		if (strncmp(name, "refs/", 5) == 0)
			name += 5;
		if (strncmp(name, "got/", 4) == 0 &&
		    strncmp(name, "got/backup/", 11) != 0)
			continue;
		if (strncmp(name, "heads/", 6) == 0)
			name += 6;
		if (strncmp(name, "remotes/", 8) == 0) {
			name += 8;
			s = strstr(name, "/" GOT_REF_HEAD);
			if (s != NULL && s[strlen(s)] == '\0')
				continue;
		}
		err = got_ref_resolve(&ref_id, repo, re->ref);
		if (err)
			break;
		if (strncmp(name, "tags/", 5) == 0) {
			err = got_object_open_as_tag(&tag, repo, ref_id);
			if (err) {
				if (err->code != GOT_ERR_OBJ_TYPE) {
					free(ref_id);
					break;
				}
				/* Ref points at something other than a tag. */
				err = NULL;
				tag = NULL;
			}
		}
		cmp = got_object_id_cmp(tag ?
		    got_object_tag_get_object_id(tag) : ref_id, id);
		free(ref_id);
		if (tag)
			got_object_tag_close(tag);
		if (cmp != 0)
			continue;
		s = *refs_str;
		if (asprintf(refs_str, "%s%s%s", s ? s : "",
		    s ? ", " : "", name) == -1) {
			err = got_error_from_errno("asprintf");
			free(s);
			*refs_str = NULL;
			break;
		}
		free(s);
	}

	return err;
}

static const struct got_error *
format_author(wchar_t **wauthor, int *author_width, char *author, int limit,
    int col_tab_align)
{
	char *smallerthan;

	smallerthan = strchr(author, '<');
	if (smallerthan && smallerthan[1] != '\0')
		author = smallerthan + 1;
	author[strcspn(author, "@>")] = '\0';
	return format_line(wauthor, author_width, NULL, author, 0, limit,
	    col_tab_align, 0);
}

static const struct got_error *
draw_commit(struct tog_view *view, struct got_commit_object *commit,
    struct got_object_id *id, const size_t date_display_cols,
    int author_display_cols)
{
	struct tog_log_view_state *s = &view->state.log;
	const struct got_error *err = NULL;
	char datebuf[12]; /* YYYY-MM-DD + SPACE + NUL */
	char *logmsg0 = NULL, *logmsg = NULL;
	char *author = NULL;
	wchar_t *wlogmsg = NULL, *wauthor = NULL;
	int author_width, logmsg_width;
	char *newline, *line = NULL;
	int col, limit, scrollx;
	const int avail = view->ncols;
	struct tm tm;
	time_t committer_time;
	struct tog_color *tc;

	committer_time = got_object_commit_get_committer_time(commit);
	if (gmtime_r(&committer_time, &tm) == NULL)
		return got_error_from_errno("gmtime_r");
	if (strftime(datebuf, sizeof(datebuf), "%G-%m-%d ", &tm) == 0)
		return got_error(GOT_ERR_NO_SPACE);

	if (avail <= date_display_cols)
		limit = MIN(sizeof(datebuf) - 1, avail);
	else
		limit = MIN(date_display_cols, sizeof(datebuf) - 1);
	tc = get_color(&s->colors, TOG_COLOR_DATE);
	if (tc)
		wattr_on(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	waddnstr(view->window, datebuf, limit);
	if (tc)
		wattr_off(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	col = limit;
	if (col > avail)
		goto done;

	if (avail >= 120) {
		char *id_str;
		err = got_object_id_str(&id_str, id);
		if (err)
			goto done;
		tc = get_color(&s->colors, TOG_COLOR_COMMIT);
		if (tc)
			wattr_on(view->window,
			    COLOR_PAIR(tc->colorpair), NULL);
		wprintw(view->window, "%.8s ", id_str);
		if (tc)
			wattr_off(view->window,
			    COLOR_PAIR(tc->colorpair), NULL);
		free(id_str);
		col += 9;
		if (col > avail)
			goto done;
	}

	author = strdup(got_object_commit_get_author(commit));
	if (author == NULL) {
		err = got_error_from_errno("strdup");
		goto done;
	}
	err = format_author(&wauthor, &author_width, author, avail - col, col);
	if (err)
		goto done;
	tc = get_color(&s->colors, TOG_COLOR_AUTHOR);
	if (tc)
		wattr_on(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	waddwstr(view->window, wauthor);
	if (tc)
		wattr_off(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	col += author_width;
	while (col < avail && author_width < author_display_cols + 2) {
		waddch(view->window, ' ');
		col++;
		author_width++;
	}
	if (col > avail)
		goto done;

	err = got_object_commit_get_logmsg(&logmsg0, commit);
	if (err)
		goto done;
	logmsg = logmsg0;
	while (*logmsg == '\n')
		logmsg++;
	newline = strchr(logmsg, '\n');
	if (newline)
		*newline = '\0';
	limit = avail - col;
	if (view->child && view_is_splitscreen(view->child) && limit > 0)
		limit--;	/* for the border */
	err = format_line(&wlogmsg, &logmsg_width, &scrollx, logmsg, view->x,
	    limit, col, 1);
	if (err)
		goto done;
	waddwstr(view->window, &wlogmsg[scrollx]);
	col += MAX(logmsg_width, 0);
	while (col < avail) {
		waddch(view->window, ' ');
		col++;
	}
done:
	free(logmsg0);
	free(wlogmsg);
	free(author);
	free(wauthor);
	free(line);
	return err;
}

static struct commit_queue_entry *
alloc_commit_queue_entry(struct got_commit_object *commit,
    struct got_object_id *id)
{
	struct commit_queue_entry *entry;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;

	entry->id = id;
	entry->commit = commit;
	return entry;
}

static void
pop_commit(struct commit_queue *commits)
{
	struct commit_queue_entry *entry;

	entry = TAILQ_FIRST(&commits->head);
	TAILQ_REMOVE(&commits->head, entry, entry);
	got_object_commit_close(entry->commit);
	commits->ncommits--;
	/* Don't free entry->id! It is owned by the commit graph. */
	free(entry);
}

static void
free_commits(struct commit_queue *commits)
{
	while (!TAILQ_EMPTY(&commits->head))
		pop_commit(commits);
}

static const struct got_error *
match_commit(int *have_match, struct got_object_id *id,
    struct got_commit_object *commit, regex_t *regex)
{
	const struct got_error *err = NULL;
	regmatch_t regmatch;
	char *id_str = NULL, *logmsg = NULL;

	*have_match = 0;

	err = got_object_id_str(&id_str, id);
	if (err)
		return err;

	err = got_object_commit_get_logmsg(&logmsg, commit);
	if (err)
		goto done;

	if (regexec(regex, got_object_commit_get_author(commit), 1,
	    &regmatch, 0) == 0 ||
	    regexec(regex, got_object_commit_get_committer(commit), 1,
	    &regmatch, 0) == 0 ||
	    regexec(regex, id_str, 1, &regmatch, 0) == 0 ||
	    regexec(regex, logmsg, 1, &regmatch, 0) == 0)
		*have_match = 1;
done:
	free(id_str);
	free(logmsg);
	return err;
}

static const struct got_error *
queue_commits(struct tog_log_thread_args *a)
{
	const struct got_error *err = NULL;

	/*
	 * We keep all commits open throughout the lifetime of the log
	 * view in order to avoid having to re-fetch commits from disk
	 * while updating the display.
	 */
	do {
		struct got_object_id *id;
		struct got_commit_object *commit;
		struct commit_queue_entry *entry;
		int errcode;

		err = got_commit_graph_iter_next(&id, a->graph, a->repo,
		    NULL, NULL);
		if (err || id == NULL)
			break;

		err = got_object_open_as_commit(&commit, a->repo, id);
		if (err)
			break;
		entry = alloc_commit_queue_entry(commit, id);
		if (entry == NULL) {
			err = got_error_from_errno("alloc_commit_queue_entry");
			break;
		}

		errcode = pthread_mutex_lock(&tog_mutex);
		if (errcode) {
			err = got_error_set_errno(errcode,
			    "pthread_mutex_lock");
			break;
		}

		entry->idx = a->commits->ncommits;
		TAILQ_INSERT_TAIL(&a->commits->head, entry, entry);
		a->commits->ncommits++;

		if (*a->searching == TOG_SEARCH_FORWARD &&
		    !*a->search_next_done) {
			int have_match;
			err = match_commit(&have_match, id, commit, a->regex);
			if (err)
				break;
			if (have_match)
				*a->search_next_done = TOG_SEARCH_HAVE_MORE;
		}

		errcode = pthread_mutex_unlock(&tog_mutex);
		if (errcode && err == NULL)
			err = got_error_set_errno(errcode,
			    "pthread_mutex_unlock");
		if (err)
			break;
	} while (*a->searching == TOG_SEARCH_FORWARD && !*a->search_next_done);

	return err;
}

static void
select_commit(struct tog_log_view_state *s)
{
	struct commit_queue_entry *entry;
	int ncommits = 0;

	entry = s->first_displayed_entry;
	while (entry) {
		if (ncommits == s->selected) {
			s->selected_entry = entry;
			break;
		}
		entry = TAILQ_NEXT(entry, entry);
		ncommits++;
	}
}

static const struct got_error *
draw_commits(struct tog_view *view)
{
	const struct got_error *err = NULL;
	struct tog_log_view_state *s = &view->state.log;
	struct commit_queue_entry *entry = s->selected_entry;
	const int limit = view->nlines;
	int width;
	int ncommits, author_cols = 4;
	char *id_str = NULL, *header = NULL, *ncommits_str = NULL;
	char *refs_str = NULL;
	wchar_t *wline;
	struct tog_color *tc;
	static const size_t date_display_cols = 12;

	if (s->selected_entry &&
	    !(view->searching && view->search_next_done == 0)) {
		struct got_reflist_head *refs;
		err = got_object_id_str(&id_str, s->selected_entry->id);
		if (err)
			return err;
		refs = got_reflist_object_id_map_lookup(tog_refs_idmap,
		    s->selected_entry->id);
		if (refs) {
			err = build_refs_str(&refs_str, refs,
			    s->selected_entry->id, s->repo);
			if (err)
				goto done;
		}
	}

	if (s->thread_args.commits_needed == 0)
		halfdelay(10); /* disable fast refresh */

	if (s->thread_args.commits_needed > 0 || s->thread_args.load_all) {
		if (asprintf(&ncommits_str, " [%d/%d] %s",
		    entry ? entry->idx + 1 : 0, s->commits.ncommits,
		    (view->searching && !view->search_next_done) ?
		    "searching..." : "loading...") == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
	} else {
		const char *search_str = NULL;

		if (view->searching) {
			if (view->search_next_done == TOG_SEARCH_NO_MORE)
				search_str = "no more matches";
			else if (view->search_next_done == TOG_SEARCH_HAVE_NONE)
				search_str = "no matches found";
			else if (!view->search_next_done)
				search_str = "searching...";
		}

		if (asprintf(&ncommits_str, " [%d/%d] %s",
		    entry ? entry->idx + 1 : 0, s->commits.ncommits,
		    search_str ? search_str :
		    (refs_str ? refs_str : "")) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
	}

	if (s->in_repo_path && strcmp(s->in_repo_path, "/") != 0) {
		if (asprintf(&header, "commit %s %s%s", id_str ? id_str :
		    "........................................",
		    s->in_repo_path, ncommits_str) == -1) {
			err = got_error_from_errno("asprintf");
			header = NULL;
			goto done;
		}
	} else if (asprintf(&header, "commit %s%s",
	    id_str ? id_str : "........................................",
	    ncommits_str) == -1) {
		err = got_error_from_errno("asprintf");
		header = NULL;
		goto done;
	}
	err = format_line(&wline, &width, NULL, header, 0, view->ncols, 0, 0);
	if (err)
		goto done;

	werase(view->window);

	if (view_needs_focus_indication(view))
		wstandout(view->window);
	tc = get_color(&s->colors, TOG_COLOR_COMMIT);
	if (tc)
		wattr_on(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	waddwstr(view->window, wline);
	if (tc)
		wattr_off(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	while (width < view->ncols) {
		waddch(view->window, ' ');
		width++;
	}
	if (view_needs_focus_indication(view))
		wstandend(view->window);
	free(wline);
	if (limit <= 1)
		goto done;

	/* Grow author column size if necessary, and set view->maxx. */
	entry = s->first_displayed_entry;
	ncommits = 0;
	view->maxx = 0;
	while (entry) {
		char *author, *eol, *msg, *msg0;
		wchar_t *wauthor, *wmsg;
		int width;
		if (ncommits >= limit - 1)
			break;
		author = strdup(got_object_commit_get_author(entry->commit));
		if (author == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
		err = format_author(&wauthor, &width, author, COLS,
		    date_display_cols);
		if (author_cols < width)
			author_cols = width;
		free(wauthor);
		free(author);
		err = got_object_commit_get_logmsg(&msg0, entry->commit);
		if (err)
			goto done;
		msg = msg0;
		while (*msg == '\n')
			++msg;
		if ((eol = strchr(msg, '\n')))
			*eol = '\0';
		err = format_line(&wmsg, &width, NULL, msg, 0, INT_MAX,
		    date_display_cols + author_cols, 0);
		if (err)
			goto done;
		view->maxx = MAX(view->maxx, width);
		free(msg0);
		free(wmsg);
		ncommits++;
		entry = TAILQ_NEXT(entry, entry);
	}

	entry = s->first_displayed_entry;
	s->last_displayed_entry = s->first_displayed_entry;
	ncommits = 0;
	while (entry) {
		if (ncommits >= limit - 1)
			break;
		if (ncommits == s->selected)
			wstandout(view->window);
		err = draw_commit(view, entry->commit, entry->id,
		    date_display_cols, author_cols);
		if (ncommits == s->selected)
			wstandend(view->window);
		if (err)
			goto done;
		ncommits++;
		s->last_displayed_entry = entry;
		entry = TAILQ_NEXT(entry, entry);
	}

	view_vborder(view);
done:
	free(id_str);
	free(refs_str);
	free(ncommits_str);
	free(header);
	return err;
}

static void
log_scroll_up(struct tog_log_view_state *s, int maxscroll)
{
	struct commit_queue_entry *entry;
	int nscrolled = 0;

	entry = TAILQ_FIRST(&s->commits.head);
	if (s->first_displayed_entry == entry)
		return;

	entry = s->first_displayed_entry;
	while (entry && nscrolled < maxscroll) {
		entry = TAILQ_PREV(entry, commit_queue_head, entry);
		if (entry) {
			s->first_displayed_entry = entry;
			nscrolled++;
		}
	}
}

static const struct got_error *
trigger_log_thread(struct tog_view *view, int wait)
{
	struct tog_log_thread_args *ta = &view->state.log.thread_args;
	int errcode;

	halfdelay(1); /* fast refresh while loading commits */

	while (ta->commits_needed > 0 || ta->load_all) {
		if (ta->log_complete)
			break;

		/* Wake the log thread. */
		errcode = pthread_cond_signal(&ta->need_commits);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_cond_signal");

		/*
		 * The mutex will be released while the view loop waits
		 * in wgetch(), at which time the log thread will run.
		 */
		if (!wait)
			break;

		/* Display progress update in log view. */
		show_log_view(view);
		update_panels();
		doupdate();

		/* Wait right here while next commit is being loaded. */
		errcode = pthread_cond_wait(&ta->commit_loaded, &tog_mutex);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_cond_wait");

		/* Display progress update in log view. */
		show_log_view(view);
		update_panels();
		doupdate();
	}

	return NULL;
}

static const struct got_error *
log_scroll_down(struct tog_view *view, int maxscroll)
{
	struct tog_log_view_state *s = &view->state.log;
	const struct got_error *err = NULL;
	struct commit_queue_entry *pentry;
	int nscrolled = 0, ncommits_needed;

	if (s->last_displayed_entry == NULL)
		return NULL;

	ncommits_needed = s->last_displayed_entry->idx + 1 + maxscroll;
	if (s->commits.ncommits < ncommits_needed &&
	    !s->thread_args.log_complete) {
		/*
		 * Ask the log thread for required amount of commits.
		 */
		s->thread_args.commits_needed += maxscroll;
		err = trigger_log_thread(view, 1);
		if (err)
			return err;
	}

	do {
		pentry = TAILQ_NEXT(s->last_displayed_entry, entry);
		if (pentry == NULL)
			break;

		s->last_displayed_entry = pentry;

		pentry = TAILQ_NEXT(s->first_displayed_entry, entry);
		if (pentry == NULL)
			break;
		s->first_displayed_entry = pentry;
	} while (++nscrolled < maxscroll);

	return err;
}

static const struct got_error *
open_diff_view_for_commit(struct tog_view **new_view, int begin_x,
    struct got_commit_object *commit, struct got_object_id *commit_id,
    struct tog_view *log_view, struct got_repository *repo)
{
	const struct got_error *err;
	struct got_object_qid *parent_id;
	struct tog_view *diff_view;

	diff_view = view_open(0, 0, 0, begin_x, TOG_VIEW_DIFF);
	if (diff_view == NULL)
		return got_error_from_errno("view_open");

	parent_id = STAILQ_FIRST(got_object_commit_get_parent_ids(commit));
	err = open_diff_view(diff_view, parent_id ? &parent_id->id : NULL,
	    commit_id, NULL, NULL, 3, 0, 0, log_view, repo);
	if (err == NULL)
		*new_view = diff_view;
	return err;
}

static const struct got_error *
tree_view_visit_subtree(struct tog_tree_view_state *s,
    struct got_tree_object *subtree)
{
	struct tog_parent_tree *parent;

	parent = calloc(1, sizeof(*parent));
	if (parent == NULL)
		return got_error_from_errno("calloc");

	parent->tree = s->tree;
	parent->first_displayed_entry = s->first_displayed_entry;
	parent->selected_entry = s->selected_entry;
	parent->selected = s->selected;
	TAILQ_INSERT_HEAD(&s->parents, parent, entry);
	s->tree = subtree;
	s->selected = 0;
	s->first_displayed_entry = NULL;
	return NULL;
}

static const struct got_error *
tree_view_walk_path(struct tog_tree_view_state *s,
    struct got_commit_object *commit, const char *path)
{
	const struct got_error *err = NULL;
	struct got_tree_object *tree = NULL;
	const char *p;
	char *slash, *subpath = NULL;

	/* Walk the path and open corresponding tree objects. */
	p = path;
	while (*p) {
		struct got_tree_entry *te;
		struct got_object_id *tree_id;
		char *te_name;

		while (p[0] == '/')
			p++;

		/* Ensure the correct subtree entry is selected. */
		slash = strchr(p, '/');
		if (slash == NULL)
			te_name = strdup(p);
		else
			te_name = strndup(p, slash - p);
		if (te_name == NULL) {
			err = got_error_from_errno("strndup");
			break;
		}
		te = got_object_tree_find_entry(s->tree, te_name);
		if (te == NULL) {
			err = got_error_path(te_name, GOT_ERR_NO_TREE_ENTRY);
			free(te_name);
			break;
		}
		free(te_name);
		s->first_displayed_entry = s->selected_entry = te;

		if (!S_ISDIR(got_tree_entry_get_mode(s->selected_entry)))
			break; /* jump to this file's entry */

		slash = strchr(p, '/');
		if (slash)
			subpath = strndup(path, slash - path);
		else
			subpath = strdup(path);
		if (subpath == NULL) {
			err = got_error_from_errno("strdup");
			break;
		}

		err = got_object_id_by_path(&tree_id, s->repo, commit,
		    subpath);
		if (err)
			break;

		err = got_object_open_as_tree(&tree, s->repo, tree_id);
		free(tree_id);
		if (err)
			break;

		err = tree_view_visit_subtree(s, tree);
		if (err) {
			got_object_tree_close(tree);
			break;
		}
		if (slash == NULL)
			break;
		free(subpath);
		subpath = NULL;
		p = slash;
	}

	free(subpath);
	return err;
}

static const struct got_error *
browse_commit_tree(struct tog_view **new_view, int begin_x,
    struct commit_queue_entry *entry, const char *path,
    const char *head_ref_name, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct tog_tree_view_state *s;
	struct tog_view *tree_view;

	tree_view = view_open(0, 0, 0, begin_x, TOG_VIEW_TREE);
	if (tree_view == NULL)
		return got_error_from_errno("view_open");

	err = open_tree_view(tree_view, entry->id, head_ref_name, repo);
	if (err)
		return err;
	s = &tree_view->state.tree;

	*new_view = tree_view;

	if (got_path_is_root_dir(path))
		return NULL;

	return tree_view_walk_path(s, entry->commit, path);
}

static const struct got_error *
block_signals_used_by_main_thread(void)
{
	sigset_t sigset;
	int errcode;

	if (sigemptyset(&sigset) == -1)
		return got_error_from_errno("sigemptyset");

	/* tog handles SIGWINCH, SIGCONT, SIGINT, SIGTERM */
	if (sigaddset(&sigset, SIGWINCH) == -1)
		return got_error_from_errno("sigaddset");
	if (sigaddset(&sigset, SIGCONT) == -1)
		return got_error_from_errno("sigaddset");
	if (sigaddset(&sigset, SIGINT) == -1)
		return got_error_from_errno("sigaddset");
	if (sigaddset(&sigset, SIGTERM) == -1)
		return got_error_from_errno("sigaddset");

	/* ncurses handles SIGTSTP */
	if (sigaddset(&sigset, SIGTSTP) == -1)
		return got_error_from_errno("sigaddset");

	errcode = pthread_sigmask(SIG_BLOCK, &sigset, NULL);
	if (errcode)
		return got_error_set_errno(errcode, "pthread_sigmask");

	return NULL;
}

static void *
log_thread(void *arg)
{
	const struct got_error *err = NULL;
	int errcode = 0;
	struct tog_log_thread_args *a = arg;
	int done = 0;

	err = block_signals_used_by_main_thread();
	if (err)
		return (void *)err;

	while (!done && !err && !tog_fatal_signal_received()) {
		err = queue_commits(a);
		if (err) {
			if (err->code != GOT_ERR_ITER_COMPLETED)
				return (void *)err;
			err = NULL;
			done = 1;
		} else if (a->commits_needed > 0 && !a->load_all)
			a->commits_needed--;

		errcode = pthread_mutex_lock(&tog_mutex);
		if (errcode) {
			err = got_error_set_errno(errcode,
			    "pthread_mutex_lock");
			break;
		} else if (*a->quit)
			done = 1;
		else if (*a->first_displayed_entry == NULL) {
			*a->first_displayed_entry =
			    TAILQ_FIRST(&a->commits->head);
			*a->selected_entry = *a->first_displayed_entry;
		}

		errcode = pthread_cond_signal(&a->commit_loaded);
		if (errcode) {
			err = got_error_set_errno(errcode,
			    "pthread_cond_signal");
			pthread_mutex_unlock(&tog_mutex);
			break;
		}

		if (done)
			a->commits_needed = 0;
		else {
			if (a->commits_needed == 0 && !a->load_all) {
				errcode = pthread_cond_wait(&a->need_commits,
				    &tog_mutex);
				if (errcode)
					err = got_error_set_errno(errcode,
					    "pthread_cond_wait");
				if (*a->quit)
					done = 1;
			}
		}

		errcode = pthread_mutex_unlock(&tog_mutex);
		if (errcode && err == NULL)
			err = got_error_set_errno(errcode,
			    "pthread_mutex_unlock");
	}
	a->log_complete = 1;
	return (void *)err;
}

static const struct got_error *
stop_log_thread(struct tog_log_view_state *s)
{
	const struct got_error *err = NULL;
	int errcode;

	if (s->thread) {
		s->quit = 1;
		errcode = pthread_cond_signal(&s->thread_args.need_commits);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_cond_signal");
		errcode = pthread_mutex_unlock(&tog_mutex);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_mutex_unlock");
		errcode = pthread_join(s->thread, (void **)&err);
		if (errcode)
			return got_error_set_errno(errcode, "pthread_join");
		errcode = pthread_mutex_lock(&tog_mutex);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_mutex_lock");
		s->thread = NULL;
	}

	if (s->thread_args.repo) {
		err = got_repo_close(s->thread_args.repo);
		s->thread_args.repo = NULL;
	}

	if (s->thread_args.pack_fds) {
		const struct got_error *pack_err =
		    got_repo_pack_fds_close(s->thread_args.pack_fds);
		if (err == NULL)
			err = pack_err;
		s->thread_args.pack_fds = NULL;
	}

	if (s->thread_args.graph) {
		got_commit_graph_close(s->thread_args.graph);
		s->thread_args.graph = NULL;
	}

	return err;
}

static const struct got_error *
close_log_view(struct tog_view *view)
{
	const struct got_error *err = NULL;
	struct tog_log_view_state *s = &view->state.log;
	int errcode;

	err = stop_log_thread(s);

	errcode = pthread_cond_destroy(&s->thread_args.need_commits);
	if (errcode && err == NULL)
		err = got_error_set_errno(errcode, "pthread_cond_destroy");

	errcode = pthread_cond_destroy(&s->thread_args.commit_loaded);
	if (errcode && err == NULL)
		err = got_error_set_errno(errcode, "pthread_cond_destroy");

	free_commits(&s->commits);
	free(s->in_repo_path);
	s->in_repo_path = NULL;
	free(s->start_id);
	s->start_id = NULL;
	free(s->head_ref_name);
	s->head_ref_name = NULL;
	return err;
}

static const struct got_error *
search_start_log_view(struct tog_view *view)
{
	struct tog_log_view_state *s = &view->state.log;

	s->matched_entry = NULL;
	s->search_entry = NULL;
	return NULL;
}

static const struct got_error *
search_next_log_view(struct tog_view *view)
{
	const struct got_error *err = NULL;
	struct tog_log_view_state *s = &view->state.log;
	struct commit_queue_entry *entry;

	/* Display progress update in log view. */
	show_log_view(view);
	update_panels();
	doupdate();

	if (s->search_entry) {
		int errcode, ch;
		errcode = pthread_mutex_unlock(&tog_mutex);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_mutex_unlock");
		ch = wgetch(view->window);
		errcode = pthread_mutex_lock(&tog_mutex);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_mutex_lock");
		if (ch == KEY_BACKSPACE) {
			view->search_next_done = TOG_SEARCH_HAVE_MORE;
			return NULL;
		}
		if (view->searching == TOG_SEARCH_FORWARD)
			entry = TAILQ_NEXT(s->search_entry, entry);
		else
			entry = TAILQ_PREV(s->search_entry,
			    commit_queue_head, entry);
	} else if (s->matched_entry) {
		int matched_idx = s->matched_entry->idx;
		int selected_idx = s->selected_entry->idx;

		/*
		 * If the user has moved the cursor after we hit a match,
		 * the position from where we should continue searching
		 * might have changed.
		 */
		if (view->searching == TOG_SEARCH_FORWARD) {
			if (matched_idx > selected_idx)
				entry = TAILQ_NEXT(s->selected_entry, entry);
			else
				entry = TAILQ_NEXT(s->matched_entry, entry);
		} else {
			if (matched_idx < selected_idx)
				entry = TAILQ_PREV(s->selected_entry,
						commit_queue_head, entry);
			else
				entry = TAILQ_PREV(s->matched_entry,
						commit_queue_head, entry);
		}
	} else {
		entry = s->selected_entry;
	}

	while (1) {
		int have_match = 0;

		if (entry == NULL) {
			if (s->thread_args.log_complete ||
			    view->searching == TOG_SEARCH_BACKWARD) {
				view->search_next_done =
				    (s->matched_entry == NULL ?
				    TOG_SEARCH_HAVE_NONE : TOG_SEARCH_NO_MORE);
				s->search_entry = NULL;
				return NULL;
			}
			/*
			 * Poke the log thread for more commits and return,
			 * allowing the main loop to make progress. Search
			 * will resume at s->search_entry once we come back.
			 */
			s->thread_args.commits_needed++;
			return trigger_log_thread(view, 0);
		}

		err = match_commit(&have_match, entry->id, entry->commit,
		    &view->regex);
		if (err)
			break;
		if (have_match) {
			view->search_next_done = TOG_SEARCH_HAVE_MORE;
			s->matched_entry = entry;
			break;
		}

		s->search_entry = entry;
		if (view->searching == TOG_SEARCH_FORWARD)
			entry = TAILQ_NEXT(entry, entry);
		else
			entry = TAILQ_PREV(entry, commit_queue_head, entry);
	}

	if (s->matched_entry) {
		int cur = s->selected_entry->idx;
		while (cur < s->matched_entry->idx) {
			err = input_log_view(NULL, view, KEY_DOWN);
			if (err)
				return err;
			cur++;
		}
		while (cur > s->matched_entry->idx) {
			err = input_log_view(NULL, view, KEY_UP);
			if (err)
				return err;
			cur--;
		}
	}

	s->search_entry = NULL;

	return NULL;
}

static const struct got_error *
open_log_view(struct tog_view *view, struct got_object_id *start_id,
    struct got_repository *repo, const char *head_ref_name,
    const char *in_repo_path, int log_branches)
{
	const struct got_error *err = NULL;
	struct tog_log_view_state *s = &view->state.log;
	struct got_repository *thread_repo = NULL;
	struct got_commit_graph *thread_graph = NULL;
	int errcode;

	if (in_repo_path != s->in_repo_path) {
		free(s->in_repo_path);
		s->in_repo_path = strdup(in_repo_path);
		if (s->in_repo_path == NULL)
			return got_error_from_errno("strdup");
	}

	/* The commit queue only contains commits being displayed. */
	TAILQ_INIT(&s->commits.head);
	s->commits.ncommits = 0;

	s->repo = repo;
	if (head_ref_name) {
		s->head_ref_name = strdup(head_ref_name);
		if (s->head_ref_name == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	}
	s->start_id = got_object_id_dup(start_id);
	if (s->start_id == NULL) {
		err = got_error_from_errno("got_object_id_dup");
		goto done;
	}
	s->log_branches = log_branches;

	STAILQ_INIT(&s->colors);
	if (has_colors() && getenv("TOG_COLORS") != NULL) {
		err = add_color(&s->colors, "^$", TOG_COLOR_COMMIT,
		    get_color_value("TOG_COLOR_COMMIT"));
		if (err)
			goto done;
		err = add_color(&s->colors, "^$", TOG_COLOR_AUTHOR,
		    get_color_value("TOG_COLOR_AUTHOR"));
		if (err) {
			free_colors(&s->colors);
			goto done;
		}
		err = add_color(&s->colors, "^$", TOG_COLOR_DATE,
		    get_color_value("TOG_COLOR_DATE"));
		if (err) {
			free_colors(&s->colors);
			goto done;
		}
	}

	view->show = show_log_view;
	view->input = input_log_view;
	view->close = close_log_view;
	view->search_start = search_start_log_view;
	view->search_next = search_next_log_view;

	if (s->thread_args.pack_fds == NULL) {
		err = got_repo_pack_fds_open(&s->thread_args.pack_fds);
		if (err)
			goto done;
	}
	err = got_repo_open(&thread_repo, got_repo_get_path(repo), NULL,
	    s->thread_args.pack_fds);
	if (err)
		goto done;
	err = got_commit_graph_open(&thread_graph, s->in_repo_path,
	    !s->log_branches);
	if (err)
		goto done;
	err = got_commit_graph_iter_start(thread_graph, s->start_id,
	    s->repo, NULL, NULL);
	if (err)
		goto done;

	errcode = pthread_cond_init(&s->thread_args.need_commits, NULL);
	if (errcode) {
		err = got_error_set_errno(errcode, "pthread_cond_init");
		goto done;
	}
	errcode = pthread_cond_init(&s->thread_args.commit_loaded, NULL);
	if (errcode) {
		err = got_error_set_errno(errcode, "pthread_cond_init");
		goto done;
	}

	s->thread_args.commits_needed = view->nlines;
	s->thread_args.graph = thread_graph;
	s->thread_args.commits = &s->commits;
	s->thread_args.in_repo_path = s->in_repo_path;
	s->thread_args.start_id = s->start_id;
	s->thread_args.repo = thread_repo;
	s->thread_args.log_complete = 0;
	s->thread_args.quit = &s->quit;
	s->thread_args.first_displayed_entry = &s->first_displayed_entry;
	s->thread_args.selected_entry = &s->selected_entry;
	s->thread_args.searching = &view->searching;
	s->thread_args.search_next_done = &view->search_next_done;
	s->thread_args.regex = &view->regex;
done:
	if (err)
		close_log_view(view);
	return err;
}

static const struct got_error *
show_log_view(struct tog_view *view)
{
	const struct got_error *err;
	struct tog_log_view_state *s = &view->state.log;

	if (s->thread == NULL) {
		int errcode = pthread_create(&s->thread, NULL, log_thread,
		    &s->thread_args);
		if (errcode)
			return got_error_set_errno(errcode, "pthread_create");
		if (s->thread_args.commits_needed > 0) {
			err = trigger_log_thread(view, 1);
			if (err)
				return err;
		}
	}

	return draw_commits(view);
}

static const struct got_error *
input_log_view(struct tog_view **new_view, struct tog_view *view, int ch)
{
	const struct got_error *err = NULL;
	struct tog_log_view_state *s = &view->state.log;
	struct tog_view *diff_view = NULL, *tree_view = NULL;
	struct tog_view *ref_view = NULL;
	struct commit_queue_entry *entry;
	int begin_x = 0, n, nscroll = view->nlines - 1;

	if (s->thread_args.load_all) {
		if (ch == KEY_BACKSPACE)
			s->thread_args.load_all = 0;
		else if (s->thread_args.log_complete) {
			s->thread_args.load_all = 0;
			log_scroll_down(view, s->commits.ncommits);
			s->selected = MIN(view->nlines - 2,
			    s->commits.ncommits - 1);
			select_commit(s);
		}
		return NULL;
	}

	switch (ch) {
	case 'q':
		s->quit = 1;
		break;
	case '0':
		view->x = 0;
		break;
	case '$':
		view->x = MAX(view->maxx - view->ncols / 2, 0);
		view->count = 0;
		break;
	case KEY_RIGHT:
	case 'l':
		if (view->x + view->ncols / 2 < view->maxx)
			view->x += 2;  /* move two columns right */
		else
			view->count = 0;
		break;
	case KEY_LEFT:
	case 'h':
		view->x -= MIN(view->x, 2);  /* move two columns back */
		if (view->x <= 0)
			view->count = 0;
		break;
	case 'k':
	case KEY_UP:
	case '<':
	case ',':
	case CTRL('p'):
		if (s->selected_entry->idx == 0)
			view->count = 0;
		if (s->first_displayed_entry == NULL)
			break;
		if (s->selected > 0)
			s->selected--;
		else
			log_scroll_up(s, 1);
		select_commit(s);
		break;
	case 'g':
	case KEY_HOME:
		s->selected = 0;
		s->first_displayed_entry = TAILQ_FIRST(&s->commits.head);
		select_commit(s);
		view->count = 0;
		break;
	case CTRL('u'):
	case 'u':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
	case 'b':
		if (s->first_displayed_entry == NULL)
			break;
		if (TAILQ_FIRST(&s->commits.head) == s->first_displayed_entry)
			s->selected = MAX(0, s->selected - nscroll - 1);
		else
			log_scroll_up(s, nscroll);
		select_commit(s);
		if (s->selected_entry->idx == 0)
			view->count = 0;
		break;
	case 'j':
	case KEY_DOWN:
	case '>':
	case '.':
	case CTRL('n'):
		if (s->first_displayed_entry == NULL)
			break;
		if (s->selected < MIN(view->nlines - 2,
		    s->commits.ncommits - 1))
			s->selected++;
		else {
			err = log_scroll_down(view, 1);
			if (err)
				break;
		}
		select_commit(s);
		if (s->thread_args.log_complete &&
		    s->selected_entry->idx == s->commits.ncommits - 1)
			view->count = 0;
		break;
	case 'G':
	case KEY_END: {
		/* We don't know yet how many commits, so we're forced to
		 * traverse them all. */
		view->count = 0;
		if (!s->thread_args.log_complete) {
			s->thread_args.load_all = 1;
			return trigger_log_thread(view, 0);
		}

		s->selected = 0;
		entry = TAILQ_LAST(&s->commits.head, commit_queue_head);
		for (n = 0; n < view->nlines - 1; n++) {
			if (entry == NULL)
				break;
			s->first_displayed_entry = entry;
			entry = TAILQ_PREV(entry, commit_queue_head, entry);
		}
		if (n > 0)
			s->selected = n - 1;
		select_commit(s);
		break;
	}
	case CTRL('d'):
	case 'd':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'):
	case 'f':
	case ' ': {
		struct commit_queue_entry *first;
		first = s->first_displayed_entry;
		if (first == NULL) {
			view->count = 0;
			break;
		}
		err = log_scroll_down(view, nscroll);
		if (err)
			break;
		if (first == s->first_displayed_entry &&
		    s->selected < MIN(view->nlines - 2,
		    s->commits.ncommits - 1)) {
			/* can't scroll further down */
			s->selected += MIN(s->last_displayed_entry->idx -
			    s->selected_entry->idx, nscroll + 1);
		}
		select_commit(s);
		if (s->thread_args.log_complete &&
		    s->selected_entry->idx == s->commits.ncommits - 1)
			view->count = 0;
		break;
	}
	case KEY_RESIZE:
		if (s->selected > view->nlines - 2)
			s->selected = view->nlines - 2;
		if (s->selected > s->commits.ncommits - 1)
			s->selected = s->commits.ncommits - 1;
		select_commit(s);
		if (s->commits.ncommits < view->nlines - 1 &&
		    !s->thread_args.log_complete) {
			s->thread_args.commits_needed += (view->nlines - 1) -
			    s->commits.ncommits;
			err = trigger_log_thread(view, 1);
		}
		break;
	case KEY_ENTER:
	case '\r':
		view->count = 0;
		if (s->selected_entry == NULL)
			break;
		if (view_is_parent_view(view))
			begin_x = view_split_begin_x(view->begin_x);
		err = open_diff_view_for_commit(&diff_view, begin_x,
		    s->selected_entry->commit, s->selected_entry->id,
		    view, s->repo);
		if (err)
			break;
		view->focussed = 0;
		diff_view->focussed = 1;
		if (view_is_parent_view(view)) {
			err = view_close_child(view);
			if (err)
				return err;
			err = view_set_child(view, diff_view);
			if (err)
				return err;
			view->focus_child = 1;
		} else
			*new_view = diff_view;
		break;
	case 't':
		view->count = 0;
		if (s->selected_entry == NULL)
			break;
		if (view_is_parent_view(view))
			begin_x = view_split_begin_x(view->begin_x);
		err = browse_commit_tree(&tree_view, begin_x,
		    s->selected_entry, s->in_repo_path, s->head_ref_name,
		    s->repo);
		if (err)
			break;
		view->focussed = 0;
		tree_view->focussed = 1;
		if (view_is_parent_view(view)) {
			err = view_close_child(view);
			if (err)
				return err;
			err = view_set_child(view, tree_view);
			if (err)
				return err;
			view->focus_child = 1;
		} else
			*new_view = tree_view;
		break;
	case KEY_BACKSPACE:
	case CTRL('l'):
	case 'B':
		view->count = 0;
		if (ch == KEY_BACKSPACE &&
		    got_path_is_root_dir(s->in_repo_path))
			break;
		err = stop_log_thread(s);
		if (err)
			return err;
		if (ch == KEY_BACKSPACE) {
			char *parent_path;
			err = got_path_dirname(&parent_path, s->in_repo_path);
			if (err)
				return err;
			free(s->in_repo_path);
			s->in_repo_path = parent_path;
			s->thread_args.in_repo_path = s->in_repo_path;
		} else if (ch == CTRL('l')) {
			struct got_object_id *start_id;
			err = got_repo_match_object_id(&start_id, NULL,
			    s->head_ref_name ? s->head_ref_name : GOT_REF_HEAD,
			    GOT_OBJ_TYPE_COMMIT, &tog_refs, s->repo);
			if (err)
				return err;
			free(s->start_id);
			s->start_id = start_id;
			s->thread_args.start_id = s->start_id;
		} else /* 'B' */
			s->log_branches = !s->log_branches;

		if (s->thread_args.pack_fds == NULL) {
			err = got_repo_pack_fds_open(&s->thread_args.pack_fds);
			if (err)
				return err;
		}
		err = got_repo_open(&s->thread_args.repo,
		    got_repo_get_path(s->repo), NULL,
		    s->thread_args.pack_fds);
		if (err)
			return err;
		tog_free_refs();
		err = tog_load_refs(s->repo, 0);
		if (err)
			return err;
		err = got_commit_graph_open(&s->thread_args.graph,
		    s->in_repo_path, !s->log_branches);
		if (err)
			return err;
		err = got_commit_graph_iter_start(s->thread_args.graph,
		    s->start_id, s->repo, NULL, NULL);
		if (err)
			return err;
		free_commits(&s->commits);
		s->first_displayed_entry = NULL;
		s->last_displayed_entry = NULL;
		s->selected_entry = NULL;
		s->selected = 0;
		s->thread_args.log_complete = 0;
		s->quit = 0;
		s->thread_args.commits_needed = view->nlines;
		s->matched_entry = NULL;
		s->search_entry = NULL;
		break;
	case 'r':
		view->count = 0;
		if (view_is_parent_view(view))
			begin_x = view_split_begin_x(view->begin_x);
		ref_view = view_open(view->nlines, view->ncols,
		    view->begin_y, begin_x, TOG_VIEW_REF);
		if (ref_view == NULL)
			return got_error_from_errno("view_open");
		err = open_ref_view(ref_view, s->repo);
		if (err) {
			view_close(ref_view);
			return err;
		}
		view->focussed = 0;
		ref_view->focussed = 1;
		if (view_is_parent_view(view)) {
			err = view_close_child(view);
			if (err)
				return err;
			err = view_set_child(view, ref_view);
			if (err)
				return err;
			view->focus_child = 1;
		} else
			*new_view = ref_view;
		break;
	default:
		view->count = 0;
		break;
	}

	return err;
}

static const struct got_error *
apply_unveil(const char *repo_path, const char *worktree_path)
{
	const struct got_error *error;

#ifdef PROFILE
	if (unveil("gmon.out", "rwc") != 0)
		return got_error_from_errno2("unveil", "gmon.out");
#endif
	if (repo_path && unveil(repo_path, "r") != 0)
		return got_error_from_errno2("unveil", repo_path);

	if (worktree_path && unveil(worktree_path, "rwc") != 0)
		return got_error_from_errno2("unveil", worktree_path);

	if (unveil(GOT_TMPDIR_STR, "rwc") != 0)
		return got_error_from_errno2("unveil", GOT_TMPDIR_STR);

	error = got_privsep_unveil_exec_helpers();
	if (error != NULL)
		return error;

	if (unveil(NULL, NULL) != 0)
		return got_error_from_errno("unveil");

	return NULL;
}

static void
init_curses(void)
{
	/*
	 * Override default signal handlers before starting ncurses.
	 * This should prevent ncurses from installing its own
	 * broken cleanup() signal handler.
	 */
	signal(SIGWINCH, tog_sigwinch);
	signal(SIGPIPE, tog_sigpipe);
	signal(SIGCONT, tog_sigcont);
	signal(SIGINT, tog_sigint);
	signal(SIGTERM, tog_sigterm);

	initscr();
	cbreak();
	halfdelay(1); /* Do fast refresh while initial view is loading. */
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);
	curs_set(0);
	if (getenv("TOG_COLORS") != NULL) {
		start_color();
		use_default_colors();
	}
}

static const struct got_error *
get_in_repo_path_from_argv0(char **in_repo_path, int argc, char *argv[],
    struct got_repository *repo, struct got_worktree *worktree)
{
	const struct got_error *err = NULL;

	if (argc == 0) {
		*in_repo_path = strdup("/");
		if (*in_repo_path == NULL)
			return got_error_from_errno("strdup");
		return NULL;
	}

	if (worktree) {
		const char *prefix = got_worktree_get_path_prefix(worktree);
		char *p;

		err = got_worktree_resolve_path(&p, worktree, argv[0]);
		if (err)
			return err;
		if (asprintf(in_repo_path, "%s%s%s", prefix,
		    (p[0] != '\0' && !got_path_is_root_dir(prefix)) ? "/" : "",
		    p) == -1) {
			err = got_error_from_errno("asprintf");
			*in_repo_path = NULL;
		}
		free(p);
	} else
		err = got_repo_map_path(in_repo_path, repo, argv[0]);

	return err;
}

static const struct got_error *
cmd_log(int argc, char *argv[])
{
	const struct got_error *error;
	struct got_repository *repo = NULL;
	struct got_worktree *worktree = NULL;
	struct got_object_id *start_id = NULL;
	char *in_repo_path = NULL, *repo_path = NULL, *cwd = NULL;
	char *start_commit = NULL, *label = NULL;
	struct got_reference *ref = NULL;
	const char *head_ref_name = NULL;
	int ch, log_branches = 0;
	struct tog_view *view;
	int *pack_fds = NULL;

	while ((ch = getopt(argc, argv, "bc:r:")) != -1) {
		switch (ch) {
		case 'b':
			log_branches = 1;
			break;
		case 'c':
			start_commit = optarg;
			break;
		case 'r':
			repo_path = realpath(optarg, NULL);
			if (repo_path == NULL)
				return got_error_from_errno2("realpath",
				    optarg);
			break;
		default:
			usage_log();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 1)
		usage_log();

	error = got_repo_pack_fds_open(&pack_fds);
	if (error != NULL)
		goto done;

	if (repo_path == NULL) {
		cwd = getcwd(NULL, 0);
		if (cwd == NULL)
			return got_error_from_errno("getcwd");
		error = got_worktree_open(&worktree, cwd);
		if (error && error->code != GOT_ERR_NOT_WORKTREE)
			goto done;
		if (worktree)
			repo_path =
			    strdup(got_worktree_get_repo_path(worktree));
		else
			repo_path = strdup(cwd);
		if (repo_path == NULL) {
			error = got_error_from_errno("strdup");
			goto done;
		}
	}

	error = got_repo_open(&repo, repo_path, NULL, pack_fds);
	if (error != NULL)
		goto done;

	error = get_in_repo_path_from_argv0(&in_repo_path, argc, argv,
	    repo, worktree);
	if (error)
		goto done;

	init_curses();

	error = apply_unveil(got_repo_get_path(repo),
	    worktree ? got_worktree_get_root_path(worktree) : NULL);
	if (error)
		goto done;

	/* already loaded by tog_log_with_path()? */
	if (TAILQ_EMPTY(&tog_refs)) {
		error = tog_load_refs(repo, 0);
		if (error)
			goto done;
	}

	if (start_commit == NULL) {
		error = got_repo_match_object_id(&start_id, &label,
		    worktree ? got_worktree_get_head_ref_name(worktree) :
		    GOT_REF_HEAD, GOT_OBJ_TYPE_COMMIT, &tog_refs, repo);
		if (error)
			goto done;
		head_ref_name = label;
	} else {
		error = got_ref_open(&ref, repo, start_commit, 0);
		if (error == NULL)
			head_ref_name = got_ref_get_name(ref);
		else if (error->code != GOT_ERR_NOT_REF)
			goto done;
		error = got_repo_match_object_id(&start_id, NULL,
		    start_commit, GOT_OBJ_TYPE_COMMIT, &tog_refs, repo);
		if (error)
			goto done;
	}

	view = view_open(0, 0, 0, 0, TOG_VIEW_LOG);
	if (view == NULL) {
		error = got_error_from_errno("view_open");
		goto done;
	}
	error = open_log_view(view, start_id, repo, head_ref_name,
	    in_repo_path, log_branches);
	if (error)
		goto done;
	if (worktree) {
		/* Release work tree lock. */
		got_worktree_close(worktree);
		worktree = NULL;
	}
	error = view_loop(view);
done:
	free(in_repo_path);
	free(repo_path);
	free(cwd);
	free(start_id);
	free(label);
	if (ref)
		got_ref_close(ref);
	if (repo) {
		const struct got_error *close_err = got_repo_close(repo);
		if (error == NULL)
			error = close_err;
	}
	if (worktree)
		got_worktree_close(worktree);
	if (pack_fds) {
		const struct got_error *pack_err =
		    got_repo_pack_fds_close(pack_fds);
		if (error == NULL)
			error = pack_err;
	}
	tog_free_refs();
	return error;
}

__dead static void
usage_diff(void)
{
	endwin();
	fprintf(stderr, "usage: %s diff [-a] [-C number] [-r repository-path] "
	    "[-w] object1 object2\n", getprogname());
	exit(1);
}

static int
match_line(const char *line, regex_t *regex, size_t nmatch,
    regmatch_t *regmatch)
{
	return regexec(regex, line, nmatch, regmatch, 0) == 0;
}

static struct tog_color *
match_color(struct tog_colors *colors, const char *line)
{
	struct tog_color *tc = NULL;

	STAILQ_FOREACH(tc, colors, entry) {
		if (match_line(line, &tc->regex, 0, NULL))
			return tc;
	}

	return NULL;
}

static const struct got_error *
add_matched_line(int *wtotal, const char *line, int wlimit, int col_tab_align,
    WINDOW *window, int skipcol, regmatch_t *regmatch)
{
	const struct got_error *err = NULL;
	char *exstr = NULL;
	wchar_t *wline = NULL;
	int rme, rms, n, width, scrollx;
	int width0 = 0, width1 = 0, width2 = 0;
	char *seg0 = NULL, *seg1 = NULL, *seg2 = NULL;

	*wtotal = 0;

	rms = regmatch->rm_so;
	rme = regmatch->rm_eo;

	err = expand_tab(&exstr, line);
	if (err)
		return err;

	/* Split the line into 3 segments, according to match offsets. */
	seg0 = strndup(exstr, rms);
	if (seg0 == NULL) {
		err = got_error_from_errno("strndup");
		goto done;
	}
	seg1 = strndup(exstr + rms, rme - rms);
	if (seg1 == NULL) {
		err = got_error_from_errno("strndup");
		goto done;
	}
	seg2 = strdup(exstr + rme);
	if (seg2 == NULL) {
		err = got_error_from_errno("strndup");
		goto done;
	}

	/* draw up to matched token if we haven't scrolled past it */
	err = format_line(&wline, &width0, NULL, seg0, 0, wlimit,
	    col_tab_align, 1);
	if (err)
		goto done;
	n = MAX(width0 - skipcol, 0);
	if (n) {
		free(wline);
		err = format_line(&wline, &width, &scrollx, seg0, skipcol,
		    wlimit, col_tab_align, 1);
		if (err)
			goto done;
		waddwstr(window, &wline[scrollx]);
		wlimit -= width;
		*wtotal += width;
	}

	if (wlimit > 0) {
		int i = 0, w = 0;
		size_t wlen;

		free(wline);
		err = format_line(&wline, &width1, NULL, seg1, 0, wlimit,
		    col_tab_align, 1);
		if (err)
			goto done;
		wlen = wcslen(wline);
		while (i < wlen) {
			width = wcwidth(wline[i]);
			if (width == -1) {
				/* should not happen, tabs are expanded */
				err = got_error(GOT_ERR_RANGE);
				goto done;
			}
			if (width0 + w + width > skipcol)
				break;
			w += width;
			i++;
		}
		/* draw (visible part of) matched token (if scrolled into it) */
		if (width1 - w > 0) {
			wattron(window, A_STANDOUT);
			waddwstr(window, &wline[i]);
			wattroff(window, A_STANDOUT);
			wlimit -= (width1 - w);
			*wtotal += (width1 - w);
		}
	}

	if (wlimit > 0) {  /* draw rest of line */
		free(wline);
		if (skipcol > width0 + width1) {
			err = format_line(&wline, &width2, &scrollx, seg2,
			    skipcol - (width0 + width1), wlimit,
			    col_tab_align, 1);
			if (err)
				goto done;
			waddwstr(window, &wline[scrollx]);
		} else {
			err = format_line(&wline, &width2, NULL, seg2, 0,
			    wlimit, col_tab_align, 1);
			if (err)
				goto done;
			waddwstr(window, wline);
		}
		*wtotal += width2;
	}
done:
	free(wline);
	free(exstr);
	free(seg0);
	free(seg1);
	free(seg2);
	return err;
}

static const struct got_error *
draw_file(struct tog_view *view, const char *header)
{
	struct tog_diff_view_state *s = &view->state.diff;
	regmatch_t *regmatch = &view->regmatch;
	const struct got_error *err;
	int nprinted = 0;
	char *line;
	size_t linesize = 0;
	ssize_t linelen;
	struct tog_color *tc;
	wchar_t *wline;
	int width;
	int max_lines = view->nlines;
	int nlines = s->nlines;
	off_t line_offset;

	line_offset = s->line_offsets[s->first_displayed_line - 1];
	if (fseeko(s->f, line_offset, SEEK_SET) == -1)
		return got_error_from_errno("fseek");

	werase(view->window);

	if (header) {
		if (asprintf(&line, "[%d/%d] %s",
		    s->first_displayed_line - 1 + s->selected_line, nlines,
		    header) == -1)
			return got_error_from_errno("asprintf");
		err = format_line(&wline, &width, NULL, line, 0, view->ncols,
		    0, 0);
		free(line);
		if (err)
			return err;

		if (view_needs_focus_indication(view))
			wstandout(view->window);
		waddwstr(view->window, wline);
		free(wline);
		wline = NULL;
		if (view_needs_focus_indication(view))
			wstandend(view->window);
		if (width <= view->ncols - 1)
			waddch(view->window, '\n');

		if (max_lines <= 1)
			return NULL;
		max_lines--;
	}

	s->eof = 0;
	view->maxx = 0;
	line = NULL;
	while (max_lines > 0 && nprinted < max_lines) {
		linelen = getline(&line, &linesize, s->f);
		if (linelen == -1) {
			if (feof(s->f)) {
				s->eof = 1;
				break;
			}
			free(line);
			return got_ferror(s->f, GOT_ERR_IO);
		}

		/* Set view->maxx based on full line length. */
		err = format_line(&wline, &width, NULL, line, 0, INT_MAX, 0,
		    view->x ? 1 : 0);
		if (err) {
			free(line);
			return err;
		}
		view->maxx = MAX(view->maxx, width);
		free(wline);
		wline = NULL;

		tc = match_color(&s->colors, line);
		if (tc)
			wattr_on(view->window,
			    COLOR_PAIR(tc->colorpair), NULL);
		if (s->first_displayed_line + nprinted == s->matched_line &&
		    regmatch->rm_so >= 0 && regmatch->rm_so < regmatch->rm_eo) {
			err = add_matched_line(&width, line, view->ncols, 0,
			    view->window, view->x, regmatch);
			if (err) {
				free(line);
				return err;
			}
		} else {
			int skip;
			err = format_line(&wline, &width, &skip, line,
			    view->x, view->ncols, 0, view->x ? 1 : 0);
			if (err) {
				free(line);
				return err;
			}
			waddwstr(view->window, &wline[skip]);
			free(wline);
			wline = NULL;
		}
		if (tc)
			wattr_off(view->window,
			    COLOR_PAIR(tc->colorpair), NULL);
		if (width <= view->ncols - 1)
			waddch(view->window, '\n');
		nprinted++;
	}
	free(line);
	if (nprinted >= 1)
		s->last_displayed_line = s->first_displayed_line +
		    (nprinted - 1);
	else
		s->last_displayed_line = s->first_displayed_line;

	view_vborder(view);

	if (s->eof) {
		while (nprinted < view->nlines) {
			waddch(view->window, '\n');
			nprinted++;
		}

		err = format_line(&wline, &width, NULL, TOG_EOF_STRING, 0,
		    view->ncols, 0, 0);
		if (err) {
			return err;
		}

		wstandout(view->window);
		waddwstr(view->window, wline);
		free(wline);
		wline = NULL;
		wstandend(view->window);
	}

	return NULL;
}

static char *
get_datestr(time_t *time, char *datebuf)
{
	struct tm mytm, *tm;
	char *p, *s;

	tm = gmtime_r(time, &mytm);
	if (tm == NULL)
		return NULL;
	s = asctime_r(tm, datebuf);
	if (s == NULL)
		return NULL;
	p = strchr(s, '\n');
	if (p)
		*p = '\0';
	return s;
}

static const struct got_error *
get_changed_paths(struct got_pathlist_head *paths,
    struct got_commit_object *commit, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object_id *tree_id1 = NULL, *tree_id2 = NULL;
	struct got_tree_object *tree1 = NULL, *tree2 = NULL;
	struct got_object_qid *qid;

	qid = STAILQ_FIRST(got_object_commit_get_parent_ids(commit));
	if (qid != NULL) {
		struct got_commit_object *pcommit;
		err = got_object_open_as_commit(&pcommit, repo,
		    &qid->id);
		if (err)
			return err;

		tree_id1 = got_object_id_dup(
		    got_object_commit_get_tree_id(pcommit));
		if (tree_id1 == NULL) {
			got_object_commit_close(pcommit);
			return got_error_from_errno("got_object_id_dup");
		}
		got_object_commit_close(pcommit);

	}

	if (tree_id1) {
		err = got_object_open_as_tree(&tree1, repo, tree_id1);
		if (err)
			goto done;
	}

	tree_id2 = got_object_commit_get_tree_id(commit);
	err = got_object_open_as_tree(&tree2, repo, tree_id2);
	if (err)
		goto done;

	err = got_diff_tree(tree1, tree2, NULL, NULL, -1, -1, "", "", repo,
	    got_diff_tree_collect_changed_paths, paths, 0);
done:
	if (tree1)
		got_object_tree_close(tree1);
	if (tree2)
		got_object_tree_close(tree2);
	free(tree_id1);
	return err;
}

static const struct got_error *
add_line_offset(off_t **line_offsets, size_t *nlines, off_t off)
{
	off_t *p;

	p = reallocarray(*line_offsets, *nlines + 1, sizeof(off_t));
	if (p == NULL)
		return got_error_from_errno("reallocarray");
	*line_offsets = p;
	(*line_offsets)[*nlines] = off;
	(*nlines)++;
	return NULL;
}

static const struct got_error *
write_commit_info(off_t **line_offsets, size_t *nlines,
    struct got_object_id *commit_id, struct got_reflist_head *refs,
    struct got_repository *repo, FILE *outfile)
{
	const struct got_error *err = NULL;
	char datebuf[26], *datestr;
	struct got_commit_object *commit;
	char *id_str = NULL, *logmsg = NULL, *s = NULL, *line;
	time_t committer_time;
	const char *author, *committer;
	char *refs_str = NULL;
	struct got_pathlist_head changed_paths;
	struct got_pathlist_entry *pe;
	off_t outoff = 0;
	int n;

	TAILQ_INIT(&changed_paths);

	if (refs) {
		err = build_refs_str(&refs_str, refs, commit_id, repo);
		if (err)
			return err;
	}

	err = got_object_open_as_commit(&commit, repo, commit_id);
	if (err)
		return err;

	err = got_object_id_str(&id_str, commit_id);
	if (err) {
		err = got_error_from_errno("got_object_id_str");
		goto done;
	}

	err = add_line_offset(line_offsets, nlines, 0);
	if (err)
		goto done;

	n = fprintf(outfile, "commit %s%s%s%s\n", id_str, refs_str ? " (" : "",
	    refs_str ? refs_str : "", refs_str ? ")" : "");
	if (n < 0) {
		err = got_error_from_errno("fprintf");
		goto done;
	}
	outoff += n;
	err = add_line_offset(line_offsets, nlines, outoff);
	if (err)
		goto done;

	n = fprintf(outfile, "from: %s\n",
	    got_object_commit_get_author(commit));
	if (n < 0) {
		err = got_error_from_errno("fprintf");
		goto done;
	}
	outoff += n;
	err = add_line_offset(line_offsets, nlines, outoff);
	if (err)
		goto done;

	committer_time = got_object_commit_get_committer_time(commit);
	datestr = get_datestr(&committer_time, datebuf);
	if (datestr) {
		n = fprintf(outfile, "date: %s UTC\n", datestr);
		if (n < 0) {
			err = got_error_from_errno("fprintf");
			goto done;
		}
		outoff += n;
		err = add_line_offset(line_offsets, nlines, outoff);
		if (err)
			goto done;
	}
	author = got_object_commit_get_author(commit);
	committer = got_object_commit_get_committer(commit);
	if (strcmp(author, committer) != 0) {
		n = fprintf(outfile, "via: %s\n", committer);
		if (n < 0) {
			err = got_error_from_errno("fprintf");
			goto done;
		}
		outoff += n;
		err = add_line_offset(line_offsets, nlines, outoff);
		if (err)
			goto done;
	}
	if (got_object_commit_get_nparents(commit) > 1) {
		const struct got_object_id_queue *parent_ids;
		struct got_object_qid *qid;
		int pn = 1;
		parent_ids = got_object_commit_get_parent_ids(commit);
		STAILQ_FOREACH(qid, parent_ids, entry) {
			err = got_object_id_str(&id_str, &qid->id);
			if (err)
				goto done;
			n = fprintf(outfile, "parent %d: %s\n", pn++, id_str);
			if (n < 0) {
				err = got_error_from_errno("fprintf");
				goto done;
			}
			outoff += n;
			err = add_line_offset(line_offsets, nlines, outoff);
			if (err)
				goto done;
			free(id_str);
			id_str = NULL;
		}
	}

	err = got_object_commit_get_logmsg(&logmsg, commit);
	if (err)
		goto done;
	s = logmsg;
	while ((line = strsep(&s, "\n")) != NULL) {
		n = fprintf(outfile, "%s\n", line);
		if (n < 0) {
			err = got_error_from_errno("fprintf");
			goto done;
		}
		outoff += n;
		err = add_line_offset(line_offsets, nlines, outoff);
		if (err)
			goto done;
	}

	err = get_changed_paths(&changed_paths, commit, repo);
	if (err)
		goto done;
	TAILQ_FOREACH(pe, &changed_paths, entry) {
		struct got_diff_changed_path *cp = pe->data;
		n = fprintf(outfile, "%c  %s\n", cp->status, pe->path);
		if (n < 0) {
			err = got_error_from_errno("fprintf");
			goto done;
		}
		outoff += n;
		err = add_line_offset(line_offsets, nlines, outoff);
		if (err)
			goto done;
		free((char *)pe->path);
		free(pe->data);
	}

	fputc('\n', outfile);
	outoff++;
	err = add_line_offset(line_offsets, nlines, outoff);
done:
	got_pathlist_free(&changed_paths);
	free(id_str);
	free(logmsg);
	free(refs_str);
	got_object_commit_close(commit);
	if (err) {
		free(*line_offsets);
		*line_offsets = NULL;
		*nlines = 0;
	}
	return err;
}

static const struct got_error *
create_diff(struct tog_diff_view_state *s)
{
	const struct got_error *err = NULL;
	FILE *f = NULL;
	int obj_type;

	free(s->line_offsets);
	s->line_offsets = malloc(sizeof(off_t));
	if (s->line_offsets == NULL)
		return got_error_from_errno("malloc");
	s->nlines = 0;

	f = got_opentemp();
	if (f == NULL) {
		err = got_error_from_errno("got_opentemp");
		goto done;
	}
	if (s->f && fclose(s->f) == EOF) {
		err = got_error_from_errno("fclose");
		goto done;
	}
	s->f = f;

	if (s->id1)
		err = got_object_get_type(&obj_type, s->repo, s->id1);
	else
		err = got_object_get_type(&obj_type, s->repo, s->id2);
	if (err)
		goto done;

	switch (obj_type) {
	case GOT_OBJ_TYPE_BLOB:
		err = got_diff_objects_as_blobs(&s->line_offsets, &s->nlines,
		    s->f1, s->f2, s->fd1, s->fd2, s->id1, s->id2,
		    s->label1, s->label2, s->diff_context,
		    s->ignore_whitespace, s->force_text_diff, s->repo, s->f);
		break;
	case GOT_OBJ_TYPE_TREE:
		err = got_diff_objects_as_trees(&s->line_offsets, &s->nlines,
		    s->f1, s->f2, s->fd1, s->fd2, s->id1, s->id2, NULL, "", "",
		    s->diff_context, s->ignore_whitespace, s->force_text_diff,
		    s->repo, s->f);
		break;
	case GOT_OBJ_TYPE_COMMIT: {
		const struct got_object_id_queue *parent_ids;
		struct got_object_qid *pid;
		struct got_commit_object *commit2;
		struct got_reflist_head *refs;

		err = got_object_open_as_commit(&commit2, s->repo, s->id2);
		if (err)
			goto done;
		refs = got_reflist_object_id_map_lookup(tog_refs_idmap, s->id2);
		/* Show commit info if we're diffing to a parent/root commit. */
		if (s->id1 == NULL) {
			err = write_commit_info(&s->line_offsets, &s->nlines,
			    s->id2, refs, s->repo, s->f);
			if (err)
				goto done;
		} else {
			parent_ids = got_object_commit_get_parent_ids(commit2);
			STAILQ_FOREACH(pid, parent_ids, entry) {
				if (got_object_id_cmp(s->id1, &pid->id) == 0) {
					err = write_commit_info(
					    &s->line_offsets, &s->nlines,
					    s->id2, refs, s->repo, s->f);
					if (err)
						goto done;
					break;
				}
			}
		}
		got_object_commit_close(commit2);

		err = got_diff_objects_as_commits(&s->line_offsets, &s->nlines,
		    s->f1, s->f2, s->fd1, s->fd2, s->id1, s->id2, NULL,
		    s->diff_context, s->ignore_whitespace, s->force_text_diff,
		    s->repo, s->f);
		break;
	}
	default:
		err = got_error(GOT_ERR_OBJ_TYPE);
		break;
	}
	if (err)
		goto done;
done:
	if (s->f && fflush(s->f) != 0 && err == NULL)
		err = got_error_from_errno("fflush");
	return err;
}

static void
diff_view_indicate_progress(struct tog_view *view)
{
	mvwaddstr(view->window, 0, 0, "diffing...");
	update_panels();
	doupdate();
}

static const struct got_error *
search_start_diff_view(struct tog_view *view)
{
	struct tog_diff_view_state *s = &view->state.diff;

	s->matched_line = 0;
	return NULL;
}

static const struct got_error *
search_next_diff_view(struct tog_view *view)
{
	struct tog_diff_view_state *s = &view->state.diff;
	const struct got_error *err = NULL;
	int lineno;
	char *line = NULL;
	size_t linesize = 0;
	ssize_t linelen;

	if (!view->searching) {
		view->search_next_done = TOG_SEARCH_HAVE_MORE;
		return NULL;
	}

	if (s->matched_line) {
		if (view->searching == TOG_SEARCH_FORWARD)
			lineno = s->matched_line + 1;
		else
			lineno = s->matched_line - 1;
	} else
		lineno = s->first_displayed_line;

	while (1) {
		off_t offset;

		if (lineno <= 0 || lineno > s->nlines) {
			if (s->matched_line == 0) {
				view->search_next_done = TOG_SEARCH_HAVE_MORE;
				break;
			}

			if (view->searching == TOG_SEARCH_FORWARD)
				lineno = 1;
			else
				lineno = s->nlines;
		}

		offset = s->line_offsets[lineno - 1];
		if (fseeko(s->f, offset, SEEK_SET) != 0) {
			free(line);
			return got_error_from_errno("fseeko");
		}
		linelen = getline(&line, &linesize, s->f);
		if (linelen != -1) {
			char *exstr;
			err = expand_tab(&exstr, line);
			if (err)
				break;
			if (match_line(exstr, &view->regex, 1,
			    &view->regmatch)) {
				view->search_next_done = TOG_SEARCH_HAVE_MORE;
				s->matched_line = lineno;
				free(exstr);
				break;
			}
			free(exstr);
		}
		if (view->searching == TOG_SEARCH_FORWARD)
			lineno++;
		else
			lineno--;
	}
	free(line);

	if (s->matched_line) {
		s->first_displayed_line = s->matched_line;
		s->selected_line = 1;
	}

	return err;
}

static const struct got_error *
close_diff_view(struct tog_view *view)
{
	const struct got_error *err = NULL;
	struct tog_diff_view_state *s = &view->state.diff;

	free(s->id1);
	s->id1 = NULL;
	free(s->id2);
	s->id2 = NULL;
	if (s->f && fclose(s->f) == EOF)
		err = got_error_from_errno("fclose");
	s->f = NULL;
	if (s->f1 && fclose(s->f1) == EOF && err == NULL)
		err = got_error_from_errno("fclose");
	s->f1 = NULL;
	if (s->f2 && fclose(s->f2) == EOF && err == NULL)
		err = got_error_from_errno("fclose");
	s->f2 = NULL;
	if (s->fd1 != -1 && close(s->fd1) == -1 && err == NULL)
		err = got_error_from_errno("close");
	s->fd1 = -1;
	if (s->fd2 != -1 && close(s->fd2) == -1 && err == NULL)
		err = got_error_from_errno("close");
	s->fd2 = -1;
	free_colors(&s->colors);
	free(s->line_offsets);
	s->line_offsets = NULL;
	s->nlines = 0;
	return err;
}

static const struct got_error *
open_diff_view(struct tog_view *view, struct got_object_id *id1,
    struct got_object_id *id2, const char *label1, const char *label2,
    int diff_context, int ignore_whitespace, int force_text_diff,
    struct tog_view *log_view, struct got_repository *repo)
{
	const struct got_error *err;
	struct tog_diff_view_state *s = &view->state.diff;

	memset(s, 0, sizeof(*s));
	s->fd1 = -1;
	s->fd2 = -1;

	if (id1 != NULL && id2 != NULL) {
	    int type1, type2;
	    err = got_object_get_type(&type1, repo, id1);
	    if (err)
		return err;
	    err = got_object_get_type(&type2, repo, id2);
	    if (err)
		return err;

	    if (type1 != type2)
		return got_error(GOT_ERR_OBJ_TYPE);
	}
	s->first_displayed_line = 1;
	s->last_displayed_line = view->nlines;
	s->selected_line = 1;
	s->repo = repo;
	s->id1 = id1;
	s->id2 = id2;
	s->label1 = label1;
	s->label2 = label2;

	if (id1) {
		s->id1 = got_object_id_dup(id1);
		if (s->id1 == NULL)
			return got_error_from_errno("got_object_id_dup");
	} else
		s->id1 = NULL;

	s->id2 = got_object_id_dup(id2);
	if (s->id2 == NULL) {
		err = got_error_from_errno("got_object_id_dup");
		goto done;
	}

	s->f1 = got_opentemp();
	if (s->f1 == NULL) {
		err = got_error_from_errno("got_opentemp");
		goto done;
	}

	s->f2 = got_opentemp();
	if (s->f2 == NULL) {
		err = got_error_from_errno("got_opentemp");
		goto done;
	}

	s->fd1 = got_opentempfd();
	if (s->fd1 == -1) {
		err = got_error_from_errno("got_opentempfd");
		goto done;
	}

	s->fd2 = got_opentempfd();
	if (s->fd2 == -1) {
		err = got_error_from_errno("got_opentempfd");
		goto done;
	}

	s->first_displayed_line = 1;
	s->last_displayed_line = view->nlines;
	s->diff_context = diff_context;
	s->ignore_whitespace = ignore_whitespace;
	s->force_text_diff = force_text_diff;
	s->log_view = log_view;
	s->repo = repo;

	STAILQ_INIT(&s->colors);
	if (has_colors() && getenv("TOG_COLORS") != NULL) {
		err = add_color(&s->colors,
		    "^-", TOG_COLOR_DIFF_MINUS,
		    get_color_value("TOG_COLOR_DIFF_MINUS"));
		if (err)
			goto done;
		err = add_color(&s->colors, "^\\+",
		    TOG_COLOR_DIFF_PLUS,
		    get_color_value("TOG_COLOR_DIFF_PLUS"));
		if (err)
			goto done;
		err = add_color(&s->colors,
		    "^@@", TOG_COLOR_DIFF_CHUNK_HEADER,
		    get_color_value("TOG_COLOR_DIFF_CHUNK_HEADER"));
		if (err)
			goto done;

		err = add_color(&s->colors,
		    "^(commit [0-9a-f]|parent [0-9]|"
		    "(blob|file|tree|commit) [-+] |"
		    "[MDmA]  [^ ])", TOG_COLOR_DIFF_META,
		    get_color_value("TOG_COLOR_DIFF_META"));
		if (err)
			goto done;

		err = add_color(&s->colors,
		    "^(from|via): ", TOG_COLOR_AUTHOR,
		    get_color_value("TOG_COLOR_AUTHOR"));
		if (err)
			goto done;

		err = add_color(&s->colors,
		    "^date: ", TOG_COLOR_DATE,
		    get_color_value("TOG_COLOR_DATE"));
		if (err)
			goto done;
	}

	if (log_view && view_is_splitscreen(view))
		show_log_view(log_view); /* draw vborder */
	diff_view_indicate_progress(view);

	err = create_diff(s);

	view->show = show_diff_view;
	view->input = input_diff_view;
	view->close = close_diff_view;
	view->search_start = search_start_diff_view;
	view->search_next = search_next_diff_view;
done:
	if (err)
		close_diff_view(view);
	return err;
}

static const struct got_error *
show_diff_view(struct tog_view *view)
{
	const struct got_error *err;
	struct tog_diff_view_state *s = &view->state.diff;
	char *id_str1 = NULL, *id_str2, *header;
	const char *label1, *label2;

	if (s->id1) {
		err = got_object_id_str(&id_str1, s->id1);
		if (err)
			return err;
		label1 = s->label1 ? : id_str1;
	} else
		label1 = "/dev/null";

	err = got_object_id_str(&id_str2, s->id2);
	if (err)
		return err;
	label2 = s->label2 ? : id_str2;

	if (asprintf(&header, "diff %s %s", label1, label2) == -1) {
		err = got_error_from_errno("asprintf");
		free(id_str1);
		free(id_str2);
		return err;
	}
	free(id_str1);
	free(id_str2);

	err = draw_file(view, header);
	free(header);
	return err;
}

static const struct got_error *
set_selected_commit(struct tog_diff_view_state *s,
    struct commit_queue_entry *entry)
{
	const struct got_error *err;
	const struct got_object_id_queue *parent_ids;
	struct got_commit_object *selected_commit;
	struct got_object_qid *pid;

	free(s->id2);
	s->id2 = got_object_id_dup(entry->id);
	if (s->id2 == NULL)
		return got_error_from_errno("got_object_id_dup");

	err = got_object_open_as_commit(&selected_commit, s->repo, entry->id);
	if (err)
		return err;
	parent_ids = got_object_commit_get_parent_ids(selected_commit);
	free(s->id1);
	pid = STAILQ_FIRST(parent_ids);
	s->id1 = pid ? got_object_id_dup(&pid->id) : NULL;
	got_object_commit_close(selected_commit);
	return NULL;
}

static const struct got_error *
input_diff_view(struct tog_view **new_view, struct tog_view *view, int ch)
{
	const struct got_error *err = NULL;
	struct tog_diff_view_state *s = &view->state.diff;
	struct tog_log_view_state *ls;
	struct commit_queue_entry *old_selected_entry;
	char *line = NULL;
	size_t linesize = 0;
	ssize_t linelen;
	int i, nscroll = view->nlines - 1;

	switch (ch) {
	case '0':
		view->x = 0;
		break;
	case '$':
		view->x = MAX(view->maxx - view->ncols / 3, 0);
		view->count = 0;
		break;
	case KEY_RIGHT:
	case 'l':
		if (view->x + view->ncols / 3 < view->maxx)
			view->x += 2;  /* move two columns right */
		else
			view->count = 0;
		break;
	case KEY_LEFT:
	case 'h':
		view->x -= MIN(view->x, 2);  /* move two columns back */
		if (view->x <= 0)
			view->count = 0;
		break;
	case 'a':
	case 'w':
		if (ch == 'a')
			s->force_text_diff = !s->force_text_diff;
		if (ch == 'w')
			s->ignore_whitespace = !s->ignore_whitespace;
		wclear(view->window);
		s->first_displayed_line = 1;
		s->last_displayed_line = view->nlines;
		s->matched_line = 0;
		diff_view_indicate_progress(view);
		err = create_diff(s);
		view->count = 0;
		break;
	case 'g':
	case KEY_HOME:
		s->first_displayed_line = 1;
		view->count = 0;
		break;
	case 'G':
	case KEY_END:
		view->count = 0;
		if (s->eof)
			break;

		s->first_displayed_line = (s->nlines - view->nlines) + 2;
		s->eof = 1;
		break;
	case 'k':
	case KEY_UP:
	case CTRL('p'):
		if (s->first_displayed_line > 1)
			s->first_displayed_line--;
		else
			view->count = 0;
		break;
	case CTRL('u'):
	case 'u':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
	case 'b':
		if (s->first_displayed_line == 1) {
			view->count = 0;
			break;
		}
		i = 0;
		while (i++ < nscroll && s->first_displayed_line > 1)
			s->first_displayed_line--;
		break;
	case 'j':
	case KEY_DOWN:
	case CTRL('n'):
		if (!s->eof)
			s->first_displayed_line++;
		else
			view->count = 0;
		break;
	case CTRL('d'):
	case 'd':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'):
	case 'f':
	case ' ':
		if (s->eof) {
			view->count = 0;
			break;
		}
		i = 0;
		while (!s->eof && i++ < nscroll) {
			linelen = getline(&line, &linesize, s->f);
			s->first_displayed_line++;
			if (linelen == -1) {
				if (feof(s->f)) {
					s->eof = 1;
				} else
					err = got_ferror(s->f, GOT_ERR_IO);
				break;
			}
		}
		free(line);
		break;
	case '[':
		if (s->diff_context > 0) {
			s->diff_context--;
			s->matched_line = 0;
			diff_view_indicate_progress(view);
			err = create_diff(s);
			if (s->first_displayed_line + view->nlines - 1 >
			    s->nlines) {
				s->first_displayed_line = 1;
				s->last_displayed_line = view->nlines;
			}
		} else
			view->count = 0;
		break;
	case ']':
		if (s->diff_context < GOT_DIFF_MAX_CONTEXT) {
			s->diff_context++;
			s->matched_line = 0;
			diff_view_indicate_progress(view);
			err = create_diff(s);
		} else
			view->count = 0;
		break;
	case '<':
	case ',':
		if (s->log_view == NULL) {
			view->count = 0;
			break;
		}
		ls = &s->log_view->state.log;
		old_selected_entry = ls->selected_entry;

		/* view->count handled in input_log_view() */
		err = input_log_view(NULL, s->log_view, KEY_UP);
		if (err)
			break;

		if (old_selected_entry == ls->selected_entry)
			break;

		err = set_selected_commit(s, ls->selected_entry);
		if (err)
			break;

		s->first_displayed_line = 1;
		s->last_displayed_line = view->nlines;
		s->matched_line = 0;
		view->x = 0;

		diff_view_indicate_progress(view);
		err = create_diff(s);
		break;
	case '>':
	case '.':
		if (s->log_view == NULL) {
			view->count = 0;
			break;
		}
		ls = &s->log_view->state.log;
		old_selected_entry = ls->selected_entry;

		/* view->count handled in input_log_view() */
		err = input_log_view(NULL, s->log_view, KEY_DOWN);
		if (err)
			break;

		if (old_selected_entry == ls->selected_entry)
			break;

		err = set_selected_commit(s, ls->selected_entry);
		if (err)
			break;

		s->first_displayed_line = 1;
		s->last_displayed_line = view->nlines;
		s->matched_line = 0;
		view->x = 0;

		diff_view_indicate_progress(view);
		err = create_diff(s);
		break;
	default:
		view->count = 0;
		break;
	}

	return err;
}

static const struct got_error *
cmd_diff(int argc, char *argv[])
{
	const struct got_error *error = NULL;
	struct got_repository *repo = NULL;
	struct got_worktree *worktree = NULL;
	struct got_object_id *id1 = NULL, *id2 = NULL;
	char *repo_path = NULL, *cwd = NULL;
	char *id_str1 = NULL, *id_str2 = NULL;
	char *label1 = NULL, *label2 = NULL;
	int diff_context = 3, ignore_whitespace = 0;
	int ch, force_text_diff = 0;
	const char *errstr;
	struct tog_view *view;
	int *pack_fds = NULL;

	while ((ch = getopt(argc, argv, "aC:r:w")) != -1) {
		switch (ch) {
		case 'a':
			force_text_diff = 1;
			break;
		case 'C':
			diff_context = strtonum(optarg, 0, GOT_DIFF_MAX_CONTEXT,
			    &errstr);
			if (errstr != NULL)
				errx(1, "number of context lines is %s: %s",
				    errstr, errstr);
			break;
		case 'r':
			repo_path = realpath(optarg, NULL);
			if (repo_path == NULL)
				return got_error_from_errno2("realpath",
				    optarg);
			got_path_strip_trailing_slashes(repo_path);
			break;
		case 'w':
			ignore_whitespace = 1;
			break;
		default:
			usage_diff();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc == 0) {
		usage_diff(); /* TODO show local worktree changes */
	} else if (argc == 2) {
		id_str1 = argv[0];
		id_str2 = argv[1];
	} else
		usage_diff();

	error = got_repo_pack_fds_open(&pack_fds);
	if (error)
		goto done;

	if (repo_path == NULL) {
		cwd = getcwd(NULL, 0);
		if (cwd == NULL)
			return got_error_from_errno("getcwd");
		error = got_worktree_open(&worktree, cwd);
		if (error && error->code != GOT_ERR_NOT_WORKTREE)
			goto done;
		if (worktree)
			repo_path =
			    strdup(got_worktree_get_repo_path(worktree));
		else
			repo_path = strdup(cwd);
		if (repo_path == NULL) {
			error = got_error_from_errno("strdup");
			goto done;
		}
	}

	error = got_repo_open(&repo, repo_path, NULL, pack_fds);
	if (error)
		goto done;

	init_curses();

	error = apply_unveil(got_repo_get_path(repo), NULL);
	if (error)
		goto done;

	error = tog_load_refs(repo, 0);
	if (error)
		goto done;

	error = got_repo_match_object_id(&id1, &label1, id_str1,
	    GOT_OBJ_TYPE_ANY, &tog_refs, repo);
	if (error)
		goto done;

	error = got_repo_match_object_id(&id2, &label2, id_str2,
	    GOT_OBJ_TYPE_ANY, &tog_refs, repo);
	if (error)
		goto done;

	view = view_open(0, 0, 0, 0, TOG_VIEW_DIFF);
	if (view == NULL) {
		error = got_error_from_errno("view_open");
		goto done;
	}
	error = open_diff_view(view, id1, id2, label1, label2, diff_context,
	    ignore_whitespace, force_text_diff, NULL,  repo);
	if (error)
		goto done;
	error = view_loop(view);
done:
	free(label1);
	free(label2);
	free(repo_path);
	free(cwd);
	if (repo) {
		const struct got_error *close_err = got_repo_close(repo);
		if (error == NULL)
			error = close_err;
	}
	if (worktree)
		got_worktree_close(worktree);
	if (pack_fds) {
		const struct got_error *pack_err =
		    got_repo_pack_fds_close(pack_fds);
		if (error == NULL)
			error = pack_err;
	}
	tog_free_refs();
	return error;
}

__dead static void
usage_blame(void)
{
	endwin();
	fprintf(stderr,
	    "usage: %s blame [-c commit] [-r repository-path] path\n",
	    getprogname());
	exit(1);
}

struct tog_blame_line {
	int annotated;
	struct got_object_id *id;
};

static const struct got_error *
draw_blame(struct tog_view *view)
{
	struct tog_blame_view_state *s = &view->state.blame;
	struct tog_blame *blame = &s->blame;
	regmatch_t *regmatch = &view->regmatch;
	const struct got_error *err;
	int lineno = 0, nprinted = 0;
	char *line = NULL;
	size_t linesize = 0;
	ssize_t linelen;
	wchar_t *wline;
	int width;
	struct tog_blame_line *blame_line;
	struct got_object_id *prev_id = NULL;
	char *id_str;
	struct tog_color *tc;

	err = got_object_id_str(&id_str, &s->blamed_commit->id);
	if (err)
		return err;

	rewind(blame->f);
	werase(view->window);

	if (asprintf(&line, "commit %s", id_str) == -1) {
		err = got_error_from_errno("asprintf");
		free(id_str);
		return err;
	}

	err = format_line(&wline, &width, NULL, line, 0, view->ncols, 0, 0);
	free(line);
	line = NULL;
	if (err)
		return err;
	if (view_needs_focus_indication(view))
		wstandout(view->window);
	tc = get_color(&s->colors, TOG_COLOR_COMMIT);
	if (tc)
		wattr_on(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	waddwstr(view->window, wline);
	if (tc)
		wattr_off(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	if (view_needs_focus_indication(view))
		wstandend(view->window);
	free(wline);
	wline = NULL;
	if (width < view->ncols - 1)
		waddch(view->window, '\n');

	if (asprintf(&line, "[%d/%d] %s%s",
	    s->first_displayed_line - 1 + s->selected_line, blame->nlines,
	    s->blame_complete ? "" : "annotating... ", s->path) == -1) {
		free(id_str);
		return got_error_from_errno("asprintf");
	}
	free(id_str);
	err = format_line(&wline, &width, NULL, line, 0, view->ncols, 0, 0);
	free(line);
	line = NULL;
	if (err)
		return err;
	waddwstr(view->window, wline);
	free(wline);
	wline = NULL;
	if (width < view->ncols - 1)
		waddch(view->window, '\n');

	s->eof = 0;
	view->maxx = 0;
	while (nprinted < view->nlines - 2) {
		linelen = getline(&line, &linesize, blame->f);
		if (linelen == -1) {
			if (feof(blame->f)) {
				s->eof = 1;
				break;
			}
			free(line);
			return got_ferror(blame->f, GOT_ERR_IO);
		}
		if (++lineno < s->first_displayed_line)
			continue;

		/* Set view->maxx based on full line length. */
		err = format_line(&wline, &width, NULL, line, 0, INT_MAX, 9, 1);
		if (err) {
			free(line);
			return err;
		}
		free(wline);
		wline = NULL;
		view->maxx = MAX(view->maxx, width);

		if (view->focussed && nprinted == s->selected_line - 1)
			wstandout(view->window);

		if (blame->nlines > 0) {
			blame_line = &blame->lines[lineno - 1];
			if (blame_line->annotated && prev_id &&
			    got_object_id_cmp(prev_id, blame_line->id) == 0 &&
			    !(view->focussed &&
			    nprinted == s->selected_line - 1)) {
				waddstr(view->window, "        ");
			} else if (blame_line->annotated) {
				char *id_str;
				err = got_object_id_str(&id_str,
				    blame_line->id);
				if (err) {
					free(line);
					return err;
				}
				tc = get_color(&s->colors, TOG_COLOR_COMMIT);
				if (tc)
					wattr_on(view->window,
					    COLOR_PAIR(tc->colorpair), NULL);
				wprintw(view->window, "%.8s", id_str);
				if (tc)
					wattr_off(view->window,
					    COLOR_PAIR(tc->colorpair), NULL);
				free(id_str);
				prev_id = blame_line->id;
			} else {
				waddstr(view->window, "........");
				prev_id = NULL;
			}
		} else {
			waddstr(view->window, "........");
			prev_id = NULL;
		}

		if (view->focussed && nprinted == s->selected_line - 1)
			wstandend(view->window);
		waddstr(view->window, " ");

		if (view->ncols <= 9) {
			width = 9;
		} else if (s->first_displayed_line + nprinted ==
		    s->matched_line &&
		    regmatch->rm_so >= 0 && regmatch->rm_so < regmatch->rm_eo) {
			err = add_matched_line(&width, line, view->ncols - 9, 9,
			    view->window, view->x, regmatch);
			if (err) {
				free(line);
				return err;
			}
			width += 9;
		} else {
			int skip;
			err = format_line(&wline, &width, &skip, line,
			    view->x, view->ncols - 9, 9, 1);
			if (err) {
				free(line);
				return err;
			}
			waddwstr(view->window, &wline[skip]);
			width += 9;
			free(wline);
			wline = NULL;
		}

		if (width <= view->ncols - 1)
			waddch(view->window, '\n');
		if (++nprinted == 1)
			s->first_displayed_line = lineno;
	}
	free(line);
	s->last_displayed_line = lineno;

	view_vborder(view);

	return NULL;
}

static const struct got_error *
blame_cb(void *arg, int nlines, int lineno,
    struct got_commit_object *commit, struct got_object_id *id)
{
	const struct got_error *err = NULL;
	struct tog_blame_cb_args *a = arg;
	struct tog_blame_line *line;
	int errcode;

	if (nlines != a->nlines ||
	    (lineno != -1 && lineno < 1) || lineno > a->nlines)
		return got_error(GOT_ERR_RANGE);

	errcode = pthread_mutex_lock(&tog_mutex);
	if (errcode)
		return got_error_set_errno(errcode, "pthread_mutex_lock");

	if (*a->quit) {	/* user has quit the blame view */
		err = got_error(GOT_ERR_ITER_COMPLETED);
		goto done;
	}

	if (lineno == -1)
		goto done; /* no change in this commit */

	line = &a->lines[lineno - 1];
	if (line->annotated)
		goto done;

	line->id = got_object_id_dup(id);
	if (line->id == NULL) {
		err = got_error_from_errno("got_object_id_dup");
		goto done;
	}
	line->annotated = 1;
done:
	errcode = pthread_mutex_unlock(&tog_mutex);
	if (errcode)
		err = got_error_set_errno(errcode, "pthread_mutex_unlock");
	return err;
}

static void *
blame_thread(void *arg)
{
	const struct got_error *err, *close_err;
	struct tog_blame_thread_args *ta = arg;
	struct tog_blame_cb_args *a = ta->cb_args;
	int errcode, fd = -1;

	fd = got_opentempfd();
	if (fd == -1)
		return (void *)got_error_from_errno("got_opentempfd");

	err = block_signals_used_by_main_thread();
	if (err)
		return (void *)err;

	err = got_blame(ta->path, a->commit_id, ta->repo,
	    blame_cb, ta->cb_args, ta->cancel_cb, ta->cancel_arg, fd);
	if (err && err->code == GOT_ERR_CANCELLED)
		err = NULL;

	errcode = pthread_mutex_lock(&tog_mutex);
	if (errcode)
		return (void *)got_error_set_errno(errcode,
		    "pthread_mutex_lock");

	close_err = got_repo_close(ta->repo);
	if (err == NULL)
		err = close_err;
	ta->repo = NULL;
	*ta->complete = 1;

	errcode = pthread_mutex_unlock(&tog_mutex);
	if (errcode && err == NULL)
		err = got_error_set_errno(errcode, "pthread_mutex_unlock");

	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno("close");

	return (void *)err;
}

static struct got_object_id *
get_selected_commit_id(struct tog_blame_line *lines, int nlines,
    int first_displayed_line, int selected_line)
{
	struct tog_blame_line *line;

	if (nlines <= 0)
		return NULL;

	line = &lines[first_displayed_line - 1 + selected_line - 1];
	if (!line->annotated)
		return NULL;

	return line->id;
}

static const struct got_error *
stop_blame(struct tog_blame *blame)
{
	const struct got_error *err = NULL;
	int i;

	if (blame->thread) {
		int errcode;
		errcode = pthread_mutex_unlock(&tog_mutex);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_mutex_unlock");
		errcode = pthread_join(blame->thread, (void **)&err);
		if (errcode)
			return got_error_set_errno(errcode, "pthread_join");
		errcode = pthread_mutex_lock(&tog_mutex);
		if (errcode)
			return got_error_set_errno(errcode,
			    "pthread_mutex_lock");
		if (err && err->code == GOT_ERR_ITER_COMPLETED)
			err = NULL;
		blame->thread = NULL;
	}
	if (blame->thread_args.repo) {
		const struct got_error *close_err;
		close_err = got_repo_close(blame->thread_args.repo);
		if (err == NULL)
			err = close_err;
		blame->thread_args.repo = NULL;
	}
	if (blame->f) {
		if (fclose(blame->f) == EOF && err == NULL)
			err = got_error_from_errno("fclose");
		blame->f = NULL;
	}
	if (blame->lines) {
		for (i = 0; i < blame->nlines; i++)
			free(blame->lines[i].id);
		free(blame->lines);
		blame->lines = NULL;
	}
	free(blame->cb_args.commit_id);
	blame->cb_args.commit_id = NULL;
	if (blame->pack_fds) {
		const struct got_error *pack_err =
		    got_repo_pack_fds_close(blame->pack_fds);
		if (err == NULL)
			err = pack_err;
		blame->pack_fds = NULL;
	}
	return err;
}

static const struct got_error *
cancel_blame_view(void *arg)
{
	const struct got_error *err = NULL;
	int *done = arg;
	int errcode;

	errcode = pthread_mutex_lock(&tog_mutex);
	if (errcode)
		return got_error_set_errno(errcode,
		    "pthread_mutex_unlock");

	if (*done)
		err = got_error(GOT_ERR_CANCELLED);

	errcode = pthread_mutex_unlock(&tog_mutex);
	if (errcode)
		return got_error_set_errno(errcode,
		    "pthread_mutex_lock");

	return err;
}

static const struct got_error *
run_blame(struct tog_view *view)
{
	struct tog_blame_view_state *s = &view->state.blame;
	struct tog_blame *blame = &s->blame;
	const struct got_error *err = NULL;
	struct got_commit_object *commit = NULL;
	struct got_blob_object *blob = NULL;
	struct got_repository *thread_repo = NULL;
	struct got_object_id *obj_id = NULL;
	int obj_type, fd = -1;
	int *pack_fds = NULL;

	err = got_object_open_as_commit(&commit, s->repo,
	    &s->blamed_commit->id);
	if (err)
		return err;

	fd = got_opentempfd();
	if (fd == -1) {
		err = got_error_from_errno("got_opentempfd");
		goto done;
	}

	err = got_object_id_by_path(&obj_id, s->repo, commit, s->path);
	if (err)
		goto done;

	err = got_object_get_type(&obj_type, s->repo, obj_id);
	if (err)
		goto done;

	if (obj_type != GOT_OBJ_TYPE_BLOB) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}

	err = got_object_open_as_blob(&blob, s->repo, obj_id, 8192, fd);
	if (err)
		goto done;
	blame->f = got_opentemp();
	if (blame->f == NULL) {
		err = got_error_from_errno("got_opentemp");
		goto done;
	}
	err = got_object_blob_dump_to_file(&blame->filesize, &blame->nlines,
	    &blame->line_offsets, blame->f, blob);
	if (err)
		goto done;
	if (blame->nlines == 0) {
		s->blame_complete = 1;
		goto done;
	}

	/* Don't include \n at EOF in the blame line count. */
	if (blame->line_offsets[blame->nlines - 1] == blame->filesize)
		blame->nlines--;

	blame->lines = calloc(blame->nlines, sizeof(*blame->lines));
	if (blame->lines == NULL) {
		err = got_error_from_errno("calloc");
		goto done;
	}

	err = got_repo_pack_fds_open(&pack_fds);
	if (err)
		goto done;
	err = got_repo_open(&thread_repo, got_repo_get_path(s->repo), NULL,
	    pack_fds);
	if (err)
		goto done;

	blame->pack_fds = pack_fds;
	blame->cb_args.view = view;
	blame->cb_args.lines = blame->lines;
	blame->cb_args.nlines = blame->nlines;
	blame->cb_args.commit_id = got_object_id_dup(&s->blamed_commit->id);
	if (blame->cb_args.commit_id == NULL) {
		err = got_error_from_errno("got_object_id_dup");
		goto done;
	}
	blame->cb_args.quit = &s->done;

	blame->thread_args.path = s->path;
	blame->thread_args.repo = thread_repo;
	blame->thread_args.cb_args = &blame->cb_args;
	blame->thread_args.complete = &s->blame_complete;
	blame->thread_args.cancel_cb = cancel_blame_view;
	blame->thread_args.cancel_arg = &s->done;
	s->blame_complete = 0;

	if (s->first_displayed_line + view->nlines - 1 > blame->nlines) {
		s->first_displayed_line = 1;
		s->last_displayed_line = view->nlines;
		s->selected_line = 1;
	}
	s->matched_line = 0;

done:
	if (commit)
		got_object_commit_close(commit);
	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno("close");
	if (blob)
		got_object_blob_close(blob);
	free(obj_id);
	if (err)
		stop_blame(blame);
	return err;
}

static const struct got_error *
open_blame_view(struct tog_view *view, char *path,
    struct got_object_id *commit_id, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct tog_blame_view_state *s = &view->state.blame;

	STAILQ_INIT(&s->blamed_commits);

	s->path = strdup(path);
	if (s->path == NULL)
		return got_error_from_errno("strdup");

	err = got_object_qid_alloc(&s->blamed_commit, commit_id);
	if (err) {
		free(s->path);
		return err;
	}

	STAILQ_INSERT_HEAD(&s->blamed_commits, s->blamed_commit, entry);
	s->first_displayed_line = 1;
	s->last_displayed_line = view->nlines;
	s->selected_line = 1;
	s->blame_complete = 0;
	s->repo = repo;
	s->commit_id = commit_id;
	memset(&s->blame, 0, sizeof(s->blame));

	STAILQ_INIT(&s->colors);
	if (has_colors() && getenv("TOG_COLORS") != NULL) {
		err = add_color(&s->colors, "^", TOG_COLOR_COMMIT,
		    get_color_value("TOG_COLOR_COMMIT"));
		if (err)
			return err;
	}

	view->show = show_blame_view;
	view->input = input_blame_view;
	view->close = close_blame_view;
	view->search_start = search_start_blame_view;
	view->search_next = search_next_blame_view;

	return run_blame(view);
}

static const struct got_error *
close_blame_view(struct tog_view *view)
{
	const struct got_error *err = NULL;
	struct tog_blame_view_state *s = &view->state.blame;

	if (s->blame.thread)
		err = stop_blame(&s->blame);

	while (!STAILQ_EMPTY(&s->blamed_commits)) {
		struct got_object_qid *blamed_commit;
		blamed_commit = STAILQ_FIRST(&s->blamed_commits);
		STAILQ_REMOVE_HEAD(&s->blamed_commits, entry);
		got_object_qid_free(blamed_commit);
	}

	free(s->path);
	free_colors(&s->colors);
	return err;
}

static const struct got_error *
search_start_blame_view(struct tog_view *view)
{
	struct tog_blame_view_state *s = &view->state.blame;

	s->matched_line = 0;
	return NULL;
}

static const struct got_error *
search_next_blame_view(struct tog_view *view)
{
	struct tog_blame_view_state *s = &view->state.blame;
	const struct got_error *err = NULL;
	int lineno;
	char *line = NULL;
	size_t linesize = 0;
	ssize_t linelen;

	if (!view->searching) {
		view->search_next_done = TOG_SEARCH_HAVE_MORE;
		return NULL;
	}

	if (s->matched_line) {
		if (view->searching == TOG_SEARCH_FORWARD)
			lineno = s->matched_line + 1;
		else
			lineno = s->matched_line - 1;
	} else
		lineno = s->first_displayed_line - 1 + s->selected_line;

	while (1) {
		off_t offset;

		if (lineno <= 0 || lineno > s->blame.nlines) {
			if (s->matched_line == 0) {
				view->search_next_done = TOG_SEARCH_HAVE_MORE;
				break;
			}

			if (view->searching == TOG_SEARCH_FORWARD)
				lineno = 1;
			else
				lineno = s->blame.nlines;
		}

		offset = s->blame.line_offsets[lineno - 1];
		if (fseeko(s->blame.f, offset, SEEK_SET) != 0) {
			free(line);
			return got_error_from_errno("fseeko");
		}
		linelen = getline(&line, &linesize, s->blame.f);
		if (linelen != -1) {
			char *exstr;
			err = expand_tab(&exstr, line);
			if (err)
				break;
			if (match_line(exstr, &view->regex, 1,
			    &view->regmatch)) {
				view->search_next_done = TOG_SEARCH_HAVE_MORE;
				s->matched_line = lineno;
				free(exstr);
				break;
			}
			free(exstr);
		}
		if (view->searching == TOG_SEARCH_FORWARD)
			lineno++;
		else
			lineno--;
	}
	free(line);

	if (s->matched_line) {
		s->first_displayed_line = s->matched_line;
		s->selected_line = 1;
	}

	return err;
}

static const struct got_error *
show_blame_view(struct tog_view *view)
{
	const struct got_error *err = NULL;
	struct tog_blame_view_state *s = &view->state.blame;
	int errcode;

	if (s->blame.thread == NULL && !s->blame_complete) {
		errcode = pthread_create(&s->blame.thread, NULL, blame_thread,
		    &s->blame.thread_args);
		if (errcode)
			return got_error_set_errno(errcode, "pthread_create");

		halfdelay(1); /* fast refresh while annotating  */
	}

	if (s->blame_complete)
		halfdelay(10); /* disable fast refresh */

	err = draw_blame(view);

	view_vborder(view);
	return err;
}

static const struct got_error *
input_blame_view(struct tog_view **new_view, struct tog_view *view, int ch)
{
	const struct got_error *err = NULL, *thread_err = NULL;
	struct tog_view *diff_view;
	struct tog_blame_view_state *s = &view->state.blame;
	int begin_x = 0, nscroll = view->nlines - 2;

	switch (ch) {
	case '0':
		view->x = 0;
		break;
	case '$':
		view->x = MAX(view->maxx - view->ncols / 3, 0);
		view->count = 0;
		break;
	case KEY_RIGHT:
	case 'l':
		if (view->x + view->ncols / 3 < view->maxx)
			view->x += 2;  /* move two columns right */
		else
			view->count = 0;
		break;
	case KEY_LEFT:
	case 'h':
		view->x -= MIN(view->x, 2);  /* move two columns back */
		if (view->x <= 0)
			view->count = 0;
		break;
	case 'q':
		s->done = 1;
		break;
	case 'g':
	case KEY_HOME:
		s->selected_line = 1;
		s->first_displayed_line = 1;
		view->count = 0;
		break;
	case 'G':
	case KEY_END:
		if (s->blame.nlines < view->nlines - 2) {
			s->selected_line = s->blame.nlines;
			s->first_displayed_line = 1;
		} else {
			s->selected_line = view->nlines - 2;
			s->first_displayed_line = s->blame.nlines -
			    (view->nlines - 3);
		}
		view->count = 0;
		break;
	case 'k':
	case KEY_UP:
	case CTRL('p'):
		if (s->selected_line > 1)
			s->selected_line--;
		else if (s->selected_line == 1 &&
		    s->first_displayed_line > 1)
			s->first_displayed_line--;
		else
			view->count = 0;
		break;
	case CTRL('u'):
	case 'u':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
	case 'b':
		if (s->first_displayed_line == 1) {
			if (view->count > 1)
				nscroll += nscroll;
			s->selected_line = MAX(1, s->selected_line - nscroll);
			view->count = 0;
			break;
		}
		if (s->first_displayed_line > nscroll)
			s->first_displayed_line -= nscroll;
		else
			s->first_displayed_line = 1;
		break;
	case 'j':
	case KEY_DOWN:
	case CTRL('n'):
		if (s->selected_line < view->nlines - 2 &&
		    s->first_displayed_line +
		    s->selected_line <= s->blame.nlines)
			s->selected_line++;
		else if (s->last_displayed_line <
		    s->blame.nlines)
			s->first_displayed_line++;
		else
			view->count = 0;
		break;
	case 'c':
	case 'p': {
		struct got_object_id *id = NULL;

		view->count = 0;
		id = get_selected_commit_id(s->blame.lines, s->blame.nlines,
		    s->first_displayed_line, s->selected_line);
		if (id == NULL)
			break;
		if (ch == 'p') {
			struct got_commit_object *commit, *pcommit;
			struct got_object_qid *pid;
			struct got_object_id *blob_id = NULL;
			int obj_type;
			err = got_object_open_as_commit(&commit,
			    s->repo, id);
			if (err)
				break;
			pid = STAILQ_FIRST(
			    got_object_commit_get_parent_ids(commit));
			if (pid == NULL) {
				got_object_commit_close(commit);
				break;
			}
			/* Check if path history ends here. */
			err = got_object_open_as_commit(&pcommit,
			    s->repo, &pid->id);
			if (err)
				break;
			err = got_object_id_by_path(&blob_id, s->repo,
			    pcommit, s->path);
			got_object_commit_close(pcommit);
			if (err) {
				if (err->code == GOT_ERR_NO_TREE_ENTRY)
					err = NULL;
				got_object_commit_close(commit);
				break;
			}
			err = got_object_get_type(&obj_type, s->repo,
			    blob_id);
			free(blob_id);
			/* Can't blame non-blob type objects. */
			if (obj_type != GOT_OBJ_TYPE_BLOB) {
				got_object_commit_close(commit);
				break;
			}
			err = got_object_qid_alloc(&s->blamed_commit,
			    &pid->id);
			got_object_commit_close(commit);
		} else {
			if (got_object_id_cmp(id,
			    &s->blamed_commit->id) == 0)
				break;
			err = got_object_qid_alloc(&s->blamed_commit,
			    id);
		}
		if (err)
			break;
		s->done = 1;
		thread_err = stop_blame(&s->blame);
		s->done = 0;
		if (thread_err)
			break;
		STAILQ_INSERT_HEAD(&s->blamed_commits,
		    s->blamed_commit, entry);
		err = run_blame(view);
		if (err)
			break;
		break;
	}
	case 'C': {
		struct got_object_qid *first;

		view->count = 0;
		first = STAILQ_FIRST(&s->blamed_commits);
		if (!got_object_id_cmp(&first->id, s->commit_id))
			break;
		s->done = 1;
		thread_err = stop_blame(&s->blame);
		s->done = 0;
		if (thread_err)
			break;
		STAILQ_REMOVE_HEAD(&s->blamed_commits, entry);
		got_object_qid_free(s->blamed_commit);
		s->blamed_commit =
		    STAILQ_FIRST(&s->blamed_commits);
		err = run_blame(view);
		if (err)
			break;
		break;
	}
	case KEY_ENTER:
	case '\r': {
		struct got_object_id *id = NULL;
		struct got_object_qid *pid;
		struct got_commit_object *commit = NULL;

		view->count = 0;
		id = get_selected_commit_id(s->blame.lines, s->blame.nlines,
		    s->first_displayed_line, s->selected_line);
		if (id == NULL)
			break;
		err = got_object_open_as_commit(&commit, s->repo, id);
		if (err)
			break;
		pid = STAILQ_FIRST(
		    got_object_commit_get_parent_ids(commit));
		if (view_is_parent_view(view))
		    begin_x = view_split_begin_x(view->begin_x);
		diff_view = view_open(0, 0, 0, begin_x, TOG_VIEW_DIFF);
		if (diff_view == NULL) {
			got_object_commit_close(commit);
			err = got_error_from_errno("view_open");
			break;
		}
		err = open_diff_view(diff_view, pid ? &pid->id : NULL,
		    id, NULL, NULL, 3, 0, 0, NULL, s->repo);
		got_object_commit_close(commit);
		if (err) {
			view_close(diff_view);
			break;
		}
		view->focussed = 0;
		diff_view->focussed = 1;
		if (view_is_parent_view(view)) {
			err = view_close_child(view);
			if (err)
				break;
			err = view_set_child(view, diff_view);
			if (err)
				break;
			view->focus_child = 1;
		} else
			*new_view = diff_view;
		if (err)
			break;
		break;
	}
	case CTRL('d'):
	case 'd':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'):
	case 'f':
	case ' ':
		if (s->last_displayed_line >= s->blame.nlines &&
		    s->selected_line >= MIN(s->blame.nlines,
		    view->nlines - 2)) {
			view->count = 0;
			break;
		}
		if (s->last_displayed_line >= s->blame.nlines &&
		    s->selected_line < view->nlines - 2) {
			s->selected_line +=
			    MIN(nscroll, s->last_displayed_line -
			    s->first_displayed_line - s->selected_line + 1);
		}
		if (s->last_displayed_line + nscroll <= s->blame.nlines)
			s->first_displayed_line += nscroll;
		else
			s->first_displayed_line =
			    s->blame.nlines - (view->nlines - 3);
		break;
	case KEY_RESIZE:
		if (s->selected_line > view->nlines - 2) {
			s->selected_line = MIN(s->blame.nlines,
			    view->nlines - 2);
		}
		break;
	default:
		view->count = 0;
		break;
	}
	return thread_err ? thread_err : err;
}

static const struct got_error *
cmd_blame(int argc, char *argv[])
{
	const struct got_error *error;
	struct got_repository *repo = NULL;
	struct got_worktree *worktree = NULL;
	char *cwd = NULL, *repo_path = NULL, *in_repo_path = NULL;
	char *link_target = NULL;
	struct got_object_id *commit_id = NULL;
	struct got_commit_object *commit = NULL;
	char *commit_id_str = NULL;
	int ch;
	struct tog_view *view;
	int *pack_fds = NULL;

	while ((ch = getopt(argc, argv, "c:r:")) != -1) {
		switch (ch) {
		case 'c':
			commit_id_str = optarg;
			break;
		case 'r':
			repo_path = realpath(optarg, NULL);
			if (repo_path == NULL)
				return got_error_from_errno2("realpath",
				    optarg);
			break;
		default:
			usage_blame();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage_blame();

	error = got_repo_pack_fds_open(&pack_fds);
	if (error != NULL)
		goto done;

	if (repo_path == NULL) {
		cwd = getcwd(NULL, 0);
		if (cwd == NULL)
			return got_error_from_errno("getcwd");
		error = got_worktree_open(&worktree, cwd);
		if (error && error->code != GOT_ERR_NOT_WORKTREE)
			goto done;
		if (worktree)
			repo_path =
			    strdup(got_worktree_get_repo_path(worktree));
		else
			repo_path = strdup(cwd);
		if (repo_path == NULL) {
			error = got_error_from_errno("strdup");
			goto done;
		}
	}

	error = got_repo_open(&repo, repo_path, NULL, pack_fds);
	if (error != NULL)
		goto done;

	error = get_in_repo_path_from_argv0(&in_repo_path, argc, argv, repo,
	    worktree);
	if (error)
		goto done;

	init_curses();

	error = apply_unveil(got_repo_get_path(repo), NULL);
	if (error)
		goto done;

	error = tog_load_refs(repo, 0);
	if (error)
		goto done;

	if (commit_id_str == NULL) {
		struct got_reference *head_ref;
		error = got_ref_open(&head_ref, repo, worktree ?
		    got_worktree_get_head_ref_name(worktree) : GOT_REF_HEAD, 0);
		if (error != NULL)
			goto done;
		error = got_ref_resolve(&commit_id, repo, head_ref);
		got_ref_close(head_ref);
	} else {
		error = got_repo_match_object_id(&commit_id, NULL,
		    commit_id_str, GOT_OBJ_TYPE_COMMIT, &tog_refs, repo);
	}
	if (error != NULL)
		goto done;

	view = view_open(0, 0, 0, 0, TOG_VIEW_BLAME);
	if (view == NULL) {
		error = got_error_from_errno("view_open");
		goto done;
	}

	error = got_object_open_as_commit(&commit, repo, commit_id);
	if (error)
		goto done;

	error = got_object_resolve_symlinks(&link_target, in_repo_path,
	    commit, repo);
	if (error)
		goto done;

	error = open_blame_view(view, link_target ? link_target : in_repo_path,
	    commit_id, repo);
	if (error)
		goto done;
	if (worktree) {
		/* Release work tree lock. */
		got_worktree_close(worktree);
		worktree = NULL;
	}
	error = view_loop(view);
done:
	free(repo_path);
	free(in_repo_path);
	free(link_target);
	free(cwd);
	free(commit_id);
	if (commit)
		got_object_commit_close(commit);
	if (worktree)
		got_worktree_close(worktree);
	if (repo) {
		const struct got_error *close_err = got_repo_close(repo);
		if (error == NULL)
			error = close_err;
	}
	if (pack_fds) {
		const struct got_error *pack_err =
		    got_repo_pack_fds_close(pack_fds);
		if (error == NULL)
			error = pack_err;
	}
	tog_free_refs();
	return error;
}

static const struct got_error *
draw_tree_entries(struct tog_view *view, const char *parent_path)
{
	struct tog_tree_view_state *s = &view->state.tree;
	const struct got_error *err = NULL;
	struct got_tree_entry *te;
	wchar_t *wline;
	struct tog_color *tc;
	int width, n, i, nentries;
	int limit = view->nlines;

	s->ndisplayed = 0;

	werase(view->window);

	if (limit == 0)
		return NULL;

	err = format_line(&wline, &width, NULL, s->tree_label, 0, view->ncols,
	    0, 0);
	if (err)
		return err;
	if (view_needs_focus_indication(view))
		wstandout(view->window);
	tc = get_color(&s->colors, TOG_COLOR_COMMIT);
	if (tc)
		wattr_on(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	waddwstr(view->window, wline);
	if (tc)
		wattr_off(view->window,
		    COLOR_PAIR(tc->colorpair), NULL);
	if (view_needs_focus_indication(view))
		wstandend(view->window);
	free(wline);
	wline = NULL;
	if (width < view->ncols - 1)
		waddch(view->window, '\n');
	if (--limit <= 0)
		return NULL;
	err = format_line(&wline, &width, NULL, parent_path, 0, view->ncols,
	    0, 0);
	if (err)
		return err;
	waddwstr(view->window, wline);
	free(wline);
	wline = NULL;
	if (width < view->ncols - 1)
		waddch(view->window, '\n');
	if (--limit <= 0)
		return NULL;
	waddch(view->window, '\n');
	if (--limit <= 0)
		return NULL;

	if (s->first_displayed_entry == NULL) {
		te = got_object_tree_get_first_entry(s->tree);
		if (s->selected == 0) {
			if (view->focussed)
				wstandout(view->window);
			s->selected_entry = NULL;
		}
		waddstr(view->window, "  ..\n");	/* parent directory */
		if (s->selected == 0 && view->focussed)
			wstandend(view->window);
		s->ndisplayed++;
		if (--limit <= 0)
			return NULL;
		n = 1;
	} else {
		n = 0;
		te = s->first_displayed_entry;
	}

	nentries = got_object_tree_get_nentries(s->tree);
	for (i = got_tree_entry_get_index(te); i < nentries; i++) {
		char *line = NULL, *id_str = NULL, *link_target = NULL;
		const char *modestr = "";
		mode_t mode;

		te = got_object_tree_get_entry(s->tree, i);
		mode = got_tree_entry_get_mode(te);

		if (s->show_ids) {
			err = got_object_id_str(&id_str,
			    got_tree_entry_get_id(te));
			if (err)
				return got_error_from_errno(
				    "got_object_id_str");
		}
		if (got_object_tree_entry_is_submodule(te))
			modestr = "$";
		else if (S_ISLNK(mode)) {
			int i;

			err = got_tree_entry_get_symlink_target(&link_target,
			    te, s->repo);
			if (err) {
				free(id_str);
				return err;
			}
			for (i = 0; i < strlen(link_target); i++) {
				if (!isprint((unsigned char)link_target[i]))
					link_target[i] = '?';
			}
			modestr = "@";
		}
		else if (S_ISDIR(mode))
			modestr = "/";
		else if (mode & S_IXUSR)
			modestr = "*";
		if (asprintf(&line, "%s  %s%s%s%s", id_str ? id_str : "",
		    got_tree_entry_get_name(te), modestr,
		    link_target ? " -> ": "",
		    link_target ? link_target : "") == -1) {
			free(id_str);
			free(link_target);
			return got_error_from_errno("asprintf");
		}
		free(id_str);
		free(link_target);
		err = format_line(&wline, &width, NULL, line, 0, view->ncols,
		    0, 0);
		if (err) {
			free(line);
			break;
		}
		if (n == s->selected) {
			if (view->focussed)
				wstandout(view->window);
			s->selected_entry = te;
		}
		tc = match_color(&s->colors, line);
		if (tc)
			wattr_on(view->window,
			    COLOR_PAIR(tc->colorpair), NULL);
		waddwstr(view->window, wline);
		if (tc)
			wattr_off(view->window,
			    COLOR_PAIR(tc->colorpair), NULL);
		if (width < view->ncols - 1)
			waddch(view->window, '\n');
		if (n == s->selected && view->focussed)
			wstandend(view->window);
		free(line);
		free(wline);
		wline = NULL;
		n++;
		s->ndisplayed++;
		s->last_displayed_entry = te;
		if (--limit <= 0)
			break;
	}

	return err;
}

static void
tree_scroll_up(struct tog_tree_view_state *s, int maxscroll)
{
	struct got_tree_entry *te;
	int isroot = s->tree == s->root;
	int i = 0;

	if (s->first_displayed_entry == NULL)
		return;

	te = got_tree_entry_get_prev(s->tree, s->first_displayed_entry);
	while (i++ < maxscroll) {
		if (te == NULL) {
			if (!isroot)
				s->first_displayed_entry = NULL;
			break;
		}
		s->first_displayed_entry = te;
		te = got_tree_entry_get_prev(s->tree, te);
	}
}

static void
tree_scroll_down(struct tog_tree_view_state *s, int maxscroll)
{
	struct got_tree_entry *next, *last;
	int n = 0;

	if (s->first_displayed_entry)
		next = got_tree_entry_get_next(s->tree,
		    s->first_displayed_entry);
	else
		next = got_object_tree_get_first_entry(s->tree);

	last = s->last_displayed_entry;
	while (next && last && n++ < maxscroll) {
		last = got_tree_entry_get_next(s->tree, last);
		if (last) {
			s->first_displayed_entry = next;
			next = got_tree_entry_get_next(s->tree, next);
		}
	}
}

static const struct got_error *
tree_entry_path(char **path, struct tog_parent_trees *parents,
    struct got_tree_entry *te)
{
	const struct got_error *err = NULL;
	struct tog_parent_tree *pt;
	size_t len = 2; /* for leading slash and NUL */

	TAILQ_FOREACH(pt, parents, entry)
		len += strlen(got_tree_entry_get_name(pt->selected_entry))
		    + 1 /* slash */;
	if (te)
		len += strlen(got_tree_entry_get_name(te));

	*path = calloc(1, len);
	if (path == NULL)
		return got_error_from_errno("calloc");

	(*path)[0] = '/';
	pt = TAILQ_LAST(parents, tog_parent_trees);
	while (pt) {
		const char *name = got_tree_entry_get_name(pt->selected_entry);
		if (strlcat(*path, name, len) >= len) {
			err = got_error(GOT_ERR_NO_SPACE);
			goto done;
		}
		if (strlcat(*path, "/", len) >= len) {
			err = got_error(GOT_ERR_NO_SPACE);
			goto done;
		}
		pt = TAILQ_PREV(pt, tog_parent_trees, entry);
	}
	if (te) {
		if (strlcat(*path, got_tree_entry_get_name(te), len) >= len) {
			err = got_error(GOT_ERR_NO_SPACE);
			goto done;
		}
	}
done:
	if (err) {
		free(*path);
		*path = NULL;
	}
	return err;
}

static const struct got_error *
blame_tree_entry(struct tog_view **new_view, int begin_x,
    struct got_tree_entry *te, struct tog_parent_trees *parents,
    struct got_object_id *commit_id, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *path;
	struct tog_view *blame_view;

	*new_view = NULL;

	err = tree_entry_path(&path, parents, te);
	if (err)
		return err;

	blame_view = view_open(0, 0, 0, begin_x, TOG_VIEW_BLAME);
	if (blame_view == NULL) {
		err = got_error_from_errno("view_open");
		goto done;
	}

	err = open_blame_view(blame_view, path, commit_id, repo);
	if (err) {
		if (err->code == GOT_ERR_CANCELLED)
			err = NULL;
		view_close(blame_view);
	} else
		*new_view = blame_view;
done:
	free(path);
	return err;
}

static const struct got_error *
log_selected_tree_entry(struct tog_view **new_view, int begin_x,
    struct tog_tree_view_state *s)
{
	struct tog_view *log_view;
	const struct got_error *err = NULL;
	char *path;

	*new_view = NULL;

	log_view = view_open(0, 0, 0, begin_x, TOG_VIEW_LOG);
	if (log_view == NULL)
		return got_error_from_errno("view_open");

	err = tree_entry_path(&path, &s->parents, s->selected_entry);
	if (err)
		return err;

	err = open_log_view(log_view, s->commit_id, s->repo, s->head_ref_name,
	    path, 0);
	if (err)
		view_close(log_view);
	else
		*new_view = log_view;
	free(path);
	return err;
}

static const struct got_error *
open_tree_view(struct tog_view *view, struct got_object_id *commit_id,
    const char *head_ref_name, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *commit_id_str = NULL;
	struct tog_tree_view_state *s = &view->state.tree;
	struct got_commit_object *commit = NULL;

	TAILQ_INIT(&s->parents);
	STAILQ_INIT(&s->colors);

	s->commit_id = got_object_id_dup(commit_id);
	if (s->commit_id == NULL)
		return got_error_from_errno("got_object_id_dup");

	err = got_object_open_as_commit(&commit, repo, commit_id);
	if (err)
		goto done;

	/*
	 * The root is opened here and will be closed when the view is closed.
	 * Any visited subtrees and their path-wise parents are opened and
	 * closed on demand.
	 */
	err = got_object_open_as_tree(&s->root, repo,
	    got_object_commit_get_tree_id(commit));
	if (err)
		goto done;
	s->tree = s->root;

	err = got_object_id_str(&commit_id_str, commit_id);
	if (err != NULL)
		goto done;

	if (asprintf(&s->tree_label, "commit %s", commit_id_str) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	s->first_displayed_entry = got_object_tree_get_entry(s->tree, 0);
	s->selected_entry = got_object_tree_get_entry(s->tree, 0);
	if (head_ref_name) {
		s->head_ref_name = strdup(head_ref_name);
		if (s->head_ref_name == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	}
	s->repo = repo;

	if (has_colors() && getenv("TOG_COLORS") != NULL) {
		err = add_color(&s->colors, "\\$$",
		    TOG_COLOR_TREE_SUBMODULE,
		    get_color_value("TOG_COLOR_TREE_SUBMODULE"));
		if (err)
			goto done;
		err = add_color(&s->colors, "@$", TOG_COLOR_TREE_SYMLINK,
		    get_color_value("TOG_COLOR_TREE_SYMLINK"));
		if (err)
			goto done;
		err = add_color(&s->colors, "/$",
		    TOG_COLOR_TREE_DIRECTORY,
		    get_color_value("TOG_COLOR_TREE_DIRECTORY"));
		if (err)
			goto done;

		err = add_color(&s->colors, "\\*$",
		    TOG_COLOR_TREE_EXECUTABLE,
		    get_color_value("TOG_COLOR_TREE_EXECUTABLE"));
		if (err)
			goto done;

		err = add_color(&s->colors, "^$", TOG_COLOR_COMMIT,
		    get_color_value("TOG_COLOR_COMMIT"));
		if (err)
			goto done;
	}

	view->show = show_tree_view;
	view->input = input_tree_view;
	view->close = close_tree_view;
	view->search_start = search_start_tree_view;
	view->search_next = search_next_tree_view;
done:
	free(commit_id_str);
	if (commit)
		got_object_commit_close(commit);
	if (err)
		close_tree_view(view);
	return err;
}

static const struct got_error *
close_tree_view(struct tog_view *view)
{
	struct tog_tree_view_state *s = &view->state.tree;

	free_colors(&s->colors);
	free(s->tree_label);
	s->tree_label = NULL;
	free(s->commit_id);
	s->commit_id = NULL;
	free(s->head_ref_name);
	s->head_ref_name = NULL;
	while (!TAILQ_EMPTY(&s->parents)) {
		struct tog_parent_tree *parent;
		parent = TAILQ_FIRST(&s->parents);
		TAILQ_REMOVE(&s->parents, parent, entry);
		if (parent->tree != s->root)
			got_object_tree_close(parent->tree);
		free(parent);

	}
	if (s->tree != NULL && s->tree != s->root)
		got_object_tree_close(s->tree);
	if (s->root)
		got_object_tree_close(s->root);
	return NULL;
}

static const struct got_error *
search_start_tree_view(struct tog_view *view)
{
	struct tog_tree_view_state *s = &view->state.tree;

	s->matched_entry = NULL;
	return NULL;
}

static int
match_tree_entry(struct got_tree_entry *te, regex_t *regex)
{
	regmatch_t regmatch;

	return regexec(regex, got_tree_entry_get_name(te), 1, &regmatch,
	    0) == 0;
}

static const struct got_error *
search_next_tree_view(struct tog_view *view)
{
	struct tog_tree_view_state *s = &view->state.tree;
	struct got_tree_entry *te = NULL;

	if (!view->searching) {
		view->search_next_done = TOG_SEARCH_HAVE_MORE;
		return NULL;
	}

	if (s->matched_entry) {
		if (view->searching == TOG_SEARCH_FORWARD) {
			if (s->selected_entry)
				te = got_tree_entry_get_next(s->tree,
				    s->selected_entry);
			else
				te = got_object_tree_get_first_entry(s->tree);
		} else {
			if (s->selected_entry == NULL)
				te = got_object_tree_get_last_entry(s->tree);
			else
				te = got_tree_entry_get_prev(s->tree,
				    s->selected_entry);
		}
	} else {
		if (s->selected_entry)
			te = s->selected_entry;
		else if (view->searching == TOG_SEARCH_FORWARD)
			te = got_object_tree_get_first_entry(s->tree);
		else
			te = got_object_tree_get_last_entry(s->tree);
	}

	while (1) {
		if (te == NULL) {
			if (s->matched_entry == NULL) {
				view->search_next_done = TOG_SEARCH_HAVE_MORE;
				return NULL;
			}
			if (view->searching == TOG_SEARCH_FORWARD)
				te = got_object_tree_get_first_entry(s->tree);
			else
				te = got_object_tree_get_last_entry(s->tree);
		}

		if (match_tree_entry(te, &view->regex)) {
			view->search_next_done = TOG_SEARCH_HAVE_MORE;
			s->matched_entry = te;
			break;
		}

		if (view->searching == TOG_SEARCH_FORWARD)
			te = got_tree_entry_get_next(s->tree, te);
		else
			te = got_tree_entry_get_prev(s->tree, te);
	}

	if (s->matched_entry) {
		s->first_displayed_entry = s->matched_entry;
		s->selected = 0;
	}

	return NULL;
}

static const struct got_error *
show_tree_view(struct tog_view *view)
{
	const struct got_error *err = NULL;
	struct tog_tree_view_state *s = &view->state.tree;
	char *parent_path;

	err = tree_entry_path(&parent_path, &s->parents, NULL);
	if (err)
		return err;

	err = draw_tree_entries(view, parent_path);
	free(parent_path);

	view_vborder(view);
	return err;
}

static const struct got_error *
input_tree_view(struct tog_view **new_view, struct tog_view *view, int ch)
{
	const struct got_error *err = NULL;
	struct tog_tree_view_state *s = &view->state.tree;
	struct tog_view *log_view, *ref_view;
	struct got_tree_entry *te;
	int begin_x = 0, n, nscroll = view->nlines - 3;

	switch (ch) {
	case 'i':
		s->show_ids = !s->show_ids;
		view->count = 0;
		break;
	case 'l':
		view->count = 0;
		if (!s->selected_entry)
			break;
		if (view_is_parent_view(view))
			begin_x = view_split_begin_x(view->begin_x);
		err = log_selected_tree_entry(&log_view, begin_x, s);
		view->focussed = 0;
		log_view->focussed = 1;
		if (view_is_parent_view(view)) {
			err = view_close_child(view);
			if (err)
				return err;
			err = view_set_child(view, log_view);
			if (err)
				return err;
			view->focus_child = 1;
		} else
			*new_view = log_view;
		break;
	case 'r':
		view->count = 0;
		if (view_is_parent_view(view))
			begin_x = view_split_begin_x(view->begin_x);
		ref_view = view_open(view->nlines, view->ncols,
		    view->begin_y, begin_x, TOG_VIEW_REF);
		if (ref_view == NULL)
			return got_error_from_errno("view_open");
		err = open_ref_view(ref_view, s->repo);
		if (err) {
			view_close(ref_view);
			return err;
		}
		view->focussed = 0;
		ref_view->focussed = 1;
		if (view_is_parent_view(view)) {
			err = view_close_child(view);
			if (err)
				return err;
			err = view_set_child(view, ref_view);
			if (err)
				return err;
			view->focus_child = 1;
		} else
			*new_view = ref_view;
		break;
	case 'g':
	case KEY_HOME:
		s->selected = 0;
		view->count = 0;
		if (s->tree == s->root)
			s->first_displayed_entry =
			    got_object_tree_get_first_entry(s->tree);
		else
			s->first_displayed_entry = NULL;
		break;
	case 'G':
	case KEY_END:
		s->selected = 0;
		view->count = 0;
		te = got_object_tree_get_last_entry(s->tree);
		for (n = 0; n < view->nlines - 3; n++) {
			if (te == NULL) {
				if(s->tree != s->root) {
					s->first_displayed_entry = NULL;
					n++;
				}
				break;
			}
			s->first_displayed_entry = te;
			te = got_tree_entry_get_prev(s->tree, te);
		}
		if (n > 0)
			s->selected = n - 1;
		break;
	case 'k':
	case KEY_UP:
	case CTRL('p'):
		if (s->selected > 0) {
			s->selected--;
			break;
		}
		tree_scroll_up(s, 1);
		if (s->selected_entry == NULL ||
		    (s->tree == s->root && s->selected_entry ==
		     got_object_tree_get_first_entry(s->tree)))
			view->count = 0;
		break;
	case CTRL('u'):
	case 'u':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
	case 'b':
		if (s->tree == s->root) {
			if (got_object_tree_get_first_entry(s->tree) ==
			    s->first_displayed_entry)
				s->selected -= MIN(s->selected, nscroll);
		} else {
			if (s->first_displayed_entry == NULL)
				s->selected -= MIN(s->selected, nscroll);
		}
		tree_scroll_up(s, MAX(0, nscroll));
		if (s->selected_entry == NULL ||
		    (s->tree == s->root && s->selected_entry ==
		     got_object_tree_get_first_entry(s->tree)))
			view->count = 0;
		break;
	case 'j':
	case KEY_DOWN:
	case CTRL('n'):
		if (s->selected < s->ndisplayed - 1) {
			s->selected++;
			break;
		}
		if (got_tree_entry_get_next(s->tree, s->last_displayed_entry)
		    == NULL) {
			/* can't scroll any further */
			view->count = 0;
			break;
		}
		tree_scroll_down(s, 1);
		break;
	case CTRL('d'):
	case 'd':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'):
	case 'f':
	case ' ':
		if (got_tree_entry_get_next(s->tree, s->last_displayed_entry)
		    == NULL) {
			/* can't scroll any further; move cursor down */
			if (s->selected < s->ndisplayed - 1)
				s->selected += MIN(nscroll,
				    s->ndisplayed - s->selected - 1);
			else
				view->count = 0;
			break;
		}
		tree_scroll_down(s, nscroll);
		break;
	case KEY_ENTER:
	case '\r':
	case KEY_BACKSPACE:
		if (s->selected_entry == NULL || ch == KEY_BACKSPACE) {
			struct tog_parent_tree *parent;
			/* user selected '..' */
			if (s->tree == s->root) {
				view->count = 0;
				break;
			}
			parent = TAILQ_FIRST(&s->parents);
			TAILQ_REMOVE(&s->parents, parent,
			    entry);
			got_object_tree_close(s->tree);
			s->tree = parent->tree;
			s->first_displayed_entry =
			    parent->first_displayed_entry;
			s->selected_entry =
			    parent->selected_entry;
			s->selected = parent->selected;
			free(parent);
		} else if (S_ISDIR(got_tree_entry_get_mode(
		    s->selected_entry))) {
			struct got_tree_object *subtree;
			view->count = 0;
			err = got_object_open_as_tree(&subtree, s->repo,
			    got_tree_entry_get_id(s->selected_entry));
			if (err)
				break;
			err = tree_view_visit_subtree(s, subtree);
			if (err) {
				got_object_tree_close(subtree);
				break;
			}
		} else if (S_ISREG(got_tree_entry_get_mode(
		    s->selected_entry))) {
			struct tog_view *blame_view;
			int begin_x = view_is_parent_view(view) ?
			    view_split_begin_x(view->begin_x) : 0;

			err = blame_tree_entry(&blame_view, begin_x,
			    s->selected_entry, &s->parents,
			    s->commit_id, s->repo);
			if (err)
				break;
			view->count = 0;
			view->focussed = 0;
			blame_view->focussed = 1;
			if (view_is_parent_view(view)) {
				err = view_close_child(view);
				if (err)
					return err;
				err = view_set_child(view, blame_view);
				if (err)
					return err;
				view->focus_child = 1;
			} else
				*new_view = blame_view;
		}
		break;
	case KEY_RESIZE:
		if (view->nlines >= 4 && s->selected >= view->nlines - 3)
			s->selected = view->nlines - 4;
		view->count = 0;
		break;
	default:
		view->count = 0;
		break;
	}

	return err;
}

__dead static void
usage_tree(void)
{
	endwin();
	fprintf(stderr,
	    "usage: %s tree [-c commit] [-r repository-path] [path]\n",
	    getprogname());
	exit(1);
}

static const struct got_error *
cmd_tree(int argc, char *argv[])
{
	const struct got_error *error;
	struct got_repository *repo = NULL;
	struct got_worktree *worktree = NULL;
	char *cwd = NULL, *repo_path = NULL, *in_repo_path = NULL;
	struct got_object_id *commit_id = NULL;
	struct got_commit_object *commit = NULL;
	const char *commit_id_arg = NULL;
	char *label = NULL;
	struct got_reference *ref = NULL;
	const char *head_ref_name = NULL;
	int ch;
	struct tog_view *view;
	int *pack_fds = NULL;

	while ((ch = getopt(argc, argv, "c:r:")) != -1) {
		switch (ch) {
		case 'c':
			commit_id_arg = optarg;
			break;
		case 'r':
			repo_path = realpath(optarg, NULL);
			if (repo_path == NULL)
				return got_error_from_errno2("realpath",
				    optarg);
			break;
		default:
			usage_tree();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 1)
		usage_tree();

	error = got_repo_pack_fds_open(&pack_fds);
	if (error != NULL)
		goto done;

	if (repo_path == NULL) {
		cwd = getcwd(NULL, 0);
		if (cwd == NULL)
			return got_error_from_errno("getcwd");
		error = got_worktree_open(&worktree, cwd);
		if (error && error->code != GOT_ERR_NOT_WORKTREE)
			goto done;
		if (worktree)
			repo_path =
			    strdup(got_worktree_get_repo_path(worktree));
		else
			repo_path = strdup(cwd);
		if (repo_path == NULL) {
			error = got_error_from_errno("strdup");
			goto done;
		}
	}

	error = got_repo_open(&repo, repo_path, NULL, pack_fds);
	if (error != NULL)
		goto done;

	error = get_in_repo_path_from_argv0(&in_repo_path, argc, argv,
	    repo, worktree);
	if (error)
		goto done;

	init_curses();

	error = apply_unveil(got_repo_get_path(repo), NULL);
	if (error)
		goto done;

	error = tog_load_refs(repo, 0);
	if (error)
		goto done;

	if (commit_id_arg == NULL) {
		error = got_repo_match_object_id(&commit_id, &label,
		    worktree ? got_worktree_get_head_ref_name(worktree) :
		    GOT_REF_HEAD, GOT_OBJ_TYPE_COMMIT, &tog_refs, repo);
		if (error)
			goto done;
		head_ref_name = label;
	} else {
		error = got_ref_open(&ref, repo, commit_id_arg, 0);
		if (error == NULL)
			head_ref_name = got_ref_get_name(ref);
		else if (error->code != GOT_ERR_NOT_REF)
			goto done;
		error = got_repo_match_object_id(&commit_id, NULL,
		    commit_id_arg, GOT_OBJ_TYPE_COMMIT, &tog_refs, repo);
		if (error)
			goto done;
	}

	error = got_object_open_as_commit(&commit, repo, commit_id);
	if (error)
		goto done;

	view = view_open(0, 0, 0, 0, TOG_VIEW_TREE);
	if (view == NULL) {
		error = got_error_from_errno("view_open");
		goto done;
	}
	error = open_tree_view(view, commit_id, head_ref_name, repo);
	if (error)
		goto done;
	if (!got_path_is_root_dir(in_repo_path)) {
		error = tree_view_walk_path(&view->state.tree, commit,
		    in_repo_path);
		if (error)
			goto done;
	}

	if (worktree) {
		/* Release work tree lock. */
		got_worktree_close(worktree);
		worktree = NULL;
	}
	error = view_loop(view);
done:
	free(repo_path);
	free(cwd);
	free(commit_id);
	free(label);
	if (ref)
		got_ref_close(ref);
	if (repo) {
		const struct got_error *close_err = got_repo_close(repo);
		if (error == NULL)
			error = close_err;
	}
	if (pack_fds) {
		const struct got_error *pack_err =
		    got_repo_pack_fds_close(pack_fds);
		if (error == NULL)
			error = pack_err;
	}
	tog_free_refs();
	return error;
}

static const struct got_error *
ref_view_load_refs(struct tog_ref_view_state *s)
{
	struct got_reflist_entry *sre;
	struct tog_reflist_entry *re;

	s->nrefs = 0;
	TAILQ_FOREACH(sre, &tog_refs, entry) {
		if (strncmp(got_ref_get_name(sre->ref),
		    "refs/got/", 9) == 0 &&
		    strncmp(got_ref_get_name(sre->ref),
		    "refs/got/backup/", 16) != 0)
			continue;

		re = malloc(sizeof(*re));
		if (re == NULL)
			return got_error_from_errno("malloc");

		re->ref = got_ref_dup(sre->ref);
		if (re->ref == NULL)
			return got_error_from_errno("got_ref_dup");
		re->idx = s->nrefs++;
		TAILQ_INSERT_TAIL(&s->refs, re, entry);
	}

	s->first_displayed_entry = TAILQ_FIRST(&s->refs);
	return NULL;
}

static void
ref_view_free_refs(struct tog_ref_view_state *s)
{
	struct tog_reflist_entry *re;

	while (!TAILQ_EMPTY(&s->refs)) {
		re = TAILQ_FIRST(&s->refs);
		TAILQ_REMOVE(&s->refs, re, entry);
		got_ref_close(re->ref);
		free(re);
	}
}

static const struct got_error *
open_ref_view(struct tog_view *view, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct tog_ref_view_state *s = &view->state.ref;

	s->selected_entry = 0;
	s->repo = repo;

	TAILQ_INIT(&s->refs);
	STAILQ_INIT(&s->colors);

	err = ref_view_load_refs(s);
	if (err)
		return err;

	if (has_colors() && getenv("TOG_COLORS") != NULL) {
		err = add_color(&s->colors, "^refs/heads/",
		    TOG_COLOR_REFS_HEADS,
		    get_color_value("TOG_COLOR_REFS_HEADS"));
		if (err)
			goto done;

		err = add_color(&s->colors, "^refs/tags/",
		    TOG_COLOR_REFS_TAGS,
		    get_color_value("TOG_COLOR_REFS_TAGS"));
		if (err)
			goto done;

		err = add_color(&s->colors, "^refs/remotes/",
		    TOG_COLOR_REFS_REMOTES,
		    get_color_value("TOG_COLOR_REFS_REMOTES"));
		if (err)
			goto done;

		err = add_color(&s->colors, "^refs/got/backup/",
		    TOG_COLOR_REFS_BACKUP,
		    get_color_value("TOG_COLOR_REFS_BACKUP"));
		if (err)
			goto done;
	}

	view->show = show_ref_view;
	view->input = input_ref_view;
	view->close = close_ref_view;
	view->search_start = search_start_ref_view;
	view->search_next = search_next_ref_view;
done:
	if (err)
		free_colors(&s->colors);
	return err;
}

static const struct got_error *
close_ref_view(struct tog_view *view)
{
	struct tog_ref_view_state *s = &view->state.ref;

	ref_view_free_refs(s);
	free_colors(&s->colors);

	return NULL;
}

static const struct got_error *
resolve_reflist_entry(struct got_object_id **commit_id,
    struct tog_reflist_entry *re, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object_id *obj_id;
	struct got_tag_object *tag = NULL;
	int obj_type;

	*commit_id = NULL;

	err = got_ref_resolve(&obj_id, repo, re->ref);
	if (err)
		return err;

	err = got_object_get_type(&obj_type, repo, obj_id);
	if (err)
		goto done;

	switch (obj_type) {
	case GOT_OBJ_TYPE_COMMIT:
		*commit_id = obj_id;
		break;
	case GOT_OBJ_TYPE_TAG:
		err = got_object_open_as_tag(&tag, repo, obj_id);
		if (err)
			goto done;
		free(obj_id);
		err = got_object_get_type(&obj_type, repo,
		    got_object_tag_get_object_id(tag));
		if (err)
			goto done;
		if (obj_type != GOT_OBJ_TYPE_COMMIT) {
			err = got_error(GOT_ERR_OBJ_TYPE);
			goto done;
		}
		*commit_id = got_object_id_dup(
		    got_object_tag_get_object_id(tag));
		if (*commit_id == NULL) {
			err = got_error_from_errno("got_object_id_dup");
			goto done;
		}
		break;
	default:
		err = got_error(GOT_ERR_OBJ_TYPE);
		break;
	}

done:
	if (tag)
		got_object_tag_close(tag);
	if (err) {
		free(*commit_id);
		*commit_id = NULL;
	}
	return err;
}

static const struct got_error *
log_ref_entry(struct tog_view **new_view, int begin_x,
    struct tog_reflist_entry *re, struct got_repository *repo)
{
	struct tog_view *log_view;
	const struct got_error *err = NULL;
	struct got_object_id *commit_id = NULL;

	*new_view = NULL;

	err = resolve_reflist_entry(&commit_id, re, repo);
	if (err) {
		if (err->code != GOT_ERR_OBJ_TYPE)
			return err;
		else
			return NULL;
	}

	log_view = view_open(0, 0, 0, begin_x, TOG_VIEW_LOG);
	if (log_view == NULL) {
		err = got_error_from_errno("view_open");
		goto done;
	}

	err = open_log_view(log_view, commit_id, repo,
	    got_ref_get_name(re->ref), "", 0);
done:
	if (err)
		view_close(log_view);
	else
		*new_view = log_view;
	free(commit_id);
	return err;
}

static void
ref_scroll_up(struct tog_ref_view_state *s, int maxscroll)
{
	struct tog_reflist_entry *re;
	int i = 0;

	if (s->first_displayed_entry == TAILQ_FIRST(&s->refs))
		return;

	re = TAILQ_PREV(s->first_displayed_entry, tog_reflist_head, entry);
	while (i++ < maxscroll) {
		if (re == NULL)
			break;
		s->first_displayed_entry = re;
		re = TAILQ_PREV(re, tog_reflist_head, entry);
	}
}

static void
ref_scroll_down(struct tog_ref_view_state *s, int maxscroll)
{
	struct tog_reflist_entry *next, *last;
	int n = 0;

	if (s->first_displayed_entry)
		next = TAILQ_NEXT(s->first_displayed_entry, entry);
	else
		next = TAILQ_FIRST(&s->refs);

	last = s->last_displayed_entry;
	while (next && last && n++ < maxscroll) {
		last = TAILQ_NEXT(last, entry);
		if (last) {
			s->first_displayed_entry = next;
			next = TAILQ_NEXT(next, entry);
		}
	}
}

static const struct got_error *
search_start_ref_view(struct tog_view *view)
{
	struct tog_ref_view_state *s = &view->state.ref;

	s->matched_entry = NULL;
	return NULL;
}

static int
match_reflist_entry(struct tog_reflist_entry *re, regex_t *regex)
{
	regmatch_t regmatch;

	return regexec(regex, got_ref_get_name(re->ref), 1, &regmatch,
	    0) == 0;
}

static const struct got_error *
search_next_ref_view(struct tog_view *view)
{
	struct tog_ref_view_state *s = &view->state.ref;
	struct tog_reflist_entry *re = NULL;

	if (!view->searching) {
		view->search_next_done = TOG_SEARCH_HAVE_MORE;
		return NULL;
	}

	if (s->matched_entry) {
		if (view->searching == TOG_SEARCH_FORWARD) {
			if (s->selected_entry)
				re = TAILQ_NEXT(s->selected_entry, entry);
			else
				re = TAILQ_PREV(s->selected_entry,
				    tog_reflist_head, entry);
		} else {
			if (s->selected_entry == NULL)
				re = TAILQ_LAST(&s->refs, tog_reflist_head);
			else
				re = TAILQ_PREV(s->selected_entry,
				    tog_reflist_head, entry);
		}
	} else {
		if (s->selected_entry)
			re = s->selected_entry;
		else if (view->searching == TOG_SEARCH_FORWARD)
			re = TAILQ_FIRST(&s->refs);
		else
			re = TAILQ_LAST(&s->refs, tog_reflist_head);
	}

	while (1) {
		if (re == NULL) {
			if (s->matched_entry == NULL) {
				view->search_next_done = TOG_SEARCH_HAVE_MORE;
				return NULL;
			}
			if (view->searching == TOG_SEARCH_FORWARD)
				re = TAILQ_FIRST(&s->refs);
			else
				re = TAILQ_LAST(&s->refs, tog_reflist_head);
		}

		if (match_reflist_entry(re, &view->regex)) {
			view->search_next_done = TOG_SEARCH_HAVE_MORE;
			s->matched_entry = re;
			break;
		}

		if (view->searching == TOG_SEARCH_FORWARD)
			re = TAILQ_NEXT(re, entry);
		else
			re = TAILQ_PREV(re, tog_reflist_head, entry);
	}

	if (s->matched_entry) {
		s->first_displayed_entry = s->matched_entry;
		s->selected = 0;
	}

	return NULL;
}

static const struct got_error *
show_ref_view(struct tog_view *view)
{
	const struct got_error *err = NULL;
	struct tog_ref_view_state *s = &view->state.ref;
	struct tog_reflist_entry *re;
	char *line = NULL;
	wchar_t *wline;
	struct tog_color *tc;
	int width, n;
	int limit = view->nlines;

	werase(view->window);

	s->ndisplayed = 0;

	if (limit == 0)
		return NULL;

	re = s->first_displayed_entry;

	if (asprintf(&line, "references [%d/%d]", re->idx + s->selected + 1,
	    s->nrefs) == -1)
		return got_error_from_errno("asprintf");

	err = format_line(&wline, &width, NULL, line, 0, view->ncols, 0, 0);
	if (err) {
		free(line);
		return err;
	}
	if (view_needs_focus_indication(view))
		wstandout(view->window);
	waddwstr(view->window, wline);
	if (view_needs_focus_indication(view))
		wstandend(view->window);
	free(wline);
	wline = NULL;
	free(line);
	line = NULL;
	if (width < view->ncols - 1)
		waddch(view->window, '\n');
	if (--limit <= 0)
		return NULL;

	n = 0;
	while (re && limit > 0) {
		char *line = NULL;
		char ymd[13];  /* YYYY-MM-DD + "  " + NUL */

		if (s->show_date) {
			struct got_commit_object *ci;
			struct got_tag_object *tag;
			struct got_object_id *id;
			struct tm tm;
			time_t t;

			err = got_ref_resolve(&id, s->repo, re->ref);
			if (err)
				return err;
			err = got_object_open_as_tag(&tag, s->repo, id);
			if (err) {
				if (err->code != GOT_ERR_OBJ_TYPE) {
					free(id);
					return err;
				}
				err = got_object_open_as_commit(&ci, s->repo,
				    id);
				if (err) {
					free(id);
					return err;
				}
				t = got_object_commit_get_committer_time(ci);
				got_object_commit_close(ci);
			} else {
				t = got_object_tag_get_tagger_time(tag);
				got_object_tag_close(tag);
			}
			free(id);
			if (gmtime_r(&t, &tm) == NULL)
				return got_error_from_errno("gmtime_r");
			if (strftime(ymd, sizeof(ymd), "%G-%m-%d  ", &tm) == 0)
				return got_error(GOT_ERR_NO_SPACE);
		}
		if (got_ref_is_symbolic(re->ref)) {
			if (asprintf(&line, "%s%s -> %s", s->show_date ?
			    ymd : "", got_ref_get_name(re->ref),
			    got_ref_get_symref_target(re->ref)) == -1)
				return got_error_from_errno("asprintf");
		} else if (s->show_ids) {
			struct got_object_id *id;
			char *id_str;
			err = got_ref_resolve(&id, s->repo, re->ref);
			if (err)
				return err;
			err = got_object_id_str(&id_str, id);
			if (err) {
				free(id);
				return err;
			}
			if (asprintf(&line, "%s%s: %s", s->show_date ? ymd : "",
			    got_ref_get_name(re->ref), id_str) == -1) {
				err = got_error_from_errno("asprintf");
				free(id);
				free(id_str);
				return err;
			}
			free(id);
			free(id_str);
		} else if (asprintf(&line, "%s%s", s->show_date ? ymd : "",
		    got_ref_get_name(re->ref)) == -1)
			return got_error_from_errno("asprintf");

		err = format_line(&wline, &width, NULL, line, 0, view->ncols,
		    0, 0);
		if (err) {
			free(line);
			return err;
		}
		if (n == s->selected) {
			if (view->focussed)
				wstandout(view->window);
			s->selected_entry = re;
		}
		tc = match_color(&s->colors, got_ref_get_name(re->ref));
		if (tc)
			wattr_on(view->window,
			    COLOR_PAIR(tc->colorpair), NULL);
		waddwstr(view->window, wline);
		if (tc)
			wattr_off(view->window,
			    COLOR_PAIR(tc->colorpair), NULL);
		if (width < view->ncols - 1)
			waddch(view->window, '\n');
		if (n == s->selected && view->focussed)
			wstandend(view->window);
		free(line);
		free(wline);
		wline = NULL;
		n++;
		s->ndisplayed++;
		s->last_displayed_entry = re;

		limit--;
		re = TAILQ_NEXT(re, entry);
	}

	view_vborder(view);
	return err;
}

static const struct got_error *
browse_ref_tree(struct tog_view **new_view, int begin_x,
    struct tog_reflist_entry *re, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object_id *commit_id = NULL;
	struct tog_view *tree_view;

	*new_view = NULL;

	err = resolve_reflist_entry(&commit_id, re, repo);
	if (err) {
		if (err->code != GOT_ERR_OBJ_TYPE)
			return err;
		else
			return NULL;
	}


	tree_view = view_open(0, 0, 0, begin_x, TOG_VIEW_TREE);
	if (tree_view == NULL) {
		err = got_error_from_errno("view_open");
		goto done;
	}

	err = open_tree_view(tree_view, commit_id,
	    got_ref_get_name(re->ref), repo);
	if (err)
		goto done;

	*new_view = tree_view;
done:
	free(commit_id);
	return err;
}
static const struct got_error *
input_ref_view(struct tog_view **new_view, struct tog_view *view, int ch)
{
	const struct got_error *err = NULL;
	struct tog_ref_view_state *s = &view->state.ref;
	struct tog_view *log_view, *tree_view;
	struct tog_reflist_entry *re;
	int begin_x = 0, n, nscroll = view->nlines - 1;

	switch (ch) {
	case 'i':
		s->show_ids = !s->show_ids;
		view->count = 0;
		break;
	case 'm':
		s->show_date = !s->show_date;
		view->count = 0;
		break;
	case 'o':
		s->sort_by_date = !s->sort_by_date;
		view->count = 0;
		err = got_reflist_sort(&tog_refs, s->sort_by_date ?
		    got_ref_cmp_by_commit_timestamp_descending :
		    tog_ref_cmp_by_name, s->repo);
		if (err)
			break;
		got_reflist_object_id_map_free(tog_refs_idmap);
		err = got_reflist_object_id_map_create(&tog_refs_idmap,
		    &tog_refs, s->repo);
		if (err)
			break;
		ref_view_free_refs(s);
		err = ref_view_load_refs(s);
		break;
	case KEY_ENTER:
	case '\r':
		view->count = 0;
		if (!s->selected_entry)
			break;
		if (view_is_parent_view(view))
			begin_x = view_split_begin_x(view->begin_x);
		err = log_ref_entry(&log_view, begin_x, s->selected_entry,
		    s->repo);
		view->focussed = 0;
		log_view->focussed = 1;
		if (view_is_parent_view(view)) {
			err = view_close_child(view);
			if (err)
				return err;
			err = view_set_child(view, log_view);
			if (err)
				return err;
			view->focus_child = 1;
		} else
			*new_view = log_view;
		break;
	case 't':
		view->count = 0;
		if (!s->selected_entry)
			break;
		if (view_is_parent_view(view))
			begin_x = view_split_begin_x(view->begin_x);
		err = browse_ref_tree(&tree_view, begin_x, s->selected_entry,
		    s->repo);
		if (err || tree_view == NULL)
			break;
		view->focussed = 0;
		tree_view->focussed = 1;
		if (view_is_parent_view(view)) {
			err = view_close_child(view);
			if (err)
				return err;
			err = view_set_child(view, tree_view);
			if (err)
				return err;
			view->focus_child = 1;
		} else
			*new_view = tree_view;
		break;
	case 'g':
	case KEY_HOME:
		s->selected = 0;
		view->count = 0;
		s->first_displayed_entry = TAILQ_FIRST(&s->refs);
		break;
	case 'G':
	case KEY_END:
		s->selected = 0;
		view->count = 0;
		re = TAILQ_LAST(&s->refs, tog_reflist_head);
		for (n = 0; n < view->nlines - 1; n++) {
			if (re == NULL)
				break;
			s->first_displayed_entry = re;
			re = TAILQ_PREV(re, tog_reflist_head, entry);
		}
		if (n > 0)
			s->selected = n - 1;
		break;
	case 'k':
	case KEY_UP:
	case CTRL('p'):
		if (s->selected > 0) {
			s->selected--;
			break;
		}
		ref_scroll_up(s, 1);
		if (s->selected_entry == TAILQ_FIRST(&s->refs))
			view->count = 0;
		break;
	case CTRL('u'):
	case 'u':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
	case 'b':
		if (s->first_displayed_entry == TAILQ_FIRST(&s->refs))
			s->selected -= MIN(nscroll, s->selected);
		ref_scroll_up(s, MAX(0, nscroll));
		if (s->selected_entry == TAILQ_FIRST(&s->refs))
			view->count = 0;
		break;
	case 'j':
	case KEY_DOWN:
	case CTRL('n'):
		if (s->selected < s->ndisplayed - 1) {
			s->selected++;
			break;
		}
		if (TAILQ_NEXT(s->last_displayed_entry, entry) == NULL) {
			/* can't scroll any further */
			view->count = 0;
			break;
		}
		ref_scroll_down(s, 1);
		break;
	case CTRL('d'):
	case 'd':
		nscroll /= 2;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'):
	case 'f':
	case ' ':
		if (TAILQ_NEXT(s->last_displayed_entry, entry) == NULL) {
			/* can't scroll any further; move cursor down */
			if (s->selected < s->ndisplayed - 1)
				s->selected += MIN(nscroll,
				    s->ndisplayed - s->selected - 1);
			if (view->count > 1 && s->selected < s->ndisplayed - 1)
				s->selected += s->ndisplayed - s->selected - 1;
			view->count = 0;
			break;
		}
		ref_scroll_down(s, nscroll);
		break;
	case CTRL('l'):
		view->count = 0;
		tog_free_refs();
		err = tog_load_refs(s->repo, s->sort_by_date);
		if (err)
			break;
		ref_view_free_refs(s);
		err = ref_view_load_refs(s);
		break;
	case KEY_RESIZE:
		if (view->nlines >= 2 && s->selected >= view->nlines - 1)
			s->selected = view->nlines - 2;
		break;
	default:
		view->count = 0;
		break;
	}

	return err;
}

__dead static void
usage_ref(void)
{
	endwin();
	fprintf(stderr, "usage: %s ref [-r repository-path]\n",
	    getprogname());
	exit(1);
}

static const struct got_error *
cmd_ref(int argc, char *argv[])
{
	const struct got_error *error;
	struct got_repository *repo = NULL;
	struct got_worktree *worktree = NULL;
	char *cwd = NULL, *repo_path = NULL;
	int ch;
	struct tog_view *view;
	int *pack_fds = NULL;

	while ((ch = getopt(argc, argv, "r:")) != -1) {
		switch (ch) {
		case 'r':
			repo_path = realpath(optarg, NULL);
			if (repo_path == NULL)
				return got_error_from_errno2("realpath",
				    optarg);
			break;
		default:
			usage_ref();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 1)
		usage_ref();

	error = got_repo_pack_fds_open(&pack_fds);
	if (error != NULL)
		goto done;

	if (repo_path == NULL) {
		cwd = getcwd(NULL, 0);
		if (cwd == NULL)
			return got_error_from_errno("getcwd");
		error = got_worktree_open(&worktree, cwd);
		if (error && error->code != GOT_ERR_NOT_WORKTREE)
			goto done;
		if (worktree)
			repo_path =
			    strdup(got_worktree_get_repo_path(worktree));
		else
			repo_path = strdup(cwd);
		if (repo_path == NULL) {
			error = got_error_from_errno("strdup");
			goto done;
		}
	}

	error = got_repo_open(&repo, repo_path, NULL, pack_fds);
	if (error != NULL)
		goto done;

	init_curses();

	error = apply_unveil(got_repo_get_path(repo), NULL);
	if (error)
		goto done;

	error = tog_load_refs(repo, 0);
	if (error)
		goto done;

	view = view_open(0, 0, 0, 0, TOG_VIEW_REF);
	if (view == NULL) {
		error = got_error_from_errno("view_open");
		goto done;
	}

	error = open_ref_view(view, repo);
	if (error)
		goto done;

	if (worktree) {
		/* Release work tree lock. */
		got_worktree_close(worktree);
		worktree = NULL;
	}
	error = view_loop(view);
done:
	free(repo_path);
	free(cwd);
	if (repo) {
		const struct got_error *close_err = got_repo_close(repo);
		if (close_err)
			error = close_err;
	}
	if (pack_fds) {
		const struct got_error *pack_err =
		    got_repo_pack_fds_close(pack_fds);
		if (error == NULL)
			error = pack_err;
	}
	tog_free_refs();
	return error;
}

static void
list_commands(FILE *fp)
{
	size_t i;

	fprintf(fp, "commands:");
	for (i = 0; i < nitems(tog_commands); i++) {
		const struct tog_cmd *cmd = &tog_commands[i];
		fprintf(fp, " %s", cmd->name);
	}
	fputc('\n', fp);
}

__dead static void
usage(int hflag, int status)
{
	FILE *fp = (status == 0) ? stdout : stderr;

	fprintf(fp, "usage: %s [-h] [-V | --version] [command] [arg ...]\n",
	    getprogname());
	if (hflag) {
		fprintf(fp, "lazy usage: %s path\n", getprogname());
		list_commands(fp);
	}
	exit(status);
}

static char **
make_argv(int argc, ...)
{
	va_list ap;
	char **argv;
	int i;

	va_start(ap, argc);

	argv = calloc(argc, sizeof(char *));
	if (argv == NULL)
		err(1, "calloc");
	for (i = 0; i < argc; i++) {
		argv[i] = strdup(va_arg(ap, char *));
		if (argv[i] == NULL)
			err(1, "strdup");
	}

	va_end(ap);
	return argv;
}

/*
 * Try to convert 'tog path' into a 'tog log path' command.
 * The user could simply have mistyped the command rather than knowingly
 * provided a path. So check whether argv[0] can in fact be resolved
 * to a path in the HEAD commit and print a special error if not.
 * This hack is for mpi@ <3
 */
static const struct got_error *
tog_log_with_path(int argc, char *argv[])
{
	const struct got_error *error = NULL, *close_err;
	const struct tog_cmd *cmd = NULL;
	struct got_repository *repo = NULL;
	struct got_worktree *worktree = NULL;
	struct got_object_id *commit_id = NULL, *id = NULL;
	struct got_commit_object *commit = NULL;
	char *cwd = NULL, *repo_path = NULL, *in_repo_path = NULL;
	char *commit_id_str = NULL, **cmd_argv = NULL;
	int *pack_fds = NULL;

	cwd = getcwd(NULL, 0);
	if (cwd == NULL)
		return got_error_from_errno("getcwd");

	error = got_repo_pack_fds_open(&pack_fds);
	if (error != NULL)
		goto done;

	error = got_worktree_open(&worktree, cwd);
	if (error && error->code != GOT_ERR_NOT_WORKTREE)
		goto done;

	if (worktree)
		repo_path = strdup(got_worktree_get_repo_path(worktree));
	else
		repo_path = strdup(cwd);
	if (repo_path == NULL) {
		error = got_error_from_errno("strdup");
		goto done;
	}

	error = got_repo_open(&repo, repo_path, NULL, pack_fds);
	if (error != NULL)
		goto done;

	error = get_in_repo_path_from_argv0(&in_repo_path, argc, argv,
	    repo, worktree);
	if (error)
		goto done;

	error = tog_load_refs(repo, 0);
	if (error)
		goto done;
	error = got_repo_match_object_id(&commit_id, NULL, worktree ?
	    got_worktree_get_head_ref_name(worktree) : GOT_REF_HEAD,
	    GOT_OBJ_TYPE_COMMIT, &tog_refs, repo);
	if (error)
		goto done;

	if (worktree) {
		got_worktree_close(worktree);
		worktree = NULL;
	}

	error = got_object_open_as_commit(&commit, repo, commit_id);
	if (error)
		goto done;

	error = got_object_id_by_path(&id, repo, commit, in_repo_path);
	if (error) {
		if (error->code != GOT_ERR_NO_TREE_ENTRY)
			goto done;
		fprintf(stderr, "%s: '%s' is no known command or path\n",
		    getprogname(), argv[0]);
		usage(1, 1);
		/* not reached */
	}

	close_err = got_repo_close(repo);
	if (error == NULL)
		error = close_err;
	repo = NULL;

	error = got_object_id_str(&commit_id_str, commit_id);
	if (error)
		goto done;

	cmd = &tog_commands[0]; /* log */
	argc = 4;
	cmd_argv = make_argv(argc, cmd->name, "-c", commit_id_str, argv[0]);
	error = cmd->cmd_main(argc, cmd_argv);
done:
	if (repo) {
		close_err = got_repo_close(repo);
		if (error == NULL)
			error = close_err;
	}
	if (commit)
		got_object_commit_close(commit);
	if (worktree)
		got_worktree_close(worktree);
	if (pack_fds) {
		const struct got_error *pack_err =
		    got_repo_pack_fds_close(pack_fds);
		if (error == NULL)
			error = pack_err;
	}
	free(id);
	free(commit_id_str);
	free(commit_id);
	free(cwd);
	free(repo_path);
	free(in_repo_path);
	if (cmd_argv) {
		int i;
		for (i = 0; i < argc; i++)
			free(cmd_argv[i]);
		free(cmd_argv);
	}
	tog_free_refs();
	return error;
}

int
main(int argc, char *argv[])
{
	const struct got_error *error = NULL;
	const struct tog_cmd *cmd = NULL;
	int ch, hflag = 0, Vflag = 0;
	char **cmd_argv = NULL;
	static const struct option longopts[] = {
	    { "version", no_argument, NULL, 'V' },
	    { NULL, 0, NULL, 0}
	};

	setlocale(LC_CTYPE, "");

	while ((ch = getopt_long(argc, argv, "+hV", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			hflag = 1;
			break;
		case 'V':
			Vflag = 1;
			break;
		default:
			usage(hflag, 1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;
	optind = 1;
	optreset = 1;

	if (Vflag) {
		got_version_print_str();
		return 0;
	}

#ifndef PROFILE
	if (pledge("stdio rpath wpath cpath flock proc tty exec sendfd unveil",
	    NULL) == -1)
		err(1, "pledge");
#endif

	if (argc == 0) {
		if (hflag)
			usage(hflag, 0);
		/* Build an argument vector which runs a default command. */
		cmd = &tog_commands[0];
		argc = 1;
		cmd_argv = make_argv(argc, cmd->name);
	} else {
		size_t i;

		/* Did the user specify a command? */
		for (i = 0; i < nitems(tog_commands); i++) {
			if (strncmp(tog_commands[i].name, argv[0],
			    strlen(argv[0])) == 0) {
				cmd = &tog_commands[i];
				break;
			}
		}
	}

	if (cmd == NULL) {
		if (argc != 1)
			usage(0, 1);
		/* No command specified; try log with a path */
		error = tog_log_with_path(argc, argv);
	} else {
		if (hflag)
			cmd->cmd_usage();
		else
			error = cmd->cmd_main(argc, cmd_argv ? cmd_argv : argv);
	}

	endwin();
	putchar('\n');
	if (cmd_argv) {
		int i;
		for (i = 0; i < argc; i++)
			free(cmd_argv[i]);
		free(cmd_argv);
	}

	if (error && error->code != GOT_ERR_CANCELLED)
		fprintf(stderr, "%s: %s\n", getprogname(), error->msg);
	return 0;
}
