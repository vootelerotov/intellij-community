/*
 * Copyright 2000-2016 JetBrains s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fsnotifier.h"

#include <errno.h>
#include <limits.h>
#include <mntent.h>
#include <paths.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define LOG_ENV "FSNOTIFIER_LOG_LEVEL"
#define LOG_ENV_DEBUG "debug"
#define LOG_ENV_INFO "info"
#define LOG_ENV_WARNING "warning"
#define LOG_ENV_ERROR "error"
#define LOG_ENV_OFF "off"


#define USAGE_MSG \
    "fsnotifier - IntelliJ IDEA companion program for watching and reporting file and directory structure modifications.\n\n" \
    "fsnotifier utilizes \"user\" facility of syslog(3) - messages usually can be found in /var/log/user.log.\n" \
    "Verbosity is regulated via " LOG_ENV " environment variable, possible values are: " \
    LOG_ENV_DEBUG ", " LOG_ENV_INFO ", " LOG_ENV_WARNING ", " LOG_ENV_ERROR ", " LOG_ENV_OFF "; default is " LOG_ENV_WARNING ".\n\n" \
    "Use 'fsnotifier --selftest' to perform some self-diagnostics (output will be logged and printed to console).\n"

#define HELP_MSG \
    "Try 'fsnotifier --help' for more information.\n"

#define INSTANCE_LIMIT_TEXT \
    "The <b>inotify</b>(7) instances limit reached. " \
    "<a href=\"https://confluence.jetbrains.com/display/IDEADEV/Inotify+Instances+Limit\">More details.</a>\n"

#define WATCH_LIMIT_TEXT \
    "The current <b>inotify</b>(7) watch limit is too low. " \
    "<a href=\"https://confluence.jetbrains.com/display/IDEADEV/Inotify+Watches+Limit\">More details.</a>\n"

#define MISSING_ROOT_TIMEOUT 1

#define UNFLATTEN(root) (root[0] == '|' ? root + 1 : root)

struct string_cmp {
  bool operator() (const char* lhs, const char* rhs) const {
    return strcmp(lhs, rhs) > 0;
  }
};

typedef struct {
  char* path;
  int id;  // negative value means missing root
} watch_root;

static vector<watch_root*> *roots = NULL;
static set<char*, string_cmp>* roots_as_paths = NULL;

static int log_level = 0;
static bool self_test = false;

static void init_log();
static void run_self_test();
static bool main_loop();
static int read_input();
static bool update_roots(set<char*, string_cmp>* new_roots);
static void unregister_roots(set<char*, string_cmp>* to_remove);
static bool register_roots(set<char*, string_cmp>* new_roots, vector<char*>* unwatchable, vector<char*>* mounts);
static vector<char*>* unwatchable_mounts();
static void inotify_callback(const char* path, int event);
static void report_event(const char* event, const char* path);
static void output(const char* format, ...);
static void check_missing_roots();
static void check_root_removal(const char*);


int main(int argc, char** argv) {
  if (argc > 1) {
    if (strcmp(argv[1], "--help") == 0) {
      printf(USAGE_MSG);
      return 0;
    }
    else if (strcmp(argv[1], "--version") == 0) {
      printf("fsnotifier " VERSION "\n");
      return 0;
    }
    else if (strcmp(argv[1], "--selftest") == 0) {
      self_test = true;
    }
    else {
      printf("unrecognized option: %s\n", argv[1]);
      printf(HELP_MSG);
      return 1;
    }
  }

  init_log();
  if (!self_test) {
    userlog(LOG_INFO, "started (v." VERSION ")");
  }
  else {
    userlog(LOG_INFO, "started (self-test mode) (v." VERSION ")");
  }

  setvbuf(stdin, NULL, _IONBF, 0);

  int rv = 0;
  roots = new vector<watch_root*>(20);
  roots_as_paths = new set<char*, string_cmp>();
  if (roots != NULL && init_inotify()) {
    set_inotify_callback(&inotify_callback);

    if (!self_test) {
      if (!main_loop()) {
        rv = 3;
      }
    }
    else {
      run_self_test();
    }

    unregister_roots(roots_as_paths);
  }
  else {
    output("GIVEUP\n");
    rv = 2;
  }
  close_inotify();
  delete(roots);

  userlog(LOG_INFO, "finished (%d)", rv);
  closelog();

  return rv;
}


static void init_log() {
  int level = LOG_WARNING;

  char* env_level = getenv(LOG_ENV);
  if (env_level != NULL) {
    if (strcmp(env_level, LOG_ENV_DEBUG) == 0)  level = LOG_DEBUG;
    else if (strcmp(env_level, LOG_ENV_INFO) == 0)  level = LOG_INFO;
    else if (strcmp(env_level, LOG_ENV_WARNING) == 0)  level = LOG_WARNING;
    else if (strcmp(env_level, LOG_ENV_ERROR) == 0)  level = LOG_ERR;
  }

  if (self_test) {
    level = LOG_DEBUG;
  }

  char ident[32];
  snprintf(ident, sizeof(ident), "fsnotifier[%d]", getpid());
  openlog(ident, 0, LOG_USER);
  log_level = level;
}


void message(MSG id) {
  if (id == MSG_INSTANCE_LIMIT) {
    output("MESSAGE\n" INSTANCE_LIMIT_TEXT);
  }
  else if (id == MSG_WATCH_LIMIT) {
    output("MESSAGE\n" WATCH_LIMIT_TEXT);
  }
  else {
    userlog(LOG_ERR, "unknown message: %d", id);
  }
}


void userlog(int priority, const char* format, ...) {
  if (priority > log_level) {
    return;
  }

  va_list ap;
  va_start(ap, format);
  vsyslog(priority, format, ap);
  va_end(ap);

  if (self_test) {
    const char* level = "debug";
    switch (priority) {
      case LOG_ERR:  level = "error"; break;
      case LOG_WARNING:  level = " warn"; break;
      case LOG_INFO:  level = " info"; break;
    }
    printf("fsnotifier[%d] %s: ", getpid(), level);

    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);

    printf("\n");
  }
}


static void run_self_test() {
  set<char*, string_cmp>* test_roots = new set<char*, string_cmp>();
  char* cwd = (char*) malloc(PATH_MAX);
  if (getcwd(cwd, PATH_MAX) == NULL) {
    strncpy(cwd, ".", PATH_MAX);
  }
  test_roots->insert(cwd);
  update_roots(test_roots);
}


static bool main_loop() {
  int input_fd = fileno(stdin), inotify_fd = get_inotify_fd();
  int nfds = (inotify_fd > input_fd ? inotify_fd : input_fd) + 1;
  fd_set rfds;
  struct timeval timeout;

  while (true) {
    usleep(50000);

    FD_ZERO(&rfds);
    FD_SET(input_fd, &rfds);
    FD_SET(inotify_fd, &rfds);
    timeout = (struct timeval){MISSING_ROOT_TIMEOUT, 0};

    if (select(nfds, &rfds, NULL, NULL, &timeout) < 0) {
      if (errno != EINTR) {
        userlog(LOG_ERR, "select: %s", strerror(errno));
        return false;
      }
    }
    else if (FD_ISSET(input_fd, &rfds)) {
      int result = read_input();
      if (result == 0) return true;
      else if (result != ERR_CONTINUE) return false;
    }
    else if (FD_ISSET(inotify_fd, &rfds)) {
      if (!process_inotify_input()) return false;
    }
    else {
      check_missing_roots();
    }
  }
}


static int read_input() {
  char* line = read_line(stdin);
  userlog(LOG_DEBUG, "input: %s", (line ? line : "<null>"));

  if (line == NULL || strcmp(line, "EXIT") == 0) {
    userlog(LOG_INFO, "exiting: %s", line);
    return 0;
  }

  if (strcmp(line, "ROOTS") == 0) {
    set<char*, string_cmp>* new_roots = new set<char*, string_cmp>();
    CHECK_NULL(new_roots, ERR_ABORT);

    while (1) {
      line = read_line(stdin);
      userlog(LOG_DEBUG, "input: %s", (line ? line : "<null>"));
      if (line == NULL || strlen(line) == 0) {
        return 0;
      }
      else if (strcmp(line, "#") == 0) {
        break;
      }
      else {
        int l = strlen(line);
        if (l > 1 && line[l-1] == '/')  line[l-1] = '\0';
        new_roots->insert(strdup(line));

      }
    }

    return update_roots(new_roots) ? ERR_CONTINUE : ERR_ABORT;
  }

  userlog(LOG_WARNING, "unrecognised command: %s", line);
  return ERR_CONTINUE;
}

static bool update_roots(set<char*, string_cmp>* new_roots) {
  userlog(LOG_INFO, "updating roots (curr:%d, new:%d)", roots_as_paths->size(), new_roots->size());

  if (new_roots->size() == 1 && new_roots->find("/") != new_roots->end()) {  // refuse to watch entire tree
    output("UNWATCHEABLE\n/\n#\n");
    userlog(LOG_INFO, "unwatchable: /");
    unregister_roots(roots_as_paths);
    set_delete_vs_data(roots_as_paths);
    roots_as_paths = new set<char*, string_cmp>();
    return true;
  }

  vector<char*> *mounts = unwatchable_mounts();
  if (mounts == NULL) {
    return false;
  }

  set<char*, string_cmp>* to_add = new set<char*, string_cmp>();

  if (!set_difference(roots_as_paths, new_roots, to_add)) {
    return false;
  }

  set<char*, string_cmp>* to_remove = new set<char*, string_cmp>();

  if (!set_difference(new_roots, roots_as_paths, to_remove)) {
    return false;
  }

  vector<char*> *unwatchable = new vector<char*>(20);

  if (!register_roots(to_add, unwatchable, mounts)) {
    return false;
  }

  unregister_roots(to_remove);

  output("UNWATCHEABLE\n");
  for (int i=0; i<unwatchable->size(); i++) {
    char* s = unwatchable->at(i);
    output("%s\n", s);
    userlog(LOG_INFO, "unwatchable: %s", s);
  }
  output("#\n");

  delete(to_add);
  delete(to_remove);

  set_delete_vs_data(roots_as_paths);
  roots_as_paths = new_roots;

  vector_delete_vs_data(unwatchable);
  vector_delete_vs_data(mounts);

  return true;
}


static void unregister_roots(set<char*, string_cmp>* to_remove) {
  if (to_remove->size() == 0) {
    return;
  }

  watch_root* root;
  vector<watch_root*> *temp = new vector<watch_root*>(roots->size());
  while (roots->size() > 0) {
    root = roots->back();
    roots->pop_back();
    if (to_remove->find(root->path) != to_remove->end()) {
      userlog(LOG_INFO, "unregistering root: %s", root->path);
      unwatch(root->id);
      free(root->path);
      free(root);
    }
    else {
      temp->push_back(root);
    }
  }
  int tmp_array_size = temp->size();
  for (int j = 0; j < tmp_array_size; j++) {
    roots->push_back(temp->back());
    temp->pop_back();
  }

  delete(temp);
}


static bool register_roots(set<char*, string_cmp>* new_roots, vector<char*> *unwatchable, vector<char*> *mounts) {
  char* new_root = NULL;
  for (set<char*, string_cmp>::iterator itr = new_roots->begin(); itr != new_roots->end(); itr++) {
    new_root = *itr;
    char* unflattened = UNFLATTEN(new_root);
    userlog(LOG_INFO, "registering root: %s", new_root);

    if (unflattened[0] != '/') {
      userlog(LOG_WARNING, "invalid root: %s", new_root);
      continue;
    }

    vector<char*> *inner_mounts = new vector<char*>(5);
    CHECK_NULL(inner_mounts, false);

    bool skip = false;
    for (int j=0; j<mounts->size(); j++) {
      char* mount = mounts->at(j);
      if (is_parent_path(mount, unflattened)) {
        userlog(LOG_INFO, "watch root '%s' is under mount point '%s' - skipping", unflattened, mount);
        unwatchable->push_back(strdup(unflattened));
        skip = true;
        break;
      }
      else if (is_parent_path(unflattened, mount)) {
        userlog(LOG_INFO, "watch root '%s' contains mount point '%s' - partial watch", unflattened, mount);
        char* copy = strdup(mount);
        unwatchable->push_back(copy);
        inner_mounts->push_back(copy);
      }
    }
    if (skip) {
      continue;
    }

    int id = watch(new_root, inner_mounts);
    delete(inner_mounts);

    if (id >= 0 || id == ERR_MISSING) {
      watch_root* root = (watch_root*) malloc(sizeof(watch_root));
      CHECK_NULL(root, false);
      root->id = id;
      root->path = strdup(new_root);
      CHECK_NULL(root->path, false);
      roots->push_back(root);
    }
    else if (id == ERR_ABORT) {
      return false;
    }
    else if (id != ERR_IGNORE) {
      userlog(LOG_WARNING, "watch root '%s' cannot be watched: %d", unflattened, id);
      unwatchable->push_back(strdup(unflattened));
    }
  }

  return true;
}

static bool is_watchable(const char* fs) {
  // don't watch special and network filesystems
  return !(strncmp(fs, "dev", 3) == 0 || strcmp(fs, "proc") == 0 || strcmp(fs, "sysfs") == 0 || strcmp(fs, MNTTYPE_SWAP) == 0 ||
           (strncmp(fs, "fuse", 4) == 0 && strcmp(fs, "fuseblk") != 0) ||
           strcmp(fs, "cifs") == 0 || strcmp(fs, MNTTYPE_NFS) == 0);
}

static vector<char*>* unwatchable_mounts() {
  FILE* mtab = setmntent(_PATH_MOUNTED, "r");
  if (mtab == NULL) {
    userlog(LOG_ERR, "cannot open " _PATH_MOUNTED);
    return NULL;
  }

  vector<char*> *mounts = new vector<char*>(20);
  CHECK_NULL(mounts, NULL);

  struct mntent* ent;
  while ((ent = getmntent(mtab)) != NULL) {
    userlog(LOG_DEBUG, "mtab: %s : %s", ent->mnt_dir, ent->mnt_type);
    if (strcmp(ent->mnt_type, MNTTYPE_IGNORE) != 0 && !is_watchable(ent->mnt_type)) {
      mounts->push_back(strdup(ent->mnt_dir));
    }
  }

  endmntent(mtab);
  return mounts;
}


static void inotify_callback(const char* path, int event) {
  if (event & (IN_CREATE | IN_MOVED_TO)) {
    report_event("CREATE", path);
    report_event("CHANGE", path);
  }
  else if (event & IN_MODIFY) {
    report_event("CHANGE", path);
  }
  else if (event & IN_ATTRIB) {
    report_event("STATS", path);
  }
  else if (event & (IN_DELETE | IN_MOVED_FROM)) {
    report_event("DELETE", path);
  }
  if (event & (IN_DELETE_SELF | IN_MOVE_SELF)) {
    check_root_removal(path);
  }
  else if (event & IN_UNMOUNT) {
    output("RESET\n");
    userlog(LOG_DEBUG, "RESET");
  }
}

static void report_event(const char* event, const char* path) {
  userlog(LOG_DEBUG, "%s: %s", event, path);

  char* copy = (char*) path, *p;
  for (p = copy; *p != '\0'; ++p) {
    if (*p == '\n') {
      if (copy == path) {
        copy = strdup(path);
        p = copy + (p - path);
      }
      *p = '\0';
    }
  }

  fputs(event, stdout);
  fputc('\n', stdout);
  fwrite(copy, (p - copy), 1, stdout);
  fputc('\n', stdout);

  if (copy != path) {
    free(copy);
  }

  fflush(stdout);
}


static void output(const char* format, ...) {
  if (self_test) {
    return;
  }

  va_list ap;
  va_start(ap, format);
  vprintf(format, ap);
  va_end(ap);

  fflush(stdout);
}


static void check_missing_roots() {
  struct stat st;
  for (int i=0; i<roots->size(); i++) {
    watch_root* root = roots->at(i);
    if (root->id < 0) {
      char* unflattened = UNFLATTEN(root->path);
      if (stat(unflattened, &st) == 0) {
        root->id = watch(root->path, NULL);
        userlog(LOG_INFO, "root restored: %s\n", root->path);
        report_event("CREATE", unflattened);
        report_event("CHANGE", unflattened);
      }
    }
  }
}

static void check_root_removal(const char* path) {
  for (int i=0; i<roots->size(); i++) {
    watch_root* root = roots->at(i);
    if (root->id >= 0 && strcmp(path, UNFLATTEN(root->path)) == 0) {
      unwatch(root->id);
      root->id = -1;
      userlog(LOG_INFO, "root deleted: %s\n", root->path);
      report_event("DELETE", path);
    }
  }
}
