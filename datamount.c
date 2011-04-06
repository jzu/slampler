/****************************************************************************
 * datamount : /data automounter
 *
 * Copyright (C) Jean Zundel <jzu@free.fr> 2011
 *
 * Custom automounter for the Slampler project
 * Allows to mount a memory stick on the fly
 * 
 *  gcc datamount.c -Wall -g -o datamount
 *
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 ****************************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mount.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>

#define DELAY  15
#define MAXLEN 4096
#define SOURCE "/dev/sdb1"
#define TARGET "/data"
#define VICTIM "slampler"


/****************************************************************************
 * grep()
 *
 * Looks for a string in a text file, returning 1 on success, 0 on failure
 *
 * s         string
 * filename  file to scan
 ****************************************************************************/

int grep (char *s, char *filename) {

  FILE *f;
  char line [MAXLEN];
  int  present;

  present = 0;
  f = fopen (filename, "r");
  while (fgets (line, MAXLEN, f) != NULL)
    if (strstr (line, s) != NULL) {
      fclose (f);
      return (1);
    }
  fclose (f);
  return (0);
}


/****************************************************************************
 * pgrep()
 *
 * Looks for a process whose name matches a substring, returning the first
 * instance's process number, 0 on failure
 *
 * p         string
 ****************************************************************************/

int pgrep (char *p) {

  DIR *dirp;
  struct dirent *dp;
  int f;
  char cmd [MAXLEN];
  char path [256];

  dirp = opendir ("/proc");

  while ((dp = readdir (dirp)) != NULL) 
    if ((dp->d_name [0] >= '1') &&
        (dp->d_name [0] <= '9')) {
      strcpy (path, "/proc/");
      strcat (path, dp->d_name);
      strcat (path, "/cmdline");

      f = open (path, O_RDONLY); 
      read (f, &cmd, MAXLEN);
      close (f);

      if (strstr (cmd, p)) {
        closedir (dirp);
        return (atoi (dp->d_name));
      }
    }
  closedir (dirp);
  return (0);
}


/****************************************************************************
 * main()
 ****************************************************************************/

int main (void) {

  int  sdb1,
       mnt,
       proc;

  while (1) {
    sdb1 = grep ("sdb1", "/proc/diskstats");
    mnt  = grep ("sdb1", "/proc/mounts");

    if (sdb1 && !mnt) {
      if ((proc = pgrep (VICTIM)) != 0)
        kill (proc, SIGTERM);   
      mount (SOURCE, TARGET, "vfat", MS_RDONLY, NULL);
    }
    if (!sdb1 && mnt) {
      if ((proc = pgrep (VICTIM)) != 0)
        kill (proc, SIGTERM);   
      umount (TARGET);
    }
    sleep (DELAY);
  }
}

