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

#pragma once

#define VERSION "20181113.1428"

#include <stdbool.h>
#include <stdio.h>

#include <vector>
#include <map>
#include <set>
using std::vector;
using std::map;
using std::set;

// messaging
typedef enum {
  MSG_INSTANCE_LIMIT, MSG_WATCH_LIMIT
} MSG;

void message(MSG id);


// logging
void userlog(int priority, const char* format, ...);

#define CHECK_NULL(p, r) if (p == NULL) { userlog(LOG_ERR, "out of memory"); return r; }

template <typename T, typename C>
void vector_delete_vs_data(vector<T*, C> *a) {
  if (a != NULL) {
    for (int i = 0; i < a->size(); i++) {
	  T* item = a->at(i);
	  if (item != NULL) {
		free(item);
	  }
	}
	a->clear();
	delete(a);
  }
}

template <typename T, typename C>
void set_delete_vs_data(set<T*, C> *s) {
  if (s != NULL) {
    for (typename set<T*, C>::iterator it = s->begin(); it != s->end(); it++) {
	  T* item = *it;
	  if (item != NULL) {
	    free(*it);
	  }
    }
    delete(s);
  }
}

template <typename T, typename C>
bool set_difference(set<T*, C>* s1, set<T*, C>* s2, set<T*, C>* diff) {
  if (s1 == NULL || s2 == NULL || diff == NULL) {
    return false;
  }
  typename set<T*, C>::iterator it;
  T* elem = NULL;
  for (it = s2->begin(); it != s2->end(); it++) {
	elem = *it;
    if (s1->find(elem) == s1->end()) {
	  diff->insert(elem);
    }
  }

  return true;
}

// inotify subsystem
enum {
  ERR_IGNORE = -1,
  ERR_CONTINUE = -2,
  ERR_ABORT = -3,
  ERR_MISSING = -4
};

bool init_inotify();
void set_inotify_callback(void (* callback)(const char*, int));
int get_inotify_fd();
int watch(const char* root, vector<char*>* mounts);
void unwatch(int id);
bool process_inotify_input();
void close_inotify();


// reads one line from stream, trims trailing carriage return if any
// returns pointer to the internal buffer (will be overwritten on next call)
char* read_line(FILE* stream);


// path comparison
bool is_parent_path(const char* parent_path, const char* child_path);
