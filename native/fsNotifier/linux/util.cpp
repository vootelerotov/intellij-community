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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_BUF_LEN 2048
static char input_buf[INPUT_BUF_LEN];

char* read_line(FILE* stream) {
  char* retval = fgets(input_buf, INPUT_BUF_LEN, stream);
  if (retval == NULL || feof(stream)) {
    return NULL;
  }
  int pos = strlen(input_buf) - 1;
  if (input_buf[pos] == '\n') {
    input_buf[pos] = '\0';
  }
  return input_buf;
}


bool is_parent_path(const char* parent_path, const char* child_path) {
  size_t parent_len = strlen(parent_path);
  return strncmp(parent_path, child_path, parent_len) == 0 &&
         (parent_len == strlen(child_path) || child_path[parent_len] == '/');
}
