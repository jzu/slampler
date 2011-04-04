/****************************************************************************
 * Slampler : Slug Sampler
 *
 * Stage-oriented sample player
 *
 * Copyright (C) Jean Zundel <jzu@free.fr> 2010 
 *
 * The Slampler is a sample player designed for the Linksys NSLU2 running 
 * GNU/Linux, but it works on any ALSA-based architecture.
 *
 *  gcc slampler.c -Wall -g -lasound -lpthread -o slampler
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

#define ALSA_PCM_NEW_HW_PARAMS_API

#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <alsa/asoundlib.h>
#include <linux/joystick.h>
#include <pthread.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <dirent.h>
#include <termios.h>

#define DATADIR "/data"
#define NBANKS 3
#define NSMPLS 5

#define DEBUG if (debug) printf
#define ERROR if (debug) fprintf

// Switches 6B745  547B6
// Depends on the soldering of switch wires on the joystick board

#define SW_SMPL0 9
#define SW_SMPL1 7
#define SW_SMPL2 4
#define SW_SMPL3 5
#define SW_SMPL4 8
#define SW_BANK  6
#define SW_SMPL5 0
#define SW_SMPL6 2
#define SW_SMPL7 3
#define SW_SMPL8 1

// LEDs

#define LED_DISK1  "/sys/class/leds/nslu2:green:disk-1"
#define LED_DISK2  "/sys/class/leds/nslu2:green:disk-2"
#define LED_READY  "/sys/class/leds/nslu2:green:ready"
#define LED_STATUS "/sys/class/leds/nslu2:red:status"

// WAV handling

struct RIFFfmtdata {
  char  data1 [22];
  short numchannels;                 // 1 or 2
  char  data2 [16];
  int   size;                        // of data to play back
};

struct wcb {                         // Wave Control Block
  char   path[256];                  //  filename
  int    fd;                         //  file descriptor
  int    start;                      //  switch activated
  struct RIFFfmtdata head;           //  WAV header
};

struct wcb wave [NBANKS][NSMPLS];

int smpl_flag [NSMPLS];              // Which sample to start now

int bank = 0;                        // Current bank

snd_pcm_uframes_t frames = 44;       // Don't ask

snd_pcm_t *handle_play;

short *playbuf,                      // Mixed audio
      *filebuf;                      // Read from file

int   debug = 0;

pthread_t jthread;                   // Joystick thread
pthread_t kthread;                   // Keyboard thread

struct termios raw_mode;             // ~(ICANON | IECHO)
struct termios cooked_mode;          // Backup of initial mode

void  *joystick ();                  // Thread routines
void  *keyboard ();

void  set_led (char *led, int i);
void  load_waves (int rep);
void  debugsig (int signum);


/****************************************************************************
 * Init, main read/process/write loop
 ****************************************************************************/ 

int main (int argc, char **argv) {

  int s,                             // Sample index
      b,                             // Bank index
      i;
  int len,                           // Read from file
      res,                           // Result for file operations
      rc;                            // Result for ALSA operations
  unsigned int rate = 44100;         // Sample rate

//  snd_pcm_hw_params_t *params_play;


  if ((argc > 1) && (!strcmp (argv [1], "-d")))
    debug = 1;

  /* LED init */

  set_led (LED_DISK1, 255);
  set_led (LED_DISK2, 0);
  set_led (LED_READY, 0);
  set_led (LED_STATUS,0);

  /* Read sample names and headers */

  for (b = 0; b < NBANKS; b++)
    load_waves (b);

  /* Thread */

  pthread_create (&jthread, NULL, joystick, NULL);
  pthread_create (&kthread, NULL, keyboard, NULL);

  signal (SIGINT, debugsig);

  // File buffer allocation

  filebuf = (short *) malloc (frames * 4);
  playbuf = (short *) malloc (frames * 4);

  // ALSA init

  /* PCM playback setup - stereo : some soundcards don't do mono */

  rc = snd_pcm_open (&handle_play, "plughw:0", SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    tcsetattr (0, TCSANOW, &cooked_mode);
    ERROR (stderr,
           "play - unable to open pcm device: %s\n", snd_strerror (rc));
    exit (EXIT_FAILURE);
  }
  if ((rc = snd_pcm_set_params (handle_play,
                                SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                2,                               // Stereo
                                rate,
                                1,                               // Resample
                                80000)) < 0) {                   // 0.08 sec 
    tcsetattr (0, TCSANOW, &cooked_mode);
    ERROR (stderr,
           "Playback open error: %s\n", snd_strerror (rc));
    exit (EXIT_FAILURE);
  }

  /* Processing loop */

  while (1) {

    /* Has a new sample been activated ? */

    for (s = 0; s < NSMPLS; s++)
      if (smpl_flag [s]) {
        smpl_flag [s] = 0;
        if (wave [bank][s].fd > 0) {
          close (wave [bank][s].fd);    // Only one instance at the same time
          wave [bank][s].fd = 0;        // Closed, will be restarted
        }
        if (wave [bank][s].head.size)
          wave [bank][s].fd = open (wave [bank][s].path, O_RDONLY);
        DEBUG ("start %d-%d (%s) = %d\n", 
               bank, s, wave[bank][s].path, wave[bank][s].fd);
      }

    /* Read and mix samples in playback buffer */

    memset (playbuf, 0, frames*4);                       // Stereo, 16-bit
                                              
    for (b = 0; b < NBANKS; b++)
      for (s = 0; s < NSMPLS; s++)
        if (wave [b][s].fd) {
          len = frames * 2 * wave [b][s].head.numchannels;
          res = read (wave [b][s].fd, 
                      filebuf, 
                      len);
          if (len == frames * 2)                         // Mono to stereo
            for (i = len/2-1; i >= 0; i--) {
              filebuf [i*2] = filebuf [i*2+1] = filebuf [i];
            }
          for (i = 0; i < frames*2; i += 2) {
            if ((playbuf [i] > 0) &&
                (filebuf [i] > 0) &&
                (playbuf [i] + filebuf [i]/2 < 0))
              playbuf [i] = playbuf [i+1] = SHRT_MAX;    // Prevents rollovers
            else 
            if ((playbuf [i] < 0) &&
                (filebuf [i] < 0) &&
                (playbuf [i] + filebuf [i]/2 > 0))
              playbuf [i] = playbuf [i+1] = SHRT_MIN;
            else {
              playbuf [i]   += filebuf [i]/2;            // Mix (-3 dB)
              playbuf [i+1] += filebuf [i+1]/2;
            }
          }
          if (res < len) {
            close (wave [b][s].fd);                      // Hoc finiunt samples
            wave [b][s].fd = 0;
            DEBUG ("stop  %d-%d\n", b, s);
          }
        }

    /* Write playback buffer content to device */

    rc = snd_pcm_writei (handle_play, 
                         playbuf, 
                         frames);
    if (rc == -EPIPE) {
      ERROR (stderr, 
             "writei - underrun occurred\n");
      snd_pcm_prepare (handle_play);
      snd_pcm_writei (handle_play, 
                      playbuf, 
                      frames);
    } else if (rc < 0) {
      ERROR (stderr,
             "error from write: %s\n", snd_strerror (rc));
    }  else if (rc != (int)frames) {
      ERROR (stderr,
             "short write, write %d frames\n", rc);
    }
  }

  return 0;
}


/**************************************************************************** 
 * write_to_file()
 *
 * Write a string to a file 
 * Fails silently (in case of insufficient rights)
 * *f  Filename
 * *s  String to write
 ****************************************************************************/

void write_to_file (const char *f, const char *s) {

  int fout;

  if ((fout = open (f, O_WRONLY)) > 0) {
    write (fout, s, strlen (s));
    close (fout);
  }
}

/****************************************************************************
 * set_led()
 *
 * Switch LEDs on/off 
 * Needs to be run as root to work, else nothing happens
 * *led  Dirname (/sys/classes/leds/foo/)
 * i     Value   (0||!0)
 ****************************************************************************/

void set_led (char *led, int i) {

  char led_bright [256];

  strncpy (led_bright, led, 255);
  strncat (led_bright, "/brightness", 255);
  write_to_file (led_bright, i ? "255" : "0");
}   


/****************************************************************************
 * load_waves()
 *
 * Loads .WAVs from a (numerically named) directory for a sample bank
 * All files at 44100 Hz, 2 bytes/sample, mono or stereo, normalized at -3dB
 *
 * rep  Number for directory name (should be 0,1,2...)
 ****************************************************************************/

void load_waves (int rep) {

  DIR *dirp;
  struct dirent *dp;
  int f,
      g;
  int wfile;
  char repname [256];

  char *name [5];                                 // Sort vars
  char *s;
  int  mod;

  memset (name, 0, sizeof (char *) * NSMPLS);
  s = malloc (256);

  sprintf (repname, "%s/%d", DATADIR, rep);
  if ((dirp = opendir (repname)) != NULL) {
    f = 0;
    while (((dp = readdir (dirp)) != NULL) && 
           (f < NSMPLS)) {
      if (dp->d_name[0] != '.') {
        name [f] = malloc (256);
        strcpy (name [f], dp->d_name);
        f++;
      }
    }
    for (f = 0; f < NSMPLS-1; f++) {             // Bubble sort
      for (g = 0, mod = 0; 
           (g < NSMPLS-1) && (name [g+1] != NULL); 
           g++) {
        if (strcmp (name [g], name [g+1]) > 0 ) {
          mod = 1;
          s = name [g];
          name [g] = name [g+1];
          name [g+1] = s;
        }
      }
      if (! mod)
        f = NSMPLS;                              // Completed, exit
    }
    for (f = 0; 
         (f < NSMPLS) && (name [f] != NULL); 
         f++) {
      sprintf (wave [rep][f].path, "%s/%d/%s", 
               DATADIR, 
               rep, 
               name [f]);
      wfile = open (wave [rep][f].path, O_RDONLY);
      read (wfile, 
            &wave [rep][f].head, 
            sizeof (struct RIFFfmtdata));
      DEBUG ("%10d  %s\n", wave [rep][f].head.size, wave [rep][f].path);
      close (wfile);
    }
    closedir (dirp);

    for (f = 0; (f < NSMPLS) && (name [f] != NULL); f++)
      free (name [f]);

    return;
  }
  else {
    ERROR (stderr,
           "Could not read %s\n", repname);
  }
}


/**************************************************************************** 
 * joystick()
 *
 * Separate thread
 * Access to four switches, switches NSLU2 LEDs on/off
 * Could use another joystick for more switches (open ("/dev/input/js1...)
 * Should cycle through an array mapping ev.number -> smpl_flag
 ****************************************************************************/

void *joystick ()
{
  int jfd;
  struct js_event ev,
                  oldev;

  memset (&oldev, 0, sizeof (struct js_event));
  memset (smpl_flag, 0, sizeof (int) * NSMPLS);

  jfd = open ("/dev/input/js0", O_RDONLY);

  while (1) {
    if (read (jfd, &ev, sizeof (ev)) > 0) {
      if ((ev.type == JS_EVENT_BUTTON) &&
               (ev.value == 1)) {
        if      (ev.number == SW_SMPL0)            // Yeuch
          smpl_flag[0] ^= 1;
        else if (ev.number == SW_SMPL1)
          smpl_flag[1] ^= 1;
        else if (ev.number == SW_SMPL2)
          smpl_flag[2] ^= 1;
        else if (ev.number == SW_SMPL3)
          smpl_flag[3] ^= 1;
        else if (ev.number == SW_SMPL4)
          smpl_flag[4] ^= 1;
        else if (ev.number == SW_SMPL5)
          smpl_flag[5] ^= 1;
        else if (ev.number == SW_SMPL6)
          smpl_flag[6] ^= 1;
        else if (ev.number == SW_SMPL7)
          smpl_flag[7] ^= 1;
        else if (ev.number == SW_SMPL8)
          smpl_flag[8] ^= 1;
        else if (ev.number == SW_BANK) {
          switch (++bank) {
            case 3:
              bank = 0;
            case 0:
              set_led (LED_DISK1, 255);
              set_led (LED_DISK2,   0);
              set_led (LED_READY,   0);
              break;
            case 1:
              set_led (LED_DISK1,   0);
              set_led (LED_DISK2, 255);
              set_led (LED_READY,   0);
              break;
            case 2:
              set_led (LED_DISK1,   0);
              set_led (LED_DISK2,   0);
              set_led (LED_READY, 255);
              break;
            default:
              bank = 0;
              break;
          }
          DEBUG ("bank %d\n", bank);
        }
        else {
          DEBUG ("ev.number=%d\n", ev.number);
        }
        if ((ev.value == 1) && 
            (oldev.value == 1) && 
            (((ev.number == SW_SMPL4) && (oldev.number == SW_BANK)) || 
             ((ev.number == SW_BANK)  && (oldev.number == SW_SMPL4)))) {
          tcsetattr (0, TCSANOW, &cooked_mode);
          set_led (LED_READY, 0);
          set_led (LED_DISK1, 0);
          set_led (LED_DISK2, 0);
          set_led (LED_STATUS,1);
          exit (0);               // Bye if both Bank and Sample 4 are pressed
        }
      }
    }
    oldev.value  = ev.value; 
    oldev.number = ev.number;
  }
}


/**************************************************************************** 
 * keyboard()
 *
 * Separate thread
 * No joystick at hand? Just use a keyboard.
 * Should use an array instead of a switch/case, see above.
 ****************************************************************************/

void *keyboard ()
{

  char c;

  /* We like it raw */

  tcgetattr (0, &cooked_mode);
  memcpy (&raw_mode, &cooked_mode, sizeof (struct termios));

  raw_mode.c_lflag &= ~(ICANON | ECHO);
  raw_mode.c_cc [VTIME] = 0;
  raw_mode.c_cc [VMIN] = 1;
  tcsetattr (0, TCSANOW, &raw_mode);

  c = '\0';
  memset (smpl_flag, 0, sizeof (int) * NSMPLS);

  while (1) {
    if (read (0, &c, 1) == 1) {
      if      (c == 'a')       // 'q' (and 'w' below) might suit you better
        smpl_flag[0] ^= 1;
      else if (c == 'z')
        smpl_flag[1] ^= 1;
      else if (c == 'e')
        smpl_flag[2] ^= 1;
      else if (c == 'r')
        smpl_flag[3] ^= 1;
      else if (c == 't')
        smpl_flag[4] ^= 1;
      else if (c == 'y')
        smpl_flag[5] ^= 1;
      else if (c == 'u')
        smpl_flag[6] ^= 1;
      else if (c == 'i')
        smpl_flag[7] ^= 1;
      else if (c == 'o')
        smpl_flag[8] ^= 1;
      else if (c == '\n') {
        switch (++bank) {
          case 3:
            bank = 0;
          case 0:
            set_led (LED_DISK1, 255);
            set_led (LED_DISK2,   0);
            set_led (LED_READY,   0);
            break;
          case 1:
            set_led (LED_DISK1,   0);
            set_led (LED_DISK2, 255);
            set_led (LED_READY,   0);
            break;
          case 2:
            set_led (LED_DISK1,   0);
            set_led (LED_DISK2,   0);
            set_led (LED_READY, 255);
            break;
          default:
            bank = 0;
            break;
        }
        DEBUG ("bank %d\n", bank);
      }
      else {
        DEBUG ("c=%c\n", c);
      }
    }
  }
}


/****************************************************************************
 * debugsig()
 *
 * Signal handler
 * signum  Signal number
 ****************************************************************************/

void debugsig (int signum) {

  tcsetattr (0, TCSANOW, &cooked_mode);

  set_led (LED_READY, 0);
  set_led (LED_DISK1, 0);
  set_led (LED_DISK2, 0);
  set_led (LED_STATUS,1);

  exit(0);
}

